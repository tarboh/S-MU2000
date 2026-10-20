// license:BSD-3-Clause
// MOTIF-RACK ES のエレメントの音量の EG（AEG）と、音量のベロシティ感度・キーフォロー
// （2026-10-20。tools/motif/mr_aeg.py の録音を mr_aeg_fit.py で読んだ。正弦波の波形 704、フィルター THRU）。
//
//   高さ（41 ee 28〜2B: 初め・アタック・ディケイ 1・ディケイ 2）: 測った 14 点の表（level_db）。40 × log10(L / 127) に近いが上で 0.6dB 大きい。
//             L = 0・1 は無音
//   段の長さ（41 ee 24〜27: アタック・ディケイ 1・ディケイ 2・リリース）: FEG と同じ swp50_eg::time_down(127 − 値)で、高さの差によらない。
//             ディケイ 1 の値 64 で 127 → 96・64・32・8 が 0.78〜0.79 秒。下がる段は dB でまっすぐ、無音へは −96dB まで下がって消える。
//             上がる段は振幅でまっすぐ、ずっと短い時間で着き（rise_seconds）、段の残りはそのまま待つ
//             （初めとアタックが同じ 127 なら、アタックの段は 0.79 秒続いた）
//   時間のベロシティ感度（41 ee 2E。効く段は 2D: 0 アタック・1 アタックとディケイ 1・2 ディケイ 1 と 2・3 アタックとリリース・4 全部。実機で確かめた）: 時間の値が 1.0 ×（感度 − 64）×（v − 1）/ 126 速くなる
//   時間のキーフォロー（41 ee 3C、64 が 0、中心は 41 ee 3D）: 段の長さに 2^(−0.002 ×（感度 − 64）×（鍵 − 中心））を掛ける。
//             基の時間 80・88・96・104 のどれでも同じ倍率（感度 +32・24 鍵上で 0.34 倍、+63 で 0.125 倍、中心 24 でも同じ式）。
//             時間の値を一定にずらす形や、速さの番号に掛ける形は合わない。ディケイ 2 にも同じ倍率で効く（+63・24 鍵上で 3.12 → 0.38 秒）。
//             リリースにも効くとした（同じ条件で 10dB 下がるまでが 0.078 → 0.036 秒。読みが粗く、倍率は確かめていない）
//   音量のベロシティ感度（41 ee 21、64 が 0）とカーブ（41 ee 22）: ベロシティ 127（負の感度なら 1）で下げず、そこから離れるほど下げる。
//             下げる量は x = 実効の感度 ×（127 − v）だけで決まり、カーブごとの表（VEL_X_TABLE）で引く。カーブ 0 は x にほぼ比例
//             （+16・+32 で 0.235・0.47 dB / ベロシティ）、1 はその 1.6 倍、2 は下へ曲がり、3 は上へ曲がり、4 は x = 2000 前後で 20dB 跳ぶ。
//             負の感度は v を 128 − v に裏返した同じ形（−16・−48 で確かめた）
//   音量のキーフォロー（41 ee 2F、64 が 0）: 鍵 60 を中心に、片側だけ下げる（正なら低い鍵、負なら高い鍵）。
//             音量の値の倍率 1 − 1.116e-4 × x^1.107、x = |感度 − 64| × |鍵 − 60|（+32・+63 で 12・24 半音下が −1.46・−3.33・−3.21・−7.86dB、
//             負の感度で上も同じ）
#pragma once

#include <cmath>
#include <cstdint>

#include "swp50_eg.h"

namespace swp50_aeg {

static constexpr double SILENT = -999.0;

// 音量のベロシティ感度: 下げる量は x = 実効の感度 ×（127 − v）だけで決まる（カーブごと）。実効の感度は |感度 − 64| = a が 32 までは a、
// その先は 1024 / (64 − a)（+48 で 64、+56 で 128、+60 で 256）。x = 0, 128, …, 4096 の 33 点の dB（感度 +8・+16・+24・+32・+48 と、
// カーブ 0 は +56・+60 も入れて作った。測った 47〜75 点との差は二乗平均 0.4〜0.6dB、いちばん大きい所で 3〜4.5dB）。
// VEL_SILENT_X より x が大きいと無音
static constexpr float VEL_X_TABLE[5][33] = {
	{ 0.0f, -1.7f, -3.5f, -5.8f, -7.4f, -9.2f, -11.3f, -13.5f, -14.9f, -17.3f, -19.4f, -20.8f, -22.8f, -24.8f, -27.3f, -28.6f, -30.0f,
	  -31.7f, -33.5f, -35.6f, -37.5f, -39.6f, -41.8f, -44.3f, -45.7f, -48.1f, -50.7f, -53.6f, -56.7f, -60.8f, -64.9f, -64.9f, -64.9f },
	{ 0.0f, -2.1f, -5.2f, -8.0f, -11.3f, -14.0f, -17.3f, -20.1f, -23.4f, -26.9f, -30.3f, -32.9f, -36.5f, -38.9f, -42.4f, -45.0f, -47.8f,
	  -50.8f, -53.8f, -57.2f, -60.0f, -63.2f, -66.1f, -69.1f, -72.0f, -75.0f, -78.0f, -78.0f, -78.0f, -78.0f, -78.0f, -78.0f, -78.0f },
	{ 0.0f, -0.4f, -1.2f, -1.9f, -3.4f, -5.0f, -6.3f, -7.9f, -10.0f, -12.5f, -14.3f, -16.9f, -19.2f, -21.6f, -24.6f, -27.2f, -29.8f,
	  -33.0f, -36.3f, -40.4f, -43.4f, -47.3f, -50.8f, -54.6f, -58.2f, -62.8f, -67.3f, -71.6f, -75.9f, -75.9f, -75.9f, -75.9f, -75.9f },
	{ 0.0f, -0.3f, -0.9f, -1.2f, -1.9f, -2.5f, -2.9f, -3.7f, -4.7f, -5.6f, -6.6f, -7.3f, -7.9f, -8.6f, -9.8f, -11.1f, -12.2f,
	  -13.1f, -14.2f, -15.7f, -17.1f, -18.9f, -20.3f, -22.2f, -24.7f, -26.9f, -29.5f, -32.9f, -37.6f, -43.7f, -49.8f, -49.8f, -49.8f },
	{ 0.0f, -0.3f, -0.8f, -1.2f, -1.8f, -2.5f, -2.9f, -4.2f, -5.1f, -6.4f, -7.2f, -8.3f, -9.9f, -11.7f, -14.2f, -19.4f, -33.8f,
	  -43.8f, -49.9f, -54.3f, -57.8f, -61.9f, -65.1f, -68.2f, -71.1f, -74.4f, -77.7f, -77.7f, -77.7f, -77.7f, -77.7f, -77.7f, -77.7f },
};
// 最後に音が出た x と、最初に無音になった x の真ん中
static constexpr double VEL_SILENT_X[5] = { 3824, 3408, 3680, 3920, 3408 };

static inline double vel_effective_sens(int a)
{
	return a <= 32 ? double(a) : 1024.0 / (64 - (a > 63 ? 63 : a));
}

// 音量のベロシティ感度で下げる量（dB、0 以下。無音は SILENT）
static inline double velocity_db(int sens, int curve, int velocity)
{
	const int s = sens - 64;
	if (s == 0)
		return 0.0;
	const int a = s < 0 ? -s : s;
	const double v = s > 0 ? velocity : 128 - velocity;
	curve = curve < 0 ? 0 : curve > 4 ? 4 : curve;
	const double x = vel_effective_sens(a) * (127 - v);
	if (x > VEL_SILENT_X[curve])
		return SILENT;
	const double u = x / 128.0;
	const int i = int(u) > 31 ? 31 : int(u);
	return VEL_X_TABLE[curve][i] + (u - i) * (VEL_X_TABLE[curve][i + 1] - VEL_X_TABLE[curve][i]);
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

// 音量の値 L（0..127）の dB。測った 14 点（AEG の高さを全部 L にした録音）を L でまっすぐ補間する。
// 40 × log10(L / 127) とは上のほうで 0.6dB まで違う（実機のほうが大きい）。L = 0・1 は無音。
// エレメントの音量・レベルスケーリング・AEG の高さはどれも同じ目盛り（スケーリング −32 と音量 96 が同じ −4.5dB）
static inline double level_db(double L)
{
	static const double P[13] = { 1, 8, 16, 32, 48, 64, 80, 88, 96, 104, 112, 120, 127 };
	static const double D[13] = { -84.2, -48.17, -36.14, -24.09, -16.57, -12.06, -7.51, -6.32, -4.52, -2.88, -1.80, -0.85, 0.0 };
	if (L <= 1.0)
		return SILENT;
	if (L >= 127.0)
		return 0.0;
	int i = 0;
	while (i < 11 && L > P[i + 1])
		i++;
	return D[i] + (L - P[i]) / (P[i + 1] - P[i]) * (D[i + 1] - D[i]);
}

// 音量のレベルスケーリング（41 ee 30〜33 の区切りの鍵、34〜3B のずらし。2 バイトで 128 が 0）: 区切りの間はずらしをまっすぐ補間し、
// 外は端のずらし。ずらしは音量の値に足す（127 で頭打ち。ずらし +16〜+127 を全部に掛けても 127 のエレメントは変わらなかった）。
// 区切り 36・48・72・84 で区切り 1 だけ −64 にすると、鍵 24・30・36 が −11.9〜−12.1dB、42 が −4.6dB、48 から上は 0
static inline double level_scaling(const int bp[4], const int off[4], int key)
{
	if (key <= bp[0])
		return off[0] - 128;
	for (int i = 0; i < 3; i++)
		if (key <= bp[i + 1]) {
			const int span = bp[i + 1] - bp[i];
			const double t = span > 0 ? double(key - bp[i]) / span : 1.0;
			return off[i] - 128 + t * (off[i + 1] - off[i]);
		}
	return off[3] - 128;
}

// 段の行き先の dB。無音へ行く段は −96dB（255 目盛り）を目指してまっすぐ下がり、着いたら無音
static constexpr double FLOOR_DB = -96.0;

// 上がる段で、振幅 0 → 1 にかかる秒（初め 0 → アタック 127 の 10%〜90% の時間 / 0.8。値 24〜88 の 8 刻みで
// 0.6・2.5・3.1・7.5・19.4・50.6・105.6・232.5・448.1ms。間は対数で補間、外は端の傾き）
static inline double rise_seconds(double t)
{
	static const double p[9] = { 24, 32, 40, 48, 56, 64, 72, 80, 88 };
	static const double sec[9] = { 0.0006, 0.0025, 0.0031, 0.0075, 0.0194, 0.0506, 0.1056, 0.2325, 0.4481 };
	if (t <= 0)
		return 0.0;
	int i = 0;
	while (i < 7 && t > p[i + 1])
		i++;
	const double u = (t - p[i]) / (p[i + 1] - p[i]);
	return std::exp(std::log(sec[i]) + u * (std::log(sec[i + 1]) - std::log(sec[i])));
}

static constexpr double TIME_VEL_STEPS = 1.0;      // 時間の値のずれ =（感度 − 64）×（v − 1）/ 126（速くなる向き）
static constexpr double TIME_KF_OCT = 0.0020;      // 段の長さの倍率 = 2^(−これ ×（感度 − 64）×（鍵 − 中心））

struct amp_eg {
	int level[4] = { 0, 127, 127, 127 };   // 初め・アタック・ディケイ 1・ディケイ 2（0..127）
	int time[4] = { 0, 0, 0, 0 };          // アタック・ディケイ 1・ディケイ 2・リリース（0..127）
	int time_vel_sens = 64, velocity = 100;
	int time_vel_segment = 4;              // 41 ee 2D: 0 アタック、1 アタック + ディケイ、2 ディケイ、3 アタック + リリース、4 全部
	int time_kf = 64, time_kf_center = 60, key = 60;

	int seg = 0;                           // 0 アタック、1 ディケイ 1、2 ディケイ 2、3 そのまま、4 リリース、5 終わり
	double db = SILENT;                    // 今の dB
	double from = 0, to = 0, len = 0, left = 0;
	bool rising = false;
	double amp = 0, amp_to = 0, amp_step = 0;

	// 段 s（0 アタック、1 ディケイ 1、2 ディケイ 2、4 リリース）に時間のベロシティ感度が効くか
	bool vel_applies(int s) const
	{
		// 実機: 1 はアタックとディケイ 1 だけ（ディケイ 2 は変わらない）、2 はディケイ 1・2（mr_aeg.py の seg・seg2）
		const bool atk = s == 0, dcy = s == 1 || s == 2, rls = s == 4;
		switch (time_vel_segment) {
		case 0: return atk;
		case 1: return atk || s == 1;
		case 2: return dcy;
		case 3: return atk || rls;
		default: return true;
		}
	}
	double shifted(int t, int s) const
	{
		return t - (vel_applies(s) ? TIME_VEL_STEPS * (time_vel_sens - 64) * (velocity - 1) / 126.0 : 0.0);
	}
	double kf_factor() const { return std::exp2(-TIME_KF_OCT * (time_kf - 64) * (key - time_kf_center)); }
	static double seg_samples_at(double t)
	{
		if (t <= 0)
			return 0.0;
		t = swp50_eg::motif_time(t);
		if (t <= -20)
			return swp50_eg::MIN_TIME;
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
		len = left = t <= 0 ? 0.0 : seg_samples_at(shifted(t, s)) * kf_factor();
		rising = to > from;
		if (rising) {
			// 上がる段は振幅でまっすぐ、短い時間で着き、段の残りはそのまま待つ
			amp = std::pow(10.0, from / 20.0);
			amp_to = std::pow(10.0, to / 20.0);
			const double rs = rise_seconds(t <= 0 ? 0 : shifted(t, s)) * kf_factor() * 44100.0;
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
