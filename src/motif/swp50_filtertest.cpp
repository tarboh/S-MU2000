// license:BSD-3-Clause
// エレメントのフィルターの模型（swp50_filter.h）の試験。
// 1. 音を通す処理（element_filter::process）のインパルス応答の周波数特性が、式（response_db）と一致すること
// 2. 実機の録音（2026-10-19、レゾナンス 0）の代表の点と合うこと
//
//   g++ -std=c++17 -O1 -o swp50_filtertest src/motif/swp50_filtertest.cpp && ./swp50_filtertest

#include "swp50_filter.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("NG  %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static double impulse_db(int type, int cutoff, double f, int reso = 0)
{
	swp50_filter::element_filter flt;
	flt.set(type, cutoff, reso);
	flt.reset();
	double re = 0, im = 0;
	const int n = 1 << 18;
	for (int i = 0; i < n; i++) {
		const double y = flt.process(i == 0 ? 1.0f : 0.0f);
		re += y * std::cos(2 * swp50_filter::PI * f * i / 44100.0);
		im -= y * std::sin(2 * swp50_filter::PI * f * i / 44100.0);
	}
	return 10 * std::log10(re * re + im * im + 1e-30);
}

int main()
{
	using namespace swp50_filter;

	// 1. 処理と式の一致（0.05dB 以内。-80dB より下は見ない）
	const int types[] = { LPF24D, LPF24A, LPF18, LPF18S, LPF12, LPF6, HPF24D, HPF12, BPF12D, BPFW, BPF6, BEF12, BEF6,
	                      DUAL_LPF, DUAL_HPF, DUAL_BPF, DUAL_BEF, LPF12_BPF6 };
	for (int t : types)
		for (int c : { 100, 160, 200 })
			for (double f : { 60.0, 250.0, 1000.0, 4000.0, 12000.0 }) {
				const double a = impulse_db(t, c, f), b = response_db(t, c, f);
				if (b > -80 && std::fabs(a - b) > 0.05) {
					std::printf("type %d cutoff %d f %.0f: process %.2f dB, response %.2f dB\n", t, c, f, a, b);
					g_fail++;
				}
			}

	// 1b. レゾナンスを入れても処理と式が一致すること
	for (int t : types)
		for (int r : { 32, 64 })
			for (double f : { 250.0, 458.4, 1000.0 }) {
				const double a = impulse_db(t, 128, f, r), b = response_db(t, 128, f, r);
				if (b > -80 && std::fabs(a - b) > 0.05) {
					std::printf("type %d reso %d f %.0f: process %.2f dB, response %.2f dB\n", t, r, f, a, b);
					g_fail++;
				}
			}

	// 2b. レゾナンスの実機の値（目盛り 128、f0 = 458Hz での高さ。mr_reso.py の録音）
	CHECK(std::fabs(response_db(LPF12, 128, 458.4, 48) - 8.98) < 0.8);
	CHECK(std::fabs(response_db(LPF12, 128, 458.4, 64) - 14.94) < 0.8);
	CHECK(std::fabs(response_db(BPF6, 128, 458.4, 64) - 14.87) < 0.8);
	CHECK(std::fabs(response_db(LPF24A, 128, 458.4, 64) - 14.97) < 3.0);  // LPF24A は山の頂上が 2.7dB 低く出る（全体の RMS は 0.8dB）
	CHECK(std::fabs(response_db(LPF18, 128, 458.4, 64) - 12.07) < 1.0);
	CHECK(std::fabs(response_db(LPF24D, 128, 458.4, 32) - 5.85) < 1.0);
	CHECK(std::fabs(type_q(LPF12, 0) - 2.0f) < 1e-6 && std::fabs(type_q(LPF12, 40) - 0.4844f) < 0.001f);   // 16 ごとに半分
	CHECK(type_q(BPFW, 127) > type_q(BPFW, 0));                            // BPFw は広がる

	// 2. 実機の録音の代表の点（目盛り 128 = 共通のカットオフ周波数 458Hz）
	CHECK(std::fabs(response_db(LPF24D, 128, 458.4) - (-11.9)) < 0.7);     // 測定 -11.9dB
	CHECK(std::fabs(response_db(BPF6, 128, 458.4) - (-6.0)) < 0.5);        // 測定 -6.0dB（山の頂）
	CHECK(std::fabs(response_db(HPF12, 128, 458.4) - (-6.1)) < 0.5);       // 測定 -6.1dB
	CHECK(std::fabs(response_db(BPF12D, 128, 458.4) - (-12.0)) < 0.5);     // 測定 -12.0dB
	CHECK(std::fabs(response_db(DUAL_HPF, 128, 15000.0) - 6.0) < 0.6);     // 高域で +6dB
	CHECK(response_db(BEF12, 128, 458.4) < -40);                           // 谷
	CHECK(std::fabs(response_db(LPF6, 128, 50.0)) < 0.1);                  // 通る所は 0dB

	// キーフォロー: 鍵 60 では動かない。100%（96）で鍵 +12 は約 +25.6 目盛り、−200%（0）で鍵 −24 は +102 目盛り
	CHECK(std::fabs(cutoff_with_keyfollow(128, 96, 60) - 128) < 1e-9);
	CHECK(std::fabs(cutoff_with_keyfollow(128, 96, 72) - (128 + 25.56)) < 0.01);
	CHECK(std::fabs(cutoff_with_keyfollow(128, 0, 36) - (128 + 102.24)) < 0.01);
	CHECK(cutoff_with_keyfollow(250, 127, 108) == 255 && cutoff_with_keyfollow(10, 127, 0) == 0);
	{
		// 実機: BEF12・目盛り 128・キーフォロー 96 で、鍵 72 の谷は 917Hz（鍵 60 は 459Hz）。g の比は周波数の比とほぼ同じ
		const double ratio = g_at(cutoff_with_keyfollow(128, 96, 72)) / g_at(128.0);
		CHECK(std::fabs(ratio - 916.9 / 459.2) < 0.12);
	}
	for (double c = 0; c < 254.9; c += 0.25)
		CHECK(g_at(c + 0.25) >= g_at(c));

	// HPF: LPF6・カットオフ 255 のとき、HPF 128 で f0/2 は -11.1dB、f0 は -0.1dB（実機）。LPF24D では効かない
	CHECK(std::fabs(response_db(LPF6, 255, 229.2, 0, 128) - response_db(LPF6, 255, 229.2) - (-11.1)) < 1.0);
	CHECK(std::fabs(response_db(LPF6, 255, 458.4, 0, 128) - response_db(LPF6, 255, 458.4) - (-0.1)) < 1.0);
	CHECK(std::fabs(response_db(LPF24D, 255, 229.2, 0, 128) - response_db(LPF24D, 255, 229.2)) < 1e-9);
	{
		// 処理でも同じ（LPF12・カットオフ 200・HPF 100）
		swp50_filter::element_filter flt;
		flt.set(LPF12, 200, 0, 64, 60, 100);
		flt.reset();
		double re = 0, im = 0;
		for (int i = 0; i < (1 << 18); i++) {
			const double y = flt.process(i == 0 ? 1.0f : 0.0f);
			re += y * std::cos(2 * PI * 150.0 * i / 44100.0);
			im -= y * std::sin(2 * PI * 150.0 * i / 44100.0);
		}
		CHECK(std::fabs(10 * std::log10(re * re + im * im) - response_db(LPF12, 200, 150.0, 0, 100)) < 0.05);
	}

	// 欠番は次の値になる
	CHECK(canonical_type(9) == BPFW && canonical_type(18) == LPF12_BPF6 && canonical_type(20) == THRU);

	// g の表は増え続ける（230 から上は頭打ち）
	for (int c = 0; c < 230; c++)
		CHECK(g_table[c + 1] >= g_table[c]);

	std::printf("swp50_filtertest: %s\n", g_fail ? "NG" : "OK");
	return g_fail ? 1 : 0;
}
