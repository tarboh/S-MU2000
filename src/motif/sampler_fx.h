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

inline bool dist_type(const block &b) { return b.is(9, 48) || b.is(9, 49) || b.is(9, 0) || b.is(9, 16); }

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
	stereo_ir ir = load_ir(fxddir + "/" + name + ".dsx");
	if (!ir.ok)
		return;
	if (drive_s < 0)
		drive_s = std::atoi(ir.head["drive"].c_str());
	double c = std::atof(ir.head["c"].c_str()), p = std::atof(ir.head["p"].c_str());
	if (b.is(9, 48)) {
		c = drive_clip(b.p[1]);
		if (b.p[1] >= 15)
			p = edge_p(b.p[11]);
	}
	double fc = std::atof(ir.head["fc"].c_str());
	if ((b.is(9, 48) || b.is(9, 49)) && b.p[3] > 0)
		fc = t3(b.p[3]);
	const double k = std::pow(10.0, (drive_gain_db(b.p[1]) - drive_gain_db(drive_s)) / 20);
	double og = 1.0;
	if (b.is(9, 48))
		og = std::pow(10.0, 40 * std::log10(std::max(1, b.p[4]) / 80.0) / 20);
	const double wmix = (std::max(1, b.p[10]) - 1) / 126.0;          // Dry/Wet（1〜127、127 が wet だけ）
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
		const buf y = filter_freq(u, n, [fc](double f) { return 1.0 / cplx(1 - (f / fc) * (f / fc), (f / fc) / 1.6); });
		buf &o = ch == 0 ? L : R;
		for (size_t i = 0; i < n; i++)
			o[i] = (1 - wmix) * o[i] + wmix * y[i] * og;
	}
}

// Cross Delay（02 30）: 左の入力 →（左 → 右、p1 × 0.1ms）→ 右の出力、右の入力 →（右 → 左、p2）→ 左の出力。出たものは反対側の線へ
// フィードバック (p3 − 64) / 64 を掛け、1 次の LPF（係数 (1 − p5 / 10)²）を通して戻る。入力の選び方 p4: 0 = L、1 = R、2 = 両方。
// Dry/Wet p10: 元の音 0.9 × min(1, (127 − v) / 63)²、こだま 0.9 × min(1, (v − 1) / 63)²
// Tempo Cross Delay（02 60、tempo > 0）: 遅れは表 14 の音符 × 60 / テンポ + Lag（p6、(v − 64) ms）、混ぜ方はエネルギーを保つ形
// （元の音 0.9 cos、こだま 0.9 sin、角 π/2 × (v − 1) / 126）
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
	double gd = 0.9 * std::pow(std::min(1.0, (127 - v) / 63.0), 2), gw = 0.9 * std::pow(std::min(1.0, (v - 1) / 63.0), 2);
	if (tempo > 0) {
		gd = 0.9 * std::cos(PI / 2 * (v - 1) / 126.0);
		gw = 0.9 * std::sin(PI / 2 * (v - 1) / 126.0);
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

// インサーションのコーラス: 遅れの動きはシステムのコーラスと同じ。元の音 0.9 cos(π/2 (v − 1) / 126)、コーラスの線のエネルギーは
// 32・64・127 で 0.364・0.838・1.5 を補間（v = Dry/Wet）
inline void insertion_chorus(buf &L, buf &R, const block &b)
{
	const int v = std::max(1, b.p[10]);
	static const double xs[4] = {1, 32, 64, 127}, ys[4] = {0.0, 0.364, 0.838, 1.5};
	const double gd = 0.9 * std::cos(PI / 2 * (v - 1) / 126.0), gw = std::sqrt(interp(xs, ys, 4, v));
	const double hz = LFO_HZ[std::min(127, b.p[1])], depth = 0.176 * b.p[2] / 1000 * FS, off = MOD_DELAY_MS[std::min(127, b.p[4])] / 1000 * FS;
	const size_t n = L.size();
	buf x(n);
	for (size_t i = 0; i < n; i++)
		x[i] = (L[i] + R[i]) / 2;
	for (size_t t = 0; t < n; t++) {
		const double ph = std::fmod(double(t) * hz / FS, 1.0), tri = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
		double o[2] = {0, 0};
		for (int c = 0; c < 2; c++) {
			const double pos = double(t) - (off + depth * (c == 0 ? tri : 1 - tri));
			if (pos < 0)
				continue;
			const size_t i = size_t(pos);
			const double fr = pos - double(i), a0 = x[i], a1 = i + 1 < n ? x[i + 1] : a0;
			o[c] = a0 + (a1 - a0) * fr;
		}
		L[t] = gd * L[t] + gw * o[0];
		R[t] = gd * R[t] + gw * o[1];
	}
}

inline void apply_block(buf &L, buf &R, const block &b, const std::string &fxddir, int tempo = 120)
{
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
	} else if (b.is(10, 0)) {
		compressor(L, R, b);
	} else if (dist_type(b)) {
		distortion(L, R, b, fxddir);
	}
}

// つなぎ方 0 = 並列（スイッチ 1 は A、2 は B）、1 = A → B（1 は A → B、2 は B）、2 = B → A（2 は B → A、1 は A）
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
