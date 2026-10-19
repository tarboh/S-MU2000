// license:BSD-3-Clause
// PEG と音程の感度（swp50_peg.h）の試験。実機の録音（2026-10-20、mr_peg.py → mr_peg_fit.py）の代表の点と合うこと。
//
//   g++ -std=c++17 -O1 -o swp50_pegtest src/motif/swp50_pegtest.cpp && ./swp50_pegtest

#include "swp50_filter.h"
#include "swp50_peg.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("NG  %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

int main()
{
	using namespace swp50_peg;
	// 目盛り（深さ / 高さ → セント）
	static const struct { int depth, level; double c; } SC[] = {
		{ 72, -64, -269.6 }, { 72, 16, 66.6 }, { 80, 64, 540.2 }, { 96, -16, -269.6 }, { 96, 64, 1081.7 }, { 96, 127, 2164.6 },
		{ 127, 64, 2163.3 }, { 127, 127, 4329.0 }, { 127, -64, -2163.7 },
	};
	for (auto &s : SC)
		CHECK(std::fabs(cents(s.level, s.depth) / s.c - 1) < 0.006 || std::fabs(cents(s.level, s.depth) - s.c) < 1.5);
	// キーフォロー（鍵 48 / 72 の、鍵 60 からのセント）
	CHECK(std::fabs(key_follow_ratio(96) - 1.0) < 1e-9);
	CHECK(std::fabs(key_follow_ratio(80) * 1200 - 480.4) < 5);
	CHECK(std::fabs(key_follow_ratio(48) * 1200 - -480.4) < 5);
	CHECK(std::fabs(key_follow_ratio(112) * 1200 - 1919.6) < 5);
	CHECK(std::fabs(key_follow_ratio(127) * 1200 - 2400.0) < 5);
	// ベロシティ感度
	CHECK(std::fabs(velocity_cents(80, 127) - 296.4) < 5);
	CHECK(std::fabs(velocity_cents(96, 64) - 300.2) < 5);
	CHECK(std::fabs(velocity_cents(127, 127) - 1189.6) < 5);
	CHECK(std::fabs(velocity_cents(80, 1)) < 3);

	// 段: 深さ +32、ホールドの高さ +64 → アタック 0（時間 64）。0.83 秒ほどでまっすぐ 1082 → 0 セント
	{
		swp50_filter::filter_eg e;
		e.depth = 96;
		e.level[0] = 64;
		e.time[1] = 64;
		e.key_on();
		double at02 = 0, at04 = 0;
		long end = -1;
		for (long n = 0; n < 44100 * 2; n++) {
			e.step();
			const double c = cents(e.value, e.depth);
			if (n == long(0.2 * 44100))
				at02 = c;
			if (n == long(0.4 * 44100))
				at04 = c;
			if (end < 0 && c < 1.0)
				end = n;
		}
		CHECK(std::fabs(end / 44100.0 - 0.83) < 0.06);
		CHECK(std::fabs(at02 - 1082 * (1 - 0.2 / 0.81)) < 40);      // 測った 0.2 秒の所は 781〜846 セント
		CHECK(std::fabs(at04 - 1082 * (1 - 0.4 / 0.81)) < 40);
	}

	if (g_fail) {
		std::printf("swp50_pegtest: %d NG\n", g_fail);
		return 1;
	}
	std::printf("swp50_pegtest: OK\n");
	return 0;
}
