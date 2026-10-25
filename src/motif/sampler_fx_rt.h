// license:BSD-3-Clause
// sampler_fx.h のエフェクトを、その場で（1 サンプルずつ）回せる形にしたもの（2026-10-25。motif_play が使う。試験は fxrt_test.cpp）。
//
// * たたみ込み（conv_rt）: 頭の 128 タップはそのまま、4096 タップまでは 128 ずつ、その後ろは 4096 ずつに分けた FFT で掛ける。遅れは出ない
// * リバーブ: 応答（fx/reverb_*.irx、Table 4 の型は reshape で残響の長さを合わせる）は sampler_fx.h の reverb と同じ。絶対値の合計で 1e-6 ぶんの尾は切る
// * コーラスの枠: コーラスの型（05 系）は chorus と同じ揺れる遅れ、Cross Delay（02 30・02 60）は cross_delay の wet（出口 − 入れた音）
// * インサーションとコーラスの枠のほかの型（block_rt）: 3 Band EQ・2 Band EQ・Harmonic Enhancer（周波数応答から作ったインパルス応答のたたみ込み）、
//   Cross Delay、コンプ、インサーションのコーラス、フランジャー、Ensemble Detune、歪み（09 00・01・10・20・30・31。録った応答 → 頭打ち → LPF）、
//   LTI（ディレイ・リバーブ系などプリセットの組のまま録った応答、fxlti）。インサーションの組（エレメントのスイッチ 0・1・2）とつなぎ方は
//   insertion_groups と同じ。ほかの型は、インサーションでは素通し（supported() が偽）、コーラスの枠では掛けない（active() が偽）。
//   コーラス系とフランジャーは、オフラインが録った応答（.ltv）を使う型なら近似（approx()。ltv はその場では 93ms 遅れるので式の作りで回す）
#pragma once

#include "sampler_fx.h"

#include <cmath>
#include <vector>

namespace motif_fx_rt {

using motif_fx::block;
using motif_fx::buf;
using motif_fx::cplx;
using motif_fx::FS;

// 分割したたたみ込み（周波数の側で足し合わせる）。入れた音から B サンプル遅れて、応答 h を掛けた音が出る
class upols {
public:
	void init(const std::vector<double> &h, int block)
	{
		B = block;
		N = 2 * B;
		P = int((h.size() + B - 1) / B);
		H.assign(P, std::vector<cplx>(N));
		for (int p = 0; p < P; p++) {
			for (int i = 0; i < B; i++) {
				const size_t k = size_t(p) * B + i;
				H[p][i] = k < h.size() ? h[k] : 0.0;
			}
			motif_fx::fft(H[p], false);
		}
		X.assign(std::max(P, 1), std::vector<cplx>(N));
		in.assign(B, 0.0);
		out.assign(B, 0.0);
		ol.assign(B, 0.0);
		acc.assign(N, cplx());
		xi = 0;
		fill = 0;
	}
	int partitions() const { return P; }

	double tick(double x)
	{
		if (P == 0)
			return 0.0;
		in[fill] = x;
		const double y = out[fill];
		if (++fill == B) {
			fill = 0;
			process();
		}
		return y;
	}

private:
	int B = 0, N = 0, P = 0, xi = 0, fill = 0;
	std::vector<double> in, out, ol;
	std::vector<std::vector<cplx>> H, X;
	std::vector<cplx> acc;

	void process()
	{
		std::vector<cplx> &x = X[xi];
		for (int i = 0; i < B; i++) {
			x[i] = in[i];
			x[B + i] = 0.0;
		}
		motif_fx::fft(x, false);
		std::fill(acc.begin(), acc.end(), cplx());
		for (int p = 0; p < P; p++) {
			const std::vector<cplx> &xs = X[(xi - p + P) % P];
			const std::vector<cplx> &h = H[p];
			for (int k = 0; k < N; k++)
				acc[k] += xs[k] * h[k];
		}
		motif_fx::fft(acc, true);
		for (int i = 0; i < B; i++) {
			out[i] = acc[i].real() + ol[i];
			ol[i] = acc[B + i].real();
		}
		xi = (xi + 1) % P;
	}
};

// 1 入力 1 出力のたたみ込み。遅れなし: 頭の H1 タップはそのまま掛け、H1〜L1 タップは H1 ずつの分割（遅れ H1 がちょうどその始まり）、
// L1 タップから後ろは L1 ずつの分割（遅れ L1 がちょうどその始まり）
class conv_rt {
public:
	static constexpr int H1 = 128, L1 = 4096;

	void init(const std::vector<double> &h)
	{
		head.assign(H1, 0.0);
		for (int i = 0; i < H1 && i < int(h.size()); i++)
			head[i] = h[i];
		hist.assign(H1, 0.0);
		hp = 0;
		std::vector<double> mid, tail;
		for (size_t i = H1; i < h.size() && i < size_t(L1); i++)
			mid.push_back(h[i]);
		for (size_t i = L1; i < h.size(); i++)
			tail.push_back(h[i]);
		m.init(mid, H1);
		t.init(tail, L1);
	}
	int partitions() const { return m.partitions() + t.partitions(); }

	double tick(double x)
	{
		hist[hp] = x;
		double y = 0;
		int j = hp;
		for (int k = 0; k < H1; k++) {
			y += head[k] * hist[j];
			j = j == 0 ? H1 - 1 : j - 1;
		}
		hp = hp + 1 == H1 ? 0 : hp + 1;
		return y + m.tick(x) + t.tick(x);
	}

private:
	int hp = 0;
	std::vector<double> head, hist;
	upols m, t;
};

// リバーブ: 左右を足して半分を、左右の応答にたたみ込んで Return・送りを掛けた wet
class reverb_rt {
public:
	bool init(const block &rv, const std::string &fxdir)
	{
		active_ = false;
		char name[64];
		std::snprintf(name, sizeof name, "/reverb_%d_%d.irx", rv.msb, rv.lsb);
		motif_fx::stereo_ir ir = motif_fx::load_ir(fxdir + name);
		if (!ir.ok || rv.send == 0)
			return false;
		if (motif_fx::table4_type(rv)) {
			double rt[7];
			const char *c = ir.head["rt"].c_str();
			for (int i = 0; i < 7; i++) {
				rt[i] = std::atof(c);
				while (*c && *c != ',')
					c++;
				if (*c)
					c++;
			}
			const double target = motif_fx::TABLE4[rv.p[1] < 0 ? 0 : rv.p[1] > 69 ? 69 : rv.p[1]];
			ir.L = motif_fx::reshape(ir.L, rt, target);
			ir.R = motif_fx::reshape(ir.R, rt, target);
		}
		const double g = rv.ret / 96.0 * rv.send / 127.0;
		// 尾を切る: 切る部分の絶対値の合計が、全体の 1e-6 より小さい所まで（1 サンプルずつの大きさで切ると、小さくても長く続く尾
		// （01 40 の MelodicTom）が足し合わさって 4e-3 ずれた）
		double total = 0;
		for (size_t i = 0; i < ir.L.size(); i++)
			total += std::fabs(ir.L[i]) + std::fabs(ir.R[i]);
		size_t len = ir.L.size();
		for (double tail = 0; len > 0; len--) {
			tail += std::fabs(ir.L[len - 1]) + std::fabs(ir.R[len - 1]);
			if (tail > total * 1e-6)
				break;
		}
		if (len == 0)
			return false;
		std::vector<double> hl(len), hr(len);
		for (size_t i = 0; i < len; i++) {
			hl[i] = ir.L[i] * g;
			hr[i] = ir.R[i] * g;
		}
		cl.init(hl);
		cr.init(hr);
		active_ = true;
		return true;
	}
	bool active() const { return active_; }
	int partitions() const { return cl.partitions(); }

	void tick(double l, double r, double &ol, double &orr)
	{
		const double x = (l + r) / 2;
		ol = cl.tick(x);
		orr = cr.tick(x);
	}

private:
	bool active_ = false;
	conv_rt cl, cr;
};

// Cross Delay（cross_delay を 1 サンプルずつ）。入れた左右 → 出口の左右
class cross_delay_rt {
public:
	void init(const block &b, int tempo)
	{
		d1 = std::max<size_t>(1, size_t(std::lround(b.p[1] * 0.1e-3 * FS)));
		d2 = std::max<size_t>(1, size_t(std::lround(b.p[2] * 0.1e-3 * FS)));
		if (tempo > 0) {
			const double lag = (b.p[6] - 64) * 1e-3, q = 60.0 / tempo;
			d1 = std::max<size_t>(1, size_t(std::lround((motif_fx::TABLE14_Q[std::min(19, b.p[1])] * q + lag) * FS)));
			d2 = std::max<size_t>(1, size_t(std::lround((motif_fx::TABLE14_Q[std::min(19, b.p[2])] * q + lag) * FS)));
		}
		fb = (b.p[3] - 64) / 64.0;
		sel = b.p[4];
		a = std::pow(1 - std::max(1, b.p[5]) / 10.0, 2);
		const int v = std::max(1, b.p[10]);
		gd = std::pow(std::min(1.0, (127 - v) / 63.0), 2);
		gw = std::pow(std::min(1.0, (v - 1) / 63.0), 2);
		if (tempo > 0) {
			gd = std::cos(motif_fx::PI / 2 * (v - 1) / 126.0);
			gw = std::sin(motif_fx::PI / 2 * (v - 1) / 126.0);
		}
		size_t sz = 1;
		while (sz < std::max(d1, d2) + 2)
			sz <<= 1;
		lr_in.assign(sz, 0.0);
		rl_in.assign(sz, 0.0);
		plr = prl = 0;
		t = 0;
	}
	void tick(double &L, double &R)
	{
		const size_t mask = lr_in.size() - 1;
		const double lr = t >= d1 ? lr_in[(t - d1) & mask] : 0.0;
		const double rl = t >= d2 ? rl_in[(t - d2) & mask] : 0.0;
		plr = (1 - a) * lr + a * plr;
		prl = (1 - a) * rl + a * prl;
		lr_in[t & mask] = (sel == 0 || sel == 2 ? L : 0.0) + fb * prl;
		rl_in[t & mask] = (sel == 1 || sel == 2 ? R : 0.0) + fb * plr;
		L = gd * L + gw * rl;
		R = gd * R + gw * lr;
		t++;
	}

private:
	size_t d1 = 1, d2 = 1, t = 0;
	double fb = 0, a = 0, gd = 1, gw = 0, plr = 0, prl = 0;
	int sel = 0;
	std::vector<double> lr_in, rl_in;
};

// 周波数応答 H（f >= 0 で与え、負の側は共役）から、4096 タップのインパルス応答を作る（filter_freq と同じ応答）
// pre を渡すと、負の時刻（オフラインの filter_freq は音全体の円のたたみ込みなので、先の音も使う）の pre タップを頭へ回す。
// その応答で掛けた音は pre サンプル遅れる
template <class F>
std::vector<double> fir_from(F H, size_t taps = 4096, size_t pre = 0)
{
	const size_t Nf = 1 << 15;
	std::vector<cplx> a(Nf);
	for (size_t k = 0; k <= Nf / 2; k++) {
		const cplx h = H(double(k) * FS / double(Nf));
		a[k] = h;
		if (k > 0 && k < Nf / 2)
			a[Nf - k] = std::conj(h);
	}
	motif_fx::fft(a, true);
	std::vector<double> h(taps);
	for (size_t i = 0; i < taps; i++)
		h[i] = i < pre ? a[Nf - pre + i].real() : a[i - pre].real();
	return h;
}

// オフラインの apply_block が、録った応答（fxltv の .ltv、4096 の窓を 1024 ずつずらす STFT）を優先する型か。
// その場で回すと窓 1 枚ぶん（93ms）遅れるので、こちらは式の作り（insertion_chorus・flanger）で回し、近似として扱う
inline bool has_ltv(const block &b, const std::string &ltvdir, int tempo)
{
	if (!motif_fx::ltv_type(b))
		return false;
	std::string k = "t" + std::to_string(b.msb) + "_" + std::to_string(b.lsb);
	for (int i = 1; i <= 16; i++)
		k += "_" + std::to_string(b.p[i]);
	k += "_" + std::to_string(b.is(7, 0x50) || b.is(6, 0x50) || b.is(14, 0x30) ? tempo : 0);
	FILE *f = std::fopen((ltvdir + "/" + k + ".ltv").c_str(), "rb");
	if (!f)
		return false;
	std::fclose(f);
	return true;
}

// エフェクトの 1 つのブロック（インサーションの A・B、コーラスの枠のほかの型）を 1 サンプルずつ。入れた左右 → 出口の左右（その場で書き換える）。
// 回せる型: 3 Band EQ・2 Band EQ（0F 00・0F 01）、Harmonic Enhancer（0F 10）、Cross Delay（02 30・02 60）、コンプ（0A 00・0A 08）、
// インサーションのコーラス（05 00 系）、フランジャー（06 00・06 01）、Ensemble Detune（05 30）。ほかは素通し（supported() が偽）
class block_rt {
public:
	// fxddir は capture の置き場の fxd（録った応答があるかを見るため。空なら見ない）
	void init(const block &b, int tempo, const std::string &fxddir = std::string())
	{
		kind = 0;
		eq_on = false;
		approx_ = !fxddir.empty() && (motif_fx::chorus_type(b) || motif_fx::flanger_type(b)) && has_ltv(b, fxddir + "/../fxltv", tempo);
		if (b.msb == 0)
			return;                          // thru
		ring_l.assign(1 << 16, 0.0);
		ring_r.assign(1 << 16, 0.0);
		t = 0;
		stereo_in = b.p[15] == 1;
		const int v = std::max(1, b.p[10]);
		if (b.is(15, 0) || b.is(15, 1)) {
			const bool three = b.is(15, 0);
			const auto h = fir_from([&](double f) { return three ? motif_fx::eq3(b, f) : motif_fx::eq2(b, f); });
			fl.init(h);
			fr.init(h);
			kind = 1;
		} else if (b.is(15, 0x10)) {
			// Harmonic Enhancer: 1 + g × HPF のあと ±1.0 で頭打ち
			const double fc = std::min(motif_fx::t3(b.p[1]), FS * 0.49), g = 8.13 * b.p[2] / 64.0 * b.p[3] / 64.0;
			const auto h = fir_from([&](double f) {
				const double fa = std::min(std::fabs(f), FS * 0.4999);
				const cplx s(0, std::tan(motif_fx::PI * fa / FS) / std::tan(motif_fx::PI * fc / FS));
				return cplx(1.0 + g * (s / (s + 1.0)) * (s * s / (s * s + s * std::sqrt(2.0) + 1.0)));
			});
			fl.init(h);
			fr.init(h);
			kind = 7;
		} else if (b.is(2, 0x30) || b.is(2, 0x60)) {
			cd.init(b, b.is(2, 0x60) ? tempo : 0);
			kind = 2;
		} else if (b.is(10, 0) || b.is(10, 8)) {
			win = int(0.005 * FS);
			T = b.p[3] - 127;
			Rr = motif_fx::COMP_RATIO[std::min(7, b.p[4])];
			ka = std::exp(-1.0 / (1.6 * motif_fx::COMP_ATTACK_MS[std::min(19, b.p[1])] / 1000 * FS));
			rel = 7.5 / (motif_fx::COMP_RELEASE_MS[std::min(15, b.p[2])] / 1000) / FS;
			makeup = 40 * std::log10(std::max(1, b.p[5]) / 64.0);
			sq.assign(win, 0.0);
			acc = 0;
			g = 0;
			kind = 3;
		} else if (b.is(5, 0x30)) {
			// Ensemble Detune（ensemble_detune と同じ）
			gd = std::cos(motif_fx::PI / 2 * (v - 1) / 126.0);
			gw = std::sin(motif_fx::PI / 2 * (v - 1) / 126.0);
			const int cents = b.p[1] - 64;
			for (int c = 0; c < 2; c++) {
				const double cc = c == 0 ? -cents : cents;
				ens_init[c] = motif_fx::MOD_DELAY_MS[std::min(127, b.p[c == 0 ? 2 : 3])] / 1000 * FS;
				ens_rate[c] = 1 - std::exp2(cc / 1200.0);
				ens_span[c] = std::fabs(ens_rate[c]) / (1 / 1.485) * FS;
			}
			eq_init(b, 11);
			kind = 6;
		} else if (motif_fx::chorus_type(b)) {
			// インサーションのコーラス（insertion_chorus と同じ）
			static const double vx[4] = {1, 32, 64, 127}, vy[4] = {0.0, 0.364, 0.838, 1.5};
			gd = std::cos(motif_fx::PI / 2 * (v - 1) / 126.0);
			gw = std::sqrt(motif_fx::interp(vx, vy, 4, v));
			hz = motif_fx::LFO_HZ[std::min(127, b.p[1])];
			depth = 0.176 * b.p[2] / 1000 * FS;
			off = motif_fx::MOD_DELAY_MS[std::min(127, b.p[4])] / 1000 * FS;
			eq_init(b, 6);
			kind = 4;
		} else if (motif_fx::flanger_type(b)) {
			// フランジャー（flanger と同じ）
			gd = std::cos(motif_fx::PI / 2 * (v - 1) / 126.0);
			gw = std::sin(motif_fx::PI / 2 * (v - 1) / 126.0);
			hz = motif_fx::LFO_HZ[std::min(127, b.p[1])];
			depth = 0.0914 * b.p[2] / 1000 * FS;
			off = motif_fx::MOD_DELAY_MS[std::min(127, b.p[4])] / 1000 * FS;
			fb = (b.p[3] - 64) / 64.0;
			dph = (b.p[14] - 64) * 3.0 / 360.0;
			fl_y[0].assign(1 << 16, 0.0);
			fl_y[1].assign(1 << 16, 0.0);
			eq_init(b, 6);
			kind = 5;
		} else if (motif_fx::dist_type(b) && !fxddir.empty() && dist_init(b, fxddir)) {
			kind = 8;
		} else if (motif_fx::lti_type(b) && !fxddir.empty() && lti_init(b, fxddir + "/../fxlti", tempo)) {
			kind = 9;
		} else
			kind = -1;                       // まだ回せない型（素通し）
	}
	bool supported() const { return kind >= 0; }
	bool thru() const { return kind == 0; }
	bool approx() const { return approx_; }
	// この枠の出口の遅れ（サンプル。歪みだけ DIST_PRE）
	int latency() const { return kind == 8 ? int(DIST_PRE) : 0; }
	// 前の枠の遅れで音が n サンプル遅れて届くとき、揺れなどの時計もその分遅らせて始める（オフラインと同じ位相にする）
	void delay_start(int n) { skip = n; }

	void tick(double &L, double &R)
	{
		if (skip > 0) {
			skip--;
			return;
		}
		const size_t mask = ring_l.size() - 1;
		switch (kind) {
		case 1:
			L = fl.tick(L);
			R = fr.tick(R);
			break;
		case 7:
			L = std::max(-1.0, std::min(1.0, fl.tick(L)));
			R = std::max(-1.0, std::min(1.0, fr.tick(R)));
			break;
		case 2:
			cd.tick(L, R);
			break;
		case 3: {
			// compressor を 1 サンプルずつ
			const double x = (L + R) / 2, s = x * x;
			acc += s;
			if (t >= size_t(win))
				acc -= sq[t % win];
			sq[t % win] = s;
			const double lev = 10 * std::log10(std::max(acc, 0.0) / win + 1e-20);
			const double tt = lev > T ? (lev - T) * (1 - 1 / Rr) : 0.0;
			g = tt > g ? tt + (g - tt) * ka : std::max(tt, g - rel);
			const double k = std::pow(10.0, (makeup - g) / 20);
			L *= k;
			R *= k;
			t++;
			break;
		}
		case 4: {
			push(L, R);
			const double ph = std::fmod(double(t) * hz / FS, 1.0), tri = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
			double o[2] = {0, 0};
			for (int c = 0; c < 2; c++) {
				const double pos = double(t) - (off + depth * (c == 0 ? tri : 1 - tri));
				if (pos >= 0)
					o[c] = at(c == 0 ? ring_l : ring_r, pos);
			}
			L = gd * L + gw * o[0];
			R = gd * R + gw * o[1];
			t++;
			eq(L, R);
			break;
		}
		case 5: {
			const double x = (L + R) / 2;
			double w[2];
			for (int c = 0; c < 2; c++) {
				const double ph0 = c == 0 ? 0.0 : dph;
				double ph = std::fmod(double(t) * hz / FS + ph0, 1.0);
				if (ph < 0)
					ph += 1.0;
				const double pos = double(t) - (off + depth * (ph < 0.5 ? 2 * ph : 2 - 2 * ph));
				double o = 0;
				std::vector<double> &y = fl_y[c];
				if (pos >= 0) {
					const size_t k = size_t(pos);
					const double fr2 = pos - double(k);
					o = (1 - fr2) * y[k & mask] + (k + 1 < t ? fr2 * y[(k + 1) & mask] : 0.0);
				}
				w[c] = o;
				y[t & mask] = x + fb * o;
			}
			L = gd * L + gw * w[0];
			R = gd * R + gw * w[1];
			t++;
			eq(L, R);
			break;
		}
		case 8: {
			// 歪み（distortion と同じ）: 左右を足した音 → 録った応答 → 柔らかい頭打ち → 2 次の LPF →（V-Distortion はディレイ）→ 元の音と混ぜる。
			// LPF は負の時刻のタップ（DIST_PRE）を頭へ回したので、出口はこの枠の中で DIST_PRE サンプル遅れる（元の音も遅らせて混ぜる）
			const double x = (L + R) / 2;
			ring_l[t & mask] = L;
			ring_r[t & mask] = R;
			const double L0 = t >= DIST_PRE ? ring_l[(t - DIST_PRE) & mask] : 0.0, R0 = t >= DIST_PRE ? ring_r[(t - DIST_PRE) & mask] : 0.0;
			double y[2];
			for (int c = 0; c < 2; c++) {
				double u = (c == 0 ? fl : fr).tick(x) * dk;
				u = u / std::pow(1 + std::pow(std::fabs(u / dc), dp), 1 / dp);
				y[c] = (c == 0 ? el : er).tick(u);
				if (vd_on) {
					std::vector<double> &line = c == 0 ? fl_y[0] : fl_y[1];
					const double back = t >= vd_df ? line[(t - vd_df) & mask] : 0.0;
					vd_lp[c] = (1 - vd_a) * back + vd_a * vd_lp[c];
					line[t & mask] = y[c] + vd_fb * vd_lp[c];
					const size_t dt = vd_dt[c];
					y[c] += vd_mix * (t >= dt ? line[(t - dt) & mask] : 0.0);
				}
			}
			L = gdry * L0 + gwet * y[0] * og;
			R = gdry * R0 + gwet * y[1] * og;
			t++;
			break;
		}
		case 9: {
			// LTI（lti と同じ）: 左右から左右への 4 本の録った応答
			if (lti_thru)
				break;
			const double l = L, r = R;
			L = l4[0].tick(l) + l4[2].tick(r);
			R = l4[1].tick(l) + l4[3].tick(r);
			break;
		}
		case 6: {
			push(L, R);
			double w[2];
			for (int c = 0; c < 2; c++) {
				const std::vector<double> &x = c == 0 ? ring_l : ring_r;
				const double ph0 = c == 0 ? 0.0 : 0.5, init = ens_init[c], rate = ens_rate[c], span = ens_span[c];
				if (span < 1e-9) {
					w[c] = at0(x, double(t) - init);
					continue;
				}
				const double sw = std::fmod(double(t) * (1 / 1.485) / FS + ph0, 1.0), s1 = rate > 0 ? sw : 1 - sw;
				const double d1 = init + span * (0.25 + s1), d2 = rate > 0 ? init + span * (s1 - 0.75) : init + span * (s1 + 1.25);
				const double u = std::min(1.0, std::max(0.0, (sw - 0.75) / 0.25));
				w[c] = (1 - u) * at0(x, double(t) - d1) + u * at0(x, double(t) - d2);
			}
			L = gd * L + gw * w[0];
			R = gd * R + gw * w[1];
			t++;
			eq(L, R);
			break;
		}
		}
	}

private:
	int kind = 0;
	size_t t = 0;
	bool stereo_in = false, eq_on = false, approx_ = false;
	int skip = 0;
	conv_rt fl, fr, el, er;
	cross_delay_rt cd;
	std::vector<double> ring_l, ring_r, fl_y[2];
	int win = 1;
	double T = 0, Rr = 1, ka = 0, rel = 0, makeup = 0, acc = 0, g = 0;
	std::vector<double> sq;
	double gd = 1, gw = 0, hz = 0, depth = 0, off = 0, fb = 0, dph = 0;
	double ens_init[2] = {0, 0}, ens_rate[2] = {0, 0}, ens_span[2] = {0, 0};
	// オフラインの LPF（filter_freq の 2 次）の応答には先の音を使う尾がある。角がナイキストに近い組（V-Distortion の 20000Hz）では 1/n でゆっくり減り、
	// 先読み 64・256・1024 で差が 1.3e-3・3.4e-4・9.8e-5。弾くときの遅れを小さくしたいので 64（1.45ms）
	conv_rt l4[4];
	bool lti_thru = false;

	// lti と同じ: プリセットの組のまま録った応答（fxlti/<名前>.lti、左→左・左→右・右→左・右→右）。Dry/Wet 1 は素通し
	bool lti_init(const block &b, const std::string &ltidir, int tempo)
	{
		lti_thru = !b.is(14, 0x40) && b.p[10] <= 1;
		if (lti_thru)
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
		for (int c = 0; c < 4; c++) {
			std::vector<double> h(frames);
			for (size_t i = 0; i < frames; i++)
				h[i] = raw[i * 4 + c];
			l4[c].init(h);
		}
		return true;
	}

	static constexpr size_t DIST_PRE = 64;
	double dk = 1, dc = 1, dp = 2, og = 1, gdry = 1, gwet = 0, vd_fb = 0, vd_a = 0, vd_mix = 0, vd_lp[2] = {0, 0};
	size_t vd_df = 1, vd_dt[2] = {1, 1};
	bool vd_on = false;

	// distortion と同じ値の決め方（応答は fxd の既定の組か、fxds のプリセットの組のまま録ったもの）
	bool dist_init(const block &b, const std::string &fxddir)
	{
		static const motif_fx::dist_set amp[] = {{1, 30, "insa_t9_48_lev16"}, {0, 30, "insa_t9_48_amptype0_lev16"}, {2, 30, "insa_t9_48_amptype2_lev16"},
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
		bool per_set = false;
		motif_fx::stereo_ir ir;
		if (!b.is(9, 48)) {
			std::string k = "t" + std::to_string(b.msb) + "_" + std::to_string(b.lsb);
			for (int i = 1; i <= 16; i++)
				k += "_" + std::to_string(i == 10 ? 127 : (i == 11 && b.is(9, 16)) ? 0 : b.p[i]);
			ir = motif_fx::load_ir(fxddir + "/../fxds/" + k + ".dsx");
			per_set = ir.ok;
		}
		if (!per_set) {
			if (b.is(9, 1) || b.is(9, 32))
				name = "insa_t9_0_distdef_lev16";
			ir = motif_fx::load_ir(fxddir + "/" + name + ".dsx");
		}
		if (!ir.ok)
			return false;
		if (drive_s < 0)
			drive_s = std::atoi(ir.head["drive"].c_str());
		dc = std::atof(ir.head["c"].c_str());
		dp = std::atof(ir.head["p"].c_str());
		if (b.is(9, 48) && !per_set) {
			dc = motif_fx::drive_clip(b.p[1]);
			if (b.p[1] >= 15)
				dp = motif_fx::edge_p(b.p[11]);
		}
		double fc = std::atof(ir.head["fc"].c_str());
		if ((b.is(9, 48) || b.is(9, 49)) && b.p[3] > 0 && !per_set)
			fc = motif_fx::t3(b.p[3]);
		dk = per_set ? 1.0 : std::pow(10.0, (motif_fx::drive_gain_db(b.p[1]) - motif_fx::drive_gain_db(drive_s)) / 20);
		og = 1.0;
		if (b.is(9, 48) && !per_set)
			og = std::pow(10.0, 40 * std::log10(std::max(1, b.p[4]) / 80.0) / 20);
		const double ang = motif_fx::PI / 2 * (std::max(1, b.p[10]) - 1) / 126.0;
		size_t pk = 0;
		for (size_t i = 0; i < ir.L.size(); i++)
			if (std::fabs(ir.L[i]) > std::fabs(ir.L[pk]))
				pk = i;
		gdry = (ir.L.empty() || ir.L[pk] >= 0 ? 1.0 : -1.0) * std::cos(ang);
		gwet = std::sin(ang);
		fl.init(std::vector<double>(ir.L.begin(), ir.L.end()));
		fr.init(std::vector<double>(ir.R.begin(), ir.R.end()));
		const auto h = fir_from([fc](double f) { return 1.0 / cplx(1 - (f / fc) * (f / fc), (f / fc) / 1.6); }, 4096, DIST_PRE);
		el.init(h);
		er.init(h);
		vd_on = b.is(9, 16) && b.p[11] > 0;
		if (vd_on) {
			vd_dt[0] = size_t(std::max(1, b.p[6])) * FS / 10000;
			vd_dt[1] = size_t(std::max(1, b.p[7])) * FS / 10000;
			vd_df = size_t(std::max(1, b.p[8])) * FS / 10000;
			vd_fb = (b.p[9] - 64) / 64.0;
			vd_a = std::pow(1 - std::max(1, b.p[12]) / 10.0, 2);
			vd_mix = b.p[11] / 95.0;
			vd_lp[0] = vd_lp[1] = 0;
			fl_y[0].assign(1 << 16, 0.0);
			fl_y[1].assign(1 << 16, 0.0);
		}
		return true;
	}

	// 揺れる系のエフェクトの EQ（fx_eq と同じ。利きが両方 64 なら掛けない）
	void eq_init(const block &b, int base)
	{
		eq_on = !(b.p[base + 1] == 64 && b.p[base + 3] == 64);
		if (!eq_on)
			return;
		block q;
		q.p[1] = b.p[base], q.p[2] = b.p[base + 1], q.p[3] = b.p[base + 2], q.p[4] = b.p[base + 3];
		const auto h = fir_from([&](double f) { return motif_fx::eq2(q, f); });
		el.init(h);
		er.init(h);
	}
	void eq(double &L, double &R)
	{
		if (!eq_on)
			return;
		L = el.tick(L);
		R = er.tick(R);
	}
	// Input Mode（p15）: 1 は左右それぞれ、ほかは左右を足して半分を両方へ
	void push(double L, double R)
	{
		const size_t mask = ring_l.size() - 1;
		if (stereo_in) {
			ring_l[t & mask] = L;
			ring_r[t & mask] = R;
		} else
			ring_l[t & mask] = ring_r[t & mask] = (L + R) / 2;
	}
	// 遅れた位置の値（間はまっすぐ）。オフラインは「次の 1 つ」が先の音でも読むが、ここでは今までの音だけ（遅れが 1 サンプルより短いときだけ違う）
	double at(const std::vector<double> &x, double pos) const
	{
		const size_t mask = x.size() - 1, i = size_t(pos);
		const double fr2 = pos - double(i), a0 = x[i & mask], a1 = i + 1 <= t ? x[(i + 1) & mask] : a0;
		return a0 + (a1 - a0) * fr2;
	}
	double at0(const std::vector<double> &x, double pos) const { return pos < 0 ? 0.0 : at(x, pos); }
};

// コーラスの枠
class chorus_rt {
public:
	bool init(const block &ch, int tempo, const std::string &fxddir = std::string())
	{
		kind = 0;
		if (ch.send == 0)
			return false;
		t = 0;
		if (motif_fx::chorus_type(ch)) {
			kind = 1;
			ring.assign(1 << 16, 0.0);
			hz = motif_fx::LFO_HZ[std::min(127, ch.p[1])];
			depth = 0.176 * ch.p[2] / 1000 * FS;
			off = motif_fx::MOD_DELAY_MS[std::min(127, ch.p[4])] / 1000 * FS;
			g = 2.06 * ch.ret / 96.0 * ch.send / 127.0;
			return true;
		}
		// ほかの型は、送りを掛けた音をインサーションと同じ部品（block_rt）に通す（sampler_fx.h の system_chorus）
		bk.init(ch, tempo, fxddir);
		if (bk.supported() && !bk.thru()) {
			kind = 2;
			gi = ch.send / 127.0;
			go = ch.ret / 96.0;
			return true;
		}
		return false;
	}
	bool active() const { return kind != 0; }
	bool approx() const { return kind == 2 && bk.approx(); }
	// インサーションの遅れで音が n サンプル遅れて届くとき、時計もその分遅らせて始める
	void delay_start(int n) { skip = n; }

	void tick(double l, double r, double &ol, double &orr)
	{
		ol = orr = 0;
		if (skip > 0) {
			skip--;
			return;
		}
		if (kind == 1) {
			const size_t mask = ring.size() - 1;
			ring[t & mask] = (l + r) / 2;
			const double ph = std::fmod(double(t) * hz / FS, 1.0);
			const double tri = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
			for (int c = 0; c < 2; c++) {
				const double d = off + depth * (c == 0 ? tri : 1 - tri);
				const double pos = double(t) - d;
				if (pos < 0)
					continue;
				const size_t i = size_t(pos);
				const double fr = pos - double(i);
				const double x0 = ring[i & mask], x1 = i + 1 <= t ? ring[(i + 1) & mask] : x0;
				(c == 0 ? ol : orr) += (x0 + (x1 - x0) * fr) * g;
			}
			t++;
		} else if (kind == 2) {
			// 送りを掛けた音を Cross Delay に通し、効いた分（出口 − 入れた音）に Return（sampler_fx.h の system_chorus、SYSCHO_WET）
			double L = l * gi, R = r * gi;
			const double dl = L, dr = R;
			bk.tick(L, R);
			ol = (L - dl) * go;
			orr = (R - dr) * go;
		}
	}

private:
	int kind = 0;
	size_t t = 0;
	std::vector<double> ring;
	double hz = 0, depth = 0, off = 0, g = 0, gi = 0, go = 0;
	int skip = 0;
	block_rt bk;
};

// 決まった長さの遅れ（左右）
class delay2 {
public:
	void init(int d)
	{
		n = d;
		size_t sz = 1;
		while (sz < size_t(d) + 1)
			sz <<= 1;
		l.assign(sz, 0.0);
		r.assign(sz, 0.0);
		t = 0;
	}
	void tick(double &L, double &R)
	{
		if (n == 0)
			return;
		const size_t mask = l.size() - 1;
		l[t & mask] = L;
		r[t & mask] = R;
		L = t >= size_t(n) ? l[(t - n) & mask] : 0.0;
		R = t >= size_t(n) ? r[(t - n) & mask] : 0.0;
		t++;
	}

private:
	int n = 0;
	size_t t = 0;
	std::vector<double> l, r;
};

// インサーション全体: 組（エレメントのスイッチ 0・1・2）ごとの音を、つなぎ方（0 並列・1 A → B・2 B → A）どおりに通して足す（insertion_groups と同じ）。
// 枠に遅れ（歪み）があれば、足し合わせる所で遅れの少ない側を遅らせてそろえる。出口全体は latency() サンプル遅れる
class insertion_rt {
public:
	void init(const block &a, const block &b, int connect, int tempo, const std::string &fxddir = std::string())
	{
		A.init(a, tempo, fxddir);
		Bk.init(b, tempo, fxddir);
		con = connect;
		const int la = A.latency(), lb = Bk.latency();
		if (con == 1) {
			lat = la + lb;
			Bk.delay_start(la);
			d_in.init(la);         // 組 2 を A の出口にそろえてから B へ
			d0.init(lat);
			da.init(0);
			db.init(0);
		} else if (con == 2) {
			lat = la + lb;
			A.delay_start(lb);
			d_in.init(lb);         // 組 1 を B の出口にそろえてから A へ
			d0.init(lat);
			da.init(0);
			db.init(0);
		} else {
			lat = std::max(la, lb);
			d_in.init(0);
			d0.init(lat);
			da.init(lat - la);
			db.init(lat - lb);
		}
	}
	bool supported() const { return A.supported() && Bk.supported(); }
	bool approx() const { return A.approx() || Bk.approx(); }
	const block_rt &block_a() const { return A; }
	const block_rt &block_b() const { return Bk; }
	int latency() const { return lat; }

	void tick(const double gl[3], const double gr[3], double &L, double &R)
	{
		double iL, iR;
		tick(gl, gr, L, R, iL, iR);
	}
	// iL・iR に、インサーションを通った分だけ（組 0 の素通しを除いたもの。ドラムの Ins-EF の送りに使う）
	void tick(const double gl[3], const double gr[3], double &L, double &R, double &iL, double &iR)
	{
		L = gl[0];
		R = gr[0];
		d0.tick(L, R);
		const double tL = L, tR = R;
		tick_ins(gl, gr, L, R);
		iL = L - tL;
		iR = R - tR;
	}

private:
	void tick_ins(const double gl[3], const double gr[3], double &L, double &R)
	{
		double aL = gl[1], aR = gr[1], bL = gl[2], bR = gr[2];
		if (con == 1) {
			A.tick(aL, aR);
			d_in.tick(bL, bR);
			bL += aL;
			bR += aR;
			Bk.tick(bL, bR);
			L += bL;
			R += bR;
		} else if (con == 2) {
			Bk.tick(bL, bR);
			d_in.tick(aL, aR);
			aL += bL;
			aR += bR;
			A.tick(aL, aR);
			L += aL;
			R += aR;
		} else {
			A.tick(aL, aR);
			Bk.tick(bL, bR);
			da.tick(aL, aR);
			db.tick(bL, bR);
			L += aL + bL;
			R += aR + bR;
		}
	}

private:
	block_rt A, Bk;
	int con = 0, lat = 0;
	delay2 d0, d_in, da, db;
};

} // namespace motif_fx_rt
