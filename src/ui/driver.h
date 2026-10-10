// license:BSD-3-Clause
//
// パネルの操作を音源に反映し、画面へ写しを返す。音声スレッドから呼ぶ。
// exe（gui.exe）と VST3 の両方が同じものを使うので、押し方も見え方も揃う。

#ifndef S_MU2000_UI_DRIVER_H
#define S_MU2000_UI_DRIVER_H

#pragma once

#include "bridge.h"
#include "mu2000.h"
#include "xg/ram.h"

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace ui {

class driver
{
	bridge::sampling_view m_samp;     // sampling_tick の写し（入れ替えて渡す）
	// 直前の仕事の結果。写しの入れ物は bridge と入れ替えて 2 つを交互に使うので、結果は外に持って
	// 毎回入れ直す（入れ物に置くと、新しい結果と古い結果が交互に窓へ届いて表示が行き来した）
	std::string m_samp_msg;
	struct samp_peak { int number = 0; u32 start = 0, end = 0; int peak = 0; };
	std::vector<samp_peak> m_samp_peaks;
	int m_wave_made = -1;              // 見取り図を作ったサンプル（-1 はまだ）
	u32 m_wave_frames = 0;
	u32 m_wave_from = 0, m_wave_to = 0;
	std::vector<s16> m_wave_lo, m_wave_hi;
	// 拡大した部分（bridge::request_detail）。made が false なら作り直す
	std::array<bridge::sampling_view::slice, bridge::DETAIL_SLOTS> m_details{};
	bool m_details_fresh = false;
	int m_samp_tick = 0;
	panel_macro m_macro;               // 前面のボタンを決まった順に押す（bridge::request_macro）
	// マクロが回っている間、1 ブロックで早送りに使う時間（マイクロ秒）。ブロックは 10ms ほどなので、
	// その半分弱を使い、音を作る残りの仕事に余裕を残す
	static constexpr int FAST_FORWARD_US = 4000;
public:
	// 1 ブロックの頭で。画面から押されているボタンを音源へ
	void apply_buttons(mu2000 &mu, const bridge &br)
	{
		const u64 want = br.buttons();
		if (want == m_applied)
			return;
		for (int i = 0; i < int(mu2000::button::count); i++)
			if (((want ^ m_applied) >> i) & 1)
				mu.set_button(mu2000::button(i), ((want >> i) & 1) != 0);
		m_applied = want;
	}

	// サンプリングの窓からの仕事を実行し、表の写しを返す（bridge::post）。
	// 写しは 8 ブロックに 1 回（512 サンプルのブロックで 90ms ほど）と、仕事をした直後と、録音中は毎回
	void sampling_tick(mu2000 &mu, bridge &br)
	{
		bridge::sampling_job job;
		bool did = false;
		while (br.take_job(job)) {
			m_samp_msg = job(mu);
			did = true;
		}
		// ボタンのマクロ。頼まれたら始め、回っている間は 1 ブロックずつ進める
		{
			std::vector<panel_macro::step> steps;
			std::string done;
			if (br.take_macro_request(steps, done)) {
				m_macro.cancel(mu);
				m_macro.start(std::move(steps), std::move(done));
				did = true;
			}
			std::string msg;
			bool finished = m_macro.tick(mu, msg);
			// 早送り。回っている間は、このブロックの中で FAST_FORWARD_US だけ音源を先へ回す（出た音は捨てる）。
			// 押す間と firmware の読み込みは音の時間で 8 秒ほどかかるが、全速なら実時間の 18 倍ほどで回るので、
			// 1 秒ほどで済む。firmware の LOAD をそのまま使うので、読み込まれるものは実機で押したのと同じ
			const auto t0 = std::chrono::steady_clock::now();
			while (!finished && m_macro.active() &&
			       std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(FAST_FORWARD_US)) {
				for (int i = 0; i < 441; i++) {
					s32 l, r;
					mu.run_sample(l, r);
				}
				finished = m_macro.tick(mu, msg);
			}
			if (finished) {
				m_samp_msg = msg;
				did = true;   // 読み込みでサンプルの表が変わる
			}
		}
		if (did) {
			// 波形を書き換える仕事もあるので、測ったものは捨てる
			m_samp_peaks.clear();
			m_wave_made = -1;
			m_details_fresh = false;
		}
		const bridge::overview_req want_wave = br.overview_wanted();
		const bool new_wave = want_wave.number != m_wave_made || want_wave.from != m_wave_from ||
		                      want_wave.to != m_wave_to;
		if (new_wave)
			did = true;
		const auto want_details = br.details_wanted();
		bool new_details = !m_details_fresh;
		for (size_t i = 0; i < want_details.size(); i++)
			new_details = new_details || want_details[i].number != m_details[i].number ||
			              want_details[i].from != m_details[i].from || want_details[i].to != m_details[i].to;
		if (new_details)
			did = true;
		if (!did && mu.rec_state() == 0 && !mu.preview_number() && !m_samp.preview_number && ++m_samp_tick < 8)
			return;
		m_samp_tick = 0;
		m_samp.ready = mu.midi_ready();
		m_samp.message = m_samp_msg;
		m_samp.samples = mu.sampling_list();
		// 最大の大きさは、番号・開始・終わりが変わったときだけ測り直す（4MB を毎回は舐めない）
		if (m_samp_peaks.size() < m_samp.samples.size())
			m_samp_peaks.resize(m_samp.samples.size());
		for (size_t i = 0; i < m_samp.samples.size(); i++) {
			smu2000::sampling::sample &s = m_samp.samples[i];
			samp_peak &c = m_samp_peaks[i];
			if (c.number != s.number || c.start != s.start || c.end != s.end) {
				c.number = s.number;
				c.start = s.start;
				c.end = s.end;
				c.peak = mu.sampling_peak(s);
			}
			s.peak = c.peak;
		}
		for (int i = 0; i < smu2000::sampling::MAX_VOICES; i++)
			mu.sampling_voice(i, m_samp.voices[size_t(i)]);
		m_samp.free_frames = mu.sampling_free_frames();
		m_samp.rec_state = mu.rec_state();
		m_samp.rec_frames = mu.rec_frames();
		m_samp.macro_busy = m_macro.active();
		m_samp.card_in = mu.card_inserted();
		if (new_wave) {
			mu.sampling_overview(want_wave.number, bridge::WAVE_BUCKETS, m_wave_lo, m_wave_hi, m_wave_frames,
			                      want_wave.from, want_wave.to);
			m_wave_made = want_wave.number;
			m_wave_from = want_wave.from;
			m_wave_to = want_wave.to;
		}
		// 入れ物は入れ替えて使うので、見取り図も毎回入れ直す
		m_samp.wave_number = m_wave_made;
		m_samp.wave_frames = m_wave_frames;
		m_samp.wave_from = m_wave_from;
		m_samp.preview_number = mu.preview_number();
		m_samp.preview_pos = mu.preview_pos();
		m_samp.wave_to = m_wave_to;
		m_samp.wave_lo = m_wave_lo;
		m_samp.wave_hi = m_wave_hi;
		if (new_details) {
			for (size_t i = 0; i < want_details.size(); i++) {
				bridge::sampling_view::slice &d = m_details[i];
				d.number = want_details[i].number;
				d.from = want_details[i].from;
				d.to = want_details[i].to;
				u32 frames = 0;
				d.lo.clear();
				d.hi.clear();
				if (d.number && d.to > d.from)
					mu.sampling_overview(d.number, bridge::DETAIL_BUCKETS, d.lo, d.hi, frames, d.from, d.to);
			}
			m_details_fresh = true;
		}
		m_samp.details = m_details;
		m_samp.peak[0] = mu.ad_peak(0);
		m_samp.peak[1] = mu.ad_peak(1);
		br.put_sampling(m_samp);
	}

	// エディタから送られた MIDI を音源へ。echo には MIDI 出力の口を渡す
	// （実機の THRU と同じで、画面から出したものも外へ出る）
	template <typename F>
	void pump_midi(mu2000 &mu, bridge &br, F &&echo)
	{
		sampling_tick(mu, br);
		serve_defaults(mu, br);
		br.set_board_port(mu.virtual_board_multi() != 0);
		u8 b;
		while (br.take_midi(b)) {
			watch(b, mu.midi_in(b));
			echo(b);
		}
		// パラメータの問い合わせと本体のモードリセット。外へは流さない
		while (br.take_ask(b))
			watch(b, mu.midi_in(b));
		// 画面から口 B・C・D へ（一覧の鍵盤）。外へは流さない
		for (int port = 1; port < mu2000::MIDI_PORTS; port++)
			while (br.take_midi_port(port, b))
				watch(b, mu.midi_in(b, port));
		// 口 E（マルチパートのプラグインボード）。本体（firmware）には行かない
		while (br.take_midi_port(mu2000::MIDI_PORTS, b))
			mu.board_midi_in(b);
	}

	void pump_midi(mu2000 &mu, bridge &br)
	{
		pump_midi(mu, br, [](u8) {});
	}

	// 画面に頼まれたら、XG の既定値を 1 度だけ作って置く（bridge の request_defaults）。
	// 機械の姿を丸ごと控え、XG System On を流して firmware に既定値を書かせ、写してから
	// 控えを戻す。戻すので、鳴っている音も設定も元のまま。この区間は少し長くかかる
	// （firmware を 0.3 秒ほど回す）ので、音が一瞬途切れることがある
	void serve_defaults(mu2000 &mu, bridge &br)
	{
		if (!br.take_defaults_request())
			return;
		if (br.have_defaults())
			return;
		const std::vector<u8> saved = mu.save_state();
		const int native = mu.native_engine();
		if (native)
			mu.set_native_engine(0);             // XG System On は firmware に読ませる
		for (u8 b : { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 })
			mu.midi_in(b);
		s32 l, r;
		u8 v;
		const int rate = 44100;
		for (int i = 0; i < 3 * rate && mu.midi_pending(); i++) {   // 前に溜まっていた分ごと読ませる
			mu.run_sample(l, r);
			while (mu.midi_out_take(v)) {}
		}
		for (int i = 0; i < rate * 3 / 10; i++) {
			mu.run_sample(l, r);
			while (mu.midi_out_take(v)) {}
		}
		const std::unique_ptr<xg_snapshot> s = std::make_unique<xg_snapshot>();   // 大きいので糸の積み場に置かない
		copy_xg(mu, *s);
		std::string err;
		mu.load_state(saved.data(), saved.size(), err);
		if (native)
			mu.set_native_engine(native);
		br.publish_defaults(*s);
	}

	// ブロックの終わりで。音源が MIDI OUT から送り出したものを画面へ渡す。
	// echo には外の MIDI OUT の口を渡す
	template <typename F>
	void pump_out(mu2000 &mu, bridge &br, F &&echo)
	{
		u8 b;
		while (mu.midi_out_take(b)) {
			br.put_out(b);
			echo(b);
		}
	}

	void pump_out(mu2000 &mu, bridge &br)
	{
		pump_out(mu, br, [](u8) {});
	}

	// ホイールで回された分をダイヤルへ。実機と同じロータリーエンコーダなので、
	// 目盛りを渡すだけでよい（位相は音源が自分で進める）
	void pump_wheel(mu2000 &mu, bridge &br)
	{
		for (int step = br.take_turn(); step; step = br.take_turn())
			mu.turn_encoder(step);
	}

	// 音源へ入れた MIDI を 1 バイトずつ見せる。押さえている鍵とベロシティを写しに書く
	// （音源の中の鍵の状態はきれいに取り出せないので、入口で数える）
	void watch(u8 b, int port)
	{
		if (port < 0 || port >= mu2000::MIDI_PORTS)
			return;
		if (b >= 0xf8)
			return;                           // リアルタイム
		if (b == 0xf0) {
			m_sysex[port] = true;
			m_sx_len[port] = 0;
			m_status[port] = 0;                   // SysEx はランニングステータスを打ち切る
			return;
		}
		if (m_sysex[port]) {
			if (!(b & 0x80)) {
				if (m_sx_len[port] < sizeof(m_sx[port]))
					m_sx[port][m_sx_len[port]] = b;
				m_sx_len[port]++;
				return;
			}
			m_sysex[port] = false;                // F7 か、途中で別のものが来た
			if (b == 0xf7) {
				if (is_reset(m_sx[port], m_sx_len[port]))
					for (int ch = 0; ch < 16; ch++) {
						m_xg.notes[port * 16 + ch][0] = m_xg.notes[port * 16 + ch][1] = 0;
						m_xg.bend[port * 16 + ch] = 0;    // ベンドも真ん中へ
					}
				return;
			}
		}
		if (b & 0x80) {
			m_status[port] = b < 0xf0 ? b : 0;    // F1-F7 は無視して、ランニングステータスも捨てる
			m_have[port] = 0;
			return;
		}
		const u8 st = m_status[port];
		if (!st)
			return;
		const u8 kind = st & 0xf0;
		m_data[port][m_have[port]++] = b;
		const int need = (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
		if (m_have[port] < need)
			return;
		m_have[port] = 0;
		const int slot = port * 16 + (st & 0x0f);
		const u8 d0 = m_data[port][0], d1 = m_data[port][1];
		u64 &bits = m_xg.notes[slot][d0 >> 6];
		const u64 bit = u64(1) << (d0 & 63);
		if (kind == 0x90 && d1) {
			bits |= bit;
			m_xg.velocity[slot] = d1;
			m_xg.note_ons[slot]++;
		} else if (kind == 0x80 || kind == 0x90) {
			bits &= ~bit;
		} else if (kind == 0xb0 && (d0 == 120 || d0 >= 123)) {
			// オールサウンドオフ・オールノートオフ、オムニ／モノ／ポリの切り替え（どれも全部離す）
			m_xg.notes[slot][0] = m_xg.notes[slot][1] = 0;
		} else if (kind == 0xe0) {
			// **ピッチベンド**。真ん中からの離れで覚える。
			// 式だけの口では firmware にベンドを渡さない（音程は自分で作る）ので、
			// ワーク RAM の PART_BEND は動かない。画面はここを見る
			m_xg.bend[slot] = s16((int(d0 & 0x7f) | (int(d1 & 0x7f) << 7)) - 8192);
		} else if (kind == 0xb0 && d0 == 121) {
			// リセットオールコントローラ。ベンドは真ん中へ戻る（MIDI の決まり）
			m_xg.bend[slot] = 0;
		}
	}

	// 音源を初期状態に戻す SysEx か（鳴っている音が全部止まる）。F0 と F7 を除いた中身
	static bool is_reset(const u8 *p, size_t n)
	{
		auto is = [&](std::initializer_list<int> want, int any_low_nibble_at = -1) {
			if (n != want.size())
				return false;
			int i = 0;
			for (int w : want) {
				const u8 v = i == any_low_nibble_at ? u8(p[i] & 0xf0) : p[i];
				if (v != w)
					return false;
				i++;
			}
			return true;
		};
		return is({ 0x7e, 0x7f, 0x09, 0x01 }) || is({ 0x7e, 0x7f, 0x09, 0x03 }) ||          // GM / GM2 On
		       is({ 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00 }, 1) ||                     // XG System On
		       is({ 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7f, 0x00 }, 1) ||                     // XG All Parameter Reset
		       is({ 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41 });              // GS Reset
	}

	// ブロックの終わりで。25ms ごとに LCD と LED を画面へ渡す
	void publish(mu2000 &mu, bridge &br, u32 frames, u32 rate,
	             bool ready, const char *message)
	{
		br.advance_clock(frames, rate);
		mu.set_part_mute(br.part_mute());           // ミュート・ソロは次のブロックから効く
		m_since += frames;
		if (m_since < rate / 40)
			return;
		m_since = 0;
		publish_now(mu, br, ready, message);
		if (ready) {
			publish_xg(mu, br);
			publish_scope(mu, br);
			publish_part_scopes(mu, br);
		}
	}

	// パートの音（音色の窓のスペクトラム）。見たいパートを音源に伝え、直近の波形を置く
	void publish_scope(mu2000 &mu, bridge &br)
	{
		const int want = br.scope_wanted();
		mu.set_scope_part(want);
		if (want < 0)
			return;
		static_assert(bridge::SCOPE_N <= mu2000::SCOPE_N, "scope sizes");
		mu.scope_read(m_scope.data(), bridge::SCOPE_N);
		for (int fx = 0; fx < mu2000::SCOPE_FX_N; fx++)
			for (int out = 0; out < 2; out++)
				mu.scope_read_fx(fx, out != 0, m_scope.data() + size_t(bridge::scope_src(fx, out != 0)) * bridge::SCOPE_N, bridge::SCOPE_N);
		br.publish_scope(m_scope.data(), want);
	}
	std::vector<float> m_scope = std::vector<float>(size_t(bridge::SCOPE_SRCS) * bridge::SCOPE_N);

	// 全パートの音と最終の出力（一覧の小さなスペクトラム）。一覧が見えているあいだだけ
	void publish_part_scopes(mu2000 &mu, bridge &br)
	{
		const bool want = br.part_scopes_wanted();
		mu.set_part_scopes(want);
		if (!want)
			return;
		for (int s = 0; s < bridge::PSCOPE_SRCS; s++)
			mu.part_scope_read(s, m_pscope.data() + size_t(s) * bridge::PSCOPE_N, bridge::PSCOPE_N);
		br.publish_part_scopes(m_pscope.data());
	}
	std::vector<float> m_pscope = std::vector<float>(size_t(bridge::PSCOPE_SRCS) * bridge::PSCOPE_N);

	// firmware のワーク RAM から XG の値を写す（xg/ram.h）
	void publish_xg(mu2000 &mu, bridge &br)
	{
		copy_xg(mu, m_xg);
		m_xg.serial++;
		br.publish_xg(m_xg);
	}

	// XG の値だけを写す（鍵の見張りの欄と serial には触らない）。機械を持っている糸から呼ぶこと
	static void copy_xg(mu2000 &mu, xg_snapshot &out)
	{
		const std::vector<u8> &ram = mu.nvram();
		std::memcpy(out.system, ram.data() + xg::ram::SYSTEM, XG_SYSTEM_SIZE);
		out.voice_mode = ram[xg::ram::VOICE_MODE];
		out.voice_set  = ram[xg::ram::VOICE_SET];
		std::memcpy(out.effect, ram.data() + xg::ram::EFFECT, XG_EFFECT_SIZE);
		for (int p = 0; p < XG_PARTS; p++)
			std::memcpy(out.parts[p], ram.data() + xg::ram::part_base(p), XG_PART_COPY);
		// ドラムセットアップは 4 組ぶん続けて並んでいる
		static_assert(XG_DRUM_SETS == xg::ram::DRUM_SETUP_SETS && XG_DRUM_KEYS == int(xg::ram::DRUM_SETUP_NOTES) &&
		              XG_DRUM_PARAMS == int(xg::ram::DRUM_SETUP_PARAM), "drum setup size");
		std::memcpy(out.drum, ram.data() + xg::ram::DRUM_SETUP, sizeof(out.drum));
		for (int p = 0; p < XG_PARTS; p++)
			out.kit[p] = ram[xg::ram::part_base(p) + 0x110];      // xg::nv::PART_KIT
	}

	static void publish_now(mu2000 &mu, bridge &br, bool ready, const char *message)
	{
		snapshot s;
		hd44780_device &lcd = mu.lcd();
		const u8 *img = mu.lcd_render();
		const int cols = lcd.line_size();
		for (int row = 0; row < LCD_ROWS; row++)
			for (int col = 0; col < LCD_COLS; col++)
				for (int y = 0; y < CELL_H; y++)
					s.dots[(row * LCD_COLS + col) * CELL_H + y] =
						img[16 * (row * cols + col) + y];
		s.leds   = mu.leds();
		s.lcd_on = lcd.display_on();
		s.contrast = u8(mu.lcd_contrast());
		s.voices_master = u8(mu.swpm().sounding_voices());
		s.voices_slave  = u8(mu.swps().sounding_voices());
		s.card   = mu.card_inserted();
		s.ready  = ready;
		if (!ready && message)
			std::snprintf(s.message, sizeof(s.message), "%s", message);
		br.publish(s);
	}

	// 音源が無いとき（起動前、ROM が無い）の写し
	static void publish_message(bridge &br, const char *message)
	{
		snapshot s;
		std::snprintf(s.message, sizeof(s.message), "%s", message ? message : "");
		br.publish(s);
	}

private:
	u64 m_applied = 0;
	u64 m_since = 0;
	xg_snapshot m_xg;                        // 音声の糸だけが触る
	u8   m_status[mu2000::MIDI_PORTS] = {}, m_data[mu2000::MIDI_PORTS][2] = {};
	int  m_have[mu2000::MIDI_PORTS] = {};
	bool m_sysex[mu2000::MIDI_PORTS] = {};
	u8   m_sx[mu2000::MIDI_PORTS][16] = {};                    // SysEx の頭（リセットかを見るだけ）
	size_t m_sx_len[mu2000::MIDI_PORTS] = {};
};

} // namespace ui

#endif // S_MU2000_UI_DRIVER_H
