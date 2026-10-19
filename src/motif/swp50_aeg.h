// license:BSD-3-Clause
// MOTIF-RACK ES のエレメントの音量の EG（AEG）と、音量のベロシティ感度・キーフォロー
// （2026-10-20。tools/motif/mr_aeg.py の録音を mr_aeg_fit.py で読んだ。正弦波の波形 704、フィルター THRU）。
//
//   高さ（41 ee 28〜2B: 初め・アタック・ディケイ 1・ディケイ 2）: 40 × log10(L / 127) dB。測った 14 点と 0.6dB 以内。L = 0・1 は無音
//   段の長さ（41 ee 24〜27: アタック・ディケイ 1・ディケイ 2・リリース）: FEG と同じ swp50_eg::time_down(127 − 値)で、高さの差によらない。
//             ディケイ 1 の値 64 で 127 → 96・64・32・8 が 0.78〜0.79 秒。下がる段は dB でまっすぐ、無音へは −96dB まで下がって消える。
//             上がる段は振幅でまっすぐ、ずっと短い時間で着き（rise_seconds）、段の残りはそのまま待つ
//             （初めとアタックが同じ 127 なら、アタックの段は 0.79 秒続いた）
//   時間のベロシティ感度（41 ee 2E、対象 2D は ALL でだけ測った）: 時間の値が 1.0 ×（感度 − 64）×（v − 1）/ 126 速くなる
//   時間のキーフォロー（41 ee 3C、64 が 0、中心は 41 ee 3D）: 速さの番号（127 − 時間の値）に 1 + 3.2e-4 ×（感度 − 64）×（鍵 − 中心）を掛ける。
//             時間の値を一定にずらす形だと、ディケイ 1 = 64 と 80 で合わない（感度 +32・鍵 84 で 15.1 と 12.0 目盛り）
//   音量のベロシティ感度（41 ee 21、64 が 0）とカーブ（41 ee 22）: ベロシティ 127（負の感度なら 1）で下げず、そこから離れるほど下げる。
//             カーブ 0 では dB で (127 − v) に比例し、傾きは感度 +16・+32・+48・+56・+60 で 0.235・0.47・0.95・1.93・3.9 dB。
//             +32 から先は 15.5 / (128 − 感度) dB。計算で 55dB を超えると少し急になり、58dB を超えると無音（感度 +32 の v = 1、+48 の v = 64 など）。
//             負の感度は v を 128 − v に裏返した同じ形（−16・−48 で確かめた）。カーブ 1〜4 は感度 +32 で測った表を、
//             カーブ 0 との傾きの比で伸ばす（ほかの感度では確かめていない）
//   音量のキーフォロー（41 ee 2F、64 が 0）: 鍵 60 を中心に、片側だけ下げる（正なら低い鍵、負なら高い鍵）。
//             音量の値の倍率 1 − 1.116e-4 × x^1.107、x = |感度 − 64| × |鍵 − 60|（+32・+63 で 12・24 半音下が −1.46・−3.33・−3.21・−7.86dB、
//             負の感度で上も同じ）
#pragma once

#include <cmath>
#include <cstdint>

#include "swp50_eg.h"

namespace swp50_aeg {

static constexpr double SILENT = -999.0;

// ベロシティ 1, 8, 16, …, 120, 127 の 17 点。感度 +32（96）・カーブ 0〜4 で測った、ベロシティ 127 からの dB
static constexpr float VEL_POINTS[17] = { 1, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127 };
static constexpr float S = float(SILENT);
static constexpr float VEL_S32[5][17] = {
	{ S, -63.9f, -55.7f, -50.0f, -45.0f, -41.1f, -37.0f, -33.0f, -29.6f, -25.6f, -22.2f, -18.6f, -14.6f, -11.0f, -7.2f, -3.3f, 0.0f },
	{ S, S, S, -77.2f, -71.2f, -65.2f, -59.2f, -53.1f, -47.1f, -41.1f, -35.1f, -29.1f, -23.0f, -17.0f, -11.0f, -5.0f, 0.0f },
	{ S, S, -74.7f, -66.2f, -57.1f, -49.7f, -42.4f, -35.6f, -29.1f, -23.6f, -18.3f, -13.5f, -9.7f, -6.3f, -3.3f, -1.2f, 0.0f },
	{ S, -48.2f, -36.1f, -28.6f, -24.1f, -19.8f, -16.6f, -13.8f, -12.1f, -9.3f, -7.5f, -6.3f, -4.5f, -2.9f, -1.8f, -0.9f, 0.0f },
	{ S, S, S, -76.8f, -70.3f, -63.9f, -56.7f, -49.0f, -29.6f, -13.2f, -8.9f, -6.9f, -5.0f, -2.9f, -1.8f, -0.8f, 0.0f },
};

// カーブ 0 の傾き（dB / ベロシティ）。|感度 − 64| = a
static inline double vel_slope(int a)
{
	if (a <= 0)
		return 0.0;
	if (a <= 16)
		return 0.235 * a / 16.0;
	if (a <= 32)
		return 0.235 + (0.47 - 0.235) * (a - 16) / 16.0;
	return 15.5 / (64.0 - (a > 63 ? 63 : a));     // +32 で 0.484、+48 で 0.97、+56 で 1.94、+60 で 3.9
}

static inline double vel_interp(const float *tab, double v)
{
	if (v <= VEL_POINTS[0])
		return tab[0];
	int i = 0;
	while (i < 15 && v > VEL_POINTS[i + 1])
		i++;
	if (tab[i] <= S || tab[i + 1] <= S)
		return v >= VEL_POINTS[i + 1] ? tab[i + 1] : SILENT;
	const double t = (v - VEL_POINTS[i]) / (VEL_POINTS[i + 1] - VEL_POINTS[i]);
	return tab[i] + t * (tab[i + 1] - tab[i]);
}

// 音量のベロシティ感度で下げる量（dB、0 以下。無音は SILENT）
static inline double velocity_db(int sens, int curve, int velocity)
{
	const int s = sens - 64;
	if (s == 0)
		return 0.0;
	const int a = s < 0 ? -s : s;
	const double v = s > 0 ? velocity : 128 - velocity;
	const double k = vel_slope(a);
	if (curve <= 0 || curve > 4) {
		// カーブ 0: dB で (127 − v) に比例。55dB を超えると急になり（感度 +32 の v = 8 が式 57.6 に対し 63.9dB）、58dB を超えると無音
		const double lin = k * (127 - v);
		if (lin > 58.0)
			return SILENT;
		return -(lin + 2.0 * (lin > 55.0 ? lin - 55.0 : 0.0));
	}
	const double t = vel_interp(VEL_S32[curve], v);
	if (t <= SILENT)
		return SILENT;
	return t * k / vel_slope(32);
}

// 音量のキーフォローの、音量の値への倍率
static inline double key_follow_factor(int sens, int key)
{
	const int s = sens - 64;
	const int d = key - 60;
	if (s == 0 || (s > 0 && d >= 0) || (s < 0 && d <= 0))
		return 1.0;
	const double x = double(s < 0 ? -s : s) * (d < 0 ? -d : d);
	const double f = 1.0 - 1.116e-4 * std::pow(x, 1.107);
	return f < 0 ? 0 : f;
}

// 高さ L（0..127）の dB
static inline double level_db(double L)
{
	return L <= 1.0 ? SILENT : 40.0 * std::log10(L / 127.0);
}

// 段の行き先の dB。無音へ行く段は −96dB（255 目盛り）を目指してまっすぐ下がり、着いたら無音
static constexpr double FLOOR_DB = -96.0;

// 上がる段で、振幅 0 → 1 にかかる秒（初め 0 → アタック 127 で、値 32・48・64・80 が 2.5・10・57.5・254ms。間は対数で補間、外は端の傾き）
static inline double rise_seconds(double t)
{
	static const double p[4] = { 32, 48, 64, 80 }, sec[4] = { 0.0025, 0.010, 0.0575, 0.254 };
	if (t <= 0)
		return 0.0;
	const int i = t < p[1] ? 0 : t < p[2] ? 1 : 2;
	const double u = (t - p[i]) / (p[i + 1] - p[i]);
	return std::exp(std::log(sec[i]) + u * (std::log(sec[i + 1]) - std::log(sec[i])));
}

static constexpr double TIME_VEL_STEPS = 1.0;      // 時間の値のずれ =（感度 − 64）×（v − 1）/ 126（速くなる向き）
static constexpr double TIME_KF_RATE = 3.2e-4;     // 速さの番号（127 − 時間の値）の倍率 = 1 + これ ×（感度 − 64）×（鍵 − 中心）

struct amp_eg {
	int level[4] = { 0, 127, 127, 127 };   // 初め・アタック・ディケイ 1・ディケイ 2（0..127）
	int time[4] = { 0, 0, 0, 0 };          // アタック・ディケイ 1・ディケイ 2・リリース（0..127）
	int time_vel_sens = 64, velocity = 100;
	int time_kf = 64, time_kf_center = 60, key = 60;

	int seg = 0;                           // 0 アタック、1 ディケイ 1、2 ディケイ 2、3 そのまま、4 リリース、5 終わり
	double db = SILENT;                    // 今の dB
	double from = 0, to = 0, len = 0, left = 0;
	bool rising = false;
	double amp = 0, amp_to = 0, amp_step = 0;

	double shifted(int t) const
	{
		double v = t - TIME_VEL_STEPS * (time_vel_sens - 64) * (velocity - 1) / 126.0;
		const double r = (127 - v) * (1.0 + TIME_KF_RATE * (time_kf - 64) * (key - time_kf_center));
		return 127 - r;
	}
	static double seg_samples_at(double t)
	{
		if (t <= 0)
			return 0.0;
		const double r = 127 - (t > 127 ? 127 : t);
		const int i = int(r);
		const double a = swp50_eg::time_down(uint32_t(i < 1 ? 1 : i)), b = swp50_eg::time_down(uint32_t(i + 1 > 127 ? 127 : i + 1));
		return std::exp(std::log(a) + (r - i) * (std::log(b) - std::log(a)));
	}

	static double to_db(int L) { const double d = level_db(L); return d <= SILENT ? FLOOR_DB : d; }

	void begin(int s, double target_db, int t)
	{
		seg = s;
		from = db <= SILENT ? FLOOR_DB : db;
		to = target_db;
		len = left = t <= 0 ? 0.0 : seg_samples_at(shifted(t));
		rising = to > from;
		if (rising) {
			// 上がる段は振幅でまっすぐ、短い時間で着き、段の残りはそのまま待つ
			amp = std::pow(10.0, from / 20.0);
			amp_to = std::pow(10.0, to / 20.0);
			const double rs = rise_seconds(t <= 0 ? 0 : shifted(t)) * 44100.0;
			amp_step = rs <= 0 ? 1.0 : 1.0 / rs;
		}
	}

	void key_on()
	{
		db = level_db(level[0]);
		begin(0, to_db(level[1]), time[0]);
	}
	void key_off()
	{
		if (seg < 4)
			begin(4, FLOOR_DB, time[3] <= 0 ? 0 : time[3]);
	}
	bool finished() const { return seg >= 5; }

	// 1 サンプル進めて、振幅の倍率を返す
	double step()
	{
		while (left <= 0 && seg != 3 && seg != 5) {
			db = to;                                   // 今の段の行き先に着いた
			if (seg <= 1)
				begin(seg + 1, to_db(level[seg + 2]), time[seg + 1]);
			else
				seg = seg == 2 ? 3 : 5;
		}
		if (seg == 5)
			return 0.0;
		if (left > 0) {
			left -= 1;
			if (rising) {
				amp = std::fmin(amp_to, amp + amp_step);
				db = 20.0 * std::log10(amp);
			} else {
				db = from + (to - from) * (1.0 - left / len);
			}
		}
		return db <= FLOOR_DB ? 0.0 : std::pow(10.0, db / 20.0);
	}
};

} // namespace swp50_aeg
