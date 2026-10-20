// license:BSD-3-Clause
// SWP50 の音量の EG の模型。doc/motif-rack-es-capture.md の「基準の録音の結果」と「録音と Firm の表の突き合わせ」。
//
// 実機（MOTIF-RACK ES）のデジタル出力を録って合わせたもので、チップの中を読んだものではない。
// 合っていると確かめた範囲:
//   * 高さは「減らす量」の 8bit。0x10 = 6.02dB、0 = いちばん大きい、0xFF = 無音
//   * 下がる側は dB でまっすぐ。速さ r（7bit）の 1 サンプルあたりの進みは (8 + (r & 7)) << (r >> 3)。
//     高さは 26bit の数え（上位 8bit が高さ）で持ち、0 から 0x3FFF000 まで進むのにかかる時間が
//     本体プログラムの表（8C0DDC08）の値と一致する（8388096 × 8 = 0x3FFF000）。録音とは 1% 以内で合う
//   * 上がる側は振幅でほぼまっすぐ。かかる時間は表（8C0DDA08）と合うが、表の値は上の式からは ±2.5% ずれる
//     （m = r & 7 ごとに決まった癖がある）。ここでは式で近似していて、そのぶんの誤差がある
//   * 本体プログラムは「時間のパラメーター → 表の時間 t → t × 255 ÷ 高さの差 → それに当たる速さの番号」と選ぶので、
//     高さの差が違っても、かかる時間はほぼ同じになる
// 確かめていないもの: 速さ 0 の扱い（表では下がる側だけ「終わらない」になっている）、いちばん速い側の頭打ち（表は 129 サンプル）。

#ifndef SMU2000_MOTIF_SWP50_EG_H
#define SMU2000_MOTIF_SWP50_EG_H

#include <algorithm>
#include <cmath>
#include <cstdint>

struct swp50_eg
{
	static constexpr uint32_t FULL = 0x3fff000;      // 高さの数えの端（255 目盛りぶん）
	// 上がる側: 0 から全開までのサンプル数 × 進み。表（8C0DDA08）の値から出した、r & 7 ごとの定数。
	// 速い側（r が 40 を超えるあたり）では表のほうが数 % 短い
	static double up_scale(uint32_t rate)
	{
		static const double k[8] = { 6.985e6, 7.094e6, 7.186e6, 7.265e6, 7.335e6, 7.218e6, 7.124e6, 7.047e6 };
		return k[rate & 7];
	}
	static constexpr double   MIN_TIME = 129.0;      // いちばん速い側の頭打ち（表の値）

	static uint32_t increment(uint32_t rate) { return (8u + (rate & 7)) << (rate >> 3); }

	// 255 目盛りぶん動くのにかかるサンプル数
	static double time_down(uint32_t rate) { return rate ? std::max(MIN_TIME, double(FULL) / increment(rate)) : INFINITY; }
	static double time_up(uint32_t rate) { return std::max(MIN_TIME, up_scale(rate) / increment(rate)); }

	// MOTIF-RACK ES の時間の値（0..127、端数あり）を、time_down(127 − 値) で引くときの値に直す。56 から上はそのまま。
	// それより下は実機のほうが速い: AEG のディケイ 1（127 → 64）が値 48・40・32 で 0.135・0.055・0.028 秒（表なら 0.198・0.099・0.050）、
	// PEG の値 48 も 0.13 秒（2026-10-20、tools/motif/mr_aeg.py の table）。48 で −4.4、40 から下で −6.8 目盛り、間はまっすぐ
	static double motif_time(double t)
	{
		if (t >= 56)
			return t;
		if (t >= 48)
			return t - 4.4 * (56 - t) / 8.0;
		if (t >= 40)
			return t - 4.4 - 2.4 * (48 - t) / 8.0;
		return t - 6.8;
	}

	static double gain_of(double atten) { return atten >= 255.0 ? 0.0 : std::pow(2.0, -atten / 16.0); }

	// 本体プログラムの選び方: 時間 t（サンプル）で差 delta（目盛り）を動かしたいときの速さの番号
	static uint32_t rate_for(double t, double delta, bool up)
	{
		if (delta <= 0)
			return 127;
		const double want = t * 255.0 / delta;
		uint32_t best = 1;
		double err = INFINITY;
		for (uint32_t r = 1; r < 128; r++) {
			const double e = std::fabs((up ? time_up(r) : time_down(r)) - want);
			if (e < err) {
				err = e;
				best = r;
			}
		}
		return best;
	}

	// ---- 状態 ----
	double   atten = 255.0;        // 今の高さ（減らす量、端数つき）
	double   target = 255.0;
	uint32_t rate = 0;
	bool     rising = false;       // 振幅でまっすぐ上がっている途中
	double   lin = 0.0, lin_step = 0.0, lin_target = 0.0;

	void set(double from_atten) { atten = from_atten; rising = false; }

	void go(double target_atten, uint32_t r)
	{
		target = target_atten;
		rate = r;
		if (target < atten) {
			rising = true;
			lin = gain_of(atten);
			lin_target = gain_of(target);
			lin_step = 1.0 / time_up(r);         // 全開ぶん（0 → 1）を time_up で上がる傾き
		} else {
			rising = false;
		}
	}

	// 1 サンプル進めて、今の倍率を返す
	double step()
	{
		if (rising) {
			lin += lin_step;
			if (lin >= lin_target) {
				lin = lin_target;
				rising = false;
				atten = target;
			}
			return lin;
		}
		if (atten < target) {
			atten += 255.0 / time_down(rate);
			if (atten > target)
				atten = target;
		}
		return gain_of(atten);
	}
};

#endif
