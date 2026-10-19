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

struct element_params {
	int coarse = 0;               // 半音
	int fine = 0;                 // -64..+63（1 = 1.5625 セント）
	int level = 127;
	int pan = 64;                 // 1..127、64 が中央
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
	double gl = 1, gr = 1;

	void start(const sample &s, const element_params &ep, int n, int v)
	{
		smp = &s; p = ep; note = n; velocity = v;
		pos = 0; step = step_for(s, n, p.coarse, p.fine);
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
		const double a = (p.pan < 1 ? 1 : p.pan > 127 ? 127 : p.pan - 1) / 126.0 * (3.14159265358979 / 2);
		gl = std::cos(a) * std::sqrt(2.0);
		gr = std::sin(a) * std::sqrt(2.0);
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
		pos += cents != 0.0 ? step * std::exp2(cents / 1200.0) : step;
		if (s.loop_start < 0 && pos >= double(s.L.size()))
			done = true;
		// FEG を目盛りに足してフィルターの g を決め直すのは重いので、FEG の深さが 0 でなければ毎サンプル（芯の試しなので素直に書く）
		const double fs = p.feg.step();
		if (p.feg.depth != 64 || p.lfo.fmod) {
			swp50_filter::filter_params fp = p.filter;
			fp.cutoff = int(std::lround(swp50_filter::cutoff_with_feg(p.filter.cutoff, fs + p.lfo.filter_octaves_now() * 25.6)));
			swp50_filter::element_filter tmp;
			tmp.set(fp, note, velocity);
			flt.g = flt_r.g = tmp.g;
			flt.g2 = flt_r.g2 = tmp.g2;
		}
		l = eq.process(flt.process(float(l)));
		r = eq_r.process(flt_r.process(float(r)));
		const double eg = p.aeg.step();
		if (p.aeg.finished()) {
			done = true;
			return;
		}
		const double g = eg * vel_gain * level_gain(level - p.lfo.level_drop_now());
		outl += float(l * gl * g);
		outr += float(r * gr * g);
	}
};

} // namespace motif_sampler
