// license:BSD-3-Clause

// Regression checks that must affect the process exit status.
static bool rcp_mute_flags_test_ok = true;
static bool rcp_player_api_test_ok = true;
static bool rcp_loop_budget_test_ok = true;
static bool rcp_e6_note_off_route_test_ok = true;
//
// 移植が成立しているかの最小確認
#include "mame/sound/swp30.h"
#include "compat/a64asm.h"
#include "ui/bend_thinner.h"
#include "ui/player.h"
#include "smf.h"
#include "rcp.h"
#include "ui/font_check.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

// 再生のピッチベンドの間引き（ui/bend_thinner.h）。1ms おきに 1000 個のベンドの途中に音符を挟み、
// 送った数、音符の直前に送ったベンドがその時点の最新の値か、最後の値が届くかを見る
static void check_bend_thinner()
{
	ui::bend_thinner th;
	struct sent { int to; u8 b[3]; };
	std::vector<sent> out;
	auto send = [&out](int to, const u8 *d, size_t n) {
		sent s{ to, { 0, 0, 0 } };
		for (size_t i = 0; i < n && i < 3; i++)
			s.b[i] = d[i];
		out.push_back(s);
	};
	int bends_sent = 0;
	bool note_ok = false;
	u8 latest = 0;
	for (int i = 0; i < 1000; i++) {
		const double t = i * 0.001;
		const u8 msb = u8(i & 0x7f);
		const u8 bend[3] = { 0xe0, 0x00, msb };
		th.event(0, bend, 3, t, send);
		latest = msb;
		th.tick(t, false, send);
		if (i == 500) {
			const u8 on[3] = { 0x90, 60, 100 };
			th.event(0, on, 3, t + 0.0001, send);
			// 音符の直前に出たものが、今の最新のベンド
			note_ok = out.size() >= 2 && out[out.size() - 2].b[0] == 0xe0 && out[out.size() - 2].b[2] == latest &&
			          out.back().b[0] == 0x90;
		}
	}
	th.tick(1.0, true, send);
	for (const sent &s : out)
		if (s.b[0] == 0xe0)
			bends_sent++;
	const bool last_ok = !out.empty() && out.back().b[0] == 0xe0 && out.back().b[2] == latest;
	std::printf("ピッチベンドの間引き: 1000 個 → %d 個、音符の前の高さ %s、最後の値 %s\n",
	            bends_sent, note_ok ? "合" : "NG", last_ok ? "合" : "NG");
}

// MIDI プレイヤーの土台（smf.h、イシュー #124）。小さな SMF を作って、曲名・小節と拍・頭出しの追いかけを見る。
// 3/4 拍子・テンポ 120 で始まり、2 小節目の頭（1440 tick = 1.5 秒）でテンポ 60 になる曲
static void check_smf_player()
{
	auto chunk = [](std::vector<u8> &f, const char *id, const std::vector<u8> &d) {
		f.insert(f.end(), id, id + 4);
		const u32 n = u32(d.size());
		const u8 len[4] = { u8(n >> 24), u8(n >> 16), u8(n >> 8), u8(n) };
		f.insert(f.end(), len, len + 4);
		f.insert(f.end(), d.begin(), d.end());
	};
	auto put = [](std::vector<u8> &t, u32 delta, std::initializer_list<u8> b) {
		u8 v[4];
		int n = 0;
		do { v[n++] = u8(delta & 0x7f); delta >>= 7; } while (delta);
		while (n--) t.push_back(u8(v[n] | (n ? 0x80 : 0)));
		t.insert(t.end(), b);
	};
	std::vector<u8> f, t0, t1;
	chunk(f, "MThd", { 0, 1, 0, 2, 0x01, 0xe0 });                      // format 1、2 トラック、480 tick/拍
	put(t0, 0, { 0xff, 0x03, 9, 'T', 'e', 's', 't', ' ', 'S', 'o', 'n', 'g' });
	put(t0, 0, { 0xff, 0x58, 4, 3, 2, 24, 8 });                         // 3/4
	put(t0, 0, { 0xff, 0x51, 3, 0x07, 0xa1, 0x20 });                    // 120
	put(t0, 1440, { 0xff, 0x51, 3, 0x0f, 0x42, 0x40 });                 // 60
	put(t0, 0, { 0xff, 0x2f, 0 });
	put(t1, 0, { 0xf0, 8, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 });   // XG オン
	put(t1, 0, { 0xb0, 0, 0 });
	put(t1, 0, { 0xb0, 32, 3 });
	put(t1, 0, { 0xc0, 5 });
	put(t1, 0, { 0xb0, 7, 100 });
	put(t1, 0, { 0x90, 60, 100 });
	put(t1, 480, { 0x80, 60, 0 });
	put(t1, 0, { 0xb0, 7, 80 });
	put(t1, 0, { 0xe0, 0, 0x50 });
	put(t1, 960, { 0xc0, 10 });
	put(t1, 1440, { 0x90, 64, 100 });
	put(t1, 480, { 0x80, 64, 0 });
	put(t1, 0, { 0xff, 0x2f, 0 });
	chunk(f, "MTrk", t0);
	chunk(f, "MTrk", t1);

	std::vector<smf::event> ev;
	std::string err;
	smf::song_meta meta;
	if (!smf::load_from_memory(f.data(), f.size(), ev, err, &meta)) {
		std::printf("プレイヤーの土台: SMF が読めない（%s）\n", err.c_str());
		return;
	}
	std::printf("曲名: 「%s」\n", meta.title.c_str());
	const double at[] = { 0.1, 0.6, 1.6, 3.0, 4.6 };
	for (double sec : at) {
		int bar = 0, beat = 0;
		double bpm = 0;
		meta.bar_beat(sec, bar, beat, bpm);
		std::printf("  %.1f 秒: 小節 %d・拍 %d・テンポ %.0f\n", sec, bar, beat, bpm);
	}
	// 2.0 秒へ頭出し。音符は送らない、SysEx と音色は順に全部、ボリュームとベンドは最後の値だけ
	const std::vector<smf::event> c = smf::chase(ev, 2.0);
	int notes = 0, sysex = 0, pcs = 0, vols = 0, bends = 0, last_pc = -1, vol = -1, bank_at = -1, pc_at = -1;
	for (size_t i = 0; i < c.size(); i++) {
		const std::vector<u8> &b = c[i].bytes;
		if (b.empty())
			continue;
		const u8 st = u8(b[0] & 0xf0);
		if (b[0] == 0xf0)
			sysex++;
		else if (st == 0x90 || st == 0x80)
			notes++;
		else if (st == 0xc0) {
			pcs++;
			last_pc = b[1];
			if (pc_at < 0)
				pc_at = int(i);
		} else if (st == 0xb0 && b[1] == 7) {
			vols++;
			vol = b[2];
		} else if (st == 0xb0 && b[1] == 0 && bank_at < 0)
			bank_at = int(i);
		else if (st == 0xe0)
			bends++;
	}
	std::printf("頭出しの追いかけ（2.0 秒へ）: 音符 %d・SysEx %d・音色 %d（最後 %d）・ボリューム %d 個（値 %d）・ベンド %d・バンクが音色より先 %s\n",
	            notes, sysex, pcs, last_pc, vols, vol, bends, bank_at >= 0 && bank_at < pc_at ? "合" : "NG");
	std::printf("頭（0 秒）へ: %zu 個\n", smf::chase(ev, 0.0).size());
}

// 文字ファイルを読めるかの確かめ（ui/font_check.h、issue #135）。表の並びだけの小さな書体を作って試す:
// glyf の書体・CFF の書体は通り、CFF2 だけの書体（可変フォント）・cmap に Unicode の表が無い書体・
// 束（TTC）の無い番号・ごみは通らない
static void check_font_parses()
{
	auto be16 = [](std::vector<u8> &v, u32 x) { v.push_back(u8(x >> 8)); v.push_back(u8(x)); };
	auto be32 = [](std::vector<u8> &v, u32 x) { v.push_back(u8(x >> 24)); v.push_back(u8(x >> 16)); v.push_back(u8(x >> 8)); v.push_back(u8(x)); };
	// 表の名前の並びと、cmap の（platform, encoding）から書体を作る
	auto make = [&](const char *magic, std::initializer_list<const char *> names, u32 platform, u32 encoding) {
		std::vector<u8> f(magic, magic + 4);
		be16(f, u32(names.size()));
		be16(f, 0); be16(f, 0); be16(f, 0);
		const u32 data = 12 + u32(names.size()) * 16;
		u32 at = data;
		for (const char *nm : names) {
			f.insert(f.end(), nm, nm + 4);
			be32(f, 0);
			be32(f, at);
			be32(f, 16);
			at += 16;
		}
		for (const char *nm : names) {
			std::vector<u8> t(16, 0);
			if (!std::strncmp(nm, "cmap", 4)) {
				t = { 0, 0, 0, 1, u8(platform >> 8), u8(platform), u8(encoding >> 8), u8(encoding), 0, 0, 0, 12, 0, 0, 0, 0 };
			}
			f.insert(f.end(), t.begin(), t.end());
		}
		return f;
	};
	const char ttf[4] = { 0, 1, 0, 0 };
	const std::vector<u8> glyf = make(ttf, { "cmap", "glyf", "head", "hhea", "hmtx", "loca" }, 3, 1);
	const std::vector<u8> cff = make("OTTO", { "CFF ", "cmap", "head", "hhea", "hmtx" }, 3, 10);
	const std::vector<u8> cff2 = make("OTTO", { "CFF2", "cmap", "head", "hhea", "hmtx" }, 3, 1);
	const std::vector<u8> symbol = make(ttf, { "cmap", "glyf", "head", "hhea", "hmtx", "loca" }, 3, 0);
	const std::vector<u8> noloca = make(ttf, { "cmap", "glyf", "head", "hhea", "hmtx" }, 0, 3);
	// 束: 2 つ目に glyf の書体を入れる（1 つ目は CFF2）。番地は束の頭から
	std::vector<u8> ttc = { 't', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 2 };
	be32(ttc, 20);
	be32(ttc, 20 + u32(cff2.size()));
	auto append = [&](const std::vector<u8> &font) {
		const u32 base = u32(ttc.size());
		std::vector<u8> g = font;
		const u32 tables = u32(g[4]) << 8 | g[5];
		for (u32 i = 0; i < tables; i++) {
			const u32 e = 12 + i * 16 + 8;
			const u32 off = (u32(g[e]) << 24 | u32(g[e + 1]) << 16 | u32(g[e + 2]) << 8 | g[e + 3]) + base;
			g[e] = u8(off >> 24); g[e + 1] = u8(off >> 16); g[e + 2] = u8(off >> 8); g[e + 3] = u8(off);
		}
		ttc.insert(ttc.end(), g.begin(), g.end());
	};
	append(cff2);
	append(glyf);
	const std::vector<u8> junk(64, 0x55);
	std::printf("文字ファイルを読めるか: glyf %d・CFF %d・CFF2 だけ %d・Unicode の表なし %d・loca なし %d・束の 1 つ目 %d・2 つ目 %d・無い番号 %d・ごみ %d・途中で切れた %d\n",
	            ui::font_parses(glyf.data(), glyf.size()), ui::font_parses(cff.data(), cff.size()), ui::font_parses(cff2.data(), cff2.size()),
	            ui::font_parses(symbol.data(), symbol.size()), ui::font_parses(noloca.data(), noloca.size()),
	            ui::font_parses(ttc.data(), ttc.size(), 0), ui::font_parses(ttc.data(), ttc.size(), 1), ui::font_parses(ttc.data(), ttc.size(), 2),
	            ui::font_parses(junk.data(), junk.size()), ui::font_parses(glyf.data(), 40));
}


// RCP2 の固定ヘッダーを合成バイト列で検証する。曲データは含めない。
static void check_rcp_header()
{
	const char signature[] = "RCM-PC98V2.0(C)COME ON MUSIC\r\n";
	std::vector<u8> data(0x586 + 0x2c, 0);
	std::memcpy(data.data(), signature, sizeof(signature) - 1);
	data[30] = 0;
	data[31] = 0;
	std::memcpy(data.data() + 0x020, "Synthetic test", 14);
	std::memcpy(data.data() + 0x060, "Generated header", 16);
	std::memcpy(data.data() + 0x406, "GS test", 7);
	std::fill(data.begin() + 0x406 + 0x18, data.begin() + 0x406 + 0x30, 0xf7);
	const u8 sysex_template[] = {0x41, 0x80, 0x81, 0xf7};
	std::memcpy(data.data() + 0x406 + 0x18, sysex_template, sizeof(sysex_template));
	data[0x1c0] = 96;  // low byte of ticks per quarter
	data[0x1e7] = 1;   // high byte -> 352 ticks per quarter
	data[0x1c1] = 120;
	data[0x1c2] = 4;
	data[0x1c3] = 4;
	data[0x1c4] = 3;
	data[0x1c5] = 0;
	data[0x1e6] = 0x24;

	rcp::header_info info;
	std::string err;
	const bool identify_ok = rcp::inspect_header(data.data(), data.size(), info, err) &&
	                         info.type == rcp::format::rcp2;
	rcp::document_header header;
	const bool parse_ok = rcp::parse_header(data.data(), data.size(), header, err);
	const bool parsed_fields_ok = parse_ok && header.title == "Synthetic test" &&
	                             header.comment == "Generated header" &&
	                             header.ticks_per_quarter == 352 && header.tempo_bpm == 120 &&
	                             header.beat_numerator == 4 && header.beat_denominator == 4 &&
	                             header.track_count == 0x24 &&
	                             header.user_sysex[0].name == "GS test" &&
	                             header.user_sysex[0].data == std::vector<u8>({0x41, 0x80, 0x81});
	data[0x1e6] = 0; // Legacy RCP2 header: zero means the standard 18 tracks.
	rcp::document_header legacy_header;
	const bool legacy_zero_count_ok =
	    rcp::parse_header(data.data(), data.size(), legacy_header, err) &&
	    legacy_header.track_count == 18;
	std::vector<u8> legacy_data(0x586 + 18 * 0x30, 0);
	std::memcpy(legacy_data.data(), signature, sizeof(signature) - 1);
	legacy_data[30] = 0;
	legacy_data[31] = 0;
	for (size_t i = 0; i < 18; ++i) {
		const size_t offset = 0x586 + i * 0x30;
		legacy_data[offset] = 0x30;
		legacy_data[offset + 1] = 0;
		legacy_data[offset + 2] = static_cast<u8>(i + 1);
		legacy_data[offset + 44] = 0xfe;
	}
	std::vector<rcp::track> legacy_tracks;
	const bool legacy_tracks_ok =
	    rcp::parse_tracks(legacy_data.data(), legacy_data.size(), legacy_tracks, err) &&
	    legacy_tracks.size() == 18 &&
	    std::all_of(legacy_tracks.begin(), legacy_tracks.end(),
	                [](const rcp::track &track) {
		                return track.sequence.size() == 1 &&
		                       track.sequence[0].code == 0xfe;
	                });
	data[0x1e6] = 0x24;
	const bool fields_ok = parsed_fields_ok && legacy_zero_count_ok && legacy_tracks_ok;
	const bool short_prefix_ok = !rcp::inspect_header(data.data(), 0x1f, info, err);
	const bool short_header_ok = !rcp::parse_header(data.data(), 0x585, header, err);
	std::vector<u8> junk(0x586, 0x55);
	const bool junk_ok = !rcp::parse_header(junk.data(), junk.size(), header, err);
	// トラックヘッダーはファイル末尾に合成し、イベントデータは作らない。
	const size_t track = 0x586;
	data[track + 0] = 0x2c;
	data[track + 1] = 0;
	data[track + 2] = 1;
	data[track + 3] = 0;
	data[track + 4] = 3;
	data[track + 5] = 0;
	data[track + 6] = 0xff;
	data[track + 7] = 1;
	std::memcpy(data.data() + track + 8, "Track one", 9);
	rcp::track_header th;
	const bool track_ok = rcp::parse_track_header(data.data() + track, 0x2c, th, err) &&
	                      th.encoded_length == 0x2c && th.track_id == 1 &&
	                      th.midi_channel == 3 && th.tick_offset == 0xff &&
	                      th.mute == 1 && th.name == "Track one";
	// The track header stores the mute byte verbatim; test common flag values.
	const u8 mute_flags[] = { 0x00, 0x01, 0x02, 0x04 };
	bool mute_flags_ok = true;
	for (u8 flag : mute_flags) {
		data[track + 7] = flag;
		rcp::track_header mute_header;
		const bool case_ok =
		    rcp::parse_track_header(data.data() + track, 0x2c, mute_header, err) &&
		    mute_header.mute == flag;
		mute_flags_ok = mute_flags_ok && case_ok;
		std::printf("RCP2 mute flag 0x%02x: %s (parsed=0x%02x)\n",
		            unsigned(flag), case_ok ? "OK" : "FAIL",
		            unsigned(mute_header.mute));
	}
	std::printf("RCP2 mute flag preservation: %s\n", mute_flags_ok ? "OK" : "FAIL");
	rcp_mute_flags_test_ok = mute_flags_ok;
	data[track + 7] = 1;
	const bool short_track_ok = !rcp::parse_track_header(data.data() + track, 0x2b, th, err);
	const bool null_track_ok = !rcp::parse_track_header(nullptr, 0x2c, th, err);
	const bool null_ok = !rcp::parse_header(nullptr, data.size(), header, err);

	std::printf("RCP2 固定ヘッダー: 識別 %s・項目 %s・短い入力 %s/%s・不一致 %s・null %s・トラックヘッダー %s・ミュートフラグ %s・短いトラック %s・null トラック %s\n",
	            identify_ok ? "合" : "NG", fields_ok ? "合" : "NG",
	            short_prefix_ok ? "合" : "NG", short_header_ok ? "合" : "NG",
	            junk_ok ? "合" : "NG", null_ok ? "合" : "NG", track_ok ? "合" : "NG",
	            mute_flags_ok ? "合" : "NG", short_track_ok ? "合" : "NG", null_track_ok ? "合" : "NG");
}

// RCP2 の 4 byte シーケンスレコードを合成データで検証する。
// ここではコマンド展開やタイミング解釈をせず、生レコードと終端だけを確認する。
static void check_rcp_sequence()
{
	const std::vector<u8> sequence = {
		60, 12, 100, 90,       // note-like record
		0xfd, 0, 0, 0,         // measure end command
		0xfe, 0, 0, 0,         // track end command
		0x90, 1, 2, 3           // data after FE must not be consumed
	};
	std::vector<rcp::sequence_event> events;
	size_t consumed = 0;
	std::string err;
	const bool ok = rcp::parse_sequence(sequence.data(), sequence.size(), events, consumed, err);
	const bool fields_ok = ok && events.size() == 3 && consumed == 12 &&
		events[0].code == 60 && events[0].p0 == 12 &&
		events[0].p1 == 100 && events[0].p2 == 90 &&
		events[1].code == 0xfd && events[2].code == 0xfe;

	const u8 no_end[] = { 60, 0, 90, 80 };
	const bool missing_end_ok = !rcp::parse_sequence(no_end, sizeof(no_end), events, consumed, err) &&
		events.empty() && consumed == 0;
	const u8 partial[] = { 60, 0, 90 };
	const bool partial_ok = !rcp::parse_sequence(partial, sizeof(partial), events, consumed, err) &&
		events.empty() && consumed == 0;

	// FE の後ろに不完全な断片があっても、終端までで解析を止める。
	const u8 end_with_partial_tail[] = { 0xfe, 0, 0, 0, 0x90, 1 };
	const bool partial_tail_ignored =
		rcp::parse_sequence(end_with_partial_tail, sizeof(end_with_partial_tail),
			events, consumed, err) &&
		events.size() == 1 && events[0].code == 0xfe && consumed == 4 && err.empty();

	const bool null_ok = !rcp::parse_sequence(nullptr, 4, events, consumed, err) &&
		events.empty() && consumed == 0;

	// F6 の継続データ候補 F7 は意味解釈せず、順序と raw field を保持する。
	const u8 multi_record_text[] = {
		0xf6, 0, 'A', 'B',
		0xf7, 0, 'C', 'D',
		0xf7, 0, 'E', 'F',
		0xfe, 0, 0, 0
	};
	const bool continuation_preserved =
		rcp::parse_sequence(multi_record_text, sizeof(multi_record_text),
			events, consumed, err) &&
		events.size() == 4 && consumed == sizeof(multi_record_text) &&
		events[0].code == 0xf6 && events[0].p1 == 'A' && events[0].p2 == 'B' &&
		events[1].code == 0xf7 && events[1].p1 == 'C' && events[1].p2 == 'D' &&
		events[2].code == 0xf7 && events[2].p1 == 'E' && events[2].p2 == 'F' &&
		events[3].code == 0xfe;

	// 生レコード解析は F6/F7 の意味を消費せず、途中の別レコードも順序どおり保持する。
	const u8 interrupted_continuation[] = {
		0xf6, 0, 'A', 'B',
		0xf7, 0, 'C', 'D',
		60, 1, 2, 3,
		0xf7, 0, 'E', 'F',
		0xfe, 0, 0, 0
	};
	const bool interruption_preserved =
		rcp::parse_sequence(interrupted_continuation, sizeof(interrupted_continuation),
			events, consumed, err) &&
		events.size() == 5 && consumed == sizeof(interrupted_continuation) &&
		events[0].code == 0xf6 && events[1].code == 0xf7 &&
		events[2].code == 60 && events[3].code == 0xf7 &&
		events[4].code == 0xfe;

	// 実験的な意味解釈: F6 と直後に連なる F7 の p1/p2 だけを集め、
	// 最初の別レコードや FE は消費しない。
	auto event = [](u8 code, u8 p0, u8 p1, u8 p2) {
		rcp::sequence_event e;
		e.code = code; e.p0 = p0; e.p1 = p1; e.p2 = p2;
		return e;
	};
	std::vector<u8> comment;
	size_t comment_records = 0;
	std::vector<rcp::sequence_event> comment_sequence = {
		event(0xf6, 99, 'A', 'B'), event(0xf7, 88, 'C', 'D'),
		event(0xf7, 77, 'E', 'F'), event(0x3c, 1, 2, 3),
		event(0xf7, 0, 'X', 'Y'), event(0xfe, 0, 0, 0)
	};
	const bool comment_chain_ok =
		rcp::extract_comment_data(comment_sequence, 0, comment, comment_records, err) &&
		comment == std::vector<u8>({'A', 'B', 'C', 'D', 'E', 'F'}) &&
		comment_records == 3 && err.empty();
	const bool comment_stop_ok =
		rcp::extract_comment_data(comment_sequence, 0, comment, comment_records, err) &&
		comment_records == 3 && comment.size() == 6;
	std::vector<rcp::sequence_event> comment_at_end = {
		event(0xf6, 0, 'G', 'H'), event(0xfe, 0, 0, 0),
		event(0xf7, 0, 'I', 'J')
	};
	const bool comment_fe_boundary_ok =
		rcp::extract_comment_data(comment_at_end, 0, comment, comment_records, err) &&
		comment == std::vector<u8>({'G', 'H'}) && comment_records == 1;
	comment = {'Z'};
	comment_records = 9;
	const bool comment_bad_start_ok =
		!rcp::extract_comment_data(comment_sequence, 3, comment, comment_records, err) &&
		comment.empty() && comment_records == 0 && !err.empty();
	comment = {'Z'};
	comment_records = 9;
	const bool comment_missing_end_ok =
		!rcp::extract_comment_data(
			std::vector<rcp::sequence_event>{event(0xf6, 0, 'A', 'B'),
				event(0xf7, 0, 'C', 'D')},
			0, comment, comment_records, err) &&
		comment.empty() && comment_records == 0 && !err.empty();

	// Referenced converter trims only trailing ASCII spaces, not NUL or leading spaces.
	std::vector<u8> trim_comment = {' ', 'A', ' ', 'B', ' ', ' '};
	const bool comment_trim_ok = rcp::trim_comment_trailing_spaces(trim_comment) == 4 &&
		trim_comment == std::vector<u8>({' ', 'A', ' ', 'B'});
	std::vector<u8> all_spaces = {' ', ' ', ' '};
	const bool comment_all_spaces_ok = rcp::trim_comment_trailing_spaces(all_spaces) == 0 &&
		all_spaces.empty();
	std::vector<u8> embedded_nul = {'A', 0, ' ', ' '};
	const bool comment_nul_ok = rcp::trim_comment_trailing_spaces(embedded_nul) == 2 &&
		embedded_nul == std::vector<u8>({'A', 0});
	std::vector<u8> no_trim = {'A', 0, 'B'};
	const bool comment_no_trim_ok = rcp::trim_comment_trailing_spaces(no_trim) == 3 &&
		no_trim == std::vector<u8>({'A', 0, 'B'});

	std::printf("RCP2 シーケンスレコード: 内容/FE 終端 %s・終端なし拒否 %s・途中切れ拒否 %s・FE 後の断片無視 %s・null 拒否 %s・F6/F7 raw 保持 %s・別レコード順序保持 %s・F6 コメント抽出 %s/%s/%s/%s\n",
		fields_ok ? "合" : "NG", missing_end_ok ? "合" : "NG",
		partial_ok ? "合" : "NG", partial_tail_ignored ? "合" : "NG", null_ok ? "合" : "NG",
		continuation_preserved ? "合" : "NG", interruption_preserved ? "合" : "NG",
		comment_chain_ok ? "合" : "NG", comment_stop_ok ? "合" : "NG",
		comment_fe_boundary_ok ? "合" : "NG",
		comment_bad_start_ok && comment_missing_end_ok ? "合" : "NG");
	std::printf("RCP2 コメント末尾空白: 先頭/途中保持 %s・全空白 %s・NUL 非終端扱い %s・空白なし %s\n",
		comment_trim_ok ? "合" : "NG", comment_all_spaces_ok ? "合" : "NG",
		comment_nul_ok ? "合" : "NG", comment_no_trim_ok ? "合" : "NG");

	// F5 の p0 はキー署名データとして解釈し、時間進行は別フィールドで 0 とする。
	rcp::sequence_event f5;
	f5.code = 0xf5; f5.p0 = 0x03; f5.p1 = 44; f5.p2 = 55;
	rcp::key_signature_event key;
	const bool f5_sharps_ok = rcp::decode_key_signature_event(f5, key, err) &&
		key.accidentals == 3 && !key.minor && key.delay_advance == 0 && err.empty();
	f5.p0 = 0x0a; // two flats
	const bool f5_flats_ok = rcp::decode_key_signature_event(f5, key, err) &&
		key.accidentals == -2 && !key.minor && key.delay_advance == 0;
	f5.p0 = 0x17; // seven sharps, minor
	const bool f5_minor_ok = rcp::decode_key_signature_event(f5, key, err) &&
		key.accidentals == 7 && key.minor && key.delay_advance == 0;
	f5.p0 = 0x1f; // seven flats, minor
	const bool f5_flats_minor_ok = rcp::decode_key_signature_event(f5, key, err) &&
		key.accidentals == -7 && key.minor && key.delay_advance == 0;
	f5.code = 0xf6;
	const bool f5_wrong_command_ok = !rcp::decode_key_signature_event(f5, key, err) &&
		key.accidentals == 0 && !key.minor && key.delay_advance == 0 && !err.empty();
	std::printf("RCP2 F5 キー署名: シャープ %s・フラット %s・短調 %s・フラット短調 %s・コマンド境界拒否 %s\n",
		f5_sharps_ok ? "合" : "NG", f5_flats_ok ? "合" : "NG",
		f5_minor_ok ? "合" : "NG", f5_flats_minor_ok ? "合" : "NG",
		f5_wrong_command_ok ? "合" : "NG");

    // F5 は直前までの tick に置き、p0 を加算しない。
    std::vector<rcp::sequence_event> mixed = {
        { 60, 5, 90, 100 },
        { 0xf5, 0x10, 0, 0 },
        { 62, 7, 80, 95 },
        { 0xfe, 0, 0, 0 }
    };
    std::vector<rcp::timed_event> mixed_timeline;
    const bool mixed_ok = rcp::expand_note_key_signature_timeline(mixed, mixed_timeline, err) &&
        mixed_timeline.size() == 3 &&
        mixed_timeline[0].kind == rcp::timeline_event_kind::note &&
        mixed_timeline[0].tick == 0 && mixed_timeline[0].note.note == 60 &&
        mixed_timeline[1].kind == rcp::timeline_event_kind::key_signature &&
        mixed_timeline[1].tick == 5 && mixed_timeline[1].key.minor &&
        mixed_timeline[1].key.delay_advance == 0 &&
        mixed_timeline[2].kind == rcp::timeline_event_kind::note &&
        mixed_timeline[2].tick == 5 && mixed_timeline[2].note.note == 62;
    mixed[1].code = 0xf6;
    const bool mixed_reject_ok = !rcp::expand_note_key_signature_timeline(mixed, mixed_timeline, err) &&
        mixed_timeline.empty() && !err.empty();
    std::printf("RCP2 F5 時間展開: tick 維持 %s・未対応コマンド拒否 %s\n",
        mixed_ok ? "合" : "NG", mixed_reject_ok ? "合" : "NG");
}


// RCP2 のノートレコードを型付きフィールドへ写し、コマンドを誤認しないことを確認する。
static void check_rcp_note_decode()
{
    rcp::sequence_event raw;
    raw.code = 60;
    raw.p0 = 12;
    raw.p1 = 100;
    raw.p2 = 90;
    rcp::note_event note;
    std::string err = "stale error";
    const bool note_ok = rcp::decode_note_event(raw, note, err) &&
        note.note == 60 && note.delay == 12 &&
        note.duration == 100 && note.velocity == 90 && err.empty();

    raw.code = 0x7f;
    raw.p0 = 0xff;
    raw.p1 = 0x80;
    raw.p2 = 0;
    const bool upper_note_ok = rcp::decode_note_event(raw, note, err) &&
        note.note == 0x7f && note.delay == 0xff &&
        note.duration == 0x80 && note.velocity == 0 && err.empty();

    raw.code = 0x80;
    note = {};
    const bool lower_command_rejected = !rcp::decode_note_event(raw, note, err) &&
        note.note == 0 && note.delay == 0 &&
        note.duration == 0 && note.velocity == 0 && !err.empty();

    raw.code = 0xff;
    note = {};
    const bool upper_command_rejected = !rcp::decode_note_event(raw, note, err) &&
        note.note == 0 && note.delay == 0 &&
        note.duration == 0 && note.velocity == 0 && !err.empty();

    // A zero duration is preserved as raw data; semantic handling belongs downstream.
    raw.code = 62;
    raw.p0 = 0;
    raw.p1 = 0;
    raw.p2 = 127;
    const bool zero_duration_ok = rcp::decode_note_event(raw, note, err) &&
        note.note == 62 && note.delay == 0 &&
        note.duration == 0 && note.velocity == 127 && err.empty();

    // A successful decode after a command error must clear the old error.
    raw.code = 61;
    raw.p0 = 1;
    raw.p1 = 2;
    raw.p2 = 3;
    const bool recovery_ok = rcp::decode_note_event(raw, note, err) &&
        note.note == 61 && note.delay == 1 &&
        note.duration == 2 && note.velocity == 3 && err.empty();

    std::printf("RCP2 ノート解釈: フィールド分離 %s・上端ノート %s・コマンド境界拒否 %s・duration 0 保持 %s・エラー解除 %s\n",
        note_ok ? "合" : "NG", upper_note_ok ? "合" : "NG",
        lower_command_rejected && upper_command_rejected ? "合" : "NG",
        zero_duration_ok ? "合" : "NG", recovery_ok ? "合" : "NG");
}


static void check_rcp_note_timeline()
{
    auto event = [](u8 code, u8 p0, u8 p1, u8 p2) {
        rcp::sequence_event raw;
        raw.code = code; raw.p0 = p0; raw.p1 = p1; raw.p2 = p2;
        return raw;
    };
    std::vector<rcp::sequence_event> sequence;
    sequence.push_back(event(60, 12, 100, 90));
    sequence.push_back(event(64, 3, 20, 70));
    sequence.push_back(event(62, 0, 0, 127));
    sequence.push_back(event(0xfe, 0, 0, 0));

    std::vector<rcp::timed_note_event> timeline;
    std::string err;
    const bool timeline_ok = rcp::expand_note_only_timeline(sequence, timeline, err) &&
        timeline.size() == 3 &&
        timeline[0].start_tick == 0 && timeline[0].note.note == 60 &&
        timeline[0].note.duration == 100 && timeline[0].note.velocity == 90 &&
        timeline[1].start_tick == 12 && timeline[1].note.note == 64 &&
        timeline[2].start_tick == 15 && timeline[2].note.note == 62 &&
        timeline[2].note.duration == 0 && err.empty();

    std::vector<rcp::sequence_event> after_end;
    after_end.push_back(event(60, 12, 100, 90));
    after_end.push_back(event(0xfe, 0, 0, 0));
    after_end.push_back(event(0xf6, 7, 0, 0));
    const bool end_stops_scan =
        rcp::expand_note_only_timeline(after_end, timeline, err) &&
        timeline.size() == 1 && timeline[0].note.note == 60 && err.empty();

    std::vector<rcp::sequence_event> with_command;
    with_command.push_back(event(60, 12, 100, 90));
    with_command.push_back(event(0xf6, 7, 0, 0));
    with_command.push_back(event(0xfe, 0, 0, 0));
    const bool command_rejected =
        !rcp::expand_note_only_timeline(with_command, timeline, err) &&
        timeline.empty() && !err.empty();

    std::vector<rcp::sequence_event> no_end;
    no_end.push_back(event(60, 12, 100, 90));
    const bool missing_end_rejected =
        !rcp::expand_note_only_timeline(no_end, timeline, err) &&
        timeline.empty() && !err.empty();

    std::printf("RCP2 ノート時間展開（ノート専用）: tick/順序/duration 0 %s・FE 後を無視 %s・未知コマンド拒否 %s・FE なし拒否 %s\n",
        timeline_ok ? "合" : "NG", end_stops_scan ? "合" : "NG",
        command_rejected ? "合" : "NG", missing_end_rejected ? "合" : "NG");
}

static void check_rcp_track_merge()
{
    auto timed = [](u32 tick, u8 note) {
        rcp::timed_event e;
        e.tick = tick;
        e.kind = rcp::timeline_event_kind::note;
        e.note.note = note;
        return e;
    };
    std::vector<std::vector<rcp::timed_event>> tracks = {
        {timed(0, 60), timed(10, 61), timed(10, 62)},
        {timed(5, 70), timed(10, 71)},
        {}
    };
    std::vector<rcp::timed_track_event> merged;
    std::string err;
    const bool order_ok = rcp::merge_timed_tracks(tracks, merged, err) &&
        merged.size() == 5 &&
        merged[0].track_index == 0 && merged[0].event.tick == 0 && merged[0].event.note.note == 60 &&
        merged[1].track_index == 1 && merged[1].event.tick == 5 && merged[1].event.note.note == 70 &&
        merged[2].track_index == 0 && merged[2].event.tick == 10 && merged[2].event.note.note == 61 &&
        merged[3].track_index == 0 && merged[3].event.tick == 10 && merged[3].event.note.note == 62 &&
        merged[4].track_index == 1 && merged[4].event.tick == 10 && merged[4].event.note.note == 71 &&
        err.empty();
    tracks[1] = {timed(8, 72), timed(7, 73)};
    const bool unsorted_rejected =
        !rcp::merge_timed_tracks(tracks, merged, err) && merged.empty() && !err.empty();
    tracks.clear();
    const bool empty_ok = rcp::merge_timed_tracks(tracks, merged, err) &&
        merged.empty() && err.empty();
    std::printf("RCP2 複数トラック時間統合: tick 順/同 tick 安定順序 %s・逆順拒否 %s・空入力 %s\n",
        order_ok ? "合" : "NG", unsorted_rejected ? "合" : "NG",
        empty_ok ? "合" : "NG");
}


static void check_rcp_document_timeline()
{
    auto raw = [](u8 code, u8 p0, u8 p1, u8 p2) {
        rcp::intermediate_event event;
        event.raw.code = code;
        event.raw.p0 = p0;
        event.raw.p1 = p1;
        event.raw.p2 = p2;
        return event;
    };
    rcp::intermediate_document doc;
    doc.header.tempo_bpm = 120;
    doc.tracks.resize(2);
    doc.tracks[0].events = {raw(60, 5, 8, 90), raw(0xe7, 2, 32, 0), raw(0xfe, 0, 0, 0)};
    doc.tracks[1].events = {raw(65, 2, 8, 80), raw(0xf5, 3, 0, 0), raw(0xfe, 0, 0, 0)};
    doc.tracks.resize(3);
    doc.tracks[2].header.tick_offset = 0xfe; // signed -2 ticks
    doc.tracks[2].events = {raw(67, 5, 8, 75), raw(68, 5, 8, 75), raw(0xfe, 0, 0, 0)};

    std::vector<rcp::timed_track_event> merged;
    std::string err;
    const bool order_ok = rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.size() == 10 &&
        merged[0].track_index == 0 && merged[0].event.tick == 0 &&
        merged[0].event.kind == rcp::timeline_event_kind::note &&
        merged[1].track_index == 1 && merged[1].event.tick == 0 &&
        merged[1].event.kind == rcp::timeline_event_kind::note &&
        merged[2].track_index == 2 && merged[2].event.tick == 0 &&
        merged[2].event.kind == rcp::timeline_event_kind::note &&
        merged[3].track_index == 1 && merged[3].event.tick == 2 &&
        merged[3].event.kind == rcp::timeline_event_kind::key_signature &&
        merged[4].track_index == 2 && merged[4].event.tick == 3 &&
        merged[4].event.kind == rcp::timeline_event_kind::note &&
        merged[5].track_index == 0 && merged[5].event.tick == 5 &&
        merged[5].event.kind == rcp::timeline_event_kind::tempo &&
        merged[6].track_index == 2 && merged[6].event.tick == 6 &&
        merged[6].event.kind == rcp::timeline_event_kind::note_off &&
        merged[7].track_index == 0 && merged[7].event.tick == 8 &&
        merged[7].event.kind == rcp::timeline_event_kind::note_off &&
        merged[8].track_index == 1 && merged[8].event.tick == 8 &&
        merged[8].event.kind == rcp::timeline_event_kind::note_off &&
        merged[9].track_index == 2 && merged[9].event.tick == 11 &&
        merged[9].event.kind == rcp::timeline_event_kind::note_off && err.empty();
    const bool offset_ok = merged.size() == 10 &&
        merged[2].track_index == 2 && merged[2].event.tick == 0 &&
        merged[4].track_index == 2 && merged[4].event.tick == 3;

    // EB/EC/EE are emitted at the current tick; their p0 delay advances
    // the cursor after each command, matching the reference converter.
    doc.tracks.resize(1);
    doc.tracks[0].header.tick_offset = 0;
    doc.tracks[0].header.key = 0;
    doc.header.play_bias = 0;
    doc.tracks[0].events = {raw(0xeb, 2, 7, 100), raw(0xec, 3, 10, 0),
                            raw(0xee, 1, 0, 64), raw(0xfe, 0, 0, 0)};
    const bool midi_commands_ok =
        rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.size() == 3 &&
        merged[0].event.kind == rcp::timeline_event_kind::control_change &&
        merged[0].event.tick == 0 && merged[0].event.channel.data1 == 7 &&
        merged[0].event.channel.data2 == 100 &&
        merged[1].event.kind == rcp::timeline_event_kind::program_change &&
        merged[1].event.tick == 2 && merged[1].event.channel.data1 == 10 &&
        merged[2].event.kind == rcp::timeline_event_kind::pitch_bend &&
        merged[2].event.tick == 5 && merged[2].event.channel.data1 == 0 &&
        merged[2].event.channel.data2 == 64 && err.empty();

    doc.tracks[0].events = {raw(0xec, 0, 0x80, 0), raw(0xfe, 0, 0, 0)};
    const bool special_instrument_rejected =
        !rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.empty() && !err.empty();
    doc.tracks[0].events = {raw(0xeb, 0, 0x80, 0), raw(0xfe, 0, 0, 0)};
    const bool invalid_control_value_rejected =
        !rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.empty() && !err.empty();
    std::printf("RCP2 EB/EC/EE MIDI コマンド: CC/Program/Pitch Bend と tick %s・特殊 EC 拒否 %s・7-bit 範囲外拒否 %s\n",
        midi_commands_ok ? "合" : "NG",
        special_instrument_rejected ? "合" : "NG",
        invalid_control_value_rejected ? "合" : "NG");

    doc.tracks.resize(3);
    doc.tracks[0].events[0].raw.code = 0xf6;
    const bool unsupported_rejected =
        !rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.empty() && !err.empty();

    doc.tracks.clear();
    const bool empty_ok = rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.empty() && err.empty();

    // A muted track must not contribute events or make an otherwise playable
    rcp::intermediate_document mute_doc;
    mute_doc.header.tempo_bpm = 120;
    // document fail merely because that track contains an unsupported command.
    mute_doc.tracks.resize(2);
    mute_doc.tracks[0].header.mute = 0x01;
    mute_doc.tracks[0].events = {raw(0xf6, 0, 'X', 'Y'), raw(0xfe, 0, 0, 0)};
    mute_doc.tracks[1].header.mute = 0;
    mute_doc.tracks[1].events = {raw(60, 0, 4, 90), raw(0xfe, 0, 0, 0)};
    const bool mute_skips_track_ok =
        rcp::expand_intermediate_timeline(mute_doc, merged, err) &&
        merged.size() == 2 &&
        merged[0].track_index == 1 &&
        merged[0].event.kind == rcp::timeline_event_kind::note &&
        merged[1].track_index == 1 &&
        merged[1].event.kind == rcp::timeline_event_kind::note_off &&
        err.empty();

    // Exercise both signed 8-bit offset endpoints: -128 clamps tick zero,
    // while +127 shifts an event at local tick zero to tick 127.
    doc.tracks.resize(1);
    doc.tracks[0].events = {raw(60, 0, 8, 90), raw(0xfe, 0, 0, 0)};
    doc.tracks[0].header.tick_offset = 0x80;
    const bool minimum_offset_ok =
        rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.size() == 2 &&
        merged[0].event.kind == rcp::timeline_event_kind::note && merged[0].event.tick == 0 &&
        merged[1].event.kind == rcp::timeline_event_kind::note_off && merged[1].event.tick == 0 && err.empty();
    doc.tracks[0].header.tick_offset = 0x7f;
    const bool maximum_offset_ok =
        rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.size() == 2 &&
        merged[0].event.kind == rcp::timeline_event_kind::note && merged[0].event.tick == 127 &&
        merged[1].event.kind == rcp::timeline_event_kind::note_off && merged[1].event.tick == 135 && err.empty();
    const bool offset_bounds_ok = minimum_offset_ok && maximum_offset_ok;

    // The signed offset applies to all emitted event kinds, not only notes.
    doc.tracks[0].events = {raw(60, 0, 8, 90), raw(0xf5, 0, 0, 0),
                            raw(0xe7, 0, 32, 0), raw(0xfe, 0, 0, 0)};
    doc.tracks[0].header.tick_offset = 3;
    const bool all_kinds_offset_ok =
        rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.size() == 4 &&
        merged[0].event.kind == rcp::timeline_event_kind::note && merged[0].event.tick == 3 &&
        merged[1].event.kind == rcp::timeline_event_kind::key_signature && merged[1].event.tick == 3 &&
        merged[2].event.kind == rcp::timeline_event_kind::tempo && merged[2].event.tick == 3 &&
        merged[3].event.kind == rcp::timeline_event_kind::note_off && merged[3].event.tick == 11 && err.empty();

    doc.tracks[0].events = {raw(60, 0, 8, 90), raw(0xfe, 0, 0, 0)};
    doc.tracks[0].header.tick_offset = 0;
    doc.tracks[0].header.key = 0x7f; // signed 7-bit -1
    doc.header.play_bias = 2;
    const bool timeline_transpose_ok =
        rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.size() == 2 &&
        merged[0].event.kind == rcp::timeline_event_kind::note && merged[0].event.note.note == 61 &&
        merged[1].event.kind == rcp::timeline_event_kind::note_off && merged[1].event.note.note == 61 && err.empty();
    doc.tracks[0].header.key = 0x80; // rhythm-channel flag suppresses both offsets
    doc.header.play_bias = 12;
    const bool timeline_rhythm_transpose_ok =
        rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.size() == 2 &&
        merged[0].event.kind == rcp::timeline_event_kind::note && merged[0].event.note.note == 60 &&
        merged[1].event.kind == rcp::timeline_event_kind::note_off && merged[1].event.note.note == 60 && err.empty();
    std::printf("RCP2 文書移調適用: トラック/文書移調 %s・リズム抑止 %s\n",
        timeline_transpose_ok ? "合" : "NG",
        timeline_rhythm_transpose_ok ? "合" : "NG");

    u8 transposed_note = 0;
    const bool transpose_signed_track_ok =
        rcp::apply_note_transposition(60, 0x7f, 2, transposed_note, err) &&
        transposed_note == 61 && err.empty();
    const bool transpose_signed_document_ok =
        rcp::apply_note_transposition(60, 2, 0xff, transposed_note, err) &&
        transposed_note == 61 && err.empty();
    const bool transpose_rhythm_channel_ok =
        rcp::apply_note_transposition(60, 0x80, 12, transposed_note, err) &&
        transposed_note == 60 && err.empty();
    const bool transpose_wrap_ok =
        rcp::apply_note_transposition(127, 1, 0, transposed_note, err) &&
        transposed_note == 0 && err.empty();
    const bool transpose_negative_wrap_ok =
        rcp::apply_note_transposition(0, 0x7f, 0, transposed_note, err) &&
        transposed_note == 127 && err.empty();
    const bool transpose_invalid_note_rejected =
        !rcp::apply_note_transposition(0x80, 0, 0, transposed_note, err) &&
        transposed_note == 0 && !err.empty();
    std::printf("RCP2 ノート移調（参照実装準拠）: トラック符号付き %s・文書符号付き %s・リズムチャンネル %s・7-bit 折返し %s・範囲外ノート拒否 %s\n",
        transpose_signed_track_ok ? "合" : "NG",
        transpose_signed_document_ok ? "合" : "NG",
        transpose_rhythm_channel_ok ? "合" : "NG",
        (transpose_wrap_ok && transpose_negative_wrap_ok) ? "合" : "NG",
        transpose_invalid_note_rejected ? "合" : "NG");

    u32 adjusted_tick = 99;
    const bool offset_helper_negative_ok =
        rcp::apply_signed_track_offset(0, 0x80, adjusted_tick, err) &&
        adjusted_tick == 0 && err.empty();
    const bool offset_helper_positive_ok =
        rcp::apply_signed_track_offset(0, 0x7f, adjusted_tick, err) &&
        adjusted_tick == 127 && err.empty();
    const bool offset_helper_overflow_ok =
        !rcp::apply_signed_track_offset(0xffffffffu, 1, adjusted_tick, err) &&
        adjusted_tick == 0 && !err.empty();
    std::printf("RCP2 符号付き tick オフセット: 負値クランプ %s・正値加算 %s・32-bit 超過拒否 %s\n",
        offset_helper_negative_ok ? "合" : "NG",
        offset_helper_positive_ok ? "合" : "NG",
        offset_helper_overflow_ok ? "合" : "NG");

    doc.tracks.resize(1);
    doc.tracks[0].header.tick_offset = 0;
    doc.tracks[0].header.key = 0;
    doc.header.play_bias = 0;
    doc.tracks[0].events = {
        raw(62, 0, 0, 80), raw(60, 5, 8, 90), raw(60, 1, 4, 70),
        raw(64, 2, 3, 85), raw(0xfe, 0, 0, 0)
    };
    const bool note_gate_ok =
        rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.size() == 4 &&
        merged[0].event.kind == rcp::timeline_event_kind::note &&
        merged[0].event.tick == 0 && merged[0].event.note.note == 60 &&
        merged[0].event.note.velocity == 90 &&
        merged[1].event.kind == rcp::timeline_event_kind::note &&
        merged[1].event.tick == 6 && merged[1].event.note.note == 64 &&
        merged[2].event.kind == rcp::timeline_event_kind::note_off &&
        merged[2].event.tick == 9 && merged[2].event.note.note == 60 &&
        merged[3].event.kind == rcp::timeline_event_kind::note_off &&
        merged[3].event.tick == 9 && merged[3].event.note.note == 64 &&
        merged[2].event.note.velocity == 0 && merged[3].event.note.velocity == 0 && err.empty();
    std::printf("RCP2 ノート長展開: duration 0 抑止 %s・同音高の終了更新 %s・note-off 時刻/音高 %s\n",
        note_gate_ok ? "合" : "NG", note_gate_ok ? "合" : "NG",
        note_gate_ok ? "合" : "NG");
    doc.tracks[0].events = {
        raw(60, 5, 10, 90), raw(0xeb, 0, 0x0b, 0x7f),
        raw(60, 0, 0, 0), raw(0xfe, 0, 0, 0)
    };
    const bool explicit_note_off_order_ok =
        rcp::expand_intermediate_timeline(doc, merged, err) &&
        merged.size() == 3 &&
        merged[0].event.kind == rcp::timeline_event_kind::note &&
        merged[0].event.tick == 0 &&
        merged[1].event.kind == rcp::timeline_event_kind::control_change &&
        merged[1].event.tick == 5 &&
        merged[2].event.kind == rcp::timeline_event_kind::note_off &&
        merged[2].event.tick == 5 && err.empty();
    std::printf("RCP2 明示的 Note Off 同tick順序: CC の後に Note Off %s\n",
        explicit_note_off_order_ok ? "合" : "NG");


    std::printf("RCP2 文書時間展開: 複数トラック統合/トラック識別 %s・符号付き開始オフセット %s・オフセット境界 %s・全イベント種別オフセット %s・未対応コマンド拒否 %s・空文書 %s\n",
        order_ok ? "合" : "NG", offset_ok ? "合" : "NG",
        offset_bounds_ok ? "合" : "NG", all_kinds_offset_ok ? "合" : "NG",
        unsupported_rejected ? "合" : "NG", empty_ok ? "合" : "NG");
    std::printf("RCP2 ミュートトラック: イベント除外/未対応コマンド無視 %s\n",
        mute_skips_track_ok ? "合" : "NG");
}


static void check_rcp_aftertouch_commands()
{
    auto raw_event = [](u8 code, u8 p0, u8 p1, u8 p2) {
        rcp::sequence_event e;
        e.code = code; e.p0 = p0; e.p1 = p1; e.p2 = p2;
        return e;
    };
    std::string err;
    rcp::midi_channel_event channel_event;
    rcp::timeline_event_kind kind;
    const bool channel_decode = rcp::decode_midi_channel_event(
        raw_event(0xea, 3, 91, 0), channel_event, kind, err) &&
        kind == rcp::timeline_event_kind::channel_aftertouch &&
        channel_event.delay == 3 && channel_event.data1 == 91 && channel_event.data2 == 0 && err.empty();
    const bool poly_decode = rcp::decode_midi_channel_event(
        raw_event(0xed, 4, 60, 77), channel_event, kind, err) &&
        kind == rcp::timeline_event_kind::poly_aftertouch &&
        channel_event.delay == 4 && channel_event.data1 == 60 && channel_event.data2 == 77 && err.empty();
    const bool bad_channel_value = !rcp::decode_midi_channel_event(
        raw_event(0xea, 0, 0x80, 0), channel_event, kind, err) && !err.empty();
    const bool bad_poly_value = !rcp::decode_midi_channel_event(
        raw_event(0xed, 0, 60, 0x80), channel_event, kind, err) && !err.empty();

    // D0/D2/D3 are documented legacy aliases for Pitch Bend, Control Change,
    // and Program Change. Keep the existing standard MIDI event path.
    const bool d0_pitch_bend = rcp::decode_midi_channel_event(
        raw_event(0xd0, 4, 1, 64), channel_event, kind, err) &&
        kind == rcp::timeline_event_kind::pitch_bend &&
        channel_event.delay == 4 && channel_event.data1 == 1 && channel_event.data2 == 64 && err.empty();
    const bool d2_control_change = rcp::decode_midi_channel_event(
        raw_event(0xd2, 2, 7, 99), channel_event, kind, err) &&
        kind == rcp::timeline_event_kind::control_change &&
        channel_event.delay == 2 && channel_event.data1 == 7 && channel_event.data2 == 99 && err.empty();
    const bool d3_program_change = rcp::decode_midi_channel_event(
        raw_event(0xd3, 3, 10, 0), channel_event, kind, err) &&
        kind == rcp::timeline_event_kind::program_change &&
        channel_event.delay == 3 && channel_event.data1 == 10 && err.empty();

    rcp::timed_event event;
    event.kind = rcp::timeline_event_kind::channel_aftertouch;
    event.channel.data1 = 91;
    std::vector<u8> bytes;
    const bool channel_encode = rcp::encode_midi_message(event, 2, bytes, err) &&
        bytes == std::vector<u8>({0xd2, 91}) && err.empty();
    event.kind = rcp::timeline_event_kind::poly_aftertouch;
    event.channel.data1 = 60;
    event.channel.data2 = 77;
    const bool poly_encode = rcp::encode_midi_message(event, 2, bytes, err) &&
        bytes == std::vector<u8>({0xa2, 60, 77}) && err.empty();

    // The reference converter encodes note termination as Note On with velocity zero.
    event.kind = rcp::timeline_event_kind::note_off;
    event.note.note = 80;
    event.note.velocity = 0;
    const bool note_off_encode = rcp::encode_midi_message(event, 9, bytes, err) &&
        bytes == std::vector<u8>({0x99, 80, 0}) && err.empty();
    event.note.note = 0x80;
    const bool bad_note_off_encode = !rcp::encode_midi_message(event, 9, bytes, err) &&
        bytes.empty() && !err.empty();


    std::vector<rcp::timed_event> timeline;
    const std::vector<rcp::sequence_event> sequence = {
        raw_event(0xea, 2, 91, 0), raw_event(0xed, 3, 60, 77), raw_event(0xfe, 0, 0, 0)
    };
    const bool timeline_ok = rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 2 && timeline[0].tick == 0 &&
        timeline[0].kind == rcp::timeline_event_kind::channel_aftertouch &&
        timeline[1].tick == 2 && timeline[1].kind == rcp::timeline_event_kind::poly_aftertouch &&
        err.empty();

    const std::vector<rcp::sequence_event> alias_sequence = {
        raw_event(0xd2, 2, 7, 99), raw_event(0xd3, 3, 10, 0),
        raw_event(0xd0, 4, 1, 64), raw_event(0xfe, 0, 0, 0)
    };
    const bool aliases_timeline = rcp::expand_note_key_signature_tempo_timeline(
        alias_sequence, 120, timeline, err) &&
        timeline.size() == 3 &&
        timeline[0].kind == rcp::timeline_event_kind::control_change && timeline[0].tick == 0 &&
        timeline[1].kind == rcp::timeline_event_kind::program_change && timeline[1].tick == 2 &&
        timeline[2].kind == rcp::timeline_event_kind::pitch_bend && timeline[2].tick == 5 &&
        err.empty();

    std::printf("RCP2 EA/ED アフタータッチ: Channel/Poly decode %s・MIDI符号化 %s・時間展開 %s・7-bit 範囲外拒否 %s\n",
        channel_decode && poly_decode ? "合" : "NG",
        channel_encode && poly_encode && note_off_encode && bad_note_off_encode ? "合" : "NG",
        timeline_ok ? "合" : "NG",
        bad_channel_value && bad_poly_value ? "合" : "NG");
    std::printf("RCP2 D0/D2/D3 MIDI alias: decode %s・時間展開 %s\n",
        d0_pitch_bend && d2_control_change && d3_program_change ? "合" : "NG",
        aliases_timeline ? "合" : "NG");

    // E5 is the RCP2 Key Scan command: it emits no MIDI event but
    // advances the track clock by p0, as in the reference converter.
    const std::vector<rcp::sequence_event> key_scan_sequence = {
        raw_event(60, 2, 4, 90), raw_event(0xe5, 3, 0, 0),
        raw_event(62, 4, 4, 90), raw_event(0xfe, 0, 0, 0)
    };
    const bool key_scan_timeline =
        rcp::expand_note_key_signature_tempo_timeline(key_scan_sequence, 120, timeline, err) &&
        timeline.size() == 2 &&
        timeline[0].kind == rcp::timeline_event_kind::note && timeline[0].tick == 0 &&
        timeline[1].kind == rcp::timeline_event_kind::note && timeline[1].tick == 5 &&
        err.empty();
    std::printf("RCP2 E5 Key Scan: no MIDI event + p0 tick advance %s\n",
        key_scan_timeline ? "合" : "NG");

    // E1 selects XG instrument: CC32=p2, Program Change=p1.
    // E2 selects GS instrument: CC0=p2, Program Change=p1. Both emit at
    // the current tick and advance time once by p0.
    const std::vector<rcp::sequence_event> bank_sequence = {
        raw_event(0xe1, 2, 10, 65), raw_event(0xe2, 3, 40, 80),
        raw_event(60, 4, 2, 90), raw_event(0xfe, 0, 0, 0)
    };
    const bool bank_commands_timeline =
        rcp::expand_note_key_signature_tempo_timeline(bank_sequence, 120, timeline, err) &&
        timeline.size() == 5 &&
        timeline[0].tick == 0 && timeline[0].kind == rcp::timeline_event_kind::control_change &&
        timeline[0].channel.data1 == 32 && timeline[0].channel.data2 == 65 &&
        timeline[1].tick == 0 && timeline[1].kind == rcp::timeline_event_kind::program_change &&
        timeline[1].channel.data1 == 10 &&
        timeline[2].tick == 2 && timeline[2].kind == rcp::timeline_event_kind::control_change &&
        timeline[2].channel.data1 == 0 && timeline[2].channel.data2 == 80 &&
        timeline[3].tick == 2 && timeline[3].kind == rcp::timeline_event_kind::program_change &&
        timeline[3].channel.data1 == 40 &&
        timeline[4].tick == 5 && timeline[4].kind == rcp::timeline_event_kind::note &&
        timeline[4].note.note == 60 && err.empty();

    const std::vector<rcp::sequence_event> invalid_bank_sequence = {
        raw_event(0xe2, 0, 0x80, 80), raw_event(0xfe, 0, 0, 0)
    };
    const bool invalid_bank_value_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(invalid_bank_sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    std::printf("RCP2 E1/E2 MIDI command: XG/GS bank+program and timeline %s・範囲外拒否 %s\n",
        bank_commands_timeline ? "合" : "NG",
        invalid_bank_value_rejected ? "合" : "NG");

    const std::vector<rcp::sequence_event> route_sequence = {
        raw_event(0xe6, 2, 0x25, 0), raw_event(60, 3, 2, 90),
        raw_event(0xe6, 1, 0x81, 0), raw_event(62, 4, 2, 90),
        raw_event(0xfe, 0, 0, 0)
    };
    const bool route_timeline =
        rcp::expand_note_key_signature_tempo_timeline(route_sequence, 120, timeline, err) &&
        timeline.size() == 4 &&
        timeline[0].kind == rcp::timeline_event_kind::route_change && timeline[0].tick == 0 &&
        timeline[0].route.enabled && timeline[0].route.port == 2 && timeline[0].route.channel == 4 &&
        timeline[1].kind == rcp::timeline_event_kind::note && timeline[1].tick == 2 &&
        timeline[2].kind == rcp::timeline_event_kind::route_change && timeline[2].tick == 5 &&
        !timeline[2].route.enabled &&
        timeline[3].kind == rcp::timeline_event_kind::note && timeline[3].tick == 6 && err.empty();
    std::printf("RCP2 E6 MIDI route: port/channel 切替・dummy 切替・tick 進行 %s\n",
        route_timeline ? "合" : "NG");

    // DD sets GS address, DF sets device/model, and DE emits Roland DT1 SysEx.
    const std::vector<rcp::sequence_event> gs_sequence = {
        raw_event(0xdf, 1, 0x10, 0x42), raw_event(0xdd, 2, 0x40, 0x00),
        raw_event(0xde, 3, 0x7f, 0x00), raw_event(60, 4, 2, 90),
        raw_event(0xfe, 0, 0, 0)
    };
    const std::vector<u8> expected_gs_sysex = {
        0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7
    };
    const bool gs_parameter_timeline =
        rcp::expand_note_key_signature_tempo_timeline(gs_sequence, 120, timeline, err) &&
        timeline.size() == 2 &&
        timeline[0].kind == rcp::timeline_event_kind::sysex && timeline[0].tick == 3 &&
        timeline[0].sysex == expected_gs_sysex &&
        timeline[1].kind == rcp::timeline_event_kind::note && timeline[1].tick == 6 &&
        err.empty();
    const std::vector<rcp::sequence_event> invalid_gs_sequence = {
        raw_event(0xde, 0, 0x80, 0), raw_event(0xfe, 0, 0, 0)
    };
    const bool invalid_gs_value_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(invalid_gs_sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();
    // 0x90..0x97 fetch a header template and substitute p1/p2/current channel/checksum.
    std::array<rcp::user_sysex_template, 8> user_sysex{};
    user_sysex[0].name = "synthetic GS";
    user_sysex[0].data = {0x41, 0x80, 0x81, 0x82, 0x83, 0x01, 0x84};
    const std::vector<rcp::sequence_event> user_sysex_sequence = {
        raw_event(0xe6, 2, 0x06, 0), // raw route 6 -> channel 5
        raw_event(0x90, 3, 0x12, 0x34),
        raw_event(60, 1, 2, 90), raw_event(0xfe, 0, 0, 0)
    };
    const std::vector<u8> expected_user_sysex = {
        0xf0, 0x41, 0x12, 0x34, 0x05, 0x01, 0x7f, 0xf7
    };
    const bool user_sysex_ok =
        rcp::expand_note_key_signature_tempo_timeline(
            user_sysex_sequence, 120, timeline, err, &user_sysex, 2) &&
        timeline.size() == 3 &&
        timeline[0].kind == rcp::timeline_event_kind::route_change &&
        timeline[1].kind == rcp::timeline_event_kind::sysex && timeline[1].tick == 2 &&
        timeline[1].sysex == expected_user_sysex &&
        timeline[2].kind == rcp::timeline_event_kind::note && timeline[2].tick == 5 &&
        err.empty();
    const std::vector<rcp::sequence_event> dummy_sysex_sequence = {
        raw_event(0xe6, 0, 0x81, 0), raw_event(0x90, 1, 0x12, 0x34),
        raw_event(60, 1, 2, 90), raw_event(0xfe, 0, 0, 0)
    };
    const bool dummy_user_sysex_suppressed =
        rcp::expand_note_key_signature_tempo_timeline(
            dummy_sysex_sequence, 120, timeline, err, &user_sysex, 0) &&
        timeline.size() == 2 &&
        timeline[0].kind == rcp::timeline_event_kind::route_change &&
        timeline[1].kind == rcp::timeline_event_kind::note;
    const std::vector<rcp::sequence_event> invalid_user_sysex_sequence = {
        raw_event(0x90, 0, 0x80, 0), raw_event(0xfe, 0, 0, 0)
    };
    const bool invalid_user_sysex_parameter_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(
            invalid_user_sysex_sequence, 120, timeline, err, &user_sysex, 0) &&
        timeline.empty() && !err.empty();
    std::printf("RCP2 90..97 ユーザー SysEx: header/置換/チェックサム/tick %s・dummy 抑止 %s・7-bit 拒否 %s\n",
        user_sysex_ok ? "合" : "NG",
        dummy_user_sysex_suppressed ? "合" : "NG",
        invalid_user_sysex_parameter_rejected ? "合" : "NG");
    const std::vector<rcp::sequence_event> track_sysex_sequence = {
        raw_event(0xe6, 2, 0x06, 0),
        raw_event(0x98, 2, 0x12, 0x34),
        raw_event(0xf7, 0, 0x41, 0x80),
        raw_event(0xf7, 0, 0x81, 0x82),
        raw_event(0xf7, 0, 0x83, 0x01),
        raw_event(0xf7, 0, 0x84, 0xf7),
        raw_event(60, 3, 2, 90), raw_event(0xfe, 0, 0, 0)
    };
    const std::vector<u8> expected_track_sysex = {
        0xf0, 0x41, 0x12, 0x34, 0x05, 0x01, 0x7f, 0xf7
    };
    const bool track_sysex_continuation_ok =
        rcp::expand_note_key_signature_tempo_timeline(
            track_sysex_sequence, 120, timeline, err, nullptr, 2) &&
        timeline.size() == 3 &&
        timeline[0].kind == rcp::timeline_event_kind::route_change &&
        timeline[1].kind == rcp::timeline_event_kind::sysex &&
        timeline[1].tick == 2 && timeline[1].sysex == expected_track_sysex &&
        timeline[2].kind == rcp::timeline_event_kind::note &&
        timeline[2].tick == 4 && err.empty();

    const std::vector<rcp::sequence_event> orphan_continuation_sequence = {
        raw_event(0xf7, 0, 0x41, 0), raw_event(0xfe, 0, 0, 0)
    };
    const bool orphan_track_continuation_skipped =
        rcp::expand_note_key_signature_tempo_timeline(
            orphan_continuation_sequence, 120, timeline, err) &&
        timeline.empty() && err.empty();
    std::printf("RCP2 98 可変長 SysEx: F7連結/置換/チェックサム/tick %s・孤立F7スキップ %s\n",
        track_sysex_continuation_ok ? "合" : "NG",
        orphan_track_continuation_skipped ? "合" : "NG");

    // The loop expander must duplicate the complete 0x98 + F7 continuation
    // group, so each iteration emits one independent SysEx at its own tick.
    const std::vector<rcp::sequence_event> looped_track_sysex_sequence = {
        raw_event(0xf9, 0, 0, 0),
        raw_event(0x98, 1, 0x12, 0x34),
        raw_event(0xf7, 0, 0x41, 0x01),
        raw_event(0xf7, 0, 0xf7, 0xf7),
        raw_event(0xf8, 2, 0, 0),
        raw_event(0xfe, 0, 0, 0)
    };
    const bool track_sysex_loop_boundary_ok =
        rcp::expand_note_key_signature_tempo_timeline(
            looped_track_sysex_sequence, 120, timeline, err) &&
        timeline.size() == 2 &&
        timeline[0].kind == rcp::timeline_event_kind::sysex &&
        timeline[0].tick == 0 &&
        timeline[0].sysex == std::vector<u8>({0xf0, 0x41, 0x01, 0xf7}) &&
        timeline[1].kind == rcp::timeline_event_kind::sysex &&
        timeline[1].tick == 1 &&
        timeline[1].sysex == std::vector<u8>({0xf0, 0x41, 0x01, 0xf7}) &&
        err.empty();

    // A continuation after the loop terminator is outside the 0x98 command's
    // loop body and must not attach to the final expanded iteration.
    const std::vector<rcp::sequence_event> continuation_outside_loop_sequence = {
        raw_event(0xf9, 0, 0, 0),
        raw_event(0x98, 1, 0x12, 0x34),
        raw_event(0xf8, 2, 0, 0),
        raw_event(0xf7, 0, 0x41, 0x01),
        raw_event(60, 0, 2, 90), raw_event(0xfe, 0, 0, 0)
    };
    const bool track_sysex_continuation_outside_loop_ok =
        rcp::expand_note_key_signature_tempo_timeline(
            continuation_outside_loop_sequence, 120, timeline, err) &&
        timeline.size() == 1 &&
        timeline[0].kind == rcp::timeline_event_kind::note &&
        timeline[0].tick == 2 && err.empty();
    std::printf("RCP2 98 ループ外F7: 前のループ内SysExへ誤接続しない %s\n",
        track_sysex_continuation_outside_loop_ok ? "合" : "NG");

    // Invalid 7-bit parameters on the 0x98 command must still be rejected.
    // Unknown high-bit bytes in the continuation payload are handled separately
    // for compatibility with the reference converter.
    const std::vector<rcp::sequence_event> invalid_track_sysex_sequence = {
        raw_event(0x98, 0, 0x90, 0x34),
        raw_event(0xf7, 0, 0x41, 0x01),
        raw_event(0xfe, 0, 0, 0)
    };
    const bool invalid_track_sysex_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(
            invalid_track_sysex_sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    const std::vector<rcp::sequence_event> unknown_track_sysex_byte_sequence = {
        raw_event(0x98, 0, 0x12, 0x34),
        raw_event(0xf7, 0, 0x41, 0xff),
        raw_event(0xfe, 0, 0, 0)
    };
    const bool unknown_track_sysex_byte_skipped =
        rcp::expand_note_key_signature_tempo_timeline(
            unknown_track_sysex_byte_sequence, 120, timeline, err) &&
        timeline.size() == 1 &&
        timeline[0].kind == rcp::timeline_event_kind::sysex &&
        timeline[0].sysex == std::vector<u8>({0xf0, 0x41, 0xf7}) &&
        err.empty();
    std::printf("RCP2 98 ループ境界: 各反復でSysEx再生成 %s・不正パラメーター拒否 %s・未知高位バイトをスキップ %s\n",
        track_sysex_loop_boundary_ok ? "合" : "NG",
        invalid_track_sysex_rejected ? "合" : "NG",
        unknown_track_sysex_byte_skipped ? "合" : "NG");
    // A 0x98 event whose continuation records contain only F7 padding
    // emits no empty SysEx, but its step still advances the following event.
    const std::vector<rcp::sequence_event> empty_track_sysex_sequence = {
        raw_event(0x98, 2, 0x12, 0x34),
        raw_event(0xf7, 0, 0xf7, 0xf7),
        raw_event(60, 3, 2, 90), raw_event(0xfe, 0, 0, 0)
    };
    const bool empty_track_sysex_tick_ok =
        rcp::expand_note_key_signature_tempo_timeline(
            empty_track_sysex_sequence, 120, timeline, err) &&
        timeline.size() == 1 &&
        timeline[0].kind == rcp::timeline_event_kind::note &&
        timeline[0].tick == 2 && err.empty();
    std::printf("RCP2 98 空 payload: SysEx省略/tick維持 %s\n",
        empty_track_sysex_tick_ok ? "合" : "NG");
    // External commands gather their F7 continuation records but do not
    // generate MIDI output. Their p0 delay still advances the next event.
    const std::vector<rcp::sequence_event> external_command_sequence = {
        raw_event(0x99, 2, 0x41, 0x42),
        raw_event(0xf7, 0, 'O', 'K'),
        raw_event(0xf7, 0, 0xf7, 0xf7),
        raw_event(60, 3, 2, 90), raw_event(0xfe, 0, 0, 0)
    };
    const bool external_command_ignored_tick_ok =
        rcp::expand_note_key_signature_tempo_timeline(
            external_command_sequence, 120, timeline, err) &&
        timeline.size() == 1 &&
        timeline[0].kind == rcp::timeline_event_kind::note &&
        timeline[0].tick == 2 && err.empty();

    // The 0x99 + F7 group is copied together when inside an F8/F9 loop.
    const std::vector<rcp::sequence_event> looped_external_command_sequence = {
        raw_event(0xf9, 0, 0, 0),
        raw_event(0x99, 1, 0x41, 0x42),
        raw_event(0xf7, 0, 'A', 'B'),
        raw_event(0xf8, 2, 0, 0),
        raw_event(60, 0, 2, 90), raw_event(0xfe, 0, 0, 0)
    };
    const bool external_command_loop_group_ok =
        rcp::expand_note_key_signature_tempo_timeline(
            looped_external_command_sequence, 120, timeline, err) &&
        timeline.size() == 1 &&
        timeline[0].kind == rcp::timeline_event_kind::note &&
        timeline[0].tick == 2 && err.empty();
    std::printf("RCP2 99 外部コマンド: F7連結/MIDI出力なし/tick %s・ループ境界 %s\n",
        external_command_ignored_tick_ok ? "合" : "NG",
        external_command_loop_group_ok ? "合" : "NG");
    std::printf("RCP2 DD/DE/DF GS SysEx: state/checksum/tick %s・7-bit 範囲外拒否 %s\n",
        gs_parameter_timeline ? "合" : "NG",
        invalid_gs_value_rejected ? "合" : "NG");
}

static void check_rcp_midi_encoding()
{
    std::string err;
    std::vector<u8> bytes;
    rcp::timed_event event;
    event.kind = rcp::timeline_event_kind::note;
    event.note.note = 60;
    event.note.velocity = 100;
    const bool note_on_ok =
        rcp::encode_midi_message(event, 2, bytes, err) &&
        bytes == std::vector<u8>({0x92, 60, 100}) && err.empty();

    event.kind = rcp::timeline_event_kind::note_off;
    event.note.note = 60;
    const bool note_off_ok =
        rcp::encode_midi_message(event, 2, bytes, err) &&
        bytes == std::vector<u8>({0x92, 60, 0}) && err.empty();

    event.kind = rcp::timeline_event_kind::control_change;
    event.channel.data1 = 7;
    event.channel.data2 = 99;
    const bool cc_ok =
        rcp::encode_midi_message(event, 1, bytes, err) &&
        bytes == std::vector<u8>({0xb1, 7, 99}) && err.empty();

    event.kind = rcp::timeline_event_kind::program_change;
    event.channel.data1 = 10;
    const bool program_ok =
        rcp::encode_midi_message(event, 15, bytes, err) &&
        bytes == std::vector<u8>({0xcf, 10}) && err.empty();

    event.kind = rcp::timeline_event_kind::pitch_bend;
    event.channel.data1 = 0;
    event.channel.data2 = 64;
    const bool bend_ok =
        rcp::encode_midi_message(event, 0, bytes, err) &&
        bytes == std::vector<u8>({0xe0, 0, 64}) && err.empty();

    event.kind = rcp::timeline_event_kind::note;
    event.note.note = 60;
    event.note.velocity = 100;
    const bool invalid_channel_rejected =
        !rcp::encode_midi_message(event, 16, bytes, err) &&
        bytes.empty() && !err.empty();

    const bool metadata_rejected =
        (event.kind = rcp::timeline_event_kind::tempo,
         !rcp::encode_midi_message(event, 0, bytes, err)) &&
        bytes.empty() && !err.empty();

    std::printf("RCP2 MIDIメッセージ符号化: Note On %s・Note Off %s・CC %s・Program Change %s・Pitch Bend %s・不正チャンネル拒否 %s・メタデータ分離 %s\n",
        note_on_ok ? "合" : "NG", note_off_ok ? "合" : "NG",
        cc_ok ? "合" : "NG", program_ok ? "合" : "NG",
        bend_ok ? "合" : "NG", invalid_channel_rejected ? "合" : "NG",
        metadata_rejected ? "合" : "NG");
}


static void check_rcp_tick_to_seconds()
{
    rcp::document_header header;
    header.ticks_per_quarter = 480;
    header.tempo_bpm = 120;
    std::vector<rcp::timed_track_event> timeline;
    rcp::timed_track_event event;
    event.track_index = 0;
    event.event.kind = rcp::timeline_event_kind::note;
    event.event.tick = 0;
    timeline.push_back(event);
    event.event.kind = rcp::timeline_event_kind::tempo;
    event.event.tick = 480;
    event.event.tempo.microseconds_per_quarter = 250000;
    timeline.push_back(event);
    event.event.kind = rcp::timeline_event_kind::note;
    event.event.tick = 960;
    timeline.push_back(event);

    std::vector<rcp::timed_seconds_event> seconds;
    std::string err;
    const bool timing_ok =
        rcp::convert_timeline_to_seconds(header, timeline, seconds, err) &&
        seconds.size() == 3 &&
        seconds[0].time == 0.0 &&
        seconds[1].time == 0.5 &&
        seconds[2].time == 0.75 &&
        seconds[1].source.event.kind == rcp::timeline_event_kind::tempo &&
        seconds[2].source.event.tick == 960 && err.empty();

    rcp::document_header invalid_header = header;
    invalid_header.ticks_per_quarter = 0;
    const bool zero_ppq_rejected =
        !rcp::convert_timeline_to_seconds(invalid_header, timeline, seconds, err) &&
        seconds.empty() && !err.empty();
    invalid_header = header;
    invalid_header.tempo_bpm = 0;
    const bool zero_bpm_rejected =
        !rcp::convert_timeline_to_seconds(invalid_header, timeline, seconds, err) &&
        seconds.empty() && !err.empty();

    timeline[2].event.tick = 400;
    const bool reverse_tick_rejected =
        !rcp::convert_timeline_to_seconds(header, timeline, seconds, err) &&
        seconds.empty() && !err.empty();

    timeline[2].event.tick = 960;
    timeline[1].event.tempo.microseconds_per_quarter = 0;
    const bool zero_tempo_rejected =
        !rcp::convert_timeline_to_seconds(header, timeline, seconds, err) &&
        seconds.empty() && !err.empty();

    std::printf("RCP2 tick→秒変換: テンポ変更前後 %s・PPQ 0 拒否 %s・初期 BPM 0 拒否 %s・逆順 tick 拒否 %s・テンポ値 0 拒否 %s\n",
        timing_ok ? "合" : "NG", zero_ppq_rejected ? "合" : "NG",
        zero_bpm_rejected ? "合" : "NG", reverse_tick_rejected ? "合" : "NG",
        zero_tempo_rejected ? "合" : "NG");
}


static void check_rcp_timeline_output_encoding()
{
    std::vector<rcp::timed_seconds_event> t(3);
    t[0].time=0.0; t[0].source.track_index=0;
    t[0].source.event.kind=rcp::timeline_event_kind::note;
    t[0].source.event.note.note=60; t[0].source.event.note.velocity=100;
    t[1].time=0.25; t[1].source.track_index=1;
    t[1].source.event.kind=rcp::timeline_event_kind::tempo;
    t[2].time=0.5; t[2].source.track_index=1;
    t[2].source.event.kind=rcp::timeline_event_kind::program_change;
    t[2].source.event.channel.data1=10;
    std::vector<rcp::midi_output_event> out; std::string err;
    const bool ok=rcp::encode_timeline_to_midi_events(t,{2,5},{1,3},out,err) &&
        out.size()==2 && out[0].bytes==std::vector<u8>({0x92,60,100}) && out[0].port==1 &&
        out[1].bytes==std::vector<u8>({0xc5,10}) && out[1].port==3 && err.empty();
    const bool missing=!rcp::encode_timeline_to_midi_events(t,{2},{1},out,err) && out.empty() && !err.empty();
    t[0].time=0.6;
    const bool reversed=!rcp::encode_timeline_to_midi_events(t,{2,5},{1,3},out,err) && out.empty() && !err.empty();
    std::printf("RCP2 秒イベント→MIDI出力: 割当/メタデータ除外 %s・割当不足拒否 %s・逆順時刻拒否 %s\n",
        ok?"合":"NG",missing?"合":"NG",reversed?"合":"NG");
}


static void check_rcp_track_midi_mapping()
{
    rcp::intermediate_document document;
    document.tracks.resize(3);
    document.tracks[0].header.midi_channel = 0x23; // port 2, channel 3
    document.tracks[1].header.midi_channel = 0x41; // port 4, channel 1
    document.tracks[2].header.midi_channel = 0x80; // dummy/unrouted
    std::vector<rcp::track_midi_mapping> mapping;
    std::string err;
    const bool mapped = rcp::map_track_midi_outputs(document, mapping, err) &&
        mapping.size() == 3 && mapping[0].enabled && mapping[0].channel == 3 && mapping[0].port == 2 &&
        mapping[1].enabled && mapping[1].channel == 1 && mapping[1].port == 4 &&
        !mapping[2].enabled && err.empty();

    std::vector<rcp::timed_seconds_event> timeline(4);
    timeline[0].time=0.0; timeline[0].source.track_index=0;
    timeline[0].source.event.kind=rcp::timeline_event_kind::route_change;
    timeline[0].source.event.route={4, 3, true};
    timeline[1].time=0.1; timeline[1].source.track_index=0;
    timeline[1].source.event.kind=rcp::timeline_event_kind::note;
    timeline[1].source.event.note.note=60; timeline[1].source.event.note.velocity=90;
    timeline[2].time=0.2; timeline[2].source.track_index=0;
    timeline[2].source.event.kind=rcp::timeline_event_kind::route_change;
    timeline[2].source.event.route={0, 0, false};
    timeline[3].time=0.3; timeline[3].source.track_index=0;
    timeline[3].source.event.kind=rcp::timeline_event_kind::note;
    timeline[3].source.event.note.note=62; timeline[3].source.event.note.velocity=90;
    std::vector<rcp::midi_output_event> output;
    const bool basic_routed = rcp::encode_timeline_to_midi_events(timeline, mapping, output, err) &&
        output.size() == 1 && output[0].bytes == std::vector<u8>({0x94, 60, 90}) &&
        output[0].port == 3 && err.empty();

    // Existing note is released on its original route after E6; later notes use the new route.
    timeline.assign(6, rcp::timed_seconds_event{});
    timeline[0].time=0.0; timeline[0].source.track_index=0;
    timeline[0].source.event.kind=rcp::timeline_event_kind::note;
    timeline[0].source.event.note.note=60; timeline[0].source.event.note.velocity=90;
    timeline[0].source.event.route={3, 2, true}; timeline[0].source.event.route_fixed=true;
    timeline[1].time=0.1; timeline[1].source.track_index=0;
    timeline[1].source.event.kind=rcp::timeline_event_kind::route_change;
    timeline[1].source.event.route={2, 1, true};
    timeline[2].time=0.15; timeline[2].source.track_index=0;
    timeline[2].source.event.kind=rcp::timeline_event_kind::note;
    timeline[2].source.event.note.note=62; timeline[2].source.event.note.velocity=80;
    timeline[2].source.event.route={2, 1, true}; timeline[2].source.event.route_fixed=true;
    timeline[3].time=0.2; timeline[3].source.track_index=0;
    timeline[3].source.event.kind=rcp::timeline_event_kind::note_off;
    timeline[3].source.event.note.note=60; timeline[3].source.event.route={3, 2, true};
    timeline[3].source.event.route_fixed=true;
    timeline[4].time=0.25; timeline[4].source.track_index=0;
    timeline[4].source.event.kind=rcp::timeline_event_kind::note_off;
    timeline[4].source.event.note.note=62; timeline[4].source.event.route={2, 1, true};
    timeline[4].source.event.route_fixed=true;
    timeline[5].time=0.3; timeline[5].source.track_index=0;
    timeline[5].source.event.kind=rcp::timeline_event_kind::route_change;
    timeline[5].source.event.route={0, 0, false};
    const bool fixed_route_ok = rcp::encode_timeline_to_midi_events(timeline, mapping, output, err) &&
        output.size() == 4 &&
        output[0].bytes == std::vector<u8>({0x93, 60, 90}) && output[0].port == 2 &&
        output[1].bytes == std::vector<u8>({0x92, 62, 80}) && output[1].port == 1 &&
        output[2].bytes.size() == 3 && (output[2].bytes[0] == 0x83 || output[2].bytes[0] == 0x93) &&
        output[2].bytes[1] == 60 && output[2].port == 2 &&
        output[3].bytes.size() == 3 && (output[3].bytes[0] == 0x82 || output[3].bytes[0] == 0x92) &&
        output[3].bytes[1] == 62 && output[3].port == 1 && err.empty();
    rcp_e6_note_off_route_test_ok = mapped && basic_routed && fixed_route_ok;
    std::printf("RCP2 トラック MIDI 割当: ポート/チャンネル分離 %s・E6 動的切替/dummy 除外 %s・Note Off 発音時ルート維持 %s\n",
        mapped ? "合" : "NG", basic_routed ? "合" : "NG", fixed_route_ok ? "合" : "NG");
}

static void check_rcp_tracks()
{
    const char signature[] = "RCM-PC98V2.0(C)COME ON MUSIC\r\n";
    const size_t header_size = 0x586;
    const size_t track_header_size = 0x2c;
    const size_t first_length = track_header_size + 12;
    const size_t second_length = track_header_size + 8;
    std::vector<u8> data(header_size + first_length + second_length, 0);
    std::memcpy(data.data(), signature, sizeof(signature) - 1);
    data[30] = 0;
    data[31] = 0;
    data[0x1e6] = 2; // two tracks
    const size_t at = header_size;
    data[at] = u8(first_length);
    data[at + 1] = u8(first_length >> 8);
    data[at + 2] = 1;
    data[at + track_header_size] = 60;
    data[at + track_header_size + 4] = 0xfe;
    // A complete, valid note record after FE remains inside track 1's declared boundary but must not be parsed.
    data[at + track_header_size + 8] = 61;
    data[at + track_header_size + 9] = 0;
    data[at + track_header_size + 10] = 48;
    data[at + track_header_size + 11] = 100;
    const size_t at2 = at + first_length;
    data[at2] = u8(second_length);
    data[at2 + 1] = u8(second_length >> 8);
    data[at2 + 2] = 2;
    data[at2 + track_header_size] = 64;
    data[at2 + track_header_size + 4] = 0xfe;

    std::vector<rcp::track> tracks;
    std::string err;
    const bool valid_ok = rcp::parse_tracks(data.data(), data.size(), tracks, err) &&
        tracks.size() == 2 &&
        tracks[0].header.track_id == 1 && tracks[0].sequence.size() == 2 &&
        tracks[0].sequence[0].code == 60 && tracks[0].sequence[1].code == 0xfe &&
        tracks[1].header.track_id == 2 && tracks[1].sequence.size() == 2 &&
        tracks[1].sequence[0].code == 64 && tracks[1].sequence[1].code == 0xfe;

    std::vector<u8> short_length = data;
    short_length[at] = 0x2b;
    short_length[at + 1] = 0;
    const bool short_length_ok = !rcp::parse_tracks(short_length.data(), short_length.size(), tracks, err) &&
        tracks.empty();

    std::vector<u8> overrun = data;
    overrun[at] = 0xff;
    overrun[at + 1] = 0xff;
    const bool overrun_ok = !rcp::parse_tracks(overrun.data(), overrun.size(), tracks, err) &&
        tracks.empty();

    std::vector<u8> no_end = data;
    no_end[at + track_header_size + 4] = 0x7f;
    const bool no_end_ok = !rcp::parse_tracks(no_end.data(), no_end.size(), tracks, err) &&
        tracks.empty();

    std::printf("RCP2 トラック走査: 複数トラック/FE 後の余剰 %s・短い長さ拒否 %s・末尾越え拒否 %s・FE なし拒否 %s\n",
        valid_ok ? "合" : "NG", short_length_ok ? "合" : "NG",
        overrun_ok ? "合" : "NG", no_end_ok ? "合" : "NG");
}

static void check_rcp_intermediate()
{
    const char signature[] = "RCM-PC98V2.0(C)COME ON MUSIC\r\n";
    const size_t header_size = 0x586;
    const size_t track_header_size = 0x2c;
    const size_t track_length = track_header_size + 16;
    std::vector<u8> data(header_size + track_length, 0);
    std::memcpy(data.data(), signature, sizeof(signature) - 1);
    data[30] = 0;
    data[31] = 0;
    data[0x1c0] = 0xe0;
    data[0x1e7] = 1;
    data[0x1c1] = 100;
    data[0x1e6] = 1;
    const size_t at = header_size;
    data[at] = u8(track_length);
    data[at + 1] = u8(track_length >> 8);
    data[at + 2] = 7;
    data[at + 8] = 'I';
    data[at + 9] = 'R';
    data[at + 10] = ' ';
    data[at + 11] = 't';
    data[at + 12] = 'e';
    data[at + 13] = 's';
    data[at + 14] = 't';
    // Two distinct note records followed by FE: preserve raw fields and source order.
    data[at + track_header_size] = 60;
    data[at + track_header_size + 1] = 12;
    data[at + track_header_size + 2] = 100;
    data[at + track_header_size + 3] = 90;
    data[at + track_header_size + 4] = 64;
    data[at + track_header_size + 5] = 3;
    data[at + track_header_size + 6] = 20;
    data[at + track_header_size + 7] = 70;
    data[at + track_header_size + 8] = 0xfe;

    rcp::intermediate_document doc;
    std::string err;
    const bool ok = rcp::build_intermediate(data.data(), data.size(), doc, err);
    const bool fields_ok = ok && doc.header.ticks_per_quarter == 480 &&
        doc.header.tempo_bpm == 100 && doc.tracks.size() == 1 &&
        doc.tracks[0].header.track_id == 7 &&
        doc.tracks[0].header.name == "IR test" &&
        doc.tracks[0].events.size() == 3 &&
        doc.tracks[0].events[0].record_index == 0 &&
        doc.tracks[0].events[0].raw.code == 60 &&
        doc.tracks[0].events[0].raw.p0 == 12 &&
        doc.tracks[0].events[0].raw.p1 == 100 &&
        doc.tracks[0].events[0].raw.p2 == 90 &&
        doc.tracks[0].events[1].record_index == 1 &&
        doc.tracks[0].events[1].raw.code == 64 &&
        doc.tracks[0].events[1].raw.p0 == 3 &&
        doc.tracks[0].events[1].raw.p1 == 20 &&
        doc.tracks[0].events[1].raw.p2 == 70 &&
        doc.tracks[0].events[2].record_index == 2 &&
        doc.tracks[0].events[2].raw.code == 0xfe;

    std::vector<u8> invalid = data;
    invalid[at + track_header_size + 8] = 0x7f;
    const bool reject_ok = !rcp::build_intermediate(invalid.data(), invalid.size(), doc, err) &&
        doc.tracks.empty() && doc.header.title.empty();

    std::printf("RCP2 中間表現: 生レコード保持/順序 %s・不正入力時に出力を消去 %s\n",
        fields_ok ? "合" : "NG", reject_ok ? "合" : "NG");
}

static void check_rcp_tempo_event_decode()
{
    rcp::sequence_event raw;
    raw.code = 0xe7;
    raw.p0 = 7;
    raw.p1 = 32;
    raw.p2 = 4;
    rcp::tempo_event event;
    std::string err = "stale error";
    const bool fields_ok = rcp::decode_tempo_event(raw, 120, event, err) &&
        event.delay == 7 && event.scale == 32 && event.gradual == 4 &&
        event.microseconds_per_quarter == 1000000 && err.empty();

    raw.code = 0xe6;
    const bool wrong_command_rejected =
        !rcp::decode_tempo_event(raw, 120, event, err) &&
        event.delay == 0 && event.scale == 0 &&
        event.microseconds_per_quarter == 0 && !err.empty();

    raw.code = 0xe7;
    raw.p1 = 0;
    const bool zero_scale_rejected =
        !rcp::decode_tempo_event(raw, 120, event, err) &&
        event.delay == 0 && event.scale == 0 &&
        event.microseconds_per_quarter == 0 && !err.empty();

    std::printf("RCP2 E7 イベント解釈（実験）: p0/p1/p2 保持 %s・誤コマンド拒否 %s・scale 0 拒否 %s\n",
        fields_ok ? "合" : "NG", wrong_command_rejected ? "合" : "NG",
        zero_scale_rejected ? "合" : "NG");
}



static void check_rcp_tempo_timeline()
{
    auto event = [](u8 code, u8 p0, u8 p1, u8 p2) {
        rcp::sequence_event raw;
        raw.code = code; raw.p0 = p0; raw.p1 = p1; raw.p2 = p2;
        return raw;
    };
    std::vector<rcp::sequence_event> sequence = {
        event(60, 5, 4, 90),
        event(0xe7, 7, 32, 0),
        event(62, 3, 4, 80),
        event(0xfe, 0, 0, 0)
    };
    std::vector<rcp::timed_event> timeline;
    std::string err;
    const bool timing_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 3 &&
        timeline[0].kind == rcp::timeline_event_kind::note &&
        timeline[0].tick == 0 && timeline[0].note.note == 60 &&
        timeline[1].kind == rcp::timeline_event_kind::tempo &&
        timeline[1].tick == 5 && timeline[1].tempo.delay == 7 &&
        timeline[1].tempo.microseconds_per_quarter == 1000000 &&
        timeline[2].kind == rcp::timeline_event_kind::note &&
        timeline[2].tick == 12 && timeline[2].note.note == 62 && err.empty();

    sequence = {event(60, 5, 4, 90), event(0xf5, 3, 0, 0),
                event(0xe7, 7, 32, 0), event(62, 3, 4, 80),
                event(0xfe, 0, 0, 0)};
    const bool same_tick_order_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 4 &&
        timeline[0].tick == 0 && timeline[0].kind == rcp::timeline_event_kind::note &&
        timeline[1].tick == 5 && timeline[1].kind == rcp::timeline_event_kind::key_signature &&
        timeline[1].key.accidentals == 3 &&
        timeline[2].tick == 5 && timeline[2].kind == rcp::timeline_event_kind::tempo &&
        timeline[3].tick == 12 && timeline[3].kind == rcp::timeline_event_kind::note &&
        timeline[3].note.note == 62 && err.empty();

    // F6 comment and following F7 continuation records are metadata: they
    // neither emit timeline events nor advance the tick counter.
    sequence = {event(60, 5, 4, 90), event(0xf6, 7, 32, 0),
                event(0xf7, 1, 2, 3), event(62, 3, 4, 80),
                event(0xfe, 0, 0, 0)};
    const bool comments_ignored =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 2 &&
        timeline[0].kind == rcp::timeline_event_kind::note &&
        timeline[0].tick == 0 && timeline[0].note.note == 60 &&
        timeline[1].kind == rcp::timeline_event_kind::note &&
        timeline[1].tick == 5 && timeline[1].note.note == 62 && err.empty();

    sequence = {event(0xf7, 0, 0, 0), event(0xfe, 0, 0, 0)};
    const bool orphan_comment_continuation_skipped =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && err.empty();

    // Loop expansion can change the indices before a comment. F7 continuation
    // records must be consumed from the expanded sequence, not the raw input.
    sequence = {event(0xf9, 0, 0, 0), event(60, 1, 4, 90),
                event(0xf8, 2, 0, 0), event(0xf6, 0, 'A', 'B'),
                event(0xf7, 0, 'C', 'D'), event(62, 1, 4, 80),
                event(0xfe, 0, 0, 0)};
    const bool comments_after_loop_expansion_ignored =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 3 &&
        timeline[0].kind == rcp::timeline_event_kind::note && timeline[0].note.note == 60 &&
        timeline[1].kind == rcp::timeline_event_kind::note && timeline[1].note.note == 60 &&
        timeline[2].kind == rcp::timeline_event_kind::note && timeline[2].note.note == 62 &&
        err.empty();

    // FD marks a bar boundary; its step byte must not advance the timeline.
    sequence = {event(60, 5, 4, 90), event(0xfd, 99, 0, 0),
                event(62, 3, 4, 80), event(0xfe, 0, 0, 0)};
    const bool measure_end_zero_time =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 2 &&
        timeline[0].kind == rcp::timeline_event_kind::note && timeline[0].tick == 0 &&
        timeline[1].kind == rcp::timeline_event_kind::note && timeline[1].tick == 5 &&
        err.empty();

    sequence = {event(60, 5, 4, 90), event(0xe7, 7, 0, 0),
                event(0xfe, 0, 0, 0)};
    const bool invalid_tempo_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    // p2 は値を保持するが、段階的なテンポ曲線にはまだ展開しない。
    sequence = {event(0xe7, 0, 32, 4), event(0xfe, 0, 0, 0)};
    const bool gradual_retained =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 1 && timeline[0].kind == rcp::timeline_event_kind::tempo &&
        timeline[0].tempo.gradual == 4 && timeline[0].tempo.microseconds_per_quarter == 1000000 &&
        err.empty();

    // FE 以降の未知コマンドも無視し、FE より前のノートだけを展開する。
    sequence = {event(60, 1, 4, 90), event(0xfe, 0, 0, 0),
                event(0x90, 2, 4, 80)};
    const bool end_terminates_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 1 && timeline[0].tick == 0 &&
        timeline[0].note.note == 60 && err.empty();

    // An unsupported command after a valid note must fail the whole expansion;
    // the earlier note must not leak out as a partial successful result.
    sequence = {event(60, 1, 4, 90), event(0xf4, 0, 0, 0),
                event(0xfe, 0, 0, 0)};
    const bool unsupported_mixed_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    sequence = {event(60, 1, 4, 90)};
    const bool missing_end_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    sequence.clear();
    const bool empty_missing_end_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    std::printf("RCP2 E7 時間展開（実験）: テンポ位置/次ノート tick %s・F5 同 tick/順序 %s・F6/F7 コメント処理 %s・scale 0 拒否 %s・p2 値保持 %s・FE 終端/後続未知無視 %s・未対応コマンド混在拒否 %s・FE なし/空入力拒否 %s\n",
        timing_ok ? "合" : "NG", same_tick_order_ok ? "合" : "NG",
        comments_ignored && comments_after_loop_expansion_ignored && measure_end_zero_time && orphan_comment_continuation_skipped ? "合" : "NG",
        invalid_tempo_rejected ? "合" : "NG",
        gradual_retained ? "合" : "NG", end_terminates_ok ? "合" : "NG",
        unsupported_mixed_rejected ? "合" : "NG",
        missing_end_rejected && empty_missing_end_rejected ? "合" : "NG");
}


static void check_rcp_counted_loops()
{
    auto event = [](u8 code, u8 p0, u8 p1, u8 p2) {
        rcp::sequence_event raw;
        raw.code = code; raw.p0 = p0; raw.p1 = p1; raw.p2 = p2;
        return raw;
    };
    std::vector<rcp::sequence_event> sequence = {
        event(60, 2, 4, 90),
        event(0xf9, 0, 0, 0),
        event(62, 3, 4, 80),
        event(0xf8, 2, 0, 0),
        event(64, 1, 4, 70),
        event(0xfe, 0, 0, 0)
    };
    std::vector<rcp::timed_event> timeline;
    std::string err;
    const bool simple_loop_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 4 &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 &&
        timeline[1].note.note == 62 && timeline[1].tick == 2 &&
        timeline[2].note.note == 62 && timeline[2].tick == 5 &&
        timeline[3].note.note == 64 && timeline[3].tick == 8 && err.empty();

    // Nested finite loops: the inner body repeats twice for each outer pass.
    sequence = {
        event(0xf9, 0, 0, 0),
        event(60, 1, 4, 90),
        event(0xf9, 0, 0, 0),
        event(62, 1, 4, 80),
        event(0xf8, 2, 0, 0),
        event(0xf8, 2, 0, 0),
        event(0xfe, 0, 0, 0)
    };
    const bool nested_loop_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 6 &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 &&
        timeline[1].note.note == 62 && timeline[1].tick == 1 &&
        timeline[2].note.note == 62 && timeline[2].tick == 2 &&
        timeline[3].note.note == 60 && timeline[3].tick == 3 &&
        timeline[4].note.note == 62 && timeline[4].tick == 4 &&
        timeline[5].note.note == 62 && timeline[5].tick == 5 && err.empty();

    // FC expansion runs before F8/F9 expansion. Check that a completed FC
    // reference returns to the caller and the following counted loop still
    // expands with the expected event order and ticks.
    auto fc_to = [&](size_t target_index) {
        const size_t offset = size_t(0x2c) + target_index * 4;
        return event(0xfc, 0, u8(offset & 0xff), u8((offset >> 8) & 0xff));
    };
    sequence = {
        event(60, 1, 4, 90), fc_to(0),
        event(0xf9, 0, 0, 0), event(62, 1, 4, 80),
        event(0xf8, 2, 0, 0), event(0xfe, 0, 0, 0)
    };
    const bool fc_then_counted_loop_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 4 &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 &&
        timeline[1].note.note == 60 && timeline[1].tick == 1 &&
        timeline[2].note.note == 62 && timeline[2].tick == 2 &&
        timeline[3].note.note == 62 && timeline[3].tick == 3 && err.empty();

    // F8 count=1 means a single pass, with no replay.
    sequence = {event(0xf9, 0, 0, 0), event(60, 1, 4, 90),
                event(0xf8, 1, 0, 0), event(0xfe, 0, 0, 0)};
    const bool single_pass_loop_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 1 && timeline[0].note.note == 60 &&
        timeline[0].tick == 0 && err.empty();

    sequence = {event(60, 1, 4, 90), event(0xf8, 2, 0, 0), event(0xfe, 0, 0, 0)};
    const bool orphan_end_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    sequence = {event(0xf9, 0, 0, 0), event(60, 1, 4, 90), event(0xfe, 0, 0, 0)};
    const bool track_loop_marker_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 1 && timeline[0].kind == rcp::timeline_event_kind::note &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 && err.empty();

    // An unclosed outer F9 must not consume the F8 that closes an inner finite loop.
    sequence = {event(0xf9, 0, 0, 0), event(60, 1, 4, 90),
                event(0xf9, 0, 0, 0), event(62, 1, 4, 80),
                event(0xf8, 2, 0, 0), event(0xfe, 0, 0, 0)};
    const bool open_outer_nested_loop_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 3 &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 &&
        timeline[1].note.note == 62 && timeline[1].tick == 1 &&
        timeline[2].note.note == 62 && timeline[2].tick == 2 && err.empty();

    sequence = {event(0xf9, 0, 0, 0), event(60, 1, 4, 90),
                event(0xf8, 0, 0, 0), event(0xfe, 0, 0, 0)};
    const bool zero_count_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    sequence = {event(0xf9, 0, 0, 0), event(60, 1, 4, 90),
                event(0xf8, 0x7f, 0, 0), event(0xfe, 0, 0, 0)};
    const bool high_count_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    // Three nested 126-pass loops exceed the 1,000,000-record expansion
    // budget. The public timeline API must fail and leave no partial output.
    sequence = {
        event(0xf9, 0, 0, 0), event(0xf9, 0, 0, 0), event(0xf9, 0, 0, 0),
        event(60, 1, 1, 90),
        event(0xf8, 126, 0, 0), event(0xf8, 126, 0, 0),
        event(0xf8, 126, 0, 0), event(0xfe, 0, 0, 0)
    };
    rcp_loop_budget_test_ok =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();
    if (!rcp_loop_budget_test_ok)
        std::printf("RCP2 F8/F9 expansion budget was not enforced safely\n");

    std::printf("RCP2 F8/F9 有限ループ: 単純反復 %s・入れ子 %s・1回境界 %s・FC後の連携 %s・不対 F8/F9 拒否 %s・未確定反復数拒否 %s\n",
        simple_loop_ok ? "合" : "NG", nested_loop_ok ? "合" : "NG",
        single_pass_loop_ok ? "合" : "NG", fc_then_counted_loop_ok ? "合" : "NG",
        orphan_end_rejected && track_loop_marker_ok && open_outer_nested_loop_ok ? "合" : "NG",
        zero_count_rejected && high_count_rejected ? "合" : "NG");
}


static void check_rcp_same_tick_order()
{
    auto raw = [](u8 code, u8 p0, u8 p1, u8 p2) {
        rcp::intermediate_event event;
        event.raw.code = code;
        event.raw.p0 = p0;
        event.raw.p1 = p1;
        event.raw.p2 = p2;
        return event;
    };

    rcp::intermediate_document doc;
    doc.header.tempo_bpm = 120;
    doc.tracks.resize(1);
    doc.tracks[0].events = {
        raw(60, 10, 10, 90),
        raw(0xeb, 0, 11, 127),
        raw(61, 1, 2, 80),
        raw(0xfe, 0, 0, 0)
    };

    std::vector<rcp::timed_track_event> timeline;
    std::string err;
    const bool same_tick_order_ok =
        rcp::expand_intermediate_timeline(doc, timeline, err) &&
        timeline.size() == 5 &&
        timeline[0].event.kind == rcp::timeline_event_kind::note &&
        timeline[0].event.note.note == 60 && timeline[0].event.tick == 0 &&
        timeline[1].event.kind == rcp::timeline_event_kind::note_off &&
        timeline[1].event.tick == 10 && timeline[1].event.note.note == 60 &&
        timeline[2].event.kind == rcp::timeline_event_kind::control_change &&
        timeline[2].event.tick == 10 && timeline[2].event.channel.data1 == 11 &&
        timeline[3].event.kind == rcp::timeline_event_kind::note &&
        timeline[3].event.tick == 10 && timeline[3].event.note.note == 61 &&
        timeline[4].event.kind == rcp::timeline_event_kind::note_off &&
        timeline[4].event.tick == 12 && timeline[4].event.note.note == 61 &&
        err.empty();

    std::printf("RCP2 同一 tick の順序（Note Off→CC→次の Note On）: %s\n",
                same_tick_order_ok ? "合" : "NG");

    doc.tracks[0].events = {
        raw(60, 10, 10, 90),
        raw(0xeb, 0, 7, 97),
        raw(0xeb, 0, 32, 0),
        raw(0xfe, 0, 0, 0)
    };
    const bool expired_off_before_controls_ok =
        rcp::expand_intermediate_timeline(doc, timeline, err) &&
        timeline.size() == 4 &&
        timeline[0].event.kind == rcp::timeline_event_kind::note &&
        timeline[0].event.tick == 0 &&
        timeline[1].event.kind == rcp::timeline_event_kind::note_off &&
        timeline[1].event.tick == 10 &&
        timeline[2].event.kind == rcp::timeline_event_kind::control_change &&
        timeline[2].event.tick == 10 && timeline[2].event.channel.data1 == 7 &&
        timeline[2].event.channel.data2 == 97 &&
        timeline[3].event.kind == rcp::timeline_event_kind::control_change &&
        timeline[3].event.tick == 10 && timeline[3].event.channel.data1 == 32 &&
        timeline[3].event.channel.data2 == 0 && err.empty();
    std::printf("RCP2 同一 tick の順序（Note Off→CC7→CC32）: %s\n",
                expired_off_before_controls_ok ? "合" : "NG");
}


static void check_rcp_fc_repeats()
{
    auto event = [](u8 code, u8 p0, u8 p1, u8 p2) {
        rcp::sequence_event raw;
        raw.code = code; raw.p0 = p0; raw.p1 = p1; raw.p2 = p2;
        return raw;
    };
    auto offset_bytes = [](size_t record_index) { return size_t(0x2c) + record_index * 4; };
    auto fc_to = [&](size_t target_index) {
        const size_t offset = offset_bytes(target_index);
        return event(0xfc, 0, u8(offset & 0xff), u8((offset >> 8) & 0xff));
    };

    std::vector<rcp::sequence_event> sequence = {
        event(60, 1, 4, 90), event(62, 1, 4, 80), fc_to(1),
        event(64, 1, 4, 70), event(0xfe, 0, 0, 0)
    };
    std::vector<rcp::timed_event> timeline;
    std::string err;
    const bool valid_repeat_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 4 &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 &&
        timeline[1].note.note == 62 && timeline[1].tick == 1 &&
        timeline[2].note.note == 62 && timeline[2].tick == 2 &&
        timeline[3].note.note == 64 && timeline[3].tick == 3 && err.empty();

    // FC exactly at the target tail-calls to its own target without nesting
    // the return address; the caller resumes at the record after its FC.
    sequence = {
        event(60, 1, 4, 90), fc_to(2), fc_to(0),
        event(64, 1, 4, 70), event(0xfe, 0, 0, 0)
    };
    const bool tail_call_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 3 &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 &&
        timeline[1].note.note == 60 && timeline[1].tick == 1 &&
        timeline[2].note.note == 64 && timeline[2].tick == 2 && err.empty();

    // FD inside an FC-referenced span returns to the caller, matching the
    // reference converter's parentPos behavior.
    sequence = {
        event(60, 1, 4, 90), fc_to(3), event(62, 1, 4, 80),
        event(64, 1, 4, 70), event(0xfd, 0, 0, 0), event(0xfe, 0, 0, 0)
    };
    const bool fd_returns_from_reference_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 4 &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 &&
        timeline[1].note.note == 64 && timeline[1].tick == 1 &&
        timeline[2].note.note == 62 && timeline[2].tick == 2 &&
        timeline[3].note.note == 64 && timeline[3].tick == 3 && err.empty();

    // FD outside an FC reference must leave following records untouched.
    sequence = {
        event(60, 1, 4, 90), event(0xfd, 0, 0, 0),
        event(62, 1, 4, 80), event(0xfe, 0, 0, 0)
    };
    const bool fd_outside_reference_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 2 &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 &&
        timeline[1].note.note == 62 && timeline[1].tick == 1 && err.empty();

    // A later FC marks the end of the referenced span and returns to the
    // record immediately after the initiating FC.
    sequence = {
        event(60, 1, 4, 90), event(62, 1, 4, 80), fc_to(0),
        event(64, 1, 4, 70), event(0xfe, 0, 0, 0)
    };
    const bool endpoint_return_ok =
        rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.size() == 5 &&
        timeline[0].note.note == 60 && timeline[0].tick == 0 &&
        timeline[1].note.note == 62 && timeline[1].tick == 1 &&
        timeline[2].note.note == 60 && timeline[2].tick == 2 &&
        timeline[3].note.note == 62 && timeline[3].tick == 3 &&
        timeline[4].note.note == 64 && timeline[4].tick == 4 && err.empty();

    sequence = {event(60, 1, 4, 90), event(0xfc, 0, 0, 0), event(0xfe, 0, 0, 0)};
    const bool header_target_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    sequence = {event(60, 1, 4, 90), event(0xfc, 0, 45, 0), event(0xfe, 0, 0, 0)};
    const bool unaligned_target_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    sequence = {event(60, 1, 4, 90), event(0xfc, 0, 0xff, 0x7f), event(0xfe, 0, 0, 0)};
    const bool outside_target_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    sequence = {event(60, 1, 4, 90), fc_to(1), event(0xfe, 0, 0, 0)};
    const bool cycle_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    sequence = {event(60, 1, 4, 90), fc_to(2), event(0xfe, 0, 0, 0)};
    const bool fe_target_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    // FC's traversal budget must fail closed on an oversized sequence,
    // without returning a partial timeline. This is separate from F8/F9 budget coverage.
    sequence.assign(1000001, event(60, 0, 1, 90));
    sequence.push_back(event(0xfe, 0, 0, 0));
    const bool fc_budget_rejected =
        !rcp::expand_note_key_signature_tempo_timeline(sequence, 120, timeline, err) &&
        timeline.empty() && !err.empty();

    std::printf("RCP2 FC 小節参照: 有効参照/時刻 %s・FC 連鎖 %s・FD 復帰/通常境界 %s・参照終端から復帰 %s・ヘッダー内/未整列/範囲外拒否 %s・FE 参照拒否 %s・循環拒否 %s・展開予算超過拒否 %s\n",
        valid_repeat_ok ? "合" : "NG",
        tail_call_ok ? "合" : "NG",
        (fd_returns_from_reference_ok && fd_outside_reference_ok) ? "合" : "NG",
        endpoint_return_ok ? "合" : "NG",
        (header_target_rejected && unaligned_target_rejected && outside_target_rejected) ? "合" : "NG",
        fe_target_rejected ? "合" : "NG",
        cycle_rejected ? "合" : "NG",
        fc_budget_rejected ? "合" : "NG");
}

static void check_rcp_tempo_conversion()
{
    u32 tempo = 123;
    std::string err = "stale error";
    const bool standard_ok =
        rcp::rcp2_tempo_us_per_quarter(120, 64, tempo, err) &&
        tempo == 500000 && err.empty();
    const bool half_scale_ok =
        rcp::rcp2_tempo_us_per_quarter(120, 32, tempo, err) &&
        tempo == 1000000 && err.empty();

    tempo = 123;
    const bool zero_bpm_rejected =
        !rcp::rcp2_tempo_us_per_quarter(0, 64, tempo, err) &&
        tempo == 0 && !err.empty();
    tempo = 123;
    const bool zero_scale_rejected =
        !rcp::rcp2_tempo_us_per_quarter(120, 0, tempo, err) &&
        tempo == 0 && !err.empty();

    std::printf("RCP2 E7 テンポ計算（実験）: 120/64=%s・120/32=%s・BPM 0 拒否 %s・scale 0 拒否 %s\n",
        standard_ok ? "合" : "NG", half_scale_ok ? "合" : "NG",
        zero_bpm_rejected ? "合" : "NG", zero_scale_rejected ? "合" : "NG");
}


// The complete core conversion used by the UI player, exercised without UI or ROMs.
static void check_rcp2_e6_note_off_route()
{
    const char signature[] = "RCM-PC98V2.0(C)COME ON MUSIC\r\n";
    const size_t header_size = 0x586;
    const size_t track_header_size = 0x2c;
    const size_t track_length = track_header_size + 16;
    std::vector<u8> data(header_size + track_length, 0);
    std::memcpy(data.data(), signature, sizeof(signature) - 1);
    data[0x1c0] = 0xe0;
    data[0x1c1] = 120;
    data[0x1e6] = 1;
    data[0x1e7] = 1;
    const size_t at = header_size;
    data[at] = u8(track_length);
    data[at + 1] = u8(track_length >> 8);
    const size_t seq = at + track_header_size;
    data[seq + 0] = 60;  data[seq + 1] = 10; data[seq + 2] = 20; data[seq + 3] = 90;
    data[seq + 4] = 0xe6; data[seq + 5] = 0; data[seq + 6] = 0x13; data[seq + 7] = 0;
    data[seq + 8] = 62;  data[seq + 9] = 0; data[seq + 10] = 5; data[seq + 11] = 80;
    data[seq + 12] = 0xfe;

    rcp::intermediate_document document;
    std::vector<rcp::timed_track_event> timeline;
    std::vector<rcp::timed_seconds_event> seconds;
    std::vector<rcp::track_midi_mapping> mapping;
    std::vector<rcp::midi_output_event> output;
    std::string err;
    const bool built = rcp::build_intermediate(data.data(), data.size(), document, err);
    const bool expanded = built && rcp::expand_intermediate_timeline(document, timeline, err);
    const bool timed = expanded && rcp::convert_timeline_to_seconds(document.header, timeline, seconds, err);
    const bool mapped = timed && rcp::map_track_midi_outputs(document, mapping, err);
    const bool encoded = mapped && rcp::encode_timeline_to_midi_events(seconds, mapping, output, err);
    const bool route_ok = encoded && output.size() == 4 &&
        output[0].bytes == std::vector<u8>({0x90, 60, 90}) && output[0].port == 0 &&
        output[1].bytes == std::vector<u8>({0x92, 62, 80}) && output[1].port == 1 &&
        output[2].bytes.size() == 3 && output[2].bytes[1] == 62 &&
        (output[2].bytes[0] == 0x82 || output[2].bytes[0] == 0x92) && output[2].port == 1 &&
        output[3].bytes.size() == 3 && output[3].bytes[1] == 60 &&
        (output[3].bytes[0] == 0x80 || output[3].bytes[0] == 0x90) && output[3].port == 0 &&
        err.empty();
    rcp_e6_note_off_route_test_ok = rcp_e6_note_off_route_test_ok && route_ok;
    std::printf("RCP2 E6 Note Off 回帰: 解析/展開/秒変換/MIDI %s・Note Off 発音時ポート維持 %s\n",
        encoded ? "合" : "NG", route_ok ? "合" : "NG");
    if (!encoded) std::printf("RCP2 E6 route test error: %s\n", err.c_str());
}


// The complete core conversion used by the UI player, exercised without UI or ROMs.
static void check_rcp2_player_event_pipeline()
{
    const char signature[] = "RCM-PC98V2.0(C)COME ON MUSIC\r\n";
    const size_t header_size = 0x586;
    const size_t track_header_size = 0x2c;
    // One initial note followed by a two-pass F9/F8 loop. The loop note's
    // gate is shorter than its step, so the full pipeline must emit two strikes
    // and two note-offs rather than treating them as a same-pitch tie.
    const size_t track_length = track_header_size + 20;
    std::vector<u8> data(header_size + track_length, 0);
    std::memcpy(data.data(), signature, sizeof(signature) - 1);
    data[0x1c0] = 0xe0;
    data[0x1c1] = 100;
    data[0x1e6] = 1;
    data[0x1e7] = 1;
    const size_t at = header_size;
    data[at] = u8(track_length);
    data[at + 1] = u8(track_length >> 8);
    data[at + 2] = 0;
    data[at + 4] = 0;
    const size_t seq = at + track_header_size;
    data[seq + 0] = 60;  data[seq + 1] = 12; data[seq + 2] = 8; data[seq + 3] = 90;
    data[seq + 4] = 0xf9;
    data[seq + 8] = 62;  data[seq + 9] = 12; data[seq + 10] = 8; data[seq + 11] = 80;
    data[seq + 12] = 0xf8; data[seq + 13] = 2;
    data[seq + 16] = 0xfe;

    rcp::intermediate_document document;
    std::vector<rcp::timed_track_event> timeline;
    std::vector<rcp::timed_seconds_event> seconds;
    std::vector<rcp::track_midi_mapping> mapping;
    std::vector<rcp::midi_output_event> output;
    std::string err;
    const bool built = rcp::build_intermediate(data.data(), data.size(), document, err);
    const bool expanded = built && rcp::expand_intermediate_timeline(document, timeline, err);
    const bool timed = expanded && rcp::convert_timeline_to_seconds(document.header, timeline, seconds, err);
    const bool routed = timed && rcp::map_track_midi_outputs(document, mapping, err);
    const bool encoded = routed && rcp::encode_timeline_to_midi_events(seconds, mapping, output, err);
    const bool note_on = encoded && std::any_of(output.begin(), output.end(), [](const rcp::midi_output_event &e) {
        return e.bytes == std::vector<u8>({0x90, 60, 90}) && e.port == 0 && e.time >= 0.0;
    });
    const bool note_off = encoded && std::any_of(output.begin(), output.end(), [](const rcp::midi_output_event &e) {
        return e.bytes.size() == 3 && (e.bytes[0] == 0x80 || (e.bytes[0] == 0x90 && e.bytes[2] == 0)) && e.bytes[1] == 60 && e.port == 0;
    });
    size_t loop_note_ons = 0, loop_note_offs = 0;
    bool loop_times_ok = true;
    for (const rcp::midi_output_event &e : output) {
        if (e.bytes == std::vector<u8>({0x90, 62, 80}) && e.port == 0) {
            ++loop_note_ons;
            const double expected = loop_note_ons == 1 ? 0.015 : 0.030;
            if (e.time < expected - 0.000001 || e.time > expected + 0.000001)
                loop_times_ok = false;
        }
        if ((e.bytes == std::vector<u8>({0x80, 62, 0}) || e.bytes == std::vector<u8>({0x90, 62, 0})) && e.port == 0) {
            ++loop_note_offs;
            const double expected = loop_note_offs == 1 ? 0.025 : 0.040;
            if (e.time < expected - 0.000001 || e.time > expected + 0.000001)
                loop_times_ok = false;
        }
    }
    const bool loop_pipeline_ok = loop_note_ons == 2 && loop_note_offs == 2 && loop_times_ok;
    std::printf("RCP2 プレイヤー経路（解析→秒→MIDI）: 解析 %s・展開 %s・時刻 %s・割当 %s・変換 %s・Note On/Off %s・F8/F9 順序/時刻 %s\n",
        built ? "合" : "NG", expanded ? "合" : "NG", timed ? "合" : "NG", routed ? "合" : "NG",
        encoded ? "合" : "NG", (note_on && note_off) ? "合" : "NG", loop_pipeline_ok ? "合" : "NG");
    if (!encoded) std::printf("RCP2 player pipeline error: %s\n", err.c_str());

    // Exercise the actual public player API, not just the conversion helpers.
    // A valid synthetic RCP2 must be appended with converted events; an
    // unsupported command must fail without appending a partial song.
    ui::player api_player;
    int api_index = -1;
    std::string api_err = "stale error";
    const bool api_valid_accepted =
        api_player.add_from_memory(data.data(), data.size(), "synthetic.rcp", api_err, &api_index);
    const auto api_entries_after_valid = api_player.list();
    const bool api_valid_ok = api_valid_accepted && api_err.empty() && api_index == 0 &&
        api_entries_after_valid.size() == 1 &&
        api_entries_after_valid[0].path.empty() &&
        !api_entries_after_valid[0].evs.empty();

    std::vector<u8> invalid_data = data;
    invalid_data[seq + 4] = 0x80; // syntactically valid record, unsupported by timeline expansion
    int rejected_index = 77;
    api_err.clear();
    const bool api_invalid_rejected =
        !api_player.add_from_memory(invalid_data.data(), invalid_data.size(),
                                    "unsupported.rcp", api_err, &rejected_index);
    const auto api_entries_after_invalid = api_player.list();
    const bool api_no_partial_append = api_invalid_rejected && !api_err.empty() &&
        rejected_index == 77 && api_entries_after_invalid.size() == 1 &&
        api_entries_after_invalid[0].evs.size() == api_entries_after_valid[0].evs.size();

    rcp_player_api_test_ok = api_valid_ok && api_no_partial_append;
    // Keep successful verify output stable for tests/verify.txt; emit details only on failure.
    if (!rcp_player_api_test_ok)
        std::printf("RCP2 player::add_from_memory API failed: valid=%s, reject-without-partial-append=%s; error=%s\n",
            api_valid_ok ? "ok" : "NG", api_no_partial_append ? "ok" : "NG", api_err.c_str());
}


// Read real RCP2 files on demand without placing private song data in the repository.
// Usage: verify --rcp2-sample|--rcp2-event-dump|--rcp2-raw-record-dump <file.rcp> [another.rcp ...]
static int check_rcp2_sample_file(const char *path, bool dump_event_trace = false,
                                  bool dump_raw_records = false)
{
	bool event_dump_failed = false;
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		std::fprintf(stderr, "RCP2 sample: cannot open: %s\n", path);
		return 1;
	}
	std::vector<u8> bytes((std::istreambuf_iterator<char>(file)),
	                      std::istreambuf_iterator<char>());
	if (bytes.empty()) {
		std::fprintf(stderr, "RCP2 sample: empty file: %s\n", path);
		return 1;
	}

	rcp::header_info info;
	std::string err;
	if (!rcp::inspect_header(bytes.data(), bytes.size(), info, err)) {
		std::fprintf(stderr, "RCP2 sample: header failed: %s: %s\n", path, err.c_str());
		return 1;
	}
	std::vector<rcp::track> tracks;
	if (!rcp::parse_tracks(bytes.data(), bytes.size(), tracks, err)) {
		std::fprintf(stderr, "RCP2 sample: parse_tracks failed: %s: %s\n", path, err.c_str());
		return 1;
	}
	rcp::intermediate_document document;
	if (!rcp::build_intermediate(bytes.data(), bytes.size(), document, err)) {
		std::fprintf(stderr, "RCP2 sample: build_intermediate failed: %s: %s\n", path, err.c_str());
		return 1;
	}

	std::printf("RCP2 sample: %s (%zu bytes) OK; parsed_tracks=%zu; intermediate_tracks=%zu\n",
	            path, bytes.size(), tracks.size(), document.tracks.size());

	// Opt-in source-record trace. This deliberately reports raw record provenance
	// without guessing command timing or changing the normal parser/timeline path.
	if (dump_raw_records) {
		size_t track_file_offset = info.minimum_header_size;
		std::printf("  raw_record_dump: begin; fields=track,record,file_offset,code,p0,p1,p2\n");
		for (size_t track_index = 0; track_index < tracks.size(); ++track_index) {
			const rcp::track &track = tracks[track_index];
			const size_t sequence_offset = track_file_offset + 0x2c;
			for (size_t record_index = 0; record_index < track.sequence.size(); ++record_index) {
				const rcp::sequence_event &raw = track.sequence[record_index];
				const size_t file_offset = sequence_offset + record_index * 4;
				std::printf("  raw_record: track=%02zu record=%zu file_offset=0x%zx code=%02X p0=%02X p1=%02X p2=%02X\n",
				            track_index, record_index, file_offset,
				            unsigned(raw.code), unsigned(raw.p0),
				            unsigned(raw.p1), unsigned(raw.p2));
			}
			track_file_offset += track.header.encoded_length;
		}
		std::printf("  raw_record_dump: end\n");
	}

	// Diagnostic-only end-to-end probe: keep structural parsing success separate
	// from timeline/MIDI support. A failure here is reported per sample but does
	// not make the raw-parser sample check fail.
	std::vector<rcp::timed_track_event> timeline;
	if (!rcp::expand_intermediate_timeline(document, timeline, err)) {
		std::printf("  timeline: FAIL; %s\n", err.c_str());
	} else {
		std::printf("  timeline: OK; events=%zu\n", timeline.size());
		std::vector<rcp::timed_seconds_event> seconds_timeline;
		if (!rcp::convert_timeline_to_seconds(document.header, timeline, seconds_timeline, err)) {
			std::printf("  seconds: FAIL; %s\n", err.c_str());
		} else {
			std::printf("  seconds: OK; events=%zu\n", seconds_timeline.size());
			std::vector<rcp::track_midi_mapping> mapping;
			std::vector<rcp::midi_output_event> midi_events;
			if (!rcp::map_track_midi_outputs(document, mapping, err)) {
				std::printf("  midi_mapping: FAIL; %s\n", err.c_str());
			} else if (!rcp::encode_timeline_to_midi_events(
			               seconds_timeline, mapping, midi_events, err)) {
				std::printf("  midi_encode: FAIL; %s\n", err.c_str());
			} else {
				std::printf("  midi_encode: OK; events=%zu\n", midi_events.size());
				if (dump_event_trace) {
					// Emit a stable, line-oriented trace for local comparison. This is
					// intentionally opt-in so normal sample diagnostics stay compact.
					std::vector<rcp::track_midi_mapping> active_mapping = mapping;
					size_t dumped_event_count = 0;
					bool event_dump_ok = true;
					std::printf("  event_dump: begin; fields=index,track,tick,time_seconds,port,midi_bytes\n");
					for (const rcp::timed_seconds_event &source : seconds_timeline) {
						const rcp::timed_event &event = source.source.event;
						if (event.kind == rcp::timeline_event_kind::tempo ||
						    event.kind == rcp::timeline_event_kind::key_signature)
							continue;
						const size_t track_index = source.source.track_index;
						if (track_index >= active_mapping.size())
							continue; // Already rejected by midi_encode; defensive only.
						if (event.kind == rcp::timeline_event_kind::route_change) {
							active_mapping[track_index].channel = event.route.channel;
							active_mapping[track_index].port = event.route.port;
							active_mapping[track_index].enabled = event.route.enabled;
							continue;
						}
						if (!active_mapping[track_index].enabled)
							continue;
						std::vector<u8> midi_bytes;
						if (event.kind == rcp::timeline_event_kind::sysex) {
							midi_bytes = event.sysex;
						} else if (!rcp::encode_midi_message(
						               event, active_mapping[track_index].channel, midi_bytes, err)) {
							std::printf("  event_dump: FAIL; %s\n", err.c_str());
							event_dump_ok = false;
							break;
						}
						std::printf("  event: index=%zu track=%02zu tick=%010u time=%.9f port=%02X bytes=",
						            dumped_event_count, track_index, unsigned(event.tick), source.time,
						            unsigned(active_mapping[track_index].port));
						for (size_t byte_index = 0; byte_index < midi_bytes.size(); ++byte_index)
							std::printf("%s%02X", byte_index == 0 ? "" : " ",
							            unsigned(midi_bytes[byte_index]));
						std::printf("\n");
						++dumped_event_count;
					}
					const bool event_count_ok = event_dump_ok && dumped_event_count == midi_events.size();
					std::printf("  event_dump: count=%zu encoded_count=%zu consistency=%s\n",
					            dumped_event_count, midi_events.size(), event_count_ok ? "OK" : "FAIL");
					std::printf("  event_dump: end\n");
					if (!event_count_ok) {
						event_dump_failed = true;
						std::fprintf(stderr, "RCP2 event dump: encoded/dumped event count mismatch: %s\n", path);
					}
				}
			}
		}
	}
	size_t command_counts[256] = {};
	for (size_t i = 0; i < tracks.size(); ++i) {
		const auto &track = tracks[i];
		size_t fc = 0, f8 = 0, f9 = 0, fe = 0;
		for (const auto &event : track.sequence) {
			++command_counts[event.code];
			fc += event.code == 0xfc;
			f8 += event.code == 0xf8;
			f9 += event.code == 0xf9;
			fe += event.code == 0xfe;
		}
		std::printf("  track[%zu] name=%s records=%zu FC=%zu F8=%zu F9=%zu FE=%zu\n",
		            i, track.header.name.c_str(), track.sequence.size(), fc, f8, f9, fe);
		// Raw parsing intentionally accepts command combinations without assigning
		// semantics. When F8/F9 counts differ, report exact marker positions and
		// use a stack to identify unmatched starts/ends without changing parsing.
		if (f8 != f9) {
			std::printf("    F8/F9 detail (record:code p0 p1 p2):");
			for (size_t j = 0; j < track.sequence.size(); ++j) {
				const auto &event = track.sequence[j];
				if (event.code == 0xf8 || event.code == 0xf9)
					std::printf(" %zu:%02X %02X %02X %02X", j,
					            unsigned(event.code), unsigned(event.p0),
					            unsigned(event.p1), unsigned(event.p2));
			}
			std::printf("\n");
			std::vector<size_t> open_f9;
			for (size_t j = 0; j < track.sequence.size(); ++j) {
				if (track.sequence[j].code == 0xf9) {
					open_f9.push_back(j);
				} else if (track.sequence[j].code == 0xf8) {
					if (open_f9.empty()) {
						std::printf("    unmatched F8 record=%zu p0=%02X\n",
						            j, unsigned(track.sequence[j].p0));
					} else {
						open_f9.pop_back();
					}
				}
			}
			for (size_t j : open_f9)
				std::printf("    unmatched F9 record=%zu p0=%02X\n",
				            j, unsigned(track.sequence[j].p0));
		}
		// F6 comments and 0x98/0x99 multi-record commands may own immediately
		// following F7 continuation records. Keep this as diagnostics only:
		// unconfirmed semantics must not cause sample rejection.
		for (size_t j = 0; j < track.sequence.size(); ++j) {
			const u8 code = track.sequence[j].code;
			if (code == 0xf6 || code == 0x98 || code == 0x99) {
				size_t end = j + 1;
				while (end < track.sequence.size() && track.sequence[end].code == 0xf7)
					++end;
				if (end > j + 1)
					std::printf("    %02X/F7 chain start=%zu records=%zu next=%s\n",
					            unsigned(code), j, end - j, end < track.sequence.size() ?
					            (track.sequence[end].code == 0xfe ? "FE" : "other") : "EOF");
				j = end - 1;
			} else if (code == 0xf7 &&
			           (j == 0 || (track.sequence[j - 1].code != 0xf6 &&
			                       track.sequence[j - 1].code != 0x98 &&
			                       track.sequence[j - 1].code != 0x99 &&
			                       track.sequence[j - 1].code != 0xf7))) {
				std::printf("    orphan F7 record=%zu p0=%02X p1=%02X p2=%02X\n",
				            j, unsigned(track.sequence[j].p0),
				            unsigned(track.sequence[j].p1), unsigned(track.sequence[j].p2));
				const size_t begin = j > 3 ? j - 3 : 0;
				const size_t finish = std::min(track.sequence.size(), j + 4);
				std::printf("      context (record:code p0 p1 p2):");
				for (size_t k = begin; k < finish; ++k) {
					const auto &near = track.sequence[k];
					std::printf(" %zu:%02X %02X %02X %02X", k,
					            unsigned(near.code), unsigned(near.p0),
					            unsigned(near.p1), unsigned(near.p2));
				}
				std::printf("\n");
			}
		}
	}
	// Per-track probe complements the document-level result above: one unsupported
	// command should not hide the independent status of the remaining real tracks.
	for (size_t track_index = 0; track_index < document.tracks.size(); ++track_index) {
		rcp::intermediate_document single_track;
		single_track.header = document.header;
		single_track.tracks.push_back(document.tracks[track_index]);
		std::vector<rcp::timed_track_event> track_timeline;
		err.clear();
		if (!rcp::expand_intermediate_timeline(single_track, track_timeline, err)) {
			std::printf("  track_pipeline[%zu] name=%s: timeline FAIL; %s\n",
			            track_index, document.tracks[track_index].header.name.c_str(), err.c_str());
			continue;
		}
		std::vector<rcp::timed_seconds_event> track_seconds;
		if (!rcp::convert_timeline_to_seconds(single_track.header, track_timeline, track_seconds, err)) {
			std::printf("  track_pipeline[%zu] name=%s: timeline OK events=%zu; seconds FAIL; %s\n",
			            track_index, document.tracks[track_index].header.name.c_str(),
			            track_timeline.size(), err.c_str());
			continue;
		}
		std::vector<rcp::track_midi_mapping> track_mapping;
		std::vector<rcp::midi_output_event> track_midi;
		if (!rcp::map_track_midi_outputs(single_track, track_mapping, err)) {
			std::printf("  track_pipeline[%zu] name=%s: timeline OK events=%zu; seconds OK events=%zu; mapping FAIL; %s\n",
			            track_index, document.tracks[track_index].header.name.c_str(),
			            track_timeline.size(), track_seconds.size(), err.c_str());
			continue;
		}
		if (!rcp::encode_timeline_to_midi_events(track_seconds, track_mapping, track_midi, err)) {
			std::printf("  track_pipeline[%zu] name=%s: timeline OK events=%zu; seconds OK events=%zu; mapping OK; MIDI FAIL; %s\n",
			            track_index, document.tracks[track_index].header.name.c_str(),
			            track_timeline.size(), track_seconds.size(), err.c_str());
			continue;
		}
		std::printf("  track_pipeline[%zu] name=%s: timeline OK events=%zu; seconds OK events=%zu; mapping OK; MIDI OK events=%zu\n",
		            track_index, document.tracks[track_index].header.name.c_str(),
		            track_timeline.size(), track_seconds.size(), track_midi.size());
	}
	// E7 p1/p2 combinations are reported separately so real-sample diagnostics
	// can show whether gradual-tempo parameters occur without guessing their meaning.
	std::map<std::pair<unsigned, unsigned>, size_t> tempo_parameter_counts;
	for (const auto &track : document.tracks) {
		for (const auto &event : track.events) {
			if (event.raw.code == 0xe7)
				++tempo_parameter_counts[{event.raw.p1, event.raw.p2}];
		}
	}
	if (!tempo_parameter_counts.empty()) {
		std::printf("  E7 p1/p2 pairs:");
		for (const auto &entry : tempo_parameter_counts)
			std::printf(" %02X/%02X=%zu", entry.first.first,
			            entry.first.second, entry.second);
		std::printf("\n");
		size_t gradual_tempo_count = 0;
		for (const auto &entry : tempo_parameter_counts) {
			if (entry.first.second != 0)
				gradual_tempo_count += entry.second;
		}
		std::printf("  E7 gradual tempo: %zu event(s); interpolation not implemented\n",
		            gradual_tempo_count);
	}
	std::printf("  command histogram:");
	for (size_t code = 0; code < 256; ++code) {
		if (command_counts[code] != 0)
			std::printf(" %02X=%zu", static_cast<unsigned>(code), command_counts[code]);
	}
	std::printf("\n");
	return event_dump_failed ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc >= 3 && (std::strcmp(argv[1], "--rcp2-sample") == 0 ||
	                  std::strcmp(argv[1], "--rcp2-event-dump") == 0 ||
	                  std::strcmp(argv[1], "--rcp2-raw-record-dump") == 0)) {
		const bool dump_event_trace = std::strcmp(argv[1], "--rcp2-event-dump") == 0;
		const bool dump_raw_records = std::strcmp(argv[1], "--rcp2-raw-record-dump") == 0;
		int failures = 0;
		for (int i = 2; i < argc; ++i)
			failures += check_rcp2_sample_file(argv[i], dump_event_trace, dump_raw_records);
		return failures == 0 ? 0 : 1;
	}
	std::vector<u8>  wave(64 * 1024 * 1024, 0);   // 波形 ROM 相当のダミー
	std::vector<u16> sintab(0x8000, 0);

	swp30_device swp;
	swp.set_wave_rom(wave.data(), wave.size());
	swp.set_sintab(sintab.data(), sintab.size());
	swp.reset();

	// レジスタの読み書きが素通しできるか（ピッチ = slot 0x11）
	swp.write16(0 * 0x40 + 0x11, 0x1234);
	u16 back = swp.read16(0 * 0x40 + 0x11);
	std::printf("ピッチレジスタ 書き 0x1234 -> 読み 0x%04x  %s\n",
	            back, back == 0x1234 ? "一致" : "不一致");

	// 1 サンプル回してみる（無音のはず）
	s32 l = 0, r = 0;
	for (int i = 0; i < 100; i++) swp.run_sample(l, r);
	std::printf("100 サンプル実行  最後の出力 L=%d R=%d\n", l, r);

	// 乱数が MAME と同じ数列か
	// printf の引数は評価順が未規定。1 個ずつ取り出さないと順序が入れ替わる
	std::printf("乱数 1〜3 個目:");
	for (int i = 0; i < 3; i++) std::printf(" %08x", swp.machine().rand());
	std::printf("\n");

	// MEG の JIT が機械語で書いたリバーブ RAM の詰め方・戻し方と係数の広げ方（m1_expand）が、C++ の関数と全部の入力で同じか
	std::printf("MEG の JIT のリバーブ RAM の詰め方と戻し方・係数の広げ方: 食い違い %llu\n",
	            (unsigned long long)swp30_device::meg_jit_selftest());

	// aarch64 emitter selftest: every primitive the arm64 JIT uses, executed
	// on this CPU and compared against plain C++ (a64asm.cpp; 0 on x86-64 builds)
	std::printf("a64 emitter selftest: %llu mismatch(es)\n", (unsigned long long)a64::selftest());

	check_bend_thinner();
	check_smf_player();
	check_rcp_header();
	check_rcp_sequence();
	check_rcp_note_decode();
	check_rcp_note_timeline();
	check_rcp_tempo_conversion();
	check_rcp_tempo_event_decode();
	check_rcp_tempo_timeline();
	check_rcp_counted_loops();
	check_rcp_same_tick_order();
	check_rcp_fc_repeats();
	check_rcp_track_merge();
	check_rcp_document_timeline();
	check_rcp_aftertouch_commands();
	check_rcp_midi_encoding();
	check_rcp_tick_to_seconds();
	check_rcp_timeline_output_encoding();
	check_rcp_track_midi_mapping();
	check_rcp_tracks();
	check_rcp_intermediate();
	check_rcp2_e6_note_off_route();
	check_rcp2_player_event_pipeline();
	check_font_parses();

	// Roland の液晶のデータ（F0 41 10 45 12）だけを抜き、GS リセット（F0 41 10 42 12）と XG（F0 43）は通す
	{
		ui::bend_thinner th;
		int passed = 0;
		auto send = [&passed](int, const u8 *, size_t) { passed++; };
		const u8 disp[] = { 0xf0, 0x41, 0x10, 0x45, 0x12, 0x10, 0x00, 0x00, 0x48, 0x49, 0x0f, 0xf7 };
		const u8 gs[]   = { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
		const u8 xg[]   = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 };
		th.event(0, disp, sizeof(disp), 0.0, send);
		th.event(0, gs, sizeof(gs), 0.0, send);
		th.event(0, xg, sizeof(xg), 0.0, send);
		std::printf("Roland の液晶のデータを抜く: 抜いた %zu・通した %d（GS リセットと XG）\n", th.dropped(), passed);
	}
	return (rcp_mute_flags_test_ok && rcp_player_api_test_ok && rcp_loop_budget_test_ok && rcp_e6_note_off_route_test_ok) ? 0 : 1;
}