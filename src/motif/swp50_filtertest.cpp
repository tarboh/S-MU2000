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

	// 2b. レゾナンスの実機の値（目盛り 128、f0 = 458Hz での高さ。mr_reso.py の録音）。
	//     録音では雛形のベロシティ感度（+35）がベロシティ 100 で効いていたので、本当のレゾナンスは書いた値 48・64・32 より 7.4 低い 41・57・25
	CHECK(std::fabs(response_db(LPF12, 128, 458.4, 41) - 8.98) < 0.8);
	CHECK(std::fabs(response_db(LPF12, 128, 458.4, 57) - 14.94) < 0.8);
	CHECK(std::fabs(response_db(BPF6, 128, 458.4, 57) - 14.87) < 0.8);
	CHECK(std::fabs(response_db(LPF24A, 128, 458.4, 57) - 14.97) < 3.0);  // LPF24A は山の頂上が 2.7dB 低く出る（全体の RMS は 0.8dB）
	CHECK(std::fabs(response_db(LPF18, 128, 458.4, 57) - 12.07) < 1.0);
	CHECK(std::fabs(response_db(LPF24D, 128, 458.4, 25) - 5.85) < 1.0);
	CHECK(std::fabs(type_q(LPF12, 0) - 2.0f) < 1e-6 && std::fabs(type_q(LPF12, 32) - 0.5f) < 1e-6);         // 16 ごとに半分
	CHECK(std::fabs(type_q(BPF6, 32) - 0.511f) < 0.02);                     // mr_fscale.py の録音（感度 0）で 0.511
	// レゾナンスのベロシティ感度（実機: 感度 96 = +32 でベロシティ 64 は 32 → 15.6（q 1.02）、127 は 32 のまま。感度 32 = −32 は裏返し）
	CHECK(resonance_with_velocity(32, 96, 127) == 32 && resonance_with_velocity(32, 32, 1) == 32);
	CHECK(std::abs(resonance_with_velocity(32, 96, 64) - 16) <= 1);
	CHECK(resonance_with_velocity(32, 0, 127) == 0 && resonance_with_velocity(32, 64, 1) == 32);
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

	// ベロシティ感度（実機: 感度 80 でベロシティ 127 が +4.9 目盛り、1 が -51.1。感度 48 は裏返しで同じ値）
	CHECK(std::fabs(velocity_steps(80, 127) - 4.9) < 1.0);
	CHECK(std::fabs(velocity_steps(80, 1) - (-51.1)) < 1.5);
	CHECK(std::fabs(velocity_steps(48, 1) - velocity_steps(80, 127)) < 1e-9);
	CHECK(velocity_steps(64, 1) == 0 && velocity_steps(64, 127) == 0);

	// Filter Gain（実機: 255 を 0dB として、64 で -71.7dB、32 以下は無音）
	CHECK(std::fabs(filter_gain_db(64) - (-71.7)) < 0.5);
	CHECK(filter_gain_db(32) < -1e6 && filter_gain_db(255) == 0);

	// Distance（実機、目盛り 128: 128 で DualBPF の山は 0dB、DualLPF の低域は +6dB。96 で DualBEF の 2 つめの谷が 120Hz 付近）
	CHECK(std::fabs(response_db(DUAL_BPF, 128, 458.4, 0, 0, 128) - 0.0) < 0.5);
	CHECK(std::fabs(response_db(DUAL_LPF, 128, 30.0, 0, 0, 128) - 6.0) < 0.5);
	CHECK(response_db(DUAL_BEF, 128, 115.0, 0, 0, 96) < -20);
	for (int t : { DUAL_LPF, DUAL_HPF, DUAL_BPF, DUAL_BEF })
		for (double f : { 60.0, 458.4, 2000.0 }) {
			swp50_filter::element_filter flt;
			flt.set(t, 128, 0, 64, 60, 0, 64, 96);
			flt.reset();
			double re = 0, im = 0;
			for (int i = 0; i < (1 << 18); i++) {
				const double y = flt.process(i == 0 ? 1.0f : 0.0f);
				re += y * std::cos(2 * PI * f * i / 44100.0);
				im -= y * std::sin(2 * PI * f * i / 44100.0);
			}
			const double a = 10 * std::log10(re * re + im * im + 1e-30), b = response_db(t, 128, f, 0, 0, 96);
			if (b > -80 && std::fabs(a - b) > 0.05) {
				std::printf("dual type %d f %.0f: process %.2f dB, response %.2f dB\n", t, f, a, b);
				g_fail++;
			}
		}

	// カットオフのスケーリング（実機: ブレークポイント 36・48・72・84）
	{
		const int bp[4] = { 36, 48, 72, 84 };
		const int a[4] = { 168, 88, 168, 88 };     // +40, -40, +40, -40
		const int b[4] = { 192, 192, 192, 192 };   // 全部 +64
		CHECK(std::fabs(scaling_steps(60, bp, a)) < 1e-9);                   // 測定 +0.0
		CHECK(std::fabs(scaling_steps(24, bp, a) - 30.6) < 2.0);             // 測定 +30.6（式は +32）
		CHECK(std::fabs(scaling_steps(45, bp, a) - (-17.8)) < 2.5);          // 測定 -17.8（式は -16）
		CHECK(std::fabs(scaling_steps(96, bp, a) - (-32.9)) < 2.0);          // 測定 -32.9（式は -32）
		for (int k = 24; k <= 96; k += 3)
			CHECK(std::fabs(scaling_steps(k, bp, b) - 51.2) < 1e-9);         // 測定はどの鍵も +51.1〜51.3
	}
	{
		// パラメーターの構造体の既定値は、何も効かせない set と同じ
		swp50_filter::filter_params p;
		p.type = LPF12; p.cutoff = 150;
		swp50_filter::element_filter a, b;
		a.set(p, 72, 90);
		b.set(LPF12, 150);
		CHECK(a.g == b.g && a.q == b.q && a.hg == b.hg);
	}

	// FEG: 高さ +32・深さ +32（96）で +1 オクターブ = +25.6 目盛り（実機 +25.7）
	{
		swp50_filter::filter_eg e;
		for (int i = 0; i < 5; i++)
			e.level[i] = 32;
		e.depth = 96;
		e.key_on();
		e.step();
		CHECK(std::fabs(e.steps() - 25.6) < 1e-9);
	}
	// FEG のアタック（-64 → +64、深さ +63）: 90% に届く時刻は実機で 0.708 秒（時間 64）・2.821 秒（時間 80）
	for (int T : { 64, 80 }) {
		swp50_filter::filter_eg e;
		const int lv[5] = { -64, 64, 64, 64, 64 };
		for (int i = 0; i < 5; i++)
			e.level[i] = lv[i];
		e.time[1] = T;
		e.depth = 127;
		e.key_on();
		int n = 0;
		while (n < 44100 * 10) {
			e.step();
			n++;
			if (e.value >= -64 + 0.9 * 128)
				break;
		}
		const double t = n / 44100.0, want = T == 64 ? 0.708 : 2.821;
		CHECK(std::fabs(t - want) / want < 0.05);
	}
	CHECK(std::fabs(cutoff_with_feg(128, -202) - 74) < 1e-9 && cutoff_with_feg(250, 30) == 255);
	// FEG の高さのベロシティ感度（実機: 感度 +63 でベロシティ 127 は 1.166 倍、1 は 0。感度 −63 は裏返し。感度 +32・ベロシティ 64 は 0.687）
	CHECK(std::fabs(feg_level_velocity_factor(127, 0, 127) - 1.166) < 0.01);
	CHECK(std::fabs(feg_level_velocity_factor(127, 0, 1)) < 0.01);
	CHECK(std::fabs(feg_level_velocity_factor(0, 0, 1) - 1.166) < 0.01);
	CHECK(std::fabs(feg_level_velocity_factor(96, 0, 64) - 0.687) < 0.01);
	CHECK(std::fabs(feg_level_velocity_factor(127, 3, 64) - 0.843) < 0.01);
	CHECK(feg_level_velocity_factor(64, 2, 30) == 1.0);
	// FEG の時間のベロシティ感度（実機: 感度 +16 で、ベロシティ 64 の長さはベロシティ 1 の 0.51 倍）
	{
		swp50_filter::filter_eg e;
		e.time_vel_sens = 80;
		e.velocity = 1;
		const double a = e.seg_samples(64);
		e.velocity = 64;
		const double b = e.seg_samples(64);
		CHECK(std::fabs(b / a - 0.51) < 0.08);
	}
	// FEG のホールド（実機: 値 64 で 0.453 秒）と時間のキーフォロー（実機: 感度 +32 で鍵 36 は鍵 60 の 2.74 倍、鍵 84 は 0.29 倍）
	CHECK(std::fabs(swp50_filter::filter_eg::hold_seconds(64) - 0.453) < 1e-9);
	{
		swp50_filter::filter_eg e;
		e.time_kf = 96;
		e.key = 60;
		const double c = e.seg_samples(64);
		e.key = 36;
		const double lo = e.seg_samples(64);
		e.key = 84;
		const double hi = e.seg_samples(64);
		CHECK(std::fabs(lo / c - 2.74) < 0.4);
		CHECK(std::fabs(hi / c - 0.29) < 0.08);
	}

	// 欠番は次の値になる
	CHECK(canonical_type(9) == BPFW && canonical_type(18) == LPF12_BPF6 && canonical_type(20) == THRU);

	// g の表は増え続ける（230 から上は頭打ち）
	for (int c = 0; c < 230; c++)
		CHECK(g_table[c + 1] >= g_table[c]);

	// 上の端（レゾナンス 0）: LPF24D は目盛り 240 で素通し、220 は 8kHz で −4〜−6dB（実機 −5.9dB、mr_filter_top.py --r0）
	{
		auto gain_db = [](int c, double f) {
			element_filter e;
			e.set(LPF24D, c, 0);
			double in = 0, out = 0;
			for (int n = 0; n < 44100; n++) {
				const float x = float(std::sin(2 * PI * f * n / 44100.0));
				const float y = e.process(x);
				if (n >= 22050) {
					in += double(x) * x;
					out += double(y) * y;
				}
			}
			return 10 * std::log10(out / in);
		};
		CHECK(std::fabs(gain_db(240, 15000)) < 0.2);
		const double g8 = gain_db(220, 8000);
		CHECK(g8 < -3.5 && g8 > -6.5);
	}

	std::printf("swp50_filtertest: %s\n", g_fail ? "NG" : "OK");
	return g_fail ? 1 : 0;
}
