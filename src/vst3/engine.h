// license:BSD-3-Clause
//
// VST3 プラグインの中身。MU2000 の面倒を全部ここで見る。
//
//   ・ROM の置き場を探す
//   ・起動（4 秒ぶんの空回し）を別スレッドで済ませる
//   ・ホストの標本化周波数へ変換する（MU2000 は 44100 固定）
//
// plugin.cpp からはこれだけを触る。VST3 の型は一切出てこない。

#ifndef S_MU2000_VST3_ENGINE_H
#define S_MU2000_VST3_ENGINE_H

#pragma once

#include "ui/bridge.h"
#include "ui/cpu_meter.h"
#include "ui/driver.h"
#include "ui/resampler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class mu2000;
namespace xg { struct param; }

namespace smu2000 {
namespace vst3 {

// MU2000 が動く唯一の周波数
constexpr double NATIVE_RATE = 44100.0;

// MIDI の 1 メッセージの長さ。先頭のバイトで決まる。
// システムエクスクルーシブ（0xf0）は終わりのバイトまで数えないと分からないので 1 を返す
inline int midi_length(uint8_t status)
{
	switch (status & 0xf0) {
	case 0xc0: case 0xd0: return 2;
	case 0xf0:
		switch (status) {
		case 0xf1: case 0xf3: return 2;
		case 0xf5:            return 2;   // ケーブルメッセージ（口の切り替え、mu2000::midi_in）
		case 0xf2:            return 3;
		default:              return 1;
		}
	default: return 3;
	}
}

// **音源を初期値に戻す SysEx か**（GM・GM2 On、GS リセット、XG System On・All Parameter Reset）。
// 頭の F0 はあってもなくてもよい。
//
// プラグインは、同じ時刻に来たメッセージのうち**リセットを先に**流す（issue #51）。
// VST3 にはコントロールチェンジ・プログラムチェンジのイベントが無く、ホストはパラメータの
// 変化として別に渡してくる。foo_midi のように SysEx をチャンネルメッセージと別に渡すホストもある。
// どちらも同じ時刻での前後が失われ、曲頭の「XG System On → 音色の指定」が
// 「音色の指定 → XG System On」になって、リセットが音色を全部消していた（全パートがピアノになる）。
// 曲の中でリセットが同じ時刻のほかのメッセージより後ろに来る意味はまず無いので、先に出してよい
inline bool is_reset_sysex(const uint8_t *p, size_t n)
{
	if (n && p[0] == 0xf0) {
		p++;
		n--;
	}
	auto is = [&](std::initializer_list<int> want, int any_low_nibble_at = -1) {
		if (n < want.size())
			return false;
		size_t i = 0;
		for (int w : want) {
			const uint8_t v = int(i) == any_low_nibble_at ? uint8_t(p[i] & 0xf0) : p[i];
			if (w >= 0 && v != w)
				return false;
			i++;
		}
		return true;
	};
	return is({ 0x7e, -1, 0x09, 0x01 }) || is({ 0x7e, -1, 0x09, 0x03 }) ||          // GM / GM2 On
	       is({ 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00 }, 1) ||                     // XG System On
	       is({ 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7f, 0x00 }, 1) ||                     // XG All Parameter Reset
	       is({ 0x41, -1, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41 });                // GS Reset
}

enum class status {
	loading,   // ROM を読んで起動している最中。音は出ない
	ready,
	failed,    // ROM が見つからないなど。message() に理由が入る
};

class engine
{
public:
	engine();
	~engine();
	bool m_voicecache = false;      // plugin.ini の voicecache=1

	// ROM を探して読み、起動するまでを別スレッドで進める。すぐ返る。
	// 起動は 2 度とやらせない（m_boot_once が守る。2 度 boot を走らせると
	// 動き中の機械を差し替えてしまう）。待ちたければ wait_ready を
	void start();
	// 起動が終わるまで待つ。**DAW の本スレッドからだけ**呼ぶこと。
	// 待ちきれずに時間切れなら false。始まっていなければ始めてから待つ
	bool wait_ready(int ms);
	// 止めておく・戻す。IPluginBase::terminate / initialize から（**音声スレッドが回っていないとき**）。
	// park は起動を待ちきってから、スレーブの別スレッドを止める。この DLL のコードを走るスレッドを
	// 残さないため（ホストが本体を手放さないまま DLL を外すと、残ったスレッドが消えたコードを走って落ちる）。
	// unpark は止めたものを戻す
	void park();
	void unpark();
	// まだ生きている engine を全部 park する。ExitDll（DLL を外す直前）から
	static void park_all();

	status state() const { return m_state.load(std::memory_order_acquire); }
	// state() が failed のときの理由。ready でも「代用品を使った」等が入る
	std::string message() const;

	// ホスト側の標本化周波数。44100 ちょうどなら変換を通さない
	void set_output_rate(double rate);
	// 変換のぶんだけ音が遅れる。ホストに申告する
	uint32_t latency_samples() const { return m_latency; }

	// MIDI を 1 メッセージ流す。実機と同じく 31250bps の直列に崩される。
	// 起動が終わっていない間に来たものは**落さず**溜めておいて、終わってから
	// fill() が順番どおりに流す（issue #19）
	// port は 0 が MIDI IN A（パート 1-16）、1 が B（17-32）、2 が C（33-48）、3 が D（49-64）
	void midi(const uint8_t *bytes, size_t n, int port = 0);
	// オールサウンドオフ + オールノートオフを流す。mask は口ごとのチャンネルのビット
	// （bit 0 が 1ch）で、ports 個ぶん並べて渡す。全チャンネルに流すと 1 口あたり
	// 192 バイト＝31250bps で 61ms かかり、そのあとに続く音が丸ごと遅れるので、
	// 鳴らした覚えのあるチャンネルだけに絞る
	void all_notes_off(const uint16_t *mask, int ports);

	// MIDI OUT. Takes what the firmware sent out of the hardware OUT jack
	// (replies to XG queries and the like). Call from the same thread as
	// fill(), after fill(). Returns bytes written into dst (0 when empty)
	size_t midi_out(uint8_t *dst, size_t max);

	// n サンプルぶん作る。左右は別々の配列（VST3 はそういう渡し方をする）。
	// in_l / in_r はホストの周波数で n サンプルぶんの A/D INPUT（無ければ nullptr）
	void fill(float *left, float *right, int n, const float *in_l = nullptr, const float *in_r = nullptr);

	// パネルの画面と触れ合う口。ボタンは画面から、LCD の写しはこちらから
	ui::bridge &panel() { return m_bridge; }

	// firmware のワーク RAM から XG の値を直接写す。echo も直列も使わないので
	// その場で 1 枚取れる。状態を戻した直後の Automation の種に使う
	// （automation_host.h の seed_values）。**音声の糸から呼ぶ**ので錠は
	// try_lock — 保存中で取れなければ false を返し、呼んだ側が bridge の
	// 写し（read_xg）へ落ちる。音声スレッドを待たせない（m_machine の注意）
	bool copy_xg_now(ui::xg_snapshot &ram)
	{
		std::unique_lock<std::mutex> guard(m_machine, std::try_to_lock);
		if (!guard.owns_lock() || state() != status::ready || !m_mu)
			return false;
		ui::driver::copy_xg(*m_mu, ram);
		return true;
	}

	// bridge に載っている XG の写しを今の RAM から作り直す。状態を戻した直後に
	// 呼ぶと、音声の糸が read_xg で古い写しを引いて種を潰す事故が消える
	void publish_xg_now()
	{
		std::lock_guard<std::mutex> guard(m_machine);
		if (state() == status::ready && m_mu)
			m_drv.publish_xg(*m_mu, m_bridge);
	}

	// **firmware を走らせない口**（doc/native-engine.md）の入切。
	// gui.exe の F4 と同じで、**切り替えは音声の糸が fill() の頭で行う**。
	// 入れ直すと写し取りは白紙に戻るので、その音色の 1 音目はまた firmware が鳴らす
	void request_native_engine(int on)
	{ m_want_native.store(on, std::memory_order_relaxed); }
	int native_engine() const
	{ return m_native_engine.load(std::memory_order_relaxed); }

	// **重さを一覧に出す**。fill() 1 回にかかった時間の、
	// その区間の長さに対する割合（100% を越えると音が途切れる）。
	// 毎回振れるので、大きい側へはすぐ、小さい側へはゆっくり寄せる
	void publish_load(std::chrono::steady_clock::time_point t0, int n, double rate);

	// 記録（%LOCALAPPDATA%\S-MU2000\log.txt）へ 1 行書く
	void log_line(const char *text);
	// ホストからの呼び出しを記録に残す（どのスレッドが、何を、どこまで）。固まる・落ちるの報告で、最後にどこまで
	// 進んだかを見るため。呼ばれるのは起動・終了・画面の開閉などまれなものだけ（音声の処理では呼ばない）
	static void trace(const char *what, const void *self = nullptr, long long a = 0, long long b = 0);

	// 再生位置が飛んだ、止まった等。変換器の中身だけ捨てる
	void flush_resampler();

	// ---- 状態の保存と復元（DAW のプロジェクトに音色を覚えさせる）
	//
	// 機械（m_mu）に触るところは全部 m_machine で守る。音声スレッドは待たない:
	// 取れなければその区間は無音を返し、MIDI は溜めて fill が流す（落さない）。保存・復元・カードの
	// 差し替えは、呼んだスレッドで取れるまで待ってその場でやる。
	//
	// 前は「音を作っている最中は音声スレッドに頼む、止まっていればその場でやる」と
	// していたが、FL Studio の「Reset plugin when FL Studio resets」は保存の途中で
	// setProcessing(false) や setActive(false) を呼び、そのあとも process() を呼び続ける。
	// 「止まっている」と見てその場で書き出す間に音声スレッドが機械を回し、落ちたり、
	// 壊れた状態がプロジェクトに入ったりした（issue #9）
	void set_processing(bool on) { m_processing.store(on, std::memory_order_release); }
	std::vector<uint8_t> save_state();
	// 起動が終わっていなければ、終わってから最初の区間で戻す。
	// setup は XG の値だけの控え（save_xg_setup）。機械まるごとの状態が読めなかったとき
	// （版が違う、壊れている、無い）は、これを MIDI IN A に流して戻す
	bool load_state(const uint8_t *p, size_t n, const uint8_t *setup = nullptr, size_t setup_n = 0);
	// XG の値だけの控え。システム・エフェクト・64 パートを、流し込めば同じ設定になる MIDI にしたもの
	// （ui/xg_state.h の setup_messages）。S-MU2000 の版が変わって機械まるごとの状態が読めなくなっても、
	// 音色とエフェクトの設定はこれで戻る
	std::vector<uint8_t> save_xg_setup();

	// ---- 画面で値を触ったことを、プラグインの口（ホストのオートメーション）へ知らせる
	//
	// 画面（パネルとPC の窓）の層が値を書くたびに edit が呼ばれる（xg::model の edit_listener）。
	// idle は画面の 1 コマごと。closing が true なら画面が閉じるところで、続いている操作を全部終える。
	// どちらも画面の糸から呼ばれる
	using edit_fn = std::function<void(const xg::param &p, int part, int value)>;
	using idle_fn = std::function<void(bool closing)>;
	using raw_fn  = std::function<void(u32 addr, int size, int value)>;   // 定義表に無い番地（set_raw）
	void set_edit_handlers(edit_fn edit, idle_fn idle, raw_fn raw = nullptr);
	void notify_edit(const xg::param &p, int part, int value);
	void notify_edit_raw(u32 addr, int size, int value);
	void notify_idle(bool closing);

	// The editor's width as the user last dragged it (0 = never). The view is
	// made and thrown away with the host's window; this stays with the
	// instance, so reopening the editor gives the size it was closed at.
	// The height follows from the width (view.cpp keeps the panel's ratio)
	int view_width() const { return m_view_w.load(std::memory_order_relaxed); }
	void set_view_width(int w) { m_view_w.store(w, std::memory_order_relaxed); }

	// ---- SmartMedia（前面のカードの差し込み口）
	//
	// カードの中身は PC のファイル（gui.exe と同じ .img）。firmware が書いたブロックは card_flush() で書き戻す。
	// 差し替えと写しは音声スレッドに頼む（上の保存と同じやり方）。path は UTF-8
	bool card_insert(const std::string &path, std::string &err);
	void card_eject();
	void card_flush();
	std::string card_path() const;

	// The machine's parallel thread's audio workgroup (macOS): an
	// os_workgroup_t, kept as void*. Arrives on the render thread, so it
	// is only stashed here; fill() forwards it while holding the lock.
	// Pre-boot wants survive too.
	void set_realtime_workgroup(void *wg);

private:
	// 機械に触る仕事を、m_machine を取ってその場でやる
	bool on_machine(const std::function<void(mu2000 &)> &fn);
	std::mutex m_machine;                   // m_mu と、音を作る途中の入れ物を守る
	mutable std::mutex m_card_mutex;        // m_card_path を守る
	std::string m_card_path;

	void boot();
	void apply_deferred_state();   // 起動前に来た状態を戻す（m_machine を持って呼ぶ）
	void silence_restored();       // 戻した状態で鳴っていた声を止める（m_machine を持って呼ぶ）
	static bool load_state_allowed();   // plugin.ini の load_state（既定 1）
	bool m_load_state_noted = false;
	// 機械まるごとの状態を戻す。読めなければ XG の値の控えを流す（m_machine を持って呼ぶ）
	bool restore(const uint8_t *p, size_t n, const std::vector<uint8_t> &setup);
	void one_sample(float &l, float &r);
	void build_table();

	std::atomic<status> m_state{status::loading};
	std::atomic<bool> m_processing{false};
	std::atomic<int> m_view_w{0};          // see view_width()
	std::vector<uint8_t> m_deferred_state;  // 起動が終わる前に来た状態（m_machine で守る）
	std::vector<uint8_t> m_deferred_setup;  // 同じく、XG の値の控え
	bool                 m_deferred = false;

	std::mutex m_hook_mutex;
	edit_fn    m_on_edit;
	idle_fn    m_on_idle;
	raw_fn     m_on_raw;
	std::thread         m_thread;
	std::atomic<bool>   m_abort{false};
	// 起動は 1 度だけ。start() が exchange で守る（2 度やると
	// 動き中の機械 m_mu を丸ごと差し替えてしまう）
	std::atomic<bool>   m_boot_once{false};
	bool                m_threaded = true;   // plugin.ini の threaded（unpark で戻す）
	bool                m_parked = false;
	std::mutex          m_park_mutex;

	std::unique_ptr<mu2000> m_mu = nullptr;
	// 読み込んだ ROM を掴んでおく。他の枚数ぶんと分け合っている
	std::shared_ptr<void> m_roms;
	// boot() が state を立てる前に書き、読むのは state が loading でなくなってから
	std::string m_message;

	// ---- 標本化周波数の変換。窓関数付き sinc の畳み込み
	//
	// 44100 で作った音を任意の周波数へ。ホストが 44100 なら丸ごと省く。
	static constexpr int TAPS = 64;
	static constexpr int HALF = TAPS / 2;
	static constexpr int STEPS = 256;              // 1 サンプル間隔あたりの表の刻み
	static constexpr int RING = 256, RMASK = RING - 1;

	std::vector<float> m_tab;      // 窓関数付き sinc。[0, HALF] を STEPS 刻みで
	float   m_ring_l[RING] = {};
	float   m_ring_r[RING] = {};
	int64_t m_written = 0;         // これまでに作った 44100 側のサンプル数
	double  m_pos = 0.0;           // 次に出す音の、44100 側での位置
	double  m_step = 1.0;          // 出力 1 サンプルあたり 44100 側で進む量
	double  m_cutoff = 1.0;
	bool    m_direct = true;       // 変換なし
	uint32_t m_latency = 0;

	// ---- A/D INPUT。ホストの周波数で来る音を 44100 に直して溜め、音源が 1 サンプル進むごとに 1 つ使う
	ui::resampler m_in_rs;
	static constexpr int IN_RING = 8192, IN_MASK = IN_RING - 1;
	s16     m_in_q[IN_RING * 2] = {};
	int     m_in_w = 0, m_in_r = 0;
	std::vector<s16> m_in_stage;
	std::vector<float> m_in_conv;
	void push_input(const float *in_l, const float *in_r, int n);

	ui::bridge m_bridge;
	// 口の入切（-1 は「頑みが無い」）と、いまの口
	std::atomic<int> m_want_native{-1};
	// The wanted audio workgroup and the one already forwarded to the
	// machine. The observer must not wait: stash here and forward inside
	// fill() (same shape as m_want_native). m_machine guards m_wg_sent.
	std::atomic<void *> m_wg_want{nullptr};
	void *m_wg_sent = nullptr;
	std::atomic<int> m_native_engine{0};
	ui::cpu_meter m_cpu_meter;     // recent CPU load, measured in audio time
	ui::driver m_drv;

	// MIDI OUT mirror. pump_out() inside fill() drains the machine queue
	// first, so its echo is copied here and midi_out() reads this copy.
	// Both run on the audio thread, so no lock is needed
	static constexpr int TX_RING = 4096, TX_MASK = TX_RING - 1;
	uint8_t m_tx[TX_RING] = {};
	int     m_tx_w = 0, m_tx_r = 0;
	void tx_push(uint8_t v);

	// 起動待ちや、機械を他が使っている間に来た MIDI。落さず溜めて fill が流す。
	// 口ごとに持つ。音声スレッドしか触らない
	std::vector<uint8_t> m_pending[mu2000::MIDI_PORTS];
};

} // namespace vst3

// This engine is not VST3-specific (no VST3 types in it).
// AUv3 (src/auv3/) uses the same one, so alias it to spare that side writing vst3
namespace plug = vst3;

} // namespace smu2000

#endif // S_MU2000_VST3_ENGINE_H
