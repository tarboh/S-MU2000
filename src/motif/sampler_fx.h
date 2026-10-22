// license:BSD-3-Clause
// サンプリング方式の芯のエフェクト（ボイスのリバーブ・コーラス・インサーション A/B）。tools/motif/mr_fx_apply.py と同じ計算。
// どれも実機の録音から測った形（doc/motif-rack-es-capture.md の「ボイスのリバーブ」〜「インサーションの歪み系」）。
//
//   リバーブ   : 型ごとの応答（mr_fx_export.py が書く fx/reverb_<MSB>_<LSB>.irx）を、Reverb Time の表 4 に合わせて帯ごとに伸び縮みさせて畳み込む。
//                音量は Return / 96 × 送り / 127、入力は左右の平均
//   コーラス   : Chorus 1〜5・Celeste 1〜4。遅れ = Delay Offset（表 2）+ 0.176ms × Depth × 三角波、速さは表 1、左右は逆向き。高さ 2.06
//   インサーション: 3 Band EQ・2 Band EQ（周波数ごとの応答を掛ける）、Compressor（時間で回す）、歪み系（fxd/*.dsx の応答 → 頭打ち → 山のある LPF）、
//                Cross Delay（たすき掛けの 2 本の遅れ）。
//                つなぎ方（並列・A → B・B → A）と、エレメントのスイッチ（全部のエレメントで同じとする）に合わせて掛ける
// 掛ける順は インサーション → （その出力から）リバーブとコーラスを並列に足す。エレメントを足してから掛けるので、エレメントごとに違う
// インサーションを通る音色は正しくない。
#pragma once

#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace motif_fx {

using cplx = std::complex<double>;
using buf = std::vector<double>;
static constexpr double FS = 44100.0;
static constexpr double PI = 3.14159265358979323846;

// ---- FFT（2 のべき乗、その場で） ----
inline void fft(std::vector<cplx> &a, bool inv)
{
	const size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; i++) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j)
			std::swap(a[i], a[j]);
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		const double ang = 2 * PI / double(len) * (inv ? 1 : -1);
		const cplx wl(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len) {
			cplx w(1);
			for (size_t j = 0; j < len / 2; j++) {
				const cplx u = a[i + j], v = a[i + j + len / 2] * w;
				a[i + j] = u + v;
				a[i + j + len / 2] = u - v;
				w *= wl;
			}
		}
	}
	if (inv)
		for (auto &x : a)
			x /= double(n);
}

inline size_t pow2(size_t n)
{
	size_t m = 1;
	while (m < n)
		m <<= 1;
	return m;
}

inline std::vector<cplx> spectrum(const buf &x, size_t N)
{
	std::vector<cplx> a(N);
	for (size_t i = 0; i < x.size() && i < N; i++)
		a[i] = x[i];
	fft(a, false);
	return a;
}

// 周波数 k の値（0..N-1）を、周波数 f（Hz、負の側も含む）として返す
inline double bin_hz(size_t k, size_t N) { return (k <= N / 2 ? double(k) : double(k) - double(N)) * FS / double(N); }

// x に周波数応答 H(f)（f >= 0 で与え、負の側は共役）を掛ける。out の長さは len
template <class F>
buf filter_freq(const buf &x, size_t len, F H)
{
	const size_t N = pow2(len + 1);
	auto a = spectrum(x, N);
	for (size_t k = 0; k <= N / 2; k++) {
		const cplx h = H(double(k) * FS / double(N));
		a[k] *= h;
		if (k > 0 && k < N / 2)
			a[N - k] *= std::conj(h);
	}
	fft(a, true);
	buf y(len);
	for (size_t i = 0; i < len; i++)
		y[i] = a[i].real();
	return y;
}

inline buf convolve(const buf &x, const buf &h, size_t len)
{
	const size_t N = pow2(x.size() + h.size());
	auto a = spectrum(x, N), b = spectrum(h, N);
	for (size_t k = 0; k < N; k++)
		a[k] *= b[k];
	fft(a, true);
	buf y(len);
	for (size_t i = 0; i < len && i < N; i++)
		y[i] = a[i].real();
	return y;
}

// ---- 表（Data List の Effect Data Assign Table） ----
static const double TABLE4[70] = {0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0, 2.1, 2.2, 2.3,
	2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.0, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8, 3.9, 4.0, 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7, 4.8, 4.9, 5.0, 5.5,
	6.0, 6.5, 7.0, 7.5, 8.0, 8.5, 9.0, 9.5, 10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0, 25.0, 30.0};
static const double T3[61] = {20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 70, 80, 90, 100, 110, 125, 140, 160, 180, 200, 225, 250, 280,
	315, 355, 400, 450, 500, 560, 630, 700, 800, 900, 1000, 1100, 1200, 1400, 1600, 1800, 2000, 2200, 2500, 2800, 3200, 3600, 4000, 4500,
	5000, 5600, 6300, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000, 18000, 20000};
static const double LFO_HZ[128] = {0.0, 0.04, 0.08, 0.13, 0.17, 0.21, 0.25, 0.29, 0.34, 0.38, 0.42, 0.46, 0.51, 0.55, 0.59, 0.63, 0.67,
	0.72, 0.76, 0.8, 0.84, 0.88, 0.93, 0.97, 1.01, 1.05, 1.09, 1.14, 1.18, 1.22, 1.26, 1.3, 1.35, 1.39, 1.43, 1.47, 1.51, 1.56, 1.6, 1.64,
	1.68, 1.72, 1.77, 1.81, 1.85, 1.89, 1.94, 1.98, 2.02, 2.06, 2.1, 2.15, 2.19, 2.23, 2.27, 2.31, 2.36, 2.4, 2.44, 2.48, 2.52, 2.57, 2.61,
	2.65, 2.69, 2.78, 2.86, 2.94, 3.03, 3.11, 3.2, 3.28, 3.37, 3.45, 3.53, 3.62, 3.7, 3.87, 4.04, 4.21, 4.37, 4.54, 4.71, 4.88, 5.05, 5.22,
	5.38, 5.55, 5.72, 6.06, 6.39, 6.73, 7.07, 7.4, 7.74, 8.08, 8.41, 8.75, 9.08, 9.42, 9.76, 10.1, 10.8, 11.4, 12.1, 12.8, 13.5, 14.1, 14.8,
	15.5, 16.2, 16.8, 17.5, 18.2, 19.5, 20.9, 22.2, 23.6, 24.9, 26.2, 27.6, 28.9, 30.3, 31.6, 33.0, 34.3, 37.0, 39.7};
static const double MOD_DELAY_MS[128] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9,
	2.0, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.0, 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7, 3.8, 3.9, 4.0, 4.1, 4.2, 4.3, 4.4, 4.5, 4.6,
	4.7, 4.8, 4.9, 5.0, 5.1, 5.2, 5.3, 5.4, 5.5, 5.6, 5.7, 5.8, 5.9, 6.0, 6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8, 6.9, 7.0, 7.1, 7.2, 7.3,
	7.4, 7.5, 7.6, 7.7, 7.8, 7.9, 8.0, 8.1, 8.2, 8.3, 8.4, 8.5, 8.6, 8.7, 8.8, 8.9, 9.0, 9.1, 9.2, 9.3, 9.4, 9.5, 9.6, 9.7, 9.8, 9.9, 10.0,
	11.1, 12.2, 13.3, 14.4, 15.5, 17.1, 18.6, 20.2, 21.8, 23.3, 24.9, 26.5, 28.0, 29.6, 31.2, 32.8, 34.3, 35.9, 37.5, 39.0, 40.6, 42.2,
	43.7, 45.3, 46.9, 48.4, 50.0};
static const double COMP_ATTACK_MS[20] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 23, 26, 30, 35, 40};
static const double COMP_RELEASE_MS[16] = {10, 15, 25, 35, 45, 55, 65, 75, 85, 100, 115, 140, 170, 230, 340, 680};
static const double COMP_RATIO[8] = {1.0, 1.5, 2.0, 3.0, 5.0, 7.0, 10.0, 20.0};

inline double t3(int v) { return T3[v < 0 ? 0 : v > 60 ? 60 : v]; }

// ---- エフェクトのブロック（型と パラメーター 1〜16、リバーブ・コーラスは Return と送り） ----
struct block {
	int msb = 0, lsb = 0;
	int p[17] = {0};          // p[1]..p[16]
	int ret = 96, send = 0;
	double pfine[17] = {0};   // Pitch Change 2 から Pitch Change 1 の模型へ渡す、小数の Fine
	bool use_pfine = false;
	bool is(int m, int l) const { return msb == m && lsb == l; }
};

inline block parse_block(const std::string &s)
{
	block b;
	std::vector<int> v;
	const char *c = s.c_str();
	while (*c) {
		v.push_back(std::atoi(c));
		while (*c && *c != ',')
			c++;
		if (*c == ',')
			c++;
	}
	if (v.empty())
		return b;
	b.msb = v[0] >> 7;
	b.lsb = v[0] & 0x7F;
	for (int i = 1; i <= 16 && i < int(v.size()); i++)
		b.p[i] = v[i];
	if (v.size() > 17)
		b.ret = v[17];
	if (v.size() > 18)
		b.send = v[18];
	return b;
}

// ---- 読み込み（.irx・.dsx） ----
struct stereo_ir {
	buf L, R;
	std::map<std::string, std::string> head;
	bool ok = false;
};

inline stereo_ir load_ir(const std::string &path)
{
	stereo_ir r;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return r;
	char line[512];
	if (!std::fgets(line, sizeof line, f)) {
		std::fclose(f);
		return r;
	}
	std::string s(line);
	size_t pos = s.find(' ');
	while (pos != std::string::npos) {
		const size_t e = s.find_first_of(" \n", pos + 1);
		const std::string tok = s.substr(pos + 1, e == std::string::npos ? std::string::npos : e - pos - 1);
		const size_t eq = tok.find('=');
		if (eq != std::string::npos)
			r.head[tok.substr(0, eq)] = tok.substr(eq + 1);
		pos = (e != std::string::npos && s[e] == ' ') ? e : std::string::npos;
	}
	const long n = std::atol(r.head["frames"].c_str());
	std::vector<float> d(size_t(n) * 2);
	if (n > 0 && std::fread(d.data(), sizeof(float), d.size(), f) == d.size()) {
		r.L.resize(n);
		r.R.resize(n);
		for (long i = 0; i < n; i++) {
			r.L[i] = d[2 * i];
			r.R[i] = d[2 * i + 1];
		}
		r.ok = true;
	}
	std::fclose(f);
	return r;
}

// ---- リバーブ ----
inline bool table4_type(const block &b)
{
	if (b.msb != 1)
		return false;
	static const int l[] = {0x00, 0x01, 0x10, 0x11, 0x12, 0x20, 0x21, 0x30, 0x02, 0x13, 0x31, 0x40, 0x41, 0x42, 0x43};
	for (int x : l)
		if (b.lsb == x)
			return true;
	return false;
}

// h の帯ごとの減り方を、1kHz で rt[3] → target に伸び縮みさせる（隣の帯と半分ずつ重なる三角の重み）
inline buf reshape(const buf &h, const double rt[7], double target)
{
	static const double bands[7] = {125, 250, 500, 1000, 2000, 4000, 8000};
	const size_t N = pow2(h.size());
	const auto H = spectrum(h, N);
	const double k = target / rt[3];
	buf out(h.size(), 0.0);
	for (int b = 0; b < 7; b++) {
		std::vector<cplx> a(H);
		const double c = std::log2(bands[b] / 1000.0);
		for (size_t i = 0; i < N; i++) {
			const double f = std::fabs(bin_hz(i, N));
			const double lf = std::log2(std::max(f, 1e-3) / 1000.0);
			double w = std::max(0.0, 1 - std::fabs(lf - c));
			if ((b == 0 && lf < c) || (b == 6 && lf > c))
				w = 1;
			a[i] *= w;
		}
		fft(a, true);
		const double rm = (std::isfinite(rt[b]) && rt[b] > 0.05 && rt[b] < 60) ? rt[b] : rt[3];
		const double d = 6.9078 * (1.0 / (rm * k) - 1.0 / rm);
		for (size_t i = 0; i < h.size(); i++)
			out[i] += a[i].real() * std::exp(-d * double(i) / FS);
	}
	return out;
}

// 入力（左右）にリバーブの wet を足した出力（長さ len）
inline void reverb(const buf &L, const buf &R, const block &rv, const std::string &fxdir, buf &oL, buf &oR, size_t len)
{
	char name[64];
	std::snprintf(name, sizeof name, "/reverb_%d_%d.irx", rv.msb, rv.lsb);
	stereo_ir ir = load_ir(fxdir + name);
	if (!ir.ok || rv.send == 0)
		return;
	if (table4_type(rv)) {
		double rt[7];
		const std::string &s = ir.head["rt"];
		const char *c = s.c_str();
		for (int i = 0; i < 7; i++) {
			rt[i] = std::atof(c);
			while (*c && *c != ',')
				c++;
			if (*c)
				c++;
		}
		const double target = TABLE4[rv.p[1] < 0 ? 0 : rv.p[1] > 69 ? 69 : rv.p[1]];
		ir.L = reshape(ir.L, rt, target);
		ir.R = reshape(ir.R, rt, target);
	}
	const double g = rv.ret / 96.0 * rv.send / 127.0;
	buf x(L.size());
	for (size_t i = 0; i < x.size(); i++)
		x[i] = (L[i] + R[i]) / 2;
	const buf wL = convolve(x, ir.L, len), wR = convolve(x, ir.R, len);
	for (size_t i = 0; i < len; i++) {
		oL[i] += wL[i] * g;
		oR[i] += wR[i] * g;
	}
}

// ---- コーラス ----
inline bool chorus_type(const block &b)
{
	if (b.msb != 5)
		return false;
	static const int l[] = {0x00, 0x01, 0x02, 0x08, 0x09, 0x10, 0x11, 0x12, 0x18};
	for (int x : l)
		if (b.lsb == x)
			return true;
	return false;
}

inline void chorus(const buf &L, const buf &R, const block &ch, buf &oL, buf &oR)
{
	if (!chorus_type(ch) || ch.send == 0)
		return;
	const double hz = LFO_HZ[std::min(127, ch.p[1])];
	const double depth = 0.176 * ch.p[2] / 1000 * FS;
	const double off = MOD_DELAY_MS[std::min(127, ch.p[4])] / 1000 * FS;
	const double g = 2.06 * ch.ret / 96.0 * ch.send / 127.0;
	const size_t n = L.size();
	for (size_t t = 0; t < n; t++) {
		double ph = std::fmod(double(t) * hz / FS, 1.0);
		const double tri = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
		for (int c = 0; c < 2; c++) {
			const double d = off + depth * (c == 0 ? tri : 1 - tri);
			const double pos = double(t) - d;
			if (pos < 0)
				continue;
			const size_t i = size_t(pos);
			const double fr = pos - double(i);
			const double a = (L[i] + R[i]) / 2, b = i + 1 < n ? (L[i + 1] + R[i + 1]) / 2 : a;
			(c == 0 ? oL : oR)[t] += (a + (b - a) * fr) * g;
		}
	}
}

// ---- インサーション ----
inline cplx shelf1(bool low, double f0, double g, double f)
{
	const cplx z = std::exp(cplx(0, -2 * PI * f / FS));
	const double A = std::pow(10.0, g / 20), K = std::tan(PI * std::min(f0, FS * 0.49) / FS), r = std::sqrt(A);
	double b0, b1, a0, a1;
	if (low) {
		b0 = K * r + 1; b1 = K * r - 1; a0 = K / r + 1; a1 = K / r - 1;
	} else {
		b0 = r + K; b1 = K - r; a0 = 1 / r + K; a1 = K - 1 / r;
	}
	return (b0 + b1 * z) / (a0 + a1 * z);
}

inline cplx eq3(const block &b, double f)
{
	cplx H = 1;
	if (b.p[1] != 64)
		H *= shelf1(true, t3(b.p[6]) * std::sqrt(2.0), b.p[1] - 64, f);
	if (b.p[5] != 64)
		H *= shelf1(false, t3(b.p[7]) / std::sqrt(2.0), b.p[5] - 64, f);
	const int g = b.p[3] - 64;
	if (g != 0) {
		const cplx z = std::exp(cplx(0, -2 * PI * f / FS));
		const double A = std::pow(10.0, g / 20.0), w = 2 * PI * t3(b.p[2]) / FS;
		const double al = std::sin(w) / (2 * std::max(0.1, b.p[4] / 10.0)), c = std::cos(w);
		double n0 = 1 + al, n2 = 1 - al, d0 = 1 + al, d2 = 1 - al;
		if (g > 0) {
			n0 = 1 + al * A; n2 = 1 - al * A;
		} else {
			d0 = 1 + al / A; d2 = 1 - al / A;
		}
		H *= (n0 - 2 * c * z + n2 * z * z) / (d0 - 2 * c * z + d2 * z * z);
	}
	return H;
}

inline cplx eq2(const block &b, double f)
{
	cplx H = 1;
	for (int band = 0; band < 2; band++) {
		const int g = b.p[band == 0 ? 2 : 4] - 64;
		if (g == 0)
			continue;
		const double r = std::sqrt(std::pow(10.0, std::abs(g) / 20.0));
		const double f0 = t3(b.p[band == 0 ? 1 : 3]) * (band == 0 ? r : 1 / r);
		H *= shelf1(band == 0, f0, g, f);
	}
	return H;
}

inline void compressor(buf &L, buf &R, const block &b)
{
	const size_t n = L.size();
	const int win = int(0.005 * FS);
	const double T = b.p[3] - 127, Rr = COMP_RATIO[std::min(7, b.p[4])];
	const double ka = std::exp(-1.0 / (1.6 * COMP_ATTACK_MS[std::min(19, b.p[1])] / 1000 * FS));
	const double rel = 7.5 / (COMP_RELEASE_MS[std::min(15, b.p[2])] / 1000) / FS;
	const double makeup = 40 * std::log10(std::max(1, b.p[5]) / 64.0);
	double acc = 0, g = 0;
	std::vector<double> sq(n);
	for (size_t i = 0; i < n; i++) {
		const double x = (L[i] + R[i]) / 2;
		sq[i] = x * x;
		acc += sq[i];
		if (i >= size_t(win))
			acc -= sq[i - win];
		const double lev = 10 * std::log10(std::max(acc, 0.0) / win + 1e-20);
		const double t = lev > T ? (lev - T) * (1 - 1 / Rr) : 0.0;
		g = t > g ? t + (g - t) * ka : std::max(t, g - rel);
		const double k = std::pow(10.0, (makeup - g) / 20);
		L[i] *= k;
		R[i] *= k;
	}
}

struct dist_set { int amp, drive; const char *name; };

inline double interp(const double *xs, const double *ys, int n, double x)
{
	if (x <= xs[0])
		return ys[0];
	for (int i = 0; i < n - 1; i++)
		if (x <= xs[i + 1])
			return ys[i] + (x - xs[i]) / (xs[i + 1] - xs[i]) * (ys[i + 1] - ys[i]);
	return ys[n - 1];
}

// Amp Simulator（Stack）の 130Hz の小さい音の上げ幅（Drive 0〜10 は変わらず、15 から上がる。127 は 64 からの延ばし）
inline double drive_gain_db(double d)
{
	static const double xs[7] = {0, 10, 15, 20, 30, 64, 127}, ys[7] = {0.9, 0.9, 6.7, 11.0, 17.3, 28.7, 28.7 + 0.34 * 63};
	return interp(xs, ys, 7, d);
}

// 頭打ちの大きさ c（Drive 0〜10 で下がり、15 から上はほぼ一定）と、Edge による硬さ p（対数で補間）
inline double drive_clip(double d)
{
	static const double xs[5] = {0, 5, 10, 15, 127}, ys[5] = {0.625, 0.429, 0.168, 0.153, 0.153};
	return interp(xs, ys, 5, d);
}

inline double edge_p(double e)
{
	static const double xs[4] = {0, 64, 112, 127}, ys[4] = {std::log(2.0), std::log(4.0), std::log(32.0), std::log(32.0)};
	return std::exp(interp(xs, ys, 4, e));
}

inline bool dist_type(const block &b) { return b.is(9, 48) || b.is(9, 49) || b.is(9, 0) || b.is(9, 16) || b.is(9, 1) || b.is(9, 32); }

// V-Distortion の中のディレイ（遅れ・帰り・大きさを 01_049・01_068 で確かめた）: 帰りの線（遅れ p8 × 0.1ms、帰り (p9 − 64) / 64、戻りに 1 次の LPF (1 − p12 / 10)²）から、
// 左は p6・右は p7（× 0.1ms）で取り出し、Delay Mix（p11 / 95）で足す
inline void vdist_delay(buf &y, const block &b, int ch)
{
	const size_t n = y.size();
	const size_t dt = size_t(std::max(1, b.p[ch == 0 ? 6 : 7])) * FS / 10000, df = size_t(std::max(1, b.p[8])) * FS / 10000;
	const double fb = (b.p[9] - 64) / 64.0, a = std::pow(1 - std::max(1, b.p[12]) / 10.0, 2), mix = b.p[11] / 95.0;   // Delay Mix 30 の 2 組で、こだまが元の音より −10dB
	buf line(n, 0.0);
	double lp = 0;
	for (size_t t = 0; t < n; t++) {
		const double back = t >= df ? line[t - df] : 0.0;
		lp = (1 - a) * back + a * lp;
		line[t] = y[t] + fb * lp;
	}
	for (size_t t = n; t-- > 0;)
		y[t] += mix * (t >= dt ? line[t - dt] : 0.0);
}

inline void distortion(buf &L, buf &R, const block &b, const std::string &fxddir)
{
	static const dist_set amp[] = {{1, 30, "insa_t9_48_lev16"}, {0, 30, "insa_t9_48_amptype0_lev16"}, {2, 30, "insa_t9_48_amptype2_lev16"},
		{3, 30, "insa_t9_48_amptype3_lev16"}, {1, 0, "insa_t9_48_drive0_lev16"}, {1, 64, "insa_t9_48_drive64_lev16"},
		{1, 5, "insa_t9_48_drive5_lev16"}, {1, 10, "insa_t9_48_drive10_lev16"}, {1, 15, "insa_t9_48_drive15_lev16"},
		{1, 20, "insa_t9_48_drive20_lev16"}};
	const int NAMP = int(sizeof amp / sizeof amp[0]);
	std::string name;
	int drive_s;
	if (b.is(9, 48)) {
		int best = -1;
		long key = 1L << 30;
		for (int i = 0; i < NAMP; i++) {
			const long k2 = (amp[i].amp != b.p[2]) * 1000L + std::abs(amp[i].drive - b.p[1]);
			if (k2 < key) {
				key = k2;
				best = i;
			}
		}
		name = amp[best].name;
		drive_s = amp[best].drive;
	} else {
		name = b.is(9, 49) ? "insa_t9_49_amp2def_lev16" : b.is(9, 0) ? "insa_t9_0_distdef_lev16" : "insa_t9_16_vdistdef_lev16";
		drive_s = -1;
	}
	// プリセットの組のまま録ったもの（tools/motif/mr_fx_dist_sets.py、fxds/<名前>.dsx）があれば、それをそのまま使う
	bool per_set = false;
	stereo_ir ir;
	if (!b.is(9, 48)) {      // Amp Simulator は式のまま（組のまま録ったものより 3 音色で少し合った）
		std::string k = "t" + std::to_string(b.msb) + "_" + std::to_string(b.lsb);
		for (int i = 1; i <= 16; i++)
			k += "_" + std::to_string(i == 10 ? 127 : (i == 11 && b.is(9, 16)) ? 0 : b.p[i]);
		ir = load_ir(fxddir + "/../fxds/" + k + ".dsx");
		per_set = ir.ok;
	}
	if (!per_set) {
		if (b.is(9, 1) || b.is(9, 32))
			name = "insa_t9_0_distdef_lev16";        // 組の録音が無いときは Distortion の既定値の組で代える
		ir = load_ir(fxddir + "/" + name + ".dsx");
	}
	if (!ir.ok)
		return;
	if (drive_s < 0)
		drive_s = std::atoi(ir.head["drive"].c_str());
	double c = std::atof(ir.head["c"].c_str()), p = std::atof(ir.head["p"].c_str());
	if (b.is(9, 48) && !per_set) {
		c = drive_clip(b.p[1]);
		if (b.p[1] >= 15)
			p = edge_p(b.p[11]);
	}
	double fc = std::atof(ir.head["fc"].c_str());
	if ((b.is(9, 48) || b.is(9, 49)) && b.p[3] > 0 && !per_set)
		fc = t3(b.p[3]);
	const double k = per_set ? 1.0 : std::pow(10.0, (drive_gain_db(b.p[1]) - drive_gain_db(drive_s)) / 20);
	double og = 1.0;
	if (b.is(9, 48) && !per_set)
		og = std::pow(10.0, 40 * std::log10(std::max(1, b.p[4]) / 80.0) / 20);
	const double ang = PI / 2 * (std::max(1, b.p[10]) - 1) / 126.0;  // Dry/Wet: 元の音 cos・wet sin（Dry/Wet 20・64 で確かめた）
	size_t pk = 0;
	for (size_t i = 0; i < ir.L.size(); i++)
		if (std::fabs(ir.L[i]) > std::fabs(ir.L[pk]))
			pk = i;
	// ブロック全体（元の音の側も）が負の向きで出る組がある。元の音の向きを h の山の向きにそろえる
	const double gdry = (ir.L.empty() || ir.L[pk] >= 0 ? 1.0 : -1.0) * std::cos(ang), gwet = std::sin(ang);
	const size_t n = L.size();
	buf x(n);
	for (size_t i = 0; i < n; i++)
		x[i] = (L[i] + R[i]) / 2;
	for (int ch = 0; ch < 2; ch++) {
		buf u = convolve(x, ch == 0 ? ir.L : ir.R, n);
		for (auto &v : u) {
			v *= k;
			v = v / std::pow(1 + std::pow(std::fabs(v / c), p), 1 / p);
		}
		buf y = filter_freq(u, n, [fc](double f) { return 1.0 / cplx(1 - (f / fc) * (f / fc), (f / fc) / 1.6); });
		if (b.is(9, 16) && b.p[11] > 0)
			vdist_delay(y, b, ch);
		buf &o = ch == 0 ? L : R;
		for (size_t i = 0; i < n; i++)
			o[i] = gdry * o[i] + gwet * y[i] * og;
	}
}

// Cross Delay（02 30）: 左の入力 →（左 → 右、p1 × 0.1ms）→ 右の出力、右の入力 →（右 → 左、p2）→ 左の出力。出たものは反対側の線へ
// フィードバック (p3 − 64) / 64 を掛け、1 次の LPF（係数 (1 − p5 / 10)²）を通して戻る。入力の選び方 p4: 0 = L、1 = R、2 = 両方。
// Dry/Wet p10: 元の音 min(1, (127 − v) / 63)²、こだま min(1, (v − 1) / 63)²
// Tempo Cross Delay（02 60、tempo > 0）: 遅れは表 14 の音符 × 60 / テンポ + Lag（p6、(v − 64) ms）、混ぜ方はエネルギーを保つ形
// （元の音 cos、こだま sin、角 π/2 × (v − 1) / 126）
static const double TABLE14_Q[20] = {1.0 / 12, 3.0 / 32, 1.0 / 8, 1.0 / 6, 3.0 / 16, 1.0 / 4, 1.0 / 3, 3.0 / 8, 1.0 / 2, 2.0 / 3, 3.0 / 4, 1,
	4.0 / 3, 3.0 / 2, 2, 8.0 / 3, 3, 4, 5, 6};

inline void cross_delay(buf &L, buf &R, const block &b, int tempo = 0)
{
	const size_t n = L.size();
	size_t d1 = std::max<size_t>(1, size_t(std::lround(b.p[1] * 0.1e-3 * FS)));
	size_t d2 = std::max<size_t>(1, size_t(std::lround(b.p[2] * 0.1e-3 * FS)));
	if (tempo > 0) {
		const double lag = (b.p[6] - 64) * 1e-3, q = 60.0 / tempo;
		d1 = std::max<size_t>(1, size_t(std::lround((TABLE14_Q[std::min(19, b.p[1])] * q + lag) * FS)));
		d2 = std::max<size_t>(1, size_t(std::lround((TABLE14_Q[std::min(19, b.p[2])] * q + lag) * FS)));
	}
	const double fb = (b.p[3] - 64) / 64.0;
	const int sel = b.p[4];
	const double a = std::pow(1 - std::max(1, b.p[5]) / 10.0, 2);
	const int v = std::max(1, b.p[10]);
	double gd = std::pow(std::min(1.0, (127 - v) / 63.0), 2), gw = std::pow(std::min(1.0, (v - 1) / 63.0), 2);
	if (tempo > 0) {
		gd = std::cos(PI / 2 * (v - 1) / 126.0);
		gw = std::sin(PI / 2 * (v - 1) / 126.0);
	}
	buf lr(n, 0.0), rl(n, 0.0), lr_in(n, 0.0), rl_in(n, 0.0);
	double plr = 0, prl = 0;
	for (size_t t = 0; t < n; t++) {
		if (t >= d1)
			lr[t] = lr_in[t - d1];
		if (t >= d2)
			rl[t] = rl_in[t - d2];
		plr = (1 - a) * lr[t] + a * plr;
		prl = (1 - a) * rl[t] + a * prl;
		lr_in[t] = (sel == 0 || sel == 2 ? L[t] : 0.0) + fb * prl;
		rl_in[t] = (sel == 1 || sel == 2 ? R[t] : 0.0) + fb * plr;
	}
	for (size_t t = 0; t < n; t++) {
		L[t] = gd * L[t] + gw * rl[t];
		R[t] = gd * R[t] + gw * lr[t];
	}
}

// コーラス系と Ensemble Detune の Input Mode（p15）: 0 = mono（左右を足して半分）、1 = stereo（左右それぞれ）
inline void input_mode(const buf &L, const buf &R, const block &b, buf xs[2])
{
	if (b.p[15] == 1) {
		xs[0] = L, xs[1] = R;
		return;
	}
	xs[0].resize(L.size());
	for (size_t i = 0; i < L.size(); i++)
		xs[0][i] = (L[i] + R[i]) / 2;
	xs[1] = xs[0];
}

// 揺れる系のエフェクトの EQ（p6 低の周波数・p7 低の利き・p8 高の周波数・p9 高の利き）。2 Band EQ と同じ棚を、元の音も含めた出口に掛ける
inline void fx_eq(buf &L, buf &R, const block &b, int base = 6)
{
	if (b.p[base + 1] == 64 && b.p[base + 3] == 64)
		return;
	block q;
	q.p[1] = b.p[base], q.p[2] = b.p[base + 1], q.p[3] = b.p[base + 2], q.p[4] = b.p[base + 3];
	L = filter_freq(L, L.size(), [&](double f) { return eq2(q, f); });
	R = filter_freq(R, R.size(), [&](double f) { return eq2(q, f); });
}

// インサーションのコーラス: 遅れの動きはシステムのコーラスと同じ。元の音 cos(π/2 (v − 1) / 126)、コーラスの線のエネルギーは
// 32・64・127 で 0.364・0.838・1.5 を補間（v = Dry/Wet）
inline void insertion_chorus(buf &L, buf &R, const block &b)
{
	const int v = std::max(1, b.p[10]);
	static const double vx[4] = {1, 32, 64, 127}, vy[4] = {0.0, 0.364, 0.838, 1.5};
	const double gd = std::cos(PI / 2 * (v - 1) / 126.0), gw = std::sqrt(interp(vx, vy, 4, v));
	const double hz = LFO_HZ[std::min(127, b.p[1])], depth = 0.176 * b.p[2] / 1000 * FS, off = MOD_DELAY_MS[std::min(127, b.p[4])] / 1000 * FS;
	const size_t n = L.size();
	buf xs[2];
	input_mode(L, R, b, xs);
	for (size_t t = 0; t < n; t++) {
		const double ph = std::fmod(double(t) * hz / FS, 1.0), tri = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
		double o[2] = {0, 0};
		for (int c = 0; c < 2; c++) {
			const double pos = double(t) - (off + depth * (c == 0 ? tri : 1 - tri));
			if (pos < 0)
				continue;
			const buf &x = xs[c];
			const size_t i = size_t(pos);
			const double fr = pos - double(i), a0 = x[i], a1 = i + 1 < n ? x[i + 1] : a0;
			o[c] = a0 + (a1 - a0) * fr;
		}
		L[t] = gd * L[t] + gw * o[0];
		R[t] = gd * R[t] + gw * o[1];
	}
	fx_eq(L, R, b);
}

// フランジャー（Flanger 1〜3、06 00・01）: 左右それぞれ y = x + 帰り × 遅れた y。遅れ = 表 2[p4] + p2 × 0.0914ms × 三角波（表 1[p1]）、
// 右は (p14 − 64) × 3° ずらす。帰り (p3 − 64) / 64。元の音 cos、揺れの線 sin（角 π/2 × (v − 1) / 126）
inline bool flanger_type(const block &b) { return b.is(6, 0) || b.is(6, 1); }

inline void flanger(buf &L, buf &R, const block &b)
{
	const int v = std::max(1, b.p[10]);
	const double gd = std::cos(PI / 2 * (v - 1) / 126.0), gw = std::sin(PI / 2 * (v - 1) / 126.0);
	const double hz = LFO_HZ[std::min(127, b.p[1])], depth = 0.0914 * b.p[2] / 1000 * FS, off = MOD_DELAY_MS[std::min(127, b.p[4])] / 1000 * FS;
	const double fb = (b.p[3] - 64) / 64.0, dph = (b.p[14] - 64) * 3.0 / 360.0;
	const size_t n = L.size();
	buf x(n), y(n), w[2] = {buf(n), buf(n)};
	for (size_t i = 0; i < n; i++)
		x[i] = (L[i] + R[i]) / 2;
	for (int c = 0; c < 2; c++) {
		const double ph0 = c == 0 ? 0.0 : dph;
		for (size_t t = 0; t < n; t++) {
			double ph = std::fmod(double(t) * hz / FS + ph0, 1.0);
			if (ph < 0)
				ph += 1.0;
			const double pos = double(t) - (off + depth * (ph < 0.5 ? 2 * ph : 2 - 2 * ph));
			double o = 0;
			if (pos >= 0) {
				const size_t k = size_t(pos);
				const double fr = pos - double(k);
				o = (1 - fr) * y[k] + (k + 1 < t ? fr * y[k + 1] : 0.0);
			}
			w[c][t] = o;
			y[t] = x[t] + fb * o;
		}
	}
	for (size_t t = 0; t < n; t++) {
		L[t] = gd * L[t] + gw * w[0][t];
		R[t] = gd * R[t] + gw * w[1][t];
	}
	fx_eq(L, R, b);
}

// Harmonic Enhancer（0F 10）: 1 + g × HPF（3 次のバターワース、双一次変換、角は表 3[p1]）、g = 8.13 × p2 / 64 × p3 / 64。出口は ±1.0 で頭打ち
inline void enhancer(buf &L, buf &R, const block &b)
{
	const double fc = std::min(t3(b.p[1]), FS * 0.49), g = 8.13 * b.p[2] / 64.0 * b.p[3] / 64.0;
	auto H = [&](double f) {
		const double fa = std::min(std::fabs(f), FS * 0.4999);
		const cplx s(0, std::tan(PI * fa / FS) / std::tan(PI * fc / FS));
		cplx h = 1.0 + g * (s / (s + 1.0)) * (s * s / (s * s + s * std::sqrt(2.0) + 1.0));
		return f < 0 ? std::conj(h) : h;
	};
	L = filter_freq(L, L.size(), H);
	R = filter_freq(R, R.size(), H);
	for (size_t i = 0; i < L.size(); i++) {
		L[i] = std::max(-1.0, std::min(1.0, L[i]));
		R[i] = std::max(-1.0, std::min(1.0, R[i]));
	}
}

// Ensemble Detune（05 30）: 左は (p1 − 64) セント下げ、右は上げる遅れ線。遅れはのこぎり（1.485 秒で 1 周）で 0.25〜1.25 × 幅を回り
// （幅 = 速さの差 × 1.485 秒）、最後の 1/4 で近い側から入る 2 本目へまっすぐ移る。左右は半周ずれる。p2・p3 Initial Delay（表 2）を足す。元の音 cos・wet sin。EQ はこの型では p11〜14
inline void ensemble_detune(buf &L, buf &R, const block &b)
{
	const double hz = 1 / 1.485;
	const int v = std::max(1, b.p[10]);
	const double gd = std::cos(PI / 2 * (v - 1) / 126.0), gw = std::sin(PI / 2 * (v - 1) / 126.0);
	const int cents = b.p[1] - 64;
	const size_t n = L.size();
	buf xs[2], w[2] = {buf(n), buf(n)};
	input_mode(L, R, b, xs);
	auto at = [&](const buf &x, double pos) {
		if (pos < 0)
			return 0.0;
		const size_t k = size_t(pos);
		const double fr = pos - double(k);
		return k + 1 < n ? x[k] + (x[k + 1] - x[k]) * fr : (k < n ? x[k] : 0.0);
	};
	for (int c = 0; c < 2; c++) {
		const double cc = c == 0 ? -cents : cents, ph0 = c == 0 ? 0.0 : 0.5;
		const double init = MOD_DELAY_MS[std::min(127, b.p[c == 0 ? 2 : 3])] / 1000 * FS;
		const double rate = 1 - std::exp2(cc / 1200.0), span = std::fabs(rate) / hz * FS;
		for (size_t t = 0; t < n; t++) {
			if (span < 1e-9) {
				w[c][t] = at(xs[c], double(t) - init);
				continue;
			}
			const double sw = std::fmod(double(t) * hz / FS + ph0, 1.0), s1 = rate > 0 ? sw : 1 - sw;
			const double d1 = init + span * (0.25 + s1), d2 = rate > 0 ? init + span * (s1 - 0.75) : init + span * (s1 + 1.25);
			const double u = std::min(1.0, std::max(0.0, (sw - 0.75) / 0.25));
			w[c][t] = (1 - u) * at(xs[c], double(t) - d1) + u * at(xs[c], double(t) - d2);
		}
	}
	for (size_t t = 0; t < n; t++) {
		L[t] = gd * L[t] + gw * w[0][t];
		R[t] = gd * R[t] + gw * w[1][t];
	}
	fx_eq(L, R, b, 11);
}

// Pitch Change 1（0B 00）: 2 本の音程変え。Fine → セントは測った表（まっすぐでない）、遅れの下限と幅は |Fine| の表（音程 0 のとき）。
// のこぎりと最後の 1/4 の受け渡しは Ensemble Detune と同じ形（確かめていない）。1 本目は p11 Pan・p12 Level、2 本目は p13・p14。元の音 cos・wet sin
inline void pitch_change(buf &L, buf &R, const block &b)
{
	static const double fx[15] = {-50, -10, -3, 0, 3, 5, 6, 7, 8, 10, 15, 20, 30, 40, 50},
			    fy[15] = {-26.2, -10.79, -5.51, 0.0, 2.48, 5.13, 5.47, 7.88, 7.58, 10.59, 10.22, 10.06, 18.26, 23.77, 26.15};
	static const double gx[4] = {3, 5, 10, 50}, glo[4] = {2.1, 3.2, 4.6, 13.0}, gsp[4] = {11.4, 12.7, 16.7, 29.8};
	const int v = std::max(1, b.p[10]);
	const double gd = std::cos(PI / 2 * (v - 1) / 126.0), gw = std::sin(PI / 2 * (v - 1) / 126.0);
	const size_t n = L.size();
	buf x(n), oL(n, 0.0), oR(n, 0.0);
	for (size_t i = 0; i < n; i++)
		x[i] = (L[i] + R[i]) / 2;
	auto at = [&](double pos) {
		if (pos < 0)
			return 0.0;
		const size_t k = size_t(pos);
		const double fr = pos - double(k);
		return k + 1 < n ? x[k] + (x[k + 1] - x[k]) * fr : (k < n ? x[k] : 0.0);
	};
	const int semi = b.p[1] - 64;
	for (int c = 0; c < 2; c++) {
		const double fine = b.use_pfine ? b.pfine[c == 0 ? 3 : 4] - 64 : b.p[c == 0 ? 3 : 4] - 64;
		const int pan = std::max(1, b.p[c == 0 ? 11 : 13]), lvl = b.p[c == 0 ? 12 : 14];
		const double ph0 = c == 0 ? 0.0 : 0.5, cents = 100.0 * semi + interp(fx, fy, 15, fine), rate = 1 - std::exp2(cents / 1200.0);
		double lo, span;
		if (semi == 0) {
			const double af = std::min(50.0, std::max(3.0, std::fabs(fine)));
			lo = interp(gx, glo, 4, af) / 1000 * FS;
			span = interp(gx, gsp, 4, af) / 1000 * FS;
		} else {
			span = 1280.0;
			lo = span / 4;
		}
		const double a = PI / 2 * (pan - 1) / 126.0, g = lvl / 127.0, gl = g * std::cos(a), gr = g * std::sin(a);
		const double hz = std::fabs(rate) * FS / span;
		for (size_t t = 0; t < n; t++) {
			double y;
			if (std::fabs(rate) < 1e-9) {
				y = at(double(t) - lo);
			} else {
				const double sw = std::fmod(double(t) * hz / FS + ph0, 1.0), s1 = rate > 0 ? sw : 1 - sw;
				const double d1 = lo + span * s1, d2 = std::max(0.0, rate > 0 ? lo + span * (s1 - 1) : lo + span * (s1 + 1));
				const double u = std::min(1.0, std::max(0.0, (sw - 0.75) / 0.25));
				y = (1 - u) * at(double(t) - d1) + u * at(double(t) - d2);
			}
			oL[t] += y * gl;
			oR[t] += y * gr;
		}
	}
	for (size_t t = 0; t < n; t++) {
		L[t] = gd * L[t] + gw * oL[t];
		R[t] = gd * R[t] + gw * oR[t];
	}
}

// 時間で変わらない型（ディレイ類・ER・ゲート・Karaoke・インサーションのリバーブ・Ambience）は、プリセットの組のまま録った 2×2 の応答
// （tools/motif/mr_fx_lti.py、fxlti/<名前>.lti）で畳み込む。名前は型と 16 個のパラメーター（テンポに合わせる型はテンポも）
inline bool lti_type(const block &b)
{
	static const int t[][2] = {{2, 0x00}, {2, 0x10}, {2, 0x18}, {2, 0x20}, {2, 0x40}, {2, 0x48}, {2, 0x50}, {2, 0x58}, {1, 0x00}, {1, 0x01}, {1, 0x10},
		{1, 0x11}, {1, 0x12}, {1, 0x20}, {1, 0x21}, {1, 0x30}, {1, 0x40}, {1, 0x41}, {1, 0x42}, {1, 0x43}, {1, 0x02}, {1, 0x13}, {1, 0x31}, {1, 0x08},
		{1, 0x09}, {1, 0x18}, {1, 0x19}, {1, 0x38}, {3, 0x00}, {3, 0x01}, {3, 0x10}, {3, 0x20}, {4, 0x00}, {4, 0x01}, {4, 0x02}, {15, 0x30}, {14, 0x40}};
	for (auto &x : t)
		if (b.is(x[0], x[1]))
			return true;
	return false;
}

inline bool lti(buf &L, buf &R, const block &b, const std::string &ltidir, int tempo)
{
	if (!b.is(14, 0x40) && b.p[10] <= 1)          // Dry/Wet 1 は素通し（Isolator は Dry/Wet の欄が無い）
		return true;
	std::string k = "t" + std::to_string(b.msb) + "_" + std::to_string(b.lsb);
	for (int i = 1; i <= 16; i++)
		k += "_" + std::to_string(b.p[i]);
	k += "_" + std::to_string(b.is(2, 0x50) || b.is(2, 0x58) ? tempo : 0);
	FILE *f = std::fopen((ltidir + "/" + k + ".lti").c_str(), "rb");
	if (!f)
		return false;
	char head[128];
	if (!std::fgets(head, sizeof head, f)) {
		std::fclose(f);
		return false;
	}
	size_t frames = 0;
	std::sscanf(head, "LTI frames=%zu", &frames);
	std::vector<float> raw(frames * 4);
	frames = std::fread(raw.data(), sizeof(float) * 4, frames, f);
	std::fclose(f);
	buf h[4];
	for (int c = 0; c < 4; c++) {
		h[c].resize(frames);
		for (size_t i = 0; i < frames; i++)
			h[c][i] = raw[i * 4 + c];
	}
	const size_t n = L.size();
	const buf a = convolve(L, h[0], n), bb = convolve(R, h[2], n), c = convolve(L, h[1], n), d = convolve(R, h[3], n);
	for (size_t i = 0; i < n; i++) {
		L[i] = a[i] + bb[i];
		R[i] = c[i] + d[i];
	}
	return true;
}

// 音量や遅れを周期的に揺らす型（Tremolo・Auto Pan・Rotary・Dual Rotor・Slice）は、プリセットの組のまま録った A(t)・d(t)
// （tools/motif/mr_fx_mod.py、fxmod/<名前>.mod）を、1kHz で分けた 2 つの帯に掛ける
inline bool mod_type(const block &b)
{
	static const int t[][2] = {{8, 0x00}, {8, 0x10}, {8, 0x11}, {8, 0x50}, {8, 0x51}, {8, 0x52}, {8, 0x53}, {8, 0x60}, {8, 0x61}, {8, 0x62},
		{8, 0x63}, {8, 0x70}, {8, 0x71}, {8, 0x72}, {14, 0x50}};
	for (auto &x : t)
		if (b.is(x[0], x[1]))
			return true;
	return false;
}

// 揺れる型の EQ（mr_fx_mod.eq_response と同じ）。has に有無を返す
inline cplx mod_eq(const block &b, double f, bool &has)
{
	int base = 0;
	bool mid = false;
	if (b.is(8, 0x00) || b.is(8, 0x10) || b.is(8, 0x11) || b.is(8, 0x50) || b.is(8, 0x60))
		base = 6, mid = true;
	else if (b.is(8, 0x70) || b.is(8, 0x71) || b.is(8, 0x72))
		base = 11;
	else if (b.is(14, 0x50))
		base = 13;
	has = base != 0;
	if (!has)
		return 1.0;
	block q;
	q.p[1] = b.p[base], q.p[2] = b.p[base + 1], q.p[3] = b.p[base + 2], q.p[4] = b.p[base + 3];
	cplx H = eq2(q, f);
	if (mid) {
		block m;
		m.p[1] = 64, m.p[5] = 64, m.p[2] = b.p[11], m.p[3] = b.p[12], m.p[4] = b.p[13];
		H *= eq3(m, f);
	}
	return H;
}

struct mod_file {
	int rate = 1000, frames = 0, period = 0;
	double indb = 0;
	std::vector<float> raw;
};

inline bool load_mod(const std::string &path, mod_file &m)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char head[160];
	if (!std::fgets(head, sizeof head, f)) {
		std::fclose(f);
		return false;
	}
	m.indb = 0;
	std::sscanf(head, "MOD rate=%d frames=%d period=%d indb=%lf", &m.rate, &m.frames, &m.period, &m.indb);
	m.raw.resize(size_t(m.frames) * 8);
	m.frames = int(std::fread(m.raw.data(), sizeof(float) * 8, size_t(m.frames), f));
	std::fclose(f);
	return m.frames >= 2;
}

// 揺れる型の芯: 1kHz で分けた 2 つの帯に、録った A(t)・d(t) を掛ける。nrm は帯ごとの周波数の形の大きさ（220Hz・2093Hz での値）で A を割る
inline void mod_core(const buf xb2[2][2], const mod_file &m, const double nrm[2][2], buf out[2])
{
	const size_t n = xb2[0][0].size();
	auto series = [&](int col, double tt) {
		const double mm = m.frames;
		double u = tt / FS * m.rate;
		if (m.period > 0 && u >= mm - 1)
			u = (mm - 1 - m.period) + std::fmod(u - (mm - 1 - m.period), double(m.period));
		else if (u > mm - 1)
			u = mm - 1;
		const int i = int(u);
		const double fr = u - i;
		const float a0 = m.raw[size_t(i) * 8 + col], a1 = m.raw[size_t(std::min(i + 1, m.frames - 1)) * 8 + col];
		return a0 + (a1 - a0) * fr;
	};
	out[0].assign(n, 0.0);
	out[1].assign(n, 0.0);
	for (size_t t = 0; t < n; t++)
		for (int bd = 0; bd < 2; bd++)
			for (int ch = 0; ch < 2; ch++) {
				const double A = series(bd * 4 + ch * 2, double(t)) / nrm[bd][ch], d = series(bd * 4 + ch * 2 + 1, double(t));
				const double pos = double(t) - d;
				if (pos < 0)
					continue;
				const size_t k0 = size_t(pos);
				const double fr = pos - double(k0);
				const buf &xb = xb2[ch][bd];
				const double v = k0 + 1 < n ? xb[k0] + (xb[k0 + 1] - xb[k0]) * fr : (k0 < n ? xb[k0] : 0.0);
				out[ch][t] += A * v;
			}
}

inline bool modfx(buf &L, buf &R, const block &b, const std::string &moddir, int tempo)
{
	std::string k = "t" + std::to_string(b.msb) + "_" + std::to_string(b.lsb);
	for (int i = 1; i <= 16; i++)
		k += "_" + std::to_string(b.p[i]);
	k += "_" + std::to_string(b.is(14, 0x50) ? tempo : 0);
	mod_file m100;
	if (!load_mod(moddir + "/" + k + ".mod", m100))
		return false;
	const size_t n = L.size();
	// 左右を別々に通す（Rotary・Dual Rotor はスピーカーへ足して送る形なので左右を足す）
	const bool stereo = !(b.msb == 8 && b.lsb >= 0x50 && b.lsb <= 0x72);
	buf xs[2] = {buf(L.begin(), L.end()), buf(R.begin(), R.end())};
	if (!stereo) {
		for (size_t i = 0; i < n; i++)
			xs[0][i] = (L[i] + R[i]) / 2;
		xs[1] = xs[0];
	}
	// 周波数の形: ノイズで録った時間平均の応答（fxmod/<名前>.modeq、左右 2049 点）があればそれ、無ければパラメーターの EQ
	std::vector<float> eqm;
	{
		FILE *f = std::fopen((moddir + "/" + k + ".modeq").c_str(), "rb");
		if (f) {
			eqm.resize(2 * 2049);
			if (std::fread(eqm.data(), sizeof(float), eqm.size(), f) != eqm.size())
				eqm.clear();
			std::fclose(f);
		}
	}
	bool has_eq = false;
	mod_eq(b, 1000.0, has_eq);
	const bool has_shape = !eqm.empty() || has_eq;
	auto shape = [&](double fq, int ch) -> cplx {
		if (!eqm.empty()) {
			const double x = fq / (FS / 4096.0);
			const int i = std::max(0, std::min(2047, int(x)));
			const double fr = std::max(0.0, std::min(1.0, x - i));
			return eqm[ch * 2049 + i] + (eqm[ch * 2049 + i + 1] - eqm[ch * 2049 + i]) * fr;
		}
		bool h;
		return mod_eq(b, fq, h);
	};
	double nrm[2][2] = {{1, 1}, {1, 1}};
	if (has_shape)
		for (int ch = 0; ch < 2; ch++) {
			nrm[0][ch] = std::max(1e-3, std::abs(shape(220.0, ch)));
			nrm[1][ch] = std::max(1e-3, std::abs(shape(440.0 * std::exp2(27 / 12.0), ch)));
		}
	// 1kHz で 2 つに分ける（2 次のバターワースを 2 回）
	auto band = [&](const buf &x, bool low) {
		return filter_freq(x, n, [low](double fq) {
			const cplx s(0, std::tan(PI * std::min(fq, FS * 0.4999) / FS) / std::tan(PI * 1000.0 / FS));
			const cplx den = s * s + std::sqrt(2.0) * s + 1.0;
			const cplx h = low ? 1.0 / den : s * s / den;
			return h * h;
		});
	};
	const buf xb2[2][2] = {{band(xs[0], true), band(xs[0], false)}, {band(xs[1], true), band(xs[1], false)}};
	buf out[2];
	// 歪みを中に持つ Rotary（08 51〜53・61〜63）は音量 100・25 で録った 2 つを、入口の包絡（50ms の RMS）で混ぜる
	mod_file m25;
	const bool lv = b.msb == 8 && ((b.lsb >= 0x51 && b.lsb <= 0x53) || (b.lsb >= 0x61 && b.lsb <= 0x63)) &&
			load_mod(moddir + "/" + k + "_L25.mod", m25) && m25.indb != m100.indb;
	if (lv) {
		buf o100[2], o25[2];
		mod_core(xb2, m100, nrm, o100);
		mod_core(xb2, m25, nrm, o25);
		const mod_file &lo = m25.indb < m100.indb ? m25 : m100;
		const mod_file &hi = m25.indb < m100.indb ? m100 : m25;
		const buf *olo = m25.indb < m100.indb ? o25 : o100, *ohi = m25.indb < m100.indb ? o100 : o25;
		std::vector<double> c(n + 1, 0.0);
		for (size_t i = 0; i < n; i++) {
			const double mo = (xs[0][i] + xs[1][i]) / 2;
			c[i + 1] = c[i] + mo * mo;
		}
		const size_t w = size_t(0.05 * FS);
		out[0].assign(n, 0.0);
		out[1].assign(n, 0.0);
		for (size_t i = 0; i < n; i++) {
			const size_t a = i >= w / 2 ? i - w / 2 : 0, z = std::min(n, i + w / 2);
			const double e = 10 * std::log10((c[z] - c[a]) / double(w) + 1e-20);
			const double u = std::max(0.0, std::min(1.0, (e - lo.indb) / (hi.indb - lo.indb)));
			for (int ch = 0; ch < 2; ch++)
				out[ch][i] = (1 - u) * olo[ch][i] + u * ohi[ch][i];
		}
	} else
		mod_core(xb2, m100, nrm, out);
	L = out[0];
	R = out[1];
	if (has_shape) {
		L = filter_freq(L, n, [&](double fq) { return shape(fq, 0); });
		R = filter_freq(R, n, [&](double fq) { return shape(fq, 1); });
	}
	return true;
}

// 揺れながら周波数ごとに違う動きをする型（フェイザー類・V-Flanger・Vintage Flanger・Tempo Flanger・Flanger 3・Symphonic・Auto Wah など）は、
// プリセットの組のまま録った時間ごとの周波数応答（tools/motif/mr_fx_ltv.py、fxltv/<名前>.ltv）を、窓 4096・ずらし 1024 の STFT で掛ける
inline bool ltv_type(const block &b)
{
	static const int t[][2] = {{5, 0x00}, {5, 0x01}, {5, 0x02}, {5, 0x08}, {5, 0x09}, {5, 0x10}, {5, 0x11}, {5, 0x12}, {5, 0x18}, {6, 0x00}, {6, 0x01},
		{7, 0x00}, {7, 0x08}, {7, 0x20}, {7, 0x21}, {7, 0x30}, {7, 0x31}, {7, 0x50}, {6, 0x08}, {6, 0x10}, {6, 0x20},
		{6, 0x21}, {6, 0x22}, {6, 0x50}, {5, 0x20}, {12, 0x00}, {12, 0x20}, {12, 0x21}, {14, 0x30}, {15, 0x40}, {14, 0x20},
		{12, 0x10}, {12, 0x11}, {12, 0x12}, {12, 0x30}, {12, 0x31}, {15, 0x78}, {6, 0x60}, {7, 0x60}, {10, 0x50}, {10, 0x51}, {14, 0x10}, {12, 0x01}, {12, 0x02}, {12, 0x40}, {12, 0x41}, {10, 0x40},
		{10, 0x41}, {13, 0x10}, {13, 0x20}, {13, 0x30}};
	for (auto &x : t)
		if (b.is(x[0], x[1]))
			return true;
	return false;
}

inline bool ltvfx(buf &L, buf &R, const block &b, const std::string &ltvdir, int tempo)
{
	std::string k = "t" + std::to_string(b.msb) + "_" + std::to_string(b.lsb);
	for (int i = 1; i <= 16; i++)
		k += "_" + std::to_string(b.p[i]);
	k += "_" + std::to_string(b.is(7, 0x50) || b.is(6, 0x50) || b.is(14, 0x30) ? tempo : 0);
	FILE *f = std::fopen((ltvdir + "/" + k + ".ltv").c_str(), "rb");
	if (!f)
		return false;
	char head[160];
	if (!std::fgets(head, sizeof head, f)) {
		std::fclose(f);
		return false;
	}
	int NW = 4096, HOP = 1024, F = 0, per = 0;
	std::sscanf(head, "LTV n=%d hop=%d frames=%d period=%d", &NW, &HOP, &F, &per);
	const int NB = NW / 2 + 1;
	std::vector<float> raw(size_t(F) * 2 * NB * 2);
	F = int(std::fread(raw.data(), sizeof(float) * 2 * NB * 2, size_t(F), f));
	std::fclose(f);
	if (F < 1)
		return false;
	const size_t n = L.size();
	// 左右を別々に通す（Dynamic Filter だけは足す方が実機に合う）
	const bool stereo = !b.is(15, 0x78) && !(chorus_type(b) && b.p[15] == 0);   // コーラス系は Input Mode p15 が 0 なら mono
	// 頭の前に窓 1 枚ぶんの無音を足す（最初の枠では窓の重なりの合計がほぼ 0 で割り算が暴れる）。出口は NW ずらして取る
	buf xs2[2] = {buf(n + 2 * NW, 0.0), buf(n + 2 * NW, 0.0)};
	for (size_t i = 0; i < n; i++) {
		xs2[0][NW + i] = stereo ? L[i] : (L[i] + R[i]) / 2;
		xs2[1][NW + i] = stereo ? R[i] : (L[i] + R[i]) / 2;
	}
	const buf &x = xs2[0];
	std::vector<double> w(NW);
	for (int i = 0; i < NW; i++)
		w[i] = 0.5 - 0.5 * std::cos(2 * PI * i / NW);
	buf out[2] = {buf(n + 3 * NW, 0.0), buf(n + 3 * NW, 0.0)}, norm(n + 3 * NW, 0.0);
	const int nf = 1 + int((x.size() - NW) / HOP);
	std::vector<cplx> X(NW), Y(NW);
	for (int i = 0; i < nf; i++) {
		int j = i;
		if (i >= F && per > 0)
			j = (F - 1 - per) + (i - (F - 1 - per)) % per;
		j = std::min(j, F - 1);
		for (int ch = 0; ch < 2; ch++) {
			for (int q = 0; q < NW; q++)
				X[q] = xs2[ch][size_t(i) * HOP + q] * w[q];
			fft(X, false);
			const float *h = &raw[((size_t(j) * 2 + ch) * NB) * 2];
			for (int q = 0; q < NB; q++) {
				const cplx hq(h[q * 2], h[q * 2 + 1]);
				Y[q] = X[q] * hq;
				if (q > 0 && q < NW / 2)
					Y[NW - q] = std::conj(Y[q]);
			}
			fft(Y, true);
			for (int q = 0; q < NW; q++)
				out[ch][size_t(i) * HOP + q] += Y[q].real() * w[q];
		}
		for (int q = 0; q < NW; q++)
			norm[size_t(i) * HOP + q] += w[q] * w[q];
	}
	for (size_t t = 0; t < n; t++) {
		const double nn = norm[NW + t] > 1e-9 ? norm[NW + t] : 1.0;
		L[t] = out[0][NW + t] / nn;
		R[t] = out[1][NW + t] / nn;
	}
	return true;
}

// Lo-Fi（0D 00）: 入口の 1 次の HPF（20.5Hz）→ Pre-LPF（双一次変換の 2 次、角は表 3[p4]、Q = p6 / 10）→ 標本化の間引き（p1 + 1 サンプルごとに持ち替え）
// → 語長（1 サンプルの動きの上限）→ Filter Type（p5、録った応答 fxlf/lofi_type<n>.ir）→ Output Gain（(p3 − 6) dB）。元の音 cos・wet sin（確かめていない）
inline void lofi(buf &L, buf &R, const block &b, const std::string &lfdir)
{
	buf xs[2];
	input_mode(L, R, b, xs);
	const size_t n = L.size();
	const double fc = std::min(t3(b.p[4]), FS * 0.49), q = std::max(1, b.p[6]) / 10.0;
	buf h;
	if (b.p[5] >= 1 && b.p[5] <= 5) {
		FILE *f = std::fopen((lfdir + "/lofi_type" + std::to_string(b.p[5]) + ".ir").c_str(), "rb");
		if (f) {
			std::vector<float> raw(4096);
			const size_t m = std::fread(raw.data(), sizeof(float), raw.size(), f);
			std::fclose(f);
			for (size_t i = 0; i < m; i++)          // 山を 64 点目に置いて切ってある。山より前にもエネルギーがあるので頭は落とさない
				h.push_back(raw[i]);
		}
	}
	const double g = std::pow(10.0, (b.p[3] - 6) / 20.0);
	const int k = b.p[1] + 1;
	buf out[2];
	for (int c = 0; c < 2; c++) {
		buf y = filter_freq(xs[c], n, [&](double f) {
			const double ff = std::min(f, FS * 0.4999);
			const cplx sh(0, std::tan(PI * ff / FS) / std::tan(PI * 20.5 / FS)), sl(0, std::tan(PI * ff / FS) / std::tan(PI * fc / FS));
			return (sh / (sh + 1.0)) / (sl * sl + sl / q + 1.0);
		});
		if (k > 1)
			for (size_t i = 0; i < n; i++)
				y[i] = y[i - i % size_t(k)];
		if (b.p[2] < 127) {
			// 語長: 1 サンプルの動きを ±s に切る（s は測った表を対数で補間し、Bit Assign p7 で 2^p7 倍）。Emphasis p8 は前後に 1 次の強調と戻し（係数 0.88）
			static const double wx[8] = {1, 2, 4, 8, 16, 32, 64, 96}, wy[8] = {0.00209, 0.00222, 0.00248, 0.00300, 0.00405, 0.00614, 0.01279, 0.02606};
			double ly[8];
			for (int i = 0; i < 8; i++)
				ly[i] = std::log(wy[i]);
			const double sl = std::exp(interp(wx, ly, 8, b.p[2])) * std::exp2(b.p[7]);
			const double a = b.p[8] ? 0.88 : 0.0;
			double prev = 0, v = 0, acc = 0;
			for (size_t i = 0; i < n; i++) {
				double xi = y[i];
				if (a) {
					const double e = (xi - (i ? a * prev : 0.0)) / (1 - a);
					prev = xi;
					xi = e;
				}
				const double d = xi - v;
				v += d > sl ? sl : (d < -sl ? -sl : d);
				if (a) {
					acc = (1 - a) * v + a * acc;
					y[i] = acc;
				} else
					y[i] = v;
			}
		}
		if (!h.empty())
			y = convolve(y, h, n);
		for (auto &v : y)
			v *= g;
		out[c] = y;
	}
	const int v = std::max(1, b.p[10]);
	const double gd = std::cos(PI / 2 * (v - 1) / 126.0), gw = std::sin(PI / 2 * (v - 1) / 126.0);
	for (size_t i = 0; i < n; i++) {
		L[i] = gd * L[i] + gw * out[0][i];
		R[i] = gd * R[i] + gw * out[1][i];
	}
}

// Ring Modulator（0F 70）: 入口に 1 次の HPF（表 3[p6]）・LPF（表 3[p7]）、搬送波（正弦、振幅 1.07）との掛け算、EQ（p13〜16）、元の音 cos・wet sin。
// 搬送波 = 表 19[p1] + 1.70Hz × p2。LFO（p3 形 0 = 2 値・1 = 正弦、p4 深さ、p5 速さ）は搬送波を 2^(±0.35 × 深さ / 64) 倍に動かす（近似）
static const double TABLE19[128] = {0.5, 1.5, 2.0, 2.5, 3.5, 4.0, 4.5, 5.5, 6.0, 6.5, 7.5, 8.0, 8.5, 8.9, 9.4, 10.0,
	10.6, 11.2, 11.8, 12.5, 13.2, 13.9, 14.7, 15.6, 16.5, 17.4, 18.4, 19.4, 20.6, 21.7, 23.0, 24.3,
	25.7, 27.1, 28.7, 30.3, 32.0, 33.9, 35.8, 37.8, 40.0, 42.3, 44.7, 47.3, 49.9, 52.8, 55.8, 59.0,
	62.4, 65.9, 69.7, 73.7, 77.9, 82.3, 87.0, 92.0, 97.2, 103.0, 109.0, 115.0, 121.0, 128.0, 136.0, 143.0,
	152.0, 160.0, 169.0, 179.0, 189.0, 200.0, 211.0, 224.0, 236.0, 250.0, 264.0, 279.0, 295.0, 312.0, 330.0, 348.0,
	368.0, 389.0, 412.0, 435.0, 460.0, 486.0, 514.0, 543.0, 574.0, 607.0, 642.0, 678.0, 717.0, 758.0, 801.0, 847.0,
	895.0, 946.0, 1000.0, 1050.0, 1110.0, 1180.0, 1240.0, 1320.0, 1390.0, 1470.0, 1550.0, 1640.0, 1740.0, 1840.0, 1940.0, 2050.0,
	2170.0, 2290.0, 2430.0, 2560.0, 2710.0, 2870.0, 3030.0, 3200.0, 3390.0, 3580.0, 3790.0, 4000.0, 4230.0, 4470.0, 4730.0, 5000.0};

inline void ring_mod(buf &L, buf &R, const block &b)
{
	const int v = std::max(1, b.p[10]);
	if (v <= 1)
		return;
	const double gd = std::cos(PI / 2 * (v - 1) / 126.0), gw = std::sin(PI / 2 * (v - 1) / 126.0);
	const double f0 = TABLE19[std::min(127, b.p[1])] + 1.70 * b.p[2], hz = LFO_HZ[std::min(127, b.p[5])];
	const size_t n = L.size();
	buf car(n);
	double acc = 0;
	for (size_t i = 0; i < n; i++) {
		const double ph = std::fmod(double(i) / FS * hz, 1.0);
		const double m = b.p[3] == 1 ? std::sin(2 * PI * ph) : (ph < 0.5 ? 1.0 : -1.0);
		acc += f0 * std::exp2(0.35 * b.p[4] / 64.0 * m);
		car[i] = 1.07 * std::cos(2 * PI * acc / FS);
	}
	const double fh = t3(b.p[6]), fl = std::min(t3(b.p[7]), FS * 0.49);
	buf o[2];
	for (int c = 0; c < 2; c++) {
		o[c] = filter_freq(c == 0 ? L : R, n, [&](double f) {
			const double z = std::tan(PI * std::min(f, FS * 0.4999) / FS);
			const cplx hp(0, z / std::tan(PI * fh / FS)), lp(0, z / std::tan(PI * fl / FS));
			return (hp / (hp + 1.0)) / (lp + 1.0);
		});
		for (size_t i = 0; i < n; i++)
			o[c][i] *= car[i];
	}
	fx_eq(o[0], o[1], b, 13);
	for (size_t i = 0; i < n; i++) {
		L[i] = gd * L[i] + gw * o[0][i];
		R[i] = gd * R[i] + gw * o[1][i];
	}
}

// 入口の大きさで効き方が変わる型のうち Touch Wah 系は、音量 4 段で録った応答
// （tools/motif/mr_fx_dyn.py、fxdyn/<名前>.dyn）を、STFT の枠ごとに入口（左右を足して半分）の音量で補間して掛ける
inline bool dyn_type(const block &b)
{
	static const int t[][2] = {{12, 0x10}, {12, 0x11}, {12, 0x12}, {12, 0x30}, {12, 0x31}};   // Touch Wah 系だけ（ほかは一つの音量の LTV の方が合った）
	for (auto &x : t)
		if (b.is(x[0], x[1]))
			return true;
	return false;
}

inline bool dynfx(buf &L, buf &R, const block &b, const std::string &dyndir)
{
	std::string k = "t" + std::to_string(b.msb) + "_" + std::to_string(b.lsb);
	for (int i = 1; i <= 16; i++)
		k += "_" + std::to_string(b.p[i]);
	FILE *f = std::fopen((dyndir + "/" + k + ".dyn").c_str(), "rb");
	if (!f)
		return false;
	char head[128];
	if (!std::fgets(head, sizeof head, f)) {
		std::fclose(f);
		return false;
	}
	int NW = 4096, HOP = 1024, K = 0;
	std::sscanf(head, "DYN n=%d hop=%d levels=%d", &NW, &HOP, &K);
	const int NB = NW / 2 + 1;
	std::vector<double> dbs(K);
	std::vector<std::vector<cplx>> H(size_t(K) * 2, std::vector<cplx>(NB));
	for (int j = 0; j < K; j++) {
		float db = 0;
		if (std::fread(&db, sizeof db, 1, f) != 1) {
			std::fclose(f);
			return false;
		}
		dbs[j] = db;
		for (int ch = 0; ch < 2; ch++) {
			std::vector<float> raw(size_t(NB) * 2);
			if (std::fread(raw.data(), sizeof(float), raw.size(), f) != raw.size()) {
				std::fclose(f);
				return false;
			}
			for (int q = 0; q < NB; q++)
				H[j * 2 + ch][q] = cplx(raw[q * 2], raw[q * 2 + 1]);
		}
	}
	std::fclose(f);
	if (K < 2)
		return false;
	const size_t n = L.size();
	buf xs[2] = {buf(n + 2 * NW, 0.0), buf(n + 2 * NW, 0.0)}, mono(n + 2 * NW, 0.0);      // 頭に窓 1 枚ぶんの無音
	for (size_t i = 0; i < n; i++) {
		xs[0][NW + i] = L[i];
		xs[1][NW + i] = R[i];
		mono[NW + i] = (L[i] + R[i]) / 2;
	}
	std::vector<double> w(NW);
	for (int i = 0; i < NW; i++)
		w[i] = 0.5 - 0.5 * std::cos(2 * PI * i / NW);
	buf out[2] = {buf(n + 3 * NW, 0.0), buf(n + 3 * NW, 0.0)}, norm(n + 3 * NW, 0.0);
	const int nf = 1 + int((mono.size() - NW) / HOP);
	std::vector<cplx> X(NW), Y(NW);
	for (int i = 0; i < nf; i++) {
		for (int q = 0; q < NW; q++)
			X[q] = mono[size_t(i) * HOP + q] * w[q];
		fft(X, false);
		double e = 0;
		for (int q = 0; q < NB; q++)
			e += std::norm(X[q]);
		const double db = 10 * std::log10(e / NB + 1e-30);
		int j = int(std::upper_bound(dbs.begin(), dbs.end(), db) - dbs.begin()) - 1;
		j = std::max(0, std::min(K - 2, j));
		const double u = std::max(0.0, std::min(1.0, (db - dbs[j]) / (dbs[j + 1] - dbs[j])));
		for (int ch = 0; ch < 2; ch++) {
			for (int q = 0; q < NW; q++)
				X[q] = xs[ch][size_t(i) * HOP + q] * w[q];
			fft(X, false);
			for (int q = 0; q < NB; q++) {
				Y[q] = X[q] * ((1 - u) * H[j * 2 + ch][q] + u * H[(j + 1) * 2 + ch][q]);
				if (q > 0 && q < NW / 2)
					Y[NW - q] = std::conj(Y[q]);
			}
			fft(Y, true);
			for (int q = 0; q < NW; q++)
				out[ch][size_t(i) * HOP + q] += Y[q].real() * w[q];
		}
		for (int q = 0; q < NW; q++)
			norm[size_t(i) * HOP + q] += w[q] * w[q];
	}
	for (size_t t = 0; t < n; t++) {
		const double nn = norm[NW + t] > 1e-9 ? norm[NW + t] : 1.0;
		L[t] = out[0][NW + t] / nn;
		R[t] = out[1][NW + t] / nn;
	}
	return true;
}

inline void apply_block(buf &L, buf &R, const block &b, const std::string &fxddir, int tempo = 120)
{
	if (dyn_type(b) && dynfx(L, R, b, fxddir + "/../fxdyn"))
		return;
	if (b.is(15, 0x70)) {
		ring_mod(L, R, b);
		return;
	}
	if (b.is(13, 0)) {
		lofi(L, R, b, fxddir + "/../fxlf");
		return;
	}
	if (b.is(11, 1)) {
		// Pitch Change 2: Fine 1・2 がセントそのもの。Pitch Change 1 の表を逆に引いて、そのセントになる Fine の値で同じ模型を通す（確かめていない）
		static const double fx[15] = {-50, -10, -3, 0, 3, 5, 6, 7, 8, 10, 15, 20, 30, 40, 50},
				    fy[15] = {-26.2, -10.79, -5.51, 0.0, 2.48, 5.13, 5.47, 7.88, 7.58, 10.59, 10.22, 10.06, 18.26, 23.77, 26.15};
		block q = b;
		for (int c = 3; c <= 4; c++) {
			std::vector<std::pair<double, double>> pr;
			for (int i = 0; i < 15; i++)
				pr.push_back({fy[i], fx[i]});
			std::stable_sort(pr.begin(), pr.end(), [](const auto &a2, const auto &b2) { return a2.first < b2.first; });
			std::vector<double> ys, xs2;
			for (auto &e : pr) {
				ys.push_back(e.first);
				xs2.push_back(e.second);
			}
			q.pfine[c] = 64 + interp(ys.data(), xs2.data(), 15, b.p[c] - 64);
		}
		q.use_pfine = true;
		pitch_change(L, R, q);
		return;
	}
	if (ltv_type(b) && ltvfx(L, R, b, fxddir + "/../fxltv", tempo))      // 式のある Chorus・Celeste・Flanger 1・2 も、録ったものがあれば優先
		return;
	if (mod_type(b)) {
		modfx(L, R, b, fxddir + "/../fxmod", tempo);
		return;
	}
	if (lti_type(b)) {
		lti(L, R, b, fxddir + "/../fxlti", tempo);
		return;
	}
	if (b.is(11, 0)) {
		pitch_change(L, R, b);
		return;
	}
	if (b.is(5, 0x30)) {
		ensemble_detune(L, R, b);
		return;
	}
	if (b.is(15, 0x10)) {
		enhancer(L, R, b);
		return;
	}
	if (flanger_type(b)) {
		flanger(L, R, b);
		return;
	}
	if (chorus_type(b)) {
		insertion_chorus(L, R, b);
		return;
	}
	if (b.is(2, 0x30)) {
		cross_delay(L, R, b);
		return;
	}
	if (b.is(2, 0x60)) {
		cross_delay(L, R, b, tempo);
		return;
	}
	if (b.is(15, 0) || b.is(15, 1)) {
		const bool three = b.is(15, 0);
		L = filter_freq(L, L.size(), [&](double f) { return three ? eq3(b, f) : eq2(b, f); });
		R = filter_freq(R, R.size(), [&](double f) { return three ? eq3(b, f) : eq2(b, f); });
	} else if (b.is(10, 0) || b.is(10, 8)) {          // 0A 08 は表に無いが、値の並びがコンプなので同じ模型
		compressor(L, R, b);
	} else if (dist_type(b)) {
		distortion(L, R, b, fxddir);
	}
}

// つなぎ方 0 = 並列（スイッチ 1 は A、2 は B）、1 = A → B（1 は A → B、2 は B）、2 = B → A（2 は B → A、1 は A）
// エレメントをインサーションのスイッチ（0・1・2）ごとの組に分けて通す（実機の流れ）。g[s][0/1] は組 s の左右。
// つなぎ方 0（並列）: A ← 組 1、B ← 組 2。1（A → B）: A ← 組 1、B ← A の出口 + 組 2。2（B → A）: B ← 組 2、A ← B の出口 + 組 1。組 0 はそのまま足す
inline void insertion_groups(buf g[3][2], const block &a, const block &b, int connect, const std::string &fxddir, int tempo, buf &L, buf &R)
{
	const size_t n = g[0][0].size();
	auto has = [&](int s) {
		for (size_t i = 0; i < n; i++)
			if (g[s][0][i] != 0 || g[s][1][i] != 0)
				return true;
		return false;
	};
	auto add = [&](buf &x, const buf &y) {
		for (size_t i = 0; i < n; i++)
			x[i] += y[i];
	};
	L = g[0][0];
	R = g[0][1];
	buf aL = g[1][0], aR = g[1][1], bL = g[2][0], bR = g[2][1];
	if (connect == 1) {
		if (has(1))
			apply_block(aL, aR, a, fxddir, tempo);
		add(bL, aL);
		add(bR, aR);
		if (has(1) || has(2))
			apply_block(bL, bR, b, fxddir, tempo);
		add(L, bL);
		add(R, bR);
	} else if (connect == 2) {
		if (has(2))
			apply_block(bL, bR, b, fxddir, tempo);
		add(aL, bL);
		add(aR, bR);
		if (has(1) || has(2))
			apply_block(aL, aR, a, fxddir, tempo);
		add(L, aL);
		add(R, aR);
	} else {
		if (has(1))
			apply_block(aL, aR, a, fxddir, tempo);
		if (has(2))
			apply_block(bL, bR, b, fxddir, tempo);
		add(L, aL);
		add(R, aR);
		add(L, bL);
		add(R, bR);
	}
}

inline void insertion(buf &L, buf &R, const block &a, const block &b, int connect, int sw, const std::string &fxddir, int tempo = 120)
{
	const char *chain = "";
	if (sw == 1)
		chain = connect == 1 ? "AB" : "A";
	else if (sw == 2)
		chain = connect == 2 ? "BA" : "B";
	for (const char *c = chain; *c; c++)
		apply_block(L, R, *c == 'A' ? a : b, fxddir, tempo);
}

} // namespace motif_fx
