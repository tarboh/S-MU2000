// license:BSD-3-Clause
//
// WASAPI の音声出力を、自分のスレッドで回す。
//
// live.exe で詰めた形をそのまま切り出したもの。要点は
// **時計を自分で持たないこと**。デバイスが「N サンプルくれ」と言った分だけ
// fill を呼ぶ。GUI がある側ではメッセージループを止められないので、
// ここはスレッドに分かれている必要がある。
//
// **標本化周波数の変換は自分でやる。** デバイスは 48000Hz の float を
// 言ってくることが多いが、MU2000 は 44100Hz より他では動かない。変換を
// Windows に任せると既定の品質の変換器を通されるので、窓関数付き sinc
// （ui::resampler）で自分で変換してから渡す。
//
// **溜めは満杯にしない。** 待ち時間は「書いた音の前に溜まっている量」で
// 決まる。昔は起きるたびに溜めを満杯まで埋めていたので、溜めの長さが
// そのまま待ち時間になっていた。いまは target だけ溜めて、それ以上は
// 書かない。target は音源の最悪値より長くないと音が切れる
// （doc/todo.md 2 番、build/blocktime.exe で測れる）。
//
// macOS does it with CoreAudio instead: same contract, same shape (see the
// branch below), because an output AudioUnit hands us frames on its own
// real-time thread, so there is no worker thread on that side.
//
// Linux (issue #25) uses ALSA and takes the same shape as the macOS one: the
// device handle and the worker thread live in the impl, so the header does not
// have to name either (src/ui/audio_out_linux.cpp).

#ifndef S_MU2000_UI_AUDIO_OUT_H
#define S_MU2000_UI_AUDIO_OUT_H

#pragma once

#include "compat/mamecompat.h"
#include "audio_stream.h"
#include "cpu_meter.h"

#include <atomic>
#include <functional>
#include <string>

#if defined(__APPLE__) || defined(__linux__)
#include <memory>
#include <mutex>
#include <vector>
#else
#include <thread>
#include <vector>
#endif

namespace ui {

constexpr u32 AUDIO_RATE = 44100;

#if defined(__APPLE__) || defined(__linux__)

// macOS: a CoreAudio DefaultOutput AudioUnit calls the render callback on its
// own real-time HAL thread, so unlike the Windows side there is no worker thread
// here. The public shape is identical, so live and the GUI do not know which
// implementation they are talking to.
class audio_out
{
public:
	// 16bit 2ch インタリーブで frames サンプルぶん書く（44100Hz）
	using fill_fn = std::function<void(s16 *out, u32 frames)>;

	// Both of these are declared here and defined in the .cpp. With a pimpl that
	// is not optional: the compiler otherwise generates them here, where impl is
	// still incomplete, and unique_ptr refuses to delete an incomplete type
	audio_out();
	~audio_out();

	// 使える再生デバイスの名前。番号は挿し直すとずれるので、**名前で選ぶ**
	// (live --list prints this on both platforms, so macOS needs the same answer)
	static std::vector<std::string> list();
	static std::string default_device_name();
	// Set only while stopped; read stream_info() after start() completes.
	void set_stream_options(const audio_stream_options &s) { m_stream = s; }
	// By value, behind a lock: a recovery can move the device and rewrites this
	// from the watchdog's thread.
	audio_stream_info stream_info() const
	{
		const std::lock_guard<std::mutex> lock(m_info_lock);
		return m_info;
	}

	// latency_ms is the target amount to keep queued (0 or less leaves the
	// device's own buffer size alone). exclusive asks for hog mode, which is this
	// platform's counterpart of WASAPI's exclusive mode: while we hold the device
	// nothing else can play through it. device is part of a name, empty is the
	// system default.
	//
	// raw has no meaning here -- on this side the system does the format
	// conversion rather than a driver mixer, so there is nothing to bypass. It is
	// in the signature only so both platforms take the same call
	// exact is for menu selections: a disconnected name must not select a
	// different device merely because its name contains the old one.
	bool start(int latency_ms, fill_fn fill, std::string &err, bool exclusive = false,
	           const std::string &device = std::string(), bool raw = false, bool exact = false);

	// The port that was actually opened, by name
	std::string device_name() const;

	// Whether the device was really taken for ourselves: false if hog mode was
	// not asked for, or if the device refused (something else holds it)
	bool exclusive() const;

#if defined(__APPLE__)
	// The HAL output unit's audio workgroup, for a parallel render thread
	// (Apple's parallel real-time threads pattern). Null when unavailable.
	// macOS only; Linux shares this header but has no workgroups.
	void *realtime_workgroup();
#endif

	// Keep what is handed to the audio unit, so it can be compared against a real
	// machine. Call before start(); write_capture() writes it as a WAV. Note that
	// on this platform CoreAudio converts to the device's own format *after* us,
	// so this is what we send rather than what the driver receives
	void set_capture(const std::string &path);
	u64 capture_frames() const;
	bool write_capture(std::string &err);

	void stop();

	// Progress. Written on the audio thread, safe to read from anywhere.
	//
	// Two counts, and they are not in the same unit. produced() is in 44100 Hz
	// machine frames, so dividing it by AUDIO_RATE gives seconds - which is what
	// live.cpp does, for its progress line and for the CPU-per-second in the
	// closing summary. buffer_frames() is the device's own buffer in the device's
	// own frames: the desktop back ends open the device at 44100, so the two
	// numbers are the same there, while the Apple back ends run against whatever
	// the device runs at (48000 on every iPhone) and 480 frames there is 10 ms,
	// not 10.9. So buffer_frames() is for reporting a frame count, as the iOS log
	// line does, and there is deliberately nothing to convert it with:
	// device_rate() below is the WASAPI back end's own, and the Apple back ends
	// keep the device rate to themselves. Arithmetic on produced() is safe;
	// arithmetic on buffer_frames() is not.
	u32 buffer_frames() const;
	bool running() const;
	u64 produced() const;
	u64 starved() const;
	// The CoreAudio render callback already runs at real-time priority, so this
	// is the counterpart of registering with MMCSS on Windows
	bool mmcss() const;
	// Average since start (for the closing summary) and the recent load (for the
	// display, one cpu_meter window at a time; issue #80)
	double cpu_percent() const;
	double cpu_recent() const;
	double worst_ms() const;

private:
	audio_stream_options m_stream;
	audio_stream_info m_info;
	mutable std::mutex m_info_lock;   // guards m_info against the watchdog
	struct impl;
	std::unique_ptr<impl> m_impl;

	// Outside impl: stop() throws impl away and the capture has to outlive it. The
	// render callback appends through a pointer in impl, so it stays the only
	// writer.
	std::vector<s16> m_cap;
	std::string      m_cap_path;
	// What the capture holds, for the header write_wav puts at the front of it.
	// Linux records at whatever the device opened at; the Apple back ends know it
	// in the render core, so these are declared for Linux alone.
#if defined(__linux__)
	bool             m_capturing = false;
	u32 m_capture_rate = AUDIO_RATE, m_capture_channels = 2;
#endif
};

#else

class audio_out
{
public:
	// 16bit 2ch インタリーブで frames サンプルぶん書く（44100Hz）
	using fill_fn = std::function<void(s16 *out, u32 frames)>;

	~audio_out() { stop(); }

	// 使える再生デバイスの名前。番号は挿し直すとずれるので、**名前で選ぶ**
	static std::vector<std::string> list();
	static std::string default_device_name();
	// Set only while stopped; read stream_info() after start() completes.
	void set_stream_options(const audio_stream_options &s) { m_stream = s; }
	const audio_stream_info &stream_info() const { return m_info; }

	// latency_ms は**溜める目標の長さ**。0 以下ならデバイスの周期 2 つぶん。
	// exclusive なら Windows の混ぜ合わせを通さない（他のアプリは鳴らせない）。
	// device は名前の一部（空なら Windows の既定）。**既定は勝手に変わる**ので、
	// 聞いている口が決まっているなら指定したほうがよい
	// exact disables the CLI's substring matching for a menu selection.
	bool start(int latency_ms, fill_fn fill, std::string &err, bool exclusive = false,
	           const std::string &device = std::string(), bool raw = false, bool exact = false);

	// 実際に開いた口の名前
	std::string device_name() const { return m_dev_name; }
	void stop();

	// 具合。すべて音声スレッドが書き、他所から読んでよい
	u32 buffer_frames() const { return m_buffer_frames.load(); }
	u64 produced() const      { return m_produced.load(); }
	// 間に合わなかった回数。**デバイスが待たされた（音が切れた）回数**で、
	// 「起きたときに残量ゼロ」ではない。残量ゼロは溜めを詰めれば常に起きる
	u64 late() const          { return m_late.load(); }
	// 起きたときに残っていた最小の量（ミリ秒）。余裕の実測
	double slack_min_ms() const;
	// MMCSS（Pro Audio）に登録できたか。だめだと途切れやすくなる
	bool mmcss() const        { return m_mmcss.load(); }
	bool running() const      { return m_running.load(); }
	std::string error() const { return m_err; }
	// 起動してからの平均（終わりの集計用）と、直近の重さ（画面の表示用。cpu_meter の窓ごと、issue #80）
	double cpu_percent() const;
	double cpu_recent() const { return m_cpu_meter.value(); }
	double worst_ms() const;

	// デバイスが言ってきた形式。開いた後に読む
	u32 device_rate() const     { return m_dev_rate.load(); }
	u32 device_channels() const { return m_dev_channels.load(); }
	bool converting() const     { return m_converting.load(); }
	bool exclusive() const      { return m_exclusive.load(); }
	// エンジンの信号処理を飛ばせたか（共有モードのみ）
	bool raw() const            { return m_raw.load(); }
	double period_ms() const    { return m_period_ms.load(); }
	double buffer_ms() const;

	// ---- 待ち時間の内訳（すべてミリ秒）
	//
	// 書いた音が鳴るまで = 前に溜まっている量 + デバイスの分。
	// 前に溜まっている量はこちらが決められる。デバイスの分は決められない
	double queue_ms() const;        // 起きたときに溜まっていた量（平均）
	double queue_worst_ms() const;  // 同じ（最悪）
	double target_ms() const;       // 溜める目標
	double device_ms() const { return m_stream_ms.load(); }  // GetStreamLatency
	// 書いた音が鳴るまでの見込み
	double output_ms() const { return queue_ms() + device_ms(); }

	// **書いたのに、まだ鳴っていない量。** IAudioClock の再生位置との差なので、
	// こちらの溜めだけでなく**ドライバが抱えている分も入る**。
	// 溜めが 20ms なのにこれが 100ms なら、残りはドライバの中にある
	double inflight_ms() const;
	double inflight_worst_ms() const;

	// デバイスへ渡したバイト列をそのまま書き出す（切り分け用）。
	// start() の前に呼ぶ。止めるときに WAV として閉じる
	void set_capture(const std::string &path) { m_cap_path = path; }

	// 人が読む行
	std::string format_line() const;
	std::string latency_line() const;

private:
	audio_stream_options m_stream;
	audio_stream_info m_info;
	void run(int latency_ms, bool exclusive);

	fill_fn           m_fill;
	std::thread       m_thread;
	std::atomic<bool> m_quit{false};
	std::atomic<bool> m_running{false};
	std::string       m_err;

	std::atomic<u32> m_buffer_frames{0};
	std::atomic<u64> m_produced{0}, m_late{0};
	std::atomic<u64> m_slack_min{~u64(0)};
	std::atomic<u64> m_busy_ticks{0}, m_worst_ticks{0};
	cpu_meter        m_cpu_meter;   // 直近の重さ（音の時間で窓を切る）
	std::atomic<bool> m_mmcss{false};
	// 立ち上がりの首尾。**メンバに置くこと。** スレッドは run() を抜けた後に
	// ここへ書くので、start() のローカルに置くと宙ぶらりんの参照になる
	std::atomic<int>  m_start_state{0};   // 0 待ち / 1 動いた / 2 だめ
	std::atomic<u32> m_dev_rate{AUDIO_RATE}, m_dev_channels{2};
	std::atomic<bool> m_converting{false};
	std::atomic<bool> m_exclusive{false};
	std::atomic<double> m_period_ms{0.0}, m_stream_ms{0.0};
	std::atomic<int>  m_dev_bits{16};
	std::atomic<bool> m_dev_float{false};
	std::atomic<u32>  m_target_frames{0};
	std::atomic<u64>  m_queue_sum{0}, m_queue_n{0}, m_queue_worst{0};
	std::atomic<u64>  m_inflight_sum{0}, m_inflight_n{0}, m_inflight_worst{0};
	std::string       m_cap_path;
	std::string       m_dev_name, m_want_dev;
	bool              m_want_raw = false;
	bool              m_exact_dev = false; // UI selections cannot fall back to substring matches
	std::atomic<bool> m_raw{false};
	s64 m_qpc_freq = 1;
};

#endif // __APPLE__ || __linux__

} // namespace ui

#endif // S_MU2000_UI_AUDIO_OUT_H
