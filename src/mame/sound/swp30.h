// license:BSD-3-Clause
// copyright-holders:Olivier Galibert

// Yamaha SWP30/30B, rompler/dsp combo

#ifndef MAME_SOUND_SWP30_H
#define MAME_SOUND_SWP30_H

#pragma once

// S-MU2000: MAME 本体の代わりに互換層を使う
#include "state.h"

#include "dsp/fx_native.h"
#include "../../compat/mamecompat.h"

#include <algorithm>
#include <atomic>
#include <cstdio>


// S-MU2000: swp30_disassembler はデバッガ用なので削除した

class swp30_device
{
public:
	// 状態の保存と復元（src/state.h）
	void state(state_io &s);

	swp30_device();

	// S-MU2000: 包絡線（音量・音程）の 1 サンプルの歩幅。sample_counter はチップの
	// サンプルの数え値。画面が包絡線の時間を描くのに使う（src/ui の絵）
	static u16 envelope_step(s32 speed, u32 sample_counter);
	// S-MU2000: 声のフィルタ（レジスタ 0x00-0x04 をそのまま）に 1 サンプルのインパルスを通した応答。
	// 画面がフィルタの特性を描くのに使う。out[i] は入力 1 に対する出力
	static void filter_impulse(u16 f1a, u16 level1, u16 f2a, u16 level2, u16 fb, float *out, int n);
	// S-MU2000: 声の LFO（レジスタ 0x0a をそのまま）を 0 から n サンプル回したときの音程のずれ
	// （音程のレジスタの目盛り。1 オクターブ 1024）。画面がビブラートを描くのに使う
	static void lfo_pitch_trace(u16 type_step_pitch, s16 *out, int n);

	// S-MU2000: エフェクトを C++ で鳴らす軽量モード（doc/native-dsp.md）。nullptr で切。
	// full なら MEG そのものを回さず、乾いた音も C++ 側で混ぜる（そのぶん軽い）
	void set_native_fx(smu2000::dsp::native_fx *fx, bool full = false, int mask = 15)
	{ m_native = fx; m_native_full = full; m_native_mask = mask; m_mfx_seen = ~0u; }

	// S-MU2000: address_map の代わり。レジスタは 64ch x 64 スロットの格子
	u16  read16(offs_t addr);
	void write16(offs_t addr, u16 data);

	// 外から与えるメモリ
	void set_wave_rom(const void *base, size_t bytes);
	// S-MU2000: サンプリング RAM（SWP30 から見て 0x1000000 語目から。2 チップで同じ物を共有する）
	void set_sample_ram(u8 *base, size_t bytes) { m_wave_cache.set_overlay(base, 0x1000000, bytes >> 2); }
	void set_sintab(const u16 *base, size_t count);
	// S-MU2000: 波形の記録（レジスタ 0x12/13・0x14/15・0x16/17 に書く値）のサンプルを、声と同じ読み方で
	// 鳴る順に取り出す（内蔵ウェーブを見る窓用）。ループの終わりまで。max を超える分は切る
	std::vector<s16> decode_wave(u32 start, u32 loop, u32 address, size_t max);

	void reset();

	// S-MU2000: MEG の中身（プログラム・定数・番地表・LFO）を書き出す。
	// 出た文字は tools/meg_dis.py が読んで人の読める形にする
	void dump_meg(const char *path);

	// S-MU2000: swp30 が乱数を使うのは 2 か所だけ
	// （LFO のランダム波形と MEG のノイズ）。
	//
	// MAME は machine の 1 本の数列を全デバイスで分け合うが、こちらは
	// **チップごとに数列を持つ**。MU2000 の SWP30 は 2 個あり、片方を別の
	// スレッドで回しているので、1 本を分け合うと
	//   ・どちらがどの値を引くかが実行のたびに変わり、出力が再現しない
	//   ・同じ変数を 2 スレッドで読み書きする競合そのものになる
	// 使い道が雑音と LFO の初期位相なので、数列が MAME と違っても
	// 鳴る音は変わらない。種はチップごとに分けてある。
	// 数列の作り方は MAME の running_machine::rand() と同じ
	u32 rand()
	{
		m_rand_seed = 1664525 * m_rand_seed + 1013904223;
		// 下位ビットは周期が短くよく使われるので 16bit 回転して返す
		return (m_rand_seed >> 16) | (m_rand_seed << 16);
	}
	// rand() を n 回呼んだのと同じだけ種を進める掛け数と足し数（seed = mul * seed + add）
	static constexpr void rand_jump(u32 n, u32 &mul, u32 &add)
	{
		mul = 1; add = 0;
		for(u32 i = 0; i != n; i++) {
			add = 1664525 * add + 1013904223;
			mul = 1664525 * mul;
		}
	}
	void rand_skip(u32 n)
	{
		u32 mul, add;
		rand_jump(n, mul, add);
		m_rand_seed = mul * m_rand_seed + add;
	}
	void set_rand_seed(u32 seed) { m_rand_seed_base = seed; m_rand_seed = seed; }

	running_machine &machine() { return m_machine; }

	// 1 サンプル進めて、DAC 出力 2ch を返す
	void run_sample(s32 &left, s32 &right);

	// S-MU2000: 鳴っている声の数（64 スロットのうち、エンベロープが止まっていないもの）。
	// 画面の同時発音数の表示用。離して消え切るまでの声も数える
	int sounding_voices() const;

	// S-MU2000: MU2000 は SWP30 を 2 個積み、MELO/MELI のシリアルで結んでいる。
	// スレーブの声はここを通ってマスタのミキサに入る。MAME では
	// add_route(出力 i+4, 相手, 1.0, 入力 i) がこの線に当たる。
	// 目盛りは put_int_clamp(..., 1<<26) と同じで、範囲外は切り詰める
	static constexpr s32 SERIAL_FULL_SCALE = 1 << 26;
	s32  melo(int i) const { return std::clamp(m_melo[i], -SERIAL_FULL_SCALE, SERIAL_FULL_SCALE); }
	void set_meli(int i, s32 v) { m_meli[i] = v; }

	// S-MU2000: 音が出ないときの手掛かり
	s32 m_dbg_adc_max = 0, m_dbg_meg_max = 0, m_dbg_awm_max = 0;
	u64 m_t_sample = 0, m_t_meg = 0;   // 区間ごとの所要時間
	bool m_profile = false;            // true のときだけ m_t_meg を測る

	// 発音ごとに、その声が実際にどれだけ音を出したか。
	// 「発音指示は出ているのに鳴っていない」を数えるため
	u64 m_dbg_energy[0x40] = {};
	u64 m_dbg_len[0x40] = {};
	std::vector<std::pair<u64,u64>> m_dbg_notes;   // (積算, 長さ)

	// MEG を 1 命令ずつ追う。pc の範囲とサンプルの範囲を絞って出す
	std::FILE *m_dbg_meg = nullptr;
	u32 m_dbg_meg_from = 0, m_dbg_meg_count = 0;
	u16 m_dbg_meg_pc0 = 0, m_dbg_meg_pc1 = 0x180;

	// S-MU2000: 声ごとの出力（包絡線を掛けたあと、混ぜる前）を 1 サンプルごとに渡す口。
	// 画面がパートの音のスペクトラムを描くのに使う（mu2000 の scope_*）。音には触らない
	using voice_tap_fn = void (*)(void *ctx, const s32 *samples_per_chan);
	voice_tap_fn m_voice_tap = nullptr;
	void *m_voice_tap_ctx = nullptr;
	// S-MU2000: 鳴らさない声（bit n = 声 n）。その声の出力を、ミックスと送りに入る前に 0 にする。
	// firmware が MIDI を受けないとき（デモ曲の再生中）でもパートを消せるように（mu2000::set_part_mute）
	std::atomic<u64> m_voice_mute{0};
	// S-MU2000: 外から来た音を、MEG の入口（m20-m2f。ミキサの出力 8-15 の左右）に足す。
	// プラグインボードの音がエフェクトへ入る道の代わり（mu2000::set_external_audio）。0 なら何もしない。
	// 考え方は OnjLouis さんの mu2026-smu-engine（S-MU2000 のフォーク、BSD-3）の external fx buses から
	std::array<s32, 16> m_ext_bus = {};
	bool m_ext_on = false;
	// S-MU2000: MEG の m20-m2f を 1 サンプルごとに渡す口。in は MEG を回す前（ミキサからの送り = エフェクトの入口）、
	// out は回した直後（エフェクトの出口。次のサンプルでミキサの入力 0x40-0x4f になる）。firmware の組む MEG は
	// どのエフェクトも出口を入口と同じ番地に書き戻す。音色の窓のエフェクトのスペクトラムに使う。音には触らない
	using meg_tap_fn = void (*)(void *ctx, const s32 *in, const s32 *out);
	meg_tap_fn m_meg_tap = nullptr;
	void *m_meg_tap_ctx = nullptr;

	// MEG の入口・出口の書き出し（移植の突き合わせ用）
	std::FILE *m_dbg_dac = nullptr;
	int m_dbg_chan = -1;              // --dump-dac のとき、この声の中身も出す
	u32 m_dbg_dac_from = 0, m_dbg_dac_count = 0;
	s32 m_dbg_megin_max = 0, m_dbg_melo_max = 0;

private:
	// S-MU2000: device_start/reset, state_*, disassembler, rom_region の宣言は削除

	struct streaming_block {
		static const std::array<u16, 0x400> pitch_base;
		static const std::array<s16,   256> dpcm_expand;
		static const std::array<std::array<s16, 0x800>, 2> interpolation_table;
		static const std::array<s32, 8> max_value;

		s32 m_start = 0;
		s32 m_loop = 0;
		u32 m_address = 0;
		u16 m_pitch = 0;

		s32 m_loop_size = 0;
		s32 m_pos = 0;
		s32 m_pos_dec = 0;
		s16 m_dpcm_s0 = 0, m_dpcm_s1 = 0, m_dpcm_s2 = 0, m_dpcm_s3 = 0;
		u32 m_dpcm_pos = 0;
		s32 m_dpcm_delta = 0;             // S-MU2000: 上 24bit が差分、下 8bit が持ち越す余り（-r）

		bool m_first = false, m_finetune_active = false, m_done = false;
		s16 m_last = 0;

		void clear();
		void keyon();
		std::pair<s16, bool> step(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s32 pitch_lfo, u16 pitch_offset);

		void start_h_w(u16 data);
		void start_l_w(u16 data);
		void loop_h_w(u16 data);
		void loop_l_w(u16 data);
		void address_h_w(u16 data);
		void address_l_w(u16 data);
		void pitch_w(u16 data);

		u16 start_h_r() const;
		u16 start_l_r() const;
		u16 loop_h_r() const;
		u16 loop_l_r() const;
		u16 address_h_r() const;
		u16 address_l_r() const;
		u16 pitch_r() const;

		void read_16(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3);
		void read_12(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3);
		void read_8(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3);
		void read_8c(memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache &wave, s16 &val0, s16 &val1, s16 &val2, s16 &val3);

		void dpcm_step(u8 input, u32 mode, u32 scale, s32 limit);
		void update_loop_size();
		void scale_and_clamp_one(s16 &val, u32 scale, s32 limit);
		void scale_and_clamp(s16 &val0, s16 &val1, s16 &val2, s16 &val3);

		std::string describe() const;
	};

	struct filter_block {
		u16 m_filter_1_a = 0;
		u16 m_level_1 = 0;
		u16 m_filter_2_a = 0;
		u16 m_level_2 = 0;
		u16 m_filter_b = 0;

		s32 m_filter_1_p1 = 0;
		s32 m_filter_2_p1 = 0;
		s32 m_filter_p2 = 0;

		s32 m_filter_1_x1 = 0;
		s32 m_filter_1_x2 = 0;
		s32 m_filter_1_y0 = 0;
		s32 m_filter_1_y1 = 0;

		s32 m_filter_1_h = 0;
		s32 m_filter_1_b = 0;
		s32 m_filter_1_l = 0;
		s32 m_filter_1_n = 0;

		s32 m_filter_2_x1 = 0;
		s32 m_filter_2_x2 = 0;
		s32 m_filter_2_y0 = 0;
		s32 m_filter_2_y1 = 0;

		s32 m_filter_2_h = 0;
		s32 m_filter_2_b = 0;
		s32 m_filter_2_l = 0;
		s32 m_filter_2_n = 0;

		void clear();
		// cur・cur2 は滑っている途中のフィルタ 1・2 の切る高さ（<< 16）。状態の保存の形を変えないよう、
		// 持ち主は装置の m_filter_cur・m_filter2_cur
		void keyon(u32 &cur, s32 &cur2);
		s32 step(s16 input, u32 &cur, s32 &cur2);
		static s32 filter_2_target(u16 a) { const s32 c = a & 0x7ff; return (c ? c : -0x800) << 16; }
		static s32 filter_p1(u32 a) { return (0x101 + (a & 0xff)) << ((a >> 8) & 7); }

		void f1_chamberlin_step(s16 input);

		static s32 volume_apply(u8 level, s32 sample);

		u16 filter_1_a_r() const;
		u16 level_1_r() const;
		u16 filter_2_a_r() const;
		u16 level_2_r() const;
		u16 filter_b_r() const;

		void filter_1_a_w(u16 data);
		void level_1_w(u16 data);
		void filter_2_a_w(u16 data);
		void level_2_w(u16 data);
		void filter_b_w(u16 data);
	};

	struct iir1_block {
		// S-MU2000: 初期値を書く。reset では消えず、firmware が書く前に
		// 読まれる経路があるので、埋めないと出る音が実行ごとに変わる
		s16 m_a[2][2] = {};
		s16 m_b[2] = {};
		s32 m_hx[2] = {}, m_hy[2] = {};

		void clear();
		void keyon();
		s32 step(s32 input);

		template<u32 Filter> u16 a0_r() const;
		template<u32 Filter> u16 a1_r() const;
		template<u32 Filter> u16 b1_r() const;
		template<u32 Filter> void a0_w(u16 data);
		template<u32 Filter> void a1_w(u16 data);
		template<u32 Filter> void b1_w(u16 data);
	};

	struct envelope_block {
		// Hardware values readable through internal read on variable 0, do not change
		enum {
			ATTACK  = 0,
			DECAY1  = 1,
			DECAY2  = 2,
			RELEASE = 3
		};

		u16 m_attack = 0;
		u16 m_decay1 = 0;
		u16 m_decay2 = 0;
		u16 m_release_glo = 0;
		s32 m_envelope_level = 0;
		u8  m_envelope_mode = 0;

		void clear();
		void keyon();
		u16 status() const;
		bool active() const;
		u16 step(u32 sample_counter);
		void trigger_release();

		void attack_w(u16 data);
		void decay1_w(u16 data);
		void decay2_w(u16 data);
		void release_glo_w(u16 data);

		u16 attack_r() const;
		u16 decay1_r() const;
		u16 decay2_r() const;
		u16 release_glo_r() const;

		// S-MU2000: speed は符号付き。ピッチ EG は 16 段遅らせて引くので、
		// もとの表より下（負）まで伸びる。8 段下がるごとに半分の速さ
		// 声の状態を使わないので static（画面が包絡線の時間を描くのにも使う）
		static u16 level_step(s32 speed, u32 sample_counter);
	};

	struct lfo_block {
		u32 m_counter = 0;
		u16 m_state = 0;

		u16 m_r_type_step_pitch = 0;
		u16 m_r_amplitude = 0;

		u8 m_type = 0;
		u8 m_step = 0;
		u8 m_amplitude = 0;
		bool m_pitch_mode = false;
		s8 m_pitch_depth = 0;

		void clear();
		static u32 tri_state(u32 counter);
		void keyon(swp30_device &swp);
		u16 get_amplitude() const;
		s16 get_pitch() const;
		void step(swp30_device &swp);
		// S-MU2000: 乱数を使わない分だけ進める（step の中身。戻り値は進める前のカウンタ）
		u32 advance();

		void type_step_pitch_w(u16 data);
		void amplitude_w(u16 data);

		u16 type_step_pitch_r();
		u16 amplitude_r();
	};

	struct mixer_slot {
		std::array<u16, 3> vol = {};
		std::array<u16, 3> route = {};
	};

	struct meg_state {
	public:
		static const std::array<u32, 256> lfo_increment_table;

		// S-MU2000: 命令をあらかじめ解いた形。
		// step() は 1 サンプルにつき 384 回、2 個ぶんで毎秒 3390 万回走る。
		// そのたびに 25 個ほどのビットを取り出していたので、
		// プログラムが変わったときだけ解いておく
		struct decoded {
			u8   sm = 0, sr = 0, dm = 0, dr = 0, t = 0;
			u8   mmode = 0, m1t = 0, asel = 0, rop = 0, shift = 0, clamp = 0;
			u8   dm_src = 0, memop = 0;
			bool m1_expand = false, m2_from_m = false;
			bool dr_from_r = false, no_noise = false;
			bool memw = false, index = false, t_write = false, t_from_p = false, mem_use_index = false;
			bool index2 = false, mem_use_index2 = false;   // S-MU2000: 2 つ目の idx（doc/upstream.md の 32）
			bool mem_table = false;   // S-MU2000: bit 0x23 の付いた読み出し（リバーブ RAM の絶対番地。doc/upstream.md の 24）
		};
		std::array<decoded, 0x180> m_decoded = {};
		void decode_program();

		// S-MU2000: decoded からさらに、命令ごとの判定を済ませた形。
		// run_program() が使う。**状態の保存には入れない**（meg_state の並びを
		// 変えると保存済みの状態が読めなくなる）ので、swp30_device が持つ
		struct op {
			u8  alu;                  // mmode != 0
			u8  mmode;
			u8  m1_from_t, m1_expand, m2_from_m;
			u8  asel;                 // 0 p / 1 r<<15 / 2 m<<15 / 3 p>>15 / 4 0
			u8  rop, shift, clamp;
			u8  sm, sr, dm, dr, t;
			u8  dm_src, no_noise, dr_from_r;
			u8  memw, index, t_write, t_from_p;
			u8  index2, mem_use_index2;
			u8  memop, mem_use_index, mem_table;
			u8  lfo, offset_index;
			u32 addr_mask, addr_base;   // resolve_address() を解いたもの
			u8  region;                 // どの区画（地図の何番）か。区画ごとの有効・無効を見るのに使う
			u8  latch;                // bit 0x20: 結果の符号とゼロを覚える
			u8  jump;                 // bit 0x3f: 条件つきで先へ飛ぶ（ALU もレジスタも使わない）
			u8  cond;                 // bit 0x18-0x1f
			u16 target;               // 飛び先の番地
			u16 rand_n;               // 飛ばした区画の命令: 乱数をこの回数ぶん進める（ほかの区画のディザの並びを崩さない）
		};
		void build_ops(op *ops) const;
		void run_program(const op *ops);

		swp30_device          *m_swp;
		std::array<u64, 0x180> m_program = {};
		std::array<s16, 0x180> m_const = {};
		std::array<u16,  0x80> m_offset = {};
		std::array<u16,  0x18> m_lfo = {};
		std::array<u32,  0x18> m_lfo_increment = {};
		std::array<u32,  0x18> m_lfo_counter = {};
		std::array<u16,     8> m_map = {};

		std::array<s32,  0x40> m_m = {};
		std::array<s32,  0x80> m_r = {};
		std::array<s16,     8> m_t = {};
		s64                    m_p = 0;

		std::array<s32,     3> m_mw_value = {};
		std::array<u8,      3> m_mw_reg = {};
		std::array<s32,     3> m_rw_value = {};
		std::array<u8,      3> m_rw_reg = {};
		std::array<s32,     3> m_index_value = {};
		std::array<bool,    3> m_index_active = {};
		std::array<s32,     3> m_memw_value = {};
		std::array<s32,     3> m_memr_value = {};
		std::array<s16,     2> m_t_value = {};
		std::array<bool,    3> m_memw_active = {};
		std::array<bool,    3> m_memr_active = {};
		u32                    m_delay_3 = 0;
		u32                    m_delay_2 = 0;

		u32 m_ram_read = 0, m_ram_write = 0;
		s32 m_ram_index = 0;
		u32 m_sample_counter = 0;
		u16 m_program_address = 0;
		u16 m_pc = 0;
		int m_icount = 0;
		u32 m_retval = 0;

		u16 prg_address_r();
		void prg_address_w(u16 data);
		template<int Sel> u16 prg_r();
		template<int Sel> void prg_w(u16 data);
		template<int Sel> u16 map_r();
		template<int Sel> void map_w(u16 data);
		u16 const_r(offs_t offset);
		void const_w(offs_t offset, u16 data);
		u16 offset_r(offs_t offset);
		void offset_w(offs_t offset, u16 data);
		u16 lfo_r(offs_t offset);
		void lfo_w(offs_t offset, u16 data);
		void lfo_commit_w();
		void lfo_step();
		u32 get_lfo(int lfo);
		u32 resolve_address(u16 pc, s32 offset);
		// S-MU2000: その番地がどの区画（地図の何番）に当たるか。区画ごとの
		// 有効・無効（m_revram_enable、1 が無効）を見るのに使う
		int region_of(u16 pc) const;

		static u16 revram_encode(u32 v);
		static u32 revram_decode(u16 v);
		static s16 m1_expand(s16 v);

		static void call_rand(void *ms);
		static void call_revram_encode(void *ms);
		static void call_revram_decode(void *ms);

		void step();
		void flush_writes();
		void reset();
	};

	// S-MU2000: address_space の代わりにフラットな領域を直接持つ
	running_machine m_machine;
	// このチップだけの乱数。reset で種に戻す
	u32 m_rand_seed = 0x9d14abd7, m_rand_seed_base = 0x9d14abd7;
	region_ptr<u16> m_sintab;
	std::vector<u16> m_reverb_ram;       // リバーブ RAM（18bit 空間）

	memory_access< 9, 3, -3, ENDIANNESS_LITTLE>::cache m_program_cache;
	memory_access<25, 2, -2, ENDIANNESS_LITTLE>::cache m_wave_cache;
	memory_access<18, 1, -1, ENDIANNESS_LITTLE>::cache m_reverb_cache;

	// S-MU2000: sound_stream の代わり。1 サンプル分だけ持つ
	sound_buffer m_buf;

	std::array<streaming_block, 0x40> m_streaming = {};
	// S-MU2000: チップの中のピッチ EG（doc/upstream.md の 13）。スロット 0x10（目標）、0x0B（速さ）、
	// 今の値、着いた印。streaming_block の並びを変えないよう外に置く（状態の版 2 を読めるように）
	std::array<u16, 0x40> m_pitch_offset = {};
	std::array<u16, 0x40> m_peg_rate = {};
	std::array<s32, 0x40> m_peg_cur = {};
	std::array<u32, 0x40> m_filter_cur = {};   // 滑っている途中のフィルタ 1 の切る高さ（<< 16）
	std::array<s32, 0x40> m_filter2_cur = {};  // 同じくフィルタ 2。0 のときは 0x800 下まで行く（<< 16）
	std::array<u8,  0x40> m_peg_reached = {};
public:
	// **ピッチ EG が目標に着いたか**。firmware は内部レジスタ 4 の bit14 で
	// これを見て次の段へ進む（0x12B81C）。native の口も同じものを見る
	bool peg_reached(int chan) const { return m_peg_reached[chan] != 0; }
	// **そのスロットがまだ鳴っているか**。native の口が、オルタネート
	// グループで切る相手を選ぶのに使う（doc/native-engine.md の 6.151）。
	// 実機の firmware も、鳴り終わった声は切らない
	bool slot_active(int chan) const
	{ return chan >= 0 && chan < 0x40 && m_envelope[chan].active(); }
	// **firmware がその声のスロットを空けるところ**（6.207）。firmware は
	// 内部レジスタ 0 で包絡線を読み（0x12E856）、「立ち上がりではなく、
	// 減衰が半分（0x2000）まで来た」声のスロットを空ける（0x1278C4）。
	// 空いたあとはそのスロットに一切書かないので、native もここで止める
	bool slot_freed(int chan) const
	{
		if (chan < 0 || chan >= 0x40)
			return true;
		const envelope_block &e = m_envelope[chan];
		return e.m_envelope_mode != envelope_block::ATTACK &&
			e.m_envelope_level >= 0x2000;
	}
private:
	void peg_step(int chan);
	std::array<filter_block,    0x40> m_filter = {};
	std::array<iir1_block,      0x40> m_iir1 = {};
	std::array<envelope_block,  0x40> m_envelope = {};
	std::array<lfo_block,       0x40> m_lfo = {};

	std::array<mixer_slot, 0x80> m_mixer = {};
	// S-MU2000: ミキサの振り分けを、入力ごとの「足し先と減衰」の並びにしておく。
	// route / vol が書かれたら作り直す（毎サンプル 16 出力ぶんを解くのをやめた）
	struct mix_tap {
		u8  dst;       // mixer_out の番号
		u8  frac;      // mixer_att の減衰の下 4 ビット（減衰なしは 0）
		u8  shift;     // mixer_att の減衰の上 4 ビット
	};
	std::array<std::array<mix_tap, 32>, 0x60> m_mix_taps = {};
	std::array<u8, 0x60> m_mix_ntaps = {};
	std::array<u8, 0x60> m_mix_active = {};   // S-MU2000: 振り分け先のある入力の番号（mixer_rebuild が詰める）
	u8 m_mix_nactive = 0;
	u64 m_mix_dirty[2] = { ~u64(0), ~u64(0) };   // 作り直す入力の印（0x00-0x3f、0x40-0x5f）
	void mixer_rebuild();
	void mixer_mark(int mix) { if(mix < 0x60) m_mix_dirty[mix >> 6] |= u64(1) << (mix & 63); }

	std::array<s32,  0x10> m_melo = {};
	std::array<s32,  0x10> m_meli = {};
	std::array<s32,     4> m_adc = {};

	// S-MU2000: 軽量モードの繋ぎ先（mu2000 が持っている）。ミキサから MEG への送りを
	// 横取りして 0 にし、代わりに C++ 側の出力を DAC の手前で足す
	smu2000::dsp::native_fx *m_native = nullptr;
	bool m_native_full = false;  // MEG を回さない
	int  m_native_mask = 15;     // どの口を鳴らすか（調べもの用。1 リバーブ / 2 コーラス / 4 バリエーション / 8 インサーション 1）
	s32 m_nsend[4][2] = {};      // リバーブ・コーラス・バリエーション・インサーション 1
	s32 m_ndry[2] = {};          // 乾いた音（ミキサ出力 e/f = m2e/m2f）

	// S-MU2000: DRC は使わない。meg_state はそのまま持つ
	std::unique_ptr<meg_state> m_meg_storage;
	meg_state *m_meg;
	bool m_meg_program_changed = false;
	// S-MU2000: 判定を済ませた命令表。保存しないので、読み戻したら作り直す
	std::array<meg_state::op, 0x180> m_meg_ops = {};
	bool m_meg_ops_stale = true;
	// S-MU2000: **静まった区画を回さない**（doc/native-engine.md の 6.237）。MEG のプログラムは
	// 地図（m_map）で区画に分かれていて、区画どうしはミキサを通してしかやり取りしない。
	// 入口（m20-m2f のうちその区画が読むもの）と出口（その区画が書く m20-m3f）が区画の窓の長さ
	// より長く 0 のままなら、その区画の命令を空にして回す（命令表の中で何もしない命令に替える）。
	// 入口に音が来たら、そのサンプルから元に戻す。空にした命令も乱数の種は同じだけ進めるので、
	// 回している区画はビット単位で同じ。SMU2000_MEG_SKIP=0 で使わない。状態の保存に入れる（6.237）
	struct meg_region {
		u32 in_mask = 0;          // 入口（m20-m2f。ビット = 番号 - 0x20）
		u64 out_mask = 0;         // 出口（m00-m3f のうち 0x20 から上）
		u32 hold = 0;             // 静まってから空にするまでのサンプル数（区画の窓の長さ + 余裕）
		u32 quiet = 0;            // 入口と出口が 0 のまま続いたサンプル数
		bool used = false;
	};
	std::array<meg_region, 8> m_meg_regions = {};
	u32  m_meg_skip_mask = 0;     // 空にしている区画（ビット = 区画の番号）
	bool m_meg_skip_on = true;
	bool m_meg_skip_debug = false;   // SMU2000_MEG_SKIP_DEBUG で、区画と空にしたり戻したりを出す
	void meg_regions_rebuild(bool keep_quiet);
	// プログラムの書き換えは、書き換わった命令の区画だけ戻す（地図が変わったら全部）
	std::array<u64, 6> m_meg_prg_dirty = {};   // 中身が変わった命令（ビット = 番地）
	bool m_meg_map_dirty = true;               // 地図が変わった（区画の境目が動くので全部戻す）
	// 全部の区画が空で、空の 1 サンプルを回し終えたら、MEG の状態はそれ以上動かない
	// （遅れの輪も p も同じ値に戻る）。それからは乱数の種を進めるだけにする
	bool m_meg_idle_all = false;
	bool m_meg_idle_primed = false;
	u32  m_meg_idle_rand = 0;                  // 1 サンプルで引く乱数の数（空の命令のぶんの合計）
	void meg_ops_rebuild();       // 命令表を作り直し、空にしている区画を何もしない命令に替える
	void meg_skip_before();       // ミキサのあと、MEG を回す前（入口に音が来た区画を戻す）
	void meg_skip_after();        // MEG を回したあと（静まった区画を数える）
	// S-MU2000: MEG の分岐の状態（doc/upstream.md の 11）。飛び越しは 1 サンプルの中で終わり、
	// 覚えた符号も次の比較で上書きされるので、状態の保存には入れない
	bool m_meg_flag_n = false, m_meg_flag_z = false;
	// S-MU2000: 2 つ目の idx（doc/upstream.md の 32）。idx と mw の両方が立った命令が 3 命令遅れで書き、
	// bit 0x22 の付いた読み出しが足す。meg_state の並びを変えないよう、こちらに置く
	std::array<s32, 3> m_meg_ix2_value = {};
	std::array<u8,  3> m_meg_ix2_act = {};
	s32 m_meg_ram_index2 = 0;
	u16  m_meg_skip_to = 0;

	// S-MU2000: MEG のプログラムを機械語にしたもの（swp30_jit.cpp）。命令表と同じく保存しない。
	// 環境変数 SMU2000_MEG_JIT=0 で使わない（解釈実行に戻す）
	struct meg_jit;
	static void meg_jit_delete(meg_jit *j);
	static bool meg_jit_enabled();
public:
	static u64 meg_jit_selftest();
private:
	void meg_jit_rebuild();
	void meg_jit_invalidate();
	bool meg_jit_run();
	std::unique_ptr<meg_jit, void (*)(meg_jit *)> m_jit{nullptr, &meg_jit_delete};
	// S-MU2000: MEG の定数の値が変わるたびに 1 増える（JIT の定数を焼き込んだ版を捨てる印）。
	// 状態の保存には入れない（meg_state の並びを変えると、前の版で保存した状態が読めなくなる）
	u32 m_meg_const_gen = 0;
	// S-MU2000: 軽量モードの口を MEG と同じ作りの C++（dsp/meg_fx.h）で鳴らすための控え。
	// プログラムが変わるたびに m_mfx_gen を進め、軽量モードの側で形を見分け直す
	u32  m_mfx_gen = 0, m_mfx_seen = ~0u;
	u32  m_mfx_cfg_gen[4] = {};   // 係数と番地を読んだときの書き換えの回数（違えば読み直す）
	u32  m_mfx_quiet[4] = {};     // 送りも戻りも 0 のまま続いたサンプル数
	u32  m_mfx_hold[4] = {};      // これだけ静かなら回さない（遅延の窓 + 0.1 秒）
	u32  m_meg_off_gen = 0;       // 番地表の書き換えの回数
	s32  m_mfx_out[4][8] = {};    // 戻り（MEG のレジスタと同じ目盛り）
	int  m_mfx_reg[4][8] = {};    // 戻りを書くレジスタ（m の番号）
	int  m_mfx_nout[4] = {};      // 戻りの数（0 なら MEG と同じ作りでは鳴らしていない）
	// S-MU2000: 分岐のあるプログラムを JIT で回すときの「この命令の手前まで飛ばす」位置（0 なら飛ばさない）。
	// 1 サンプルの中だけで使う。保存しない
	u32 m_meg_jit_skip = 0;
	// S-MU2000: サンプリング。m_rec_bus は録るもの（ミキサの出力 8 の左。同じサンプルの中で作って使うので保存しない）。
	// m_rec_pos は録音を始めてから書いた 16bit のサンプル数（0x30f で下の 16bit が読める）、
	// m_rec_ctrl は 0x30e に書かれた値（意味はまだ分からない。firmware は 0x001f を書く）
	s32 m_rec_bus = 0;
	u32 m_rec_pos = 0;
	u16 m_rec_ctrl = 0;
	// S-MU2000: プログラムか番地が書かれてから数えたサンプル数（0 なら JIT の作り直しを待っていない）。保存しない
	u32 m_meg_jit_wait = 0;

	u32 m_sample_counter = 0;
	u32 m_wave_adr = 0, m_wave_size = 0, m_wave_val = 0, m_revram_adr = 0, m_revram_data = 0;
	u16 m_wave_access = 0, m_revram_enable = 0;

	u64 m_keyon_mask = 0;
	// S-MU2000: 鳴っておらず、ピッチ EG も着いている声（awm2_step が回さない）。その声に何か書かれたら
	// （write16）・キーオンしたら外す。リセットと状態の読み戻しで全部外す（保存しない）
	u64 m_awm_idle = 0;
	u16 m_internal_adr = 0;

	// Streaming block trampolines
	u16 start_h_r(offs_t offset);
	u16 start_l_r(offs_t offset);
	void start_h_w(offs_t offset, u16 data);
	void start_l_w(offs_t offset, u16 data);
	u16 loop_h_r(offs_t offset);
	u16 loop_l_r(offs_t offset);
	void loop_h_w(offs_t offset, u16 data);
	void loop_l_w(offs_t offset, u16 data);
	u16 address_h_r(offs_t offset);
	u16 address_l_r(offs_t offset);
	void address_h_w(offs_t offset, u16 data);
	void address_l_w(offs_t offset, u16 data);
	u16 pitch_r(offs_t offset);
	void pitch_w(offs_t offset, u16 data);
	u16 pitch_offset_r(offs_t offset);
	void pitch_offset_w(offs_t offset, u16 data);
	u16 peg_rate_r(offs_t offset);
	void peg_rate_w(offs_t offset, u16 data);


	// Filter block trampolines
	u16 filter_1_a_r(offs_t offset);
	u16 level_1_r(offs_t offset);
	u16 filter_2_a_r(offs_t offset);
	u16 level_2_r(offs_t offset);
	u16 filter_b_r(offs_t offset);

	void filter_1_a_w(offs_t offset, u16 data);
	void level_1_w(offs_t offset, u16 data);
	void filter_2_a_w(offs_t offset, u16 data);
	void level_2_w(offs_t offset, u16 data);
	void filter_b_w(offs_t offset, u16 data);


	// IIR1 block trampolines
	template<u32 Filter> u16 a0_r(offs_t offset);
	template<u32 Filter> u16 a1_r(offs_t offset);
	template<u32 Filter> u16 b1_r(offs_t offset);
	template<u32 Filter> void a0_w(offs_t offset, u16 data);
	template<u32 Filter> void a1_w(offs_t offset, u16 data);
	template<u32 Filter> void b1_w(offs_t offset, u16 data);

	// Envelope block trampolines
	u16 attack_r(offs_t offset);
	void attack_w(offs_t offset, u16 data);
	u16 decay1_r(offs_t offset);
	void decay1_w(offs_t offset, u16 data);
	u16 decay2_r(offs_t offset);
	void decay2_w(offs_t offset, u16 data);
	u16 release_glo_r(offs_t offset);
	void release_glo_w(offs_t offset, u16 data);

	void lfo_amplitude_w(offs_t offset, u16 data);
	u16 lfo_amplitude_r(offs_t offset);
	void lfo_type_step_pitch_w(offs_t offset, u16 data);
	u16 lfo_type_step_pitch_r(offs_t offset);

	u16 internal_adr_r();
	void internal_adr_w(u16 data);
	u16 internal_r();
	template<int Sel> u16 route_r(offs_t offset);
	template<int Sel> void route_w(offs_t offset, u16 data);
	template<int Sel> u16 vol_r(offs_t offset);
	template<int Sel> void vol_w(offs_t offset, u16 data);

	static s32 volume_apply(s32 level, s32 sample);

	static s32 mixer_att(s32 sample, s32 att);

	// Control registers
	template<int Sel> u16 keyon_mask_r();
	template<int Sel> void keyon_mask_w(u16 data);
	u16 keyon_r();
	void keyon_w(u16);
	u16 meg_prg_address_r();
	void meg_prg_address_w(u16 data);
	void meg_lfo_commit_w(u16);
	template<int Sel> u16 meg_prg_r();
	template<int Sel> void meg_prg_w(u16 data);
	template<int Sel> u16 meg_map_r();
	template<int Sel> void meg_map_w(u16 data);
	template<int Sel> void wave_adr_w(u16 data);
	template<int Sel> u16 wave_adr_r();
	template<int Sel> void wave_size_w(u16 data);
	template<int Sel> u16 wave_size_r();
	void wave_access_w(u16 data);
	u16 wave_access_r();
	u16 wave_busy_r();
	template<int Sel> u16 wave_val_r();
	template<int Sel> void wave_val_w(u16 data);
	void revram_enable_w(u16 data);
	void revram_clear_w(u16 data);
	u16 revram_status_r();
	template<int Sel> void revram_adr_w(u16 data);
	template<int Sel> void revram_data_w(u16 data);
	template<int Sel> u16 revram_data_r();

	// MEG registers
	template<int Sel> u16 meg_const_r(offs_t offset);
	template<int Sel> void meg_const_w(offs_t offset, u16 data);
	template<int Sel> u16 meg_offset_r(offs_t offset);
	template<int Sel> void meg_offset_w(offs_t offset, u16 data);
	template<int Sel> u16 meg_lfo_r(offs_t offset);
	template<int Sel> void meg_lfo_w(offs_t offset, u16 data);

	u64 meg_prg_map_r(offs_t address);



	// Generic catch-all
	u16 snd_r(offs_t offset);
	void snd_w(offs_t offset, u16 data);


	void awm2_step(std::array<s32, 0x40> &samples_per_chan);
	void mixer_step(const std::array<s32, 0x40> &samples_per_chan);
	void adc_step();
	void sample_step();
};

#endif // MAME_SOUND_SWP30_H
