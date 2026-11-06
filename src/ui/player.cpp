// license:BSD-3-Clause

#include "player.h"
#include "bend_thinner.h"
#include "../rcp.h"
#include "../rcp3.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

// timeBeginPeriod() only exists on Windows, as the way to ask the scheduler for
// a 1 ms timer resolution. macOS already sleeps finely enough, so the call is
// simply not made there
#if defined(_WIN32)
#include <windows.h>
// winmm's header is what declares them, and a lean windows.h leaves it out.
// CMake defines WIN32_LEAN_AND_MEAN tree-wide; the Makefile does not, which is
// why only the CMake build missed it. midi_in.cpp includes it either way.
#include <mmsystem.h>
#endif

namespace ui {

namespace {

// 鳴りっぱなしを消す。口 A と口 B の 16 チャンネルぶん
void all_off(bridge &br)
{
	for (int ch = 0; ch < 16; ch++) {
		const u8 msg[3] = { u8(0xb0 | ch), 0x7b, 0x00 };   // オールノートオフ
		const u8 sus[3] = { u8(0xb0 | ch), 0x40, 0x00 };   // ダンパも離す
		br.send(msg, 3);
		br.send(sus, 3);
		br.send_b(msg, 3);
		br.send_b(sus, 3);
	}
}

std::string file_name(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// RCP2 と SMF を同じ秒ベースのプレイヤーイベント列に変換する。
// RCP2 は現時点で実装済みコマンドだけを受け付け、未対応コマンドはエラーにする。
bool load_events_from_memory(const u8 *data, size_t size,
                             std::vector<smf::event> &events, std::string &err,
                             smf::song_meta *meta)
{
    events.clear();
    err.clear();
    if (size >= 4 && std::memcmp(data, "MThd", 4) == 0)
        return smf::load_from_memory(data, size, events, err, meta);

// RCP3.0/G36 is not SMF. Parse it before falling back to the SMF reader.
    std::string rcp3_error;
    if (rcp3::inspect_header(data, size, rcp3_error)) {
        rcp3::header_info h;
        if (!rcp3::parse_header(data, size, h, err))
            return false;
        if (h.ticks_per_quarter == 0 || h.tempo_bpm == 0) {
            err = "RCP3.0 の TPQ または初期テンポが 0 です";
            return false;
        }
        std::vector<rcp3::track> tracks;
        if (!rcp3::parse_tracks(data, size, h, tracks, err))
            return false;

        struct g36_tempo_change { double tick; double bpm; };
        std::vector<g36_tempo_change> tempo_changes;
        tempo_changes.push_back({0.0, double(h.tempo_bpm)});
        if (meta) {
            *meta = smf::song_meta{};
            meta->title = smf::title_text(h.title);
            meta->ppq = h.ticks_per_quarter;
        }
        for (const rcp3::track &t : tracks) {
            if (t.header.mute || t.header.midi_channel == 0xff)
                continue;
            int channel_field = t.header.midi_channel;
            int channel = channel_field & 0x0f;
            int port = (channel_field >> 4) & 0x0f;
            const bool rhythm_track = (t.header.key & 0x80) != 0;
            const int transpose = rhythm_track ? 0 :
                static_cast<int>(static_cast<int8_t>(t.header.key)) +
                static_cast<int>(static_cast<int8_t>(h.play_bias));
            const double track_start = double(static_cast<int8_t>(t.header.tick_offset));
            double tick = track_start;
            bool channel_muted = false;
            struct loop_frame {
                size_t begin = 0;
                unsigned int completed = 0;
                unsigned int limit = 0;
            };
            std::vector<loop_frame> loop_stack;
            // FC/FD measure-repeat state. G36 command IDs are record numbers:
            // 0x30 is the first sequence record, then one ID per 6-byte record.
            size_t measure_count = 1; // measure 0 starts at the first sequence record
            size_t repeat_return = t.sequence.size();
            bool following_fc_chain = false;
            // A malformed file must not be able to create unbounded expansion.
            size_t steps_left = t.sequence.size() * 128 + 1;
            size_t event_index = 0;
            bool track_end_seen = false;
            struct active_note {
                int note;
                size_t off_event;
                double end_tick;
            };
            std::vector<active_note> active_notes;
            for (; event_index < t.sequence.size() && steps_left > 0; --steps_left) {
                const rcp3::sequence_event &e = t.sequence[event_index];
                // G36 p0 is the delay from this event to the next one.
                // Commands F0..FF explicitly ignore p0.
                const double event_time = std::max(0.0, tick);
                auto emit = [&](std::initializer_list<u8> bytes) {
                    if (!channel_muted)
                        events.push_back({event_time, std::vector<u8>(bytes), u8(port)});
                };
                if (e.code < 0x80) {
                    const int note = int(e.code) + transpose;
                    const double off_tick = std::max(std::max(0.0, tick), tick + e.p1);
                    if (note >= 0 && note <= 127 && e.p1 != 0 && e.p2 != 0 && e.p2 <= 127 && !channel_muted) {
                        // G36 reuses an already-running pitch instead of emitting
                        // another Note On. Its duration is replaced by this record's
                        // duration; a zero duration therefore ends the old note here.
                        auto active = std::find_if(active_notes.begin(), active_notes.end(),
                            [note, tick](const active_note &n) {
                                return n.note == note && n.end_tick > tick;
                            });
                        if (active != active_notes.end()) {
                            events[active->off_event].time = off_tick;
                            active->end_tick = off_tick;
                            if (e.p1 == 0)
                                active_notes.erase(active);
                        } else if (e.p1 != 0) {
                            events.push_back({event_time,
                                {u8(0x90 | channel), u8(note), u8(e.p2)}, u8(port)});
                            const size_t off_event = events.size();
                            events.push_back({off_tick,
                                {u8(0x80 | channel), u8(note), 0}, u8(port)});
                            active_notes.push_back({note, off_event, off_tick});
                        }
                    }
                } else {
                    switch (e.code) {
                    case 0x90: case 0x91: case 0x92: case 0x93:
                    case 0x94: case 0x95: case 0x96: case 0x97: {
                        // User SysEx templates are stored in eight fixed header slots.
                        const rcp3::user_sysex &definition = h.user_sysexes[e.code & 0x07];
                        if (!channel_muted && !definition.data.empty()) {
                            std::vector<u8> payload;
                            payload.reserve(definition.data.size() + 1);
                            unsigned int checksum = 0;
                            for (u8 token : definition.data) {
                                if (token == 0xf7) {
                                    payload.push_back(0xf7);
                                    break;
                                }
                                u8 value = token;
                                switch (token) {
                                case 0x80: value = e.p1; break;
                                case 0x81: value = e.p2; break;
                                case 0x82: value = u8(channel); break;
                                case 0x83: checksum = 0; continue;
                                case 0x84: value = u8((0x100 - checksum) & 0x7f); break;
                                default:
                                    if (token & 0x80) {
                                        // Unknown template controls are ignored, matching the reference converter.
                                        continue;
                                    }
                                    break;
                                }
                                payload.push_back(value);
                                checksum += value;
                            }
                            if (!payload.empty() && payload.back() != 0xf7)
                                payload.push_back(0xf7);
                            if (payload.size() > 1) {
                                payload.insert(payload.begin(), 0xf0);
                                events.push_back({event_time, std::move(payload), u8(port)});
                            }
                        }
                        break;
                    }
                    case 0xe2: // Bank MSB (p2), Program Change (p1)
                        emit({u8(0xb0 | channel), 0x00, u8(e.p2 & 0x7f)});
                        emit({u8(0xc0 | channel), u8(e.p1 & 0x7f)});
                        break;
                    case 0xe6: // MIDI channel: 0=mute, 1..16=port A, 17..32=port B
                        if (e.p1 == 0) {
                            channel_muted = true;
                        } else if (e.p1 <= 32) {
                            const int mapped = int(e.p1) - 1;
                            port = mapped / 16;
                            channel = mapped % 16;
                            channel_muted = false;
                        }
                        break;
                    case 0xe7: { // Tempo Modifier
                        if (e.p1 == 0) {
                            err = "RCP3.0 E7 のテンポ倍率 p1 が 0 です";
                            return false;
                        }
                        // Reference formula: 60,000,000 * 64 / (base BPM * p1).
                        // p2 != 0 is a gradual change; interpolation remains unsupported.
                        const double bpm = double(h.tempo_bpm) * 64.0 / double(e.p1);
                        tempo_changes.push_back({std::max(0.0, tick), bpm});
                        break;
                    }
                    case 0xea: emit({u8(0xd0 | channel), u8(e.p1 & 0x7f)}); break;
                    case 0xeb:
                        emit({u8(0xb0 | channel), u8(e.p1 & 0x7f), u8(e.p2 & 0x7f)});
                        break;
                    case 0xec: emit({u8(0xc0 | channel), u8(e.p1 & 0x7f)}); break;
                    case 0xed:
                        emit({u8(0xa0 | channel), u8(e.p1 & 0x7f), u8(e.p2 & 0x7f)});
                        break;
                    case 0xee: {
                        const int bend = (int(e.p2 & 0x7f) << 7) | int(e.p1 & 0x7f);
                        emit({u8(0xe0 | channel), u8(bend & 0x7f), u8((bend >> 7) & 0x7f)});
                        break;
                    }
                    case 0xfc: { // Repeat a previous measure (G36 record ID in p1)
                        if (following_fc_chain && repeat_return < t.sequence.size()) {
                            // A chain FC -> FC -> ... is one redirection; retain
                            // the original continuation until a non-FC/FD record.
                        } else if (repeat_return < t.sequence.size()) {
                            // FC encountered while replaying a measure returns to
                            // the command following the original FC.
                            event_index = repeat_return;
                            repeat_return = t.sequence.size();
                            following_fc_chain = false;
                            continue;
                        }
                        if (e.p0 >= measure_count || e.p1 < 0x0030) {
                            err = "RCP3.0 FC references an unavailable measure or invalid command ID";
                            return false;
                        }
                        {
                            const size_t target = size_t(e.p1 - 0x0030);
                            if (target >= t.sequence.size() ||
                                t.sequence[target].code == 0xfe ||
                                target == event_index) {
                                err = "RCP3.0 FC target is outside the sequence or points to Track End";
                                return false;
                            }
                            if (repeat_return == t.sequence.size())
                                repeat_return = event_index + 1;
                            event_index = target;
                            following_fc_chain = (t.sequence[target].code == 0xfc);
                            continue;
                        }
                    }
                    case 0xfd: // Measure End
                        if (repeat_return < t.sequence.size()) {
                            event_index = repeat_return;
                            repeat_return = t.sequence.size();
                            following_fc_chain = false;
                            ++measure_count;
                            continue;
                        }
                        ++measure_count;
                        break;
                    case 0xf8: // Loop End: p0 is the total number of passes
                        if (!loop_stack.empty()) {
                            loop_frame &frame = loop_stack.back();
                            ++frame.completed;
                            if (frame.limit != 0 && frame.completed < frame.limit) {
                                event_index = frame.begin;
                                continue;
                            }
                            loop_stack.pop_back();
                        }
                        break;
                    case 0xf9: // Loop Start
                        if (loop_stack.size() < 8) {
                            loop_stack.push_back({event_index + 1, 0,
                                (e.p0 > 0 && e.p0 < 0x7f) ? unsigned(e.p0) : 0u});
                        }
                        break;
                    case 0xfe: break;
                    default:
                        // SysEx and vendor-specific commands require dedicated
                        // stateful handling; never guess their meaning.
                        break;
                    }
                    if (e.code == 0xfe) {
                        track_end_seen = true;
                        break;
                    }
                }
                if (e.code < 0xf0)
                    tick += e.p0;
                ++event_index;
            }
            if (!track_end_seen) {
                events.clear();
                err = "RCP3.0 loop expansion exceeded its per-track step budget";
                return false;
            }
        }
        if (events.empty()) {
            err = "RCP3.0 に再生可能な音符イベントがありません";
            return false;
        }
        std::stable_sort(tempo_changes.begin(), tempo_changes.end(),
            [](const g36_tempo_change &a, const g36_tempo_change &b) { return a.tick < b.tick; });
        struct g36_tempo_point { double tick; double seconds; double bpm; };
        std::vector<g36_tempo_point> tempo_map;
        tempo_map.push_back({0.0, 0.0, double(h.tempo_bpm)});
        double last_tick = 0.0, elapsed = 0.0, current_bpm = double(h.tempo_bpm);
        for (const g36_tempo_change &change : tempo_changes) {
            if (change.tick <= 0.0) {
                current_bpm = change.bpm;
                tempo_map.back() = {0.0, 0.0, current_bpm};
                continue;
            }
            elapsed += (change.tick - last_tick) * 60.0 /
                (current_bpm * double(h.ticks_per_quarter));
            last_tick = change.tick;
            current_bpm = change.bpm;
            if (tempo_map.back().tick == change.tick)
                tempo_map.back() = {change.tick, elapsed, current_bpm};
            else
                tempo_map.push_back({change.tick, elapsed, current_bpm});
        }
        auto tick_to_seconds = [&](double target_tick) {
            if (target_tick <= 0.0) return 0.0;
            auto it = std::upper_bound(tempo_map.begin(), tempo_map.end(), target_tick,
                [](double value, const g36_tempo_point &point) { return value < point.tick; });
            if (it == tempo_map.begin())
                return target_tick * 60.0 / (double(h.tempo_bpm) * double(h.ticks_per_quarter));
            --it;
            return it->seconds + (target_tick - it->tick) * 60.0 /
                (it->bpm * double(h.ticks_per_quarter));
        };
        for (smf::event &event : events) event.time = tick_to_seconds(event.time);
        if (meta) {
            for (const g36_tempo_point &point : tempo_map) {
                // G36 tick positions are integral (record delays are integers);
                // song_meta stores the tick coordinate as u64.
                meta->tempo.push_back({point.seconds, static_cast<u64>(point.tick), point.bpm});
            }
        }
        std::stable_sort(events.begin(), events.end(), [](const smf::event &a, const smf::event &b) {
            return a.time < b.time;
        });
        return true;
    }

    rcp::header_info info;
    std::string inspect_error;
    if (!rcp::inspect_header(data, size, info, inspect_error) ||
        info.type != rcp::format::rcp2)
        return smf::load_from_memory(data, size, events, err, meta);

    rcp::intermediate_document document;
    if (!rcp::build_intermediate(data, size, document, err))
        return false;
    std::vector<rcp::timed_track_event> timeline;
    if (!rcp::expand_intermediate_timeline(document, timeline, err))
        return false;
    std::vector<rcp::timed_seconds_event> seconds;
    if (!rcp::convert_timeline_to_seconds(document.header, timeline, seconds, err))
        return false;
    std::vector<rcp::track_midi_mapping> mapping;
    if (!rcp::map_track_midi_outputs(document, mapping, err))
        return false;
    std::vector<rcp::midi_output_event> encoded;
    if (!rcp::encode_timeline_to_midi_events(seconds, mapping, encoded, err))
        return false;

    if (meta) {
        *meta = smf::song_meta{};
        meta->title = smf::title_text(document.header.title);
        meta->ppq = document.header.ticks_per_quarter;
        if (document.header.tempo_bpm == 0) {
            err = "RCP2 初期テンポが 0 です";
            return false;
        }
        meta->tempo.push_back({0.0, 0, double(document.header.tempo_bpm)});
        for (const rcp::timed_seconds_event &event : seconds) {
            if (event.source.event.kind != rcp::timeline_event_kind::tempo)
                continue;
            const u32 us = event.source.event.tempo.microseconds_per_quarter;
            if (!us)
                continue;
            const double bpm = 60000000.0 / double(us);
            if (!meta->tempo.empty() && meta->tempo.back().tick == event.source.event.tick)
                meta->tempo.back() = {event.time, event.source.event.tick, bpm};
            else
                meta->tempo.push_back({event.time, event.source.event.tick, bpm});
        }
    }
    events.reserve(encoded.size());
    for (const rcp::midi_output_event &event : encoded)
        events.push_back({event.time, event.bytes, event.port});
    return true;
}

bool load_file_events(const std::string &path, std::vector<smf::event> &events,
                      std::string &err, smf::song_meta *meta)
{
    std::FILE *file = nullptr;
#if defined(_WIN32)
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (n > 0) {
        std::wstring wide(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), n);
        file = _wfopen(wide.c_str(), L"rb");
    }
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) {
        err = "ファイルを開けません: " + path;
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        err = "ファイルサイズを取得できません: " + path;
        return false;
    }
    const long length = std::ftell(file);
    if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        err = "ファイルサイズを取得できません: " + path;
        return false;
    }
    std::vector<u8> data(static_cast<size_t>(length), u8(0));
    if (std::fread(data.data(), 1, data.size(), file) != data.size()) {
        std::fclose(file);
        err = "ファイルを読み込めません: " + path;
        return false;
    }
    std::fclose(file);
    return load_events_from_memory(data.data(), data.size(), events, err, meta);
}

// 曲の終わりから次の曲までの間（音の尾を少し待つ）
constexpr double GAP = 1.0;

} // namespace


// ---- 一覧

std::vector<player::entry> player::list() const
{
	std::lock_guard<std::mutex> lock(m_lock);
	return m_list;
}

std::vector<std::string> player::paths() const
{
	std::vector<std::string> result;
	for (const auto &e : list()) {
		if (!e.path.empty())
			result.push_back(e.path);
	}
	return result;
}

std::string player::name() const
{
	std::lock_guard<std::mutex> lock(m_lock);
	const int c = m_cur.load(std::memory_order_relaxed);
	if (c < 0 || c >= int(m_list.size()))
		return std::string();
	return m_list[size_t(c)].title.empty() ? m_list[size_t(c)].name : m_list[size_t(c)].title;
}

// 一覧に足す曲を作るのは 1 つだけ。長さと口の数もここで決まり、鳴らす側（run）が
// 状態行に出すのと同じ値が入る
static player::entry make_entry(const std::string &path, const std::string &name,
                                const std::string &title,
                                const std::vector<smf::event> &evs)
{
	player::entry e;
	e.path = path;
	e.name = name;
	e.title = title;
	e.length = evs.back().time;
	for (const smf::event &x : evs)
		e.ports = std::max(e.ports, int(x.port) + 1);
	return e;
}

bool player::add(const std::string &path, std::string &err, int *index)
{
	{
		std::lock_guard<std::mutex> lock(m_lock);
		for (size_t i = 0; i < m_list.size(); i++)
			if (m_list[i].path == path) {
				if (index)
					*index = int(i);
				return true;
			}
	}
	std::vector<smf::event> evs;
	smf::song_meta meta;
	if (!load_file_events(path, evs, err, &meta))
		return false;
	if (evs.empty()) {
		err = "中身が空";
		return false;
	}
	entry e = make_entry(path, file_name(path), meta.title, evs);
	std::lock_guard<std::mutex> lock(m_lock);
	m_list.push_back(std::move(e));
	m_order.clear();
	if (index)
		*index = int(m_list.size()) - 1;
	return true;
}

bool player::add_from_memory(const u8 *data, size_t size, const std::string &name,
                             std::string &err, int *index)
{
	std::vector<smf::event> evs;
	smf::song_meta meta;
	if (!load_events_from_memory(data, size, evs, err, &meta))
		return false;
	if (evs.empty()) {
		err = "中身が空";
		return false;
	}
	entry e = make_entry(std::string(), name, meta.title, evs);
	e.evs = std::move(evs);   // path が無いぶん、ここが中身の唯一の持ち物
	std::lock_guard<std::mutex> lock(m_lock);
	m_list.push_back(std::move(e));
	m_order.clear();
	if (index)
		*index = int(m_list.size()) - 1;
	return true;
}

void player::remove(int index)
{
	bool was_current = false;
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (index < 0 || index >= int(m_list.size()))
			return;
		const int c = m_cur.load(std::memory_order_relaxed);
		was_current = c == index;
		if (!was_current) {
			m_list.erase(m_list.begin() + index);
			m_order.clear();
			if (c > index)
				m_cur.store(c - 1, std::memory_order_relaxed);
			return;
		}
	}
	// 鳴らしている曲を消す: 先に止める（止めるのは鍵の外で）
	stop();
	std::lock_guard<std::mutex> lock(m_lock);
	if (index < int(m_list.size()))
		m_list.erase(m_list.begin() + index);
	m_order.clear();
	m_cur.store(m_list.empty() ? -1 : std::min(index, int(m_list.size()) - 1), std::memory_order_relaxed);
}

void player::move(int from, int to)
{
	std::lock_guard<std::mutex> lock(m_lock);
	const int n = int(m_list.size());
	if (from < 0 || from >= n || to < 0 || to >= n || from == to)
		return;
	entry e = std::move(m_list[size_t(from)]);
	m_list.erase(m_list.begin() + from);
	m_list.insert(m_list.begin() + to, std::move(e));
	m_order.clear();
	// 鳴らしている曲の番号を付け替える
	int c = m_cur.load(std::memory_order_relaxed);
	if (c == from)
		c = to;
	else if (from < c && c <= to)
		c--;
	else if (to <= c && c < from)
		c++;
	m_cur.store(c, std::memory_order_relaxed);
}

void player::clear()
{
	stop();
	std::lock_guard<std::mutex> lock(m_lock);
	m_list.clear();
	m_order.clear();
	m_cur.store(-1, std::memory_order_relaxed);
}

void player::reshuffle(int first)
{
	m_order.resize(m_list.size());
	for (size_t i = 0; i < m_order.size(); i++)
		m_order[i] = int(i);
	static std::mt19937 rng{ std::random_device{}() };
	std::shuffle(m_order.begin(), m_order.end(), rng);
	// いまの曲から始める（同じ曲が続けて鳴らないように）
	const auto it = std::find(m_order.begin(), m_order.end(), first);
	if (it != m_order.end())
		std::iter_swap(m_order.begin(), it);
}

int player::after(int index, int step)
{
	const int n = int(m_list.size());
	if (n == 0)
		return -1;
	const loop_mode mode = loop();
	if (mode == loop_mode::shuffle) {
		if (m_order.size() != m_list.size())
			reshuffle(index);
		const auto it = std::find(m_order.begin(), m_order.end(), index);
		int k = it == m_order.end() ? 0 : int(it - m_order.begin()) + step;
		if (k >= n) {                      // 一回りした: 混ぜ直す
			reshuffle(-1);
			k = 0;
		}
		if (k < 0)
			k = n - 1;
		return m_order[size_t(k)];
	}
	int k = index + step;
	if (k >= n)
		return mode == loop_mode::none ? -1 : 0;
	if (k < 0)
		return mode == loop_mode::none ? 0 : n - 1;
	return k;
}


// ---- 操作

bool player::start(const std::string &path, bridge &br, std::string &err)
{
	int index = -1;
	if (!add(path, err, &index))
		return false;
	play(index, br);
	return true;
}

bool player::start_from_memory(const u8 *data, size_t size, const std::string &name,
                               bridge &br, std::string &err)
{
	int index = -1;
	if (!add_from_memory(data, size, name, err, &index))
		return false;
	play(index, br);
	return true;
}

void player::launch(bridge &br)
{
	m_quit.store(false);
	m_playing.store(true, std::memory_order_release);
	m_thread = std::thread([this, &br] { run(br); });
}

void player::play(int index, bridge &br)
{
	{
		std::lock_guard<std::mutex> lock(m_lock);
		if (index < 0 || index >= int(m_list.size()))
			return;
		// 長さと口の数は、流す糸が曲を開く前でも聞かれる（「再生: …（N 秒）」の表示）。一覧に控えたものを先に入れておく
		m_len.store(m_list[size_t(index)].length, std::memory_order_relaxed);
		m_ports_used.store(m_list[size_t(index)].ports, std::memory_order_relaxed);
	}
	m_paused.store(false, std::memory_order_relaxed);
	if (m_playing.load(std::memory_order_acquire) && m_thread.joinable()) {
		m_jump.store(index, std::memory_order_release);     // 動いている糸に曲を替えさせる
		return;
	}
	if (m_thread.joinable())
		m_thread.join();
	m_cur.store(index, std::memory_order_relaxed);
	m_jump.store(-1);
	m_step.store(0);
	m_seek.store(-1.0);
	launch(br);
}

void player::stop()
{
	m_quit.store(true, std::memory_order_release);
	if (m_thread.joinable())
		m_thread.join();
	m_playing.store(false, std::memory_order_release);
	m_paused.store(false, std::memory_order_relaxed);
	m_state.store(int(state::stopped), std::memory_order_relaxed);
	m_pos.store(0);
}

void player::next() { if (playing()) m_step.store(1, std::memory_order_release); }

void player::prev()
{
	if (!playing())
		return;
	if (position() > 2.0)
		m_seek.store(0.0, std::memory_order_release);
	else
		m_step.store(-1, std::memory_order_release);
}

void player::pause(bool on) { if (playing()) m_paused.store(on, std::memory_order_release); }

void player::seek(double sec)
{
	if (playing())
		m_seek.store(std::max(0.0, sec), std::memory_order_release);
}

bool player::beat(int &bar, int &beat_, double &bpm) const
{
	if (!playing())
		return false;
	std::lock_guard<std::mutex> lock(m_lock);
	m_meta.bar_beat(position(), bar, beat_, bpm);
	return true;
}


// ---- 流す糸。曲を次々に開いて流す。止める・曲を替える・飛ぶは、旗を見て自分でやる

void player::run(bridge &br)
{
	// 1 ミリ秒で起きられるようにしておく。既定の 15.6 ミリ秒だと
	// 音符の頭がばらつく
#if defined(_WIN32)
	timeBeginPeriod(1);
#endif
	using clock = std::chrono::steady_clock;
	auto send = [&br](int to, const u8 *d, size_t n) {
		if (to == 0)      br.send(d, n);
		else if (to > 0)  br.send_port(to, d, n);
	};
	auto quit = [this] { return m_quit.load(std::memory_order_acquire); };
	// 旗を見ながら待つ（止める・曲を替えるが来たら早く戻る）
	auto wait = [&](double sec) {
		const auto until = clock::now() + std::chrono::duration<double>(sec);
		while (clock::now() < until && !quit() && m_jump.load() < 0 && m_step.load() == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
	};

	while (!quit()) {
		// ---- 曲を開く
		std::string path;
		std::vector<smf::event> events;
		smf::song_meta meta;
		{
			std::lock_guard<std::mutex> lock(m_lock);
			const int c = m_cur.load(std::memory_order_relaxed);
			if (c < 0 || c >= int(m_list.size()))
				break;
			const entry &e = m_list[size_t(c)];
			path = e.path;
			events = e.evs;   // ファイルでない曲（iOS の選択、wasm）は最初から入っている
		}
		std::string err;
		const bool ok = (events.empty() ? load_file_events(path, events, err, &meta)
		                                : true) && !events.empty();
		int ports = 1;
		for (const smf::event &e : events)
			ports = std::max(ports, int(e.port) + 1);
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_meta = meta;
		}
		m_ports_used.store(ports, std::memory_order_relaxed);
		m_len.store(ok ? events.back().time : 0.0, std::memory_order_relaxed);
		m_pos.store(0.0, std::memory_order_relaxed);
		m_seek.store(-1.0);
		m_state.store(int(state::playing), std::memory_order_relaxed);

		// ---- 流す
		bend_thinner thinner;   // set_thin_bends のとき
		size_t at = 0;
		double pos = 0.0;
		auto last = clock::now();
		bool was_paused = false;
		int step = 0, jump = -1;
		while (ok && !quit()) {
			jump = m_jump.exchange(-1);
			step = m_step.exchange(0);
			if (jump >= 0 || step)
				break;

			// 頭出し: 鳴っている音を止め、行き先までの設定を追いかけてから続きを流す
			const double to_sec = m_seek.exchange(-1.0);
			if (to_sec >= 0.0) {
				all_off(br);
				const double target = std::min(to_sec, events.back().time);
				const bool fold = m_fold.load(std::memory_order_relaxed), usb = m_usb.load(std::memory_order_relaxed);
				size_t bytes = 0;
				for (const smf::event &e : smf::chase(events, target)) {
					send(smf::mu_port(e.port, fold, usb, br.board_port()), e.bytes.data(), e.bytes.size());
					bytes += e.bytes.size();
				}
				at = size_t(std::lower_bound(events.begin(), events.end(), target,
				                             [](const smf::event &e, double t) { return e.time < t; }) - events.begin());
				pos = target;
				m_pos.store(pos, std::memory_order_relaxed);
				// 音源が読み終えるまで待つ（待たずに続きを流すと、続きの音符が設定の後ろに並んで遅れる）
				m_state.store(int(state::chasing), std::memory_order_relaxed);
				wait(double(bytes) / (usb ? 10000.0 : 3125.0) + 0.05);
				m_state.store(int(m_paused.load() ? state::paused : state::playing), std::memory_order_relaxed);
				thinner = bend_thinner();
				last = clock::now();
				continue;
			}

			// 一時停止: 時計を止める。入るときに鳴っている音を止める
			if (m_paused.load(std::memory_order_acquire)) {
				if (!was_paused) {
					all_off(br);
					was_paused = true;
					m_state.store(int(state::paused), std::memory_order_relaxed);
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				last = clock::now();
				continue;
			}
			if (was_paused) {
				was_paused = false;
				m_state.store(int(state::playing), std::memory_order_relaxed);
			}

			const auto now = clock::now();
			pos += std::chrono::duration<double>(now - last).count();
			last = now;
			m_pos.store(std::min(pos, events.back().time), std::memory_order_relaxed);

			// 来ている分をまとめて送る。トラックの出し先（SMF のポート指定かトラック名）が 0-3 なら口 A-D。
			// USB の口（gui の既定）なら 4 口ともそのまま。DIN の口だけ（--host-midi）なら、口 3・4 は
			// 選んだ扱いに従う（A・B に重ねるか、鳴らさない）
			const bool fold = m_fold.load(std::memory_order_relaxed);
			const bool usb = m_usb.load(std::memory_order_relaxed);
			const bool thin = m_thin.load(std::memory_order_relaxed);
			while (at < events.size() && events[at].time <= pos) {
				const smf::event &e = events[at];
				const int to = smf::mu_port(e.port, fold, usb, br.board_port());
				at++;
				if (thin)
					thinner.event(to, e.bytes.data(), e.bytes.size(), e.time, send);
				else
					send(to, e.bytes.data(), e.bytes.size());
			}
			// 持っているベンドは GAP たったら送る（途中で切りにしたとき・曲の終わりはすぐ）
			const bool done = at >= events.size();
			thinner.tick(pos, !thin || done, send);
			if (done)
				break;

			// 次まで待つ。長く待ちすぎないように刻む
			const double until = events[at].time - pos;
			std::this_thread::sleep_for(std::chrono::milliseconds(until > 0.010 ? 5 : 1));
		}
		all_off(br);
		if (quit())
			break;

		// ---- 次の曲を決める
		int next_index = -1;
		{
			std::lock_guard<std::mutex> lock(m_lock);
			const int c = m_cur.load(std::memory_order_relaxed);
			if (jump >= 0)
				next_index = jump < int(m_list.size()) ? jump : -1;
			else if (step)
				next_index = after(c, step);
			else if (loop() == loop_mode::one)
				next_index = c;
			else
				next_index = after(c, 1);
		}
		if (next_index < 0)
			break;
		// 曲が自分で終わったときは、音の尾を少し待ってから次へ（操作で替えたときはすぐ）
		if (jump < 0 && !step) {
			wait(GAP);
			if (quit())
				break;
			const int j = m_jump.exchange(-1);
			if (j >= 0)
				next_index = j;
		}
		m_cur.store(next_index, std::memory_order_relaxed);
	}

#if defined(_WIN32)
	timeEndPeriod(1);
#endif
	m_state.store(int(state::stopped), std::memory_order_relaxed);
	m_pos.store(0);
	m_playing.store(false, std::memory_order_release);
}

} // namespace ui
