// license:BSD-3-Clause
// sampler_fx.h のエフェクトを、その場で（1 サンプルずつ）回せる形にしたもの（2026-10-25。motif_play が使う。試験は fxrt_test.cpp）。
//
// * たたみ込み（conv_rt）: 頭の 128 タップはそのまま、4096 タップまでは 128 ずつ、その後ろは 4096 ずつに分けた FFT で掛ける。遅れは出ない
// * リバーブ: 応答（fx/reverb_*.irx、Table 4 の型は reshape で残響の長さを合わせる）は sampler_fx.h の reverb と同じ。絶対値の合計で 1e-6 ぶんの尾は切る
// * コーラスの枠: コーラスの型（05 系）は chorus と同じ揺れる遅れ、Cross Delay（02 30・02 60）は cross_delay の wet（出口 − 入れた音）
// * インサーションとコーラスの枠のほかの型（block_rt）: 3 Band EQ・2 Band EQ・Harmonic Enhancer（式どおりの IIR。2026-10-30 まではたたみ込み）、
//   Cross Delay、コンプ、インサーションのコーラス、フランジャー、Ensemble Detune、歪み（09 00・01・10・20・30・31。録った応答 → 頭打ち → LPF）、
//   LTI（ディレイ・リバーブ系などプリセットの組のまま録った応答、fxlti）、揺れる型（フェイザー・ワウなどの .ltv、Touch Wah の .dyn、
//   Tremolo・Rotary などの .mod。ltv_rt・mod_rt、オフラインとは少し違う）。インサーションの組（エレメントのスイッチ 0・1・2）とつなぎ方は
//   insertion_groups と同じ。ほかの型は、インサーションでは素通し（supported() が偽）、コーラスの枠では掛けない（active() が偽）。
//   コーラス系とフランジャーも、録った応答（.ltv）があれば ltv_rt で回す（2026-10-27。それまでは式の作りで回す近似だった）
#pragma once

#include "sampler_fx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
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
	// chorev: コーラスからリバーブへの送り（0〜127、Send Chorus To Reverb）。0 でなければ、応答には Return だけを掛け、送りは入口で掛けて、
	// tick の xl・xr（コーラスの Return の前の wet）に chorev / 127 を掛けて足す（実機で測った。tools/motif/mr_cho_rev.py、2026-10-28）。
	// 0 なら前と同じ（送りも応答に焼き込む）
	bool init(const block &rv, const std::string &fxdir, int chorev = 0)
	{
		active_ = false;
		char name[64];
		std::snprintf(name, sizeof name, "/reverb_%d_%d.irx", rv.msb, rv.lsb);
		motif_fx::stereo_ir ir = motif_fx::load_ir(fxdir + name);
		if (!ir.ok || (rv.send == 0 && chorev <= 0))
			return false;
		in_dry = 1.0;
		in_x = 0.0;
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
		double g = rv.ret / 96.0 * rv.send / 127.0;
		if (chorev > 0) {
			g = rv.ret / 96.0;
			in_dry = rv.send / 127.0;
			in_x = chorev / 127.0;
		}
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
		motif_fx::return_pan(false, rv.pan, pgl, pgr);
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

	void tick(double l, double r, double &ol, double &orr, double xl = 0, double xr = 0)
	{
		const double x = in_x != 0.0 ? (l + r) / 2 * in_dry + (xl + xr) / 2 * in_x : (l + r) / 2;
		ol = cl.tick(x) * pgl;
		orr = cr.tick(x) * pgr;
	}
	bool takes_chorus() const { return in_x != 0.0; }

private:
	bool active_ = false;
	double in_dry = 1.0, in_x = 0.0, pgl = 1.0, pgr = 1.0;     // Return のパン（motif_fx::return_pan）
	conv_rt cl, cr;
};

// Cross Delay（cross_delay を 1 サンプルずつ）。入れた左右 → 出口の左右
class cross_delay_rt {
public:
	void init(const block &b, int tempo)
	{
		lr_in.clear();
		rl_in.clear();
		set(b, tempo);
		plr = prl = 0;
		t = 0;
	}
	// 値だけ替える（覚えている音は残す。遅れが今の輪より長くなったときだけ輪を作り直す）
	void set(const block &b, int tempo)
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
		size_t sz = size_t(1) << 17;
		while (sz < std::max(d1, d2) + 2)
			sz <<= 1;
		if (lr_in.size() < sz) {
			lr_in.assign(sz, 0.0);
			rl_in.assign(sz, 0.0);
		}
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

// 時間で変わるたたみ込みの部品: 入口の側（今までの音と、128 ずつの区切りの FFT）だけを持ち、応答（filt）は外に置く。
// 頭の 128 タップはそのまま掛け、後ろは 128 ずつに分けた FFT（overlap-save）。応答の側に覚えておく音が無いので、応答はいつ作り替えてもよく、遅れも無い
class tv_conv {
public:
	static constexpr int B = 128;
	struct filt {
		std::vector<double> head[2];
		std::vector<cplx> Hp[2];             // 分けた P 個（それぞれ 2B 点）
	};

	void init(int taps)
	{
		P = std::max(1, (taps - B + B - 1) / B);
		L = taps;
		for (int c = 0; c < 2; c++) {
			hist[c].assign(1 << 13, 0.0);
			X[c].assign(size_t(P) * 2 * B, cplx());
		}
		t = 0;
	}
	int taps() const { return L; }
	// インパルス応答（左右）から応答を作る（L タップまで）
	void make(filt &f, const std::vector<double> h[2]) const
	{
		std::vector<cplx> a(2 * B);
		for (int c = 0; c < 2; c++) {
			f.head[c].assign(B, 0.0);
			for (int k = 0; k < B && k < int(h[c].size()); k++)
				f.head[c][k] = h[c][k];
			f.Hp[c].assign(size_t(P) * 2 * B, cplx());
			for (int p = 0; p < P; p++) {
				std::fill(a.begin(), a.end(), cplx());
				for (int j = 0; j < B; j++) {
					const size_t k = size_t(B + p * B + j);
					a[j] = k < h[c].size() && int(k) < L ? h[c][k] : 0.0;
				}
				motif_fx::fft(a, false);
				std::copy(a.begin(), a.end(), f.Hp[c].begin() + size_t(p) * 2 * B);
			}
		}
	}
	// 区切りの頭か（このときは先に start() を呼ぶ）
	bool at_start() const { return t % B == 0; }
	// 区切りの頭で: 1 つ前までの区切りの FFT を足す
	void start()
	{
		const long k = long(t / B);
		if (k < 1)
			return;
		const size_t m = hist[0].size() - 1;
		std::vector<cplx> a(2 * B);
		for (int c = 0; c < 2; c++) {
			for (int j = 0; j < 2 * B; j++) {
				const long s = long(t) - 2 * B + j;
				a[j] = s >= 0 ? hist[c][size_t(s) & m] : 0.0;
			}
			motif_fx::fft(a, false);
			std::copy(a.begin(), a.end(), X[c].begin() + size_t((k - 1) % P) * 2 * B);
		}
	}
	// この区切りの B サンプルぶんの、後ろのタップの出口（start() のあとで）
	void tail(const filt &f, int c, double out[B]) const
	{
		const long k = long(t / B);
		std::vector<cplx> acc(2 * B);
		for (int p = 0; p < P && k - 1 - p >= 0; p++) {
			const cplx *x = &X[c][size_t((k - 1 - p) % P) * 2 * B], *h = &f.Hp[c][size_t(p) * 2 * B];
			for (int j = 0; j < 2 * B; j++)
				acc[j] += x[j] * h[j];
		}
		motif_fx::fft(acc, true);
		for (int j = 0; j < B; j++)
			out[j] = acc[B + j].real();
	}
	// 入れる（左右）。このあと head() で頭のタップを掛け、最後に next()
	void push(double l, double r)
	{
		const size_t m = hist[0].size() - 1;
		hist[0][t & m] = l;
		hist[1][t & m] = r;
	}
	double head(const filt &f, int c) const
	{
		const size_t m = hist[0].size() - 1;
		double y = 0;
		for (int k = 0; k < B && size_t(k) <= t; k++)
			y += f.head[c][k] * hist[c][(t - k) & m];
		return y;
	}
	void next() { t++; }
	size_t time() const { return t; }
	const std::vector<double> &history(int c) const { return hist[c]; }

private:
	int P = 1, L = 0;
	size_t t = 0;
	std::vector<double> hist[2];
	std::vector<cplx> X[2];
};

// 揺れながら周波数ごとに違う動きをする型（ltv_type: フェイザー・Auto Wah・Vintage Wah・Tech など）を、録った時間ごとの応答（fxltv の .ltv）で 1 サンプルずつ。
// オフラインの ltvfx は窓 4096・ずらし 1024 の STFT で、出口の時刻 t には枠 i（i × 1024 が t より後で t + 4096 以下）の応答が窓の 2 乗の重みで混ざる。
// こちらは枠ごとの応答をインパルス応答（頭から 4096 − 64 タップ。後ろの 64 は負の時刻なので捨てる）にしてたたみ込み、同じ重みで混ぜる。
// STFT では遅れ k のタップに窓の自己相関 R(k) / R(0) が掛かる（窓をずらして重ねると、遅れの大きいタップほど窓の外へ落ちる）ので、インパルス応答にもそれを掛ける。
// 窓の中で円のたたみ込みになる分（遅れ k のタップが k − 4096 にも R(4096 − k) で回る）は先の音なので回せない。尾が雑音のように長い
// Lo-Res・Talking Mod（中に頭打ちや間引きがあり、時間ごとの応答では表せない型）は、オフラインより 2.3・0.7dB 小さい（R(k) を掛けないと 1dB 大きい。
// 大きさだけ √(R(k)² + R(4096 − k)²) で足すと音量は合うが、Lo-Res の低域が 12dB 出過ぎ、Vintage Wah も少し離れた）。
// 同時に効く枠は 4 つで、使い始める時に作る。
// 山が負の時刻にある型（Flanger 3・Multi Band Comp・Attack LoFi などで −200 サンプルほど。元の音より先に出る分）は、山の前 64 サンプルから取るように
// D（128 の倍数）だけ先読みし、出口は D サンプル遅れる
//
// 入口の大きさで効き方が変わる Touch Wah 系（dyn_type、fxdyn の .dyn）も同じ部品で回す: 音量ごとの応答を作っておき、
// 今までの 4096 サンプル（ハン窓）の大きさで 2 つを選んで混ぜる。オフラインは枠の窓（先の音を含む）で測るので、こちらは 2048 サンプルほど遅れて追う（近似）
class ltv_rt {
public:
	bool init_ltv(const block &b, const std::string &ltvdir, int tempo)
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
		NW = 4096, HOP = 1024, F = 0, per = 0;
		std::sscanf(head, "LTV n=%d hop=%d frames=%d period=%d", &NW, &HOP, &F, &per);
		NB = NW / 2 + 1;
		raw.resize(size_t(F) * 2 * NB * 2);
		F = int(std::fread(raw.data(), sizeof(float) * 2 * NB * 2, size_t(F), f));
		std::fclose(f);
		if (F < 1 || NW % tv_conv::B || HOP % tv_conv::B)
			return false;
		perf = motif_fx::loop_period(b, double(FS) / HOP, per, F);
		dyn = false;
		stereo = !b.is(15, 0x78) && !(motif_fx::chorus_type(b) && b.p[15] == 0);
		setup();
		{
			std::vector<const cplx *> hs;
			std::vector<std::vector<cplx>> tmp;
			for (int j = 0; j < F; j += std::max(1, F / 16))
				for (int c = 0; c < 2; c++) {
					std::vector<cplx> H(NB);
					const float *r = &raw[((size_t(j) * 2 + c) * NB) * 2];
					for (int q = 0; q < NB; q++)
						H[q] = cplx(r[q * 2], r[q * 2 + 1]);
					tmp.push_back(H);
				}
			for (auto &H : tmp)
				hs.push_back(H.data());
			find_lead(hs);
			// 元の音の遅れ: 枠の応答を平均した（揺れの線は打ち消し合う）インパルス応答の山（型によって 0、または 200 サンプルほど）
			std::vector<double> m(NW, 0.0);
			for (const cplx *H : hs) {
				const std::vector<double> h = ir(H, true);
				for (int i = 0; i < NW; i++)
					m[i] += h[i];
			}
			int pk = 0;
			for (int i = 0; i < NW; i++)
				if (std::fabs(m[i]) > std::fabs(m[pk]))
					pk = i;
			dry_lag = pk > NW / 2 ? pk - NW : pk;
		}
		for (auto &s : slot)
			s.i = -1;
		ma = mat = 1;
		rate = 1;
		i_last = -1;
		phi_last = 0;
		return true;
	}
	// つまみで Dry/Wet が動いたとき: 録った応答の出口に掛ける倍率（元の音を足し引きする分は block_rt が delayed_in に掛けて足す）
	void set_scale(double a) { mat = a; }
	// 出口のうち元の音にあたる入口（録った応答の中の元の音の遅れ dry_lag も入れて。左右、mono の型は左右を足して半分）
	double delayed_in(int c) const
	{
		const long q = long(cv.time()) - 1 - D - dry_lag;
		return q >= 0 ? cv.history(c)[size_t(q) & (cv.history(c).size() - 1)] : 0.0;
	}
	// つまみで LFO Speed が動いたとき: 録った枠を進める速さ（録った組の速さとの比）
	void set_rate(double r) { rate = std::max(0.0, r); }
	bool is_dyn() const { return dyn; }
	bool init_dyn(const block &b, const std::string &dyndir)
	{
		perf = 0;
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
		NW = 4096, HOP = 1024;
		int K = 0;
		std::sscanf(head, "DYN n=%d hop=%d levels=%d", &NW, &HOP, &K);
		NB = NW / 2 + 1;
		dbs.assign(size_t(std::max(K, 0)), 0.0);
		std::vector<std::vector<cplx>> H(size_t(std::max(K, 0)) * 2, std::vector<cplx>(NB));
		for (int j = 0; j < K; j++) {
			float db = 0;
			if (std::fread(&db, sizeof db, 1, f) != 1) {
				std::fclose(f);
				return false;
			}
			dbs[j] = db;
			for (int ch = 0; ch < 2; ch++) {
				std::vector<float> r(size_t(NB) * 2);
				if (std::fread(r.data(), sizeof(float), r.size(), f) != r.size()) {
					std::fclose(f);
					return false;
				}
				for (int q = 0; q < NB; q++)
					H[j * 2 + ch][q] = cplx(r[q * 2], r[q * 2 + 1]);
			}
		}
		std::fclose(f);
		if (K < 2 || NW % tv_conv::B)
			return false;
		dyn = true;
		stereo = true;
		setup();
		{
			std::vector<const cplx *> hs;
			for (auto &H : H)
				hs.push_back(H.data());
			find_lead(hs);
		}
		lev.resize(K);
		for (int j = 0; j < K; j++) {
			std::vector<double> h[2] = {ir(&H[j * 2][0]), ir(&H[j * 2 + 1][0])};
			cv.make(lev[j], h);
		}
		u_prev = -1;
		return true;
	}

	int latency() const { return D; }

	void tick(double &L, double &R)
	{
		const int B = tv_conv::B;
		if (cv.at_start()) {
			cv.start();
			dyn ? start_dyn() : start_ltv();
		}
		const size_t t = cv.time();
		const int n = int(t % B);
		const double m = (L + R) / 2;
		cv.push(stereo ? L : m, stereo ? R : m);
		if (dyn)
			mono[t & (mono.size() - 1)] = m;
		double y[2] = {0, 0};
		if (dyn) {
			const double u = du0 + (du1 - du0) * (n + 1) / B;
			for (int c = 0; c < 2; c++)
				y[c] = (1 - u) * (cv.head(lev[dj], c) + dtail[0][c][n]) + u * (cv.head(lev[dj + 1], c) + dtail[1][c][n]);
		} else {
			double ws = 0;
			for (auto &s : slot) {
				if (s.i < 0)
					continue;
				const long q = long(t) - D + NW - long(s.i) * HOP;
				if (q <= 0 || q >= NW)
					continue;
				const double wq = win2[size_t(q)];
				ws += wq;
				for (int c = 0; c < 2; c++)
					y[c] += wq * (cv.head(s.f, c) + s.tl[c][n]);
			}
			if (ws > 1e-9) {
				y[0] /= ws;
				y[1] /= ws;
			}
			ma += (mat - ma) * 0.005;
			if (ma != 1)
				for (int c = 0; c < 2; c++)
					y[c] *= ma;
		}
		cv.next();
		L = y[0];
		R = y[1];
	}

private:
	int NW = 4096, HOP = 1024, F = 0, per = 0, NB = 2049, D = 0;
	double perf = 0;                         // 繰り返す周期（枠、小数。motif_fx::loop_period）
	bool dyn = false, stereo = true;
	double ma = 1, mat = 1;                  // Dry/Wet のつまみ: 今と目指す倍率
	double rate = 1, phi_last = 0;          // LFO Speed のつまみ: 枠を進める速さと、枠 i_last の録った枠の位置
	long i_last = -1;
	int dry_lag = 0;                        // 録った応答の中の元の音の遅れ（サンプル）
	std::vector<float> raw;
	std::vector<double> win2, acw;
	tv_conv cv;
	struct slot_t {
		long i = -1;
		tv_conv::filt f;
		double tl[2][tv_conv::B];
	};
	slot_t slot[5];
	// Touch Wah 系
	std::vector<double> dbs, mono;
	std::vector<tv_conv::filt> lev;
	int dj = 0;
	double du0 = 0, du1 = 0, u_prev = -1;
	double dtail[2][2][tv_conv::B];

	void setup()
	{
		win2.resize(NW);
		for (int i = 0; i < NW; i++) {
			const double w = 0.5 - 0.5 * std::cos(2 * motif_fx::PI * i / NW);
			win2[i] = w * w;
		}
		// 窓の自己相関（R(k) / R(0)）
		std::vector<cplx> a(size_t(2 * NW));
		for (int i = 0; i < NW; i++)
			a[i] = 0.5 - 0.5 * std::cos(2 * motif_fx::PI * i / NW);
		motif_fx::fft(a, false);
		for (auto &v : a)
			v = std::norm(v);
		motif_fx::fft(a, true);
		acw.resize(NW);
		for (int i = 0; i < NW; i++)
			acw[i] = a[i].real() / a[0].real();
		cv.init(NW - 64);
		mono.assign(size_t(NW) * 2, 0.0);
	}
	// 周波数応答（NB 点）→ インパルス応答（NW 点、遅れ −D から。plain なら 0 から、窓の自己相関も掛けない）
	std::vector<double> ir(const cplx *H, bool plain = false) const
	{
		std::vector<cplx> a(NW);
		for (int q = 0; q < NB; q++) {
			a[q] = H[q];
			if (q > 0 && q < NW / 2)
				a[NW - q] = std::conj(H[q]);
		}
		motif_fx::fft(a, true);
		// 遅れ k = i − D（負なら頭から回った所）
		std::vector<double> h(NW);
		for (int i = 0; i < NW; i++) {
			const int k = i - D;
			h[i] = plain ? a[i].real() : a[size_t((k + NW) % NW)].real() * acw[size_t(std::abs(k))];
		}
		return h;
	}
	// 山の位置（いくつかの枠のエネルギーの合計）が負の時刻なら、D を決める
	void find_lead(const std::vector<const cplx *> &hs)
	{
		D = 0;
		std::vector<double> e(NW, 0.0);
		for (const cplx *H : hs) {
			const std::vector<double> h = ir(H, true);
			for (int i = 0; i < NW; i++)
				e[i] += h[i] * h[i];
		}
		const int pk = int(std::max_element(e.begin(), e.end()) - e.begin());
		if (pk > NW / 2)
			D = (NW - pk + 64 + tv_conv::B - 1) / tv_conv::B * tv_conv::B;
	}
	void start_ltv()
	{
		const long T = long(cv.time()) - D;      // 回している時刻（入口より D 遅れ）
		if (T < 0) {
			for (auto &s : slot)
				s.i = -1;
			return;
		}
		const long i0 = T / HOP + 1, i1 = (T + NW) / HOP;
		for (auto &s : slot)
			if (s.i >= 0 && (s.i < i0 || s.i > i1))
				s.i = -1;
		for (long i = i0; i <= i1; i++) {
			bool have = false;
			for (auto &s : slot)
				have |= s.i == i;
			if (have)
				continue;
			for (auto &s : slot) {
				if (s.i >= 0)
					continue;
				const long j = std::min<long>(F - 1, std::lround(motif_fx::loop_index(phi(i), perf, F)));
				std::vector<cplx> H(NB);
				std::vector<double> h[2];
				for (int c = 0; c < 2; c++) {
					const float *r = &raw[((size_t(j) * 2 + c) * NB) * 2];
					for (int q = 0; q < NB; q++)
						H[q] = cplx(r[q * 2], r[q * 2 + 1]);
					h[c] = ir(H.data());
				}
				cv.make(s.f, h);
				s.i = i;
				break;
			}
		}
		for (auto &s : slot)
			if (s.i >= 0)
				for (int c = 0; c < 2; c++)
					cv.tail(s.f, c, s.tl[c]);
	}
	// 出口の枠 i で読む録った枠の位置（小数）。速さを替えるまでは i のまま
	double phi(long i)
	{
		if (i_last < 0) {
			i_last = i;
			phi_last = double(i);
		}
		phi_last += double(i - i_last) * rate;
		i_last = i;
		return phi_last;
	}
	void start_dyn()
	{
		// 今までの NW サンプルにハン窓を掛けた大きさ（オフラインの「片側のスペクトルの 2 乗の平均」とほぼ同じ値: Σ(x w)²）
		const size_t t = cv.time(), mm = mono.size() - 1;
		double e = 0;
		for (int q = 0; q < NW; q++) {
			const long s = long(t) - NW + q;
			const double x = s >= 0 ? mono[size_t(s) & mm] : 0.0;
			e += x * x * win2[q];
		}
		const double db = 10 * std::log10(e + 1e-30);
		const int K = int(dbs.size());
		int j = int(std::upper_bound(dbs.begin(), dbs.end(), db) - dbs.begin()) - 1;
		j = std::max(0, std::min(K - 2, j));
		const double u = std::max(0.0, std::min(1.0, (db - dbs[j]) / (dbs[j + 1] - dbs[j])));
		// 区切りの中では、前の区切りの値から今の値へまっすぐ動かす（2 つの音量の間を越えたら、そこで切り替える）
		du0 = (u_prev < 0 || j != dj) ? u : u_prev;
		du1 = u;
		dj = j;
		u_prev = u;
		for (int c = 0; c < 2; c++) {
			cv.tail(lev[j], c, dtail[0][c]);
			cv.tail(lev[j + 1], c, dtail[1][c]);
		}
	}
};

// 音量や遅れを周期的に揺らす型（mod_type: Tremolo・Auto Pan・Rotary・Dual Rotor・Slice）を 1 サンプルずつ（modfx と同じ）。
// 1kHz で 2 つの帯に分け（2 次のバターワースを 2 回、双一次変換。オフラインの filter_freq と同じ応答）、録った A(t)・d(t) を掛けて足し、
// 周波数の形（.modeq か EQ）を掛ける。周波数の形は位相の無い応答なので負の時刻の PRE タップを先読みし、出口は PRE サンプル遅れる。
// 録った d(t) が負になる型（Rotary・Auto Pan 2 で −258 サンプルまで。元の音より先に出る分）は、その分も先読みして D サンプル遅らせて回す。
// 歪みを中に持つ Rotary の音量 2 段は、入口の包絡をオフラインは前後 25ms の RMS で測るが、こちらは今までの 50ms（近似）
class mod_rt {
public:
	static constexpr size_t PRE = 64;

	bool init(const block &b, const std::string &moddir, int tempo)
	{
		std::string k = "t" + std::to_string(b.msb) + "_" + std::to_string(b.lsb);
		for (int i = 1; i <= 16; i++)
			k += "_" + std::to_string(b.p[i]);
		k += "_" + std::to_string(b.is(14, 0x50) ? tempo : 0);
		// Dual Rotor は、回る位置で読む形（.rot）があればそれ（Speed Control の切り替えと、速さの坂も回す）
		use_rot = motif_fx::rot_type(b) && motif_fx::load_rot(moddir + "/" + motif_fx::rot_key(b) + ".rot", rot);
		if (use_rot) {
			k = motif_fx::rot_key(b);
			rm.init(rot, b);
			motif_fx::rot_balance(b.p[9], bal0);
			bal0[0] = std::max(bal0[0], 0.05);
			bal0[1] = std::max(bal0[1], 0.05);
			bg[0] = bg[1] = bgt[0] = bgt[1] = 1;
			m100 = motif_fx::mod_file();
		} else if (!motif_fx::load_mod(moddir + "/" + k + ".mod", m100))
			return false;
		stereo = !(b.msb == 8 && b.lsb >= 0x50 && b.lsb <= 0x72);
		std::vector<float> eqm;
		if (FILE *f = std::fopen((moddir + "/" + k + ".modeq").c_str(), "rb")) {
			eqm.resize(2 * 2049);
			if (std::fread(eqm.data(), sizeof(float), eqm.size(), f) != eqm.size())
				eqm.clear();
			std::fclose(f);
		}
		bool has_eq = false;
		motif_fx::mod_eq(b, 1000.0, has_eq);
		has_shape = !eqm.empty() || has_eq;
		auto shape = [&](double fq, int ch) -> cplx {
			if (!eqm.empty()) {
				const double x = fq / (FS / 4096.0);
				const int i = std::max(0, std::min(2047, int(x)));
				const double fr = std::max(0.0, std::min(1.0, x - i));
				return eqm[ch * 2049 + i] + (eqm[ch * 2049 + i + 1] - eqm[ch * 2049 + i]) * fr;
			}
			bool h;
			return motif_fx::mod_eq(b, fq, h);
		};
		for (int bd = 0; bd < 2; bd++)
			for (int ch = 0; ch < 2; ch++)
				nrm[bd][ch] = 1;
		// Dual Rotor で録った形があれば、形の時間平均（rot_avg）で割る（modfx と同じ）
		const bool rot_norm = use_rot && !eqm.empty();
		motif_fx::rot_avg ra;
		if (rot_norm)
			ra.make(rot);
		if (has_shape)
			for (int ch = 0; ch < 2; ch++) {
				if (!rot_norm) {
					nrm[0][ch] = std::max(1e-3, std::abs(shape(220.0, ch)));
					nrm[1][ch] = std::max(1e-3, std::abs(shape(440.0 * std::exp2(27 / 12.0), ch)));
				}
				sh[ch].init(fir_from([&](double f) { return rot_norm ? shape(f, ch) / ra.at(ch, f) : shape(f, ch); }, 4096, PRE));
			}
		lv = !use_rot && b.msb == 8 && ((b.lsb >= 0x51 && b.lsb <= 0x53) || (b.lsb >= 0x61 && b.lsb <= 0x63)) &&
		     motif_fx::load_mod(moddir + "/" + k + "_L25.mod", m25) && m25.indb != m100.indb;
		m100.perf = motif_fx::loop_period(b, m100.rate, m100.period, m100.frames);
		// 揺れの深さ（Tremolo の AM Depth・Auto Pan・Auto Pan 2 の L/R Depth、p2）を替えて録った組（tools/motif/mr_fx_mod.py record --var）。つまみで深さが動いたら両隣を混ぜる
		gv.clear();
		gm.clear();
		gsel = -1;
		rate_on = false;
		rt_ = 1;
		tpos = 0;
		if (!use_rot && b.msb == 8 && (b.lsb == 0x00 || b.lsb == 0x10 || b.lsb == 0x11)) {
			std::vector<std::pair<int, motif_fx::mod_file>> g = {{b.p[2], m100}};
			for (int val : {0, 32, 64, 96, 127}) {
				if (val == b.p[2])
					continue;
				block v2 = b;
				v2.p[2] = val;
				std::string k2 = "t" + std::to_string(v2.msb) + "_" + std::to_string(v2.lsb);
				for (int i = 1; i <= 16; i++)
					k2 += "_" + std::to_string(v2.p[i]);
				motif_fx::mod_file m;
				if (motif_fx::load_mod(moddir + "/" + k2 + "_0.mod", m)) {
					m.perf = motif_fx::loop_period(v2, m.rate, m.period, m.frames);
					g.emplace_back(val, std::move(m));
				}
			}
			std::sort(g.begin(), g.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
			for (auto &e : g) {
				gv.push_back(e.first);
				gm.push_back(std::move(e.second));
			}
			gbase = b.p[2];
			if (gv.size() < 2) {
				gv.clear();
				gm.clear();
			}
		}
		m25.perf = motif_fx::loop_period(b, m25.rate, m25.period, m25.frames);
		// 2 つの帯に分ける 2 次のバターワース（双一次変換）。1kHz、Dual Rotor は motif_fx::rot_band と同じ 600Hz・400Hz
		auto bw = [](double fc, bool low, double c[5]) {
			const double K = std::tan(motif_fx::PI * fc / FS), nn = 1 / (1 + std::sqrt(2.0) * K + K * K);
			if (low)
				c[0] = K * K * nn, c[1] = 2 * K * K * nn, c[2] = K * K * nn;
			else
				c[0] = nn, c[1] = -2 * nn, c[2] = nn;
			c[3] = 2 * (K * K - 1) * nn, c[4] = (1 - std::sqrt(2.0) * K + K * K) * nn;
		};
		bw(use_rot ? motif_fx::ROT_LP_HZ : 1000.0, true, bc[0]);
		bw(use_rot ? motif_fx::ROT_HP_HZ : 1000.0, false, bc[1]);
		for (auto &r : ring)
			r.assign(1 << 15, 0.0);
		for (auto &s : st)
			s[0] = s[1] = 0;
		env.assign(size_t(0.05 * FS), 0.0);
		esum = 0;
		t = 0;
		double dmin = 0;
		std::vector<const motif_fx::mod_file *> all = {&m100, &m25};
		for (auto &m : gm)
			all.push_back(&m);
		for (const motif_fx::mod_file *m : all)
			for (int i = 0; i < m->frames; i++)
				for (int c = 1; c < 8; c += 2)
					dmin = std::min(dmin, double(m->raw[size_t(i) * 8 + c]));
		if (use_rot)
			dmin = std::min(dmin, motif_fx::rot_dmin(rot));
		D = size_t(std::ceil(-dmin)) + (dmin < 0 ? 1 : 0);
		return true;
	}
	int latency() const { return int(D) + (has_shape ? int(PRE) : 0); }
	bool rotor() const { return use_rot; }
	// Speed Control（p16）の切り替え（つまみからのずらしを足した値で）。速さは坂で移る
	void set_fast(bool f) { rm.fast = f; }
	// Low/High Balance（p9）: 帯ごとの倍率を、録った組の値からの比で（5ms ほどでなめらかに移す）
	// LFO Speed（p1）: 録った並びを読む速さ（録った組の速さとの比）
	void set_rate(double r)
	{
		if (!rate_on) {
			tpos = t >= D ? (double(t) - double(D)) / FS : 0.0;
			rate_on = true;
		}
		rt_ = std::max(0.0, r);
	}
	// 揺れの深さ（p2）: 録った深さの両隣の組を混ぜる（録った組の値なら元の組だけ）
	void set_depth(int v)
	{
		if (gv.empty())
			return;
		if (v == gbase) {
			gsel = -1;
			return;
		}
		size_t lo = 0;
		while (lo + 2 < gv.size() && gv[lo + 1] <= v)
			lo++;
		const double u = std::max(0.0, std::min(1.0, double(v - gv[lo]) / double(gv[lo + 1] - gv[lo])));
		gsel = int(lo);
		gw_ = u;
	}
	void set_balance(int v)
	{
		double g[2];
		motif_fx::rot_balance(v, g);
		bgt[0] = g[0] / bal0[0];
		bgt[1] = g[1] / bal0[1];
	}

	void tick(double &L, double &R)
	{
		const double xs[2] = {stereo ? L : (L + R) / 2, stereo ? R : (L + R) / 2};
		const size_t mask = ring[0].size() - 1;
		// 帯に分ける（2 次を 2 回。st は [ch × 2 + 帯][段] の状態）
		for (int ch = 0; ch < 2; ch++)
			for (int bd = 0; bd < 2; bd++) {
				double v = xs[ch];
				for (int s = 0; s < 2; s++)
					v = biquad(bc[bd], st[(ch * 2 + bd) * 2 + s], v);
				ring[ch * 2 + bd][t & mask] = v;
			}
		double o[2];
		if (use_rot)
			rot_core(o);
		else
			gsel < 0 ? core(m100, o) : core_mix(o);
		if (lv) {
			const double mo = (xs[0] + xs[1]) / 2;
			const size_t w = env.size(), ei = t % w;
			esum += mo * mo - env[ei];
			env[ei] = mo * mo;
			const double e = 10 * std::log10(std::max(esum, 0.0) / double(w) + 1e-20);
			double o25[2];
			core(m25, o25);
			const bool lo25 = m25.indb < m100.indb;
			const double lo_db = lo25 ? m25.indb : m100.indb, hi_db = lo25 ? m100.indb : m25.indb;
			const double u = std::max(0.0, std::min(1.0, (e - lo_db) / (hi_db - lo_db)));
			for (int ch = 0; ch < 2; ch++) {
				const double olo = lo25 ? o25[ch] : o[ch], ohi = lo25 ? o[ch] : o25[ch];
				o[ch] = (1 - u) * olo + u * ohi;
			}
		}
		if (has_shape) {
			o[0] = sh[0].tick(o[0]);
			o[1] = sh[1].tick(o[1]);
		}
		L = o[0];
		R = o[1];
		if (rate_on && t >= D)
			tpos += rt_ / FS;
		t++;
	}

private:
	motif_fx::mod_file m100, m25;
	bool rate_on = false;                    // LFO Speed をつまみで替えたか（替えるまでは時刻から読む）
	double rt_ = 1, tpos = 0;                // 読む速さと、読んでいる位置（録った秒）
	motif_fx::rot_file rot;
	motif_fx::rotor_motion rm;
	bool stereo = true, has_shape = false, lv = false, use_rot = false;
	double nrm[2][2] = {{1, 1}, {1, 1}};
	double bal0[2] = {1, 1}, bg[2] = {1, 1}, bgt[2] = {1, 1};   // Low/High Balance: 録った組の倍率・今の比・目指す比
	std::vector<int> gv;                     // 揺れの深さを替えて録った組の深さ（小さい順、元の組も入る）
	std::vector<motif_fx::mod_file> gm;      // その組
	int gsel = -1, gbase = 0;                // 混ぜる両隣の小さいほう（−1 なら元の組だけ）・元の組の深さ
	double gw_ = 0;
	conv_rt sh[2];
	double bc[2][5] = {};                    // 帯（低い・高い）ごとの 2 次の係数（b0 b1 b2 a1 a2）
	double st[8][2] = {};
	std::vector<double> ring[4], env;
	double esum = 0;
	size_t t = 0, D = 0;

	double biquad(const double *b, double (&s)[2], double x)
	{
		// 転置直接形 II
		const double y = b[0] * x + s[0];
		s[0] = b[1] * x - b[3] * y + s[1];
		s[1] = b[2] * x - b[4] * y;
		return y;
	}
	double series(const motif_fx::mod_file &m, int col) const
	{
		const double u = motif_fx::loop_index((rate_on ? tpos : (double(t) - double(D)) / FS) * m.rate, m.perf, m.frames);
		const int i = int(u);
		const double fr = u - i;
		const float a0 = m.raw[size_t(i) * 8 + col], a1v = m.raw[size_t(std::min(i + 1, m.frames - 1)) * 8 + col];
		return a0 + (a1v - a0) * fr;
	}
	// Dual Rotor（rot_core と同じ）: 帯・左右ごとに、ローターとホーンの成分の A × 入口(τ − d)。位置は回している時刻で進める
	void rot_core(double o[2])
	{
		const size_t mask = ring[0].size() - 1;
		o[0] = o[1] = 0;
		if (t < D)
			return;
		const size_t tau = t - D;
		for (int bd = 0; bd < 2; bd++)
			bg[bd] += (bgt[bd] - bg[bd]) * 0.005;
		for (int bd = 0; bd < 2; bd++)
			for (int ch = 0; ch < 2; ch++) {
				double A[2], d[2];
				motif_fx::rot_taps(rot, rm, bd, ch, A, d);
				A[0] *= bg[bd];
				A[1] *= bg[bd];
				const std::vector<double> &xb = ring[ch * 2 + bd];
				for (int c = 0; c < 2; c++) {
					const double pos = double(tau) - d[c];
					if (pos < 0)
						continue;
					const size_t k0 = size_t(pos);
					const double fr = pos - double(k0);
					const double v0 = xb[k0 & mask], v = k0 + 1 <= t ? v0 + (xb[(k0 + 1) & mask] - v0) * fr : v0;
					o[ch] += A[c] / nrm[bd][ch] * v;
				}
			}
		rm.step();
	}
	// 深さの両隣の組（gm[gsel]・gm[gsel + 1]）の A・d を gw_ で混ぜて回す
	void core_mix(double o[2]) const
	{
		const size_t mask = ring[0].size() - 1;
		o[0] = o[1] = 0;
		if (t < D)
			return;
		const size_t tau = t - D;
		const motif_fx::mod_file &m0 = gm[size_t(gsel)], &m1 = gm[size_t(gsel) + 1];
		for (int bd = 0; bd < 2; bd++)
			for (int ch = 0; ch < 2; ch++) {
				const int ca = bd * 4 + ch * 2;
				const double A = ((1 - gw_) * series(m0, ca) + gw_ * series(m1, ca)) / nrm[bd][ch];
				const double d = (1 - gw_) * series(m0, ca + 1) + gw_ * series(m1, ca + 1);
				const double pos = double(tau) - d;
				if (pos < 0)
					continue;
				const size_t k0 = size_t(pos);
				const double fr = pos - double(k0);
				const std::vector<double> &xb = ring[ch * 2 + bd];
				const double v0 = xb[k0 & mask], v = k0 + 1 <= t ? v0 + (xb[(k0 + 1) & mask] - v0) * fr : v0;
				o[ch] += A * v;
			}
	}
	void core(const motif_fx::mod_file &m, double o[2]) const
	{
		const size_t mask = ring[0].size() - 1;
		o[0] = o[1] = 0;
		if (t < D)
			return;
		const size_t tau = t - D;               // 回している時刻（入口より D 遅れ）
		for (int bd = 0; bd < 2; bd++)
			for (int ch = 0; ch < 2; ch++) {
				const double A = series(m, bd * 4 + ch * 2) / nrm[bd][ch], d = series(m, bd * 4 + ch * 2 + 1);
				const double pos = double(tau) - d;
				if (pos < 0)
					continue;
				const size_t k0 = size_t(pos);
				const double fr = pos - double(k0);
				const std::vector<double> &xb = ring[ch * 2 + bd];
				const double v0 = xb[k0 & mask], v = k0 + 1 <= t ? v0 + (xb[(k0 + 1) & mask] - v0) * fr : v0;
				o[ch] += A * v;
			}
	}
};

// 双一次変換の 1 次・2 次のフィルター（オフラインの filter_freq に渡す式と同じ応答）
struct iir1 {
	double b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
	void hpf(double fc)
	{
		const double K = std::tan(motif_fx::PI * fc / FS);
		b0 = 1 / (1 + K), b1 = -b0, a1 = (K - 1) / (K + 1);
	}
	void lpf(double fc)
	{
		const double K = std::tan(motif_fx::PI * fc / FS);
		b0 = K / (1 + K), b1 = b0, a1 = (K - 1) / (K + 1);
	}
	double tick(double x)
	{
		const double y = b0 * x + b1 * x1 - a1 * y1;
		x1 = x, y1 = y;
		return y;
	}
};
struct iir2 {
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;
	void lpf(double fc, double q)
	{
		const double K = std::tan(motif_fx::PI * fc / FS), nn = 1 / (1 + K / q + K * K);
		b0 = K * K * nn, b1 = 2 * b0, b2 = b0, a1 = 2 * (K * K - 1) * nn, a2 = (1 - K / q + K * K) * nn;
	}
	// Butterworth の 2 次の HPF（s² / (s² + √2 s + 1)、角 fc で前ゆがみ）
	void hpf(double fc)
	{
		const double K = std::tan(motif_fx::PI * fc / FS), nn = 1 / (1 + std::sqrt(2.0) * K + K * K);
		b0 = nn, b1 = -2 * nn, b2 = nn, a1 = 2 * (K * K - 1) * nn, a2 = (1 - std::sqrt(2.0) * K + K * K) * nn;
	}
	// (n0 + n1 z⁻¹ + n2 z⁻²) / (d0 + d1 z⁻¹ + d2 z⁻²)。係数だけ替え、覚えている音（s1・s2）は残す
	void set(double n0, double n1, double n2, double d0, double d1, double d2)
	{
		b0 = n0 / d0, b1 = n1 / d0, b2 = n2 / d0, a1 = d1 / d0, a2 = d2 / d0;
	}
	double tick(double x)
	{
		const double y = b0 * x + s1;
		s1 = b1 * x - a1 * y + s2;
		s2 = b2 * x - a2 * y;
		return y;
	}
};

// 3 Band EQ・2 Band EQ（eq3・eq2 と同じ式の IIR。2026-10-30 まではその周波数応答から作った 4096 タップのたたみ込み）。
// set は係数だけ替えるので、鳴っている途中でつまみで値が動いても音が途切れない
class eq_iir {
public:
	void set(const block &b, bool three)
	{
		n = 0;
		auto shelf = [&](bool low, double f0, double g) {
			const double A = std::pow(10.0, g / 20), K = std::tan(motif_fx::PI * std::min(f0, FS * 0.49) / FS), r = std::sqrt(A);
			for (int c = 0; c < 2; c++)
				low ? s[n][c].set(K * r + 1, K * r - 1, 0, K / r + 1, K / r - 1, 0) : s[n][c].set(r + K, K - r, 0, 1 / r + K, K - 1 / r, 0);
			n++;
		};
		if (three) {
			if (b.p[1] != 64)
				shelf(true, motif_fx::t3(b.p[6]) * std::sqrt(2.0), b.p[1] - 64);
			if (b.p[5] != 64)
				shelf(false, motif_fx::t3(b.p[7]) / std::sqrt(2.0), b.p[5] - 64);
			const int g = b.p[3] - 64;
			if (g != 0) {
				const double A = std::pow(10.0, g / 20.0), w = 2 * motif_fx::PI * motif_fx::t3(b.p[2]) / FS;
				const double al = std::sin(w) / (2 * std::max(0.1, b.p[4] / 10.0)), c = std::cos(w);
				double n0 = 1 + al, n2 = 1 - al, d0 = 1 + al, d2 = 1 - al;
				if (g > 0) {
					n0 = 1 + al * A; n2 = 1 - al * A;
				} else {
					d0 = 1 + al / A; d2 = 1 - al / A;
				}
				for (int ch = 0; ch < 2; ch++)
					s[n][ch].set(n0, -2 * c, n2, d0, -2 * c, d2);
				n++;
			}
		} else
			for (int band = 0; band < 2; band++) {
				const int g = b.p[band == 0 ? 2 : 4] - 64;
				if (g == 0)
					continue;
				const double r = std::sqrt(std::pow(10.0, std::abs(g) / 20.0));
				shelf(band == 0, motif_fx::t3(b.p[band == 0 ? 1 : 3]) * (band == 0 ? r : 1 / r), g);
			}
	}
	void tick(double &L, double &R)
	{
		for (int k = 0; k < n; k++) {
			L = s[k][0].tick(L);
			R = s[k][1].tick(R);
		}
	}

private:
	int n = 0;
	iir2 s[3][2];
};

// エフェクトのパラメーターの値の幅（capture/fx_params.txt。Data List の Effect Parameter List から取ったもの）。型の番号（MSB × 128 + LSB）→ パラメーター 1〜16。
// Hall 1〜Plate は、コーラス・インサーションの枠では番号 + 100000 の行。読めなければ空（0〜127 として扱う）
struct prange {
	int min = 0, max = 127;
	std::string name;
};
using ptable = std::map<int, std::array<prange, 17>>;
inline const ptable &param_table(const std::string &path)
{
	static std::mutex mtx;
	static std::map<std::string, ptable> cache;
	std::lock_guard<std::mutex> lk(mtx);
	auto it = cache.find(path);
	if (it != cache.end())
		return it->second;
	ptable &t = cache[path];
	if (FILE *f = std::fopen(path.c_str(), "rb")) {
		char line[512];
		while (std::fgets(line, sizeof line, f)) {
			int code = 0, no = 0, lo = 0, hi = 0, n = 0;
			if (line[0] == '#' || std::sscanf(line, "%d %d %d %d %n", &code, &no, &lo, &hi, &n) < 4 || no < 1 || no > 16)
				continue;
			std::string name = line + n;
			name = name.substr(0, name.find('|'));
			t[code][no] = {lo, hi, name};
		}
		std::fclose(f);
	}
	return t;
}

// Lo-Fi（0D 00）を 1 サンプルずつ（オフラインの lofi と同じ）: 20.5Hz の 1 次の HPF → 2 次の Pre-LPF → 標本化の間引き → スルーレートの制限（語長）→
// Filter Type の録った応答 → Output Gain → 元の音 cos・wet sin。2026-10-30 まではその場では素通しだった（プリセットの 22 音色）
class lofi_rt {
public:
	void init(const block &b, const std::string &lfdir)
	{
		for (int c = 0; c < 2; c++) {
			hp[c] = iir1();
			hp[c].hpf(20.5);
			lp[c] = iir2();
			v[c] = prev[c] = acc[c] = held[c] = 0;
		}
		has_h = false;
		if (b.p[5] >= 1 && b.p[5] <= 5)
			if (FILE *f = std::fopen((lfdir + "/lofi_type" + std::to_string(b.p[5]) + ".ir").c_str(), "rb")) {
				std::vector<float> raw(4096);
				const size_t m = std::fread(raw.data(), sizeof(float), raw.size(), f);
				std::fclose(f);
				std::vector<double> h(raw.begin(), raw.begin() + long(m));
				if (!h.empty()) {
					fir[0].init(h);
					fir[1].init(h);
					has_h = true;
				}
			}
		set(b);
		t = 0;
	}
	// 値だけ替える（覚えている音は残す。Filter Type（p5）の応答は init のときのまま）
	void set(const block &b)
	{
		stereo = b.p[15] == 1;
		k = b.p[1] + 1;
		const double fc = std::min(motif_fx::t3(b.p[4]), FS * 0.49), q = std::max(1, b.p[6]) / 10.0;
		for (int c = 0; c < 2; c++)
			lp[c].lpf(fc, q);
		slew = 0;
		if (b.p[2] < 127) {
			static const double wx[8] = {1, 2, 4, 8, 16, 32, 64, 96}, wy[8] = {0.00209, 0.00222, 0.00248, 0.00300, 0.00405, 0.00614, 0.01279, 0.02606};
			double ly[8];
			for (int i = 0; i < 8; i++)
				ly[i] = std::log(wy[i]);
			slew = std::exp(motif_fx::interp(wx, ly, 8, b.p[2])) * std::exp2(b.p[7]);
		}
		a = b.p[8] ? 0.88 : 0.0;
		g = std::pow(10.0, (b.p[3] - 6) / 20.0);
		const int vv = std::max(1, b.p[10]);
		gd = std::cos(motif_fx::PI / 2 * (vv - 1) / 126.0);
		gw = std::sin(motif_fx::PI / 2 * (vv - 1) / 126.0);
	}
	void tick(double &L, double &R)
	{
		const double xs[2] = {stereo ? L : (L + R) / 2, stereo ? R : (L + R) / 2};
		double o[2];
		for (int c = 0; c < 2; c++) {
			double y = lp[c].tick(hp[c].tick(xs[c]));
			if (k > 1) {
				if (t % k == 0)
					held[c] = y;
				y = held[c];
			}
			if (slew > 0) {
				double xi = y;
				if (a) {
					const double e = (xi - (t ? a * prev[c] : 0.0)) / (1 - a);
					prev[c] = xi;
					xi = e;
				}
				const double d = xi - v[c];
				v[c] += d > slew ? slew : (d < -slew ? -slew : d);
				if (a) {
					acc[c] = (1 - a) * v[c] + a * acc[c];
					y = acc[c];
				} else
					y = v[c];
			}
			if (has_h)
				y = fir[c].tick(y);
			o[c] = y * g;
		}
		L = gd * L + gw * o[0];
		R = gd * R + gw * o[1];
		t++;
	}

private:
	bool stereo = false, has_h = false;
	int k = 1;
	unsigned long long t = 0;
	double slew = 0, a = 0, g = 1, gd = 1, gw = 0;
	double v[2] = {}, prev[2] = {}, acc[2] = {}, held[2] = {};
	iir1 hp[2];
	iir2 lp[2];
	conv_rt fir[2];
};

// Ring Modulator（0F 70）を 1 サンプルずつ（オフラインの ring_mod と同じ）: 1 次の HPF・LPF → 搬送波（表 19 + 1.70Hz × Fine、LFO で 2^(±0.35 × 深さ / 64)）を掛ける →
// EQ（13〜16）→ 元の音 cos・wet sin。2026-10-30 まではその場では素通しだった（プリセットの 17 音色）
class ringmod_rt {
public:
	void init(const block &b)
	{
		for (int c = 0; c < 2; c++) {
			hp[c] = iir1();
			lp[c] = iir1();
		}
		ph0 = 0;
		t = 0;
		set(b);
		has_eq = !(b.p[14] == 64 && b.p[16] == 64);
		if (has_eq) {
			block q;
			q.p[1] = b.p[13], q.p[2] = b.p[14], q.p[3] = b.p[15], q.p[4] = b.p[16];
			const auto h = fir_from([q](double f) { return motif_fx::eq2(q, f); });
			eq[0].init(h);
			eq[1].init(h);
		}
		accp = 0;
	}
	// 値だけ替える（覚えている音と搬送波の位相は残す。EQ（13〜16）は init のときのまま）
	void set(const block &b)
	{
		const int vv = std::max(1, b.p[10]);
		on = vv > 1;
		gd = std::cos(motif_fx::PI / 2 * (vv - 1) / 126.0);
		gw = std::sin(motif_fx::PI / 2 * (vv - 1) / 126.0);
		f0 = motif_fx::TABLE19[std::min(127, b.p[1])] + 1.70 * b.p[2];
		const double nhz = motif_fx::LFO_HZ[std::min(127, b.p[5])];
		ph0 += double(t) / FS * (hz - nhz);       // LFO の位相が飛ばないように（初めは 0 のまま）
		hz = nhz;
		sine = b.p[3] == 1;
		depth = b.p[4];
		for (int c = 0; c < 2; c++) {
			hp[c].hpf(motif_fx::t3(b.p[6]));
			lp[c].lpf(std::min(motif_fx::t3(b.p[7]), FS * 0.49));
		}
	}
	void tick(double &L, double &R)
	{
		if (!on) {
			t++;
			return;
		}
		double ph = std::fmod(double(t) / FS * hz + ph0, 1.0);
		if (ph < 0)
			ph += 1.0;
		const double m = sine ? std::sin(2 * motif_fx::PI * ph) : (ph < 0.5 ? 1.0 : -1.0);
		accp += f0 * std::exp2(0.35 * depth / 64.0 * m);
		const double car = 1.07 * std::cos(2 * motif_fx::PI * accp / FS);
		double o[2];
		for (int c = 0; c < 2; c++) {
			o[c] = lp[c].tick(hp[c].tick(c == 0 ? L : R)) * car;
			if (has_eq)
				o[c] = eq[c].tick(o[c]);
		}
		L = gd * L + gw * o[0];
		R = gd * R + gw * o[1];
		t++;
	}

private:
	bool on = false, sine = false, has_eq = false;
	int depth = 0;
	unsigned long long t = 0;
	double gd = 1, gw = 0, f0 = 0, hz = 0, accp = 0, ph0 = 0;
	iir1 hp[2], lp[2];
	conv_rt eq[2];
};

// エフェクトの 1 つのブロック（インサーションの A・B、コーラスの枠のほかの型）を 1 サンプルずつ。入れた左右 → 出口の左右（その場で書き換える）。
// 回せる型: 3 Band EQ・2 Band EQ（0F 00・0F 01）、Harmonic Enhancer（0F 10）、Cross Delay（02 30・02 60）、コンプ（0A 00・0A 08）、
// インサーションのコーラス（05 00 系）、フランジャー（06 00・06 01）、Ensemble Detune（05 30）、Pitch Change 1・2（0B 00・01）。ほかは素通し（supported() が偽）
class block_rt {
public:
	// fxddir は capture の置き場の fxd（録った応答があるかを見るため。空なら見ない）
	void init(const block &b, int tempo, const std::string &fxddir = std::string())
	{
		kind = 0;
		eq_on = false;
		wah_on = false;
		blk_ = base_ = b;
		tempo_ = tempo;
		ph_off = 0;
		hz = 0;
		pc_off[0] = pc_off[1] = pc_hz[0] = pc_hz[1] = 0;
		dryeq_on = false;
		mixb = mixbt = 0;
		for (int i = 1; i <= 16; i++)
			rng_[i] = prange();
		if (!fxddir.empty()) {
			const ptable &pt = param_table(fxddir + "/../fx_params.txt");
			const int code = b.msb * 128 + b.lsb;
			auto it = pt.find(code + 100000);
			if (it == pt.end())
				it = pt.find(code);
			if (it != pt.end())
				for (int i = 1; i <= 16; i++)
					rng_[i] = it->second[i];
		}
		approx_ = !fxddir.empty() && (motif_fx::chorus_type(b) || motif_fx::flanger_type(b)) && has_ltv(b, fxddir + "/../fxltv", tempo);
		if (b.msb == 0)
			return;                          // thru
		ring_l.assign(1 << 16, 0.0);
		ring_r.assign(1 << 16, 0.0);
		t = 0;
		stereo_in = b.p[15] == 1;
		if (approx_ && tv.init_ltv(b, fxddir + "/../fxltv", tempo)) {
			// コーラス系・フランジャーも、録った応答（.ltv）があればオフラインと同じくそれを使う（2026-10-27。それまでは式で回す近似）
			approx_ = false;
			kind = 10;
		} else if (b.is(15, 0) || b.is(15, 1)) {
			eqf = eq_iir();
			kind = 1;
			coeffs(b);
		} else if (b.is(15, 0x10)) {
			for (int c = 0; c < 2; c++) {
				en1[c] = iir1();
				en2[c] = iir2();
			}
			kind = 7;
			coeffs(b);
		} else if (b.is(2, 0x30) || b.is(2, 0x60)) {
			cd.init(b, b.is(2, 0x60) ? tempo : 0);
			kind = 2;
		} else if (b.is(10, 0) || b.is(10, 8)) {
			win = int(0.005 * FS);
			sq.assign(win, 0.0);
			acc = 0;
			g = 0;
			kind = 3;
			coeffs(b);
		} else if (b.is(5, 0x30)) {
			eq_init(b, 11);
			kind = 6;
			coeffs(b);
		} else if (motif_fx::chorus_type(b)) {
			eq_init(b, 6);
			kind = 4;
			coeffs(b);
		} else if (motif_fx::flanger_type(b)) {
			fl_y[0].assign(1 << 16, 0.0);
			fl_y[1].assign(1 << 16, 0.0);
			eq_init(b, 6);
			kind = 5;
			coeffs(b);
		} else if (b.is(11, 0) || b.is(11, 1)) {
			kind = 12;
			coeffs(b);
		} else if (b.is(12, 0x40) && !fxddir.empty() && wdd_init(b, fxddir)) {
			kind = 8;                        // Wah+Dist+Dly（wah_dist_delay と同じ）: 歪みの枠に、頭打ちの前のワウと混ぜたあとのこだまを足して回す
		} else if (motif_fx::dist_type(b) && !fxddir.empty() && dist_init(b, fxddir)) {
			kind = 8;
		} else if (motif_fx::lti_type(b) && !fxddir.empty() && lti_init(b, fxddir + "/../fxlti", tempo)) {
			kind = 9;
		} else if (motif_fx::dyn_type(b) && !fxddir.empty() && tv.init_dyn(b, fxddir + "/../fxdyn")) {
			kind = 10;
		} else if (motif_fx::ltv_type(b) && !fxddir.empty() && tv.init_ltv(b, fxddir + "/../fxltv", tempo)) {
			kind = 10;
		} else if (motif_fx::mod_type(b) && !fxddir.empty() && md.init(b, fxddir + "/../fxmod", tempo)) {
			kind = 11;
		} else if (b.is(13, 0)) {
			lf.init(b, fxddir + "/../fxlf");
			kind = 13;
		} else if (b.is(15, 0x70)) {
			rmod.init(b);
			kind = 14;
		} else
			kind = -1;                       // まだ回せない型（素通し）
	}
	bool supported() const { return kind >= 0; }
	bool thru() const { return kind == 0; }
	// つまみ（コントロールセット）で動いたパラメーター p（1〜16）の値 v を、鳴らしている途中で効かせる（覚えている音は残す）。
	// 式で回す型（EQ・Enhancer・Cross Delay・コンプ・コーラス・フランジャー・Ensemble Detune・Pitch Change・Lo-Fi・Ring Mod）はどのパラメーターも
	// （ただし揺れる型の EQ、Lo-Fi の Filter Type、Ring Mod の EQ は init のまま）、歪みは Dry/Wet・Output Level・Drive（AmpSimulator は Edge も）。
	// 録った応答の型: Dual Rotor は Speed Control と Low/High Balance、.ltv は Dry/Wet と LFO Speed、.mod は LFO Speed と揺れの深さ（深さを替えて録った組があるとき）。
	// ほか（.lti、Dual Rotor の Drive など）は動かさない
	void set_param(int p, int v)
	{
		if (p < 1 || p > 16 || blk_.p[p] == v)
			return;
		blk_.p[p] = v;
		switch (kind) {
		case 1: case 3: case 4: case 5: case 6: case 7: case 12:
			coeffs(blk_);
			break;
		case 2:
			cd.set(blk_, blk_.is(2, 0x60) ? tempo_ : 0);
			break;
		case 8:
			dist_set(p);
			break;
		case 10:
			if (!tv.is_dyn())
				ltv_set(p);
			break;
		case 11:
			if (md.rotor() && p == 16)
				md.set_fast(v != 0);
			if (md.rotor() && p == 9)
				md.set_balance(v);
			if (!md.rotor() && p == 2)
				md.set_depth(v);
			if (!md.rotor() && rng_[p].name == "LFO Speed" && motif_fx::LFO_HZ[std::min(127, base_.p[p])] > 0)
				md.set_rate(motif_fx::LFO_HZ[std::min(127, v)] / motif_fx::LFO_HZ[std::min(127, base_.p[p])]);
			break;
		case 13:
			lf.set(blk_);
			break;
		case 14:
			rmod.set(blk_);
			break;
		}
	}
	// つまみのずらし raw（engine::ins_offset、Σ 深さ' × ソース'）で動いたパラメーター p の値: 音色の値 + raw × 値の数 / 4096 を 0 の側へ切り捨て、
	// 値の幅に収める。実機で測った（2026-10-30、tools/motif/mr_fx_comp.py --cs --ccat、録音は capture/csscale と rotmw）: Compressor の Output Level
	// （0〜127）・3 Band EQ の Low Gain（52〜76）・Dual Rotor の Speed Control（0・1）で、MW の深さ +1・+2・+4・+16・+32・+48・+63・−16・−32・−64、
	// AS1 の深さ +16 の 90 点ほどがどれもこの式どおり（四捨五入や × 4 / 127 では合わない点がある）
	int ctrl_value(int p, int raw) const
	{
		if (p < 1 || p > 16)
			return 0;
		if (raw == 0)
			return base_.p[p];
		const prange &r = rng_[p];
		const long long n = r.max - r.min + 1;
		return std::max(r.min, std::min(r.max, base_.p[p] + int(raw * n / 4096)));
	}
	bool approx() const { return approx_; }
	// この枠の出口の遅れ（サンプル。歪みだけ DIST_PRE）
	int latency() const { return kind == 8 ? int(DIST_PRE) : kind == 10 ? tv.latency() : kind == 11 ? md.latency() : 0; }
	// 録った応答をその場の形に直して回す型（オフラインとは少し違う。ltv_rt・mod_rt）
	bool recorded() const { return kind == 10 || kind == 11; }
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
			eqf.tick(L, R);
			break;
		case 7:
			L = std::max(-1.0, std::min(1.0, L + eng * en2[0].tick(en1[0].tick(L))));
			R = std::max(-1.0, std::min(1.0, R + eng * en2[1].tick(en1[1].tick(R))));
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
			double ph = std::fmod(double(t) * hz / FS + ph_off, 1.0);
			if (ph < 0)
				ph += 1.0;
			const double tri = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
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
				double ph = std::fmod(double(t) * hz / FS + ph0 + ph_off, 1.0);
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
			// 歪み（distortion と同じ）: 左右を足した音 → 録った応答 → 柔らかい頭打ち → 2 次の LPF → 元の音と混ぜる →（V-Distortion はディレイ）。
			// LPF は負の時刻のタップ（DIST_PRE）を頭へ回したので、出口はこの枠の中で DIST_PRE サンプル遅れる（元の音も遅らせて混ぜる）
			const double x = wah_on ? wah.tick((L + R) / 2) : (L + R) / 2;
			ring_l[t & mask] = L;
			ring_r[t & mask] = R;
			const double L0 = t >= DIST_PRE ? ring_l[(t - DIST_PRE) & mask] : 0.0, R0 = t >= DIST_PRE ? ring_r[(t - DIST_PRE) & mask] : 0.0;
			double y[2];
			for (int c = 0; c < 2; c++) {
				double u = (c == 0 ? fl : fr).tick(x) * dk + dbs;
				u = u / std::pow(1 + std::pow(std::fabs(u / dc), dp), 1 / dp) - db0;
				y[c] = (c == 0 ? el : er).tick(u);
			}
			L = gdry * L0 + gwet * y[0] * og;
			R = gdry * R0 + gwet * y[1] * og;
			if (vd_on)
				for (int c = 0; c < 2; c++) {
					double &o = c == 0 ? L : R;
					std::vector<double> &line = fl_y[c];
					const double back = t >= vd_df ? line[(t - vd_df) & mask] : 0.0;
					vd_lp[c] = (1 - vd_a) * back + vd_a * vd_lp[c];
					line[t & mask] = o + vd_fb * vd_lp[c];
					const size_t dt = vd_dt[c];
					o += vd_mix * (t >= dt ? line[(t - dt) & mask] : 0.0);
				}
			t++;
			break;
		}
		case 10:
			tv.tick(L, R);
			mixb += (mixbt - mixb) * 0.005;
			if (mixb != 0) {
				double dl = tv.delayed_in(0), dr = tv.delayed_in(1);
				dryeq.tick(dl, dr);
				L += mixb * dl;
				R += mixb * dr;
			}
			break;
		case 11:
			md.tick(L, R);
			break;
		case 13:
			lf.tick(L, R);
			break;
		case 14:
			rmod.tick(L, R);
			break;
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
		case 12: {
			// 入口は左右の平均
			ring_l[t & mask] = (L + R) / 2;
			double oL = 0, oR = 0;
			for (int c = 0; c < 2; c++) {
				const double rate = pc_rate[c], lo = pc_lo[c], span = pc_span[c];
				double y;
				if (std::fabs(rate) < 1e-9)
					y = at0(ring_l, double(t) - lo);
				else {
					double sw = std::fmod(double(t) * pc_hz[c] / FS + (c == 0 ? 0.0 : 0.5) + pc_off[c], 1.0);
					if (sw < 0)
						sw += 1.0;
					const double s1 = rate > 0 ? sw : 1 - sw;
					const double d1 = lo + span * s1, d2 = std::max(0.0, rate > 0 ? lo + span * (s1 - 1) : lo + span * (s1 + 1));
					const double u = std::min(1.0, std::max(0.0, (sw - 0.75) / 0.25));
					y = (1 - u) * at0(ring_l, double(t) - d1) + u * at0(ring_l, double(t) - d2);
				}
				oL += y * pc_gl[c];
				oR += y * pc_gr[c];
			}
			L = gd * L + gw * oL;
			R = gd * R + gw * oR;
			t++;
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
	double pc_rate[2] = {0, 0}, pc_lo[2] = {0, 0}, pc_span[2] = {0, 0}, pc_hz[2] = {0, 0}, pc_gl[2] = {0, 0}, pc_gr[2] = {0, 0};
	// オフラインの LPF（filter_freq の 2 次）の応答には先の音を使う尾がある。角がナイキストに近い組（V-Distortion の 20000Hz）では 1/n でゆっくり減り、
	// 先読み 64・256・1024 で差が 1.3e-3・3.4e-4・9.8e-5。弾くときの遅れを小さくしたいので 64（1.45ms）
	conv_rt l4[4];
	bool lti_thru = false;
	ltv_rt tv;
	mod_rt md;
	eq_iir dryeq;                            // .ltv の Dry/Wet で足し引きする元の音に掛ける型の EQ
	bool dryeq_on = false;
	double mixb = 0, mixbt = 0;              // その倍率（今・目指す）
	lofi_rt lf;
	ringmod_rt rmod;
	block blk_, base_;                        // 今の値（つまみで動いたあと）と、init のときの値
	int tempo_ = 0;
	prange rng_[17];
	eq_iir eqf;
	iir1 en1[2];
	iir2 en2[2];
	double eng = 0, ph_off = 0, pc_off[2] = {0, 0};
	double og0 = 1, dk0 = 1, dp_file = 2, dist_bh = 0, dist_sgn = 1;
	bool per_set_ = false;

	// 揺れの速さを替えても位相が飛ばないように（初めは ph_off = 0 のまま、オフラインと同じ）
	void set_hz(double nh)
	{
		ph_off += double(t) / FS * (hz - nh);
		hz = nh;
	}
	// 式で回す型の値（init と set_param から。覚えている音には触らない）
	void coeffs(const block &b)
	{
		const int v = std::max(1, b.p[10]);
		switch (kind) {
		case 1:
			eqf.set(b, b.is(15, 0));
			break;
		case 7: {
			// Harmonic Enhancer: 1 + g × HPF（1 次 × Butterworth の 2 次、角 fc）のあと ±1.0 で頭打ち
			const double fc = std::min(motif_fx::t3(b.p[1]), FS * 0.49);
			for (int c = 0; c < 2; c++) {
				en1[c].hpf(fc);
				en2[c].hpf(fc);
			}
			eng = 8.13 * b.p[2] / 64.0 * b.p[3] / 64.0;
			break;
		}
		case 3:
			T = b.p[3] - 127;
			Rr = motif_fx::COMP_RATIO[std::min(7, b.p[4])];
			ka = std::exp(-1.0 / (1.6 * motif_fx::COMP_ATTACK_MS[std::min(19, b.p[1])] / 1000 * FS));
			rel = 7.5 / (motif_fx::COMP_RELEASE_MS[std::min(15, b.p[2])] / 1000) / FS;
			makeup = 40 * std::log10(std::max(1, b.p[5]) / 64.0);
			break;
		case 6: {
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
			break;
		}
		case 4: {
			// インサーションのコーラス（insertion_chorus と同じ）
			static const double vx[4] = {1, 32, 64, 127}, vy[4] = {0.0, 0.364, 0.838, 1.5};
			gd = std::cos(motif_fx::PI / 2 * (v - 1) / 126.0);
			gw = std::sqrt(motif_fx::interp(vx, vy, 4, v));
			set_hz(motif_fx::LFO_HZ[std::min(127, b.p[1])]);
			depth = 0.176 * b.p[2] / 1000 * FS;
			off = motif_fx::MOD_DELAY_MS[std::min(127, b.p[4])] / 1000 * FS;
			break;
		}
		case 5:
			// フランジャー（flanger と同じ）
			gd = std::cos(motif_fx::PI / 2 * (v - 1) / 126.0);
			gw = std::sin(motif_fx::PI / 2 * (v - 1) / 126.0);
			set_hz(motif_fx::LFO_HZ[std::min(127, b.p[1])]);
			depth = 0.0914 * b.p[2] / 1000 * FS;
			off = motif_fx::MOD_DELAY_MS[std::min(127, b.p[4])] / 1000 * FS;
			fb = (b.p[3] - 64) / 64.0;
			dph = (b.p[14] - 64) * 3.0 / 360.0;
			break;
		case 12: {
			// Pitch Change 1・2（pitch_change と同じ。2 は Fine をセントとして 1 の表を逆に引く）
			static const double fx[15] = {-50, -10, -3, 0, 3, 5, 6, 7, 8, 10, 15, 20, 30, 40, 50},
					    fy[15] = {-26.2, -10.79, -5.51, 0.0, 2.48, 5.13, 5.47, 7.88, 7.58, 10.59, 10.22, 10.06, 18.26, 23.77, 26.15};
			static const double gx[4] = {3, 5, 10, 50}, glo[4] = {2.1, 3.2, 4.6, 13.0}, gsp[4] = {11.4, 12.7, 16.7, 29.8};
			gd = std::cos(motif_fx::PI / 2 * (v - 1) / 126.0);
			gw = std::sin(motif_fx::PI / 2 * (v - 1) / 126.0);
			const int semi = b.p[1] - 64;
			for (int c = 0; c < 2; c++) {
				double fine = b.p[c == 0 ? 3 : 4] - 64;
				if (b.is(11, 1)) {
					double ys[15], xs[15];
					int ord[15];
					for (int i = 0; i < 15; i++)
						ord[i] = i;
					std::stable_sort(ord, ord + 15, [](int a2, int b2) { return fy[a2] < fy[b2]; });
					for (int i = 0; i < 15; i++) {
						ys[i] = fy[ord[i]];
						xs[i] = fx[ord[i]];
					}
					fine = motif_fx::interp(ys, xs, 15, fine);
				}
				const int pan = std::max(1, b.p[c == 0 ? 11 : 13]), lvl = b.p[c == 0 ? 12 : 14];
				const double cents = 100.0 * semi + motif_fx::interp(fx, fy, 15, fine);
				pc_rate[c] = 1 - std::exp2(cents / 1200.0);
				if (semi == 0) {
					const double af = std::min(50.0, std::max(3.0, std::fabs(fine)));
					pc_lo[c] = motif_fx::interp(gx, glo, 4, af) / 1000 * FS;
					pc_span[c] = motif_fx::interp(gx, gsp, 4, af) / 1000 * FS;
					if (fine == 0)
						pc_lo[c] = motif_fx::MOD_DELAY_MS[std::min(127, b.p[2])] / 1000 * FS;
				} else {
					pc_span[c] = 1280.0;
					pc_lo[c] = pc_span[c] / 4;
				}
				const double a = motif_fx::PI / 2 * (pan - 1) / 126.0, g = lvl / 127.0;
				pc_gl[c] = g * std::cos(a);
				pc_gr[c] = g * std::sin(a);
				const double nh = std::fabs(pc_rate[c]) * FS / pc_span[c];
				pc_off[c] += double(t) / FS * (pc_hz[c] - nh);     // 揺れの位相が飛ばないように（初めは 0 のまま）
				pc_hz[c] = nh;
			}
			break;
		}
		}
	}
	// 歪みの枠でつまみが動かしたパラメーター（名前で見る）。Output Level は (値 / init の値)²（40·log10、実機で確かめた曲線）、
	// Drive は init の値からの drive_gain_db の差（録った組から外れるので近似）。AmpSimulator を既定の応答で回しているときは頭打ちの形も替える
	// 録った応答（.ltv）で回す型: Dry/Wet は録った H を元の音と揺れの線に分けて混ぜ直し（H × a に、型の EQ を掛けた元の音を b だけ足す。
	// 録った組の値が 1 なら揺れの線が無いので動かさない）、LFO Speed は録った枠を進める速さを LFO_HZ の比に
	void ltv_set(int p)
	{
		const std::string &nm = rng_[p].name;
		const int v = blk_.p[p], v0 = base_.p[p];
		if (nm == "Dry / Wet Balance" || nm == "Dry/Wet Balance") {
			double gd, gw, gd0, gw0;
			mix_law(v, gd, gw);
			mix_law(v0, gd0, gw0);
			if (gw0 < 1e-3)
				return;
			if (!dryeq_on) {
				// 元の音の側にも、型の EQ（混ぜたあとに掛かる）を掛ける
				block q;
				for (int i = 1; i <= 16; i++) {
					const std::string &n = rng_[i].name;
					const int k = n == "EQ Low Frequency" ? 1 : n == "EQ Low Gain" ? 2 : n == "EQ High Frequency" ? 3 : n == "EQ High Gain" ? 4 : 0;
					if (k)
						q.p[k] = blk_.p[i];
				}
				if (q.p[2] == 0)
					q.p[2] = 64;
				if (q.p[4] == 0)
					q.p[4] = 64;
				dryeq.set(q, false);
				dryeq_on = true;
			}
			tv.set_scale(gw / gw0);
			mixbt = gd - gw / gw0 * gd0;
		} else if (nm == "LFO Speed") {
			const double h0 = motif_fx::LFO_HZ[std::min(127, v0)];
			if (h0 > 0)
				tv.set_rate(motif_fx::LFO_HZ[std::min(127, v)] / h0);
		}
	}
	// Dry/Wet の倍率: 元の音 cos・揺れの線 sin（角 π/2 × (v − 1) / 126）。Chorus 1（02_039 の組）を Dry/Wet 1・32・64・96・127 で録り、
	// 元の音の倍率（wet と dry の遅れ 0 の相関）と残りのパワーを読んだ: 元の音 1.10 × cos、揺れの線 1.35〜1.38 × sin（1.10 は EQ の高い棚）
	static void mix_law(int v, double &gd, double &gw)
	{
		v = std::max(1, std::min(127, v));
		gd = std::cos(motif_fx::PI / 2 * (v - 1) / 126.0);
		gw = std::sin(motif_fx::PI / 2 * (v - 1) / 126.0);
	}
	void dist_set(int p)
	{
		const std::string &nm = rng_[p].name;
		const int v = blk_.p[p], v0 = base_.p[p];
		const bool amp = blk_.is(9, 48) && !per_set_;
		if (nm == "Dry / Wet Balance") {
			const double ang = motif_fx::PI / 2 * (std::max(1, v) - 1) / 126.0;
			gdry = dist_sgn * std::cos(ang);
			gwet = std::sin(ang);
		} else if (nm == "Output Level")
			og = og0 * motif_fx::dist_level_gain(v, v0);
		else if (nm == "Drive") {
			dk = dk0 * std::pow(10.0, (motif_fx::drive_gain_db(v) - motif_fx::drive_gain_db(v0)) / 20);
			if (amp) {
				dc = motif_fx::drive_clip(v);
				dp = v >= 15 ? motif_fx::edge_p(blk_.p[11]) : dp_file;
			}
		} else if (nm == "Edge" && amp && blk_.p[1] >= 15)
			dp = motif_fx::edge_p(v);
		else
			return;
		dbs = dist_bh * dc;
		db0 = dbs / std::pow(1 + std::pow(std::fabs(dbs / dc), dp), 1 / dp);
	}

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
	double dbs = 0, db0 = 0;                 // 頭打ちの上下の違い（distortion と同じ）
	double dk = 1, dc = 1, dp = 2, og = 1, gdry = 1, gwet = 0, vd_fb = 0, vd_a = 0, vd_mix = 0, vd_lp[2] = {0, 0};
	size_t vd_df = 1, vd_dt[2] = {1, 1};
	bool vd_on = false, wah_on = false;
	motif_fx::touch_wah wah;

	// Wah+Dist+Dly（wah_dist_delay と同じ値）。こだまは V-Distortion の線を、LPF なし・左右同じ遅れで使う
	bool wdd_init(const block &b, const std::string &fxddir)
	{
		const motif_fx::stereo_ir ir = motif_fx::load_ir(fxddir + "/../fxds/" + motif_fx::wdd_key(b) + ".dsx");
		if (!ir.ok)
			return false;
		dc = std::atof(ir.head.at("c").c_str());
		dp = std::atof(ir.head.at("p").c_str());
		dk = 1.0;
		og = 1.0;
		const double ang = motif_fx::PI / 2 * (std::max(1, b.p[10]) - 1) / 126.0;
		gdry = std::cos(ang);
		gwet = std::sin(ang);
		fl.init(std::vector<double>(ir.L.begin(), ir.L.end()));
		fr.init(std::vector<double>(ir.R.begin(), ir.R.end()));
		dist_bh = std::atof(ir.head.count("b") ? ir.head.at("b").c_str() : "0");
		dbs = dist_bh * dc;
		dist_sgn = 1;
		dp_file = dp;
		per_set_ = true;
		og0 = og;
		dk0 = dk;
		db0 = dbs / std::pow(1 + std::pow(std::fabs(dbs / dc), dp), 1 / dp);
		std::vector<double> h(DIST_PRE, 0.0);
		if (ir.post.empty())
			h.push_back(1.0);
		else
			h.insert(h.end(), ir.post.begin(), ir.post.end());
		el.init(h);
		er.init(h);
		wah.init(b);
		wah_on = true;
		vd_on = b.p[3] > 0;
		if (vd_on) {
			vd_dt[0] = vd_dt[1] = vd_df = size_t(std::max(1, b.p[1])) * FS / 10000;
			vd_fb = (b.p[2] - 64) / 63.0;
			vd_a = 0;
			vd_mix = (b.p[3] / 127.0) * (b.p[3] / 127.0) / std::sqrt(2.0);
			vd_lp[0] = vd_lp[1] = 0;
			fl_y[0].assign(1 << 16, 0.0);
			fl_y[1].assign(1 << 16, 0.0);
		}
		return true;
	}

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
		int lev_ref = b.p[5];
		if (!b.is(9, 48)) {
			ir = motif_fx::load_dist_set(b, fxddir, lev_ref);
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
		og = per_set ? motif_fx::dist_level_gain(b.p[5], lev_ref) : 1.0;
		if (b.is(9, 48) && !per_set)
			og = std::pow(10.0, 40 * std::log10(std::max(1, b.p[4]) / 80.0) / 20);
		const double ang = motif_fx::PI / 2 * (std::max(1, b.p[10]) - 1) / 126.0;
		size_t pk = 0;
		for (size_t i = 0; i < ir.L.size(); i++)
			if (std::fabs(ir.L[i]) > std::fabs(ir.L[pk]))
				pk = i;
		dist_sgn = ir.L.empty() || ir.L[pk] >= 0 ? 1.0 : -1.0;
		gdry = dist_sgn * std::cos(ang);
		gwet = std::sin(ang);
		dp_file = std::atof(ir.head["p"].c_str());
		dist_bh = std::atof(ir.head["b"].c_str());
		per_set_ = per_set;
		og0 = og;
		dk0 = dk;
		fl.init(std::vector<double>(ir.L.begin(), ir.L.end()));
		fr.init(std::vector<double>(ir.R.begin(), ir.R.end()));
		dbs = std::atof(ir.head["b"].c_str()) * dc;
		db0 = dbs / std::pow(1 + std::pow(std::fabs(dbs / dc), dp), 1 / dp);
		std::vector<double> h;
		if (!ir.post.empty()) {
			// 頭打ちのあとの応答（オフラインと同じ）。出口を LPF の組と同じだけ（DIST_PRE）遅らせる
			h.assign(DIST_PRE, 0.0);
			h.insert(h.end(), ir.post.begin(), ir.post.end());
		} else
			h = fir_from([fc](double f) { return 1.0 / cplx(1 - (f / fc) * (f / fc), (f / fc) / 1.6); }, 4096, DIST_PRE);
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
		motif_fx::return_pan(true, ch.pan, pgl, pgr);
		if (motif_fx::chorus_type(ch)) {
			kind = 1;
			ring.assign(1 << 16, 0.0);
			hz = motif_fx::LFO_HZ[std::min(127, ch.p[1])];
			depth = 0.176 * ch.p[2] / 1000 * FS;
			off = motif_fx::MOD_DELAY_MS[std::min(127, ch.p[4])] / 1000 * FS;
			g = 2.06 * ch.ret / 96.0 * ch.send / 127.0;
			gp = 2.06 * ch.send / 127.0;
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

	// pl・pr を渡すと、Return を掛ける前の wet も返す（コーラスからリバーブへの送りは Return の前から。Return 0 でも送られた）
	void tick(double l, double r, double &ol, double &orr, double *pl = nullptr, double *pr = nullptr)
	{
		ol = orr = 0;
		if (pl)
			*pl = *pr = 0;
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
				(c == 0 ? ol : orr) += (x0 + (x1 - x0) * fr) * g * (c == 0 ? pgl : pgr);
				if (pl)
					*(c == 0 ? pl : pr) += (x0 + (x1 - x0) * fr) * gp;
			}
			t++;
		} else if (kind == 2) {
			// 送りを掛けた音を Cross Delay に通し、効いた分（出口 − 入れた音）に Return（sampler_fx.h の system_chorus、SYSCHO_WET）
			double L = l * gi, R = r * gi;
			const double dl = L, dr = R;
			bk.tick(L, R);
			ol = (L - dl) * go * pgl;
			orr = (R - dr) * go * pgr;
			if (pl) {
				*pl = L - dl;
				*pr = R - dr;
			}
		}
	}

private:
	int kind = 0;
	size_t t = 0;
	std::vector<double> ring;
	double hz = 0, depth = 0, off = 0, g = 0, gp = 0, gi = 0, go = 0, pgl = 1.0, pgr = 1.0;
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
		for (auto &w : last_)
			for (int &v : w)
				v = -1;
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
	// which: 0 = A、1 = B のパラメーター p（1〜16）をつまみで動かした値（前と同じなら何もしない）
	void set_param(int which, int p, int v)
	{
		if (which < 0 || which > 1 || p < 1 || p > 16 || last_[which][p] == v)
			return;
		last_[which][p] = v;
		(which ? Bk : A).set_param(p, v);
	}
	// つまみのずらし raw（engine::ins_offset）を A・B のパラメーター p の値にして渡す（block_rt::ctrl_value）
	void set_ctrl(int which, int p, int raw)
	{
		if (which < 0 || which > 1)
			return;
		set_param(which, p, (which ? Bk : A).ctrl_value(p, raw));
	}
	bool approx() const { return A.approx() || Bk.approx(); }
	bool recorded() const { return A.recorded() || Bk.recorded(); }
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
	int last_[2][17] = {};
	delay2 d0, d_in, da, db;
};

} // namespace motif_fx_rt
