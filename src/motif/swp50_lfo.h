// license:BSD-3-Clause
// MOTIF-RACK ES のエレメントの LFO（2026-10-20。tools/motif/mr_lfo.py の録音を mr_lfo_fit.py で読んだ。正弦波の波形 704・鍵 60）。
//
//   アドレスは 44 ee 40〜47（形・キーオンでそろえる・ディレイ・速さ・音量の深さ・音程の深さ・フィルターの深さ・フェード）。
//   速さ    : 値 4 ごとに √2 倍。値 32 で 2.86Hz、63 で 42.1Hz（20〜63 は ±3% で合う）。
//             4・8・12 は 0.224・0.338・0.443Hz と測ったが、録音が 1〜3 周しかなく式（0.253・0.358・0.506）と 10% ほどずれる。
//             0 は止まる（揺れない）
//   形      : 0 = のこぎり（音程は一気に上がって、まっすぐ下がる）、1 = 三角、2 = 四角（半分ずつ）
//   音程の深さ: 値 16 ごとに 2 倍。±37.9 セント × 2^((値 − 64) / 16)（16・32・64・96・127 で 4.4・9.9・37.9・147.7・582.0、式は 4.7・9.5・37.9・151・579）
//   音量の深さ: 音量の値（40 × log10(L / 127) dB の L）を、LFO が下へ振れた所で「深さ」だけ下げる。上へは上げない。
//             深さ 32・64・96・127 で山から谷 4.3・12.0・23.8・93dB（式は 5.0・11.9・24.5・無音）
//   フィルターの深さ: カットオフの谷が上下に、山から谷で 0.0378 × 深さ^0.95 オクターブ（32・64・127 で 1.02・1.93・3.78 オクターブ、式は 1.02・1.97・3.78）。
//             真ん中はもとのカットオフ（BEF12・目盛り 128 で揺れの真ん中が 470Hz 前後）
//   ディレイ: キーオンから揺れが始まるまで 0.0056 × 値^1.5 秒。値 8・16・32・48・64 で 0.125・0.354・1.018・1.871・2.894 秒
//             （式は 0.127・0.358・1.01・1.86・2.87）、96 は 5 秒のうちに始まらない（式 5.3）。ディレイの間も LFO は回っている
//   フェード: 揺れが 0 からまっすぐ大きくなる長さ（ディレイのあと）0.006 × 値^1.5 秒。半周ごとの揺れの大きさの傾きから、
//             値 8・16・32・48・64・80・96 で約 0.15・0.35・1.15・1.95・2.9・4.15・5.5 秒（式は 0.14・0.38・1.09・2.0・3.07・4.29・5.64）
//   キーオンでそろえる: on なら毎回同じ所から始まる。三角とのこぎりは真ん中から音程が下がる向き、四角は上の側から。
//             off なら前の音から続いた所から。音程と音量を同時に揺らすと、2 つの揺れの相関はほぼ 0（+0.02。位相がずれている。模型は同じ位相）
#pragma once

#include <cmath>

namespace swp50_lfo {

static inline double speed_hz(int v)
{
	return v <= 0 ? 0.0 : 2.86 * std::exp2((v - 32) / 8.0);
}

static inline double pitch_cents(int depth)
{
	return depth <= 0 ? 0.0 : 37.9 * std::exp2((depth - 64) / 16.0);
}

static inline double filter_octaves_pp(int depth)
{
	return depth <= 0 ? 0.0 : 0.0378 * std::pow(double(depth), 0.95);
}

static inline double delay_seconds(int v)
{
	return v <= 0 ? 0.0 : 0.0056 * std::pow(double(v), 1.5);
}

static inline double fade_seconds(int v)
{
	return v <= 0 ? 0.0 : 0.0060 * std::pow(double(v), 1.5);
}

struct element_lfo {
	int wave = 1, speed = 32, delay = 0, fade = 0, pmod = 0, amod = 0, fmod = 0;
	bool key_sync = true;
	double phase = 0;          // 0..1（三角・のこぎりは 0 で真ん中・下がる向き、四角は上）
	double t = 0;              // キーオンからの秒

	void key_on()
	{
		t = 0;
		if (key_sync)
			phase = 0;
	}

	// 形の値（-1..+1）
	double shape() const
	{
		switch (wave) {
		case 0: return phase < 0.5 ? -2.0 * phase : 2.0 - 2.0 * phase;        // のこぎり: 真ん中から下がり、上へ跳んでまた下がる
		case 2: return phase < 0.5 ? 1.0 : -1.0;                             // 四角
		default: {                                                           // 三角: 0 → -1 → +1 → 0
			const double p = phase < 0.25 ? -4 * phase : phase < 0.75 ? -2 + 4 * phase : 4 - 4 * phase;
			return p;
		}
		}
	}

	// 揺れの大きさ（ディレイとフェード、0..1）
	double amount() const
	{
		const double d = delay_seconds(delay);
		if (t < d)
			return 0.0;
		const double f = fade_seconds(fade);
		return f <= 0 ? 1.0 : std::fmin(1.0, (t - d) / f);
	}

	// 1 サンプル進める
	void step(double fs)
	{
		t += 1.0 / fs;
		phase += speed_hz(speed) / fs;
		phase -= std::floor(phase);
	}

	double pitch_cents_now() const { return pitch_cents(pmod) * shape() * amount(); }
	// 音量の値から下げる量（0..amod）。LFO が下へ振れた所で一番下げる
	double level_drop_now() const { return amod * (1.0 - shape()) * 0.5 * amount(); }
	double filter_octaves_now() const { return filter_octaves_pp(fmod) * 0.5 * shape() * amount(); }
};

} // namespace swp50_lfo
