// license:BSD-3-Clause
//
// Windows の MIDI 入力を受けて、そのまま音を鳴らす。
//
//   live --list                          MIDI 入力の一覧
//   live <rom ディレクトリ> [--midi 番号] [--latency ミリ秒] [--fast-midi]
//   live <rom ディレクトリ> --waveout    古い方式（WinMM）で鳴らす
//   live <rom ディレクトリ> --factory    覚えている設定を捨てて工場出荷状態で起動する
//
// 設定（ワーク RAM）は終わるときに src/nvram.h の置き場へ残し、次の起動で使う。
// Ctrl+C でも、音を止めて残してから終わる。
//
// 設計の要点は doc/design.md にあるとおりで、**時計を自分で持たない**こと。
// 音声デバイスが「N サンプルくれ」と要求した分だけ CPU と音源を進める。
// こうするとホストとずれようがない。MAME が外部同期で破綻したのはここの違い。
//
// 音声出力は 2 通り持っている。
//   WASAPI 共有モード（既定）  イベント駆動。待ち時間は Windows の周期に従う
//   WinMM waveOut (--waveout)  素直だが、この環境では 1 枚 23ms を割ると
//                              供給が追いつかず細切れになる（合計 70ms 必要）

#include "compat/cli_text.h"
#include "mu2000.h"
#include "voicecache.h"
#include "nvram.h"
#include "ui/audio_out.h"
#include "ui/wav.h"
#include "ui/midi_in.h"
#include "ui/options.h"
#include "compat/console.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#else
#include "ui/audio_out.h"

#include <chrono>
#include <thread>
#endif

namespace {

constexpr u32 RATE = 44100;

// ---- MIDI 入力。輪っかも SysEx の受け皿も ui::midi_in が持っている
// （gui.exe と同じもの。**SysEx の入れ物を Windows へ渡す**のもそちら）

ui::midi_in g_midi;

#if defined(_WIN32)

// Ctrl+C や窓を閉じたとき。すぐには死なず、止めて NVRAM を残してから終わる
std::atomic<bool> g_quit{false};
std::atomic<bool> g_done{false};

BOOL WINAPI on_console_ctrl(DWORD type)
{
	g_quit.store(true);
	// 窓を閉じられたときは、ここから戻ると数秒で殺される。後始末を待つ
	if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
		for (int i = 0; i < 400 && !g_done.load(); i++)
			Sleep(10);
	return TRUE;
}
void list_midi_inputs()
{
	const UINT n = midiInGetNumDevs();
	if (!n) {
		std::printf(CLI_T("No MIDI input found\n", "MIDI 入力が見つからない\n"));
		return;
	}
	std::printf(CLI_T("MIDI inputs:\n", "MIDI 入力:\n"));
	for (UINT i = 0; i < n; i++) {
		MIDIINCAPSA caps{};
		if (midiInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			std::printf("  %u: %s\n", i, caps.szPname);
	}
}
#else
void list_midi_inputs()
{
	const std::vector<std::string> names = ui::midi_in::list();
	if (names.empty()) {
		std::printf(CLI_T("No MIDI input found\n", "MIDI 入力が見つからない\n"));
		return;
	}
	std::printf(CLI_T("MIDI inputs:\n", "MIDI 入力:\n"));
	for (size_t i = 0; i < names.size(); i++)
		std::printf("  %zu: %s\n", i, names[i].c_str());
}
#endif

// Counted only to decide which input to open by default
int midi_input_count()
{
#if defined(_WIN32)
	return int(midiInGetNumDevs());
#else
	return int(ui::midi_in::list().size());
#endif
}

#if defined(_WIN32)
// 音声スレッドを MMCSS へ登録する。SetThreadPriority だけでは、
// 他の仕事のために数十ミリ秒まとめて止められることがある
struct mmcss_guard {
	HANDLE h = nullptr;
	mmcss_guard()
	{
		DWORD idx = 0;
		h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
		if (h) AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
	}
	~mmcss_guard() { if (h) AvRevertMmThreadCharacteristics(h); }
};
#endif


#if defined(_WIN32)

// ---- 音を作る側。どちらの出力方式からもこれを呼ぶ

struct generator {
	mu2000 &mu;
	std::vector<s16> *rec;          // 確認用の録音。要らなければ nullptr
	LARGE_INTEGER freq{};
	u64 busy_ticks = 0, produced = 0, late = 0, worst_ticks = 0;
	// 取りこぼしの判定に使う「一杯ぶん」の長さ。出力方式が決める
	u32 cushion_frames = 0;
	u64 starved = 0;      // デバイスの残量がゼロになった回数（本当の枯渇）

	generator(mu2000 &m, std::vector<s16> *r) : mu(m), rec(r)
	{
		QueryPerformanceFrequency(&freq);
	}

	// n サンプルぶん作って out に書く（16bit 2ch のインタリーブ）
	void fill(s16 *out, u32 n)
	{
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);

		// 溜まっている MIDI を音源へ。実機と同じく 31250bps の直列で流れる
		u8 b;
		while (g_midi.pop(b))
			mu.midi_in(b);

		for (u32 i = 0; i < n; i++) {
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
			l = l * 32768 / mu2000::DAC_FULL_SCALE;
			r = r * 32768 / mu2000::DAC_FULL_SCALE;
			out[i * 2 + 0] = s16(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
			out[i * 2 + 1] = s16(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
		}

		QueryPerformanceCounter(&t1);
		const u64 one = u64(t1.QuadPart - t0.QuadPart);
		busy_ticks += one;
		if (one > worst_ticks) worst_ticks = one;
		// 取りこぼすのは、1 回の生成が「溜めてある量」を超えたとき。
		// 頼まれた n は回ごとに変わるので、n と比べても意味がない
		if (cushion_frames && double(one) / freq.QuadPart > double(cushion_frames) / RATE)
			late++;

		if (rec)
			rec->insert(rec->end(), out, out + size_t(n) * 2);
		produced += n;
	}

	void report(u32 period_frames) const
	{
		const double audio = double(produced) / RATE;
		const double busy  = double(busy_ticks) / freq.QuadPart;
		std::printf(CLI_T("  %.0f s elapsed  MIDI %llu bytes  CPU load %.1f%%\n", "  %.0f 秒経過  MIDI %llu バイト  CPU 使用率 %.1f%%\n"),
		            audio, (unsigned long long)g_midi.bytes(), 100.0 * busy / audio);
		std::printf(CLI_T("     late %llu times, worst render %.1f ms (headroom %.1f ms)\n", "     間に合わなかった %llu 回、生成の最悪 %.1f ms（余裕は %.1f ms）\n"),
		            (unsigned long long)starved, 1000.0 * worst_ticks / freq.QuadPart,
		            1000.0 * cushion_frames / RATE);
	}
};

// ---- WASAPI 共有モード。イベント駆動

// ---- WASAPI 共有モード。ui::audio_out に任せる
//
// 以前はここに WASAPI の手順を直に書いていたが、gui.exe 側（ui::audio_out）と
// 二重になっていた。**標本化周波数の変換を自分でやる**ようにした分が
// 片方にしか入らないのは困るので、こちらもそちらを使う。

int run_wasapi(generator &gen, double seconds, int latency_ms, bool exclusive,
               const char *dump_dev, const char *audio_dev, bool raw)
{
	ui::audio_out out;
	std::string err;
	if (dump_dev)
		out.set_capture(dump_dev);
	if (!out.start(latency_ms, [&gen](s16 *o, u32 n) { gen.fill(o, n); }, err, exclusive,
	               audio_dev ? audio_dev : "", raw)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}

	std::printf(CLI_T("Audio output: %s\n", "音声の出口: %s\n"), out.device_name().c_str());
	std::printf("%s\n", out.format_line().c_str());
	std::printf("MMCSS: %s\n", out.mmcss() ? CLI_T("registered as Pro Audio", "Pro Audio で登録した")
	                                        : CLI_T("could not register (dropouts are likely)", "登録できず（途切れやすい）"));
	if (seconds > 0.0)
		std::printf(CLI_T("Stops after %.1f s\n", "%.1f 秒で終了\n"), seconds);
	else
		std::printf(CLI_T("Ctrl+C to stop\n", "Ctrl+C で終了\n"));

	// 取りこぼしの判定に使う「一杯ぶん」。デバイス側の長さを 44100 側に直す
	gen.cushion_frames = u32(out.target_ms() * RATE / 1000.0);

	u64 shown = 0;
	while (!g_quit.load() && (seconds <= 0.0 || gen.produced < u64(seconds * RATE))) {
		Sleep(20);
		if (!out.running()) {
			const std::string e = out.error();
			if (!e.empty())
				std::fprintf(stderr, "%s\n", e.c_str());
			break;
		}
		if (gen.produced - shown >= u64(RATE) * 5) {
			shown = gen.produced;
			gen.starved = out.late();
			gen.report(gen.cushion_frames);
			std::printf("     %s\n", out.latency_line().c_str());
		}
	}

	gen.starved = out.late();
	std::printf(CLI_T("Latency: %s\n", "待ち時間: %s\n"), out.latency_line().c_str());
	std::printf(CLI_T("Least headroom: %.1f ms, late %llu times\n", "余裕の最小: %.1f ms、間に合わなかった %llu 回\n"),
	            out.slack_min_ms(), (unsigned long long)out.late());
	out.stop();
	return 0;
}

// ---- WinMM waveOut。素直だが待ち時間を詰められない

int run_waveout(generator &gen, double seconds, int frames, int buffers)
{
	WAVEFORMATEX fmt{};
	fmt.wFormatTag      = WAVE_FORMAT_PCM;
	fmt.nChannels       = 2;
	fmt.nSamplesPerSec  = RATE;
	fmt.wBitsPerSample  = 16;
	fmt.nBlockAlign     = 4;
	fmt.nAvgBytesPerSec = RATE * 4;

	HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	HWAVEOUT hwo = nullptr;
	if (waveOutOpen(&hwo, WAVE_MAPPER, &fmt, DWORD_PTR(done), 0,
	                CALLBACK_EVENT) != MMSYSERR_NOERROR) {
		std::fprintf(stderr, CLI_T("Cannot open the audio device\n", "音声デバイスを開けない\n"));
		return 1;
	}

	std::vector<std::vector<s16>> pcm(buffers, std::vector<s16>(size_t(frames) * 2));
	std::vector<WAVEHDR> hdr(buffers);
	for (int i = 0; i < buffers; i++) {
		hdr[i] = {};
		hdr[i].lpData         = reinterpret_cast<LPSTR>(pcm[i].data());
		hdr[i].dwBufferLength = DWORD(pcm[i].size() * 2);
		waveOutPrepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
	}

	std::printf(CLI_T("waveOut  latency %.1f ms (%d samples x %d buffers)\n", "waveOut  待ち時間 %.1f ms（%d サンプル × %d 枚）\n"),
	            1000.0 * frames * buffers / RATE, frames, buffers);
	if (frames < 1024)
		std::printf(CLI_T("Warning: one buffer is only %.1f ms. waveOut collects buffers only every 20 ms\n"
"      or so, so anything shorter starves and the sound breaks up\n", "警告: 1 枚が %.1f ms しかない。waveOut は 20ms 前後の間隔でしか\n"
		            "      回収しないので、これより短いと供給が追いつかず細切れになる\n"),
		            1000.0 * frames / RATE);
	if (seconds > 0.0)
		std::printf(CLI_T("Stops after %.1f s\n", "%.1f 秒で終了\n"), seconds);
	else
		std::printf(CLI_T("Ctrl+C to stop\n", "Ctrl+C で終了\n"));

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	mmcss_guard mmcss;
	gen.cushion_frames = u32(frames) * u32(buffers);

	// 先に全枚を投入し、以後は投入した順に完了を待つ。空きを探し回ると、
	// waveOutWrite の直後でまだ WHDR_INQUEUE が立っていない枚を選び直して
	// 再生中の枚を上書きしうる
	auto emit = [&](int i) {
		gen.fill(pcm[i].data(), u32(frames));
		waveOutWrite(hwo, &hdr[i], sizeof(WAVEHDR));
	};
	for (int i = 0; i < buffers; i++)
		emit(i);

	int next = 0;
	while (!g_quit.load() && (seconds <= 0.0 || gen.produced < u64(seconds * RATE))) {
		while (!(hdr[next].dwFlags & WHDR_DONE))
			WaitForSingleObject(done, 100);
		emit(next);
		next = (next + 1) % buffers;
		if (gen.produced % (RATE * 5) < u64(frames))
			gen.report(u32(frames));
	}

	waveOutReset(hwo);
	for (int i = 0; i < buffers; i++)
		waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
	waveOutClose(hwo);
	CloseHandle(done);
	return 0;
}

#else  // macOS

// ---- CoreAudio. The default route on macOS
//
// Unlike the Windows side it spins no thread of its own: ui::audio_out (a
// CoreAudio AudioUnit) calls into this for exactly what the device asked for.
// The "keep no clock of your own" design applies here unchanged.
int run_coreaudio(mu2000 &mu, double seconds, int latency_ms, std::vector<s16> *rec,
                  u64 &produced, double &busy_sec, bool exclusive, const char *dump_dev,
                  const char *audio_dev)
{
	ui::audio_out out;
	std::string err;
	if (dump_dev)
		out.set_capture(dump_dev);

	const bool ok = out.start(latency_ms, [&](s16 *dst, u32 frames) {
		// 溜まっている MIDI を音源へ。実機と同じく 31250bps の直列で流れる
		u8 b;
		while (g_midi.pop(b))
			mu.midi_in(b);

		for (u32 i = 0; i < frames; i++) {
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
			l = l * 32768 / mu2000::DAC_FULL_SCALE;
			r = r * 32768 / mu2000::DAC_FULL_SCALE;
			dst[i * 2 + 0] = s16(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
			dst[i * 2 + 1] = s16(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
		}
		// Only for --wav. It is a research aid, so allocating here does not matter
		if (rec)
			rec->insert(rec->end(), dst, dst + size_t(frames) * 2);
	}, err, exclusive, audio_dev ? audio_dev : "");

	if (!ok) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}

	std::printf(CLI_T("Audio output: %s\n", "音声の出口: %s\n"), out.device_name().c_str());
	// A refused claim still plays: hog mode is a request, and another application
	// may be holding the device
	if (exclusive)
		std::printf(CLI_T("Exclusive use: %s\n", "独り占め: %s\n"), out.exclusive() ? CLI_T("got it", "取れた") : CLI_T("not available", "取れなかった"));
	// buffer_frames() is the last block, and the first ones are not the ones that
	// follow: the engine opens the device at its own rate and inserts its converter
	// a moment later, so read straight after start() a 96 kHz device reports 1920
	// frames against the machine's 44100 - 43.5 ms of a buffer that is 20 ms. Wait
	// for the audio to run and the block size to settle.
	for (int i = 0; i < 20 && out.produced() == 0; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	u32 block = out.buffer_frames();
	for (int i = 0; i < 4; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (out.buffer_frames() == block)
			break;
		block = out.buffer_frames();
	}
	std::printf(CLI_T("CoreAudio  latency %.1f ms (%u samples)\n", "CoreAudio  待ち時間 %.1f ms（%u サンプル）\n"),
	            1000.0 * block / RATE, block);
	if (seconds > 0.0)
		std::printf(CLI_T("Stops after %.1f s\n", "%.1f 秒で終了\n"), seconds);
	else
		std::printf(CLI_T("Ctrl+C to stop\n", "Ctrl+C で終了\n"));

	const u64 period = u64(RATE) * 5;
	u64 next = period;
	while (seconds <= 0.0 || out.produced() < u64(seconds * RATE)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (out.produced() >= next) {
			std::printf(CLI_T("  %.0f s elapsed  MIDI %llu bytes  CPU load %.1f%%\n", "  %.0f 秒経過  MIDI %llu バイト  CPU 使用率 %.1f%%\n"),
			            double(out.produced()) / RATE,
			            (unsigned long long)g_midi.bytes(), out.cpu_percent());
			std::printf(CLI_T("     underruns %llu, worst render %.1f ms (%.1f ms buffered)\n", "     枯渇 %llu 回、生成の最悪 %.1f ms（溜めは %.1f ms ぶん）\n"),
			            (unsigned long long)out.starved(), out.worst_ms(),
			            1000.0 * out.buffer_frames() / RATE);
			next += period;
		}
	}

	produced = out.produced();
	busy_sec = out.cpu_percent() / 100.0 * (double(produced) / RATE);
	out.stop();

	// The capture is complete only after stop(), so it is written here rather
	// than by the audio_out itself
	if (dump_dev) {
		std::string cerr;
		if (out.write_capture(cerr))
			std::printf(CLI_T("Wrote what went to the device: %s (%.1f s)\n", "デバイスへ渡したものを書き出した: %s（%.1f 秒）\n"), dump_dev,
			            double(out.capture_frames()) / RATE);
		else
			std::fprintf(stderr, "%s\n", cerr.c_str());
	}
	return 0;
}

#endif // _WIN32

void write_wav(const char *path, const std::vector<s16> &pcm)
{
	// The header is ui/wav.h's, shared with the Apple and Linux backends and
	// with render; only the message after it is this tool's.
	std::string err;
	if (!ui::write_wav(path, pcm, err, RATE)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return;
	}
	std::printf(CLI_T("Wrote the recording: %s\n", "録音を書き出した: %s\n"), path);
}

} // namespace


int main(int argc, char **argv)
{
	smu2000::cli::init(argc, argv);       // -jp で日本語
	smu2000::init_console_utf8();  // Windows defaults to CP932, which garbles the output

	int  midi_dev = -1;
	int  frames = 1024;     // waveOut のときの 1 枚（23.2ms）
	int  buffers = 3;
	// WASAPI の待ち時間。0 を渡すと Windows の最小周期（この環境で 23.5ms）に
	// なるが、それだと音源の山で 25 秒に 1 回ほど枯渇する。30ms なら 0 回。
	// これ以上詰めたければ音源をもっと速くするしかない
	int  latency_ms = 20;   // 溜める目標。実測の最悪 9.2ms + 余裕
	ui::output_options out_opts;
	const char *dump_dev = nullptr;   // デバイスへ渡したものをそのまま書き出す
	bool raw = false;                 // エンジンの信号処理を飛ばす
	double seconds = 0.0;   // 0 なら Ctrl+C まで
	bool nomidi = false, use_waveout = false, single = false;
	ui::engine_options eng_opts;
	const char *wav = nullptr;
	std::string dir;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--list")) {
			list_midi_inputs();
			std::printf(CLI_T("\nAudio outputs:\n", "\n音声の出口:\n"));
			const std::vector<std::string> outs = ui::audio_out::list();
			for (size_t k = 0; k < outs.size(); k++)
				std::printf("  %zu: %s\n", k, outs[k].c_str());
			std::printf(CLI_T("  give --audio part of a name to play there\n", "  --audio に名前の一部を渡すと、そこへ出す\n"));
			return 0;
		}
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) midi_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--buffers") && i + 1 < argc) buffers = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) latency_ms = std::atoi(argv[++i]);
		else if (ui::consume_output_option(argv, argc, i, out_opts)) {}
		else if (!std::strcmp(argv[i], "--nomidi")) nomidi = true;
		else if (!std::strcmp(argv[i], "--dump-dev") && i + 1 < argc) dump_dev = argv[++i];
		else if (!std::strcmp(argv[i], "--raw")) raw = true;
		else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
		else if (!std::strcmp(argv[i], "--waveout")) use_waveout = true;
		else if (!std::strcmp(argv[i], "--nomidi")) nomidi = true;
		else if (ui::consume_engine_option(argv[i], eng_opts)) {}
		else if (!std::strcmp(argv[i], "--single"))
			single = true;
		else if (!std::strcmp(argv[i], "-v")) smu2000::g_verbose = true;
		else if (dir.empty()) dir = argv[i];
	}
	if (dir.empty()) {
		std::fprintf(stderr, "%s", CLI_T(
			"Usage: live <rom directory> [options]\n"
			"       live --list              list the MIDI inputs and audio outputs\n"
			"\n"
			"Plays MIDI from an input device through the emulated MU2000, in real time.\n"
			"\n"
			"  --midi <number>      MIDI input to listen to (see --list; default: the first one)\n"
			"  --nomidi             open no MIDI input\n"
			"  --audio <name>       audio output whose name contains this text (default: the system's)\n"
			"  --latency <ms>       how much audio to keep buffered (default 20)\n"
			"  --exclusive          take the audio device for ourselves, for less latency\n"
			"  --seconds <n>        stop after n seconds (default: run until Ctrl+C)\n"
			"  --wav <file>         also record what is played to a WAV file\n"
			"  --factory            forget the remembered settings and start from factory defaults\n"
			"  --fast-midi          deliver MIDI at once instead of at the DIN rate of 3125 bytes/s\n"
			"  --native-fx          run the effects in C++ instead of emulating the effect DSP (lighter)\n"
			"  --native-engine      run the voices in C++ where possible (lighter still)\n"
			"  --single             keep everything on one thread\n"
			"  --waveout            (Windows) use waveOut instead of WASAPI; --frames <n> --buffers <n> size it\n"
			"  -v                   print more while running\n"
			"  -jp                  messages in Japanese\n",
			"使い方: live <rom ディレクトリ> [オプション]\n"
			"       live --list              MIDI 入力と音声の出口の一覧\n"
			"\n"
			"入力デバイスから来た MIDI を、エミュレートした MU2000 で、その場で鳴らす。\n"
			"\n"
			"  --midi <番号>        聞く MIDI 入力（--list で見る。既定は最初のもの）\n"
			"  --nomidi             MIDI 入力を開かない\n"
			"  --audio <名前>       名前にこの文字を含む音声の出口（既定はシステムのもの）\n"
			"  --latency <ミリ秒>   溜めておく音の長さ（既定 20）\n"
			"  --exclusive          デバイスを独り占めして待ち時間を詰める\n"
			"  --seconds <n>        n 秒で終わる（既定は Ctrl+C まで）\n"
			"  --wav <ファイル>     鳴らした音を WAV にも録る\n"
			"  --factory            覚えている設定を捨てて工場出荷状態で起動する\n"
			"  --fast-midi          MIDI を DIN の速さ（1 秒に 3125 バイト）で待たずに、すぐ渡す\n"
			"  --native-fx          エフェクトを DSP のエミュレートでなく C++ で回す（軽い）\n"
			"  --native-engine      音も、できる所は C++ で回す（さらに軽い）\n"
			"  --single             全部を 1 つのスレッドで回す\n"
			"  --waveout            （Windows）WASAPI の代わりに waveOut を使う。--frames <n> --buffers <n> で大きさ\n"
			"  -v                   動いている間の表示を増やす\n"
			"  -jp                  表示を日本語にする\n"));
		return 1;
	}

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
		std::fprintf(stderr, CLI_T("Warning: %s\n", "警告: %s\n"), mu.error().c_str());

	mu.set_threaded(!single);
	if (std::getenv("SMU2000_VOICECACHE"))
		eng_opts.voicecache = 1;
	ui::apply_engine_options(mu, eng_opts);
	if (out_opts.factory)
		std::printf(CLI_T("Starting from factory defaults (the remembered settings are overwritten on exit)\n", "工場出荷状態で起動する（覚えていた設定は終わるときに上書きされる）\n"));
	else if (smu2000::nvram::load(mu))
		std::printf(CLI_T("Settings: %s\n", "設定: %s\n"), smu2000::nvram::path(mu).c_str());
	mu.reset();

	// 起動を待つ。実機と同じで、ここを待たないと音色指定が捨てられる
	std::printf(CLI_T("Booting...", "起動中..."));
	std::fflush(stdout);
	{
		const size_t limit = size_t(30.0 * RATE);
		size_t i = 0;
		s32 l, r;
		for (; i < limit && !mu.midi_ready(); i++)
			mu.run_sample(l, r);
		if (i >= limit) {
			std::fprintf(stderr, CLI_T("\nIt did not boot\n", "\n起動しなかった\n"));
			return 1;
		}
		std::printf(CLI_T(" %.2f s\n", " %.2f 秒\n"), double(i) / RATE);
	}
	// 起動が終わってから入れる（起動には firmware が要る）
	if (eng_opts.native_engine) {
		mu.set_native_engine(eng_opts.native_engine);
		if (eng_opts.voicecache &&
		    smu2000::voicecache::load(mu, smu2000::voicecache::key(mu)))
			std::printf(CLI_T("Voice cache: %d voices from the saved copy\n", "写し取り: %d 音色を前の写しから\n"), int(mu.native_cal_count()));
		std::printf(CLI_T("Native engine: the SH-2 runs only when needed\n", "native の口: SH-2 は要るときだけ回す\n"));
	}

	// ---- MIDI 入力
	if (nomidi) midi_dev = -1;
	else if (midi_dev < 0 && midi_input_count() > 0)
		midi_dev = 0;
	if (midi_dev >= 0) {
		std::string merr;
		if (!g_midi.open(midi_dev, merr)) {
			std::fprintf(stderr, CLI_T("MIDI input %d: %s\n", "MIDI 入力 %d: %s\n"), midi_dev, merr.c_str());
			return 1;
		}
		std::printf(CLI_T("MIDI input: %d: %s\n", "MIDI 入力: %d: %s\n"), midi_dev, g_midi.device_name().c_str());
	} else
		std::printf(CLI_T("No MIDI input (audio runs, but nothing will play)\n", "MIDI 入力なし（音は出るが何も鳴らない）\n"));

	std::vector<s16> rec;
	u64 produced = 0;
	double busy_sec = 0.0;
	int rc = 1;
#if defined(_WIN32)
	generator gen(mu, wav ? &rec : nullptr);
	SetConsoleCtrlHandler(on_console_ctrl, TRUE);

	rc = use_waveout ? run_waveout(gen, seconds, frames, buffers)
	                 : run_wasapi(gen, seconds, latency_ms, out_opts.exclusive, dump_dev,
	                              out_opts.audio_dev, raw);
	produced = gen.produced;
	busy_sec = double(gen.busy_ticks) / gen.freq.QuadPart;
#else
	// Windows-only options: say so rather than silently doing something else
	if (use_waveout)
		std::fprintf(stderr, CLI_T("Note: --waveout is Windows only. macOS uses CoreAudio\n", "注意: --waveout は Windows 専用。macOS では CoreAudio を使う\n"));
	// --raw is the one that stays Windows-only: on macOS the format conversion
	// happens inside the system rather than through a driver mixer, so there is
	// no engine in the path to bypass (doc/porting-macos.md)
	if (raw)
		std::fprintf(stderr, CLI_T("Note: --raw is Windows only. It means nothing on macOS\n", "注意: --raw は Windows 専用。macOS では意味がない\n"));
	rc = run_coreaudio(mu, seconds, latency_ms, wav ? &rec : nullptr, produced, busy_sec,
	                   out_opts.exclusive, dump_dev, out_opts.audio_dev);
#endif

	if (wav && !rec.empty())
		write_wav(wav, rec);
	g_midi.close();

	// 音はもう止まっている。ここで機械に触ってよい
	if (!smu2000::nvram::save(mu))
		std::fprintf(stderr, CLI_T("Could not save the settings: %s\n", "設定を残せなかった: %s\n"), smu2000::nvram::path(mu).c_str());

	// (both platforms arrive here through the same two counters: the Windows
	// side reads them off its generator, macOS gets them back from CoreAudio)
	const double audio = double(produced) / RATE;
	const double busy  = busy_sec;
	std::printf(CLI_T("Done. Rendered %.1f s in %.2f s (CPU load %.1f%%)  MIDI %llu bytes\n", "終了。%.1f 秒ぶんを %.2f 秒で生成（CPU 使用率 %.1f%%）  MIDI %llu バイト\n"),
	            audio, busy, audio > 0 ? 100.0 * busy / audio : 0.0,
	            (unsigned long long)g_midi.bytes());
#if defined(_WIN32)
	g_done.store(true);
#endif
	return rc;
}
