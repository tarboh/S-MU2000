// license:BSD-3-Clause
//
// **架空のプラグインボード、DLS の版**。DLS（Downloadable Sounds。MIDI 規格の団体が決めた、波形と音色を 1 つに
// まとめたファイル。Windows の gm.dls がこれ）を読んで鳴らす、16 パートの音源。mu2000 の VBOARD_DLS が使う。
// 実機の PLG100-XG と同じ「マルチパートのボード」として 5 つ目の口（口 E）を受け持つ（src/vboard.h、doc/plg-protocol.md）。
//
// DLS のファイルは同梱しない。使う人が自分の持っているものを選ぶ（gm.dls の中身は Roland の音色）。
//
// 読むもの（DLS Level 1 の範囲。gm.dls が使っているものを数えて決めた）
//   波形 … 8 / 16bit の PCM（ステレオは左だけ）、基準の鍵・微調整・音量・ループ（wsmp。リージョンのものが優先）
//   音色 … バンク（MSB・LSB、ドラムの印）とプログラム、リージョン（鍵と強さの範囲、キーグループ）
//   アーティキュレーション（art1 / art2。リージョンのものが優先、無ければ音色のもの）
//     EG1（音量）のアタック・ディケイ・サステイン・リリース、鍵の高さでディケイが変わる、強さでアタックが変わる
//     EG2 のアタック・ディケイ・サステイン・リリースと、それで音程を動かす深さ
//     LFO の速さ・遅れ、音程の深さ（常に掛かる分と、モジュレーションホイールで掛かる分）、音量の深さ
//     パン
// 読まないもの: フィルター（DLS Level 2）、ほかの結線、圧縮された波形
//
// MIDI: ノート、プログラムチェンジ、バンクセレクト（CC0 / CC32）、CC1・CC64・CC120・CC121・CC123、ピッチベンド（±2 半音）。
// チャンネル 10 はドラム。音量・パン・エフェクトの送りは mu2000 の側（ボードのミキサー）が掛ける

#ifndef S_MU2000_VBOARD_DLS_H
#define S_MU2000_VBOARD_DLS_H

#pragma once

#include "compat/mamecompat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace smu2000::vboard {

// ---- ファイルの中身

struct dls_art {
	// 時間は秒、サステインは 0-1。鍵・強さで変わる分はタイムセント（1200 で 2 倍）
	double eg1_attack = 0, eg1_decay = 0, eg1_sustain = 1, eg1_release = 0;
	double eg1_decay_key = 0, eg1_attack_vel = 0;
	double eg2_attack = 0, eg2_decay = 0, eg2_sustain = 1, eg2_release = 0;
	double eg2_pitch = 0;            // セント
	double lfo_hz = 5.0, lfo_delay = 0.01;
	double lfo_pitch = 0;            // セント（常に掛かる）
	double lfo_pitch_mod = 0;        // セント（モジュレーションホイールが最大のとき）
	double lfo_gain = 0;             // dB
	double pan = 0;                  // -0.5（左）〜 +0.5（右）
};

struct dls_wave {
	std::vector<s16> pcm;
	u32 rate = 22050;
	// 波形に付いている wsmp（リージョンに無いときに使う）
	u8 unity = 60;
	s16 fine = 0;
	double gain_db = 0;
	bool loop = false;
	u32 loop_start = 0, loop_len = 0;
};

struct dls_region {
	u8 key_lo = 0, key_hi = 127, vel_lo = 0, vel_hi = 127;
	u16 key_group = 0;
	int wave = -1;
	u8 unity = 60;
	s16 fine = 0;
	double gain_db = 0;
	bool loop = false;
	u32 loop_start = 0, loop_len = 0;
	dls_art art;
};

struct dls_instrument {
	bool drum = false;
	u8 msb = 0, lsb = 0, program = 0;
	std::string name;
	std::vector<dls_region> regions;
};

struct dls_bank {
	std::vector<dls_wave> waves;
	std::vector<dls_instrument> instruments;
	std::string path;

	// バンクとプログラムから音色を探す。ぴったりのものが無いときは、近いものへ順に下がる:
	//   1. MSB・LSB とも同じ
	//   2. MSB が同じで LSB が 0（GS 向けの MIDI は LSB で SC-55 などのマップを選ぶが、gm.dls のような
	//      バンクにはその LSB の音色が無い。LSB を見ないことにすれば、MSB のバリエーションはそのまま鳴る。issue #196）
	//   3. MSB・LSB とも 0（同じプログラムの基本の音色）
	//   4. 同じプログラムの最初のもの
	// LSB を持つバンク（XG 向けの DLS）は 1 で当たるので、今までどおり。ドラムは無ければ最初のキット
	const dls_instrument *find(bool drum, u8 msb, u8 lsb, u8 program) const
	{
		const dls_instrument *same_msb = nullptr, *base = nullptr, *same_prog = nullptr, *any = nullptr;
		for (const dls_instrument &i : instruments) {
			if (i.drum != drum)
				continue;
			if (!any)
				any = &i;
			if (i.program != program)
				continue;
			if (i.msb == msb && i.lsb == lsb)
				return &i;
			if (!same_msb && i.msb == msb && i.lsb == 0)
				same_msb = &i;
			if (i.msb == 0 && i.lsb == 0)
				base = &i;
			if (!same_prog)
				same_prog = &i;
		}
		return same_msb ? same_msb : base ? base : same_prog ? same_prog : drum ? any : nullptr;
	}
};

namespace dls_detail {

inline u16 rd16(const std::vector<u8> &b, size_t p) { return u16(b[p] | b[p + 1] << 8); }
inline u32 rd32(const std::vector<u8> &b, size_t p) { return u32(b[p]) | u32(b[p + 1]) << 8 | u32(b[p + 2]) << 16 | u32(b[p + 3]) << 24; }
inline bool is(const std::vector<u8> &b, size_t p, const char *id) { return std::memcmp(b.data() + p, id, 4) == 0; }

// RIFF の塊を順に。f(id の位置, 中身の位置, 中身の長さ)。壊れた長さは終わりで切る
template <typename F>
inline void each_chunk(const std::vector<u8> &b, size_t lo, size_t hi, F &&f)
{
	size_t p = lo;
	while (p + 8 <= hi) {
		const size_t n = rd32(b, p + 4);
		if (n > hi - (p + 8))
			break;
		f(p, p + 8, n);
		p += 8 + n + (n & 1);
	}
}

// タイムセント（16.16）を秒に。0x80000000 は 0 秒
inline double tc_seconds(s32 scale)
{
	if (scale == s32(0x80000000))
		return 0.0;
	return std::min(100.0, std::pow(2.0, double(scale) / 65536.0 / 1200.0));
}

struct wsmp {
	bool have = false;
	u8 unity = 60;
	s16 fine = 0;
	double gain_db = 0;
	bool loop = false;
	u32 loop_start = 0, loop_len = 0;
};

inline wsmp read_wsmp(const std::vector<u8> &b, size_t p, size_t n)
{
	wsmp w;
	if (n < 20)
		return w;
	const u32 cb = rd32(b, p);
	w.have = true;
	w.unity = u8(std::min<u32>(127, rd16(b, p + 4)));
	w.fine = s16(rd16(b, p + 6));
	w.gain_db = double(s32(rd32(b, p + 8))) / 655360.0;       // 1 が 655360 分の 1 dB。負が小さくする向き
	const u32 loops = rd32(b, p + 16);
	if (loops && cb >= 20 && cb + 16 <= n) {
		w.loop = true;
		w.loop_start = rd32(b, p + cb + 8);
		w.loop_len = rd32(b, p + cb + 12);
	}
	return w;
}

// 結線の表（art1 / art2）を読んで、分かるものだけ a に入れる
inline void read_art(const std::vector<u8> &b, size_t p, size_t n, dls_art &a)
{
	if (n < 8)
		return;
	const u32 cb = rd32(b, p), count = rd32(b, p + 4);
	for (u32 i = 0; i < count; i++) {
		const size_t q = p + cb + size_t(i) * 12;
		if (q + 12 > p + n)
			break;
		const u16 src = rd16(b, q), ctl = rd16(b, q + 2), dst = rd16(b, q + 4);
		const s32 scale = s32(rd32(b, q + 8));
		const double v = double(scale) / 65536.0;
		if (src == 0 && ctl == 0) {
			switch (dst) {
			case 0x0004: a.pan = std::clamp(v / 1000.0, -0.5, 0.5); break;
			case 0x0104: a.lfo_hz = std::clamp(8.176 * std::pow(2.0, v / 1200.0), 0.01, 40.0); break;
			case 0x0105: a.lfo_delay = tc_seconds(scale); break;
			case 0x0206: a.eg1_attack = tc_seconds(scale); break;
			case 0x0207: a.eg1_decay = tc_seconds(scale); break;
			case 0x0209: a.eg1_release = tc_seconds(scale); break;
			case 0x020a: a.eg1_sustain = std::clamp(v / 1000.0, 0.0, 1.0); break;
			case 0x030a: a.eg2_attack = tc_seconds(scale); break;
			case 0x030b: a.eg2_decay = tc_seconds(scale); break;
			case 0x030d: a.eg2_release = tc_seconds(scale); break;
			case 0x030e: a.eg2_sustain = std::clamp(v / 1000.0, 0.0, 1.0); break;
			default: break;
			}
		} else if (src == 0x0001 && ctl == 0 && dst == 0x0003) {
			a.lfo_pitch = v;
		} else if (src == 0x0001 && ctl == 0x0081 && dst == 0x0003) {
			a.lfo_pitch_mod = v;
		} else if (src == 0x0001 && ctl == 0 && dst == 0x0001) {
			a.lfo_gain = v / 10.0;                              // 10 分の 1 dB
		} else if (src == 0x0005 && ctl == 0 && dst == 0x0003) {
			a.eg2_pitch = v;
		} else if (src == 0x0003 && ctl == 0 && dst == 0x0207) {
			a.eg1_decay_key = v;
		} else if (src == 0x0002 && ctl == 0 && dst == 0x0206) {
			a.eg1_attack_vel = v;
		}
	}
}

// LIST の中の lart / lar2 を a に読む。1 つでもあれば true
inline bool read_art_lists(const std::vector<u8> &b, size_t lo, size_t hi, dls_art &a)
{
	bool any = false;
	each_chunk(b, lo, hi, [&](size_t id, size_t p, size_t n) {
		if (!is(b, id, "LIST") || n < 4 || !(is(b, p, "lart") || is(b, p, "lar2")))
			return;
		each_chunk(b, p + 4, p + n, [&](size_t id2, size_t p2, size_t n2) {
			if (is(b, id2, "art1") || is(b, id2, "art2")) {
				read_art(b, p2, n2, a);
				any = true;
			}
		});
	});
	return any;
}

} // namespace dls_detail

// DLS のファイルを読む。読めなければ空を返して err に理由
inline std::shared_ptr<dls_bank> dls_load(const std::string &path, std::string &err)
{
	using namespace dls_detail;
	// 道は UTF-8（Windows の日本語の道も通す）。512MB より大きいものは読まない
	std::vector<u8> b;
	{
		std::error_code ec;
		const std::filesystem::path p(std::u8string(path.begin(), path.end()));
		const auto size = std::filesystem::file_size(p, ec);
		std::ifstream f(p, std::ios::binary);
		if (!ec && f && size <= 512ull * 1024 * 1024)
			b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}
	if (b.size() < 12) {
		err = "cannot read the file";
		return nullptr;
	}
	if (!is(b, 0, "RIFF") || !is(b, 8, "DLS ")) {
		err = "not a DLS file";
		return nullptr;
	}
	auto bank = std::make_shared<dls_bank>();
	bank->path = path;
	size_t lins_lo = 0, lins_hi = 0, wvpl_lo = 0, wvpl_hi = 0;
	std::vector<u32> cues;
	each_chunk(b, 12, b.size(), [&](size_t id, size_t p, size_t n) {
		if (is(b, id, "LIST") && n >= 4) {
			if (is(b, p, "lins")) { lins_lo = p + 4; lins_hi = p + n; }
			if (is(b, p, "wvpl")) { wvpl_lo = p + 4; wvpl_hi = p + n; }
		} else if (is(b, id, "ptbl") && n >= 8) {
			const u32 cb = rd32(b, p), count = rd32(b, p + 4);
			for (u32 i = 0; i < count && p + cb + size_t(i) * 4 + 4 <= p + n; i++)
				cues.push_back(rd32(b, p + cb + size_t(i) * 4));
		}
	});
	if (!wvpl_hi || !lins_hi) {
		err = "the DLS file has no waves or no instruments";
		return nullptr;
	}
	// 波形。リージョンは「波形の置き場の頭からの位置」の表（ptbl）の番号で波形を指す
	std::vector<u32> wave_at;
	each_chunk(b, wvpl_lo, wvpl_hi, [&](size_t id, size_t p, size_t n) {
		if (!is(b, id, "LIST") || n < 4 || !is(b, p, "wave"))
			return;
		dls_wave w;
		u16 tag = 0, channels = 1, bits = 16;
		size_t data = 0, data_n = 0;
		each_chunk(b, p + 4, p + n, [&](size_t id2, size_t p2, size_t n2) {
			if (is(b, id2, "fmt ") && n2 >= 16) {
				tag = rd16(b, p2);
				channels = std::max<u16>(1, rd16(b, p2 + 2));
				w.rate = std::clamp<u32>(rd32(b, p2 + 4), 1000, 192000);
				bits = rd16(b, p2 + 14);
			} else if (is(b, id2, "data")) {
				data = p2;
				data_n = n2;
			} else if (is(b, id2, "wsmp")) {
				const wsmp s = read_wsmp(b, p2, n2);
				w.unity = s.unity;
				w.fine = s.fine;
				w.gain_db = s.gain_db;
				w.loop = s.loop;
				w.loop_start = s.loop_start;
				w.loop_len = s.loop_len;
			}
		});
		if (tag == 1 && (bits == 16 || bits == 8)) {
			const size_t frame = size_t(channels) * (bits / 8), frames = data_n / frame;
			w.pcm.resize(frames);
			for (size_t i = 0; i < frames; i++)
				w.pcm[i] = bits == 16 ? s16(rd16(b, data + i * frame)) : s16((int(b[data + i * frame]) - 128) << 8);
		}
		wave_at.push_back(u32(id - wvpl_lo));
		bank->waves.push_back(std::move(w));
	});
	const auto wave_of = [&](u32 table_index) {
		if (table_index >= cues.size())
			return table_index < bank->waves.size() ? int(table_index) : -1;
		for (size_t i = 0; i < wave_at.size(); i++)
			if (wave_at[i] == cues[table_index])
				return int(i);
		return -1;
	};
	// 音色
	each_chunk(b, lins_lo, lins_hi, [&](size_t id, size_t p, size_t n) {
		if (!is(b, id, "LIST") || n < 4 || !is(b, p, "ins "))
			return;
		dls_instrument ins;
		dls_art ins_art;
		read_art_lists(b, p + 4, p + n, ins_art);
		each_chunk(b, p + 4, p + n, [&](size_t id2, size_t p2, size_t n2) {
			if (is(b, id2, "insh") && n2 >= 12) {
				const u32 bk = rd32(b, p2 + 4);
				ins.drum = (bk & 0x80000000u) != 0;
				ins.msb = u8((bk >> 8) & 127);
				ins.lsb = u8(bk & 127);
				ins.program = u8(rd32(b, p2 + 8) & 127);
			} else if (is(b, id2, "LIST") && n2 >= 4 && is(b, p2, "INFO")) {
				each_chunk(b, p2 + 4, p2 + n2, [&](size_t id3, size_t p3, size_t n3) {
					if (is(b, id3, "INAM")) {
						ins.name.assign(reinterpret_cast<const char *>(b.data() + p3), n3);
						ins.name = ins.name.c_str();
						while (!ins.name.empty() && ins.name.back() == ' ')
							ins.name.pop_back();
					}
				});
			} else if (is(b, id2, "LIST") && n2 >= 4 && is(b, p2, "lrgn")) {
				each_chunk(b, p2 + 4, p2 + n2, [&](size_t id3, size_t p3, size_t n3) {
					if (!is(b, id3, "LIST") || n3 < 4 || !(is(b, p3, "rgn ") || is(b, p3, "rgn2")))
						return;
					dls_region r;
					r.art = ins_art;
					read_art_lists(b, p3 + 4, p3 + n3, r.art);
					wsmp own;
					each_chunk(b, p3 + 4, p3 + n3, [&](size_t id4, size_t p4, size_t n4) {
						if (is(b, id4, "rgnh") && n4 >= 12) {
							r.key_lo = u8(std::min<u16>(127, rd16(b, p4)));
							r.key_hi = u8(std::min<u16>(127, rd16(b, p4 + 2)));
							r.vel_lo = u8(std::min<u16>(127, rd16(b, p4 + 4)));
							r.vel_hi = u8(std::min<u16>(127, rd16(b, p4 + 6)));
							r.key_group = rd16(b, p4 + 10);
						} else if (is(b, id4, "wsmp")) {
							own = read_wsmp(b, p4, n4);
						} else if (is(b, id4, "wlnk") && n4 >= 12) {
							r.wave = wave_of(rd32(b, p4 + 8));
						}
					});
					if (r.wave < 0 || bank->waves[size_t(r.wave)].pcm.empty())
						return;
					const dls_wave &w = bank->waves[size_t(r.wave)];
					r.unity = own.have ? own.unity : w.unity;
					r.fine = own.have ? own.fine : w.fine;
					r.gain_db = own.have ? own.gain_db : w.gain_db;
					r.loop = own.have ? own.loop : w.loop;
					r.loop_start = own.have ? own.loop_start : w.loop_start;
					r.loop_len = own.have ? own.loop_len : w.loop_len;
					if (r.loop && (r.loop_len < 2 || u64(r.loop_start) + r.loop_len > w.pcm.size()))
						r.loop = false;
					ins.regions.push_back(r);
				});
			}
		});
		if (!ins.regions.empty())
			bank->instruments.push_back(std::move(ins));
	});
	if (bank->instruments.empty()) {
		err = "the DLS file has no playable instrument";
		return nullptr;
	}
	return bank;
}

// ---- 音源

class dls_synth
{
public:
	static constexpr int VOICES = 128;
	static constexpr int CHANNELS = 16;
	static constexpr double RATE = 44100.0;
	static constexpr int BLOCK = 16;             // 音程とエンベロープを計算し直す間隔（サンプル）
	static constexpr double RETRIGGER_RELEASE = 0.01;   // ドラムを打ち直したときに前の声を消す速さ（96dB ぶんの秒）

	void set_bank(std::shared_ptr<const dls_bank> bank)
	{
		m_bank = std::move(bank);
		reset();
	}
	const dls_bank *bank() const { return m_bank.get(); }

	void reset()
	{
		for (voice &v : m_v)
			v.on = false;
		for (int c = 0; c < CHANNELS; c++) {
			m_ch[c] = channel();
			m_ch[c].drum = c == 9;
		}
		m_age = 0;
		m_tick = 0;
	}

	// チャンネルメッセージ 1 つ
	void midi(u8 status, u8 d0, u8 d1)
	{
		channel &c = m_ch[status & 15];
		const int chan = status & 15;
		switch (status & 0xf0) {
		case 0x90:
			if (d1) {
				note_on(chan, d0, d1);
				break;
			}
			[[fallthrough]];
		case 0x80:
			for (voice &v : m_v)
				if (v.on && v.chan == chan && v.key == d0 && !v.released) {
					if (c.pedal)
						v.held = true;
					else
						release(v);
				}
			break;
		case 0xb0:
			switch (d0) {
			case 0:  c.msb_next = d1; break;
			case 32: c.lsb_next = d1; break;
			case 1:  c.mod = d1 / 127.0; break;
			case 64:
				c.pedal = d1 >= 64;
				if (!c.pedal)
					for (voice &v : m_v)
						if (v.on && v.chan == chan && v.held)
							release(v);
				break;
			case 120:
				for (voice &v : m_v)
					if (v.chan == chan)
						v.on = false;
				break;
			case 121:
				c.mod = 0;
				c.bend = 0;
				c.pedal = false;
				break;
			case 123:
				for (voice &v : m_v)
					if (v.on && v.chan == chan && !v.released)
						release(v);
				break;
			default: break;
			}
			break;
		case 0xc0:
			c.msb = c.msb_next;
			c.lsb = c.lsb_next;
			c.program = d0 & 127;
			break;
		case 0xe0:
			c.bend = (((d1 << 7) | d0) - 8192) / 8192.0 * 200.0;      // セント
			break;
		default:
			break;
		}
	}

	// チャンネルをドラムのパートにする・メロディに戻す（GS の「リズムパートに使う」、XG のパートのモード）。
	// 何もしなければチャンネル 10 だけがドラム
	void set_drum(int chan, bool drum) { m_ch[size_t(chan & 15)].drum = drum; }

	// チャンネルのいまの音色（画面に出す）。ins は、その設定で実際に鳴る音色（無ければ nullptr）
	void channel_voice(int chan, u8 &msb, u8 &lsb, u8 &program, bool &drum, const dls_instrument *&ins) const
	{
		const channel &c = m_ch[size_t(chan & 15)];
		msb = c.msb;
		lsb = c.lsb;
		program = c.program;
		drum = c.drum;
		ins = m_bank ? m_bank->find(c.drum, c.msb, c.lsb, c.program) : nullptr;
	}

	bool sounding() const
	{
		for (const voice &v : m_v)
			if (v.on)
				return true;
		return false;
	}

	// 鳴っている声の数（試験用）
	int active_voices() const
	{
		int n = 0;
		for (const voice &v : m_v)
			n += v.on;
		return n;
	}

	// 1 サンプル。チャンネルごとの左右（パン込み）を out[チャンネル][0/1] に足す。±1.0 が全振幅
	void render(float out[CHANNELS][2])
	{
		const bool tick = (m_tick++ % BLOCK) == 0;
		for (voice &v : m_v) {
			if (!v.on)
				continue;
			if (tick) {
				control(v);
				if (!v.on)
					continue;
			}
			// 波形を線で補って読む
			const std::vector<s16> &pcm = *v.pcm;
			const size_t i = size_t(v.pos);
			const double frac = v.pos - double(i);
			const size_t j = (v.loop && i + 1 >= v.loop_end) ? v.loop_start : i + 1;
			const double a = pcm[i], b = j < pcm.size() ? pcm[j] : 0.0;
			const double s = (a + (b - a) * frac) * (1.0 / 32768.0) * v.amp;
			v.amp += v.amp_step;
			out[v.chan][0] += float(s * v.pan_l);
			out[v.chan][1] += float(s * v.pan_r);
			v.pos += v.inc;
			if (v.loop) {
				if (v.pos >= double(v.loop_end))
					v.pos -= double(v.loop_end - v.loop_start);
			} else if (v.pos >= double(pcm.size() - 1)) {
				v.on = false;
			}
		}
	}

private:
	struct channel {
		bool drum = false, pedal = false;
		u8 msb = 0, lsb = 0, msb_next = 0, lsb_next = 0, program = 0;
		double bend = 0.0, mod = 0.0;
	};
	struct voice {
		bool on = false, released = false, held = false, loop = false;
		int chan = 0;
		u8 key = 60;
		u16 key_group = 0;
		u32 age = 0;
		const std::vector<s16> *pcm = nullptr;
		size_t loop_start = 0, loop_end = 0;
		double pos = 0.0, inc = 0.0, base_cents = 0.0, rate_ratio = 1.0;
		double gain = 1.0, amp = 0.0, amp_step = 0.0, pan_l = 1.0, pan_r = 1.0;
		dls_art art;
		// EG1 は dB で動く（アタックだけ直線）。level は 0（いちばん大きい）〜 -96
		int eg1_phase = 0;           // 0 アタック、1 ディケイ、2 サステイン、3 リリース
		double eg1_lin = 0.0, eg1_db = -96.0, eg1_attack = 0.0, eg1_decay = 0.0;
		int eg2_phase = 0;
		double eg2 = 0.0;
		double lfo_phase = 0.0, lfo_wait = 0.0;
	};

	void release(voice &v)
	{
		v.released = true;
		v.held = false;
		if (v.eg1_phase == 0)
			v.eg1_db = v.eg1_lin > 0.0 ? std::max(-96.0, 20.0 * std::log10(v.eg1_lin)) : -96.0;
		v.eg1_phase = 3;
		v.eg2_phase = 3;
	}

	void note_on(int chan, u8 key, u8 vel)
	{
		if (!m_bank)
			return;
		const channel &c = m_ch[chan];
		const dls_instrument *ins = m_bank->find(c.drum, c.msb, c.lsb, c.program);
		if (!ins)
			return;
		// ドラムで同じ鍵をもう一度打ったら、前の声を短く消す（GM の音源と同じ）。ドラムの余韻が長い DLS
		// （SoundMAX の 4 MB General MIDI.dls はループした音で 40 秒）だと、これをしないとハイハットや
		// スネアの連打が 128 の声を使い切り、ほかのパートの声が押し出されて抜ける（#237）。
		// ばっさり止めるとプツッと鳴るので、10ms で消す
		if (c.drum)
			for (voice &o : m_v)
				if (o.on && o.chan == chan && o.key == key && !(o.released && o.art.eg1_release <= RETRIGGER_RELEASE)) {
					release(o);
					o.art.eg1_release = RETRIGGER_RELEASE;
				}
		for (const dls_region &r : ins->regions) {
			if (key < r.key_lo || key > r.key_hi || vel < r.vel_lo || vel > r.vel_hi)
				continue;
			// 同じキーグループ（ハイハットの開閉など）で鳴っている声は切る
			if (r.key_group)
				for (voice &o : m_v)
					if (o.on && o.chan == chan && o.key_group == r.key_group)
						o.on = false;
			const dls_wave &w = m_bank->waves[size_t(r.wave)];
			// 空いている声。無ければ、いちばん小さく鳴っている声を譲ってもらう（立ち上がりの途中の声は避ける）。
			// 「離された声の古い順」にはしない: ドラムはノートオフがすぐ来るので、シンバルの余韻やタムが
			// 真っ先に切られてしまう
			voice *use = nullptr;
			for (voice &v : m_v)
				if (!v.on) {
					use = &v;
					break;
				}
			if (!use)
				for (voice &v : m_v)
					if (v.eg1_phase != 0 && (!use || v.amp < use->amp))
						use = &v;
			if (!use)
				for (voice &v : m_v)
					if (!use || v.age < use->age)
						use = &v;
			voice &v = *use;
			v = voice();
			v.on = true;
			v.chan = chan;
			v.key = key;
			v.key_group = r.key_group;
			v.age = ++m_age;
			v.pcm = &w.pcm;
			v.loop = r.loop;
			v.loop_start = r.loop_start;
			v.loop_end = size_t(r.loop_start) + r.loop_len;
			v.rate_ratio = double(w.rate) / RATE;
			v.base_cents = (double(key) - double(r.unity)) * 100.0 + double(r.fine);
			v.art = r.art;
			// 強さは 2 乗の曲線（DLS の決まり: 減衰 = 20 log((127 / 強さ)^2)）
			const double vv = double(vel) / 127.0;
			v.gain = vv * vv * std::pow(10.0, r.gain_db / 20.0) * 0.4;      // 0.4: 和音や曲で振り切れにくい大きさ
			const double pan = std::clamp(0.5 + r.art.pan, 0.0, 1.0);
			v.pan_l = std::sqrt(1.0 - pan) * 1.41421356;
			v.pan_r = std::sqrt(pan) * 1.41421356;
			v.eg1_attack = r.art.eg1_attack * std::pow(2.0, r.art.eg1_attack_vel * vv / 1200.0);
			v.eg1_decay = r.art.eg1_decay * std::pow(2.0, r.art.eg1_decay_key * (double(key) / 128.0) / 1200.0);
			v.lfo_wait = r.art.lfo_delay;
			// 包絡線の 1 歩目をここで進めておく。立ち上がりの無い音（ドラムなど）はこれで最大になる。
			// これをしないと、ノートオンのすぐ後（次の計算の前）にノートオフが来た音が、音量 0 のまま
			// リリースに入って消える（ゲートの短いドラムが鳴らない）
			control(v);
			break;
		}
	}

	// BLOCK サンプルに 1 度: エンベロープ・LFO を進めて、音程と音量の行き先を決める
	void control(voice &v)
	{
		const double dt = double(BLOCK) / RATE;
		const channel &c = m_ch[v.chan];
		// EG1
		switch (v.eg1_phase) {
		case 0:
			v.eg1_lin = v.eg1_attack > dt ? std::min(1.0, v.eg1_lin + dt / v.eg1_attack) : 1.0;
			if (v.eg1_lin >= 1.0) {
				v.eg1_phase = 1;
				v.eg1_db = 0.0;
			}
			break;
		case 1: {
			const double floor_db = -96.0 * (1.0 - v.art.eg1_sustain);
			v.eg1_db = v.eg1_decay > dt ? v.eg1_db - 96.0 * dt / v.eg1_decay : floor_db;
			if (v.eg1_db <= floor_db) {
				v.eg1_db = floor_db;
				v.eg1_phase = 2;
			}
			break;
		}
		case 3:
			v.eg1_db = v.art.eg1_release > dt ? v.eg1_db - 96.0 * dt / v.art.eg1_release : -96.0;
			break;
		default:
			break;
		}
		if (v.eg1_phase != 0 && v.eg1_db <= -95.9) {
			v.on = false;
			return;
		}
		// EG2（0-1。音程に掛ける）
		switch (v.eg2_phase) {
		case 0:
			v.eg2 = v.art.eg2_attack > dt ? std::min(1.0, v.eg2 + dt / v.art.eg2_attack) : 1.0;
			if (v.eg2 >= 1.0)
				v.eg2_phase = 1;
			break;
		case 1:
			v.eg2 = v.art.eg2_decay > dt ? v.eg2 - dt / v.art.eg2_decay : v.art.eg2_sustain;
			if (v.eg2 <= v.art.eg2_sustain) {
				v.eg2 = v.art.eg2_sustain;
				v.eg2_phase = 2;
			}
			break;
		case 3:
			v.eg2 = v.art.eg2_release > dt ? std::max(0.0, v.eg2 - dt / v.art.eg2_release) : 0.0;
			break;
		default:
			break;
		}
		// LFO（遅れのあと正弦）
		double lfo = 0.0;
		if (v.lfo_wait > 0.0) {
			v.lfo_wait -= dt;
		} else {
			v.lfo_phase += v.art.lfo_hz * dt;
			v.lfo_phase -= std::floor(v.lfo_phase);
			lfo = std::sin(v.lfo_phase * 6.283185307179586);
		}
		const double cents = v.base_cents + c.bend + v.art.eg2_pitch * v.eg2 +
		                     lfo * (v.art.lfo_pitch + v.art.lfo_pitch_mod * c.mod);
		v.inc = v.rate_ratio * std::pow(2.0, cents / 1200.0);
		const double env = v.eg1_phase == 0 ? v.eg1_lin : std::pow(10.0, v.eg1_db / 20.0);
		const double target = v.gain * env * std::pow(10.0, lfo * v.art.lfo_gain / 20.0);
		v.amp_step = (target - v.amp) / double(BLOCK);
	}

	std::shared_ptr<const dls_bank> m_bank;
	std::array<voice, VOICES> m_v{};
	std::array<channel, CHANNELS> m_ch{};
	u32 m_age = 0, m_tick = 0;
};

} // namespace smu2000::vboard

#endif // S_MU2000_VBOARD_DLS_H
