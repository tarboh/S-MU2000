// license:BSD-3-Clause
// エレメントの EQ の模型（swp50_eq.h）の試験。実機の録音（2026-10-20、mr_eq.py）の代表の点と合うこと。
//
//   g++ -std=c++17 -O1 -o swp50_eqtest src/motif/swp50_eqtest.cpp && ./swp50_eqtest

#include "swp50_eq.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("NG  %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

// インパルス応答の、周波数 f での高さ（dB）
static double response(swp50_eq::element_eq &eq, double f)
{
	eq.reset();
	double re = 0, im = 0;
	for (int i = 0; i < (1 << 16); i++) {
		const double y = eq.process(i == 0 ? 1.0 : 0.0);
		re += y * std::cos(2 * swp50_eq::PI * f * i / 44100.0);
		im -= y * std::sin(2 * swp50_eq::PI * f * i / 44100.0);
	}
	return 10 * std::log10(re * re + im * im);
}

int main()
{
	using namespace swp50_eq;
	element_eq eq;

	// L/H: EQ1 の値 110・ゲイン +12（= +4.5dB）。低域は +4.5dB、高域は 0dB（実機 +4.54 / +0.05）
	eq.set(LH, 16, 110, 0x40 + 12, 200, 0x40);
	CHECK(std::fabs(response(eq, 30) - 4.5) < 0.1);
	CHECK(std::fabs(response(eq, 15000)) < 0.1);
	// 棚の半分の点（実機 761Hz）
	CHECK(std::fabs(response(eq, 761) - 2.25) < 0.3);

	// L/H のゲイン: -32 で低域 -12dB（実機 -11.94）
	eq.set(LH, 16, 110, 0x40 - 32, 200, 0x40);
	CHECK(std::fabs(response(eq, 30) - (-12.0)) < 0.15);

	// EQ2: 値 163 で高域 +4.5dB、半分の点は実機 1863Hz
	eq.set(LH, 16, 100, 0x40, 163, 0x40 + 12);
	CHECK(std::fabs(response(eq, 16000) - 4.5) < 0.15);
	CHECK(std::fabs(response(eq, 1863) - 2.25) < 0.3);

	// P.EQ: 値 163・レゾナンス 16 で 1244Hz に +4.5dB の山（実機 +4.56）、レゾナンス 31 で幅が狭い
	eq.set(PEQ, 16, 163, 0x40 + 12, 0, 0x40);
	CHECK(std::fabs(response(eq, 1244) - 4.5) < 0.15);
	CHECK(std::fabs(response(eq, 1244 * 2) - response(eq, 1244)) > 2.0);
	eq.set(PEQ, 31, 163, 0x40 + 12, 0, 0x40);
	const double narrow = response(eq, 1244 * 1.2);
	eq.set(PEQ, 0, 163, 0x40 + 12, 0, 0x40);
	CHECK(response(eq, 1244 * 1.2) > narrow);

	// Boost: どの周波数でも同じ（実機 +6.0 / +12.1 / +18.1dB）
	eq.set(BOOST12, 0, 0, 0x40, 0, 0x40);
	CHECK(std::fabs(response(eq, 100) - 12.1) < 0.01 && std::fabs(response(eq, 10000) - 12.1) < 0.01);

	// THRU は何もしない
	eq.set(THRU, 0, 0, 0x40, 0, 0x40);
	CHECK(std::fabs(response(eq, 1000)) < 1e-9);

	// 周波数の表は増え続ける
	for (double v = 46; v < 182; v += 1)
		CHECK(interp(EQ1_LOW, v + 1) > interp(EQ1_LOW, v));

	std::printf("swp50_eqtest: %s\n", g_fail ? "NG" : "OK");
	return g_fail ? 1 : 0;
}
