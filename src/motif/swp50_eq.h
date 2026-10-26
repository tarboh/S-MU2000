// license:BSD-3-Clause
// MOTIF-RACK ES のエレメントの EQ（Data List の 44 00-07）の模型。実機の録音に当てはめた（2026-10-20、tools/motif/mr_eq.py、
// doc/motif-rack-es-capture.md の「エレメントの EQ」）。ノイズの波形 705・鍵 60・フィルター THRU で、EQ THRU との差を見た。
//
// * ゲイン（0x04・0x07、0x40 が 0）は 1 あたり 0.375dB（±32 で ±12dB）。Filter Gain と同じ単位
// * EQ L/H（型 0）: EQ1 は低域の棚、EQ2 は高域の棚。どちらも RBJ の棚の式で傾き S = 0.5（誤差 0.01〜0.04dB）
// * P.EQ（型 1）: EQ1 だけの山（EQ2 は使わない）。RBJ の山の式で Q = 0.5 × 2^(レゾナンス / 8)（0・8・16・24・31 で 0.50・1.01・2.06・4.21・7.51）。
//   周波数の端（値 83・99・115 と 227・243）では Q が小さめに出るので、測った倍率で直す
// * Boost6 / 12 / 18（型 2-4）: 素直なゲイン +6.0 / +12.1 / +18.1dB（どの周波数でも同じ）。音の大きいノイズで録ると、チップの中で頭打ちになって
//   形が崩れたように見える（エレメントの音量 127 で Boost18 の中域が +13dB しか出なかった）。この模型は頭打ちを入れていない
// * 周波数の値 → 中心周波数は 3 つとも別の表（どれもおよそ 25 前後で 1 オクターブ）。16 刻みで測った点を対数で補間し、外は端の傾きで延ばす
#pragma once

#include <cmath>

namespace swp50_eq {

enum : int { LH = 0, PEQ = 1, BOOST6 = 2, BOOST12 = 3, BOOST18 = 4, THRU = 5 };

static constexpr double FS = 44100.0;
static constexpr double PI = 3.14159265358979323846;
static constexpr double GAIN_DB = 0.375;

struct points { double v0, step; int n; double hz[11]; };
static constexpr points EQ1_LOW = { 46, 16, 9, { 134, 216, 313, 491, 778, 1129, 1814, 2734, 4121 } };
static constexpr points EQ2_HIGH = { 131, 16, 7, { 769, 1173, 1838, 2700, 4228, 6291, 8780 } };
static constexpr points PEQ_FREQ = { 83, 16, 11, { 81, 176, 352, 503, 799, 1251, 1838, 3030, 4868, 7724, 15433 } };
// P.EQ の Q の倍率（レゾナンス 16 で測った Q ÷ 2.06）
static constexpr points PEQ_QFAC = { 83, 16, 11, { 0.316, 0.655, 1.141, 1.019, 1.019, 1.019, 0.966, 0.966, 0.913, 0.816, 0.398 } };

static inline double interp(const points &p, double v)
{
	double x = (v - p.v0) / p.step;
	int i = int(std::floor(x));
	if (i < 0)
		i = 0;
	if (i > p.n - 2)
		i = p.n - 2;
	const double t = x - i;
	return std::exp(std::log(p.hz[i]) + t * (std::log(p.hz[i + 1]) - std::log(p.hz[i])));
}

struct biquad {
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
	void set(double B0, double B1, double B2, double A0, double A1, double A2)
	{
		b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
	}
	double process(double x)
	{
		const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1; x1 = x; y2 = y1; y1 = y;
		return y;
	}
	void reset() { x1 = x2 = y1 = y2 = 0; }
};

static inline void low_shelf(biquad &q, double f0, double gdb, double S)
{
	const double A = std::pow(10.0, gdb / 40), w = 2 * PI * f0 / FS, c = std::cos(w), s = std::sin(w);
	const double al = s / 2 * std::sqrt((A + 1 / A) * (1 / S - 1) + 2), sa = 2 * std::sqrt(A) * al;
	q.set(A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
	      (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}

static inline void high_shelf(biquad &q, double f0, double gdb, double S)
{
	const double A = std::pow(10.0, gdb / 40), w = 2 * PI * f0 / FS, c = std::cos(w), s = std::sin(w);
	const double al = s / 2 * std::sqrt((A + 1 / A) * (1 / S - 1) + 2), sa = 2 * std::sqrt(A) * al;
	q.set(A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
	      (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}

static inline void peaking(biquad &q, double f0, double gdb, double Q)
{
	const double A = std::pow(10.0, gdb / 40), w = 2 * PI * f0 / FS, c = std::cos(w), s = std::sin(w), al = s / (2 * Q);
	q.set(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

// マルチのパートの EQ（Data List の 37 pp 2E〜38）。エレメントの EQ と同じ部品: 低域の棚（EQ1 の表）・中域の山（P.EQ の表、Q = 0.5 × 2^(幅 / 8) × 倍率）・
// 高域の棚（EQ2 の表）。利きは 1 目盛り 0.375dB（0x40 が 0、±32 で ±12dB）。ノイズの音色をパート 1 に置き、平らなときとの比で 23 組を測って、
// 二乗平均 1.7dB（ノイズの揺れと同じくらい。棚の傾き 0.7・1.0 や Q の倍率なしは少し悪い。2026-10-27、tools/motif/mr_part_eq.py）
struct part_eq {
	biquad lo[2], mid[2], hi[2];
	bool flat = true;
	void set(int lf, int lg, int mf, int mg, int mq, int hf, int hg)
	{
		flat = lg == 0x40 && mg == 0x40 && hg == 0x40;
		for (int c = 0; c < 2; c++) {
			low_shelf(lo[c], interp(EQ1_LOW, lf), (lg - 0x40) * GAIN_DB, 0.5);
			peaking(mid[c], interp(PEQ_FREQ, mf), (mg - 0x40) * GAIN_DB, 0.5 * std::exp2(mq / 8.0) * interp(PEQ_QFAC, mf));
			high_shelf(hi[c], interp(EQ2_HIGH, hf), (hg - 0x40) * GAIN_DB, 0.5);
		}
	}
	void process(double &L, double &R)
	{
		if (flat)
			return;
		L = hi[0].process(mid[0].process(lo[0].process(L)));
		R = hi[1].process(mid[1].process(lo[1].process(R)));
	}
};

struct element_eq {
	int type = THRU;
	double gain = 1.0;          // Boost の倍率
	biquad q1, q2;

	// type・resonance・EQ1 の周波数とゲイン（0x40 が 0）・EQ2 の周波数とゲイン（Data List の値のまま）
	void set(int t, int resonance, int f1, int g1, int f2, int g2)
	{
		type = t;
		gain = 1.0;
		q1 = biquad();
		q2 = biquad();
		const double G1 = (g1 - 0x40) * GAIN_DB, G2 = (g2 - 0x40) * GAIN_DB;
		switch (t) {
		case LH:
			low_shelf(q1, interp(EQ1_LOW, f1), G1, 0.5);
			high_shelf(q2, interp(EQ2_HIGH, f2), G2, 0.5);
			break;
		case PEQ:
			peaking(q1, interp(PEQ_FREQ, f1), G1, 0.5 * std::exp2(resonance / 8.0) * interp(PEQ_QFAC, f1));
			break;
		case BOOST6:  gain = std::pow(10.0, 6.0 / 20); break;
		case BOOST12: gain = std::pow(10.0, 12.1 / 20); break;
		case BOOST18: gain = std::pow(10.0, 18.1 / 20); break;
		default: break;
		}
	}
	void reset() { q1.reset(); q2.reset(); }
	double process(double x)
	{
		if (type == LH)
			return q2.process(q1.process(x));
		if (type == PEQ)
			return q1.process(x);
		return x * gain;
	}
};

} // namespace swp50_eq
