// license:BSD-3-Clause
// MOTIF-RACK ES の録音だけで作るサンプリング方式のソフトシンセの、エレメント 1 つを鳴らす芯（doc/motif-rack-es-sampler.md）。
// サンプル（tools/motif/mr_extract_all.py で切り出した「出だし + ループ」か鳴らし切り）を、実機の決まりで読み、測った部品を通す。
//
//   読む速さ: 録った鍵を元の高さとし、鍵ごとに 1 オクターブ = 1024 目盛りへ丸める（実機と同じ。隣の鍵との差は 85 か 86 目盛り）。
//             コースチューン・ファインチューン（1 = 1.5625 セント）も目盛りに足す。3 倍で折り返す決まり（swp50.h）は、
//             本当の元の鍵が分からないので入れていない
//   補間    : 4 点（エルミート）
//   その後  : FEG つきのフィルター（swp50_filter.h）→ EQ（swp50_eq.h）→ エレメントの音量 → パン
//   音量    : swp50_aeg::level_db（測った表）。レベルスケーリングを足してからキーフォローを掛ける
//   音量の EG: swp50_aeg.h（高さ・段の長さ・時間のベロシティ感度とキーフォロー、音量のベロシティ感度とカーブ、音量のキーフォロー）
//   LFO     : swp50_lfo.h（音程・音量・フィルター）
//   音程    : PEG（FEG と同じ形の swp50_filter::filter_eg を swp50_peg.h の目盛りで）、音程のキーフォロー・ベロシティ感度
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "swp50_aeg.h"
#include "swp50_eg.h"
#include "swp50_eq.h"
#include "swp50_filter.h"
#include "swp50_lfo.h"
#include "swp50_peg.h"

namespace motif_sampler {

struct sample {
	std::vector<float> L, R;
	int loop_start = -1;          // -1 = 鳴らし切り
	int loop_length = 0;
	int key = 60;                 // 録った鍵
	double stretch = 1.0;         // 切り出しのときの伸び縮み（録った鍵の高さで鳴らすには、1 サンプルあたり 1 / stretch 進む）
};

// 音色のコモン LFO（40 06 00）。tools/motif/mr_clfo.py で測った:
// 速さ 0.824 × 2^((値 − 32) / 8) Hz（8・16・32・40・48・56・63 で 0.095・0.206・0.824・1.64・3.28・6.54・11.97Hz）。テンポ同期は表 14 の音符 × 60 / テンポ。
// PMD は 16 上がるごとに 2.2 倍（16・32・64・96・127 で ±9.4・21・102・486・2189 セント）。AMD は波形の負の側だけ効き、
// エレメントの音量の値（41 e 20）を 深さ × 振れ だけ下げる（ユーザー波形の −1 を深さ 32・64・96・113 で −4.5・−12.5・−24.5・−37.5dB、127 で無音。
// 音量 96 で深さ 64 なら −24dB、音量 64 で深さ 64・音量 96 で深さ 96 は無音。Eclipse の音量 100 は深さ 104 で無音）。エレメント側の割合（44 ee 51〜53）は深さに掛かる（深さ × 割合 / 127）。
// 遅れ（5）・フェードイン（6）は値に対して指数的（遅れ 64・96・127 で 0.18・2.03・11.2 秒、フェードイン 64・96・127 で 0.27・2.3・12 秒）。
// 波形: 0 三角（0 から上がる）・1 tri+（0〜1）・2 のこぎり上（0〜1）・3 のこぎり下・4〜8 方形（1/4・1/3・1/2・2/3・3/4、高い側から）・9 台形・10/11 S/H・12 ユーザー。
// 行き先（tools/motif/mr_clfo_dest.py）: FMD はカットオフを 2 × 深さ × 振れ 目盛り、RESO はレゾナンスを 深さ / 2 × 振れ、エレメント LFO の速さは
// 速さの値を 深さ / 2 × 振れ（8 で 2 倍）動かす。PAN は振れにまっすぐ比例するが、いちばん振れたときのパンのずれが深さ 8・16・32・64 で 18・33・48・63
// （倍にするごとに +15、64 から上は 63 のまま）。左右は同じ（三角で測った。ユーザー波形の −1 だけ左が浅かったが入れていない）。
// RESO の −1 の側は、もとが 64 以下だと 50 前後より下がらなかった（もと 96 では ±16 で対称）。入れていない。
// ホールド（7、127 は続ける）は遅れのあとから数え、64・83・96・113・126 で 0.45・1.25・2.3・5.6・11 秒。そのあとフェードアウト（8）で
// 揺れが dB でまっすぐ −30dB まで小さくなって止まる（64・94・123 で 0.45・2.2・9.3 秒）
struct common_lfo {
	bool on = false;
	int phase = 0, wave = 0, speed = 32, tspeed = 11, sync = 0, delay = 0, fadein = 0, reset = 1, hold = 127, fadeout = 0;
	int dest[3] = {0, 0, 0}, depth[3] = {0, 0, 0};
	int usteps = 0, uvals[16] = {0};
	int tempo = 120;
	double hz() const
	{
		if (sync) {
			static const double beats[17] = {0.25, 1 / 3.0, 0.375, 0.5, 2 / 3.0, 0.75, 1, 4 / 3.0, 1.5, 2, 8 / 3.0, 3, 4, 5, 6, 7, 8};
			const int i = std::max(0, std::min(16, tspeed - 5));
			return 1.0 / (beats[i] * 60.0 / std::max(1, tempo));
		}
		return 0.824 * std::exp2((speed - 32) / 8.0);
	}
	static double interp(const double *x, const double *y, int n, double v)
	{
		if (v <= x[0])
			return y[0];
		for (int i = 0; i < n - 1; i++)
			if (v <= x[i + 1])
				return y[i] + (v - x[i]) / (x[i + 1] - x[i]) * (y[i + 1] - y[i]);
		return y[n - 1];
	}
	static double delay_s(int v)
	{
		static const double x[6] = {0, 16, 32, 64, 96, 127}, y[6] = {0, 0.002, 0.02, 0.18, 2.03, 11.2};
		return interp(x, y, 6, v);
	}
	static double fade_s(int v)
	{
		static const double x[5] = {0, 32, 64, 96, 127}, y[5] = {0, 0.025, 0.27, 2.3, 12.1};
		return interp(x, y, 5, v);
	}
	static double hold_s(int v)
	{
		static const double x[6] = {0, 64, 83, 96, 113, 126}, y[6] = {0, 0.45, 1.25, 2.3, 5.6, 11.0};
		return interp(x, y, 6, v);
	}
	static double fadeout_s(int v)
	{
		static const double x[6] = {0, 32, 64, 94, 123, 127}, y[6] = {0, 0.05, 0.45, 2.2, 9.3, 12.0};
		return interp(x, y, 6, v);
	}
	// PAN: いちばん振れたときのパンのずれ（目盛り）
	static double pan_units(double d)
	{
		static const double x[5] = {0, 8, 16, 32, 64}, y[5] = {0, 18.4, 33, 48, 63};
		return interp(x, y, 5, d);
	}
	static double pmd_cents(double d)
	{
		static const double x[6] = {0, 16, 32, 64, 96, 127}, y[6] = {0, 9.4, 21, 102, 486, 2189};
		return interp(x, y, 6, d);
	}
	// 波形の値（-1〜1）。ph は 0〜1 の位相、cyc は何周目か（S/H の乱数に使う）
	double wave_at(double ph, long cyc) const
	{
		switch (wave) {
		case 0: return ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4;
		case 1: return ph < 0.5 ? 2 * ph : 2 - 2 * ph;
		case 2: return ph;
		case 3: return 1 - ph;
		case 4: return ph < 0.25 ? 1 : -1;
		case 5: return ph < 1 / 3.0 ? 1 : -1;
		case 6: return ph < 0.5 ? 1 : -1;
		case 7: return ph < 2 / 3.0 ? 1 : -1;
		case 8: return ph < 0.75 ? 1 : -1;
		case 9: return ph < 0.125 ? 8 * ph : ph < 0.375 ? 1 : ph < 0.625 ? 1 - 8 * (ph - 0.375) : ph < 0.875 ? -1 : -1 + 8 * (ph - 0.875);
		case 10:
		case 11: {
			unsigned h = unsigned(cyc) * 2654435761u + 12345u;
			h ^= h >> 13;
			h *= 0x5bd1e995u;
			h ^= h >> 15;
			return (h % 2001) / 1000.0 - 1.0;
		}
		case 12: {
			static const int nsteps[7] = {2, 3, 4, 6, 8, 12, 16};
			const int n = nsteps[std::max(0, std::min(6, usteps))];
			const int k = std::min(n - 1, int(ph * n));
			return (uvals[k] - 64) / 63.0;
		}
		}
		return 0;
	}
};

struct element_params {
	int coarse = 0;               // 半音
	int fine = 0;                 // -64..+63（1 = 1.5625 セント）
	int level = 127;
	double gain = 1.0;            // Filter Gain（録った雛形の 250 からの比）
	int pan = 64;                 // 1..127、64 が中央
	int scaling_pan = 64;         // スケーリングパンの深さ（64 が 0）
	common_lfo clfo;              // 音色のコモン LFO（clfo.on が偽なら使わない）
	int lbox = 0;                 // このエレメントが受ける箱（ビット 0〜2 = 箱 1〜3）
	int lratio[4] = {0, 127, 127, 127};   // 位相のずらし（0〜5）と箱 1〜3 の深さの割合
	int level_vel_sens = 64, level_vel_curve = 0, level_kf = 64;
	int level_bp[4] = { 0, 0, 0, 0 }, level_off[4] = { 128, 128, 128, 128 };   // 音量のレベルスケーリング
	swp50_aeg::amp_eg aeg;        // 時間・高さ・時間の感度（鍵とベロシティは start で入れる）
	swp50_lfo::element_lfo lfo;
	swp50_filter::filter_eg peg;  // PEG（depth が深さ、level_factor が高さのベロシティ感度）
	int pitch_kf = 96, pitch_kf_center = 60, pitch_vel_sens = 64;
	swp50_filter::filter_params filter;
	swp50_filter::filter_eg feg;
	int eq_type = swp50_eq::THRU, eq_res = 16, eq_f1 = 100, eq_g1 = 0x40, eq_f2 = 200, eq_g2 = 0x40;
};

// 鍵 note を、録った鍵 key のサンプルで鳴らすときの 1 サンプルあたりの進み
static inline double step_for(const sample &s, int note, int coarse, int fine)
{
	const long reg = std::lround((note + coarse - s.key) * 1024.0 / 12.0) + std::lround(fine * 1.5625 * 1024.0 / 1200.0);
	return std::exp2(reg / 1024.0) / s.stretch;
}

static inline double level_gain(double level)
{
	const double d = swp50_aeg::level_db(level);
	return d <= swp50_aeg::SILENT ? 0.0 : std::pow(10.0, d / 20.0);
}

struct voice {
	const sample *smp = nullptr;
	element_params p;
	int note = 60, velocity = 100;
	double pos = 0, step = 1;
	bool on = false, released = false, done = false;
	double level = 127, vel_gain = 1;     // キーフォローを掛けた音量の値と、ベロシティ感度の倍率
	double static_cents = 0;              // 音程のキーフォロー・ベロシティ感度のずれ
	swp50_filter::element_filter flt, flt_r;
	swp50_eq::element_eq eq, eq_r;
	double gl = 1, gr = 1, pan_offset = 0;
	long tick = 0;                        // 鳴り始めからのサンプル数（コモン LFO）

	// コモン LFO の今の値。音程（セント）と音量の値の下げ幅（エレメントの音量の目盛り。AMD は深さ × 負の側の振れだけ音量を下げる）、
	// カットオフとレゾナンスとパンのずれ（目盛り）、エレメント LFO の速さの倍率
	struct clfo_out {
		double cents = 0, drop = 0, cutoff = 0, reso = 0, pan = 0, speed_mul = 1;
		bool filter = false, panning = false;
	};
	void common_lfo_now(clfo_out &o) const
	{
		const common_lfo &c = p.clfo;
		if (!c.on || !p.lbox)
			return;
		const double t = tick / 44100.0, dl = common_lfo::delay_s(c.delay);
		if (t < dl)
			return;
		const double fd = common_lfo::fade_s(c.fadein);
		double f = fd > 0 ? std::min(1.0, (t - dl) / fd) : 1.0;
		if (c.hold < 127) {
			const double th = t - dl - common_lfo::hold_s(c.hold), fo = common_lfo::fadeout_s(c.fadeout);
			if (th >= fo)
				return;
			if (th > 0)
				f *= std::pow(10.0, -1.5 * th / fo);
		}
		static const double offs[6] = {0, 90, 120, 180, 240, 270};
		const double ph0 = (offs[std::max(0, std::min(5, c.phase))] + offs[std::max(0, std::min(5, p.lratio[0]))]) / 360.0;
		const double x = t * c.hz() + ph0;
		const double ph = x - std::floor(x);
		const double w = c.wave_at(ph, long(std::floor(x)));
		for (int b = 0; b < 3; b++) {
			if (!(p.lbox >> b & 1) || c.depth[b] == 0)
				continue;
			const double d = c.depth[b] * p.lratio[b + 1] / 127.0;
			switch (c.dest[b]) {
			case 0: o.drop += d * std::max(0.0, -w) * f; break;
			case 1: o.cents += common_lfo::pmd_cents(d) * w * f; break;
			case 2: o.cutoff += 2.0 * d * w * f; o.filter = true; break;
			case 3: o.reso += 0.5 * d * w * f; o.filter = true; break;
			case 4: o.pan += common_lfo::pan_units(d) * w * f; o.panning = true; break;
			case 5: o.speed_mul *= std::exp2(d * w * f / 16.0); break;
			}
		}
	}

	void start(const sample &s, const element_params &ep, int n, int v)
	{
		smp = &s; p = ep; note = n; velocity = v;
		pos = 0; step = step_for(s, n, p.coarse, p.fine);
		tick = 0;
		on = true; released = false; done = false;
		p.aeg.key = n;
		p.aeg.velocity = v;
		p.aeg.key_on();
		p.lfo.key_on();
		p.peg.key = n;
		p.peg.velocity = v;
		p.peg.key_on();
		static_cents = (swp50_peg::key_follow_ratio(p.pitch_kf) - 1.0) * 100.0 * (n - p.pitch_kf_center)
			+ swp50_peg::velocity_cents(p.pitch_vel_sens, v);
		level = std::fmin(127.0, std::fmax(0.0, p.level + swp50_aeg::level_scaling(p.level_bp, p.level_off, n)));
		level *= swp50_aeg::key_follow_factor(p.level_kf, n);
		const double vd = swp50_aeg::velocity_db(p.level_vel_sens, p.level_vel_curve, v);
		vel_gain = vd <= swp50_aeg::SILENT ? 0.0 : std::pow(10.0, vd / 20.0);
		if (vel_gain == 0.0)
			done = true;
		p.feg.key = n;
		p.feg.velocity = v;
		p.feg.key_on();
		flt.set(p.filter, n, v);
		flt.reset();
		flt_r = flt;
		eq.set(p.eq_type, p.eq_res, p.eq_f1, p.eq_g1, p.eq_f2, p.eq_g2);
		eq.reset();
		eq_r = eq;
		// パン: 実機は近い側が 0dB のまま、遠い側だけ下がる（中央から 16・32・48・63 で −1.2・−6.3・−17.5dB・無音。tools/motif/mr_pan.py）。
		// スケーリングパンは鍵ごとのずれ（深さ 127 で鍵 36・48・60・72・84・96 が −48・−21・0・+12・+38・+63）を、(深さ − 64) / 63 倍して足す
		double o = (p.pan < 1 ? 1 : p.pan > 127 ? 127 : p.pan) - 64;
		{
			static const double kx[6] = {36, 48, 60, 72, 84, 96}, ky[6] = {-48, -21, 0, 12, 38, 63};
			double sk;
			if (n <= kx[0])
				sk = ky[0] + (n - kx[0]) * (ky[1] - ky[0]) / 12.0;
			else if (n >= kx[5])
				sk = 63;
			else {
				int i = 0;
				while (n > kx[i + 1])
					i++;
				sk = ky[i] + (n - kx[i]) / 12.0 * (ky[i + 1] - ky[i]);
			}
			o += std::max(-63.0, std::min(63.0, sk)) * (p.scaling_pan - 64) / 63.0;
		}
		pan_offset = o;
		pan_gains(o);
	}

	// パンのずれ（中央から、目盛り）から左右の倍率を決める
	void pan_gains(double o)
	{
		o = std::max(-63.0, std::min(63.0, o));
		static const double px[5] = {0, 16, 32, 48, 63}, py[5] = {1.0, 0.871, 0.484, 0.133, 0.0};
		const double ao = std::fabs(o);
		int j = 0;
		while (j < 3 && ao > px[j + 1])
			j++;
		const double far = py[j] + (ao - px[j]) / (px[j + 1] - px[j]) * (py[j + 1] - py[j]);
		gl = o > 0 ? far : 1.0;
		gr = o < 0 ? far : 1.0;
	}

	void release()
	{
		if (!on || released)
			return;
		released = true;
		p.feg.key_off();
		p.aeg.key_off();
		p.peg.key_off();
	}

	float at(const std::vector<float> &x, long i) const
	{
		const sample &s = *smp;
		if (s.loop_start >= 0) {
			const long end = s.loop_start + s.loop_length;
			while (i >= end)
				i -= s.loop_length;
		}
		if (i < 0 || i >= long(x.size()))
			return 0.0f;
		return x[size_t(i)];
	}

	// 1 サンプル進めて、左右を足し込む
	void render(float &outl, float &outr)
	{
		if (!on || done)
			return;
		const sample &s = *smp;
		const long i = long(std::floor(pos));
		const double t = pos - i;
		auto herm = [&](const std::vector<float> &x) {
			const double y0 = at(x, i - 1), y1 = at(x, i), y2 = at(x, i + 1), y3 = at(x, i + 2);
			const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
			return ((c3 * t + c2) * t + c1) * t + y1;
		};
		double l = herm(s.L), r = herm(s.R);
		p.lfo.step(44100.0);
		p.peg.step();
		double cents = static_cents;
		if (p.peg.depth != 64)
			cents += swp50_peg::cents(p.peg.value * p.peg.level_factor, p.peg.depth);
		if (p.lfo.pmod)
			cents += p.lfo.pitch_cents_now();
		clfo_out co;
		common_lfo_now(co);
		cents += co.cents;
		p.lfo.speed_mul = co.speed_mul;
		tick++;
		pos += cents != 0.0 ? step * std::exp2(cents / 1200.0) : step;
		if (s.loop_start < 0 && pos >= double(s.L.size()))
			done = true;
		// FEG を目盛りに足してフィルターの g を決め直すのは重いので、FEG の深さが 0 でなければ毎サンプル（芯の試しなので素直に書く）
		const double fs = p.feg.step();
		if (p.feg.depth != 64 || p.lfo.fmod || co.filter) {
			swp50_filter::filter_params fp = p.filter;
			fp.cutoff = int(std::lround(swp50_filter::cutoff_with_feg(p.filter.cutoff, fs + p.lfo.filter_octaves_now() * 25.6 + co.cutoff, p.filter.type)));
			fp.reso = std::max(0, std::min(127, int(std::lround(p.filter.reso + co.reso))));
			swp50_filter::element_filter tmp;
			tmp.set(fp, note, velocity);
			flt.g = flt_r.g = tmp.g;
			flt.g2 = flt_r.g2 = tmp.g2;
			flt.q = flt_r.q = tmp.q;
			flt.ga = flt_r.ga = tmp.ga;
			flt.gtop = flt_r.gtop = tmp.gtop;
			flt.wop = flt_r.wop = tmp.wop;
			flt.gop = flt_r.gop = tmp.gop;
		}
		// フィルターゲインはフィルターのあとで掛け、そこで ±2.0（出口の最大の 2 倍）で頭打ちにする
		// （2026-10-24、tools/motif/mr_filter_sat.py: 発振した LPF24D の大きさがフィルターゲインに比例し、
		// フィルターのあとのエレメントの音量を 64・32 に下げても四角い頭打ちの形が残る。頭打ちは出口の基準で振幅 2 倍の所）
		l = eq.process(std::fmax(-FILTER_CLIP, std::fmin(FILTER_CLIP, flt.process(float(l)) * float(p.gain))));
		r = eq_r.process(std::fmax(-FILTER_CLIP, std::fmin(FILTER_CLIP, flt_r.process(float(r)) * float(p.gain))));
		const double eg = p.aeg.step();
		if (p.aeg.finished()) {
			done = true;
			return;
		}
		const double g = eg * vel_gain * level_gain(level - co.drop) * (p.lfo.amod ? std::pow(10.0, p.lfo.amp_db_now() / 20.0) : 1.0);
		if (co.panning)
			pan_gains(pan_offset + co.pan);
		// 離したあと出力がほぼ聞こえない（倍率 −80dB 未満）間は、実機はリリースを 4 倍の速さで進める
		// （tools/motif/mr_clfo_gate_release.py: コモン LFO の AMD で消している間に離すと、開いたあとが小さい・鳴らない。Eclipse 03_087）
		if (released && g * p.gain < QUIET_GAIN)
			for (int k = 0; k < 3 && !p.aeg.finished(); k++)
				p.aeg.step();
		outl += float(l * gl * g);
		outr += float(r * gr * g);
	}
	static constexpr double QUIET_GAIN = 1e-4;
	static constexpr float FILTER_CLIP = 2.0f;
};

} // namespace motif_sampler
