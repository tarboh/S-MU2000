// license:BSD-3-Clause
//
// CLAP プラグインを DAW 無しで鳴らしてみる小さなホスト。
//
//   clapprobe <S-MU2000.clap> <MIDI ファイル> <出力 wav> [--rate 48000] [--block 512] [--adc-silence]
//
// vst3probe と同じ MIDI・同じ標本化周波数・同じブロックの長さで流し、同じ形の wav を書く。
// 両方の wav を比べれば、CLAP の口で音が変わっていないかが分かる。
//
// --adc-silence は A/D INPUT（音声入力の口）に無音を渡す。付けなければ入力の口は繋がない。
// --fx はエフェクトとしての口（S-MU2000 FX。issue #191）を作り、A/D INPUT に 440Hz の正弦を流す。入力がメインの口で、
//   MIDI で何も設定しなくても入れた音が出てくるかを見る
// REAPER などは CLAP の入力の口をトラックのチャンネルに繋ぐので、その形を真似る
//
//   clapprobe <S-MU2000.clap> --automation
//
// XG の値のパラメータ（src/vst3/automation.h）を試す: 一覧、値を送って音源に入ったか（process と flush）、
// 状態の保存と復元、機械まるごとの状態を抜いて XG の値の控えだけで戻るか

#include "clap/clap.h"
#include "smf.h"

#include "compat/console.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// ---- ホスト。何も頼まれても何もしない
const void *host_ext(const clap_host_t *, const char *) { return nullptr; }
void host_nop(const clap_host_t *) {}

const clap_host_t kHost = {
	CLAP_VERSION_INIT, nullptr, "clapprobe", "S-MU2000", "", "0.1",
	host_ext, host_nop, host_nop, host_nop,
};

// ---- 入ってくるイベントの並び。MIDI は 3 バイトまで中に持ち、SysEx は外を指す
struct in_events {
	std::vector<clap_event_midi_t>       midi;
	std::vector<clap_event_midi_sysex_t> sysex;
	std::vector<clap_event_note_t>       notes;
	std::vector<const clap_event_header_t *> order;
	std::vector<std::pair<int, size_t>> arrival;    // 来た順（0 MIDI / 1 SysEx / 2 ノート）
	clap_input_events_t api;

	in_events()
	{
		api.ctx = this;
		api.size = [](const clap_input_events_t *l) -> uint32_t {
			return uint32_t(static_cast<in_events *>(l->ctx)->order.size());
		};
		api.get = [](const clap_input_events_t *l, uint32_t i) -> const clap_event_header_t * {
			const auto *self = static_cast<in_events *>(l->ctx);
			return i < self->order.size() ? self->order[i] : nullptr;
		};
	}
};

bool out_push(const clap_output_events_t *, const clap_event_header_t *) { return true; }

void write_wav(const std::string &path, const std::vector<int16_t> &pcm, uint32_t rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	const uint32_t bytes = uint32_t(pcm.size() * 2);
	auto u32w = [&](uint32_t v) { uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
	                              std::fwrite(b, 1, 4, f); };
	auto u16w = [&](uint16_t v) { uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) }; std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); u32w(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); u32w(16); u16w(1); u16w(2);
	u32w(rate); u32w(rate * 4); u16w(4); u16w(16);
	std::fwrite("data", 1, 4, f); u32w(bytes);
	std::fwrite(pcm.data(), 1, bytes, f);
	std::fclose(f);
}

} // namespace

// ---- XG の値のパラメータ（--automation）

struct byte_stream {
	std::vector<uint8_t> buf;
	size_t pos = 0;
	clap_ostream_t out{};
	clap_istream_t in{};
	byte_stream()
	{
		out.ctx = this;
		out.write = [](const clap_ostream_t *s, const void *p, uint64_t n) -> int64_t {
			auto *self = static_cast<byte_stream *>(s->ctx);
			const auto *b = static_cast<const uint8_t *>(p);
			self->buf.insert(self->buf.end(), b, b + n);
			return int64_t(n);
		};
		in.ctx = this;
		in.read = [](const clap_istream_t *s, void *p, uint64_t n) -> int64_t {
			auto *self = static_cast<byte_stream *>(s->ctx);
			const size_t k = std::min<size_t>(size_t(n), self->buf.size() - self->pos);
			std::memcpy(p, self->buf.data() + self->pos, k);
			self->pos += k;
			return int64_t(k);
		};
	}
};

struct param_events {
	std::vector<clap_event_param_value_t> ev;
	clap_input_events_t api{};
	param_events()
	{
		api.ctx = this;
		api.size = [](const clap_input_events_t *l) -> uint32_t {
			return uint32_t(static_cast<param_events *>(l->ctx)->ev.size());
		};
		api.get = [](const clap_input_events_t *l, uint32_t i) -> const clap_event_header_t * {
			auto *self = static_cast<param_events *>(l->ctx);
			return i < self->ev.size() ? &self->ev[i].header : nullptr;
		};
	}
	void add(clap_id id, double value)
	{
		clap_event_param_value_t e{};
		e.header = { sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
		e.param_id = id;
		e.note_id = -1;
		e.port_index = -1;
		e.channel = -1;
		e.key = -1;
		e.value = value;
		ev.push_back(e);
	}
};

// SysEx を 1 区間で流す
void send_sysex(const clap_plugin_t *plug, int block, const std::vector<std::vector<uint8_t>> &messages)
{
	const size_t nb = static_cast<size_t>(block);
	std::vector<float> bl(nb), br(nb);
	float *outs[2] = { bl.data(), br.data() };
	clap_audio_buffer_t obuf{};
	obuf.data32 = outs;
	obuf.channel_count = 2;
	std::vector<clap_event_midi_sysex_t> ev(messages.size());
	std::vector<const clap_event_header_t *> order;
	for (size_t i = 0; i < messages.size(); i++) {
		ev[i].header = { sizeof(ev[i]), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0 };
		ev[i].port_index = 0;
		ev[i].buffer = messages[i].data();
		ev[i].size = uint32_t(messages[i].size());
		order.push_back(&ev[i].header);
	}
	clap_input_events_t in{};
	in.ctx = &order;
	in.size = [](const clap_input_events_t *l) -> uint32_t {
		return uint32_t(static_cast<std::vector<const clap_event_header_t *> *>(l->ctx)->size());
	};
	in.get = [](const clap_input_events_t *l, uint32_t i) -> const clap_event_header_t * {
		return (*static_cast<std::vector<const clap_event_header_t *> *>(l->ctx))[i];
	};
	clap_output_events_t oev{ nullptr, out_push };
	clap_process_t pr{};
	pr.frames_count = uint32_t(block);
	pr.audio_outputs = &obuf;
	pr.audio_outputs_count = 1;
	pr.in_events = &in;
	pr.out_events = &oev;
	plug->process(plug, &pr);
}

void run_blocks(const clap_plugin_t *plug, double rate, int block, double secs, param_events *events, bool repeat)
{
	const size_t nb = static_cast<size_t>(block);
	std::vector<float> bl(nb), br(nb);
	float *outs[2] = { bl.data(), br.data() };
	clap_audio_buffer_t obuf{};
	obuf.data32 = outs;
	obuf.channel_count = 2;
	param_events none;
	clap_output_events_t oev{ nullptr, out_push };
	const int blocks = int(secs * rate / block);
	for (int k = 0; k < blocks; k++) {
		clap_process_t pr{};
		pr.frames_count = uint32_t(block);
		pr.audio_outputs = &obuf;
		pr.audio_outputs_count = 1;
		pr.in_events = (events && (repeat || k == 0)) ? &events->api : &none.api;
		pr.out_events = &oev;
		plug->process(plug, &pr);
	}
}

int run_automation(const clap_plugin_factory_t *fac, const char *id)
{
	const double rate = 48000.0;
	const int block = 512;
	int bad = 0;
	auto make = [&]() {
		const clap_plugin_t *p = fac->create_plugin(fac, &kHost, id);
		if (!p || !p->init(p))
			return static_cast<const clap_plugin_t *>(nullptr);
		return p;
	};
	const clap_plugin_t *a = make();
	if (!a) { std::printf("NG: 作れない\n"); return 1; }
	const auto *params = static_cast<const clap_plugin_params_t *>(a->get_extension(a, CLAP_EXT_PARAMS));
	const auto *state = static_cast<const clap_plugin_state_t *>(a->get_extension(a, CLAP_EXT_STATE));
	if (!params || !state) { std::printf("NG: params / state が無い\n"); return 1; }

	std::map<std::string, clap_id> ids;
	std::map<clap_id, int> seen;
	const uint32_t count = params->count(a);
	int xg = 0;
	for (uint32_t i = 0; i < count; i++) {
		clap_param_info_t info{};
		if (!params->get_info(a, i, &info)) { std::printf("NG: get_info %u\n", i); bad++; continue; }
		if (seen[info.id]++) { std::printf("NG: 番号 %u が重なっている\n", info.id); bad++; }
		if (info.id >= 65536) {
			xg++;
			ids[info.name] = info.id;
			if (i == 1 || i == count - 1)
				std::printf("  %s（%s）id %u  %g〜%g\n", info.name, info.module, info.id, info.min_value, info.max_value);
		}
	}
	std::printf("パラメータ %u 本（XG の値 %d 本）\n", count, xg);
	char text[64];
	for (auto [name, v] : { std::pair<const char *, double>{ "A1 Pan", 0 }, { "B3 EQ Bass Freq", 12 }, { "Master Tune", 0x400 - 25 } }) {
		params->value_to_text(a, ids[name], v, text, sizeof(text));
		double back = -1;
		params->text_to_value(a, ids[name], text, &back);
		std::printf("  %-16s %g → \"%s\" → %g\n", name, v, text, back);
		if (back != v) bad++;
	}

	if (!a->activate(a, rate, 1, block)) { std::printf("NG: activate\n"); return 1; }
	// 音を作っていないときの flush で入れる値
	struct target { const char *name; int value; };
	const target F[] = { { "A2 Resonance", 30 }, { "Master EQ Freq 5", 40 } };
	{
		param_events pe;
		for (const target &t : F)
			pe.add(ids[t.name], t.value);
		clap_output_events_t oev{ nullptr, out_push };
		params->flush(a, &pe.api, &oev);
	}
	a->start_processing(a);
	run_blocks(a, rate, block, 1.0, nullptr, false);
	// インサーション 1 を DISTORTION（1 バイトのパラメータ）、2 を DELAY LCR（2 バイト）にしておく
	send_sysex(a, block, { { 0xf0, 0x43, 0x10, 0x4c, 0x03, 0x00, 0x00, 0x49, 0x00, 0xf7 },
	                       { 0xf0, 0x43, 0x10, 0x4c, 0x03, 0x01, 0x00, 0x05, 0x00, 0xf7 } });
	run_blocks(a, rate, block, 0.5, nullptr, false);
	// インサーションのパラメータは割合（0-1000）。Drive 0-127 の 500 は 64 になり、読み戻すと 504
	const target T[] = {
		{ "A1 Cutoff", 20 }, { "A10 Attack", 90 }, { "D16 Volume", 50 },
		{ "A1 EQ Bass Gain", 70 }, { "B3 Pan", 0 }, { "Reverb Return", 100 }, { "Master EQ Gain 3", 58 },
		{ "C5 Note Shift", 0x40 + 7 }, { "Master Tune", 0x400 - 30 },
		{ "INS1 Param 1", 504 }, { "INS2 Param 1", 250 }, { "INS2 Param 10", 1000 },
		{ "A2 Resonance", 30 }, { "Master EQ Freq 5", 40 },
	};
	const int SEND[] = { 20, 90, 50, 70, 0, 100, 58, 0x40 + 7, 0x400 - 30, 500, 250, 1000 };
	param_events pe;
	for (size_t k = 0; k < 12; k++)
		pe.add(ids[T[k].name], SEND[k]);
	run_blocks(a, rate, block, 2.0, &pe, true);      // 同じ値を区間ごとに送り続ける
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	run_blocks(a, rate, block, 0.3, nullptr, false);

	auto check = [&](const clap_plugin_t *p, const char *what) {
		const auto *pp = static_cast<const clap_plugin_params_t *>(p->get_extension(p, CLAP_EXT_PARAMS));
		int ng = 0;
		for (const target &t : T) {
			double v = -1;
			pp->get_value(p, ids[t.name], &v);
			if (int(std::lround(v)) != t.value) {
				std::printf("NG: %s %s は %g（%d のはず）\n", what, t.name, v, t.value);
				ng++;
			}
			if (!std::strncmp(t.name, "INS", 3)) {
				char text[64] = {};
				pp->value_to_text(p, ids[t.name], v, text, sizeof(text));
				std::printf("  %s = %s\n", t.name, text);
			}
		}
		const int n = int(sizeof(T) / sizeof(T[0]));
		std::printf("%s: %d 個のうち %d 個が合った\n", what, n, n - ng);
		return ng;
	};
	bad += check(a, "送った値を音源から読み戻す");

	byte_stream st;
	state->save(a, &st.out);
	std::printf("状態 %zu バイト\n", st.buf.size());
	a->stop_processing(a);
	a->deactivate(a);
	a->destroy(a);

	const clap_plugin_t *b = make();
	const auto *bstate = static_cast<const clap_plugin_state_t *>(b->get_extension(b, CLAP_EXT_STATE));
	st.pos = 0;
	bstate->load(b, &st.in);
	b->activate(b, rate, 1, block);
	b->start_processing(b);
	run_blocks(b, rate, block, 1.5, nullptr, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	bad += check(b, "状態を戻した実体");
	b->stop_processing(b);
	b->deactivate(b);
	b->destroy(b);

	// 機械まるごとの状態を抜いて、XG の値の控えだけにする
	if (st.buf.size() < 24) {
		std::printf("NG: 状態が空（音源が起動していない。ROM の場所を S_MU2000_ROMS で渡す）\n");
		return 1;
	}
	auto i32 = [&](size_t at) { int32_t v = 0; std::memcpy(&v, st.buf.data() + at, 4); return v; };
	size_t at = 8;
	const int32_t packed = i32(at); at += 4 + size_t(packed);
	const int32_t card = i32(at); at += 4 + size_t(card);
	const int32_t setup = i32(at); at += 4;
	std::printf("控え %d バイト（機械まるごと %d バイト）\n", setup, packed);
	byte_stream only;
	only.buf.assign(st.buf.begin(), st.buf.begin() + 8);
	auto push32 = [&](int32_t v) { uint8_t b4[4]; std::memcpy(b4, &v, 4); only.buf.insert(only.buf.end(), b4, b4 + 4); };
	push32(0);
	push32(0);
	push32(setup);
	only.buf.insert(only.buf.end(), st.buf.begin() + long(at), st.buf.begin() + long(at) + setup);
	const clap_plugin_t *c = make();
	const auto *cstate = static_cast<const clap_plugin_state_t *>(c->get_extension(c, CLAP_EXT_STATE));
	cstate->load(c, &only.in);
	c->activate(c, rate, 1, block);
	c->start_processing(c);
	run_blocks(c, rate, block, 4.0, nullptr, false);
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	run_blocks(c, rate, block, 0.2, nullptr, false);
	bad += check(c, "XG の値の控えだけで戻した実体");
	c->stop_processing(c);
	c->deactivate(c);
	c->destroy(c);

	std::printf("---- XG の値はここまで: %s ----\n", bad ? "NG あり" : "全部合った");
	return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
	smu2000::init_console_utf8();
	// --load: 読み込んで 1 つ作って捨てるだけ。ROM が無くても通るので、CI で出来た DLL を確かめられる
	// （2026-10 に、Windows が読み込みの時点で断る DLL をリリースしてしまった。#241・#246）
	if (argc == 3 && (!std::strcmp(argv[2], "--automation") || !std::strcmp(argv[2], "--load"))) {
		std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
		HMODULE mod = LoadLibraryA(argv[1]);
		const auto *entry = mod ? reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(mod, "clap_entry")) : nullptr;
#else
		void *mod = dlopen(argv[1], RTLD_NOW);
		const auto *entry = mod ? static_cast<const clap_plugin_entry_t *>(dlsym(mod, "clap_entry")) : nullptr;
#endif
		if (!entry || !entry->init(argv[1])) { std::fprintf(stderr, "clap_entry が無い\n"); return 1; }
		const auto *fac = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
		if (!fac || fac->get_plugin_count(fac) == 0) { std::fprintf(stderr, "工場が空\n"); return 1; }
		const char *id = fac->get_plugin_descriptor(fac, 0)->id;
		if (!std::strcmp(argv[2], "--load")) {
			const clap_plugin_t *p = fac->create_plugin(fac, &kHost, id);
			const bool ok = p && p->init(p);
			if (p)
				p->destroy(p);
			entry->deinit();
			std::printf(ok ? "OK: %s を読み込んで作れた\n" : "NG: %s を作れない\n", id);
			return ok ? 0 : 1;
		}
		const int rc = run_automation(fac, id);
		entry->deinit();
		return rc;
	}
	if (argc < 4) {
		std::fprintf(stderr, "使い方: clapprobe <S-MU2000.clap> <MIDI> <出力 wav> [--rate 48000] [--block 512] [--adc-silence] [--sysex-last] [--broadcast]\n");
		return 1;
	}
	const std::string dll = argv[1], mid = argv[2], wav = argv[3];
	double rate = 48000.0;
	int block = 512;
	double extra = 3.0;
	bool adc = false;
	bool fx = false;
	bool clap_notes = false;   // ノートオン・オフを CLAP 流（CLAP_EVENT_NOTE_*）で渡す
	// SysEx を、同じ区間のチャンネルメッセージの**後ろ**にまとめて渡す（issue #51。foo_midi の
	// ように SysEx を別に渡すホストのまね）。時刻は変えない。プラグインは同じ時刻の
	// リセットを先に流すので、曲頭の「XG System On → 音色の指定」が崩れないはず
	bool sysex_last = false;
	// どのイベントも 4 つの口すべてへ同じものを配る（issue #59。REAPER のまね）
	bool broadcast = false;
	for (int i = 4; i < argc; i++) {
		if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--block") && i + 1 < argc) block = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) extra = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--adc-silence")) adc = true;
		else if (!std::strcmp(argv[i], "--fx")) fx = adc = true;
		else if (!std::strcmp(argv[i], "--clap-notes")) clap_notes = true;
		else if (!std::strcmp(argv[i], "--sysex-last")) sysex_last = true;
		else if (!std::strcmp(argv[i], "--broadcast")) broadcast = true;
	}

#if defined(_WIN32)
	HMODULE mod = LoadLibraryA(dll.c_str());
	if (!mod) { std::fprintf(stderr, "DLL を読めない: %s（エラー %lu）\n", dll.c_str(), GetLastError()); return 1; }
	const auto *entry = reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(mod, "clap_entry"));
#else
	void *mod = dlopen(dll.c_str(), RTLD_NOW);
	if (!mod) { std::fprintf(stderr, "読めない: %s\n", dll.c_str()); return 1; }
	const auto *entry = static_cast<const clap_plugin_entry_t *>(dlsym(mod, "clap_entry"));
#endif
	if (!entry || !entry->init(dll.c_str())) { std::fprintf(stderr, "clap_entry が無い\n"); return 1; }
	const auto *fac = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	if (!fac || fac->get_plugin_count(fac) < 1) { std::fprintf(stderr, "工場が無い\n"); return 1; }
	// 口は 2 つ: 0 = 音源、1 = 同じ本体をエフェクトとして（--fx で選ぶ）
	if (fx && fac->get_plugin_count(fac) < 2) { std::fprintf(stderr, "エフェクトとしての口が無い\n"); return 1; }
	const clap_plugin_descriptor_t *desc = fac->get_plugin_descriptor(fac, fx ? 1 : 0);
	std::printf("%s（%s）\n", desc->name, desc->id);
	const clap_plugin_t *plug = fac->create_plugin(fac, &kHost, desc->id);
	if (!plug || !plug->init(plug)) { std::fprintf(stderr, "作れない\n"); return 1; }
	if (const auto *ports = static_cast<const clap_plugin_audio_ports_t *>(plug->get_extension(plug, CLAP_EXT_AUDIO_PORTS))) {
		clap_audio_port_info_t info{};
		if (ports->get(plug, 0, true, &info)) {
			const bool main_in = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0;
			std::printf("%s: 音声入力は%s\n", main_in == fx ? "OK" : "NG", main_in ? "メインの口" : "補助の口");
			if (main_in != fx)
				return 2;
		}
	}

	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(mid, events, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
	const double length = events.empty() ? 0.0 : events.back().time;
	std::printf("MIDI: %zu イベント、%.2f 秒\n", events.size(), length);

	// activate が起動を待ちきる（issue #19）
	if (!plug->activate(plug, rate, 1, uint32_t(block))) { std::fprintf(stderr, "activate に失敗\n"); return 1; }
	plug->start_processing(plug);

	const size_t nb = static_cast<size_t>(block);
	std::vector<float> bl(nb), br(nb), il(nb), ir(nb);
	float *outs[2] = { bl.data(), br.data() };
	float *ins[2]  = { il.data(), ir.data() };
	clap_audio_buffer_t obuf{}; obuf.data32 = outs; obuf.channel_count = 2;
	clap_audio_buffer_t ibuf{}; ibuf.data32 = ins;  ibuf.channel_count = 2;
	in_events ev;
	clap_output_events_t oev{ nullptr, out_push };

	const int64_t total = int64_t((length + extra) * rate);
	std::vector<int16_t> pcm;
	pcm.reserve(size_t(total) * 2);
	size_t next = 0;
	int64_t pos = 0;
	while (pos < total) {
		const uint32_t n = uint32_t(std::min<int64_t>(block, total - pos));
		ev.midi.clear(); ev.sysex.clear(); ev.notes.clear(); ev.order.clear(); ev.arrival.clear();
		ev.midi.reserve(4096); ev.sysex.reserve(256); ev.notes.reserve(4096);
		std::fill(il.begin(), il.end(), 0.0f);
		std::fill(ir.begin(), ir.end(), 0.0f);
		if (fx)
			for (uint32_t k = 0; k < n; k++)
				il[k] = ir[k] = float(0.3 * std::sin(2 * 3.14159265358979 * 440.0 * double(pos + k) / rate));
		while (next < events.size() && events[next].time * rate < double(pos + n)) {
			const smf::event &e = events[next++];
			if (e.bytes.empty())
				continue;
			const uint32_t off = uint32_t(std::clamp<int64_t>(int64_t(e.time * rate) - pos, 0, int64_t(n) - 1));
			for (int copy = 0; copy < (broadcast ? 4 : 1); copy++) {
			const uint16_t port = broadcast ? uint16_t(copy) : uint16_t(e.port < 4 ? e.port : 3);
			if (e.bytes[0] == 0xf0) {
				clap_event_midi_sysex_t s{};
				s.header = { sizeof(s), off, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0 };
				s.port_index = port;
				s.buffer = e.bytes.data();
				s.size = uint32_t(e.bytes.size());
				ev.arrival.push_back({ 1, ev.sysex.size() });
				ev.sysex.push_back(s);
			} else if (clap_notes && e.bytes.size() >= 3 && ((e.bytes[0] & 0xe0) == 0x80)) {
				const bool on = (e.bytes[0] & 0xf0) == 0x90 && e.bytes[2];
				clap_event_note_t nt{};
				nt.header = { sizeof(nt), off, CLAP_CORE_EVENT_SPACE_ID, uint16_t(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
				nt.note_id = -1;
				nt.port_index = int16_t(port);
				nt.channel = int16_t(e.bytes[0] & 15);
				nt.key = int16_t(e.bytes[1]);
				nt.velocity = double(e.bytes[2]) / 127.0;
				ev.arrival.push_back({ 2, ev.notes.size() });
				ev.notes.push_back(nt);
			} else if (e.bytes[0] >= 0x80 && e.bytes[0] < 0xf0) {
				clap_event_midi_t m{};
				m.header = { sizeof(m), off, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
				m.port_index = port;
				for (size_t k = 0; k < 3 && k < e.bytes.size(); k++)
					m.data[k] = e.bytes[k];
				ev.arrival.push_back({ 0, ev.midi.size() });
				ev.midi.push_back(m);
			}
			}
		}
		// 時刻順に並べる（同じ時刻なら来た順）。vector が伸び終わってから指す
		for (const auto &a : ev.arrival)
			ev.order.push_back(a.first == 1 ? &ev.sysex[a.second].header
			                    : a.first == 2 ? &ev.notes[a.second].header : &ev.midi[a.second].header);
		std::stable_sort(ev.order.begin(), ev.order.end(),
		                 [](const clap_event_header_t *a, const clap_event_header_t *b) { return a->time < b->time; });
		if (sysex_last)
			std::stable_partition(ev.order.begin(), ev.order.end(),
			                      [](const clap_event_header_t *h) { return h->type != CLAP_EVENT_MIDI_SYSEX; });

		clap_process_t pr{};
		pr.steady_time = pos;
		pr.frames_count = n;
		pr.audio_inputs = adc ? &ibuf : nullptr;
		pr.audio_inputs_count = adc ? 1 : 0;
		pr.audio_outputs = &obuf;
		pr.audio_outputs_count = 1;
		pr.in_events = &ev.api;
		pr.out_events = &oev;
		plug->process(plug, &pr);
		for (uint32_t i = 0; i < n; i++) {
			// vst3probe と同じ丸め方
			pcm.push_back(int16_t(std::lround(std::clamp(bl[i], -1.0f, 1.0f) * 32767.0f)));
			pcm.push_back(int16_t(std::lround(std::clamp(br[i], -1.0f, 1.0f) * 32767.0f)));
		}
		pos += n;
	}

	plug->stop_processing(plug);
	plug->deactivate(plug);
	plug->destroy(plug);
	entry->deinit();

	write_wav(wav, pcm, uint32_t(rate));
	long peak = 0;
	double sum = 0;
	for (int16_t v : pcm) { peak = std::max<long>(peak, std::labs(v)); sum += std::fabs(double(v)); }
	std::printf("最大 %ld  平均 %.1f\n", peak, pcm.empty() ? 0.0 : sum / double(pcm.size()));
	std::printf("書き出した: %s（%.1f 秒 / %.0f Hz）\n", wav.c_str(), double(pcm.size() / 2) / rate, rate);
	return 0;
}
