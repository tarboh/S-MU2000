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
		case 0xf2:            return 3;
		default:              return 1;
		}
	default: return 3;
	}
}

// Find the ROM directory. Returns where they were found, or "" with every place
// that was tried, in order, appended to `tried`. This is the same search boot()
// performs, published so that the AUv3's carrier application
// (src/auv3/main_app.mm) can report it exactly as the plug-in performs it,
// instead of keeping a second copy that can drift
std::string find_rom_dir(std::string &tried);

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

	// ROM を探して読み、起動するまでを別スレッドで進める。すぐ返る
	void start();
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

	// MIDI OUT: the machine's own OUT jack (SCI ch0 of the SH7043, or the USB
	// side when that is the host port). What the firmware sent -- replies to XG
	// enquiries, answers to dump requests. **Call it from the same thread as
	// fill(), after fill().** Returns how many bytes were put in dst, or 0
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
	// 起動が終わっていなければ、終わってから最初の区間で戻す
	bool load_state(const uint8_t *p, size_t n);

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
	void one_sample(float &l, float &r);
	void build_table();

	std::atomic<status> m_state{status::loading};
	std::atomic<bool> m_processing{false};
	std::vector<uint8_t> m_deferred_state;  // 起動が終わる前に来た状態（m_machine で守る）
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

	// 起動前や、機械を他が使っている間に来た MIDI。口ごとに持つ。音声スレッドしか触らない
	std::vector<uint8_t> m_pending[mu2000::MIDI_PORTS];

	// ---- A copy of MIDI OUT.
	//
	// **The machine's own queue cannot be drained directly.** fill() runs
	// pump_out() first and hands those bytes to the panel's display, so a
	// midi_out_take() afterwards always finds it empty. What pump_out() passes
	// by is teed in here instead, and midi_out() reads this. Both ends are the
	// audio thread, so no lock is needed
	static constexpr int TX_RING = 4096, TX_MASK = TX_RING - 1;
	uint8_t m_tx[TX_RING] = {};
	int     m_tx_w = 0, m_tx_r = 0;
	void tx_push(uint8_t v);
};

} // namespace vst3

// This engine is not VST3-only -- not one VST3 type appears in it. The AUv2
// (src/au/) and the AUv3 (src/auv3/) run the same thing, so they get a name for
// it that does not make them say "vst3". The directory keeps its name for the
// sake of history
namespace plug = vst3;

} // namespace smu2000

#endif // S_MU2000_VST3_ENGINE_H
