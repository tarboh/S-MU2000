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
	// 目盛り（深さ / 高さ → セント。2026-10-24 に高さのベロシティ感度 0 で測り直した値、tools/motif/mr_peg_amount.py）
	static const struct { int depth, level; double c; } SC[] = {
		{ 74, 64, 374.6 }, { 84, 16, 181.9 }, { 84, 102, 1189.9 }, { 84, -102, -1199.0 }, { 96, 64, 1197.0 }, { 96, 127, 2397.0 },
		{ 127, 64, 2397.0 }, { 127, 127, 4796.5 }, { 127, -64, -2400.7 },
	};
	for (auto &s : SC)
		CHECK(std::fabs(cents(s.level, s.depth) / s.c - 1) < 0.006 || std::fabs(cents(s.level, s.depth) - s.c) < 6.0);   // 深さ +20・高さ 16 は 187.5 対 181.9
	// キーフォロー（鍵 48 / 72 の、鍵 60 からのセント）
	CHECK(std::fabs(key_follow_ratio(96) - 1.0) < 1e-9);
	CHECK(std::fabs(key_follow_ratio(80) * 1200 - 480.4) < 5);
	CHECK(std::fabs(key_follow_ratio(48) * 1200 - -480.4) < 5);
	CHECK(std::fabs(key_follow_ratio(112) * 1200 - 1919.6) < 5);
	CHECK(std::fabs(key_follow_ratio(127) * 1200 - 2400.0) < 5);
	CHECK(std::fabs(key_follow_ratio(90) - 0.90) < 1e-9);     // 2026-10-28 の細かい測り（mr_pitch_kf.py）
	CHECK(std::fabs(key_follow_ratio(74) - 0.10) < 1e-9);
	CHECK(std::fabs(key_follow_ratio(32) + 1.00) < 1e-9);
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
		// 測った 0.2 秒の所は 781〜846 セント（その録音は高さのベロシティ感度が残っていて 0.90 倍。いまの式の総量は 1197）
		CHECK(std::fabs(at02 - 1197 * (1 - 0.2 / 0.81)) < 45);
		CHECK(std::fabs(at04 - 1197 * (1 - 0.4 / 0.81)) < 45);
	}

	if (g_fail) {
		std::printf("swp50_pegtest: %d NG\n", g_fail);
		return 1;
	}
	std::printf("swp50_pegtest: OK\n");
	return 0;
}
