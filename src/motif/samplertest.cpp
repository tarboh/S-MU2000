// license:BSD-3-Clause
// サンプリング方式の芯（sampler.h）の試験。作った正弦波のサンプルで、音程・ループ・音量・リリースを確かめる。
//
//   g++ -std=c++17 -O1 -o samplertest src/motif/samplertest.cpp && ./samplertest

#include "sampler.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("NG  %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

// ゼロ交差から周波数
static double freq(const std::vector<float> &x, size_t from, size_t n)
{
	std::vector<double> z;
	for (size_t i = from + 1; i < from + n && i < x.size(); i++)
		if (x[i - 1] < 0 && x[i] >= 0)
			z.push_back(i - 1 + (-x[i - 1]) / (x[i] - x[i - 1]));
	return z.size() < 3 ? 0.0 : 44100.0 * (z.size() - 1) / (z.back() - z.front());
}

static double rms(const std::vector<float> &x, size_t from, size_t n)
{
	double s = 0;
	for (size_t i = from; i < from + n; i++)
		s += double(x[i]) * x[i];
	return std::sqrt(s / n);
}

int main()
{
	using namespace motif_sampler;
	// 441Hz の正弦波（100 サンプルで 1 周期）。出だし 1000 サンプル + ループ 100 サンプル
	sample s;
	for (int i = 0; i < 1100; i++) {
		const float v = float(0.5 * std::sin(2 * 3.14159265358979 * i / 100.0));
		s.L.push_back(v);
		s.R.push_back(v);
	}
	s.loop_start = 1000;
	s.loop_length = 100;
	s.key = 60;

	auto play = [&](const element_params &p, int note, int n, int release_at = -1) {
		voice v;
		v.start(s, p, note, 100);
		std::vector<float> out(n);
		for (int i = 0; i < n; i++) {
			if (i == release_at)
				v.release();
			float l = 0, r = 0;
			v.render(l, r);
			out[i] = l;
		}
		return out;
	};

	element_params p;
	// 録った鍵では 441Hz、ループで鳴り続ける
	auto a = play(p, 60, 44100);
	CHECK(std::fabs(freq(a, 0, 4000) - 441.0) < 0.5);
	CHECK(std::fabs(freq(a, 40000, 4000) - 441.0) < 0.5);
	CHECK(std::fabs(rms(a, 40000, 4000) - rms(a, 0, 4000)) < 0.01);
	// 1 オクターブ上は 882Hz、1 半音上は 85 か 86 目盛り（2^(85/1024) = +99.6 セント）
	auto b = play(p, 72, 22050);
	CHECK(std::fabs(freq(b, 2000, 8000) - 882.0) < 1.0);
	auto c = play(p, 61, 22050);
	CHECK(std::fabs(1200 * std::log2(freq(c, 2000, 8000) / 441.0) - 100.0) < 0.8);
	// コースチューン +12 は鍵 +12 と同じ
	element_params pc = p;
	pc.coarse = 12;
	CHECK(std::fabs(freq(play(pc, 60, 22050), 2000, 8000) - 882.0) < 1.0);
	// 音量 64 は -12.06dB（測った表）
	element_params pl = p;
	pl.level = 64;
	CHECK(std::fabs(20 * std::log10(rms(play(pl, 60, 8000), 2000, 4000) / rms(a, 2000, 4000)) - (-12.06)) < 0.1);
	// パン中央は左右同じで、音量は 0dB（√2 × cos 45° = 1）
	CHECK(std::fabs(rms(a, 2000, 4000) - 0.5 / std::sqrt(2.0)) < 0.01);
	// リリース 0 はすぐ消える。リリース 64 は 34,950 サンプルで 255 目盛り（96dB）下がる
	auto d = play(p, 60, 20000, 10000);
	CHECK(rms(d, 10010, 2000) < 1e-6);
	element_params pr = p;
	pr.aeg.time[3] = 64;
	auto e = play(pr, 60, 10000 + 34950 / 2 + 2000, 10000);
	const double drop = 20 * std::log10(rms(e, 10000 + 34950 / 2 - 500, 1000) / rms(a, 2000, 4000));
	CHECK(std::fabs(drop - (-48.0)) < 1.5);
	// フィルター: LPF12・目盛り 100（f0 ≈ 200Hz）を通すと 441Hz は小さくなる
	element_params pf = p;
	pf.filter.type = swp50_filter::LPF12;
	pf.filter.cutoff = 100;
	CHECK(rms(play(pf, 60, 20000), 10000, 4000) < 0.5 * rms(a, 10000, 4000));

	std::printf("samplertest: %s\n", g_fail ? "NG" : "OK");
	return g_fail ? 1 : 0;
}
