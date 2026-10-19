// license:BSD-3-Clause
// エレメントの LFO の模型（swp50_lfo.h）の試験。実機の録音（2026-10-20、mr_lfo.py → mr_lfo_fit.py）の代表の点と合うこと。
//
//   g++ -std=c++17 -O1 -o swp50_lfotest src/motif/swp50_lfotest.cpp && ./swp50_lfotest

#include "swp50_lfo.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("NG  %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static bool near_ratio(double a, double b, double tol) { return std::fabs(a / b - 1.0) <= tol; }

int main()
{
	using namespace swp50_lfo;
	// 速さ: 測った値（Hz）と 3% 以内（20〜63）
	static const struct { int v; double hz; } SP[] = {
		{ 20, 1.011 }, { 24, 1.430 }, { 28, 2.019 }, { 32, 2.860 }, { 36, 4.122 }, { 40, 5.720 }, { 44, 8.159 },
		{ 48, 11.440 }, { 52, 16.318 }, { 56, 22.879 }, { 60, 32.300 }, { 63, 42.057 },
	};
	for (auto &s : SP)
		CHECK(near_ratio(speed_hz(s.v), s.hz, 0.03));
	CHECK(speed_hz(0) == 0.0);
	// 音程の深さ: 測った ±セントと 7% 以内（小さい所は 0.5 セント以内）
	static const struct { int d; double c; } PD[] = { { 16, 4.4 }, { 32, 9.9 }, { 64, 37.9 }, { 96, 147.7 }, { 127, 582.0 } };
	for (auto &p : PD)
		CHECK(near_ratio(pitch_cents(p.d), p.c, 0.07) || std::fabs(pitch_cents(p.d) - p.c) < 0.5);
	// フィルター: 山から谷のオクターブ
	CHECK(std::fabs(filter_octaves_pp(32) - 1.02) < 0.06);
	CHECK(std::fabs(filter_octaves_pp(64) - 1.93) < 0.06);
	CHECK(std::fabs(filter_octaves_pp(127) - 3.78) < 0.06);
	// ディレイ・フェード
	static const struct { int v; double sec; } DL[] = { { 8, 0.125 }, { 16, 0.354 }, { 32, 1.018 }, { 48, 1.871 }, { 64, 2.894 } };
	for (auto &x : DL)
		CHECK(near_ratio(delay_seconds(x.v), x.sec, 0.05));
	CHECK(delay_seconds(96) > 4.6);
	static const struct { int v; double sec; } FD[] = { { 16, 0.35 }, { 32, 1.15 }, { 48, 1.95 }, { 64, 2.9 }, { 80, 4.15 }, { 96, 5.5 } };
	for (auto &x : FD)
		CHECK(near_ratio(fade_seconds(x.v), x.sec, 0.1));

	// 動かしてみる: 三角・速さ 32・音程 64 で、一周の山と谷が ±37.9 セント、キーオンでそろう
	element_lfo l;
	l.wave = 1; l.speed = 32; l.pmod = 64;
	l.key_on();
	double lo = 0, hi = 0, first = l.pitch_cents_now();
	for (int i = 0; i < 44100; i++) {
		l.step(44100.0);
		const double c = l.pitch_cents_now();
		lo = std::fmin(lo, c);
		hi = std::fmax(hi, c);
	}
	CHECK(std::fabs(first) < 1e-9);
	CHECK(std::fabs(hi - 37.9) < 0.5 && std::fabs(lo + 37.9) < 0.5);
	// 音量: 深さ 64 で一番下げる所は 64
	l.amod = 64;
	double drop = 0;
	l.key_on();
	for (int i = 0; i < 44100; i++) {
		l.step(44100.0);
		drop = std::fmax(drop, l.level_drop_now());
	}
	CHECK(std::fabs(drop - 64) < 0.5);
	// ディレイ 32: 0.95 秒では揺れない、1.1 秒では揺れている
	l.delay = 32;
	l.key_on();
	for (int i = 0; i < int(0.95 * 44100); i++)
		l.step(44100.0);
	CHECK(l.amount() == 0.0);
	for (int i = 0; i < int(0.15 * 44100); i++)
		l.step(44100.0);
	CHECK(l.amount() == 1.0);
	// のこぎりは真ん中から下がり、四角は上から
	element_lfo w;
	w.wave = 0;
	w.key_on();
	CHECK(std::fabs(w.shape()) < 1e-9);
	w.step(44100.0);
	CHECK(w.shape() < 0);
	w.wave = 2;
	w.key_on();
	CHECK(w.shape() == 1.0);

	if (g_fail) {
		std::printf("swp50_lfotest: %d NG\n", g_fail);
		return 1;
	}
	std::printf("swp50_lfotest: OK\n");
	return 0;
}
