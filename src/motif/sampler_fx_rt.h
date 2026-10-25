// license:BSD-3-Clause
// sampler_fx.h のエフェクトを、その場で（1 サンプルずつ）回せる形にしたもの（2026-10-25。motif_play が使う。試験は fxrt_test.cpp）。
//
// * たたみ込み（conv_rt）: 頭の 128 タップはそのまま、4096 タップまでは 128 ずつ、その後ろは 4096 ずつに分けた FFT で掛ける。遅れは出ない
// * リバーブ: 応答（fx/reverb_*.irx、Table 4 の型は reshape で残響の長さを合わせる）は sampler_fx.h の reverb と同じ。−100dB より下の尾は切る
// * コーラスの枠: コーラスの型（05 系）は chorus と同じ揺れる遅れ、Cross Delay（02 30・02 60）は cross_delay の wet（出口 − 入れた音）
// * インサーション: 3 Band EQ・2 Band EQ（0F 00・0F 01。周波数応答 eq3・eq2 から作ったインパルス応答のたたみ込み）、Cross Delay（02 30・02 60）、
//   コンプ（0A 00・0A 08）。組（エレメントのスイッチ 0・1・2）とつなぎ方は insertion_groups と同じ。ほかの型は素通し（supported() が偽）
// コーラスの枠のほかの型（Ensemble Detune・フランジャーなど）は、まだ回せない（active() が偽）
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
		// −100dB より下の尾を切る
		double peak = 0;
		for (size_t i = 0; i < ir.L.size(); i++)
			peak = std::max(peak, std::max(std::fabs(ir.L[i]), std::fabs(ir.R[i])));
		size_t len = ir.L.size();
		while (len > 0 && std::fabs(ir.L[len - 1]) < peak * 1e-5 && std::fabs(ir.R[len - 1]) < peak * 1e-5)
			len--;
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

// コーラスの枠
class chorus_rt {
public:
	bool init(const block &ch, int tempo)
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
		if (ch.is(2, 0x30) || ch.is(2, 0x60)) {
			kind = 2;
			cd.init(ch, ch.is(2, 0x60) ? tempo : 0);
			gi = ch.send / 127.0;
			go = ch.ret / 96.0;
			return true;
		}
		return false;
	}
	bool active() const { return kind != 0; }

	void tick(double l, double r, double &ol, double &orr)
	{
		ol = orr = 0;
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
			cd.tick(L, R);
			ol = (L - dl) * go;
			orr = (R - dr) * go;
		}
	}

private:
	int kind = 0;
	size_t t = 0;
	std::vector<double> ring;
	double hz = 0, depth = 0, off = 0, g = 0, gi = 0, go = 0;
	cross_delay_rt cd;
};

// インサーションの 1 つの枠（A か B）
class ins_rt {
public:
	void init(const block &b, int tempo)
	{
		kind = 0;
		if (b.msb == 0)
			return;                          // thru
		if (b.is(15, 0) || b.is(15, 1)) {
			// 周波数応答から、インパルス応答（4096 タップ）を作る。filter_freq と同じ応答（f の正の側を与え、負の側は共役）
			const bool three = b.is(15, 0);
			const size_t Nf = 1 << 15, taps = 4096;
			std::vector<cplx> a(Nf);
			for (size_t k = 0; k <= Nf / 2; k++) {
				const cplx h = three ? motif_fx::eq3(b, double(k) * FS / double(Nf)) : motif_fx::eq2(b, double(k) * FS / double(Nf));
				a[k] = h;
				if (k > 0 && k < Nf / 2)
					a[Nf - k] = std::conj(h);
			}
			motif_fx::fft(a, true);
			std::vector<double> h(taps);
			for (size_t i = 0; i < taps; i++)
				h[i] = a[i].real();
			fl.init(h);
			fr.init(h);
			kind = 1;
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
			n = 0;
			kind = 3;
		} else
			kind = -1;                       // まだ回せない型（素通し）
	}
	bool supported() const { return kind >= 0; }
	bool thru() const { return kind == 0; }

	void tick(double &L, double &R)
	{
		switch (kind) {
		case 1:
			L = fl.tick(L);
			R = fr.tick(R);
			break;
		case 2:
			cd.tick(L, R);
			break;
		case 3: {
			// compressor を 1 サンプルずつ
			const double x = (L + R) / 2, s = x * x;
			acc += s;
			if (n >= size_t(win))
				acc -= sq[n % win];
			sq[n % win] = s;
			n++;
			const double lev = 10 * std::log10(std::max(acc, 0.0) / win + 1e-20);
			const double t = lev > T ? (lev - T) * (1 - 1 / Rr) : 0.0;
			g = t > g ? t + (g - t) * ka : std::max(t, g - rel);
			const double k = std::pow(10.0, (makeup - g) / 20);
			L *= k;
			R *= k;
			break;
		}
		}
	}

private:
	int kind = 0;
	conv_rt fl, fr;
	cross_delay_rt cd;
	int win = 1;
	double T = 0, Rr = 1, ka = 0, rel = 0, makeup = 0, acc = 0, g = 0;
	std::vector<double> sq;
	size_t n = 0;
};

// インサーション全体: 組（エレメントのスイッチ 0・1・2）ごとの音を、つなぎ方（0 並列・1 A → B・2 B → A）どおりに通して足す（insertion_groups と同じ）
class insertion_rt {
public:
	void init(const block &a, const block &b, int connect, int tempo)
	{
		A.init(a, tempo);
		Bk.init(b, tempo);
		con = connect;
	}
	bool supported() const { return A.supported() && Bk.supported(); }

	void tick(const double gl[3], const double gr[3], double &L, double &R)
	{
		L = gl[0];
		R = gr[0];
		double aL = gl[1], aR = gr[1], bL = gl[2], bR = gr[2];
		if (con == 1) {
			A.tick(aL, aR);
			bL += aL;
			bR += aR;
			Bk.tick(bL, bR);
			L += bL;
			R += bR;
		} else if (con == 2) {
			Bk.tick(bL, bR);
			aL += bL;
			aR += bR;
			A.tick(aL, aR);
			L += aL;
			R += aR;
		} else {
			A.tick(aL, aR);
			Bk.tick(bL, bR);
			L += aL + bL;
			R += aR + bR;
		}
	}

private:
	ins_rt A, Bk;
	int con = 0;
};

} // namespace motif_fx_rt
