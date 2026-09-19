// license:BSD-3-Clause
//
// MIDI ファイルを食わせて WAV に書き出す。
//
//   render <rom ディレクトリ> <MIDI ファイル> <出力 wav> [秒数] [--adc-in 入力 wav] [--card カード img]
//
// --adc-in は A/D INPUT に流す音（16bit PCM、1 か 2 チャンネル、44.1kHz）。MIDI の 0 秒から流す。
// 左が AD1、右が AD2（1 チャンネルなら両方に同じもの）
// --card は SmartMedia の中身のファイル（gui で作ったもの）を差す。firmware が書いたブロックは終わりに書き戻す
//
// 実機と同じく、MIDI は 31250bps の直列で MIDI IN A に流し込む。
// 出来た WAV は MAME の録音と突き合わせるためのもの。

#include "compat/platform.h"
#include "mu2000.h"
#include "voicecache.h"
#include "bootcache.h"
#include "smf.h"
#include "ui/options.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void write_wav(const std::string &path, const std::vector<s16> &pcm, u32 rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	const u32 bytes = u32(pcm.size() * 2);
	auto u32w = [&](u32 v) { u8 b[4] = { u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24) };
	                         std::fwrite(b, 1, 4, f); };
	auto u16w = [&](u16 v) { u8 b[2] = { u8(v), u8(v >> 8) }; std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); u32w(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); u32w(16); u16w(1); u16w(2);
	u32w(rate); u32w(rate * 4); u16w(4); u16w(16);
	std::fwrite("data", 1, 4, f); u32w(bytes);
	std::fwrite(pcm.data(), 1, bytes, f);
	std::fclose(f);
}

// 16bit PCM の WAV を読む。左右に分けて返す。読めなければ false
bool read_wav16(const std::string &path, std::vector<s16> &l, std::vector<s16> &r, std::string &err)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) { err = "開けない: " + path; return false; }
	std::vector<u8> d;
	u8 buf[65536];
	size_t got;
	while ((got = std::fread(buf, 1, sizeof buf, f)) > 0)
		d.insert(d.end(), buf, buf + got);
	std::fclose(f);
	auto u16at = [&](size_t o) { return u16(d[o] | (d[o + 1] << 8)); };
	auto u32at = [&](size_t o) { return u32(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (u32(d[o + 3]) << 24)); };
	if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) { err = "WAV ではない: " + path; return false; }
	int ch = 0, bits = 0;
	u32 rate = 0;
	for (size_t o = 12; o + 8 <= d.size();) {
		const u32 len = u32at(o + 4);
		const size_t body = o + 8;
		if (!std::memcmp(d.data() + o, "fmt ", 4) && body + 16 <= d.size()) {
			ch = u16at(body + 2); rate = u32at(body + 4); bits = u16at(body + 14);
		} else if (!std::memcmp(d.data() + o, "data", 4)) {
			if (bits != 16 || (ch != 1 && ch != 2)) { err = "16bit の 1 か 2 チャンネルだけ読める: " + path; return false; }
			if (rate != 44100) std::fprintf(stderr, "警告: %s は %u Hz（44100 Hz として流す）\n", path.c_str(), rate);
			const size_t n = std::min<size_t>(len, d.size() - body) / (2 * ch);
			l.resize(n); r.resize(n);
			for (size_t i = 0; i < n; i++) {
				l[i] = s16(u16at(body + i * 2 * ch));
				r[i] = s16(u16at(body + i * 2 * ch + (ch == 2 ? 2 : 0)));
			}
			return true;
		}
		o = body + len + (len & 1);
	}
	err = "data が無い: " + path;
	return false;
}

const char *reset_name(const std::vector<u8> &bytes)
{
	if (bytes.size() == 6 && bytes[0] == 0xf0 && bytes[1] == 0x7e &&
	    bytes[3] == 0x09 && bytes[5] == 0xf7) {
		if (bytes[4] == 0x01) return "GM System On";
		if (bytes[4] == 0x03) return "GM2 System On";
	}
	if (bytes.size() >= 11 && bytes[0] == 0xf0 && bytes[1] == 0x41 &&
	    (bytes[2] & 0xf0) == 0x10 && bytes[3] == 0x42 && bytes[4] == 0x12 &&
	    bytes[5] == 0x40 && bytes[6] == 0x00 && bytes[7] == 0x7f && bytes[8] == 0x00)
		return "GS Reset";
	if (bytes.size() == 9 && bytes[0] == 0xf0 && bytes[1] == 0x43 &&
	    (bytes[2] & 0xf0) == 0x10 && bytes[3] == 0x4c && bytes[4] == 0x00 &&
	    bytes[5] == 0x00 && bytes[6] == 0x7e && bytes[7] == 0x00 && bytes[8] == 0xf7)
		return "XG System On";
	return nullptr;
}

std::vector<u8> reset_bytes(const char *mode)
{
	if (!std::strcmp(mode, "gm"))
		return { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
	if (!std::strcmp(mode, "xg"))
		return { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 };
	return { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
}

void insert_reset(std::vector<smf::event> &events, const char *mode)
{
	double first = events.empty() ? 0.05 : events.front().time;
	for (const smf::event &event : events) {
		if (!(event.bytes.size() == 2 && event.bytes[0] == 0xf5)) {
			first = event.time;
			break;
		}
	}
	if (first < 0.05) {
		const double shift = 0.05 - first;
		for (smf::event &event : events)
			event.time += shift;
	}
	events.insert(events.begin(), { 0.0, reset_bytes(mode), 0 });
}

const char *event_name(const std::vector<u8> &bytes)
{
	if (bytes.empty()) return "empty";
	switch (bytes[0] & 0xf0) {
	case 0x80: return "note-off";
	case 0x90: return bytes.size() > 2 && bytes[2] ? "note-on" : "note-off";
	case 0xa0: return "poly-pressure";
	case 0xb0: return "control-change";
	case 0xc0: return "program-change";
	case 0xd0: return "channel-pressure";
	case 0xe0: return "pitch-bend";
	default: return bytes[0] == 0xf0 ? "sysex" : "system";
	}
}

void trace_event(size_t index, const smf::event &event, int port)
{
	std::printf("MIDI event %zu: %.6f s, port %d, %s:", index, event.time,
	            port + 1, event_name(event.bytes));
	for (u8 byte : event.bytes)
		std::printf(" %02X", unsigned(byte));
	std::putchar('\n');
}

} // namespace


int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr,
			"使い方: render <rom ディレクトリ> <MIDI ファイル> <出力 wav> [秒数] [--trace-midi] [--reset gm|gs|xg] [--fast-midi]\n");
		return 1;
	}
	const std::string dir = argv[1], mid = argv[2], wav = argv[3];
	double seconds = 0.0;
	bool duration_given = false;
	bool trace_midi = false;
	ui::engine_options eng_opts;
	bool usb_host  = false;
	bool use_bootcache = false;   // --bootcache。起動後の写しから始める（確かめ用）
	const char *state_at = nullptr; size_t state_sample = 0;   // --state-at（確かめ用）
	const char *forced_reset = nullptr;
	const char *swptrace = nullptr;
	bool single = false;   // スレーブを別スレッドにしない
	double boot = -1.0;     // 負なら firmware が受信を有効にするまで待つ
	const char *mu_dac_path = nullptr;
	u32 mu_dac_from = 0, mu_dac_count = 0;
	const char *meg_path = nullptr;    // MEG の中身を書き出す先
	const char *meg_trace = nullptr;   // MEG を 1 命令ずつ追う
	u32 meg_tr_from = 0, meg_tr_count = 0, meg_tr_pc0 = 0, meg_tr_pc1 = 0x180;
	const char *adc_path = nullptr;    // A/D INPUT に流す WAV
	const char *card_path = nullptr;   // 差す SmartMedia
	const char *replay = nullptr;      // --replay-swp。記録したレジスタ列を SH-2 無しで流す
	// --native-off 秒: その時刻で native の口を切る。窓の F4（聞き比べ）と
	// 同じ道を通るので、切ったときに音が鳴りっぱなしにならないかを数で確かめられる
	double native_off = -1.0;
	// --midi-block フレーム: MIDI を**そのブロックの頭でまとめて**渡す。
	// 窓やプラグインは音声のブロック単位で MIDI を配るので、同じ時刻に
	// たくさんの音が重なる。render は既定でサンプル単位に散らすため、
	// その並びでしか出ない不具合が再現できない
	int midi_block = 0;
	for (int i = 4; i < argc; i++) {
		if (!std::strcmp(argv[i], "--trace-swp") && i + 1 < argc)
			swptrace = argv[++i];
		else if (!std::strcmp(argv[i], "--replay-swp") && i + 1 < argc)
			replay = argv[++i];
		else if (!std::strcmp(argv[i], "--boot") && i + 1 < argc)
			boot = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--dump-dac") && i + 3 < argc) {
			mu_dac_path = argv[++i];
			mu_dac_from = u32(std::strtoul(argv[++i], nullptr, 0));
			mu_dac_count = u32(std::strtoul(argv[++i], nullptr, 0));
		}
		else if (!std::strcmp(argv[i], "--dump-meg") && i + 1 < argc)
			meg_path = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-meg") && i + 5 < argc) {
			meg_trace    = argv[++i];
			meg_tr_from  = u32(std::strtoul(argv[++i], nullptr, 0));
			meg_tr_count = u32(std::strtoul(argv[++i], nullptr, 0));
			meg_tr_pc0   = u32(std::strtoul(argv[++i], nullptr, 0));
			meg_tr_pc1   = u32(std::strtoul(argv[++i], nullptr, 0));
		}
		else if (!std::strcmp(argv[i], "--single"))
			single = true;
		else if (!std::strcmp(argv[i], "--adc-in") && i + 1 < argc)
			adc_path = argv[++i];
		else if (!std::strcmp(argv[i], "--card") && i + 1 < argc)
			card_path = argv[++i];
		else if (!std::strcmp(argv[i], "--trace-midi"))
			trace_midi = true;
		else if (ui::consume_engine_option(argv[i], eng_opts)) {}
		else if (!std::strcmp(argv[i], "--usb"))
			usb_host = true;
		else if (!std::strcmp(argv[i], "--native-off") && i + 1 < argc)
			native_off = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-block") && i + 1 < argc)
			midi_block = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--bootcache"))
			use_bootcache = true;
		else if (!std::strcmp(argv[i], "--state-at") && i + 2 < argc) {
			state_sample = size_t(std::strtoull(argv[++i], nullptr, 0));
			state_at = argv[++i];
		}
		else if (!std::strcmp(argv[i], "--reset")) {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "--reset requires gm, gs, or xg\n");
				return 1;
			}
			forced_reset = argv[++i];
			if (std::strcmp(forced_reset, "gm") && std::strcmp(forced_reset, "gs") &&
			    std::strcmp(forced_reset, "xg")) {
				std::fprintf(stderr, "unknown reset mode: %s (expected gm, gs, or xg)\n", forced_reset);
				return 1;
			}
		}
		else if (!std::strcmp(argv[i], "-v"))
			smu2000::g_verbose = true;
		else {
			seconds = std::atof(argv[i]);
			duration_given = true;
		}
	}

	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(mid, events, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
	if (forced_reset) {
		events.erase(std::remove_if(events.begin(), events.end(), [](const smf::event &event) {
			return reset_name(event.bytes) != nullptr;
		}), events.end());
		insert_reset(events, forced_reset);
		std::printf("MIDI reset forced: %s\n", reset_name(events.front().bytes));
	}
	std::printf("MIDI: %zu イベント、最後は %.2f 秒\n",
	            events.size(), events.empty() ? 0.0 : events.back().time);

	std::vector<s16> adc_l, adc_r;
	if (adc_path && !read_wav16(adc_path, adc_l, adc_r, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
		std::fprintf(stderr, "警告: %s\n", mu.error().c_str());

	if (card_path && !mu.card().load(card_path, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

	// --replay-swp: 記録した SWP30 への書き込みを、SH-2 を回さずに同じ時刻へ流し込む
	// （doc/native-engine.md の段 0。「音は SWP30 だけで作れる」ことの確かめ）
	struct swp_write { u64 sample; bool master; u32 reg; u16 value; };
	std::vector<swp_write> replay_list;
	size_t replay_at = 0;
	if (replay) {
		std::FILE *rf = std::fopen(replay, "r");
		if (!rf) { std::fprintf(stderr, "開けない: %s\n", replay); return 1; }
		char line[256];
		while (std::fgets(line, sizeof(line), rf)) {
			const char *p = line;
			if (*p == 'R')
				continue;               // 読み出しは要らない
			if (*p == 'W')
				p += 2;
			unsigned base = 0, reg = 0, val = 0;
			unsigned long long sample = 0;
			if (std::sscanf(p, "%x %x %x pc=%*x t=%*f s=%llu", &base, &reg, &val, &sample) != 4)
				continue;
			replay_list.push_back({ u64(sample), base == 0x800000, reg, u16(val) });
		}
		std::fclose(rf);
		std::printf("再生する書き込み: %zu 件\n", replay_list.size());
		if (boot < 0.0) {
			std::fprintf(stderr, "--replay-swp には --boot <記録したときの起動秒数> が要る\n");
			return 1;
		}
		mu.set_cpu_enabled(false);
	}

	std::FILE *tf = swptrace ? std::fopen(swptrace, "w") : nullptr;
	if (tf)
		mu.set_swp_trace(tf, true);

	if (mu_dac_path) {
		mu.swpm().m_dbg_dac = std::fopen(mu_dac_path, "w");
		mu.swpm().m_dbg_dac_from = mu_dac_from;
		mu.swpm().m_dbg_dac_count = mu_dac_count;
		if (const char *c = std::getenv("SWP30_CHAN"))
			mu.swpm().m_dbg_chan = int(std::strtol(c, nullptr, 0));
	}

	if (meg_trace) {
		mu.swpm().m_dbg_meg = std::fopen(meg_trace, "w");
		mu.swpm().m_dbg_meg_from = meg_tr_from;
		mu.swpm().m_dbg_meg_count = meg_tr_count;
		mu.swpm().m_dbg_meg_pc0 = u16(meg_tr_pc0);
		mu.swpm().m_dbg_meg_pc1 = u16(meg_tr_pc1);
	}

	mu.set_threaded(!single);
	ui::apply_engine_options(mu, eng_opts);
	mu.set_usb_host(usb_host);
	// 鍵は起動に使うワーク RAM も混ぜるので reset() の前に作る
	const u64 boot_key = use_bootcache ? smu2000::bootcache::key(mu) : 0;
	mu.reset();

	const u32 rate = 44100;
	std::vector<s16> pcm;

	// 起動を待つ。実機も電源投入から数秒は MIDI を受け付けない。
	// 待たずに流すと曲頭のリセットや音色指定が捨てられ、全パートが
	// 初期音色（ピアノ）で鳴り、発音数も足りなくなって音が抜ける。
	// firmware が受信を有効にした時点を印にする
	// 起動後の写しから始める（--bootcache）。**確かめ用**で既定では使わない。
	// 試験は毎回まっさらから始めたいので、ここを既定にはしない
	if (use_bootcache && boot < 0.0) {
		if (smu2000::bootcache::load(mu, boot_key)) {
			std::printf("起動: 前の写しから\n");
			boot = 0.0;
		}
	}
	if (boot < 0.0) {
		const size_t limit = size_t(30.0 * rate);
		size_t i = 0;
		for (; i < limit && !mu.midi_ready(); i++) {
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
			pcm.push_back(s16(std::clamp(l * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
			pcm.push_back(s16(std::clamp(r * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
		}
		boot = double(i) / rate;
		if (use_bootcache && i < limit)
			smu2000::bootcache::save(mu, boot_key);
		if (i >= limit) {
			std::fprintf(stderr, "起動を待ったが MIDI 受信が有効にならなかった\n");
			return 1;
		}
		std::printf("起動に %.6f 秒。ここから MIDI を流す\n", boot);
	}

	const size_t boot_samples = size_t(boot * rate + 0.5);
	const double estimated_seconds = duration_given ? seconds :
		(events.empty() ? 3.0 : events.back().time + 3.0);
	pcm.reserve(size_t((boot + estimated_seconds) * rate) * 2);

	// 出し先は SMF のポート指定（`FF 21`）に従う。口 0 = MIDI IN A、
	// 口 1 = MIDI IN B。加えて、ファイルの中に `F5 nn`（1=A / 2=B）を
	// 入れておけばそこから切り替わる（F7 エスケープで埋める）。
	// 実機の firmware は F5 を見ていないので、**振り分けるのはこちら側の役目**
	int port = -1;                     // -1 なら SMF の指定に従う
	size_t next = 0;
	size_t scheduled_events = 0, scheduled_bytes = 0;
	size_t tail_start = size_t(-1);
	const size_t hard_stop = duration_given ? size_t((boot + seconds) * rate) : size_t(-1);
	// 軽量モードの float 計算で、非正規化数に落ち込まないようにする。
	// 音源として挿されたときと同じ状態で鳴らすため（compat/platform.h）
	const smu2000::denormals_off no_denormals;
	for (size_t i = pcm.size() / 2; ; i++) {
		if (duration_given && i >= hard_stop)
			break;
		if (!duration_given && next == events.size() && mu.midi_idle()) {
			if (tail_start == size_t(-1)) {
				tail_start = i;
				std::printf("MIDI queue drained at %.3f s; rendering 3.0 s tail\n",
				            double(i) / rate - boot);
			}
			if (i >= tail_start + size_t(3.0 * rate))
				break;
		}
		// SMU2000_RAMWATCH=1: パートのつまみが**いつ**変わるかを 0.5 秒ごとに見る。
		// firmware は native の口では細切れにしか回らないので、曲が送った値を
		// 処理し終える時刻がずれる。その遅れを目で見るための窓
		if (std::getenv("SMU2000_RAMWATCH") && i > size_t(boot * rate) &&
		    (i - size_t(boot * rate)) % 22050 == 0) {
			const std::vector<u8> &rr = mu.nvram();
			std::fprintf(stderr, "ram s=%zu", i - size_t(boot * rate));
			for (int p = 0; p < 4; p++) {
				const u32 b = xg::ram::part_base(p);
				if (b + 0x1a < rr.size())
					std::fprintf(stderr, "  p%d 音量=%d 送り=%d コーラス=%d 明=%d", p,
					             rr[b + 0x0b], rr[b + 0x13], rr[b + 0x12], rr[b + 0x18]);
			}
			std::fprintf(stderr, "\n");
		}
		if (state_at && i == size_t(boot * rate) + state_sample) {
			const std::vector<u8> st = mu.save_state();
			if (std::FILE *sf = std::fopen(state_at, "wb")) {
				std::fwrite(st.data(), 1, st.size(), sf);
				std::fclose(sf);
			}
		}
		if (eng_opts.native_engine && native_off >= 0.0 &&
		    i == size_t(boot * rate) + size_t(native_off * rate)) {
			mu.set_native_engine(0);
			std::printf("%.3f 秒で native の口を切った\n", native_off);
		}
		// native の口は、起動が終わってから入れる（起動には firmware が要る）
		if (eng_opts.native_engine && i == boot_samples) {
			mu.set_native_engine(eng_opts.native_engine);
			// 前に写し取ったものがあれば読む（1 音目から native で鳴らせる）
			if (eng_opts.voicecache &&
			    smu2000::voicecache::load(mu, smu2000::voicecache::key(mu)))
				std::printf("写し取り: %d 音色を前の写しから\n", int(mu.native_cal_count()));
		}

		// 起動ぶんは**整数で引く**。double(i)/rate - boot と書くと桁落ちで
		// 1e-12 秒ずれ、イベントの時刻がちょうど境に乗ったときに 1 サンプル動く
		double t = (double(i) - double(boot_samples)) / rate;
		// ブロック単位で配るときは、そのブロックの**終わりまで**の分を
		// ブロックの頭でまとめて渡す（窓やプラグインと同じ並びになる）
		if (midi_block > 1) {
			const long long s2 = (long long)i - (long long)boot_samples;
			if (s2 >= 0 && (s2 % midi_block) != 0)
				goto after_midi;
			t = (double(s2) + double(midi_block) - 1.0) / rate;
		}
		while (next < events.size() && events[next].time <= t) {
			const std::vector<u8> &ev = events[next].bytes;
			if (ev.size() == 2 && ev[0] == 0xf5)
				port = std::clamp(int(ev[1]) - 1, 0, mu2000::MIDI_PORTS - 1);
			else {
				// ファイルの口 3・4 は gui の既定と同じく A・B に重ねる
				// USB の口を使うときは C・D まで届くので、ファイルの口をそのまま使う
				const int to = port >= 0 ? port
					: usb_host ? std::min<int>(events[next].port, mu2000::MIDI_PORTS - 1)
					: smf::mu_port(events[next].port, true);
				if (trace_midi)
					trace_event(next, events[next], to);
				if (const char *reset = reset_name(ev))
					std::printf("MIDI reset: %.6f s, port %d, %s\n", events[next].time, to + 1, reset);
				for (u8 b : ev)
					mu.midi_in(b, to);
				scheduled_events++;
				scheduled_bytes += ev.size();
			}
			next++;
		}
	after_midi:

		if (!adc_l.empty()) {
			const double tin = t * rate;
			const size_t k = tin < 0 ? adc_l.size() : size_t(tin);
			mu.set_audio_input(k < adc_l.size() ? adc_l[k] : 0, k < adc_r.size() ? adc_r[k] : 0);
		}
		// 記録したときと同じサンプルの頭で入れ直す（CPU はそのサンプルぶんを
		// SWP30 より先に回すので、頭で入れれば実機と同じ順になる）
		while (replay_at < replay_list.size() && replay_list[replay_at].sample <= i) {
			const swp_write &w = replay_list[replay_at];
			mu.poke_swp(w.master, w.reg, w.value);
			replay_at++;
		}
		s32 l = 0, r = 0;
		mu.run_sample(l, r);
		// DAC の全振幅は 1<<17。16bit に落とす（MAME の 1<<17 目盛りと同じ）
		l = l * 32768 / mu2000::DAC_FULL_SCALE;
		r = r * 32768 / mu2000::DAC_FULL_SCALE;
		pcm.push_back(s16(std::clamp(l, -32768, 32767)));
		pcm.push_back(s16(std::clamp(r, -32768, 32767)));

		if (!(i % (rate * 5)))
			std::printf("  %5.1f 秒  PC=%08x\n", double(i) / rate - boot, mu.cpu().pc());
	}

	const size_t total = pcm.size() / 2;
	std::printf("MIDI scheduled: %zu events, %zu bytes; pending: %zu; dropped: %llu\n",
	            scheduled_events, scheduled_bytes, mu.midi_pending(),
	            (unsigned long long)mu.midi_dropped());
	if (next != events.size())
		std::printf("MIDI unscheduled: %zu events (explicit render duration reached)\n",
		            events.size() - next);

	if (tf)
		std::fclose(tf);

	// MEG の中身。最後の姿（＝最後に設定したエフェクト）を書き出す
	if (meg_path) {
		mu.swpm().dump_meg((std::string(meg_path) + ".m").c_str());
		mu.swps().dump_meg((std::string(meg_path) + ".s").c_str());
		std::printf("MEG を書き出した: %s.m / %s.s\n", meg_path, meg_path);
	}

	if (smu2000::g_verbose)
		std::printf("最大値  AWM2=%d  MEG=%d  DAC=%d\n",
		            mu.swpm().m_dbg_awm_max, mu.swpm().m_dbg_meg_max, mu.swpm().m_dbg_adc_max),
		std::printf("        MEG入力=%d  MELO=%d\n",
		            mu.swpm().m_dbg_megin_max, mu.swpm().m_dbg_melo_max);

	std::printf("CPU %llu サイクル / %zu サンプル = %.3f（あるべき値 %.3f）\n",
	            (unsigned long long)mu.cpu().total_cycles(), total,
	            double(mu.cpu().total_cycles()) / total, 28000000.0 / rate);

	std::printf("実行ループ %llu 周（1 サンプルあたり %.2f 周）、"
	            "タイマ %llu 回、周辺イベント %llu 回\n",
	            (unsigned long long)mu.m_loops, double(mu.m_loops) / total,
	            (unsigned long long)mu.m_timer_fires, (unsigned long long)mu.m_event_fires);

	mu.print_swp_widths();

	if (smu2000::g_verbose) {
		auto report = [](const char *name, swp30_device &d) {
			int silent = 0, weak = 0;
			for (auto [e, l] : d.m_dbg_notes) {
				if (!l || e == 0) silent++;
				else if (e / l < 50) weak++;
			}
			std::printf("%s: 発音 %zu 件 無音 %d 件 ごく小さい %d 件\n",
			            name, d.m_dbg_notes.size(), silent, weak);
			int bucket[8] = {};
			for (auto [e, l] : d.m_dbg_notes) {
				if (!l) continue;
				const u64 avg = e / l;
				int k = 0;
				while (k < 7 && avg >= (u64(20) << k)) k++;
				bucket[k]++;
			}
			std::printf("   平均振幅の分布 <20:%d <40:%d <80:%d <160:%d <320:%d <640:%d <1280:%d それ以上:%d\n",
			            bucket[0],bucket[1],bucket[2],bucket[3],bucket[4],bucket[5],bucket[6],bucket[7]);
		};
		report("マスタ", mu.swpm());
		report("スレーブ", mu.swps());
	}

	if (card_path) {
		std::vector<smu2000::smartmedia::block> blocks;
		mu.card().take_dirty_blocks(blocks);
		if (!smu2000::smartmedia::write_blocks(card_path, blocks, err))
			std::fprintf(stderr, "%s\n", err.c_str());
		else if (!blocks.empty())
			std::printf("カードに書き戻した: %s（%zu ブロック）\n", card_path, blocks.size());
	}

	write_wav(wav, pcm, rate);
	if (eng_opts.native_engine && eng_opts.voicecache)
		smu2000::voicecache::save(mu, smu2000::voicecache::key(mu));
	if (eng_opts.native_engine) {
		std::printf("native の口: 演奏中に firmware を回したのは %.1f%%\n",
		            100.0 * mu.native_firmware_share());
		const mu2000::native_why w = mu.native_why_counts();
		if (w.total)
			std::printf("  内訳: firmware の音 %.1f%% / SysEx %.1f%% / 写し取り %.1f%% / "
			            "MIDI の受け取り %.1f%% / 細く回す %.1f%% / そのほか %.1f%%\n",
			            100.0 * double(w.by_note) / double(w.total),
			            100.0 * double(w.by_sysex) / double(w.total),
			            100.0 * double(w.by_learn) / double(w.total),
			            100.0 * double(w.by_midi) / double(w.total),
			            100.0 * double(w.by_keep) / double(w.total),
			            100.0 * double(w.by_other) / double(w.total));
	}
	if (eng_opts.native_engine) {
		std::printf("  いちばん多いときのスロット: %d / 64\n", mu.native_peak_slots());
		if (const u32 stomp = mu.native_fw_stomp())
			std::printf("  **firmware がこちらの鳴っているスロットに書いた %u 回**\n", stomp);
		if (const u32 wrong = mu.native_learn_wrong())
			std::printf("  **写し取りで別の音のスロットを掴んで捨てた %u 回**\n", wrong);
		if (const u32 dirty = mu.native_learn_dirty())
			std::printf("  **写し取りが汚れた %u 回**（窓の中で別の音が同じ"
			            "スロットに鳴り始めた）\n", dirty);
		if (const u32 clash = mu.native_slot_clash())
			std::printf("  **スロットの奪い合い %u 回**（写し取りのとき firmware がこちらの鳴っているスロットを取った）\n", clash);
		const mu2000::native_stats st = mu.native_counts();
		std::printf("  鍵: native %llu / firmware %llu（うち写し取り %llu）、そのほかの MIDI %llu\n",
		            (unsigned long long)st.note_native, (unsigned long long)st.note_fw,
		            (unsigned long long)st.learn, (unsigned long long)st.other);
	}
	std::printf("書き出した: %s（%.1f 秒）\n", wav.c_str(), double(total) / rate);
	return 0;
}
