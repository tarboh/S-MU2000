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
#include "ui/driver.h"
#include "ui/resampler.h"

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
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
	// block をたてると呼んだスレッドのまま起動を済ませる（返るころには
	// state() が ready か failed）。DAW は音作りを待ってくれないので、
	// プラグインは構築の場で起動を終えておき、曲頭から音を鳴らせるようにする。
	// plugin.ini の boot=async か SMU2000_SYNC_BOOT=0 で裏スレッドに戻せる
	void start(bool block = false);
	// 起動が終わるまで待つ。**DAW の本スレッドからだけ**呼ぶこと。
	// 待ちきれずに時間切れなら false。始まっていなければ始めてから待つ
	bool wait_ready(int ms);

	status state() const { return m_state.load(std::memory_order_acquire); }
	// state() が failed のときの理由。ready でも「代用品を使った」等が入る
	std::string message() const;

	// ホスト側の標本化周波数。44100 ちょうどなら変換を通さない
	void set_output_rate(double rate);
	// 変換のぶんだけ音が遅れる。ホストに申告する
	uint32_t latency_samples() const { return m_latency; }

	// MIDI を 1 メッセージ流す。実機と同じく 31250bps の直列に崩される。
	// 起動が終わっていない間は溜めておいて、終わってから流す。
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

	// 記録（%LOCALAPPDATA%\S-MU2000\log.txt）へ 1 行書く
	void log_line(const char *text);

	// 再生位置が飛んだ、止まった等。変換器の中身だけ捨てる
	void flush_resampler();

	// ---- 状態の保存と復元（DAW のプロジェクトに音色を覚えさせる）
	//
	// 機械（m_mu）に触るところは全部 m_machine で守る。音声スレッドは待たない:
	// 取れなければその区間は無音を返し、MIDI は溜めておく。保存・復元・カードの
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

	// ---- SmartMedia（前面のカードの差し込み口）
	//
	// カードの中身は PC のファイル（gui.exe と同じ .img）。firmware が書いたブロックは card_flush() で書き戻す。
	// 差し替えと写しは音声スレッドに頼む（上の保存と同じやり方）。path は UTF-8
	bool card_insert(const std::string &path, std::string &err);
	void card_eject();
	void card_flush();
	std::string card_path() const;

private:
	// 機械に触る仕事を、m_machine を取ってその場でやる
	bool on_machine(const std::function<void(mu2000 &)> &fn);
	std::mutex m_machine;                   // m_mu と、音を作る途中の入れ物を守る
	mutable std::mutex m_card_mutex;        // m_card_path を守る
	std::string m_card_path;

	void boot();
	void apply_deferred_state();   // 起動前に来た状態を戻す（m_machine を持って呼ぶ）
	// 機械まるごとの状態を戻す。読めなければ XG の値の控えを流す（m_machine を持って呼ぶ）
	bool restore(const uint8_t *p, size_t n, const std::vector<uint8_t> &setup);
	void one_sample(float &l, float &r);
	void build_table();

	std::atomic<status> m_state{status::loading};
	std::atomic<bool> m_processing{false};
	std::vector<uint8_t> m_deferred_state;  // 起動が終わる前に来た状態（m_machine で守る）
	std::vector<uint8_t> m_deferred_setup;  // 同じく、XG の値の控え
	bool                 m_deferred = false;

	std::mutex m_hook_mutex;
	edit_fn    m_on_edit;
	idle_fn    m_on_idle;
	raw_fn     m_on_raw;
	std::thread         m_thread;
	std::atomic<bool>   m_abort{false};

	mu2000     *m_mu = nullptr;
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
	ui::driver m_drv;

	// MIDI OUT mirror. pump_out() inside fill() drains the machine queue
	// first, so its echo is copied here and midi_out() reads this copy.
	// Both run on the audio thread, so no lock is needed
	static constexpr int TX_RING = 4096, TX_MASK = TX_RING - 1;
	uint8_t m_tx[TX_RING] = {};
	int     m_tx_w = 0, m_tx_r = 0;
	void tx_push(uint8_t v);

	// 起動前や、機械を他が使っている間に来た MIDI。口ごとに持つ。音声スレッドしか触らない
	std::vector<uint8_t> m_pending[mu2000::MIDI_PORTS];
};

} // namespace vst3

// This engine is not VST3-specific (no VST3 types in it).
// AUv3 (src/auv3/) uses the same one, so alias it to spare that side writing vst3
namespace plug = vst3;

} // namespace smu2000

#endif // S_MU2000_VST3_ENGINE_H
