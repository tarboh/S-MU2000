// license:BSD-3-Clause
//
// RCP2 の固定ヘッダーと未解釈シーケンスレコードを安全に読み取る。
// コマンド意味論やイベント時間の展開は別段階で実装する。

#include "rcp.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace rcp {
namespace {

// RCP2 (RCP-PC98 V2.0) の識別文字列。ヘッダー先頭の 32 バイト中、
// 先頭 30 バイトが文字列＋CRLF、残り 2 バイトは NUL。
// レイアウトは ValleyBell/MidiConverters の RCPFormat.txt を参考にした。
constexpr char RCP2_SIGNATURE[] = "RCM-PC98V2.0(C)COME ON MUSIC\r\n";
constexpr size_t RCP2_SIGNATURE_SIZE = sizeof(RCP2_SIGNATURE) - 1;
constexpr size_t RCP2_PREFIX_SIZE = 0x20;
constexpr size_t RCP2_HEADER_SIZE = 0x586;
constexpr size_t RCP2_TRACK_HEADER_SIZE = 0x2c;
constexpr size_t RCP2_USER_SYSEX_OFFSET = 0x406;
constexpr size_t RCP2_USER_SYSEX_SLOT_SIZE = 0x30;
constexpr size_t RCP2_USER_SYSEX_NAME_SIZE = 0x18;
constexpr size_t RCP2_USER_SYSEX_DATA_SIZE = 0x18;

std::string fixed_string(const u8 *data, size_t size)
{
	while (size > 0 && (data[size - 1] == 0 || data[size - 1] == ' '))
		--size;
	return std::string(reinterpret_cast<const char *>(data), size);
}

bool signature_matches(const u8 *data, size_t size)
{
	return data != nullptr && size >= RCP2_PREFIX_SIZE &&
	       std::memcmp(data, RCP2_SIGNATURE, RCP2_SIGNATURE_SIZE) == 0 &&
	       data[30] == 0 && data[31] == 0;
}

} // namespace

bool inspect_header(const u8 *data, size_t size, header_info &out, std::string &err)
{
	out = {};
	err.clear();

	if (data == nullptr) {
		err = "RCP データが null です";
		return false;
	}
	if (size < RCP2_PREFIX_SIZE) {
		err = "RCP ヘッダーが短すぎます";
		return false;
	}
	if (!signature_matches(data, size)) {
		err = "RCP2 の識別文字列が一致しません";
		return false;
	}

	out.type = format::rcp2;
	out.minimum_header_size = RCP2_PREFIX_SIZE;
	return true;
}

bool parse_header(const u8 *data, size_t size, document_header &out, std::string &err)
{
	out = {};
	err.clear();

	header_info info;
	if (!inspect_header(data, size, info, err))
		return false;
	if (size < RCP2_HEADER_SIZE) {
		err = "RCP2 固定ヘッダー全体がありません";
		return false;
	}

	// RCPFormat.txt の Header v2 [RCP] オフセット表に基づく。
	out.title = fixed_string(data + 0x020, 0x040);
	out.comment = fixed_string(data + 0x060, 0x150);
	out.ticks_per_quarter = static_cast<u16>(data[0x1c0]) |
	                        (static_cast<u16>(data[0x1e7]) << 8);
	out.tempo_bpm = data[0x1c1];
	out.beat_numerator = data[0x1c2];
	out.beat_denominator = data[0x1c3];
	out.key_signature = data[0x1c4];
	out.play_bias = data[0x1c5];
	// Legacy RCP2 files store zero here and still contain the standard 18 tracks.
	out.track_count = data[0x1e6] == 0 ? 18 : data[0x1e6];
	for (size_t i = 0; i < out.user_sysex.size(); ++i) {
		const size_t slot = RCP2_USER_SYSEX_OFFSET + i * RCP2_USER_SYSEX_SLOT_SIZE;
		out.user_sysex[i].name = fixed_string(data + slot, RCP2_USER_SYSEX_NAME_SIZE);
		const u8 *source = data + slot + RCP2_USER_SYSEX_NAME_SIZE;
		size_t length = RCP2_USER_SYSEX_DATA_SIZE;
		// RCPFormat.txt defines F7 as padding. Strip padding here; the
		// SysEx processor emits one final F7 when the template has no terminator.
		while (length > 0 && source[length - 1] == 0xf7)
			--length;
		out.user_sysex[i].data.assign(source, source + length);
	}
	return true;
}

bool parse_track_header(const u8 *data, size_t size, track_header &out, std::string &err)
{
	out = {};
	err.clear();

	if (data == nullptr) {
		err = "RCP トラックヘッダーが null です";
		return false;
	}
	if (size < RCP2_TRACK_HEADER_SIZE) {
		err = "RCP トラックヘッダーが短すぎます";
		return false;
	}

	const u16 encoded_length = static_cast<u16>(data[0]) |
	                          (static_cast<u16>(data[1]) << 8);
	// Ordinary RCP2 track lengths are multiples of four, leaving bits 0-1
	// unused. Recomposer-compatible writers use them as length bits 16-17.
	out.encoded_length = static_cast<u32>((encoded_length & ~u16(0x03)) |
	                                      ((encoded_length & 0x03) << 16));
	out.track_id = data[2];
	out.rhythm_mode = data[3];
	out.midi_channel = data[4];
	out.key = data[5];
	out.tick_offset = data[6];
	out.mute = data[7];
	out.name = fixed_string(data + 8, 0x24);
	return true;
}

bool parse_sequence(const u8 *data, size_t size,
                    std::vector<sequence_event> &out,
                    size_t &bytes_consumed, std::string &err)
{
	out.clear();
	bytes_consumed = 0;
	err.clear();

	if (data == nullptr) {
		err = "RCP シーケンスデータが null です";
		return false;
	}

	// Only inspect complete records. Avoid arithmetic that can overflow.
	const size_t record_count = size / 4;
	for (size_t i = 0; i < record_count; ++i) {
		const size_t offset = i * 4;
		sequence_event event;
		event.code = data[offset];
		event.p0 = data[offset + 1];
		event.p1 = data[offset + 2];
		event.p2 = data[offset + 3];
		out.push_back(event);
		bytes_consumed = offset + 4;

		if (event.code == 0xfe)
			return true;
	}

	out.clear();
	bytes_consumed = 0;
	if (size % 4 != 0)
		err = "RCP シーケンスの末尾が 4 byte レコード境界に一致しません";
	else
		err = "RCP シーケンスに Track End (FE) がありません";
	return false;
}


bool extract_comment_data(const std::vector<sequence_event> &sequence,
                          size_t start_index, std::vector<u8> &out,
                          size_t &records_consumed, std::string &err)
{
    out.clear();
    records_consumed = 0;
    err.clear();

    if (start_index >= sequence.size() || sequence[start_index].code != 0xf6) {
        err = "RCP コメント抽出の開始位置が F6 ではありません";
        return false;
    }

    // Require a track terminator in the supplied sequence before interpreting
    // the record chain. parse_sequence normally guarantees this, but callers
    // can construct the intermediate vector directly.
    bool found_end = false;
    for (size_t i = start_index; i < sequence.size(); ++i) {
        if (sequence[i].code == 0xfe) {
            found_end = true;
            break;
        }
    }
    if (!found_end) {
        err = "RCP コメント抽出の入力に Track End (FE) がありません";
        return false;
    }

    for (size_t i = start_index; i < sequence.size(); ++i) {
        const sequence_event &event = sequence[i];
        if (i != start_index && event.code != 0xf7)
            break;
        if (event.code == 0xfe)
            break;

        out.push_back(event.p1);
        out.push_back(event.p2);
        ++records_consumed;
    }
    return true;
}


size_t trim_comment_trailing_spaces(std::vector<u8> &data)
{
    while (!data.empty() && data.back() == 0x20)
        data.pop_back();
    return data.size();
}


bool decode_key_signature_event(const sequence_event &raw,
                                key_signature_event &out, std::string &err)
{
    out = {};
    err.clear();

    if (raw.code != 0xf5) {
        err = "RCP レコードは F5 キー署名コマンドではありません";
        return false;
    }

    // Mirrors the referenced converter's mapping: bit 3 selects flats,
    // low three bits give the count, and bit 4 selects minor.
    const int count = raw.p0 & 0x07;
    out.accidentals = (raw.p0 & 0x08) ? -count : count;
    out.minor = (raw.p0 & 0x10) != 0;
    // In the reference handler p0 is consumed as key data and then cleared,
    // so it does not advance the event timeline as a delay.
    out.delay_advance = 0;
    return true;
}


bool decode_note_event(const sequence_event &raw, note_event &out, std::string &err)
{
    out = {};
    err.clear();

    if (raw.code >= 0x80) {
        err = "RCP レコードはノートではなくコマンドです";
        return false;
    }

    out.note = raw.code;
    out.delay = raw.p0;
    out.duration = raw.p1;
    out.velocity = raw.p2;
    return true;
}


bool expand_note_only_timeline(const std::vector<sequence_event> &sequence,
                               std::vector<timed_note_event> &out,
                               std::string &err)
{
    out.clear();
    err.clear();

    u32 current_tick = 0;
    bool found_end = false;
    for (const sequence_event &raw : sequence) {
        if (raw.code == 0xfe) {
            found_end = true;
            break;
        }
        if (raw.code >= 0x80) {
            out.clear();
            err = "ノート専用時間展開では未対応コマンドを扱えません";
            return false;
        }

        note_event note;
        if (!decode_note_event(raw, note, err)) {
            out.clear();
            return false;
        }
        timed_note_event timed;
        timed.start_tick = current_tick;
        timed.note = note;
        out.push_back(timed);

        if (note.delay > 0xffffffffu - current_tick) {
            out.clear();
            err = "RCP ノートの tick が 32 bit 範囲を超えます";
            return false;
        }
        current_tick += note.delay;
    }

    if (!found_end) {
        out.clear();
        err = "ノート専用時間展開に Track End (FE) がありません";
        return false;
    }
    return true;
}



bool decode_tempo_event(const sequence_event &raw, u16 bpm,
                        tempo_event &out, std::string &err)
{
    out = tempo_event{};
    err.clear();

    if (raw.code != 0xe7) {
        err = "テンポイベントのコマンドが E7 ではありません";
        return false;
    }

    u32 tempo = 0;
    if (!rcp2_tempo_us_per_quarter(bpm, raw.p1, tempo, err))
        return false;

    out.delay = raw.p0;
    out.scale = raw.p1;
    out.gradual = raw.p2;
    out.microseconds_per_quarter = tempo;
    return true;
}


bool rcp2_tempo_us_per_quarter(u16 bpm, u8 scale, u32 &out, std::string &err)
{
    out = 0;
    err.clear();

    if (bpm == 0 || scale == 0) {
        err = "RCP2 テンポ計算では BPM と scale に 0 を指定できません";
        return false;
    }

    // Match ValleyBell/MidiConverters' Tempo2Mid integer formula without rounding:
    // 60,000,000 * 64 / (BPM * scale). Use u64 for the numerator.
    const u64 denominator = static_cast<u64>(bpm) * static_cast<u64>(scale);
    out = static_cast<u32>((60000000ull * 64ull) / denominator);
    return true;
}


bool expand_note_key_signature_timeline(const std::vector<sequence_event> &sequence,
                                        std::vector<timed_event> &out,
                                        std::string &err)
{
    out.clear();
    err.clear();

    u32 current_tick = 0;
    bool found_end = false;
    for (const sequence_event &raw : sequence) {
        if (raw.code == 0xfe) {
            found_end = true;
            break;
        }

        timed_event timed;
        timed.tick = current_tick;
        if (raw.code == 0xf5) {
            if (!decode_key_signature_event(raw, timed.key, err)) {
                out.clear();
                return false;
            }
            timed.kind = timeline_event_kind::key_signature;
            out.push_back(timed);
            // F5 p0 is key-signature data in the reference implementation.
            continue;
        }

        if (raw.code >= 0x80) {
            out.clear();
            err = "時間展開では F5 以外の未対応コマンドを扱えません";
            return false;
        }

        if (!decode_note_event(raw, timed.note, err)) {
            out.clear();
            return false;
        }
        timed.kind = timeline_event_kind::note;
        out.push_back(timed);

        if (timed.note.delay > 0xffffffffu - current_tick) {
            out.clear();
            err = "RCP イベントの tick が 32 bit 範囲を超えます";
            return false;
        }
        current_tick += timed.note.delay;
    }

    if (!found_end) {
        out.clear();
        err = "時間展開に Track End (FE) がありません";
        return false;
    }
    return true;
}



bool decode_midi_channel_event(const sequence_event &raw,
                              midi_channel_event &out,
                              timeline_event_kind &kind,
                              std::string &err)
{
    out = {};
    kind = timeline_event_kind::note;
    err.clear();

    if (raw.code == 0xea) {
        if (raw.p1 > 0x7f) {
            err = "RCP EA Channel Aftertouch の値が 7 bit 範囲外です";
            return false;
        }
        out.delay = raw.p0;
        out.data1 = raw.p1;
        out.data2 = 0;
        kind = timeline_event_kind::channel_aftertouch;
        return true;
    }

    if (raw.code == 0xeb || raw.code == 0xd2) {
        if (raw.p1 > 0x7f || raw.p2 > 0x7f) {
            err = "RCP EB Control Change のデータ値が 7 bit 範囲外です";
            return false;
        }
        out.delay = raw.p0;
        out.data1 = raw.p1;
        out.data2 = raw.p2;
        kind = timeline_event_kind::control_change;
        return true;
    }

    if (raw.code == 0xec || raw.code == 0xd3) {
        if (raw.p1 >= 0x80) {
            err = "RCP EC の特殊インストゥルメント値は未対応です";
            return false;
        }
        out.delay = raw.p0;
        out.data1 = raw.p1;
        out.data2 = 0;
        kind = timeline_event_kind::program_change;
        return true;
    }

    if (raw.code == 0xed) {
        if (raw.p1 > 0x7f || raw.p2 > 0x7f) {
            err = "RCP ED Polyphonic Aftertouch の値が 7 bit 範囲外です";
            return false;
        }
        out.delay = raw.p0;
        out.data1 = raw.p1;
        out.data2 = raw.p2;
        kind = timeline_event_kind::poly_aftertouch;
        return true;
    }

    if (raw.code == 0xee || raw.code == 0xd0) {
        if (raw.p1 > 0x7f || raw.p2 > 0x7f) {
            err = "RCP EE Pitch Bend のデータ値が 7 bit 範囲外です";
            return false;
        }
        out.delay = raw.p0;
        out.data1 = raw.p1;
        out.data2 = raw.p2;
        kind = timeline_event_kind::pitch_bend;
        return true;
    }

    err = "RCP レコードは EA/EB/EC/ED/EE MIDI チャンネルコマンドではありません";
    return false;
}


bool encode_midi_message(const timed_event &event, u8 channel,
                         std::vector<u8> &out, std::string &err)
{
    out.clear();
    err.clear();
    if (channel > 0x0f) {
        err = "MIDI チャンネルが 0..15 の範囲外です";
        return false;
    }

    switch (event.kind) {
    case timeline_event_kind::note:
        if (event.note.note > 0x7f || event.note.velocity > 0x7f) {
            err = "MIDI ノート番号またはベロシティが 7 bit 範囲外です";
            return false;
        }
        out = {u8(0x90 | channel), event.note.note, event.note.velocity};
        return true;
    case timeline_event_kind::note_off:
        if (event.note.note > 0x7f) {
            err = "MIDI ノートオフのノート番号が 7 bit 範囲外です";
            return false;
        }
        // Match the reference converter: note termination is Note On with velocity zero.
        out = {u8(0x90 | channel), event.note.note, 0};
        return true;
    case timeline_event_kind::control_change:
        if (event.channel.data1 > 0x7f || event.channel.data2 > 0x7f) {
            err = "MIDI Control Change のデータ値が 7 bit 範囲外です";
            return false;
        }
        out = {u8(0xb0 | channel), event.channel.data1, event.channel.data2};
        return true;
    case timeline_event_kind::program_change:
        if (event.channel.data1 > 0x7f) {
            err = "MIDI Program Change のデータ値が 7 bit 範囲外です";
            return false;
        }
        out = {u8(0xc0 | channel), event.channel.data1};
        return true;
    case timeline_event_kind::channel_aftertouch:
        if (event.channel.data1 > 0x7f) {
            err = "MIDI Channel Aftertouch の値が 7 bit 範囲外です";
            return false;
        }
        out = {u8(0xd0 | channel), event.channel.data1};
        return true;
    case timeline_event_kind::poly_aftertouch:
        if (event.channel.data1 > 0x7f || event.channel.data2 > 0x7f) {
            err = "MIDI Polyphonic Aftertouch の値が 7 bit 範囲外です";
            return false;
        }
        out = {u8(0xa0 | channel), event.channel.data1, event.channel.data2};
        return true;
    case timeline_event_kind::pitch_bend:
        if (event.channel.data1 > 0x7f || event.channel.data2 > 0x7f) {
            err = "MIDI Pitch Bend のデータ値が 7 bit 範囲外です";
            return false;
        }
        out = {u8(0xe0 | channel), event.channel.data1, event.channel.data2};
        return true;
    case timeline_event_kind::key_signature:
    case timeline_event_kind::tempo:
    case timeline_event_kind::sysex:
    case timeline_event_kind::route_change:
        err = "メタデータイベント、SysEx、ルート変更は MIDI チャンネルメッセージではありません";
        return false;
    }
    err = "未対応の MIDI イベント種別です";
    return false;
}


namespace {

struct counted_loop_frame {
    size_t start_index = 0;
    u8 total_passes = 0;
    u8 completed_passes = 0;
};

// Expands only finite F9/F8 loops. Zero and >=0x7f counts have converter-
// specific/infinite interpretations and are rejected until their policy is settled.
// Expands RCP2 FC same-measure references using byte offsets from the start
// of the track, including its 44-byte header. FCs encountered exactly at the
// current target tail-call to their own target; an FC later in the stream ends
// the repeat and returns to the record after the initiating FC.
bool expand_fc_repeats(const std::vector<sequence_event> &sequence,
                       std::vector<sequence_event> &out, std::string &err)
{
    out.clear();
    err.clear();
    constexpr size_t TRACK_HEADER_SIZE = 0x2c;
    constexpr size_t MAX_EXPANDED_RECORDS = 1000000;
    size_t pc = 0, return_pc = 0, target_index = 0, steps = 0;
    bool in_repeat = false;
    std::vector<size_t> visited_fc;

    auto fail = [&](const char *message) {
        out.clear();
        err = message;
        return false;
    };
    auto resolve_target = [&](const sequence_event &raw, size_t &index) {
        const size_t byte_offset = size_t(raw.p1) | (size_t(raw.p2) << 8);
        if (byte_offset < TRACK_HEADER_SIZE)
            return false;
        const size_t sequence_offset = byte_offset - TRACK_HEADER_SIZE;
        if ((sequence_offset % 4) != 0)
            return false;
        index = sequence_offset / 4;
        return index < sequence.size() && sequence[index].code != 0xfe;
    };
    auto mark_fc = [&](size_t index) {
        if (std::find(visited_fc.begin(), visited_fc.end(), index) != visited_fc.end())
            return false;
        visited_fc.push_back(index);
        return true;
    };

    while (pc < sequence.size()) {
        if (++steps > MAX_EXPANDED_RECORDS)
            return fail("RCP FC 展開がレコード予算を超えました");
        const sequence_event &raw = sequence[pc];
        if (raw.code == 0xfe) {
            if (in_repeat)
                return fail("RCP FC 参照が Track End (FE) より前に終端できません");
            out.push_back(raw);
            return true;
        }
        // The reference converter treats FD inside an FC-referenced span as
        // the return point: emit the measure-end marker, then resume after the
        // initiating FC instead of copying the rest of the referenced region.
        if (in_repeat && raw.code == 0xfd) {
            if (out.size() >= MAX_EXPANDED_RECORDS)
                return fail("RCP FC 展開後のレコード数が上限を超えました");
            out.push_back(raw);
            in_repeat = false;
            visited_fc.clear();
            pc = return_pc;
            continue;
        }
        if (raw.code != 0xfc) {
            if (out.size() >= MAX_EXPANDED_RECORDS)
                return fail("RCP FC 展開後のレコード数が上限を超えました");
            out.push_back(raw);
            ++pc;
            continue;
        }

        if (!in_repeat) {
            size_t target = 0;
            if (!resolve_target(raw, target))
                return fail("RCP FC 参照先がトラック範囲外、ヘッダー内、または未整列です");
            in_repeat = true;
            return_pc = pc + 1;
            target_index = target;
            visited_fc.clear();
            if (!mark_fc(pc))
                return fail("RCP FC 参照チェーンに循環があります");
            pc = target_index;
            continue;
        }

        if (pc < target_index)
            return fail("RCP FC 参照チェーンの位置関係が不正です");
        if (pc > target_index) {
            in_repeat = false;
            visited_fc.clear();
            pc = return_pc;
            continue;
        }

        // An FC exactly at the target position is part of the reference
        // chain, not the end of the repeated span. Skip it when returning to
        // the caller, otherwise the chain's FC would be executed again.
        return_pc = std::max(return_pc, pc + 1);
        if (!mark_fc(pc))
            return fail("RCP FC 参照チェーンに循環があります");
        size_t target = 0;
        if (!resolve_target(raw, target))
            return fail("RCP FC 連鎖の参照先がトラック範囲外、ヘッダー内、または未整列です");
        target_index = target;
        pc = target_index;
    }

    return fail("RCP FC 展開に Track End (FE) がありません");
}

bool expand_f8_f9_loops(const std::vector<sequence_event> &sequence,
                        std::vector<sequence_event> &out, std::string &err)
{
    out.clear();
    err.clear();
    constexpr size_t MAX_EXPANDED_RECORDS = 1000000;

    // Match finite F9/F8 pairs before expansion. Real RCP2 tracks can contain
    // an unclosed F9 that marks a track-level loop point; it must not consume
    // a later F8 belonging to a nested finite loop. Only paired F9 records
    // participate in counted-loop expansion. Unmatched F8 is still rejected
    // by the normal expansion path below.
    std::vector<bool> f9_has_f8(sequence.size(), false);
    std::vector<size_t> marker_stack;
    for (size_t i = 0; i < sequence.size(); ++i) {
        if (sequence[i].code == 0xfe)
            break;
        if (sequence[i].code == 0xf9) {
            marker_stack.push_back(i);
        } else if (sequence[i].code == 0xf8 && !marker_stack.empty()) {
            f9_has_f8[marker_stack.back()] = true;
            marker_stack.pop_back();
        }
    }

    std::vector<counted_loop_frame> stack;
    size_t pc = 0;
    size_t steps = 0;
    bool found_end = false;

    while (pc < sequence.size()) {
        if (++steps > MAX_EXPANDED_RECORDS) {
            out.clear();
            err = "RCP F8/F9 ループ展開がレコード予算を超えました";
            return false;
        }
        const sequence_event &raw = sequence[pc];
        if (raw.code == 0xfe) {
            out.push_back(raw);
            found_end = true;
            break;
        }
        if (raw.code == 0xf9) {
            if (f9_has_f8[pc])
                stack.push_back({pc + 1, 0, 0});
            ++pc;
            continue;
        }
        if (raw.code == 0xf8) {
            if (stack.empty()) {
                out.clear();
                err = "RCP F8 ループ終端に対応する F9 がありません";
                return false;
            }
            if (raw.p0 == 0 || raw.p0 >= 0x7f) {
                out.clear();
                err = "RCP F8 の反復回数 0 または 0x7f 以上は未対応です";
                return false;
            }
            counted_loop_frame &frame = stack.back();
            if (frame.total_passes == 0)
                frame.total_passes = raw.p0;
            ++frame.completed_passes;
            if (frame.completed_passes < frame.total_passes) {
                pc = frame.start_index;
            } else {
                stack.pop_back();
                ++pc;
            }
            continue;
        }

        // Continuation records are copied only when consumed by their owner
        // (0x98/0x99/F6) above. Drop orphan F7 records here, before flattening loop
        // iterations, so one outside a loop cannot attach to the last expanded
        // 0x98 inside that loop during the later timeline conversion.
        if (raw.code == 0xf7) {
            ++pc;
            continue;
        }

        // Keep commands that own F7 continuation records together while
        // unrolling loops. Otherwise a continuation just after F8 could be
        // incorrectly attached to the last expanded owner from inside the loop.
        if (raw.code == 0x98 || raw.code == 0x99 || raw.code == 0xf6) {
            if (out.size() >= MAX_EXPANDED_RECORDS) {
                out.clear();
                err = "RCP F8/F9 展開後のレコード数が上限を超えました";
                return false;
            }
            out.push_back(raw);
            ++pc;
            while (pc < sequence.size() && sequence[pc].code == 0xf7) {
                if (out.size() >= MAX_EXPANDED_RECORDS) {
                    out.clear();
                    err = "RCP F8/F9 展開後のレコード数が上限を超えました";
                    return false;
                }
                out.push_back(sequence[pc]);
                ++pc;
            }
            continue;
        }

        if (out.size() >= MAX_EXPANDED_RECORDS) {
            out.clear();
            err = "RCP F8/F9 展開後のレコード数が上限を超えました";
            return false;
        }
        out.push_back(raw);
        ++pc;
    }

    if (!found_end) {
        out.clear();
        err = "RCP F8/F9 展開に Track End (FE) がありません";
        return false;
    }
    if (!stack.empty()) {
        out.clear();
        err = "RCP F9 ループ開始に対応する F8 がありません";
        return false;
    }
    return true;
}

} // namespace

bool expand_note_key_signature_tempo_timeline(
    const std::vector<sequence_event> &sequence, u16 bpm,
    std::vector<timed_event> &out, std::string &err,
    const std::array<user_sysex_template, 8> *user_sysex,
    u8 initial_midi_channel)
{
    out.clear();
    err.clear();

    std::vector<sequence_event> fc_expanded_sequence;
    if (!expand_fc_repeats(sequence, fc_expanded_sequence, err)) {
        out.clear();
        return false;
    }
    std::vector<sequence_event> expanded_sequence;
    if (!expand_f8_f9_loops(fc_expanded_sequence, expanded_sequence, err)) {
        out.clear();
        return false;
    }

    u32 current_tick = 0;
    // Reference rcp2mid.c initializes all six GS parameter bytes to zero.
    u8 gs_device = 0, gs_model = 0, gs_address_high = 0, gs_address_low = 0;
    u8 current_midi_channel = static_cast<u8>(initial_midi_channel & 0x0f);
    bool current_route_enabled = true;
    bool found_end = false;
    for (size_t record_index = 0; record_index < expanded_sequence.size(); ++record_index) {
        const sequence_event &raw = expanded_sequence[record_index];
        if (raw.code == 0xfe) {
            found_end = true;
            break;
        }

        // F6 starts a comment and immediately following F7 records continue it.
        // Comment bytes are metadata, not timed MIDI events.
        if (raw.code == 0xf6) {
            while (record_index + 1 < expanded_sequence.size() &&
                   expanded_sequence[record_index + 1].code == 0xf7)
                ++record_index;
            continue;
        }
        // FD marks a measure boundary; its step byte does not advance time.
        if (raw.code == 0xfd)
            continue;
        // Reference converters skip a continuation with no owning command.
        // Such a record can occur when a continuation is outside an F8/F9 loop.
        if (raw.code == 0xf7)
            continue;

        // GS parameter state commands, matching the reference converter.
        if (raw.code == 0xdd || raw.code == 0xde || raw.code == 0xdf) {
            if (raw.p1 > 0x7f || raw.p2 > 0x7f) {
                out.clear();
                err = "RCP DD/DE/DF の SysEx 値が 7 bit 範囲外です";
                return false;
            }
            if (raw.code == 0xdd) {
                gs_address_high = raw.p1;
                gs_address_low = raw.p2;
            } else if (raw.code == 0xdf) {
                gs_device = raw.p1;
                gs_model = raw.p2;
            } else {
                timed_event sysex;
                sysex.tick = current_tick;
                sysex.kind = timeline_event_kind::sysex;
                const u8 checksum = static_cast<u8>((0x80u -
                    ((static_cast<unsigned>(gs_address_high) + gs_address_low +
                      raw.p1 + raw.p2) & 0x7fu)) & 0x7fu);
                sysex.sysex = {0xf0, 0x41, gs_device, gs_model, 0x12,
                    gs_address_high, gs_address_low, raw.p1, raw.p2, checksum, 0xf7};
                out.push_back(std::move(sysex));
            }
            if (raw.p0 > 0xffffffffu - current_tick) {
                out.clear();
                err = "RCP イベントの tick が 32 bit 範囲を超えます";
                return false;
            }
            current_tick += raw.p0;
            continue;
        }

        // E6 changes the active MIDI port/channel for subsequent events.
        // The encoded value is one-based; subtract one before decoding.
        if (raw.code == 0xe6) {
            const u8 route_value = static_cast<u8>(raw.p1 - 1);
            timed_event route;
            route.tick = current_tick;
            route.kind = timeline_event_kind::route_change;
            if (raw.p1 == 0 || (route_value & 0x80)) {
                current_route_enabled = false;
                route.route.enabled = false;
                route.route.channel = 0;
                route.route.port = 0;
            } else {
                current_route_enabled = true;
                current_midi_channel = route_value & 0x0f;
                route.route.enabled = true;
                route.route.port = route_value >> 4;
                route.route.channel = current_midi_channel;
            }
            out.push_back(route);
            if (raw.p0 > 0xffffffffu - current_tick) {
                out.clear();
                err = "RCP イベントの tick が 32 bit 範囲を超えます";
                return false;
            }
            current_tick += raw.p0;
            continue;
        }

        // 0x99 is an external-command instruction, not a MIDI message.
        // Reference converters gather its following F7 records as command text,
        // then ignore the instruction for MIDI output. Keep the group atomic in
        // loop expansion and preserve only the command's p0 time advance.
        if (raw.code == 0x99) {
            while (record_index + 1 < expanded_sequence.size() &&
                   expanded_sequence[record_index + 1].code == 0xf7)
                ++record_index;
            if (raw.p0 > 0xffffffffu - current_tick) {
                out.clear();
                err = "RCP イベントの tick が 32 bit 範囲を超えます";
                return false;
            }
            current_tick += raw.p0;
            continue;
        }

        // 0x98 carries a SysEx template in the p1/p2 bytes of following F7
        // continuation records. Gather those records after loop expansion so a
        // loop repeats the complete SysEx payload, then trim trailing F7 padding.
        if (raw.code == 0x98) {
            if (raw.p1 > 0x7f || raw.p2 > 0x7f) {
                out.clear();
                err = "RCP 98 SysEx パラメーターが 7 bit 範囲外です";
                return false;
            }
            std::vector<u8> payload;
            while (record_index + 1 < expanded_sequence.size() &&
                   expanded_sequence[record_index + 1].code == 0xf7) {
                const sequence_event &continuation = expanded_sequence[++record_index];
                payload.push_back(continuation.p1);
                payload.push_back(continuation.p2);
            }
            while (!payload.empty() && payload.back() == 0xf7)
                payload.pop_back();

            if (current_route_enabled && !payload.empty()) {
                timed_event sysex;
                sysex.tick = current_tick;
                sysex.kind = timeline_event_kind::sysex;
                sysex.sysex.push_back(0xf0);
                u8 checksum = 0;
                auto append_data = [&](u8 value) {
                    sysex.sysex.push_back(value);
                    checksum = static_cast<u8>((checksum + value) & 0x7f);
                };
                for (u8 byte : payload) {
                    u8 value = byte;
                    if (byte == 0x80) value = raw.p1;
                    else if (byte == 0x81) value = raw.p2;
                    else if (byte == 0x82) value = current_midi_channel;
                    else if (byte == 0x83) { checksum = 0; continue; }
                    else if (byte == 0x84) value = static_cast<u8>((0x80u - checksum) & 0x7f);
                    else if (byte == 0xf7) {
                        sysex.sysex.push_back(0xf7);
                        break;
                    } else if ((byte & 0x80) != 0) {
                        // The reference RCP2 converter warns and skips unknown
                        // high-bit SysEx template bytes (for example 0xFF).
                        // Do not emit them as MIDI data or reject the whole file.
                        continue;
                    }
                    append_data(value);
                }
                if (sysex.sysex.back() != 0xf7)
                    sysex.sysex.push_back(0xf7);
                if (sysex.sysex.size() > 2)
                    out.push_back(std::move(sysex));
            }
            if (raw.p0 > 0xffffffffu - current_tick) {
                out.clear();
                err = "RCP イベントの tick が 32 bit 範囲を超えます";
                return false;
            }
            current_tick += raw.p0;
            continue;
        }

        // User SysEx slots 1..8 are stored in the RCP2 fixed header at 0x406.
        // Their template bytes are processed with the same substitution rules as 0x98.
        if (raw.code >= 0x90 && raw.code <= 0x97) {
            if (user_sysex == nullptr) {
                out.clear();
                err = "RCP 90..97 ユーザー SysEx にヘッダーテンプレートが渡されていません";
                return false;
            }
            if (raw.p1 > 0x7f || raw.p2 > 0x7f) {
                out.clear();
                err = "RCP 90..97 SysEx パラメーターが 7 bit 範囲外です";
                return false;
            }
            const user_sysex_template &slot = (*user_sysex)[raw.code & 0x07];
            if (slot.data.size() > 0x18) {
                out.clear();
                err = "RCP 90..97 ユーザー SysEx テンプレートが上限を超えています";
                return false;
            }
            if (current_route_enabled && !slot.data.empty()) {
                timed_event sysex;
                sysex.tick = current_tick;
                sysex.kind = timeline_event_kind::sysex;
                sysex.sysex.push_back(0xf0);
                u8 checksum = 0;
                bool ended = false;
                auto append_data = [&](u8 value) {
                    sysex.sysex.push_back(value);
                    checksum = static_cast<u8>(checksum + value);
                };
                for (u8 byte : slot.data) {
                    if (byte == 0x80) {
                        append_data(raw.p1);
                    } else if (byte == 0x81) {
                        append_data(raw.p2);
                    } else if (byte == 0x82) {
                        append_data(current_midi_channel);
                    } else if (byte == 0x83) {
                        checksum = 0;
                    } else if (byte == 0x84) {
                        append_data(static_cast<u8>((0x100u - checksum) & 0x7fu));
                    } else if (byte == 0xf7) {
                        sysex.sysex.push_back(0xf7);
                        ended = true;
                        break;
                    } else if ((byte & 0x80) == 0) {
                        append_data(byte);
                    } else {
                        out.clear();
                        err = "RCP 90..97 ユーザー SysEx に未知の置換コードがあります";
                        return false;
                    }
                }
                if (!ended && !sysex.sysex.empty())
                    sysex.sysex.push_back(0xf7);
                if (sysex.sysex.size() > 1)
                    out.push_back(std::move(sysex));
            }
            if (raw.p0 > 0xffffffffu - current_tick) {
                out.clear();
                err = "RCP イベントの tick が 32 bit 範囲を超えます";
                return false;
            }
            current_tick += raw.p0;
            continue;
        }

        // E5 is the RCP2 "Key Scan" command. The reference converter
        // warns about it but emits no MIDI event; its p0 delay still advances
        // the track clock like other non-emitting commands.
        if (raw.code == 0xe5) {
            if (raw.p0 > 0xffffffffu - current_tick) {
                out.clear();
                err = "RCP E5 Key Scan の tick が 32 bit 範囲を超えます";
                return false;
            }
            current_tick += raw.p0;
            continue;
        }

        // Reference rcp2mid.c: E1 selects an XG instrument (CC32 = p2,
        // Program Change = p1); E2 selects a GS instrument (CC0 = p2,
        // Program Change = p1). p0 is the step/delay for either command.
        if (raw.code == 0xe1 || raw.code == 0xe2) {
            if (raw.p1 > 0x7f || raw.p2 > 0x7f) {
                out.clear();
                err = "RCP E1/E2 の MIDI 値が 7 bit 範囲外です";
                return false;
            }

            timed_event bank;
            bank.tick = current_tick;
            bank.kind = timeline_event_kind::control_change;
            bank.channel.data1 = raw.code == 0xe1 ? 32 : 0;
            bank.channel.data2 = raw.p2;
            out.push_back(bank);

            timed_event program;
            program.tick = current_tick;
            program.kind = timeline_event_kind::program_change;
            program.channel.data1 = raw.p1;
            out.push_back(program);

            if (raw.p0 > 0xffffffffu - current_tick) {
                out.clear();
                err = "RCP イベントの tick が 32 bit 範囲を超えます";
                return false;
            }
            current_tick += raw.p0;
            continue;
        }

        timed_event timed;
        timed.tick = current_tick;
        u32 delay_advance = 0;

        if (raw.code == 0xf5) {
            if (!decode_key_signature_event(raw, timed.key, err)) {
                out.clear();
                return false;
            }
            timed.kind = timeline_event_kind::key_signature;
            delay_advance = timed.key.delay_advance;
        } else if (raw.code == 0xe7) {
            if (!decode_tempo_event(raw, bpm, timed.tempo, err)) {
                out.clear();
                return false;
            }
            timed.kind = timeline_event_kind::tempo;
            delay_advance = timed.tempo.delay;
        } else if (raw.code == 0xd0 || raw.code == 0xd2 || raw.code == 0xd3 || raw.code == 0xea || raw.code == 0xeb || raw.code == 0xec || raw.code == 0xed || raw.code == 0xee) {
            if (!decode_midi_channel_event(raw, timed.channel, timed.kind, err)) {
                out.clear();
                return false;
            }
            delay_advance = timed.channel.delay;
        } else if (raw.code < 0x80) {
            if (!decode_note_event(raw, timed.note, err)) {
                out.clear();
                return false;
            }
            timed.kind = timeline_event_kind::note;
            delay_advance = timed.note.delay;
        } else {
            out.clear();
            err = "時間展開ではノート・D0/D2/D3・EA/EB/EC/ED/EE・F5・E7 以外のコマンドを扱えません";
            return false;
        }

        out.push_back(timed);
        if (delay_advance > 0xffffffffu - current_tick) {
            out.clear();
            err = "RCP イベントの tick が 32 bit 範囲を超えます";
            return false;
        }
        current_tick += delay_advance;
    }

    if (!found_end) {
        out.clear();
        err = "時間展開に Track End (FE) がありません";
        return false;
    }
    return true;
}


bool merge_timed_tracks(const std::vector<std::vector<timed_event>> &tracks,
                        std::vector<timed_track_event> &out, std::string &err)
{
    out.clear();
    err.clear();
    for (const auto &track_events : tracks) {
        for (size_t i = 1; i < track_events.size(); ++i) {
            if (track_events[i].tick < track_events[i - 1].tick) {
                err = "RCP トラック内のイベントが tick 順ではありません";
                return false;
            }
        }
    }
    std::vector<size_t> positions(tracks.size(), 0);
    size_t remaining = 0;
    for (const auto &track_events : tracks)
        remaining += track_events.size();
    out.reserve(remaining);
    while (remaining != 0) {
        size_t selected = tracks.size();
        u32 selected_tick = 0;
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (positions[i] >= tracks[i].size())
                continue;
            const u32 tick = tracks[i][positions[i]].tick;
            if (selected == tracks.size() || tick < selected_tick) {
                selected = i;
                selected_tick = tick;
            }
        }
        if (selected == tracks.size()) {
            out.clear();
            err = "RCP トラック統合で内部状態が不正です";
            return false;
        }
        out.push_back({selected, tracks[selected][positions[selected]++]});
        --remaining;
    }
    return true;
}


bool parse_tracks(const u8 *data, size_t size,
                  std::vector<track> &out, std::string &err)
{
    out.clear();
    err.clear();

    document_header header;
    if (!parse_header(data, size, header, err))
        return false;

    size_t offset = RCP2_HEADER_SIZE;
    for (size_t i = 0; i < header.track_count; ++i) {
        if (offset > size || size - offset < RCP2_TRACK_HEADER_SIZE) {
            out.clear();
            err = "RCP2 トラックヘッダーがファイル末尾を越えています";
            return false;
        }

        track parsed;
        if (!parse_track_header(data + offset, size - offset, parsed.header, err)) {
            out.clear();
            return false;
        }

        const size_t track_length = parsed.header.encoded_length;
        if (track_length < RCP2_TRACK_HEADER_SIZE) {
            out.clear();
            err = "RCP2 トラック長がトラックヘッダーより短いです";
            return false;
        }
        if (track_length > size - offset) {
            out.clear();
            err = "RCP2 トラック長がファイル末尾を越えています";
            return false;
        }

        const size_t sequence_size = track_length - RCP2_TRACK_HEADER_SIZE;
        size_t consumed = 0;
        if (!parse_sequence(data + offset + RCP2_TRACK_HEADER_SIZE,
                            sequence_size, parsed.sequence, consumed, err)) {
            out.clear();
            err = "RCP2 トラック " + std::to_string(i + 1) +
                  " のシーケンスが不正です: " + err;
            return false;
        }

        // parse_sequence stops at FE. The encoded track boundary is authoritative;
        // bytes after FE remain unparsed padding/unknown data.
        out.push_back(parsed);
        offset += track_length;
    }
    return true;
}


bool build_intermediate(const u8 *data, size_t size,
                        intermediate_document &out, std::string &err)
{
    out = {};
    err.clear();

    if (!parse_header(data, size, out.header, err))
        return false;

    std::vector<track> parsed_tracks;
    if (!parse_tracks(data, size, parsed_tracks, err)) {
        out = {};
        return false;
    }

    out.tracks.reserve(parsed_tracks.size());
    for (const track &source : parsed_tracks) {
        intermediate_track target;
        target.header = source.header;
        target.events.reserve(source.sequence.size());
        for (size_t i = 0; i < source.sequence.size(); ++i) {
            intermediate_event event;
            event.record_index = i;
            event.raw = source.sequence[i];
            target.events.push_back(event);
        }
        out.tracks.push_back(target);
    }
    return true;
}

bool apply_note_transposition(u8 note, u8 raw_track_transposition,
                              u8 raw_document_transposition, u8 &out,
                              std::string &err)
{
    out = 0;
    err.clear();
    if (note > 0x7f) {
        err = "RCP ノート番号が 7 bit 範囲外です";
        return false;
    }

    int transpose = 0;
    if ((raw_track_transposition & 0x80) == 0) {
        const int track_value = raw_track_transposition & 0x7f;
        const int track_offset = (track_value & 0x40) ? track_value - 0x80 : track_value;
        const int document_offset = raw_document_transposition < 0x80
            ? raw_document_transposition : int(raw_document_transposition) - 0x100;
        transpose = track_offset + document_offset;
    }
    int adjusted_note = (int(note) + transpose) % 128;
    if (adjusted_note < 0)
        adjusted_note += 128;
    out = static_cast<u8>(adjusted_note);
    return true;
}


bool apply_signed_track_offset(u32 tick, u8 raw_offset, u32 &out, std::string &err)
{
    out = 0;
    err.clear();

    const int tick_offset = raw_offset < 0x80 ? raw_offset : int(raw_offset) - 0x100;
    const long long adjusted = static_cast<long long>(tick) + tick_offset;
    if (adjusted > 0xffffffffll) {
        err = "RCP トラック開始オフセット適用後の tick が 32 bit 範囲を超えます";
        return false;
    }

    out = adjusted < 0 ? 0u : static_cast<u32>(adjusted);
    return true;
}


namespace {

bool expand_note_gates(const std::vector<timed_event> &timeline,
                       std::vector<timed_event> &out, std::string &err)
{
    struct running_note { u8 pitch; midi_route_event route; size_t note_off_index; u32 end_tick; };
    out.clear();
    err.clear();
    std::vector<timed_event> expanded;
    std::vector<running_note> running;
    expanded.reserve(timeline.size());

    for (const timed_event &event : timeline) {
        if (event.kind != timeline_event_kind::note) {
            expanded.push_back(event);
            continue;
        }
        for (size_t i = 0; i < running.size();) {
            if (running[i].end_tick <= event.tick)
                running.erase(running.begin() + i);
            else
                ++i;
        }
        if (event.note.duration > 0xffffffffu - event.tick) {
            out.clear();
            err = "RCP ノートオフの tick が 32 bit 範囲を超えます";
            return false;
        }
        const u32 new_end_tick = event.tick + event.note.duration;
        size_t active_index = running.size();
        for (size_t i = 0; i < running.size(); ++i) {
            if (running[i].pitch == event.note.note &&
                running[i].route.channel == event.route.channel &&
                running[i].route.port == event.route.port &&
                running[i].route.enabled == event.route.enabled) { active_index = i; break; }
        }
        if (active_index != running.size()) {
            const size_t note_off_index = running[active_index].note_off_index;
            if (event.note.duration == 0) {
                timed_event note_off = expanded[note_off_index];
                note_off.tick = new_end_tick;
                expanded.erase(expanded.begin() + note_off_index);
                for (running_note &other : running) {
                    if (other.note_off_index > note_off_index)
                        --other.note_off_index;
                }
                running.erase(running.begin() + active_index);
                expanded.push_back(note_off);
            } else {
                running_note &active = running[active_index];
                expanded[note_off_index].tick = new_end_tick;
                active.end_tick = new_end_tick;
            }
            continue;
        }
        if (event.note.duration == 0)
            continue;

        expanded.push_back(event);
        timed_event note_off;
        note_off.tick = new_end_tick;
        note_off.kind = timeline_event_kind::note_off;
        note_off.note = event.note;
        note_off.route = event.route;
        note_off.route_fixed = event.route_fixed;
        note_off.note.delay = 0;
        note_off.note.duration = 0;
        note_off.note.velocity = 0;
        const size_t note_off_index = expanded.size();
        expanded.push_back(note_off);
        running.push_back({event.note.note, event.route, note_off_index, new_end_tick});
    }
    // First preserve source order for every event sharing a tick.
    std::stable_sort(expanded.begin(), expanded.end(),
        [](const timed_event &a, const timed_event &b) {
            return a.tick < b.tick;
        });

    // Expired gate Note Offs precede controllers at the same tick. Controllers
    // keep their source order, and any new Note On follows them.
    // Other same-tick combinations retain their original relative order.
    for (size_t first = 0; first < expanded.size();) {
        size_t last = first + 1;
        while (last < expanded.size() &&
               expanded[last].tick == expanded[first].tick)
            ++last;

        // Keep the stable order established by the source timeline. A fixed
        // Note Off-before-controller priority does not match the reference
        // converter: a controller can be emitted before a gate Note Off when
        // the note's expiration is reached later in delay processing.
        // The reference comparison suite validates these same-tick cases.
        first = last;
    }

    out.swap(expanded);
    return true;
}

} // namespace

bool expand_intermediate_timeline(const intermediate_document &document,
                                  std::vector<timed_track_event> &out,
                                  std::string &err)
{
    out.clear();
    err.clear();
    std::vector<std::vector<timed_event>> timelines;
    timelines.reserve(document.tracks.size());
    for (size_t i = 0; i < document.tracks.size(); ++i) {
        // The referenced RCP2 layout assigns bit 0 of the mute byte to track mute.
        // Keep the track slot so merged events retain their original track index.
        if (document.tracks[i].header.mute & 0x01) {
            timelines.emplace_back();
            continue;
        }
        std::vector<sequence_event> sequence;
        sequence.reserve(document.tracks[i].events.size());
        for (const intermediate_event &event : document.tracks[i].events)
            sequence.push_back(event.raw);
        std::vector<timed_event> timeline;
        if (!expand_note_key_signature_tempo_timeline(
                sequence, document.header.tempo_bpm, timeline, err,
                &document.header.user_sysex, document.tracks[i].header.midi_channel & 0x0f)) {
            out.clear();
            err = "RCP2 トラック " + std::to_string(i + 1) +
                  " の時間展開に失敗しました: " + err;
            return false;
        }
        for (timed_event &event : timeline) {
            if (event.kind != timeline_event_kind::note) continue;
            u8 adjusted_note = 0;
            if (!apply_note_transposition(event.note.note,
                                          document.tracks[i].header.key,
                                          document.header.play_bias,
                                          adjusted_note, err)) {
                out.clear();
                return false;
            }
            event.note.note = adjusted_note;
        }
        // Resolve the active route on every Note On before note gates generate Note Offs.
        // This preserves the original route across a later E6 change and keeps
        // same-pitch notes on different routes independent.
        {
            const u8 raw_route = document.tracks[i].header.midi_channel;
            midi_route_event active_route;
            active_route.channel = raw_route & 0x0f;
            active_route.port = raw_route >> 4;
            active_route.enabled = (raw_route & 0x80) == 0;
            for (timed_event &event : timeline) {
                if (event.kind == timeline_event_kind::route_change) {
                    active_route = event.route;
                } else if (event.kind == timeline_event_kind::note) {
                    event.route = active_route;
                    event.route_fixed = true;
                }
            }
        }
        std::vector<timed_event> gated_timeline;
        if (!expand_note_gates(timeline, gated_timeline, err)) {
            out.clear();
            err = "RCP2 トラック " + std::to_string(i + 1) +
                  " のノート長展開に失敗しました: " + err;
            return false;
        }
        for (timed_event &event : gated_timeline) {
            u32 adjusted_tick = 0;
            if (!apply_signed_track_offset(event.tick,
                                           document.tracks[i].header.tick_offset,
                                           adjusted_tick, err)) {
                out.clear();
                return false;
            }
            event.tick = adjusted_tick;
        }
        timelines.push_back(std::move(gated_timeline));
    }
    if (!merge_timed_tracks(timelines, out, err)) {
        out.clear();
        return false;
    }
    return true;
}

bool convert_timeline_to_seconds(const document_header &header,
                                 const std::vector<timed_track_event> &timeline,
                                 std::vector<timed_seconds_event> &out,
                                 std::string &err)
{
    out.clear();
    err.clear();
    if (header.ticks_per_quarter == 0) {
        err = "RCP2 tick/秒変換では ticks_per_quarter が 0 です";
        return false;
    }
    if (header.tempo_bpm == 0) {
        err = "RCP2 tick/秒変換では初期 BPM が 0 です";
        return false;
    }

    u64 tempo_us = 60000000ull / header.tempo_bpm;
    u32 previous_tick = 0;
    double seconds = 0.0;
    out.reserve(timeline.size());
    for (const timed_track_event &source : timeline) {
        if (source.event.tick < previous_tick) {
            out.clear();
            err = "RCP2 秒変換の入力イベントが tick 順ではありません";
            return false;
        }
        const u32 delta = source.event.tick - previous_tick;
        seconds += (double(delta) * double(tempo_us)) /
                   (double(header.ticks_per_quarter) * 1000000.0);
        previous_tick = source.event.tick;

        timed_seconds_event converted;
        converted.time = seconds;
        converted.source = source;
        out.push_back(converted);

        if (source.event.kind == timeline_event_kind::tempo) {
            if (source.event.tempo.microseconds_per_quarter == 0) {
                out.clear();
                err = "RCP2 秒変換で 0 microseconds_per_quarter のテンポを検出しました";
                return false;
            }
            tempo_us = source.event.tempo.microseconds_per_quarter;
        }
    }
    return true;
}


bool map_track_midi_outputs(const intermediate_document &document,
                            std::vector<track_midi_mapping> &out, std::string &err)
{
    out.clear();
    err.clear();
    out.reserve(document.tracks.size());
    for (const intermediate_track &track : document.tracks) {
        const u8 raw = track.header.midi_channel;
        track_midi_mapping mapping;
        if (raw & 0x80) {
            // The reference converter treats this as a dummy channel and omits
            // its MIDI events instead of routing them to channel zero.
            mapping.enabled = false;
        } else {
            mapping.channel = raw & 0x0f;
            mapping.port = raw >> 4;
        }
        out.push_back(mapping);
    }
    return true;
}

bool encode_timeline_to_midi_events(
    const std::vector<timed_seconds_event> &timeline,
    const std::vector<track_midi_mapping> &mapping,
    std::vector<midi_output_event> &out, std::string &err)
{
    out.clear();
    err.clear();
    double previous_time = 0.0;
    out.reserve(timeline.size());
    std::vector<track_midi_mapping> active_mapping = mapping;
    for (const timed_seconds_event &source : timeline) {
        if (!(source.time >= 0.0) || source.time < previous_time) {
            out.clear(); err = "RCP2 MIDI 出力イベントの時刻が負値または逆順です"; return false;
        }
        previous_time = source.time;
        const auto kind = source.source.event.kind;
        if (kind == timeline_event_kind::tempo || kind == timeline_event_kind::key_signature)
            continue;
        const size_t track = source.source.track_index;
        if (track >= active_mapping.size()) {
            out.clear(); err = "RCP2 MIDI 出力のトラック割り当てが不足しています"; return false;
        }
        if (kind == timeline_event_kind::route_change) {
            active_mapping[track].channel = source.source.event.route.channel;
            active_mapping[track].port = source.source.event.route.port;
            active_mapping[track].enabled = source.source.event.route.enabled;
            continue;
        }
        const bool fixed_note_route =
            source.source.event.kind == timeline_event_kind::note_off &&
            source.source.event.route_fixed;
        const track_midi_mapping route = fixed_note_route
            ? track_midi_mapping{source.source.event.route.channel,
                                 source.source.event.route.port,
                                 source.source.event.route.enabled}
            : active_mapping[track];
        if (!route.enabled)
            continue;
        std::vector<u8> bytes;
        if (kind == timeline_event_kind::sysex) {
            bytes = source.source.event.sysex;
            if (bytes.size() < 2 || bytes.front() != 0xf0 || bytes.back() != 0xf7) {
                out.clear(); err = "RCP SysEx イベントの開始/終端が不正です"; return false;
            }
            out.push_back({source.time, std::move(bytes), route.port});
            continue;
        }
        if (!encode_midi_message(source.source.event, fixed_note_route ? route.channel : active_mapping[track].channel, bytes, err)) {
            out.clear();
            err = "RCP2 トラック " + std::to_string(track + 1) + " の MIDI 符号化失敗: " + err;
            return false;
        }
        out.push_back({source.time, std::move(bytes), route.port});
    }
    return true;
}

bool encode_timeline_to_midi_events(
    const std::vector<timed_seconds_event> &timeline,
    const std::vector<u8> &channel_by_track, const std::vector<u8> &port_by_track,
    std::vector<midi_output_event> &out, std::string &err)
{
    out.clear(); err.clear();
    double previous_time = 0.0;
    out.reserve(timeline.size());
    for (const timed_seconds_event &source : timeline) {
        if (!(source.time >= 0.0) || source.time < previous_time) {
            out.clear(); err = "RCP2 MIDI 出力イベントの時刻が負値または逆順です"; return false;
        }
        previous_time = source.time;
        const auto kind = source.source.event.kind;
        if (kind == timeline_event_kind::tempo || kind == timeline_event_kind::key_signature)
            continue;
        if (kind == timeline_event_kind::route_change) {
            out.clear(); err = "E6 ルート変更には track_midi_mapping 版の MIDI 出力 API が必要です"; return false;
        }
        const size_t track = source.source.track_index;
        if (track >= channel_by_track.size() || track >= port_by_track.size()) {
            out.clear(); err = "RCP2 MIDI 出力のトラック割り当てが不足しています"; return false;
        }
        std::vector<u8> bytes;
        if (kind == timeline_event_kind::sysex) {
            bytes = source.source.event.sysex;
            if (bytes.size() < 2 || bytes.front() != 0xf0 || bytes.back() != 0xf7) {
                out.clear(); err = "RCP SysEx イベントの開始/終端が不正です"; return false;
            }
            out.push_back({source.time, std::move(bytes), port_by_track[track]});
            continue;
        }
        if (!encode_midi_message(source.source.event, channel_by_track[track], bytes, err)) {
            out.clear();
            err = "RCP2 トラック " + std::to_string(track + 1) + " の MIDI 符号化失敗: " + err;
            return false;
        }
        out.push_back({source.time, std::move(bytes), port_by_track[track]});
    }
    return true;
}


} // namespace rcp
