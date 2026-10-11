// license:BSD-3-Clause
//
// 波形を 0 から作る（サンプリングの窓の「波形を作る」）。録る代わりに、PC で作った 1 周期の形を
// サンプリング RAM のサンプルにして、サンプル音色で鳴らす。
//
// どの作り方（基本の波形・倍音を足す・手描き）も、いったん倍音ごとの強さ（cos と sin の係数）に直す。
// そこから LOOP_FRAMES サンプルにちょうど CYCLES 周期が入る波形を作る。全体をループにすると
// 44100 × 25 / 4214 = 261.628Hz で、鍵 60 の高さ（261.626Hz）と 0.02 セントしか違わないので、
// 音色の側で音程を直さなくてよい（ループの頭は偶数の位置にしか置けないが、長さは 4214 で偶数）。
// 登録するときは with_loop_tail で終わりに頭の 4 サンプルを足す（音源は「全体 − 4」の所で折り返すので、ループがちょうど 4214 になる）。
// 1 周期は 168.56 サンプルと半端だが、倍音を足して作るので問題にならない。倍音は 20kHz より下だけを足す
// （鍵 60 で 76 倍音まで。ここでは HARMONICS までにする）。

#ifndef S_MU2000_WAVEGEN_H
#define S_MU2000_WAVEGEN_H

#pragma once

#include "compat/mamecompat.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace smu2000::wavegen {

constexpr int HARMONICS = 64;
constexpr u32 LOOP_FRAMES = 4214;
constexpr int CYCLES = 25;
constexpr double PI = 3.14159265358979323846;

// 倍音 h（1 から）の cos と sin の係数。[0] は使わない（直流は入れない）
struct spectrum {
	float a[HARMONICS + 1] = {};
	float b[HARMONICS + 1] = {};
	double mag(int h) const { return std::sqrt(double(a[h]) * a[h] + double(b[h]) * b[h]); }
};

// 1 周期の形（n 点、-1〜1）を倍音に直す
inline spectrum from_cycle(const float *cycle, int n)
{
	spectrum s;
	for (int h = 1; h <= HARMONICS && h * 2 < n; h++) {
		double ca = 0, cb = 0;
		for (int i = 0; i < n; i++) {
			const double t = 2 * PI * h * double(i) / n;
			ca += cycle[i] * std::cos(t);
			cb += cycle[i] * std::sin(t);
		}
		s.a[h] = float(ca * 2 / n);
		s.b[h] = float(cb * 2 / n);
	}
	return s;
}

enum class shape { sine, saw, square, triangle };

// 基本の波形。pulse は矩形の上側の割合（0.05〜0.95）
inline spectrum basic(shape sh, double pulse = 0.5)
{
	constexpr int N = 2048;
	std::vector<float> c(N);
	for (int i = 0; i < N; i++) {
		const double p = (double(i) + 0.5) / N;      // 0〜1
		switch (sh) {
		case shape::sine:     c[size_t(i)] = float(std::sin(2 * PI * p)); break;
		case shape::saw:      c[size_t(i)] = float(1.0 - 2.0 * p); break;
		case shape::square:   c[size_t(i)] = p < pulse ? 1.0f : -1.0f; break;
		case shape::triangle: c[size_t(i)] = float(p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4); break;
		}
	}
	return from_cycle(c.data(), N);
}

// ファミコン（2A03）の音。矩形はデューティ 12.5・25・50・75%、三角は 4bit・32 段の階段（15→0→15）
enum class famicom_wave { pulse12, pulse25, pulse50, pulse75, triangle };

inline spectrum famicom(famicom_wave k)
{
	if (k != famicom_wave::triangle) {
		static constexpr double DUTY[4] = { 0.125, 0.25, 0.5, 0.75 };
		return basic(shape::square, DUTY[int(k)]);
	}
	// 1 段を 64 点で持つ（階段の角が倍音に出るように）
	constexpr int N = 32 * 64;
	std::vector<float> c(N);
	for (int i = 0; i < N; i++) {
		const int step = i / 64;
		const int v = step < 16 ? 15 - step : step - 16;
		c[size_t(i)] = float(v) / 7.5f - 1.0f;
	}
	return from_cycle(c.data(), N);
}

// ファミコンのノイズ。15bit のシフトレジスタで、長い周期（32767 段。bit 0 と bit 1 の XOR を戻す）と
// 短い周期（93 段。bit 0 と bit 6。金属的な音）。1 段を hold サンプル持たせる。全体をループにする（長さは偶数）
inline std::vector<s16> famicom_noise(bool short_mode, double level = 0.9, int hold = 2)
{
	const u32 steps = short_mode ? 93 : 32767;
	std::vector<s16> out;
	out.reserve(size_t(steps) * size_t(hold));
	u32 r = 1;
	const s16 hi = s16(std::lround(level * 32767.0)), lo = s16(-hi);
	for (u32 i = 0; i < steps; i++) {
		for (int k = 0; k < hold; k++)
			out.push_back((r & 1) ? lo : hi);
		const u32 fb = (r ^ (r >> (short_mode ? 6 : 1))) & 1;
		r = (r >> 1) | (fb << 14);
	}
	return out;
}

// FM（2 オペレーター）。1 周期のあいだにキャリアが carrier 回、モジュレーターが modulator 回まわる（整数の比なので
// 1 周期で閉じる）。index は変調の深さ（ラジアン）、feedback はモジュレーターが自分にかける変調（0〜1。前の 2 点の平均を戻す）
inline spectrum fm(int carrier, int modulator, double index, double feedback = 0.0)
{
	constexpr int N = 2048;
	std::vector<float> c(N);
	double m1 = 0, m2 = 0;
	// フィードバックが落ち着くよう、1 周期ぶん空回ししてから取る
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < N; i++) {
			const double p = 2 * PI * (double(i) + 0.5) / N;
			const double mod = std::sin(modulator * p + feedback * PI * (m1 + m2) * 0.5);
			m2 = m1;
			m1 = mod;
			c[size_t(i)] = float(std::sin(carrier * p + index * mod));
		}
	return from_cycle(c.data(), N);
}

// ハードシンク。1 周期のあいだに ratio 回まわるノコギリを、周期の頭で必ず振り出しに戻す（ratio は 1〜16、半端でよい）
inline spectrum sync(double ratio)
{
	constexpr int N = 2048;
	std::vector<float> c(N);
	for (int i = 0; i < N; i++) {
		const double p = (double(i) + 0.5) / N * std::max(ratio, 1.0);
		c[size_t(i)] = float(1.0 - 2.0 * (p - std::floor(p)));
	}
	return from_cycle(c.data(), N);
}

// ウェーブフォールド。サインを gain 倍して折り返す（sin(gain × sin x + bias)）。gain が大きいほど倍音が増え、
// bias を入れると偶数の倍音も出る
inline spectrum fold(double gain, double bias = 0.0)
{
	constexpr int N = 2048;
	std::vector<float> c(N);
	for (int i = 0; i < N; i++)
		c[size_t(i)] = float(std::sin(gain * std::sin(2 * PI * (double(i) + 0.5) / N) + bias));
	return from_cycle(c.data(), N);
}

// 声（母音）。ノコギリ（1/n）の倍音に、フォルマント 3 つの山をかける。vowel は 0〜4（あ・い・う・え・お）で、
// 半端な値は隣とのあいだ。山の位置は鍵 60（261.6Hz）で鳴らしたときの Hz なので、鍵を変えると山も一緒に動く
inline spectrum vowel(double v)
{
	static constexpr double F[5][3] = { { 800, 1200, 2500 }, { 300, 2300, 3000 }, { 350, 1300, 2400 },
	                                    { 500, 1900, 2500 }, { 500, 900, 2500 } };
	static constexpr double BW[3] = { 90, 110, 140 }, GAIN[3] = { 1.0, 0.6, 0.3 };
	v = std::clamp(v, 0.0, 4.0);
	const int i0 = std::min(int(v), 3);
	const double t = v - i0;
	const double f0 = 44100.0 * CYCLES / LOOP_FRAMES;
	spectrum s;
	for (int h = 1; h <= HARMONICS; h++) {
		double g = 0.03;                               // 山の外にも少し残す
		for (int k = 0; k < 3; k++) {
			const double fc = F[i0][k] + (F[i0 + 1][k] - F[i0][k]) * t;
			const double d = (h * f0 - fc) / BW[k];
			g += GAIN[k] / (1.0 + d * d);
		}
		s.b[h] = float(g / h);
	}
	return s;
}

// 倍音をでたらめに（seed で決まる）。1/n の傾きに 0〜1 の乱数をかける。count 倍音まで
inline spectrum random_harmonics(u32 seed, int count = 32)
{
	spectrum s;
	u32 r = seed * 2654435761u + 1;
	for (int h = 1; h <= HARMONICS && h <= count; h++) {
		r ^= r << 13;
		r ^= r >> 17;
		r ^= r << 5;
		s.b[h] = float(double(r & 0xffff) / 65535.0 / (h == 1 ? 1.0 : std::sqrt(double(h))));
	}
	s.b[1] = std::max(s.b[1], 0.5f);
	return s;
}

// 1 周期の形を、段数（steps。0 = そのまま）と bit 数（bits。0 = そのまま）に落とす。波形メモリ音源
// （ゲームボーイの 32 段・4bit、SCC の 32 段・8bit など）の形にする
inline spectrum from_cycle_stepped(const float *cyc, int n, int steps, int bits)
{
	constexpr int N = 2048;
	std::vector<float> c(N);
	for (int i = 0; i < N; i++) {
		double p = (double(i) + 0.5) / N;
		if (steps > 0)
			p = (std::floor(p * steps) + 0.5) / steps;
		double v = cyc[std::clamp(int(p * n), 0, n - 1)];
		if (bits > 0) {
			const double q = double((1 << bits) - 1);
			v = std::round((v * 0.5 + 0.5) * q) / q * 2.0 - 1.0;
		}
		c[size_t(i)] = float(v);
	}
	return from_cycle(c.data(), N);
}

// ---- 倍音にならない成分を含む音（オルガンの低い管、デチューンした重ね）
// 成分ごとに、鍵 60 の C3 に対する高さの比・大きさ・位相。ループを LOOP_FRAMES × mult サンプルに伸ばし、
// 比を 1/(CYCLES × mult) の倍数に丸めるので、どの成分もループの中でちょうど整数回まわる（つなぎ目が出ない）。
// mult = 8 なら 0.76 秒・比の刻みは 1/200（8.6 セント）
struct partial { double ratio, amp, phase; };

inline std::vector<s16> render_partials(const std::vector<partial> &parts, int mult, double level = 0.9)
{
	const u32 frames = LOOP_FRAMES * u32(std::max(mult, 1));
	const int cycles = CYCLES * std::max(mult, 1);
	std::vector<double> x(frames, 0.0);
	for (const partial &p : parts) {
		const long k = std::lround(p.ratio * cycles);
		if (k <= 0 || p.amp == 0.0 || double(k) * 44100.0 / frames >= 20000.0)
			continue;
		// sin を回転で進める（成分 × サンプルの数だけ sin を呼ばない）
		const double w = 2 * PI * double(k) / frames;
		const double cw = std::cos(w), sw = std::sin(w);
		double c = std::cos(p.phase), s = std::sin(p.phase);
		for (u32 i = 0; i < frames; i++) {
			x[i] += p.amp * s;
			const double nc = c * cw - s * sw;
			s = s * cw + c * sw;
			c = nc;
		}
	}
	double peak = 0;
	for (double v : x)
		peak = std::max(peak, std::fabs(v));
	std::vector<s16> out(frames, 0);
	if (peak <= 0)
		return out;
	const double g = std::clamp(level, 0.0, 1.0) * 32767.0 / peak;
	for (u32 i = 0; i < frames; i++)
		out[i] = s16(std::lround(x[i] * g));
	return out;
}

// オルガンのドローバー 9 本（16' 5⅓' 8' 4' 2⅔' 2' 1⅗' 1⅓' 1'）。値は 0〜8 で、1 段 3dB。8' が鍵 60 の C3
inline std::vector<partial> organ(const int *bars)
{
	static constexpr double RATIO[9] = { 0.5, 1.5, 1, 2, 3, 4, 5, 6, 8 };
	std::vector<partial> out;
	for (int i = 0; i < 9; i++)
		if (bars[i] > 0)
			out.push_back({ RATIO[i], std::pow(10.0, -3.0 * (8 - std::min(bars[i], 8)) / 20.0), 0.0 });
	return out;
}
constexpr int ORGAN_MULT = 2;      // 16' と 5⅓' が半端な比（0.5・1.5）なので、ループを 2 倍に

// デチューンした重ね（ユニゾン）。形 s を voices 個、隣どうし step 刻み（1 刻み = 1/(CYCLES × mult)、mult = 8 で 8.6 セント）
// ずつ高さをずらして足す。位相は声ごとにずらす（頭で全部そろって山にならないように）
constexpr int UNISON_MULT = 8;
inline std::vector<partial> unison(const spectrum &s, int voices, int step, int max_h = HARMONICS)
{
	std::vector<partial> out;
	const int base = CYCLES * UNISON_MULT;
	voices = std::clamp(voices, 1, 9);
	for (int v = 0; v < voices; v++) {
		const double r = double(base + (v - voices / 2) * step) / base;
		const double shift = 2 * PI * 0.381966 * v;        // 黄金比ぶんずつ
		for (int h = 1; h <= std::min(max_h, HARMONICS); h++) {
			const double m = s.mag(h);
			if (m <= 0)
				continue;
			out.push_back({ r * h, m, std::atan2(double(s.a[h]), double(s.b[h])) + shift * h });
		}
	}
	return out;
}
inline double unison_cents(int step) { return 1200.0 * std::log2(1.0 + double(step) / (CYCLES * UNISON_MULT)); }

// PWM（パルス幅のうねり）をループに焼き込む。矩形の上側の割合が center ± depth のあいだを、ループ（0.76 秒）の中で
// sweeps 回ゆれる。倍音ごとに足すので折り返しが出ず、幅もループの頭と終わりで同じ所に戻る
inline std::vector<s16> pwm(double center, double depth, int sweeps, int max_h = HARMONICS, double level = 0.9)
{
	const u32 frames = LOOP_FRAMES * UNISON_MULT;
	const double cycles = double(CYCLES * UNISON_MULT), f0 = 44100.0 * cycles / frames;
	std::vector<double> x(frames, 0.0);
	for (u32 i = 0; i < frames; i++) {
		const double t = double(i) / frames;
		const double wd = std::clamp(center + depth * std::sin(2 * PI * sweeps * t), 0.03, 0.97);
		const double th = 2 * PI * cycles * t;
		double v = 0;
		// 位相 0〜2πw が +1、残りが -1 の矩形: a_h = 2 sin(2πhw)/(πh)、b_h = 2 (1 − cos(2πhw))/(πh)
		for (int h = 1; h <= std::min(max_h, HARMONICS) && h * f0 < 20000.0; h++)
			v += (std::sin(2 * PI * h * wd) * std::cos(h * th) + (1.0 - std::cos(2 * PI * h * wd)) * std::sin(h * th)) / h;
		x[i] = v;
	}
	double peak = 1e-12;
	for (double v : x)
		peak = std::max(peak, std::fabs(v));
	std::vector<s16> out(frames);
	for (u32 i = 0; i < frames; i++)
		out[i] = s16(std::lround(x[i] / peak * std::clamp(level, 0.0, 1.0) * 32767.0));
	return out;
}

// ---- 1 度だけ鳴って消える音（ループを入れずに登録する）
namespace detail {
inline void normalize(std::vector<double> &x, std::vector<s16> &out, double level)
{
	double peak = 1e-12;
	for (double v : x)
		peak = std::max(peak, std::fabs(v));
	out.resize(x.size());
	// 終わりの 5ms は 0 へ寄せる（切れ目のプチを消す）
	const size_t fade = std::min<size_t>(220, x.size());
	for (size_t i = 0; i < x.size(); i++) {
		const double f = i + fade >= x.size() ? double(x.size() - 1 - i) / double(fade) : 1.0;
		out[i] = s16(std::lround(x[i] / peak * f * std::clamp(level, 0.0, 1.0) * 32767.0));
	}
}
struct rng {
	u32 r;
	explicit rng(u32 seed) : r(seed ? seed : 1) {}
	double next() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return double(r & 0xffff) / 32767.5 - 1.0; }
};
} // namespace detail

// はじいた弦（Karplus–Strong）。ノイズを 1 周期ぶん詰めた遅延をくり返し、回るたびに少し丸める。
// 遅延の長さは 168.56 サンプル（鍵 60 で C3）。sustain は 0〜1（大きいほど長く鳴る）、bright は 0〜1（はじく強さ・明るさ）
inline std::vector<s16> pluck(double seconds, double sustain, double bright, u32 seed = 1, double level = 0.9)
{
	const u32 frames = u32(std::clamp(seconds, 0.1, 4.0) * 44100.0) & ~1u;
	const double period = double(LOOP_FRAMES) / CYCLES;            // 168.56
	const int n = int(period);
	const double frac = period - n;
	const double g = 0.985 + 0.0148 * std::clamp(sustain, 0.0, 1.0);
	std::vector<double> x(frames, 0.0);
	detail::rng r(seed);
	// 始めのノイズは、bright が低いほど丸める
	double lp = 0;
	const double a = 0.05 + 0.95 * std::clamp(bright, 0.0, 1.0);
	for (int i = 0; i <= n + 1 && u32(i) < frames; i++) {
		lp += a * (r.next() - lp);
		x[size_t(i)] = lp;
	}
	for (u32 i = u32(n) + 2; i < frames; i++)
		x[i] = g * ((1.0 - frac) * x[i - u32(n)] + frac * x[i - u32(n) - 1]);
	std::vector<s16> out;
	detail::normalize(x, out, level);
	return out;
}

// ドラム（アナログのリズムマシンふう）。tune は高さ（0〜1）、decay は長さ（0〜1）、tone は音色（0〜1。種類ごとに意味が違う）
enum class drum { kick, snare, tom, hat, clap, cowbell };

inline std::vector<s16> drum_hit(drum k, double tune, double decay, double tone, double level = 0.9)
{
	tune = std::clamp(tune, 0.0, 1.0);
	decay = std::clamp(decay, 0.0, 1.0);
	tone = std::clamp(tone, 0.0, 1.0);
	const double R = 44100.0;
	detail::rng r(12345);
	std::vector<double> x;
	auto alloc = [&](double sec) { x.assign(size_t(sec * R) & ~size_t(1), 0.0); };
	switch (k) {
	case drum::kick:
	case drum::tom: {
		// サインの高さを上から落とす。tone はアタックのクリック
		const double f1 = k == drum::kick ? 40.0 + 40.0 * tune : 90.0 + 160.0 * tune;
		const double f0 = f1 * (k == drum::kick ? 4.0 : 1.8);
		const double ta = (k == drum::kick ? 0.12 : 0.10) + 0.5 * decay, tp = k == drum::kick ? 0.03 : 0.05;
		alloc(std::min(2.5, ta * 6));
		double ph = 0;
		for (size_t i = 0; i < x.size(); i++) {
			const double t = double(i) / R;
			ph += 2 * PI * (f1 + (f0 - f1) * std::exp(-t / tp)) / R;
			x[i] = std::sin(ph) * std::exp(-t / ta) + tone * 0.5 * r.next() * std::exp(-t / 0.003);
		}
		break;
	}
	case drum::snare: {
		// 胴の 2 つのサインと、高い方を残したノイズ。tone はノイズ（響き線）の割合
		const double f = 150.0 + 120.0 * tune, ta = 0.06 + 0.12 * decay, tn = 0.08 + 0.3 * decay;
		alloc(std::min(2.0, tn * 6));
		double hp = 0, prev = 0;
		for (size_t i = 0; i < x.size(); i++) {
			const double t = double(i) / R, w = r.next();
			hp = 0.7 * (hp + w - prev);
			prev = w;
			x[i] = (1.0 - 0.7 * tone) * (std::sin(2 * PI * f * t) + 0.5 * std::sin(2 * PI * f * 1.59 * t)) * std::exp(-t / ta) +
			       (0.3 + 0.7 * tone) * hp * std::exp(-t / tn);
		}
		break;
	}
	case drum::hat:
	case drum::cowbell: {
		// 倍音にならない高さの矩形を重ねる（ハットは 6 つ、カウベルは 2 つ）。ハットは高い方だけ残す
		static constexpr double HAT[6] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
		const double mul = k == drum::hat ? 1.0 + 2.0 * tune : 0.8 + 0.6 * tune;
		const double ta = k == drum::hat ? 0.02 + 0.5 * decay * decay : 0.05 + 0.4 * decay;
		alloc(std::min(2.0, ta * 6 + 0.05));
		double hp = 0, prev = 0;
		for (size_t i = 0; i < x.size(); i++) {
			const double t = double(i) / R;
			double v = 0;
			if (k == drum::hat) {
				for (double f : HAT)
					v += std::fmod(f * mul * 2.0 * t, 1.0) < 0.5 ? 1.0 : -1.0;
				v = v / 6.0 + tone * 0.6 * r.next();
				hp = 0.85 * (hp + v - prev);
				prev = v;
				v = hp;
			} else {
				v = (std::fmod(540.0 * mul * t, 1.0) < 0.5 ? 1.0 : -1.0) + (std::fmod(800.0 * mul * t, 1.0) < 0.5 ? 1.0 : -1.0);
				hp += (0.15 + 0.6 * tone) * (v - hp);          // tone が低いほど丸い
				v = hp;
			}
			x[i] = v * std::exp(-t / ta);
		}
		break;
	}
	case drum::clap: {
		// ノイズの短い山を 3 つ続けてから、尾を引く。tune は帯の高さ、tone は山の間隔
		const double fc = 800.0 + 1600.0 * tune, gap = 0.008 + 0.012 * tone, tn = 0.05 + 0.35 * decay;
		alloc(std::min(2.0, tn * 6 + 0.1));
		// 帯を通すフィルタ（2 次の共振）
		const double w0 = 2 * PI * fc / R, q = 2.0, alpha = std::sin(w0) / (2 * q);
		const double b0 = alpha / (1 + alpha), a1 = -2 * std::cos(w0) / (1 + alpha), a2 = (1 - alpha) / (1 + alpha);
		double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		for (size_t i = 0; i < x.size(); i++) {
			const double t = double(i) / R, w = r.next();
			const double y = b0 * w - b0 * x2 - a1 * y1 - a2 * y2;
			x2 = x1; x1 = w; y2 = y1; y1 = y;
			double env = 0;
			for (int n = 0; n < 3; n++)
				if (t >= n * gap && t < (n + 1) * gap)
					env = std::exp(-(t - n * gap) / 0.004);
			if (t >= 3 * gap)
				env = std::exp(-(t - 3 * gap) / tn);
			x[i] = y * env;
		}
		break;
	}
	}
	std::vector<s16> out;
	detail::normalize(x, out, level);
	return out;
}

// 位相ひずみ（Casio CZ のやり方）。コサインを読む位相の進み方を曲げて、サインからノコギリ・矩形へ近づける。
// amount は 0〜1（0 でサイン）。reso は、ratio 倍の速さのコサインに 1 周期で閉じる窓（ノコギリ形）をかけた「レゾナンス」の波形
enum class pd_wave { saw, square, reso };

inline spectrum phase_distortion(pd_wave k, double amount, double ratio = 4.0)
{
	constexpr int N = 2048;
	std::vector<float> c(N);
	amount = std::clamp(amount, 0.0, 1.0);
	// 0〜1 の位相 q を、折れ目 x（0.5 から 0.01 へ）で曲げる: 前半は速く、後半はゆっくり
	auto knee = [&](double q) {
		const double x = 0.5 - 0.49 * amount;
		return q < x ? 0.5 * q / x : 0.5 + 0.5 * (q - x) / (1.0 - x);
	};
	for (int i = 0; i < N; i++) {
		const double p = (double(i) + 0.5) / N;
		double v = 0;
		switch (k) {
		case pd_wave::saw:
			v = -std::cos(2 * PI * knee(p));
			break;
		case pd_wave::square: {
			// 半周期ごとに、速く半回転して止まる
			const double h = p < 0.5 ? p * 2 : p * 2 - 1;
			const double q = std::min(1.0, h / (1.0 - 0.98 * amount));
			v = -std::cos(PI * (q + (p < 0.5 ? 0.0 : 1.0)));
			break;
		}
		case pd_wave::reso:
			v = (1.0 - p) * (1.0 - std::cos(2 * PI * std::max(ratio, 1.0) * p)) * (0.2 + 0.8 * amount) +
			    (1.0 - amount) * 0.8 * std::sin(2 * PI * p);
			break;
		}
		c[size_t(i)] = float(v);
	}
	return from_cycle(c.data(), N);
}

// 母音が行って戻るうねりをループに焼き込む（0.76 秒の中で v0 → v1 → v0）。倍音ごとの大きさを時間で変えて足す
inline std::vector<s16> vowel_morph(double v0, double v1, int max_h = HARMONICS, double level = 0.9)
{
	constexpr int STEPS = 64;                          // 母音の形を作り直す回数（そのあいだは直線でつなぐ）
	const u32 frames = LOOP_FRAMES * UNISON_MULT;
	const double cycles = double(CYCLES * UNISON_MULT), f0 = 44100.0 * cycles / frames;
	std::vector<spectrum> sp(STEPS + 1);
	for (int k = 0; k <= STEPS; k++)
		sp[size_t(k)] = vowel(v0 + (v1 - v0) * 0.5 * (1.0 - std::cos(2 * PI * k / STEPS)));
	std::vector<double> x(frames, 0.0);
	for (int h = 1; h <= std::min(max_h, HARMONICS) && h * f0 < 20000.0; h++) {
		const double w = 2 * PI * h * cycles / frames, cw = std::cos(w), sw = std::sin(w);
		double c = 1, s = 0;
		for (u32 i = 0; i < frames; i++) {
			const double t = double(i) / frames * STEPS;
			const int k = std::min(int(t), STEPS - 1);
			const double a = sp[size_t(k)].b[h] + (sp[size_t(k) + 1].b[h] - sp[size_t(k)].b[h]) * (t - k);
			x[i] += a * s;
			const double nc = c * cw - s * sw;
			s = s * cw + c * sw;
			c = nc;
		}
	}
	double peak = 1e-12;
	for (double v : x)
		peak = std::max(peak, std::fabs(v));
	std::vector<s16> out(frames);
	for (u32 i = 0; i < frames; i++)
		out[i] = s16(std::lround(x[i] / peak * std::clamp(level, 0.0, 1.0) * 32767.0));
	return out;
}

// 描いたループ（ディスカッション #106。ANS シンセサイザーのように、絵を音にする）。
// 絵は PAINT_ROWS 行 × PAINT_COLS 列で、行が 1 本のサイン、横がループの中の時間、濃さ（0〜1）が大きさ。
// cells[行 × PAINT_COLS + 列]、行 0 がいちばん低い。行 r の高さは鍵 60 の C3 の (r + 1) × spacing 倍で、
// spacing が 1 なら倍音 1〜PAINT_ROWS、ほかの値なら倍音でない並び（鐘や金属の響き）になる。
// どの行もループの中でちょうど整数回まわるように丸め、濃さは列のあいだをなめらかにつなぎ、右の端は左の端へ
// つながるので、つなぎ目は出ない。ループは PWM・母音のうねりと同じ 0.76 秒。何も描いていなければ無音を返す
constexpr int PAINT_ROWS = 48, PAINT_COLS = 64;
inline int paint_cycles(int row, double spacing)
{
	return std::max(1, int(std::lround(double(row + 1) * spacing * CYCLES * UNISON_MULT)));
}
inline std::vector<s16> paint(const float *cells, double spacing = 1.0, double level = 0.9)
{
	const u32 frames = LOOP_FRAMES * UNISON_MULT;
	std::vector<double> x(frames, 0.0);
	for (int r = 0; r < PAINT_ROWS; r++) {
		const float *row = cells + r * PAINT_COLS;
		bool any = false;
		for (int c = 0; c < PAINT_COLS && !any; c++)
			any = row[c] > 0.0f;
		const int k = paint_cycles(r, spacing);
		if (!any || 44100.0 * k / frames >= 20000.0)
			continue;
		// 行ごとに始まりの位相をずらす（そろえると頭に山が重なって、ほかの所が小さくなる）
		const double ph = 2 * PI * std::fmod(double(r) * 0.6180339887, 1.0);
		const double w = 2 * PI * double(k) / frames, cw = std::cos(w), sw = std::sin(w);
		double c = std::cos(ph), s = std::sin(ph);
		for (u32 i = 0; i < frames; i++) {
			// 列のまん中どうしを、なめらかな段でつなぐ
			const double u = double(i) / frames * PAINT_COLS - 0.5;
			const double fl = std::floor(u);
			const int c0 = (int(fl) + PAINT_COLS) % PAINT_COLS, c1 = (c0 + 1) % PAINT_COLS;
			double f = u - fl;
			f = f * f * (3.0 - 2.0 * f);
			x[i] += (double(row[c0]) + (double(row[c1]) - double(row[c0])) * f) * s;
			const double nc = c * cw - s * sw;
			s = s * cw + c * sw;
			c = nc;
		}
	}
	double peak = 0.0;
	for (double v : x)
		peak = std::max(peak, std::fabs(v));
	std::vector<s16> out(frames, 0);
	if (peak < 1e-9)
		return out;
	for (u32 i = 0; i < frames; i++)
		out[i] = s16(std::lround(x[i] / peak * std::clamp(level, 0.0, 1.0) * 32767.0));
	return out;
}

// FM のベル（1 度鳴って消える）。キャリアは鍵 60 の C3、モジュレーターはその ratio 倍（半端な比で金属の響き）。
// 変調の深さ index は時間とともに減り（明るさが先に消える）、音量は decay 秒で 1/e になる
inline std::vector<s16> fm_bell(double seconds, double ratio, double index, double decay, double level = 0.9)
{
	const u32 frames = u32(std::clamp(seconds, 0.2, 4.0) * 44100.0) & ~1u;
	const double f0 = 44100.0 * CYCLES / LOOP_FRAMES;
	std::vector<double> x(frames);
	for (u32 i = 0; i < frames; i++) {
		const double t = double(i) / 44100.0;
		const double mod = std::sin(2 * PI * f0 * ratio * t);
		x[i] = std::sin(2 * PI * f0 * t + index * std::exp(-t / (decay * 0.6)) * mod) * std::exp(-t / decay) *
		       std::min(1.0, t / 0.002);
	}
	std::vector<s16> out;
	detail::normalize(x, out, level);
	return out;
}

// 倍音の棒（強さだけ、位相は sin）から
inline spectrum from_bars(const float *amp, int count)
{
	spectrum s;
	for (int h = 1; h <= HARMONICS && h <= count; h++)
		s.b[h] = amp[h - 1];
	return s;
}

// 1 周期の形を n 点で（見せる用）。max_h までの倍音を足す
inline std::vector<float> cycle(const spectrum &s, int n, int max_h = HARMONICS)
{
	std::vector<float> out(size_t(n), 0.0f);
	for (int h = 1; h <= std::min(max_h, HARMONICS); h++) {
		if (s.a[h] == 0.0f && s.b[h] == 0.0f)
			continue;
		for (int i = 0; i < n; i++) {
			const double t = 2 * PI * h * double(i) / n;
			out[size_t(i)] += float(s.a[h] * std::cos(t) + s.b[h] * std::sin(t));
		}
	}
	return out;
}

// サンプルにする波形（LOOP_FRAMES サンプル、CYCLES 周期）。いちばん大きい所が level × 32767 になるようにそろえる。
// 全部の倍音が 0 なら無音
inline std::vector<s16> render(const spectrum &s, int max_h = HARMONICS, double level = 0.9)
{
	std::vector<double> x(LOOP_FRAMES, 0.0);
	const double f0 = 44100.0 * CYCLES / LOOP_FRAMES;
	for (int h = 1; h <= std::min(max_h, HARMONICS); h++) {
		if (h * f0 >= 20000.0 || (s.a[h] == 0.0f && s.b[h] == 0.0f))
			continue;
		for (u32 i = 0; i < LOOP_FRAMES; i++) {
			const double t = 2 * PI * h * CYCLES * double(i) / LOOP_FRAMES;
			x[i] += s.a[h] * std::cos(t) + s.b[h] * std::sin(t);
		}
	}
	double peak = 0;
	for (double v : x)
		peak = std::max(peak, std::fabs(v));
	std::vector<s16> out(LOOP_FRAMES, 0);
	if (peak <= 0)
		return out;
	const double g = std::clamp(level, 0.0, 1.0) * 32767.0 / peak;
	for (u32 i = 0; i < LOOP_FRAMES; i++)
		out[i] = s16(std::lround(x[i] * g));
	return out;
}

// ループで鳴らす波形を登録する形にする: 終わりに頭の pad サンプルを足す。音源は「全体 − pad」の所で折り返し、
// 折り返す所の次のサンプルも読むので、そこに頭と同じものが要る（mu2000 の TAIL_PAD = 4）
inline std::vector<s16> with_loop_tail(std::vector<s16> pcm, size_t pad = 4)
{
	const size_t n = pcm.size();
	for (size_t i = 0; i < pad && n; i++)
		pcm.push_back(pcm[i % n]);
	return pcm;
}

// ノイズ。frames サンプル。高さが無いので全体をループにするだけ。
// color: 0 = 白、1 = ピンク（高い方へ 1 オクターブ 3dB ずつ下がる）、2 = ブラウン（6dB ずつ）。
// 色つきは、同じ白色を 2 周ぶんフィルタに通して 2 周目を取る（頭と終わりがつながる）
inline std::vector<s16> noise(u32 frames, double level = 0.9, u32 seed = 1, int color = 0)
{
	std::vector<double> w(frames);
	u32 r = seed ? seed : 1;
	for (u32 i = 0; i < frames; i++) {
		r ^= r << 13;
		r ^= r >> 17;
		r ^= r << 5;
		w[i] = double(r & 0xffff) / 32767.5 - 1.0;
	}
	if (color) {
		std::vector<double> y(frames);
		double b0 = 0, b1 = 0, b2 = 0, br = 0;
		for (int pass = 0; pass < 2; pass++)
			for (u32 i = 0; i < frames; i++) {
				if (color == 1) {                      // Paul Kellet のピンクノイズ（3 段の近似）
					b0 = 0.99765 * b0 + w[i] * 0.0990460;
					b1 = 0.96300 * b1 + w[i] * 0.2965164;
					b2 = 0.57000 * b2 + w[i] * 1.0526913;
					y[i] = b0 + b1 + b2 + w[i] * 0.1848;
				} else {
					br = 0.995 * br + w[i] * 0.05;
					y[i] = br;
				}
			}
		w = y;
	}
	double peak = 1e-12;
	for (double v : w)
		peak = std::max(peak, std::fabs(v));
	std::vector<s16> out(frames);
	for (u32 i = 0; i < frames; i++)
		out[i] = s16(std::lround(w[i] / peak * level * 32767.0));
	return out;
}

// ローファイ。bits（2〜16。16 はそのまま）に丸め、hold サンプルずつ同じ値にする（1 はそのまま）
inline void lofi(std::vector<s16> &pcm, int bits, int hold)
{
	if (hold > 1)
		for (size_t i = 0; i < pcm.size(); i++)
			pcm[i] = pcm[i - i % size_t(hold)];
	if (bits < 16 && bits >= 1) {
		const int sh = 16 - bits;
		for (s16 &v : pcm)
			v = s16(std::clamp(((int(v) >> sh) << sh) + (1 << sh) / 2, -32768, 32767));
	}
}

} // namespace smu2000::wavegen

#endif // S_MU2000_WAVEGEN_H
