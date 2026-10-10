// license:BSD-3-Clause
//
// 画面と音源のあいだ。触れ合うのはこの 2 本だけ。
//
//   ボタン   画面 → 音源。押している間 1 のビット
//   写し     音源 → 画面。LCD の点と LED
//
// 音源は音声スレッドが回しているので、画面から直接触ってはいけない。
// 写しは seqlock で渡す（読み手は待たない。途中の絵を読んだら読み直す）。

#ifndef S_MU2000_UI_BRIDGE_H
#define S_MU2000_UI_BRIDGE_H

#pragma once

#include "snapshot.h"
#include "mu2000.h"
#include "panel_macro.h"

#include <atomic>
#include <cstring>
#include <array>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ui {

class bridge
{
public:
	// ---- 画面から

	void press(mu2000::button b, bool down)
	{
		const u64 bit = u64(1) << int(b);
		u64 cur = m_buttons.load(std::memory_order_relaxed), next;
		do {
			next = down ? (cur | bit) : (cur & ~bit);
		} while (!m_buttons.compare_exchange_weak(cur, next, std::memory_order_relaxed));
	}

	void release_all() { m_buttons.store(0, std::memory_order_relaxed); }

	// ダイヤルを回した分。音源側が 1 つずつ VALUE を叩いて消化する
	void turn(int steps) { m_wheel.fetch_add(steps, std::memory_order_relaxed); }

	// 画面から音源へ MIDI を送る（エディタのつまみ、MIDI ファイルの再生）。輪に積むだけ。
	//
	// **1 回に 1 通以上の完成したメッセージを渡すこと。** 書き終えてから 1 回で
	// 書き込み位置を進めるので、音源側からは途中までのメッセージが見えない。
	// 前は 1 バイトずつ進めていたので、音声の糸がブロックの境目で前半だけ読み、
	// 次のブロックで別の口のメッセージがその途中に挟まることがあった。
	//
	// 書き手は 2 本ある（画面の糸と、MIDI ファイルを流す糸）。輪は書き手 1 本が
	// 前提なので、**書き手どうしは錠で順番にする**。どちらも音声の糸ではないので
	// 待ってよい。読み手（音声の糸）は錠に触らない。
	// 入りきらなければ丸ごと捨てて false
	bool send(const u8 *bytes, size_t n) { return m_to_mu.put(bytes, n); }
	bool send(const std::vector<u8> &m)  { return m.empty() || send(m.data(), m.size()); }
	// 口 B・C・D（パート 17-64）へ。一覧の鍵盤から弾くときに使う。THRU には流さない。
	// 口 A は上の send()（あちらは THRU にも流す）
	bool send_port(int port, const u8 *bytes, size_t n)
	{
		if (port <= 0)
			return send(bytes, n);
		if (port > mu2000::MIDI_PORTS)          // MIDI_PORTS 自身は口 E（マルチパートのプラグインボード）
			return false;
		return m_to_mu_p[port].put(bytes, n);
	}
	bool send_b(const u8 *bytes, size_t n) { return send_port(1, bytes, n); }

	// ---- 外の MIDI 出力へ送る（音色の窓の Ctrl＋右クリック）。音源には入れない。
	// 画面の糸が積み、音声の糸が行き先の出力へ流す（出力の輪に積むのは音声の糸だけ、の決まりを守る）。
	// dest は 0 = MIDI THRU A の出力、1 = THRU B、2 = 送り先に選んだ別の出力。
	// 溜まりすぎたら（音声の糸が回っていない、プラグインなど）捨てて false
	bool send_out(int dest, std::vector<u8> msg)
	{
		std::lock_guard<std::mutex> lock(m_out_lock);
		if (m_out_q.size() >= 256 || msg.empty())
			return false;
		m_out_q.emplace_back(dest, std::move(msg));
		return true;
	}
	// 音声の糸から。錠が取れなければ次のブロックで（待たない）
	template <typename F>
	void drain_out(F &&f)
	{
		std::unique_lock<std::mutex> lock(m_out_lock, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		while (!m_out_q.empty()) {
			const std::pair<int, std::vector<u8>> e = std::move(m_out_q.front());
			m_out_q.pop_front();
			f(e.first, e.second);
		}
	}

	// Internal queries and mode resets enter port A without MIDI THRU.
	// Written only by the UI thread.
	bool ask(const std::vector<u8> &m) { return m.empty() || m_ask.put(m.data(), m.size()); }

	// 音源が MIDI OUT から送り出したもの（問い合わせの返事など）。画面の糸が 1 バイトずつ
	bool take_out(u8 &v) { return m_from_mu.take(v); }

	// 音源の時計（ミリ秒）。音声が止まっていれば進まない。
	// パラメータの層は返事を待つ時間をこれで数える（止まっている間は返事も来ない）
	u64 audio_ms() const { return m_audio_ms.load(std::memory_order_relaxed); }

	void set_gain(float g) { m_gain.store(g, std::memory_order_relaxed); }
	float gain() const     { return m_gain.load(std::memory_order_relaxed); }

	// 音声の処理にかかっている CPU（1 回の締め切りに対する割合、%）。音声を自分で回している
	// gui が書き、PC の窓が読んで出す。プラグインのようにホストが回すときは書かない（負のまま）
	void set_cpu(float percent) { m_cpu.store(percent, std::memory_order_relaxed); }
	float cpu() const             { return m_cpu.load(std::memory_order_relaxed); }

	// いまどちらの口で鳴らしているか。0 = firmware（実機どおり）、1 = native
	// （SH-2 を止めてこちらが鳴らす）、-1 = 分からない（プラグインなど）。
	// gui が F4 の切り替えを書き、一覧の帯が読んで出す
	void set_engine(int e) { m_engine.store(e, std::memory_order_relaxed); }
	int engine() const     { return m_engine.load(std::memory_order_relaxed); }

	void read(snapshot &out) const
	{
		for (;;) {
			const unsigned a = m_seq.load(std::memory_order_acquire);
			if (a & 1)
				continue;                       // 書いている最中
			std::memcpy(&out, &m_snap, sizeof(out));
			if (m_seq.load(std::memory_order_acquire) == a)
				return;
		}
	}

	void read_xg(xg_snapshot &out) const
	{
		for (;;) {
			const unsigned a = m_xg_seq.load(std::memory_order_acquire);
			if (a & 1)
				continue;
			std::memcpy(&out, &m_xg, sizeof(out));
			if (m_xg_seq.load(std::memory_order_acquire) == a)
				return;
		}
	}

	// ---- 音源から

	u64 buttons() const { return m_buttons.load(std::memory_order_relaxed); }

	// 音源側から。溜まっている MIDI を 1 バイトずつ
	bool take_midi(u8 &v) { return m_to_mu.take(v); }
	bool take_ask(u8 &v)  { return m_ask.take(v); }
	bool take_midi_port(int port, u8 &v)
	{
		return port > 0 && port <= mu2000::MIDI_PORTS && m_to_mu_p[port].take(v);
	}
	bool take_midi_b(u8 &v) { return take_midi_port(1, v); }

	// 音源の MIDI OUT から出たものを画面へ。書き手は音声の糸だけなので錠は取らない。
	// 画面が読んでいなければ（VST3 の画面を閉じている等）溢れた分は捨てる
	void put_out(u8 v) { m_from_mu.put_one(v); }

	void advance_clock(u32 frames, u32 rate)
	{
		m_clock_frac += u64(frames) * 1000;
		const u64 ms = m_clock_frac / rate;
		m_clock_frac %= rate;
		m_audio_ms.fetch_add(ms, std::memory_order_relaxed);
	}

	int take_turn()
	{
		int v = m_wheel.load(std::memory_order_relaxed);
		if (!v)
			return 0;
		const int step = (v > 0) ? 1 : -1;
		m_wheel.fetch_sub(step, std::memory_order_relaxed);
		return step;
	}

	// XG の値の写し（25ms ごと）。書き手は音声の糸だけ
	void publish_xg(const xg_snapshot &s)
	{
		m_xg_seq.fetch_add(1, std::memory_order_release);
		std::memcpy(&m_xg, &s, sizeof(m_xg));
		m_xg_seq.fetch_add(1, std::memory_order_release);
	}

	// ---- XG の既定値（.syx の書き出しで「既定と違うものだけ」を出すときの比べる相手）
	//
	// 画面が頼み、音声の糸が 1 度だけ作って置く（ui/driver.h の serve_defaults）。
	// 起動した直後の値ではなく、XG System On を受けた直後の値。置いてあれば true
	void request_defaults() { m_want_defaults.store(true, std::memory_order_relaxed); }
	bool take_defaults_request() { return m_want_defaults.exchange(false, std::memory_order_relaxed); }
	void publish_defaults(const xg_snapshot &s)
	{
		std::memcpy(&m_defaults, &s, sizeof(m_defaults));
		m_have_defaults.store(true, std::memory_order_release);
	}
	bool have_defaults() const { return m_have_defaults.load(std::memory_order_acquire); }
	bool read_defaults(xg_snapshot &out) const
	{
		if (!m_have_defaults.load(std::memory_order_acquire))
			return false;
		std::memcpy(&out, &m_defaults, sizeof(out));   // 1 度置いたら書き換えない
		return true;
	}

	// ---- パートの音（音色の窓のスペクトラム）。画面が見たいパートを置き（-1 で止める）、
	// 音声の糸が 25ms ごとに直近の SCOPE_N サンプルを置く（mu2000::scope_read）。読み手は待たない
	// 4096 サンプル（93ms）。2048 だと刻みが 21.5Hz で、低いほうが 1 本ずつ飛んで絵が荒れた
	static constexpr size_t SCOPE_N = 4096;
	void want_scope(int part) { m_scope_want.store(part, std::memory_order_relaxed); }
	int scope_wanted() const { return m_scope_want.load(std::memory_order_relaxed); }
	// 音源の中で消すパート（bit n = パート n）。一覧のミュート・ソロが入れる（mu2000::set_part_mute）
	void set_part_mute(u64 mask) { m_part_mute.store(mask, std::memory_order_relaxed); }
	u64 part_mute() const { return m_part_mute.load(std::memory_order_relaxed); }
	// 置くもの: 0 が声の和（mu2000::scope_read）、1 + fx × 2 + out がエフェクト fx（mu2000::scope_fx）の
	// 入口（out = 0。MEG への送り）と出口（out = 1）。all は SCOPE_SRCS × SCOPE_N 個を続けて
	static constexpr int SCOPE_SRCS = 1 + 2 * mu2000::SCOPE_FX_N;
	static constexpr int scope_src(int fx, bool out) { return 1 + fx * 2 + (out ? 1 : 0); }
	void publish_scope(const float *all, int part)
	{
		m_scope_seq.fetch_add(1, std::memory_order_release);
		std::memcpy(m_scope.data(), all, m_scope.size() * sizeof(float));
		m_scope_part = part;
		m_scope_seq.fetch_add(1, std::memory_order_release);
	}
	// 置いてあれば、そのパートの番号を返す（無ければ -1）。src は上の番号（既定は声の和）
	int read_scope(float *out, int src = 0) const
	{
		if (src < 0 || src >= SCOPE_SRCS)
			return -1;
		for (int tries = 0; tries < 8; tries++) {
			const unsigned a = m_scope_seq.load(std::memory_order_acquire);
			if (a & 1)
				continue;
			std::memcpy(out, m_scope.data() + size_t(src) * SCOPE_N, SCOPE_N * sizeof(float));
			const int part = m_scope_part;
			if (m_scope_seq.load(std::memory_order_acquire) == a)
				return a ? part : -1;
		}
		return -1;
	}

	// ---- 全パートの音と最終の出力（一覧の小さなスペクトラム）。一覧が描くたびに want_part_scopes を
	// 呼び、音声の糸は最後に呼ばれてから 0.5 秒のあいだだけ溜めて置く（mu2000::part_scope_read）
	static constexpr size_t PSCOPE_N = mu2000::PSCOPE_N;
	static constexpr int PSCOPE_SRCS = 65;                 // 64 パートと最終の出力（64）
	void want_part_scopes() { m_pscope_want_ms.store(audio_ms() + 1, std::memory_order_relaxed); }
	bool part_scopes_wanted() const
	{
		const u64 t = m_pscope_want_ms.load(std::memory_order_relaxed);
		return t && audio_ms() + 1 < t + 500;
	}
	void publish_part_scopes(const float *all)
	{
		m_pscope_seq.fetch_add(1, std::memory_order_release);
		std::memcpy(m_pscope.data(), all, m_pscope.size() * sizeof(float));
		m_pscope_seq.fetch_add(1, std::memory_order_release);
	}
	// 置いた回数（変わっていなければ読み直さなくてよい）。0 はまだ無い
	unsigned part_scopes_serial() const { return m_pscope_seq.load(std::memory_order_acquire) / 2; }
	bool read_part_scope(int src, float *out) const
	{
		if (src < 0 || src >= PSCOPE_SRCS)
			return false;
		for (int tries = 0; tries < 8; tries++) {
			const unsigned a = m_pscope_seq.load(std::memory_order_acquire);
			if (a & 1)
				continue;
			std::memcpy(out, m_pscope.data() + size_t(src) * PSCOPE_N, PSCOPE_N * sizeof(float));
			if (m_pscope_seq.load(std::memory_order_acquire) == a)
				return a != 0;
		}
		return false;
	}

	void publish(const snapshot &s)
	{
		m_seq.fetch_add(1, std::memory_order_release);
		std::memcpy(&m_snap, &s, sizeof(m_snap));
		m_seq.fetch_add(1, std::memory_order_release);
	}

	// ---- サンプリングの窓（src/ui/sampling_editor.cpp）
	// 窓は音源に触らない。音源の表を読み書きする仕事は post で音を作る糸へ渡し、その糸が
	// driver::sampling_tick で実行して、結果（一言）と表の写しを返す
	using sampling_job = std::function<std::string(mu2000 &)>;
	struct sampling_view
	{
		std::vector<smu2000::sampling::sample> samples;
		std::array<smu2000::sampling::voice, smu2000::sampling::MAX_VOICES> voices{};
		u32 free_frames = 0;
		int rec_state = 0;            // mu2000::rec_state
		u32 rec_frames = 0;
		s32 peak[2] = {};             // A/D INPUT のピーク（16bit の絶対値）
		std::string message;          // 直前の仕事の結果
		u64 serial = 0;               // 写しを作るたびに 1 増える
		bool ready = false;           // 音源が起動を終えた
		// 窓が選んだサンプルの見取り図（request_overview）。波形を WAVE_BUCKETS 個に分けた最小と最大
		int wave_number = 0;
		u32 wave_frames = 0;
		u32 wave_from = 0, wave_to = 0;   // 見取り図にした範囲（サンプルの位置）
		int preview_number = 0;           // 試聴しているサンプル（0 = していない、-1 = 外の PCM）
		bool macro_busy = false;          // ボタンのマクロ（request_macro）が回っている
		bool card_in = false;             // 差し込み口に SmartMedia がある
		u32 preview_pos = 0;              // 試聴している位置（サンプルの位置）
		std::vector<s16> wave_lo, wave_hi;
		// 拡大した部分の波形（request_detail）。区切りは DETAIL_BUCKETS 個まで（範囲が狭ければ 1 サンプルに 1 つ）
		struct slice
		{
			int number = 0;
			u32 from = 0, to = 0;
			std::vector<s16> lo, hi;
		};
		std::array<slice, 6> details;
	};
	static constexpr int WAVE_BUCKETS = 1024;
	static constexpr int DETAIL_BUCKETS = 1024;
	static constexpr int DETAIL_SLOTS = 6;
	// 拡大した部分の波形を作ってほしい範囲（slot ごと。number が 0 なら要らない）
	void request_detail(int slot, int number, u32 from, u32 to)
	{
		std::lock_guard<std::mutex> lock(m_wave_lock);
		m_detail_want[size_t(slot)] = { number, from, to };
	}
	// 見取り図を作ってほしいサンプル（0 = 要らない）
	// from, to はサンプルの位置（0, 0 なら全体）。拡大したときはその範囲だけ
	void request_overview(int number, u32 from = 0, u32 to = 0)
	{
		std::lock_guard<std::mutex> lock(m_wave_lock);
		m_wave_want = { number, from, to };
	}
	struct overview_req { int number = 0; u32 from = 0, to = 0; };
	overview_req overview_wanted() const
	{
		std::lock_guard<std::mutex> lock(m_wave_lock);
		return m_wave_want;
	}
	std::array<overview_req, DETAIL_SLOTS> details_wanted() const
	{
		std::lock_guard<std::mutex> lock(m_wave_lock);
		return m_detail_want;
	}
	void post(sampling_job job)
	{
		std::lock_guard<std::mutex> lock(m_job_lock);
		m_jobs.push_back(std::move(job));
	}
	// 音を作る糸から。待たずに取れた分だけ
	bool take_job(sampling_job &out)
	{
		std::unique_lock<std::mutex> lock(m_job_lock, std::try_to_lock);
		if (!lock.owns_lock() || m_jobs.empty())
			return false;
		out = std::move(m_jobs.front());
		m_jobs.pop_front();
		return true;
	}
	void put_sampling(sampling_view &v)
	{
		std::unique_lock<std::mutex> lock(m_view_lock, std::try_to_lock);
		if (!lock.owns_lock())
			return;                     // 窓が読んでいる最中なら次の回に
		v.serial = m_view.serial + 1;
		std::swap(m_view, v);
	}
	void get_sampling(sampling_view &out) const
	{
		std::lock_guard<std::mutex> lock(m_view_lock);
		out = m_view;
	}
	// 録音デバイスの選択（gui の A/D INPUT。プラグインではホストのバスなので使わない）。
	// 窓が選んだ番号（-1 = 無し）を gui が拾って開き直す。
	//
	// The list is a snapshot, the current selection is not. Counting devices is
	// slow enough to want its own thread (app::list_ain_async), but there are two
	// ways to choose one - this window's combo and the panel's own menu - so a
	// stored value goes stale the moment the other one is used, and the window
	// then shows "(none)" for a device that is open and feeding the machine. Only
	// the names are stored; the current name is asked for.
	void set_ain_devices(std::vector<std::string> names)
	{
		std::lock_guard<std::mutex> lock(m_ain_lock);
		m_ain_names = std::move(names);
		m_ain_known = true;
	}
	// From the gui: the name of the device that is open now, empty for none.
	void set_ain_current_fn(std::function<std::string()> fn)
	{
		std::lock_guard<std::mutex> lock(m_ain_lock);
		m_ain_current_fn = std::move(fn);
	}
	bool ain_devices(std::vector<std::string> &names, std::string &current) const
	{
		std::lock_guard<std::mutex> lock(m_ain_lock);
		names = m_ain_names;
		current = m_ain_current_fn ? m_ain_current_fn() : std::string();
		return m_ain_known;
	}
	void request_ain(int dev) { m_ain_want.store(dev, std::memory_order_relaxed); }
	// gui から。選ばれていれば番号（-1 = 無し）、無ければ -2
	int take_ain_request() { return m_ain_want.exchange(-2, std::memory_order_relaxed); }
	void request_ain_list() { m_ain_list_want.store(true, std::memory_order_relaxed); }
	// 本体の電源を入れ直してほしい（架空のボードを挿した・外したとき。アプリの側が受けて engine::restart を回す）
	void request_restart() { m_restart_want.store(true, std::memory_order_relaxed); }
	bool take_restart_request() { return m_restart_want.exchange(false, std::memory_order_relaxed); }
	// 口 E を受け持つボード（マルチパートのプラグインボード）が挿さっているか。音源の側が置き、MIDI ファイルの
	// プレイヤーが読む（挿さっていれば、5 口目のトラックを口 E へ送る）
	void set_board_port(bool on) { m_board_port.store(on, std::memory_order_relaxed); }
	bool board_port() const { return m_board_port.load(std::memory_order_relaxed); }
	bool take_ain_list_request() { return m_ain_list_want.exchange(false, std::memory_order_relaxed); }

	// SmartMedia の差し込み口（サンプリングの窓の「カード」）。差しているカードの場所は gui・プラグインが
	// 置き（空 = 差していない）、窓が「このカードを差す」と頼んだ場所を gui・プラグインが拾って差す
	void set_card_path(const std::string &path)
	{
		std::lock_guard<std::mutex> lock(m_card_lock);
		if (m_card_path != path)
			m_card_path = path;
	}
	std::string card_path() const
	{
		std::lock_guard<std::mutex> lock(m_card_lock);
		return m_card_path;
	}
	void request_card(const std::string &path)
	{
		std::lock_guard<std::mutex> lock(m_card_lock);
		m_card_want = path;
		m_card_wanted = true;
	}
	bool take_card_request(std::string &path)
	{
		std::lock_guard<std::mutex> lock(m_card_lock);
		if (!m_card_wanted)
			return false;
		m_card_wanted = false;
		path = m_card_want;
		return true;
	}

	// 前面のボタンを決まった順に押す（panel_macro.h）。音を作る糸の driver が拾って回し、終わったら
	// done（失敗ならその理由）を sampling_view::message に出す。回っている間は sampling_view::macro_busy
	void request_macro(std::vector<panel_macro::step> steps, std::string done)
	{
		std::lock_guard<std::mutex> lock(m_card_lock);
		m_macro_want = std::move(steps);
		m_macro_done = std::move(done);
		m_macro_wanted = true;
	}
	bool take_macro_request(std::vector<panel_macro::step> &steps, std::string &done)
	{
		std::lock_guard<std::mutex> lock(m_card_lock);
		if (!m_macro_wanted)
			return false;
		m_macro_wanted = false;
		steps = std::move(m_macro_want);
		done = std::move(m_macro_done);
		return true;
	}

private:
	std::vector<panel_macro::step> m_macro_want;
	std::string m_macro_done;
	bool m_macro_wanted = false;
	mutable std::mutex m_card_lock;
	std::string m_card_path, m_card_want;
	bool m_card_wanted = false;
	mutable std::mutex m_job_lock;
	std::deque<sampling_job> m_jobs;
	mutable std::mutex m_view_lock;
	sampling_view m_view;
	mutable std::mutex m_ain_lock;
	std::vector<std::string> m_ain_names;
	std::function<std::string()> m_ain_current_fn;
	bool m_ain_known = false;
	std::atomic<int> m_ain_want{-2};
	std::atomic<bool> m_ain_list_want{false};
	std::atomic<bool> m_restart_want{false};
	std::atomic<bool> m_board_port{false};
	mutable std::mutex m_wave_lock;
	overview_req m_wave_want;
	std::array<overview_req, DETAIL_SLOTS> m_detail_want{};

	// 読み手 1 本の輪。put はメッセージを書き終えてから 1 回で位置を進める
	class ring
	{
	public:
		bool put(const u8 *bytes, size_t n)
		{
			std::lock_guard<std::mutex> lock(m_lock);
			const size_t w = m_w.load(std::memory_order_relaxed);
			const size_t r = m_r.load(std::memory_order_acquire);
			const size_t room = (r - w - 1) & MASK;
			if (n > room)
				return false;
			for (size_t i = 0; i < n; i++)
				m_buf[(w + i) & MASK] = bytes[i];
			m_w.store((w + n) & MASK, std::memory_order_release);
			return true;
		}
		// 書き手が 1 本だけのときの、錠を取らない版（音声の糸から）
		void put_one(u8 v)
		{
			const size_t w = m_w.load(std::memory_order_relaxed);
			if (((m_r.load(std::memory_order_acquire) - w - 1) & MASK) == 0)
				return;
			m_buf[w] = v;
			m_w.store((w + 1) & MASK, std::memory_order_release);
		}
		bool take(u8 &v)
		{
			const size_t r = m_r.load(std::memory_order_relaxed);
			if (r == m_w.load(std::memory_order_acquire))
				return false;
			v = m_buf[r];
			m_r.store((r + 1) & MASK, std::memory_order_release);
			return true;
		}
	private:
		static constexpr size_t SIZE = 4096, MASK = SIZE - 1;
		u8 m_buf[SIZE] = {};
		std::atomic<size_t> m_r{0}, m_w{0};
		std::mutex m_lock;                   // 書き手どうしだけが使う
	};

	ring                  m_to_mu;        // 画面・MIDI ファイル → 音源（THRU にも流す）
	ring                  m_ask;          // パラメータの層の問い合わせ → 音源（THRU には流さない）
	// 画面 → 音源の口 B・C・D（THRU には流さない）。[0] は使わない（口 A は m_to_mu）
	ring                  m_to_mu_p[mu2000::MIDI_PORTS + 1];
	std::mutex            m_out_lock;                          // send_out の待ち行列
	std::deque<std::pair<int, std::vector<u8>>> m_out_q;
	ring                  m_from_mu;      // 音源の MIDI OUT → 画面
	std::atomic<u64>      m_audio_ms{0};
	u64                   m_clock_frac = 0;   // 音声の糸だけが触る
	std::atomic<u64>      m_buttons{0};
	std::atomic<int>      m_wheel{0};
	std::atomic<float>    m_gain{1.0f};
	std::atomic<float>    m_cpu{-1.0f};
	std::atomic<int>      m_engine{-1};
	std::atomic<unsigned> m_seq{0};
	snapshot              m_snap;
	std::atomic<unsigned> m_xg_seq{0};
	std::atomic<int>      m_scope_want{-1};
	std::atomic<u64>      m_part_mute{0};
	std::atomic<unsigned> m_scope_seq{0};
	std::vector<float>    m_scope = std::vector<float>(size_t(SCOPE_SRCS) * SCOPE_N);
	int                   m_scope_part = -1;
	std::atomic<u64>      m_pscope_want_ms{0};
	std::atomic<unsigned> m_pscope_seq{0};
	std::vector<float>    m_pscope = std::vector<float>(size_t(PSCOPE_SRCS) * PSCOPE_N);
	xg_snapshot           m_xg;
	std::atomic<bool>     m_want_defaults{false};
	std::atomic<bool>     m_have_defaults{false};
	xg_snapshot           m_defaults;
};

} // namespace ui

#endif // S_MU2000_UI_BRIDGE_H
