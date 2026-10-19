// license:BSD-3-Clause
// MOTIF-RACK ES のエレメントのピッチ EG（PEG）と音程の感度（2026-10-20。tools/motif/mr_peg.py の録音を mr_peg_fit.py で読んだ。正弦波の波形 704・鍵 60）。
// アドレスは 42 ee 00〜1D（mr_params.py）。
//
//   PEG の目盛り: 音程のずれ = 0.5282 セント × 高さ × 深さ（高さ −128..+127、深さ −64..+63。+127 は 128、+63 は 64 として効く）。
//             深さ +8・+16・+32・+63 × 高さ ±16・±64・+127 の 20 点が 0.5% 以内（+32・+64 で 1081.7、+63・+127 で 4329 セント）
//   段の動き: FEG と同じ形（swp50_filter::filter_eg をそのまま使う）。高さの単位でまっすぐ（セントでまっすぐ）動き、段の長さは
//             swp50_eg::time_down(127 − 値)（アタックの値 64・80 で 0.83・3.28 秒。値 48 は 0.13 秒で、表の 0.20 秒より短い）。
//             ホールドは FEG のホールドの表に近い（値 32・64 で 0.09・0.40 秒、FEG は 0.104・0.453）。リリースも同じ速さで動く
//   高さのベロシティ感度（42 ee 17）・カーブ（18）: FEG の倍率の表（feg_level_velocity_factor）とほぼ同じ。カーブ 0・ベロシティ 1 / 64 / 127 で
//             感度 +16 が 0.69 / 0.83 / 1.06、+32 が 0.61 / 0.71 / 1.12、+63 が 0 / 0.62 / 1.25（FEG の表は 127 の所が 0.08 ほど小さい）
//   音程のキーフォロー（42 ee 19、中心 1A）: 96 がふつう（100%）。値 48・80・112・127 で −40・40・160・200%（64 を中心に対称、間は補間）
//   音程のベロシティ感度（42 ee 02）: 0.1475 セント ×（感度 − 64）×（v − 1）。+16・+32・+63 で v = 127 が 296・595・1190 セント（+63 は 64 として）
#pragma once

#include <cmath>

namespace swp50_peg {

static constexpr double CENTS_PER_UNIT = 0.5282;

// PEG の今の高さ value（−128..+127）と深さ（0..127、64 が 0）から、音程のずれ（セント）
static inline double cents(double value, int depth)
{
	const int d = depth - 64;
	const double dd = d >= 63 ? 64.0 : double(d);
	const double v = value >= 127 ? 128.0 : value;
	return CENTS_PER_UNIT * v * dd;
}

// 音程のキーフォローの割合（1.0 = ふつう）
static inline double key_follow_ratio(int v)
{
	static const double P[5] = { 0, 16, 32, 48, 63 }, R[5] = { 0.0, 0.40, 1.00, 1.60, 2.00 };
	const int s = v - 64;
	const int a = s < 0 ? -s : s;
	double r = R[4];
	for (int i = 0; i < 4; i++)
		if (a <= P[i + 1]) {
			r = R[i] + (a - P[i]) / (P[i + 1] - P[i]) * (R[i + 1] - R[i]);
			break;
		}
	return s < 0 ? -r : r;
}

// 音程のベロシティ感度（セント）
static inline double velocity_cents(int sens, int velocity)
{
	const int s = sens - 64;
	const double ss = s >= 63 ? 64.0 : double(s);
	return 0.1475 * ss * (velocity - 1);
}

} // namespace swp50_peg
