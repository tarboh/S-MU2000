// license:BSD-3-Clause
// エレメントの音量の EG の模型（swp50_aeg.h）の試験。実機の録音（2026-10-20、mr_aeg.py → mr_aeg_fit.py）の代表の点と合うこと。
//
//   g++ -std=c++17 -O1 -o swp50_aegtest src/motif/swp50_aegtest.cpp && ./swp50_aegtest

#include "swp50_aeg.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("NG  %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

using namespace swp50_aeg;

static double kf_db(int s, int k) { return 40.0 * std::log10(key_follow_factor(s, k)); }

// EG を回して、ディケイ 1 の段（level[1] → level[2]）の長さ（秒）を、dB の直線の 20%〜80% から出す
static double decay1_seconds(amp_eg e)
{
	e.key_on();
	const double top = level_db(e.level[1]), tail = level_db(e.level[2]);
	const double hi = top - (top - tail) * 0.2, lo = top - (top - tail) * 0.8;
	long i_hi = -1, i_lo = -1;
	for (long n = 0; n < 44100L * 12; n++) {
		const double g = e.step();
		const double d = g > 0 ? 20 * std::log10(g) : -999;
		if (i_hi < 0 && d < hi)
			i_hi = n;
		if (i_lo < 0 && d < lo) {
			i_lo = n;
			break;
		}
	}
	return (i_lo - i_hi) / 44100.0 / 0.6;
}

static amp_eg decay_eg(int d1_time, int d1_level)
{
	amp_eg e;
	e.level[0] = e.level[1] = 127;
	e.level[2] = e.level[3] = d1_level;
	e.time[0] = 0;
	e.time[1] = d1_time;
	return e;
}

int main()
{
	// 高さ
	static const struct { int L; double db; } LV[] = { { 120, -0.85 }, { 96, -4.52 }, { 64, -12.06 }, { 32, -24.09 }, { 16, -36.14 }, { 8, -48.17 } };
	for (auto &l : LV)
		CHECK(std::fabs(level_db(l.L) - l.db) < 0.05);
	CHECK(std::fabs(level_db(100) - -3.7) < 0.3);
	// レベルスケーリング（区切り 36・48・72・84、区切り 1 だけ −64）
	{
		const int bp[4] = { 36, 48, 72, 84 }, off[4] = { 64, 128, 128, 128 };
		CHECK(level_scaling(bp, off, 24) == -64 && level_scaling(bp, off, 36) == -64);
		CHECK(std::fabs(level_db(127 + level_scaling(bp, off, 42)) - -4.56) < 0.6);
		CHECK(level_scaling(bp, off, 60) == 0 && level_scaling(bp, off, 96) == 0);
	}
	CHECK(level_db(1) == SILENT && level_db(0) == SILENT);

	// ベロシティ感度
	CHECK(std::fabs(velocity_db(80, 0, 1) - -29.6) < 1.5);
	CHECK(std::fabs(velocity_db(80, 0, 64) - -14.6) < 1.0);
	CHECK(std::fabs(velocity_db(96, 0, 64) - -29.6) < 1.0);
	CHECK(std::fabs(velocity_db(112, 0, 104) - -21.7) < 1.5);
	CHECK(std::fabs(velocity_db(120, 0, 112) - -28.6) < 2.0);
	CHECK(std::fabs(velocity_db(124, 0, 120) - -27.3) < 2.0);
	CHECK(velocity_db(96, 0, 1) == SILENT);
	CHECK(velocity_db(112, 0, 64) == SILENT);
	CHECK(velocity_db(127, 0, 120) == SILENT);
	CHECK(std::fabs(velocity_db(48, 0, 127) - -29.6) < 1.5);      // 負の感度は裏返し
	CHECK(std::fabs(velocity_db(16, 0, 32) - -29.0) < 1.5);
	CHECK(velocity_db(64, 0, 1) == 0.0);
	CHECK(std::fabs(velocity_db(96, 3, 64) - -12.1) < 0.6);
	CHECK(std::fabs(velocity_db(96, 1, 64) - -47.1) < 0.6);
	// カーブ 1〜4 はほかの感度でも x だけで決まる
	CHECK(std::fabs(velocity_db(80, 2, 1) - -29.1) < 1.0);
	CHECK(std::fabs(velocity_db(88, 2, 64) - -18.3) < 1.0);
	CHECK(std::fabs(velocity_db(72, 3, 1) - -4.5) < 1.0);
	CHECK(std::fabs(velocity_db(112, 3, 96) - -11.5) < 1.5);
	CHECK(std::fabs(velocity_db(80, 4, 1) - -29.6) < 2.0);
	CHECK(std::fabs(velocity_db(88, 1, 1) - -71.2) < 1.5);
	CHECK(velocity_db(96, 1, 16) == SILENT);

	// 音量のキーフォロー
	CHECK(std::fabs(kf_db(96, 36) - -3.33) < 0.4);
	CHECK(std::fabs(kf_db(127, 36) - -7.86) < 0.6);
	CHECK(std::fabs(kf_db(32, 84) - -3.27) < 0.4);
	CHECK(std::fabs(kf_db(0, 84) - -7.84) < 0.6);
	CHECK(std::fabs(kf_db(127, 84)) < 1e-9 && std::fabs(kf_db(0, 36)) < 1e-9);

	// 段の長さ: ディケイ 1 = 64 で 127 → 64・96・32・8 がどれも 0.78〜0.79 秒
	for (int L : { 64, 96, 32, 8 })
		CHECK(std::fabs(decay1_seconds(decay_eg(64, L)) - 0.79) < 0.03);
	CHECK(std::fabs(decay1_seconds(decay_eg(80, 64)) - 3.17) < 0.1);
	// 速い側（値 56 より下）は表より速い: 32・40・48・56 で 0.028・0.055・0.135・0.40 秒（5ms ごとの音量から読んだ）
	CHECK(std::fabs(decay1_seconds(decay_eg(56, 64)) / 0.40 - 1) < 0.1);
	CHECK(std::fabs(decay1_seconds(decay_eg(48, 64)) / 0.135 - 1) < 0.15);
	CHECK(std::fabs(decay1_seconds(decay_eg(40, 64)) / 0.055 - 1) < 0.2);
	CHECK(std::fabs(decay1_seconds(decay_eg(32, 64)) / 0.028 - 1) < 0.25);

	// 時間のベロシティ感度（ディケイ 1 = 80）
	static const struct { int s, v; double sec; } TV[] = {
		{ 72, 64, 2.171 }, { 80, 32, 2.112 }, { 80, 127, 0.867 }, { 88, 64, 1.097 }, { 96, 64, 0.797 }, { 96, 96, 0.401 }, { 112, 32, 1.066 },
	};
	for (auto &t : TV) {
		amp_eg e = decay_eg(80, 64);
		e.time_vel_sens = t.s;
		e.velocity = t.v;
		CHECK(std::fabs(decay1_seconds(e) / t.sec - 1) < 0.1);
	}
	// 時間のキーフォロー（ディケイ 1 = 80、中心 60）。ディケイ 1 = 64 で測った分（0.211・0.060 秒）は、アタックの平らな所との境を
	// 読み違えたらしく、ここでは使わない
	static const struct { int base, s, key; double sec; } TK[] = {
		{ 80, 80, 84, 1.839 }, { 80, 96, 72, 1.864 }, { 80, 96, 84, 1.077 }, { 80, 127, 72, 1.085 }, { 80, 127, 84, 0.388 },
	};
	for (auto &t : TK) {
		amp_eg e = decay_eg(t.base, 64);
		e.time_kf = t.s;
		e.key = t.key;
		CHECK(std::fabs(decay1_seconds(e) / t.sec - 1) < 0.05);
	}
	// 遅い段（127 → 0、傾きから出した長さ）: ディケイ 1 = 88・96・104、中心 60 と 24
	static const struct { int base, s, c, key; double sec; } TS[] = {
		{ 88, 96, 60, 84, 2.150 }, { 88, 96, 60, 96, 1.307 }, { 88, 127, 60, 84, 0.789 }, { 88, 96, 24, 60, 1.338 }, { 88, 96, 24, 36, 3.653 },
		{ 96, 96, 60, 84, 4.335 }, { 96, 127, 60, 84, 1.571 }, { 104, 96, 60, 84, 8.641 }, { 104, 96, 24, 60, 5.276 },
		{ 88, 96, 60, 36, 17.23 }, { 96, 96, 60, 36, 33.77 },
	};
	for (auto &t : TS) {
		amp_eg e;
		e.time_kf = t.s;
		e.time_kf_center = t.c;
		e.key = t.key;
		const double want = amp_eg::seg_samples_at(t.base) / 44100.0;
		CHECK(std::fabs(want * e.kf_factor() / t.sec - 1) < 0.12);
	}
	// リリースのキーフォローは弱い（感度 127・中心 48: 鍵 36・60・84 で 1.25・0.77・0.47 倍）
	{
		amp_eg e;
		e.time_kf = 127;
		e.time_kf_center = 48;
		static const struct { int key; double r; } RS[] = { { 36, 1.246 }, { 60, 0.774 }, { 84, 0.472 } };
		for (auto &t : RS) {
			e.key = t.key;
			CHECK(std::fabs(e.kf_factor(4) / t.r - 1) < 0.06);
		}
	}

	// 効く段: 1 はディケイ 2 に効かず、2・4 は効く（ディケイ 2 = 80 で 127 → 64、感度 +32・v = 127 で 3.13 / 0.13 / 0.14 秒）
	for (int sg : { 1, 2, 4 }) {
		amp_eg e;
		e.level[0] = e.level[1] = e.level[2] = 127;
		e.level[3] = 64;
		e.time[2] = 80;
		e.time_vel_sens = 96;
		e.velocity = 127;
		e.time_vel_segment = sg;
		e.key_on();
		const double want = sg == 1 ? 3.13 : 0.135;
		long i_hi = -1, i_lo = -1;
		const double hi = -12.06 * 0.2, lo = -12.06 * 0.8;
		for (long n = 0; n < 44100L * 6; n++) {
			const double g = e.step();
			const double d = g > 0 ? 20 * std::log10(g) : -999;
			if (i_hi < 0 && d < hi)
				i_hi = n;
			if (i_lo < 0 && d < lo) {
				i_lo = n;
				break;
			}
		}
		CHECK(std::fabs((i_lo - i_hi) / 44100.0 / 0.6 / want - 1) < 0.25);
	}
	// 無音への段: ディケイ 1 = 64 で 127 → 0 は、0.79 秒で −96dB に着いてそのあと無音
	{
		amp_eg e = decay_eg(64, 0);
		e.key_on();
		double g = 1;
		for (int n = 0; n < int(0.75 * 44100); n++)
			g = e.step();
		CHECK(g > 0 && 20 * std::log10(g) < -80);
		for (int n = 0; n < int(0.1 * 44100); n++)
			g = e.step();
		CHECK(g == 0.0);
	}
	// アタック: 初め 0 → 127、値 80 で振幅 90% が 271ms ごろ・50% が 150ms ごろ（2026-10-24 に capture/aeg/at2_80.wav を読み直した）
	{
		amp_eg e;
		e.level[0] = 0;
		e.time[0] = 80;
		e.key_on();
		long i5 = -1, i9 = -1;
		for (long n = 0; n < 44100; n++) {
			const double g = e.step();
			if (i5 < 0 && g >= 0.5)
				i5 = n;
			if (i9 < 0 && g >= 0.9)
				i9 = n;
		}
		CHECK(std::fabs(i5 / 44.1 - 150) < 25);
		CHECK(std::fabs(i9 / 44.1 - 271) < 25);
	}
	// アタックの高さが初めと同じなら、アタックの段は下がる側の長さ（値 64 で 0.79 秒）だけ続く
	{
		amp_eg e = decay_eg(64, 64);
		e.time[0] = 64;
		e.key_on();
		long start = -1;
		for (long n = 0; n < 44100 * 2; n++) {
			const double g = e.step();
			if (start < 0 && g < std::pow(10.0, -0.5 / 20)) {
				start = n;
				break;
			}
		}
		CHECK(std::fabs(start / 44100.0 - 0.79) < 0.1);
	}
	// リリース
	{
		amp_eg e = decay_eg(0, 127);
		e.time[3] = 64;
		e.key_on();
		for (int n = 0; n < 4410; n++)
			e.step();
		e.key_off();
		double g = 1;
		for (int n = 0; n < int(0.85 * 44100); n++)
			g = e.step();
		CHECK(g == 0.0 && e.finished());
	}

	if (g_fail) {
		std::printf("swp50_aegtest: %d NG\n", g_fail);
		return 1;
	}
	std::printf("swp50_aegtest: OK\n");
	return 0;
}
