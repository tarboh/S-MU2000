// license:BSD-3-Clause
// MOTIF-RACK ES の録音だけで作るサンプリング方式のソフトシンセの、エレメント 1 つを鳴らす芯（doc/motif-rack-es-sampler.md）。
// サンプル（tools/motif/mr_extract_all.py で切り出した「出だし + ループ」か鳴らし切り）を、実機の決まりで読み、測った部品を通す。
//
//   読む速さ: 録った鍵を元の高さとし、鍵ごとに 1 オクターブ = 1024 目盛りへ丸める（実機と同じ。隣の鍵との差は 85 か 86 目盛り）。
//             コースチューン・ファインチューン（1 = 1.5625 セント）も目盛りに足す。3 倍で折り返す決まり（swp50.h）は、
//             本当の元の鍵が分からないので入れていない
//   補間    : 4 点（エルミート）
//   その後  : FEG つきのフィルター（swp50_filter.h）→ EQ（swp50_eq.h）→ エレメントの音量 → パン
//   音量    : swp50_aeg::level_db（測った表）。レベルスケーリングを足してからキーフォローを掛ける
//   音量の EG: swp50_aeg.h（高さ・段の長さ・時間のベロシティ感度とキーフォロー、音量のベロシティ感度とカーブ、音量のキーフォロー）
//   LFO     : swp50_lfo.h（音程・音量・フィルター）
//   音程    : PEG（FEG と同じ形の swp50_filter::filter_eg を swp50_peg.h の目盛りで）、音程のキーフォロー・ベロシティ感度
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "swp50_aeg.h"
#include "swp50_eg.h"
#include "swp50_eq.h"
#include "swp50_filter.h"
#include "swp50_lfo.h"
#include "swp50_peg.h"

namespace motif_sampler {

struct sample {
	std::vector<float> L, R;
	int loop_start = -1;          // -1 = 鳴らし切り
	int loop_length = 0;
	int key = 60;                 // 録った鍵
	double stretch = 1.0;         // 切り出しのときの伸び縮み（録った鍵の高さで鳴らすには、1 サンプルあたり 1 / stretch 進む）
};

// 音色のコモン LFO（40 06 00）。tools/motif/mr_clfo.py で測った:
// 速さ 0.824 × 2^((値 − 32) / 8) Hz（8・16・32・40・48・56・63 で 0.095・0.206・0.824・1.64・3.28・6.54・11.97Hz）。テンポ同期は表 14 の音符 × 60 / テンポ。
// PMD は 16 上がるごとに 2.2 倍（16・32・64・96・127 で ±9.4・21・102・486・2189 セント）。AMD は波形の負の側だけ効き、
// エレメントの音量の値（41 e 20）を 深さ × 振れ だけ下げる（ユーザー波形の −1 を深さ 32・64・96・113 で −4.5・−12.5・−24.5・−37.5dB、127 で無音。
// 音量 96 で深さ 64 なら −24dB、音量 64 で深さ 64・音量 96 で深さ 96 は無音。Eclipse の音量 100 は深さ 104 で無音）。エレメント側の割合（44 ee 51〜53）は深さに掛かる（深さ × 割合 / 127）。
// 遅れ（5）・フェードイン（6）は値に対して指数的（遅れ 64・96・127 で 0.18・2.03・11.2 秒、フェードイン 64・96・127 で 0.27・2.3・12 秒）。
// 波形: 0 三角（0 から上がる）・1 tri+（0〜1）・2 のこぎり上（0〜1）・3 のこぎり下・4〜8 方形（1/4・1/3・1/2・2/3・3/4、高い側から）・9 台形・10/11 S/H・12 ユーザー。
// 行き先（tools/motif/mr_clfo_dest.py）: FMD はカットオフを 2 × 深さ × 振れ 目盛り、RESO はレゾナンスを 深さ / 2 × 振れ、エレメント LFO の速さは
// 速さの値を 深さ / 2 × 振れ（8 で 2 倍）動かす。PAN は振れにまっすぐ比例するが、いちばん振れたときのパンのずれが深さ 8・16・32・64 で 18・33・48・63
// （倍にするごとに +15、64 から上は 63 のまま）。左右は同じ（三角で測った。ユーザー波形の −1 だけ左が浅かったが入れていない）。
// RESO の −1 の側は、もとが 64 以下だと 50 前後より下がらなかった（もと 96 では ±16 で対称）。入れていない。
// ホールド（7、127 は続ける）は遅れのあとから数え、64・83・96・113・126 で 0.45・1.25・2.3・5.6・11 秒。そのあとフェードアウト（8）で
// 揺れが dB でまっすぐ −30dB まで小さくなって止まる（64・94・123 で 0.45・2.2・9.3 秒）
struct common_lfo {
	bool on = false;
	int phase = 0, wave = 0, speed = 32, tspeed = 11, sync = 0, delay = 0, fadein = 0, reset = 1, hold = 127, fadeout = 0;
	int dest[3] = {0, 0, 0}, depth[3] = {0, 0, 0};
	int usteps = 0, uvals[16] = {0};
	int tempo = 120;
	double hz() const
	{
		if (sync) {
			static const double beats[17] = {0.25, 1 / 3.0, 0.375, 0.5, 2 / 3.0, 0.75, 1, 4 / 3.0, 1.5, 2, 8 / 3.0, 3, 4, 5, 6, 7, 8};
			const int i = std::max(0, std::min(16, tspeed - 5));
			return 1.0 / (beats[i] * 60.0 / std::max(1, tempo));
		}
		return 0.824 * std::exp2((speed - 32) / 8.0);
	}
	static double interp(const double *x, const double *y, int n, double v)
	{
		if (v <= x[0])
			return y[0];
		for (int i = 0; i < n - 1; i++)
			if (v <= x[i + 1])
				return y[i] + (v - x[i]) / (x[i + 1] - x[i]) * (y[i + 1] - y[i]);
		return y[n - 1];
	}
	static double delay_s(int v)
	{
		static const double x[6] = {0, 16, 32, 64, 96, 127}, y[6] = {0, 0.002, 0.02, 0.18, 2.03, 11.2};
		return interp(x, y, 6, v);
	}
	static double fade_s(int v)
	{
		static const double x[5] = {0, 32, 64, 96, 127}, y[5] = {0, 0.025, 0.27, 2.3, 12.1};
		return interp(x, y, 5, v);
	}
	static double hold_s(int v)
	{
		static const double x[6] = {0, 64, 83, 96, 113, 126}, y[6] = {0, 0.45, 1.25, 2.3, 5.6, 11.0};
		return interp(x, y, 6, v);
	}
	static double fadeout_s(int v)
	{
		static const double x[6] = {0, 32, 64, 94, 123, 127}, y[6] = {0, 0.05, 0.45, 2.2, 9.3, 12.0};
		return interp(x, y, 6, v);
	}
	// PAN: いちばん振れたときのパンのずれ（目盛り）
	static double pan_units(double d)
	{
		static const double x[5] = {0, 8, 16, 32, 64}, y[5] = {0, 18.4, 33, 48, 63};
		return interp(x, y, 5, d);
	}
	static double pmd_cents(double d)
	{
		static const double x[6] = {0, 16, 32, 64, 96, 127}, y[6] = {0, 9.4, 21, 102, 486, 2189};
		return interp(x, y, 6, d);
	}
	// 波形の値（-1〜1）。ph は 0〜1 の位相、cyc は何周目か（S/H の乱数に使う）
	double wave_at(double ph, long cyc) const
	{
		switch (wave) {
		case 0: return ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4;
		case 1: return ph < 0.5 ? 2 * ph : 2 - 2 * ph;
		case 2: return ph;
		case 3: return 1 - ph;
		case 4: return ph < 0.25 ? 1 : -1;
		case 5: return ph < 1 / 3.0 ? 1 : -1;
		case 6: return ph < 0.5 ? 1 : -1;
		case 7: return ph < 2 / 3.0 ? 1 : -1;
		case 8: return ph < 0.75 ? 1 : -1;
		case 9: return ph < 0.125 ? 8 * ph : ph < 0.375 ? 1 : ph < 0.625 ? 1 - 8 * (ph - 0.375) : ph < 0.875 ? -1 : -1 + 8 * (ph - 0.875);
		case 10:
		case 11: {
			unsigned h = unsigned(cyc) * 2654435761u + 12345u;
			h ^= h >> 13;
			h *= 0x5bd1e995u;
			h ^= h >> 15;
			return (h % 2001) / 1000.0 - 1.0;
		}
		case 12: {
			static const int nsteps[7] = {2, 3, 4, 6, 8, 12, 16};
			const int n = nsteps[std::max(0, std::min(6, usteps))];
			const int k = std::min(n - 1, int(ph * n));
			return (uvals[k] - 64) / 63.0;
		}
		}
		return 0;
	}
};

struct element_params {
	int coarse = 0;               // 半音
	int fine = 0;                 // -64..+63（1 = 1.5625 セント）
	int level = 127;
	double gain = 1.0;            // Filter Gain（録った雛形の 250 からの比）
	int pan = 64;                 // 1..127、64 が中央
	int scaling_pan = 64;         // スケーリングパンの深さ（64 が 0）
	common_lfo clfo;              // 音色のコモン LFO（clfo.on が偽なら使わない）
	int lbox = 0;                 // このエレメントが受ける箱（ビット 0〜2 = 箱 1〜3）
	int lratio[4] = {0, 127, 127, 127};   // 位相のずらし（0〜5）と箱 1〜3 の深さの割合
	int level_vel_sens = 64, level_vel_curve = 0, level_kf = 64;
	int level_bp[4] = { 0, 0, 0, 0 }, level_off[4] = { 128, 128, 128, 128 };   // 音量のレベルスケーリング
	swp50_aeg::amp_eg aeg;        // 時間・高さ・時間の感度（鍵とベロシティは start で入れる）
	swp50_lfo::element_lfo lfo;
	swp50_filter::filter_eg peg;  // PEG（depth が深さ、level_factor が高さのベロシティ感度）
	int pitch_kf = 96, pitch_kf_center = 60, pitch_vel_sens = 64;
	swp50_filter::filter_params filter;
	swp50_filter::filter_eg feg;
	int eq_type = swp50_eq::THRU, eq_res = 16, eq_f1 = 100, eq_g1 = 0x40, eq_f2 = 200, eq_g2 = 0x40;
};

// 鍵 note を、録った鍵 key のサンプルで鳴らすときの 1 サンプルあたりの進み
static inline double step_for(const sample &s, int note, int coarse, int fine)
{
	const long reg = std::lround((note + coarse - s.key) * 1024.0 / 12.0) + std::lround(fine * 1.5625 * 1024.0 / 1200.0);
	return std::exp2(reg / 1024.0) / s.stretch;
}

static inline double level_gain(double level)
{
	const double d = swp50_aeg::level_db(level);
	return d <= swp50_aeg::SILENT ? 0.0 : std::pow(10.0, d / 20.0);
}

// コントロールセット（40 05 00、6 組の ソース・行き先・深さ）の、今のソースの値で決まるずれ（tools/motif/mr_ctrl.py・mr_ctrl_depth.py、2026-10-25）。
// どの行き先も「パラメーターの値に p × R を足す」形で、p は 深さ − 64 を 64 で割り、片側のソース（MW・AT・BC・FC）は 値 / 127、
// 中心から両側へ振れるソース（PB・AS1・AS2・RB）は（値 − 64）/ 64 を掛けたもの。R は行き先ごと（ctrl_dest_scale）。
// エレメントの行き先（41 から上）は、エレメントのコントロールボックス（41 ee 18、2 バイト）のビットが立っている組だけ受ける
struct ctrl_mod {
	double level = 0, pan = 0, cents = 0, cutoff = 0, reso = 0, hpf = 0, pmod = 0, amod = 0, fmod = 0, lfo_speed = 0;
	double clfo[3] = {0, 0, 0};               // コモン LFO の箱 1〜3 の深さのずれ
};

// 行き先の番号（ファームウェアの名前の表の順: 1 Volume・2 RevSend・3 ChoSend・4〜19 INS1・20〜35 INS2・36 CLFOSpd・37〜39 CLFO の深さ・40 PorTime・
// 41 ELM-Lvl・42 ELM-Pan・43 ELM-Dly・44 ELFOSpd・45〜47 ELFO-PM/AM/FM・48 PCH-Crs・49 PCH-Fin・50〜55 PEG・56〜59 AEG・60〜65 FEG・66 FLT-Frq・67 FLT-Rez・68 HPF-Frq）
// ごとに、p = 1 のときのずれ。正弦波とノイズの雛形で深さ 0・32・48・80・96・127 とソースの値 0〜127 を振って測った:
//   音量（41）: エレメントの音量の値を 265（127 から 深さ −16・−32 × 値 127・96 で −13.1・−25.2dB、p = −0.5 で無音）。上げる側は 127 で頭打ちなので確かめていない
//   Volume（1）: 音色の音量の値を 265（CTRL_VOLUME_SCALE。音量は 40·log10(値 / 127) dB。雛形の 127 と、02_049 の 87 で MW × −13 が −16.3dB の両方に合う）。鳴らす側で掛ける
//   パン（42）: パンの目盛りを 128（深さ +16 × 値 127 でパン 32 ずれ、−6.3dB）
//   コース（48）: 48 半音、±24 半音で頭打ち。ファイン（49）: 128 目盛り（1.5625 セント）、±64 目盛り（100 セント）で頭打ち
//   エレメント LFO の深さ（45〜47）: 180（もと 32 の音程の揺れが p = 0.25・0.378・0.5 で ±66・179・470 セント相当）
//   カットオフ（66）: 512 目盛り（もと 100 から p = ±0.0625・0.125 で ±32・64 目盛り。ノイズの重心を、カットオフを振って録った表で目盛りに直した）
//   コモン LFO の深さ（37〜39）: 240、127 で頭打ち（箱 1 を音程・深さ 16 にして、p = 0.125・0.25・0.378 で深さ 44・76・110 相当）。
//                      音色全体の行き先なので、エレメントのボックスによらない
//   レゾナンス（67）: 128、+63 で頭打ち（もと 0 から p = 0.25・0.378 で +31・+47 相当、p = 0.5 から上は 62 のまま）
// EG の時間・深さ（50〜65）・ディレイ（43）は、鳴っている間に CC を動かしても変わらなかった（キーオンのときだけ読むらしい。入れていない）。
// HPF（68）も雛形（LPF24D）では変わらなかったが、HPF は LPF12・LPF6 の型だけで効くため。カットオフと同じ 512 とした
static constexpr double CTRL_VOLUME_SCALE = 265;

static inline bool ctrl_dest_scale(int dest, double &r)
{
	switch (dest) {
	case 41: r = 265; return true;
	case 42: r = 128; return true;
	case 48: r = 48 * 100; return true;          // セント
	case 49: r = 128 * 1.5625; return true;      // セント
	case 44: r = 128; return true;               // 測っていない
	case 45: case 46: case 47: r = 180; return true;
	case 66: r = 512; return true;
	case 67: r = 128; return true;
	case 37: case 38: case 39: r = 240; return true;
	case 68: r = 512; return true;               // カットオフと同じとした（03_014 で確かめた）
	}
	return false;
}

static inline void ctrl_add(ctrl_mod &m, int dest, double p)
{
	double r;
	if (!ctrl_dest_scale(dest, r))
		return;
	const double x = p * r;
	switch (dest) {
	case 41: m.level += x; break;
	case 42: m.pan += x; break;
	case 48: m.cents += std::max(-2400.0, std::min(2400.0, x)); break;
	case 49: m.cents += std::max(-100.0, std::min(100.0, x)); break;
	case 44: m.lfo_speed += x; break;
	case 45: m.pmod += x; break;
	case 46: m.amod += x; break;
	case 47: m.fmod += x; break;
	case 66: m.cutoff += x; break;
	case 67: m.reso += std::min(63.0, x); break;
	case 68: m.hpf += x; break;
	case 37: case 38: case 39: m.clfo[dest - 37] += x; break;
	}
}

struct voice {
	const sample *smp = nullptr;
	element_params p;
	int note = 60, velocity = 100;
	double pos = 0, step = 1;
	bool on = false, released = false, done = false;
	double level = 127, vel_gain = 1;     // キーフォローを掛けた音量の値と、ベロシティ感度の倍率
	double static_cents = 0;              // 音程のキーフォロー・ベロシティ感度のずれ
	swp50_filter::element_filter flt, flt_r;
	swp50_eq::element_eq eq, eq_r;
	double gl = 1, gr = 1, pan_offset = 0;
	long tick = 0;                        // 鳴り始めからのサンプル数（コモン LFO）
	ctrl_mod cm;                          // コントロールセットのずれ（鳴らす側が毎サンプル入れる）
	int lfo_base[4] = {0, 0, 0, 32};      // エレメント LFO の音程・音量・フィルターの深さと速さのもと

	// コモン LFO の今の値。音程（セント）と音量の値の下げ幅（エレメントの音量の目盛り。AMD は深さ × 負の側の振れだけ音量を下げる）、
	// カットオフとレゾナンスとパンのずれ（目盛り）、エレメント LFO の速さの倍率
	struct clfo_out {
		double cents = 0, drop = 0, cutoff = 0, reso = 0, pan = 0, speed_mul = 1;
		bool filter = false, panning = false;
	};
	void common_lfo_now(clfo_out &o) const
	{
		const common_lfo &c = p.clfo;
		if (!c.on || !p.lbox)
			return;
		const double t = tick / 44100.0, dl = common_lfo::delay_s(c.delay);
		if (t < dl)
			return;
		const double fd = common_lfo::fade_s(c.fadein);
		double f = fd > 0 ? std::min(1.0, (t - dl) / fd) : 1.0;
		if (c.hold < 127) {
			const double th = t - dl - common_lfo::hold_s(c.hold), fo = common_lfo::fadeout_s(c.fadeout);
			if (th >= fo)
				return;
			if (th > 0)
				f *= std::pow(10.0, -1.5 * th / fo);
		}
		static const double offs[6] = {0, 90, 120, 180, 240, 270};
		const double ph0 = (offs[std::max(0, std::min(5, c.phase))] + offs[std::max(0, std::min(5, p.lratio[0]))]) / 360.0;
		const double x = t * c.hz() + ph0;
		const double ph = x - std::floor(x);
		const double w = c.wave_at(ph, long(std::floor(x)));
		for (int b = 0; b < 3; b++) {
			const double db = std::max(0.0, std::min(127.0, c.depth[b] + cm.clfo[b]));
			if (!(p.lbox >> b & 1) || db <= 0)
				continue;
			const double d = db * p.lratio[b + 1] / 127.0;
			switch (c.dest[b]) {
			case 0: o.drop += d * std::max(0.0, -w) * f; break;
			case 1: o.cents += common_lfo::pmd_cents(d) * w * f; break;
			case 2: o.cutoff += 2.0 * d * w * f; o.filter = true; break;
			case 3: o.reso += 0.5 * d * w * f; o.filter = true; break;
			case 4: o.pan += common_lfo::pan_units(d) * w * f; o.panning = true; break;
			case 5: o.speed_mul *= std::exp2(d * w * f / 16.0); break;
			}
		}
	}

	void start(const sample &s, const element_params &ep, int n, int v)
	{
		smp = &s; p = ep; note = n; velocity = v;
		pos = 0; step = step_for(s, n, p.coarse, p.fine);
		tick = 0;
		on = true; released = false; done = false;
		p.aeg.key = n;
		p.aeg.velocity = v;
		p.aeg.key_on();
		p.lfo.key_on();
		lfo_base[0] = p.lfo.pmod; lfo_base[1] = p.lfo.amod; lfo_base[2] = p.lfo.fmod; lfo_base[3] = p.lfo.speed;
		p.peg.key = n;
		p.peg.velocity = v;
		p.peg.key_on();
		static_cents = (swp50_peg::key_follow_ratio(p.pitch_kf) - 1.0) * 100.0 * (n - p.pitch_kf_center)
			+ swp50_peg::velocity_cents(p.pitch_vel_sens, v);
		level = std::fmin(127.0, std::fmax(0.0, p.level + swp50_aeg::level_scaling(p.level_bp, p.level_off, n)));
		level *= swp50_aeg::key_follow_factor(p.level_kf, n);
		const double vd = swp50_aeg::velocity_db(p.level_vel_sens, p.level_vel_curve, v);
		vel_gain = vd <= swp50_aeg::SILENT ? 0.0 : std::pow(10.0, vd / 20.0);
		if (vel_gain == 0.0)
			done = true;
		p.feg.key = n;
		p.feg.velocity = v;
		p.feg.key_on();
		flt.set(p.filter, n, v);
		flt.reset();
		flt_r = flt;
		eq.set(p.eq_type, p.eq_res, p.eq_f1, p.eq_g1, p.eq_f2, p.eq_g2);
		eq.reset();
		eq_r = eq;
		// パン: 実機は近い側が 0dB のまま、遠い側だけ下がる（中央から 16・32・48・63 で −1.2・−6.3・−17.5dB・無音。tools/motif/mr_pan.py）。
		// スケーリングパンは鍵ごとのずれ（深さ 127 で鍵 36・48・60・72・84・96 が −48・−21・0・+12・+38・+63）を、(深さ − 64) / 63 倍して足す
		double o = (p.pan < 1 ? 1 : p.pan > 127 ? 127 : p.pan) - 64;
		{
			static const double kx[6] = {36, 48, 60, 72, 84, 96}, ky[6] = {-48, -21, 0, 12, 38, 63};
			double sk;
			if (n <= kx[0])
				sk = ky[0] + (n - kx[0]) * (ky[1] - ky[0]) / 12.0;
			else if (n >= kx[5])
				sk = 63;
			else {
				int i = 0;
				while (n > kx[i + 1])
					i++;
				sk = ky[i] + (n - kx[i]) / 12.0 * (ky[i + 1] - ky[i]);
			}
			o += std::max(-63.0, std::min(63.0, sk)) * (p.scaling_pan - 64) / 63.0;
		}
		pan_offset = o;
		pan_gains(o);
	}

	// パンのずれ（中央から、目盛り）から左右の倍率を決める
	void pan_gains(double o)
	{
		o = std::max(-63.0, std::min(63.0, o));
		static const double px[5] = {0, 16, 32, 48, 63}, py[5] = {1.0, 0.871, 0.484, 0.133, 0.0};
		const double ao = std::fabs(o);
		int j = 0;
		while (j < 3 && ao > px[j + 1])
			j++;
		const double far = py[j] + (ao - px[j]) / (px[j + 1] - px[j]) * (py[j + 1] - py[j]);
		gl = o > 0 ? far : 1.0;
		gr = o < 0 ? far : 1.0;
	}

	void release()
	{
		if (!on || released)
			return;
		released = true;
		p.feg.key_off();
		p.aeg.key_off();
		p.peg.key_off();
	}

	float at(const std::vector<float> &x, long i) const
	{
		const sample &s = *smp;
		if (s.loop_start >= 0) {
			const long end = s.loop_start + s.loop_length;
			while (i >= end)
				i -= s.loop_length;
		}
		if (i < 0 || i >= long(x.size()))
			return 0.0f;
		return x[size_t(i)];
	}

	// 1 サンプル進めて、左右を足し込む
	void render(float &outl, float &outr)
	{
		if (!on || done)
			return;
		const sample &s = *smp;
		const long i = long(std::floor(pos));
		const double t = pos - i;
		auto herm = [&](const std::vector<float> &x) {
			const double y0 = at(x, i - 1), y1 = at(x, i), y2 = at(x, i + 1), y3 = at(x, i + 2);
			const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
			return ((c3 * t + c2) * t + c1) * t + y1;
		};
		double l = herm(s.L), r = herm(s.R);
		{   // 深さを毎サンプル もと + ずれ に戻す（ずれが 0 に戻ったときも、もとの深さに戻る）
			auto clamp127 = [](double v) { return std::max(0, std::min(127, int(std::lround(v)))); };
			p.lfo.pmod = clamp127(lfo_base[0] + cm.pmod);
			p.lfo.amod = clamp127(lfo_base[1] + cm.amod);
			p.lfo.fmod = clamp127(lfo_base[2] + cm.fmod);
			p.lfo.speed = clamp127(lfo_base[3] + cm.lfo_speed);
		}
		p.lfo.step(44100.0);
		p.peg.step();
		double cents = static_cents;
		if (p.peg.depth != 64)
			cents += swp50_peg::cents(p.peg.value * p.peg.level_factor, p.peg.depth);
		if (p.lfo.pmod)
			cents += p.lfo.pitch_cents_now();
		clfo_out co;
		common_lfo_now(co);
		cents += co.cents + cm.cents;
		p.lfo.speed_mul = co.speed_mul;
		tick++;
		pos += cents != 0.0 ? step * std::exp2(cents / 1200.0) : step;
		if (s.loop_start < 0 && pos >= double(s.L.size()))
			done = true;
		// FEG を目盛りに足してフィルターの g を決め直すのは重いので、FEG の深さが 0 でなければ毎サンプル（芯の試しなので素直に書く）
		const double fs = p.feg.step();
		if (p.feg.depth != 64 || p.lfo.fmod || co.filter || cm.cutoff != 0 || cm.reso != 0 || cm.hpf != 0) {
			swp50_filter::filter_params fp = p.filter;
			// カットオフのベロシティ感度は FEG より前に足す（FEG で 255 を越える所で頭打ちにしてから下げると暗くなりすぎる。
			// 2026-10-24、Crazy Mod のエレメント 1 を感度 +40・ベロシティ 40 で実機と比べた）
			// コントロールセットのずれは、目盛りに足して 0〜255 で止めてから FEG を足す（00_060 の AS1 × +63 を最小へ回すと、実機は −12dB で
			// 明るさはほぼそのまま。止めずに FEG と一緒に足すと 0 より下へ延びて、暗く・大きくなりすぎた）
			double base = p.filter.cutoff + swp50_filter::velocity_steps(p.filter.vel_sens, velocity);
			if (cm.cutoff != 0)
				base = std::max(0.0, std::min(255.0, base + cm.cutoff));
			fp.cutoff = int(std::lround(swp50_filter::cutoff_with_feg(base, fs + p.lfo.filter_octaves_now() * 25.6 + co.cutoff, p.filter.type)));
			fp.vel_sens = 64;
			fp.reso = std::max(0, std::min(127, int(std::lround(p.filter.reso + co.reso + cm.reso))));
			if (cm.hpf != 0)
				fp.hpf = std::max(0, std::min(255, int(std::lround(p.filter.hpf + cm.hpf))));
			swp50_filter::element_filter tmp;
			tmp.set(fp, note, velocity);
			flt.hg = flt_r.hg = tmp.hg;
			flt.g = flt_r.g = tmp.g;
			flt.g2 = flt_r.g2 = tmp.g2;
			flt.q = flt_r.q = tmp.q;
			flt.ga = flt_r.ga = tmp.ga;
			flt.gtop = flt_r.gtop = tmp.gtop;
			flt.wop = flt_r.wop = tmp.wop;
			flt.gop = flt_r.gop = tmp.gop;
		}
		// フィルターゲインはフィルターのあとで掛け、そこで ±2.0（出口の最大の 2 倍）で頭打ちにする
		// （2026-10-24、tools/motif/mr_filter_sat.py: 発振した LPF24D の大きさがフィルターゲインに比例し、
		// フィルターのあとのエレメントの音量を 64・32 に下げても四角い頭打ちの形が残る。頭打ちは出口の基準で振幅 2 倍の所）
		l = eq.process(std::fmax(-FILTER_CLIP, std::fmin(FILTER_CLIP, flt.process(float(l)) * float(p.gain))));
		r = eq_r.process(std::fmax(-FILTER_CLIP, std::fmin(FILTER_CLIP, flt_r.process(float(r)) * float(p.gain))));
		const double eg = p.aeg.step();
		if (p.aeg.finished()) {
			done = true;
			return;
		}
		const double g = eg * vel_gain * level_gain(std::min(127.0, level + cm.level) - co.drop) * (p.lfo.amod ? std::pow(10.0, p.lfo.amp_db_now() / 20.0) : 1.0);
		if (co.panning || cm.pan != 0)
			pan_gains(pan_offset + co.pan + cm.pan);
		// 離したあと出力がほぼ聞こえない（倍率 −80dB 未満）間は、実機はリリースを 4 倍の速さで進める
		// （tools/motif/mr_clfo_gate_release.py: コモン LFO の AMD で消している間に離すと、開いたあとが小さい・鳴らない。Eclipse 03_087）
		if (released && g * p.gain < QUIET_GAIN)
			for (int k = 0; k < 3 && !p.aeg.finished(); k++)
				p.aeg.step();
		outl += float(l * gl * g);
		outr += float(r * gr * g);
	}
	static constexpr double QUIET_GAIN = 1e-4;
	static constexpr float FILTER_CLIP = 2.0f;
};

} // namespace motif_sampler
