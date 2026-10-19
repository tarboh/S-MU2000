// license:BSD-3-Clause
// MOTIF-RACK ES のエレメントのフィルター（レゾナンス 0）の模型。実機の録音に当てはめた（2026-10-19、doc/motif-rack-es-capture.md の
// 「フィルターの形」）。カットオフの目盛り（0-255）ごとの係数 g を全部の型で共通にし、型ごとに次の部品の組み合わせを変える。
//
//   1 次の低域通過 LP1:  y += g (x - y)
//   状態変数フィルター SVF（Chamberlin の形。減衰 q）:
//       hp = x - lp' - q bp' ;  bp = g hp + bp' ;  lp = g bp + lp'   （' は 1 サンプル前）。BR = lp + hp
//
// g は LPF24D の録音に「SVF の低域通過（q=2）2 段」と同じ形の 1 次 4 段を当てはめた値（絶対の高さで当てるので、通る所の基準は使わない）。
// 目盛り 40 より下も同じ当てはめ（25Hz より下は測れないので誤差 0.4-0.9dB）。230 から上は 0.999（実機の周波数は 230 から上で 7,380Hz に頭打ち）。
// q = 2 の SVF は g が 0.83 を超えると不安定になるので、process では g をそこで止める（実機は安定。いちばん上はつくりが少し違うはず）。
//
// 当てはまり（dB の RMS、目盛り 60-180 / 180-210 / 210-226）: 多くの型で 0.1-0.3 / 0.3-0.6 / 1.2-2.9。
// 帯域阻止（BEF12・BEF6・DualBEF）は 0.4-1.7 / 3.0 / 4.2、LPF18s は 0.8 / 0.2 / 0.1。レゾナンスは下の「レゾナンス」の節。
#pragma once

#include <cmath>
#include <complex>

#include "swp50_eg.h"

namespace swp50_filter {

static constexpr double PI = 3.14159265358979323846;

static constexpr float g_table[256] = {
	0.002057f, 0.002164f, 0.002266f, 0.002362f, 0.002451f, 0.002543f, 0.002638f, 0.002719f,  // 0-7
	0.002801f, 0.002880f, 0.002954f, 0.003029f, 0.003107f, 0.003187f, 0.003254f, 0.003322f,  // 8-15
	0.003384f, 0.003447f, 0.003519f, 0.003576f, 0.003643f, 0.003702f, 0.003762f, 0.003814f,  // 16-23
	0.003876f, 0.003957f, 0.004163f, 0.004380f, 0.004576f, 0.004780f, 0.004960f, 0.005146f,  // 24-31
	0.005315f, 0.005476f, 0.005643f, 0.005801f, 0.005950f, 0.006117f, 0.006245f, 0.006390f,  // 32-39
	0.006546f, 0.006681f, 0.006807f, 0.006941f, 0.007053f, 0.007189f, 0.007303f, 0.007423f,  // 40-47
	0.007550f, 0.007647f, 0.007769f, 0.008076f, 0.008503f, 0.008890f, 0.009281f, 0.009679f,  // 48-55
	0.010030f, 0.010385f, 0.010755f, 0.011050f, 0.011398f, 0.011734f, 0.012024f, 0.012296f,  // 56-63
	0.012614f, 0.012879f, 0.013169f, 0.013442f, 0.013703f, 0.013954f, 0.014173f, 0.014472f,  // 64-71
	0.014671f, 0.014906f, 0.015156f, 0.015376f, 0.015638f, 0.016484f, 0.017296f, 0.018073f,  // 72-79
	0.018855f, 0.019571f, 0.020321f, 0.021022f, 0.021691f, 0.022394f, 0.022994f, 0.023621f,  // 80-87
	0.024263f, 0.024770f, 0.025393f, 0.025946f, 0.026501f, 0.027042f, 0.027515f, 0.028089f,  // 88-95
	0.028547f, 0.029023f, 0.029505f, 0.029921f, 0.030403f, 0.030803f, 0.031830f, 0.033523f,  // 96-103
	0.035150f, 0.036716f, 0.038194f, 0.039647f, 0.041084f, 0.042367f, 0.043748f, 0.045023f,  // 104-111
	0.046288f, 0.047502f, 0.048688f, 0.049889f, 0.050901f, 0.052012f, 0.053070f, 0.054222f,  // 112-119
	0.055276f, 0.056226f, 0.057205f, 0.058179f, 0.059066f, 0.059892f, 0.060812f, 0.061736f,  // 120-127
	0.064714f, 0.068102f, 0.071228f, 0.074266f, 0.077072f, 0.079851f, 0.082714f, 0.085306f,  // 128-135
	0.087896f, 0.090358f, 0.092737f, 0.095116f, 0.097552f, 0.099911f, 0.102019f, 0.104034f,  // 136-143
	0.106258f, 0.108115f, 0.110209f, 0.112156f, 0.114049f, 0.115799f, 0.117702f, 0.119534f,  // 144-151
	0.121220f, 0.123972f, 0.130699f, 0.136979f, 0.143226f, 0.149198f, 0.155073f, 0.160570f,  // 152-159
	0.165864f, 0.170714f, 0.176139f, 0.180736f, 0.185583f, 0.190894f, 0.194770f, 0.199238f,  // 160-167
	0.203683f, 0.207814f, 0.212041f, 0.216240f, 0.219976f, 0.223637f, 0.227259f, 0.231071f,  // 168-175
	0.235007f, 0.238367f, 0.242279f, 0.251807f, 0.265614f, 0.278248f, 0.290409f, 0.302778f,  // 176-183
	0.312635f, 0.324065f, 0.334542f, 0.343477f, 0.353759f, 0.363742f, 0.372545f, 0.380564f,  // 184-191
	0.390009f, 0.397762f, 0.405537f, 0.413745f, 0.421152f, 0.429050f, 0.436333f, 0.443543f,  // 192-199
	0.449882f, 0.457305f, 0.463444f, 0.469448f, 0.477621f, 0.504757f, 0.528938f, 0.553772f,  // 200-207
	0.578931f, 0.603431f, 0.626065f, 0.649414f, 0.672273f, 0.694190f, 0.713609f, 0.735718f,  // 208-215
	0.756494f, 0.775393f, 0.794918f, 0.815056f, 0.833783f, 0.851520f, 0.868629f, 0.888348f,  // 216-223
	0.906076f, 0.924184f, 0.940665f, 0.956886f, 0.976062f, 0.991159f, 0.999000f, 0.999000f,  // 224-231
	0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f,  // 232-239
	0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f,  // 240-247
	0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f, 0.999000f,  // 248-255
};


// ---- レゾナンス（2026-10-19。tools/motif/mr_reso.py の録音、目盛り 96・128・160 に当てはめ） ----
// 多くの型は SVF の q が「16 ごとに半分」: q = 2 × 2^(-reso/16)。
// 外れる型は測った q の表（reso 0, 8, … 120, 127）を対数で補間する。
// 録音のときは雛形のレゾナンスのベロシティ感度（+35）がベロシティ 100 で効いていて、実際のレゾナンスは書いた値より 7.4 低かった。
// 式と表はその分を直したもの（2026-10-19、mr_fscale.py の録音で確かめた。レゾナンス 32・感度 0 で q = 0.511）。
// 線形の模型なので、実機が発振して飽和する所（本当のレゾナンスで、4 段の型は 33〜41、2 段の型は 73、LPF18 は 81 から上）は表せない。
static constexpr float reso_points[17] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127 };
static constexpr float q_lpf24a[17] = { 1.374f, 1.192f, 0.987f, 0.846f, 0.713f, 0.622f, 0.545f, 0.494f, 0.441f, 0.407f, 0.377f,
                                        0.356f, 0.342f, 0.331f, 0.319f, 0.307f, 0.298f };   // 112 から上は飽和するので、その手前の下がり方で延ばした
static constexpr float q_bef12[17]  = { 1.975f, 1.488f, 1.022f, 0.776f, 0.553f, 0.422f, 0.334f, 0.266f, 0.204f, 0.165f, 0.133f,
                                        0.114f, 0.096f, 0.086f, 0.079f, 0.074f, 0.070f };
static constexpr float q_bef6[17]   = { 2.094f, 1.603f, 1.123f, 0.853f, 0.609f, 0.472f, 0.343f, 0.275f, 0.212f, 0.175f, 0.138f,
                                        0.118f, 0.100f, 0.091f, 0.083f, 0.078f, 0.074f };   // DualBEF も同じ
static constexpr float q_bpfw[17]   = { 1.010f, 1.010f, 1.069f, 1.216f, 1.423f, 1.709f, 2.100f, 2.558f, 3.077f, 3.784f, 4.660f,
                                        5.806f, 6.920f, 8.031f, 10.421f, 12.749f, 15.133f };    // BPFw は逆に広がる（山の高さは保つ）

static inline float q_from_table(const float *tab, int reso)
{
	int i = 0;
	while (i < 15 && reso > reso_points[i + 1])
		i++;
	const float t = (reso - reso_points[i]) / (reso_points[i + 1] - reso_points[i]);
	return std::exp(std::log(tab[i]) + t * (std::log(tab[i + 1]) - std::log(tab[i])));
}

// ---- キーフォロー（2026-10-19。tools/motif/mr_kf.py の録音、BEF12 の谷で測った） ----
// 中心の鍵は 60（C3）。値 0-127 が −200%〜+200%（64 が 0%）。100% のとき 1 半音で 2.13 目盛り（1 オクターブで 25.6 = くり返し 1 つ）。
// キーフォローはセントではなく目盛りの側に足される（目盛り 128 で 50% のとき、鍵 +12 で +680 セント、−12 で −528 セント）。
// ±6 半音ぶん動かす所では測った値と 1.8 目盛りほどずれる。値 127・鍵 84 だけ実機は 59Hz に回り込んだ（理由は不明。模型は頭打ち）
static constexpr double KEYFOLLOW_STEPS_PER_SEMITONE = 2.13;

static inline double cutoff_with_keyfollow(int cutoff, int keyfollow, int key)
{
	const double c = cutoff + (keyfollow - 64) / 32.0 * (key - 60) * KEYFOLLOW_STEPS_PER_SEMITONE;
	return c < 0 ? 0 : c > 255 ? 255 : c;
}

// ---- カットオフのベロシティ感度（0x03、64 が 0）（2026-10-19。tools/motif/mr_fparams.py の録音、BEF12 の谷で測った） ----
// 目盛りの動き = 0.0285 × |感度 − 64| ×（v − 115）。v は感度が正ならベロシティ、負なら 128 − ベロシティ（裏返し）。
// 動かないのはベロシティ 115（感度が負なら 13）。測った 30 点との差は RMS 1 目盛り、最大 1.7 目盛り
static constexpr double VELSENS_STEPS = 0.0285;
static constexpr double VELSENS_CENTER = 115.0;

static inline double velocity_steps(int sens, int velocity)
{
	const int s = sens - 64;
	const double v = s >= 0 ? velocity : 128 - velocity;
	return VELSENS_STEPS * (s < 0 ? -s : s) * (v - VELSENS_CENTER);
}

// ---- カットオフのスケーリング（0x23-0x2D）（2026-10-19。tools/motif/mr_fscale.py の録音、BEF12 の谷で測った） ----
// ブレークポイント 4 つ（鍵）とずらし 4 つ（0-255、128 が 0）。ブレークポイントの間はずらしをまっすぐにつなぎ、外側はいちばん端の値のまま。
// ずらし 1 = 0.8 目盛り（32 で 1 オクターブ）。オクターブの整数倍でない量は実機と ±1.5 目盛り（±80 セント）ずれる（キーフォローと同じ性質）
static constexpr double SCALING_STEPS = 0.8;

static inline double scaling_steps(int key, const int bp[4], const int offset[4])
{
	double o;
	if (key <= bp[0])
		o = offset[0] - 128;
	else if (key >= bp[3])
		o = offset[3] - 128;
	else {
		int i = 0;
		while (i < 2 && key > bp[i + 1])
			i++;
		const double t = bp[i + 1] > bp[i] ? double(key - bp[i]) / (bp[i + 1] - bp[i]) : 0.0;
		o = (offset[i] - 128) + t * (offset[i + 1] - offset[i]);
	}
	return SCALING_STEPS * o;
}

// ---- Filter Gain（0x0C、0-255）（同じ録音） ----
// 大きさは dB でまっすぐ、1 あたり 0.375dB（チップの減衰の単位と同じ。0x10 = 6dB）。255 を 0dB とした値を返す。32 以下は無音（-1e9）
static inline double filter_gain_db(int gain)
{
	return gain <= 32 ? -1e9 : 0.375 * (gain - 255);
}

// 小数の目盛りの g（隣の目盛りの間を対数で補間）。0 より下は 25.6 目盛りで 1 オクターブとして外へ延ばす（Dual 系の 2 つめが使う）
static inline float g_at(double c)
{
	if (c < 0)
		return float(g_table[0] * std::exp2(c / 25.6));
	if (c > 255)
		c = 255;
	const int i = c >= 255 ? 254 : int(c);
	const double t = c - i;
	return float(std::exp(std::log(double(g_table[i])) + t * (std::log(double(g_table[i + 1])) - std::log(double(g_table[i])))));
}

// 型の番号（Data List の並び。9・18・20 は欠番で、書くと次の値になる）
enum : int {
	LPF24D = 0, LPF24A = 1, LPF18 = 2, LPF18S = 3, LPF12 = 4, LPF6 = 5, HPF24D = 6, HPF12 = 7, BPF12D = 8,
	BPFW = 10, BPF6 = 11, BEF12 = 12, BEF6 = 13, DUAL_LPF = 14, DUAL_HPF = 15, DUAL_BPF = 16, DUAL_BEF = 17,
	LPF12_BPF6 = 19, THRU = 21
};

static inline int canonical_type(int t) { return t == 9 ? 10 : t == 18 ? 19 : t == 20 ? 21 : t; }

// レゾナンスのベロシティ感度（0x06、64 が 0）。感度が正ならベロシティ 127、負なら 1 のとき書いた値のままで、
// そこから離れるほど |感度 − 64| × 離れた量 / 128 だけレゾナンスが下がる（書いた値より上がることはない）
static inline int resonance_with_velocity(int reso, int sens, int velocity)
{
	const int s = sens - 64;
	if (s == 0)
		return reso;
	const int away = s > 0 ? 127 - velocity : velocity - 1;
	const double r = reso - (s < 0 ? -s : s) * away / 128.0;
	return r < 0 ? 0 : int(r + 0.5);
}

// 型とレゾナンス（0-127）から SVF の q
static inline float type_q(int t, int reso)
{
	reso = reso < 0 ? 0 : reso > 127 ? 127 : reso;
	switch (canonical_type(t)) {
	case LPF24A: return q_from_table(q_lpf24a, reso);
	case BPFW:   return q_from_table(q_bpfw, reso);
	case BEF12:  return q_from_table(q_bef12, reso);
	case BEF6: case DUAL_BEF: return q_from_table(q_bef6, reso);
	default: return 2.0f * std::exp2(-reso / 16.0f);
	}
}

// Chamberlin の SVF が安定な q の上限（g q < 2 と g^2 + 2 g q < 4。少し余裕を持たせる）
static inline float stable_q(float g, float q)
{
	const float a = 1.9f / g, b = 0.98f * (4.0f - g * g) / (2.0f * g);
	const float m = a < b ? a : b;
	return q < m ? q : m;
}

// ---- Dual 系の Distance（2026-10-19。tools/motif/mr_fparams.py の録音） ----
// Dual 系の型は 2 つのフィルターを持つ。2 つめの目盛り = 1 つめ + 1.6 ×（Distance − 128）。Distance 16 で 1 オクターブ（25.6 目盛り）、128 で重なる。
// DualLPF・DualHPF・DualBPF は 2 つの和（重なると +6dB）、DualBEF は直列（谷が 2 つ）。どれも q = 2 の SVF。
// 誤差は Distance 48〜176 で 0.1〜0.4dB（DualBEF は 0.5〜1.9dB）。192 から上は 2 つめが上限に貼りつき、1.3〜3.3dB ずれる
static constexpr double DISTANCE_STEPS = 1.6;

// ---- HPF（2026-10-19。tools/motif/mr_kf.py の録音） ----
// エレメントの HPF は、型が LPF12 と LPF6 のときだけ効く（ほかの型では値は入るが音は変わらない。THRU でも効かない）。
// 形は SVF の高域通過で q = 1.05（目盛り 16〜208 で誤差 0.25〜0.5dB。実機は f0 の少し上に +1.4dB の山がある）。
// 目盛り → g はカットオフと同じ表。キーフォローもカットオフと同じ決まり（中心の鍵 60、100% で 1 半音 2.13 目盛り）。目盛り 0 は効かない
static constexpr float HPF_Q = 1.05f;

struct svf {
	float lp = 0, bp = 0;
	void step(float x, float g, float q, float &olp, float &obp, float &ohp)
	{
		const float hp = x - lp - q * bp;
		bp = g * hp + bp;
		lp = g * bp + lp;
		olp = lp; obp = bp; ohp = hp;
	}
};

// エレメントのフィルターのパラメーター（Data List の 43 xx の欄。既定値は「効かせない」値）
struct filter_params {
	int type = 21;            // 0x00
	int cutoff = 255;         // 0x01
	int vel_sens = 64;        // 0x03
	int reso = 0;             // 0x05
	int reso_vel_sens = 64;   // 0x06
	int hpf = 0;              // 0x07
	int distance = 0;         // 0x0A
	int keyfollow = 64;       // 0x22
	int bp[4] = { 27, 48, 72, 84 };          // 0x23-0x26
	int offset[4] = { 128, 128, 128, 128 };  // 0x27-0x2D
	int hpf_keyfollow = 64;   // 0x2F
};

struct element_filter {
	int   type = THRU;
	float g = 0.999f;
	float q = 2.0f;
	float hg = 0;                                        // HPF の g（0 = 切）
	float g2 = 0.999f;                                   // Dual 系の 2 つめの g
	svf   s1, s2, s3, sh;
	float p1 = 0, p2 = 0;

	// reso はエレメントのレゾナンス（0-127）。LPF6 と LPF18s には効かない（LPF18s は実機では効くが、まだ模型にしていない）
	// keyfollow はカットオフのキーフォロー（0-127、64 が 0%）、key は鳴らした鍵。
	// hpf は HPF のカットオフ（0-255、0 = 切）、hpf_keyfollow はそのキーフォロー。HPF が効くのは型が LPF12 と LPF6 のときだけ
	// distance は Dual 系の 2 つのフィルターの間隔（0-255、128 で重なる。雛形の音色では 0）
	// vel_sens はカットオフのベロシティ感度（0-127、64 が 0）、velocity は鳴らしたベロシティ
	void set(int t, int cutoff, int reso = 0, int keyfollow = 64, int key = 60, int hpf = 0, int hpf_keyfollow = 64, int distance = 0,
	         int vel_sens = 64, int velocity = 100, int reso_vel_sens = 64)
	{
		reso = resonance_with_velocity(reso, reso_vel_sens, velocity);
		double c = keyfollow == 64 ? double(cutoff < 0 ? 0 : cutoff > 255 ? 255 : cutoff) : cutoff_with_keyfollow(cutoff, keyfollow, key);
		if (vel_sens != 64) {
			c += velocity_steps(vel_sens, velocity);
			c = c < 0 ? 0 : c > 255 ? 255 : c;
		}
		{
			const float x = g_at(c + DISTANCE_STEPS * (distance - 128));
			g2 = x < 0.82f ? x : 0.82f;
		}
		hg = 0;
		if (hpf > 0 && (canonical_type(t) == LPF12 || canonical_type(t) == LPF6)) {
			const float x = g_at(cutoff_with_keyfollow(hpf, hpf_keyfollow, key));
			hg = x < 0.82f ? x : 0.82f;
		}
		type = canonical_type(t);
		g = (keyfollow == 64 && vel_sens == 64) ? g_table[cutoff < 0 ? 0 : cutoff > 255 ? 255 : cutoff] : g_at(c);
		const float gs = g < 0.82f ? g : 0.82f;
		q = stable_q(gs, type_q(type, reso));
	}
	void reset() { s1 = s2 = s3 = sh = svf(); p1 = p2 = 0; }

	// 全部のパラメーターから（スケーリングも入れる）
	void set(const filter_params &p, int key, int velocity)
	{
		const int sc = int(std::lround(scaling_steps(key, p.bp, p.offset)));
		int c = p.cutoff + sc;
		c = c < 0 ? 0 : c > 255 ? 255 : c;
		set(p.type, c, p.reso, p.keyfollow, key, p.hpf, p.hpf_keyfollow, p.distance, p.vel_sens, velocity, p.reso_vel_sens);
	}

	float process(float x)
	{
		if (hg > 0) {                                     // HPF（SVF の高域通過、q = 1.05）
			float hl, hb, hh;
			sh.step(x, hg, HPF_Q, hl, hb, hh);
			x = hh;
		}
		const float gs = g < 0.82f ? g : 0.82f;           // q = 2 の SVF が不安定にならない所まで
		float l, b, h, l2, b2, h2;
		switch (type) {
		case LPF24D: case LPF24A: s1.step(x, gs, q, l, b, h); s2.step(l, gs, q, l2, b2, h2); return l2;
		case LPF18:  s1.step(x, gs, q, l, b, h); p1 += g * (l - p1); return p1;
		case LPF18S: { p1 += g * (x - p1); const float g2 = g * 5.657f < 0.999f ? g * 5.657f : 0.999f; p2 += g2 * (p1 - p2); return p2; }
		case LPF12: case LPF12_BPF6: s1.step(x, gs, q, l, b, h); return l;
		case DUAL_LPF: s1.step(x, gs, q, l, b, h); s2.step(x, g2, q, l2, b2, h2); return l + l2;
		case LPF6:   p1 += g * (x - p1); return p1;
		case HPF24D: s1.step(x, gs, q, l, b, h); s2.step(h, gs, q, l2, b2, h2); return h2;
		case HPF12:  s1.step(x, gs, q, l, b, h); return h;
		case BPF12D: s1.step(x, gs, q, l, b, h); s2.step(b, gs, q, l2, b2, h2); return b2;
		case BPFW:   s1.step(x, gs, q, l, b, h); s2.step(q * b, gs, q, l2, b2, h2); return q * b2;   // 山の高さを保つ
		case BPF6:   s1.step(x, gs, q, l, b, h); return b;
		case BEF12:  s1.step(x, gs, q, l, b, h); s2.step(l + h, gs, q, l2, b2, h2); return l2 + h2;
		case BEF6:   s1.step(x, gs, q, l, b, h); return l + h;
		case DUAL_BEF: s1.step(x, gs, q, l, b, h); s2.step(l + h, g2, q, l2, b2, h2); return l2 + h2;
		case DUAL_HPF: s1.step(x, gs, q, l, b, h); s2.step(x, g2, q, l2, b2, h2); return h + h2;
		case DUAL_BPF: s1.step(x, gs, q, l, b, h); s2.step(x, g2, q, l2, b2, h2); return b + b2;
		default:     return x;
		}
	}
};

// 同じ模型の周波数特性（f Hz、サンプリング 44,100Hz）。試験と、録音との突き合わせに使う
static inline double response_db(int t, int cutoff, double f, int reso = 0, int hpf = 0, int distance = 0)
{
	using cd = std::complex<double>;
	t = canonical_type(t);
	const double g0 = g_table[cutoff < 0 ? 0 : cutoff > 255 ? 255 : cutoff];
	const double gs = g0 < 0.82 ? g0 : 0.82;
	const cd z1 = std::polar(1.0, -2 * PI * f / 44100.0);
	auto lp1 = [&](double g) { return g / (1.0 - (1.0 - g) * z1); };
	auto sv = [&](double g, double q, int i) {
		const cd a = 1.0 - z1;
		const cd hp = 1.0 / (1.0 + z1 * g * g / (a * a) + q * z1 * g / a);
		const cd bp = g * hp / a, lp = g * bp / a;
		return i == 0 ? lp : i == 1 ? bp : i == 2 ? hp : lp + hp;
	};
	const double q = stable_q(float(gs), type_q(t, reso));
	const double g2a = g_at(cutoff + DISTANCE_STEPS * (distance - 128)), g2 = g2a < 0.82 ? g2a : 0.82;
	cd h = 1.0;
	switch (t) {
	case LPF24D: case LPF24A: h = sv(gs, q, 0) * sv(gs, q, 0); break;
	case LPF18:  h = sv(gs, q, 0) * lp1(g0); break;
	case LPF18S: h = lp1(g0) * lp1(g0 * 5.657 < 0.999 ? g0 * 5.657 : 0.999); break;
	case LPF12: case LPF12_BPF6: h = sv(gs, q, 0); break;
	case DUAL_LPF: h = sv(gs, q, 0) + sv(g2, q, 0); break;
	case LPF6:   h = lp1(g0); break;
	case HPF24D: h = sv(gs, q, 2) * sv(gs, q, 2); break;
	case HPF12:  h = sv(gs, q, 2); break;
	case BPF12D: h = sv(gs, q, 1) * sv(gs, q, 1); break;
	case BPFW:   h = q * sv(gs, q, 1) * q * sv(gs, q, 1); break;
	case BPF6:   h = sv(gs, q, 1); break;
	case BEF12:  h = sv(gs, q, 3) * sv(gs, q, 3); break;
	case BEF6:   h = sv(gs, q, 3); break;
	case DUAL_BEF: h = sv(gs, q, 3) * sv(g2, q, 3); break;
	case DUAL_HPF: h = sv(gs, q, 2) + sv(g2, q, 2); break;
	case DUAL_BPF: h = sv(gs, q, 1) + sv(g2, q, 1); break;
	default: break;
	}
	if (hpf > 0 && (t == LPF12 || t == LPF6)) {
		const double hg0 = g_table[hpf > 255 ? 255 : hpf];
		h *= sv(hg0 < 0.82 ? hg0 : 0.82, HPF_Q, 2);
	}
	return 20 * std::log10(std::abs(h) + 1e-30);
}

// ---- フィルター EG（FEG）（2026-10-20。tools/motif/mr_feg.py の録音、BEF12・目盛り 128 の谷を短い区切りで追った） ----
// * 動き（オクターブ）= 高さ ×（深さ − 64）/ 1024。深さ +64 なら高さ 1 = 75 セント（±128 で ±9600 セント。Data List と合う）。
//   高さ +32・深さ +32 で +1 オクターブ（測定 +1,199 セント）、高さ +64・深さ +32 で +2 オクターブ（+2,412 セント）
// * 上は 7,389Hz で頭打ち。目盛りが 0 より下に出ると 0 で折り返す（高さ +127・深さ 0 = −202 目盛りで、谷は目盛り 74 の 107Hz に来た）
// * 各段は高さの単位でまっすぐ動き（50% / 90% の時刻 = 0.55〜0.56）、かかる時間は高さの差によらない。時間の値 p の段の長さは
//   音量の EG の下がる側と同じ: swp50_eg::time_down(127 − p) サンプル（p = 64 で 0.79 秒、80 で 3.17 秒。測定 0.79〜0.80 / 3.1〜3.2 秒）
// * ホールドの時間も同じとみているが、確かめていない。ベロシティ感度・時間のキーフォローもまだ入れていない
struct filter_eg {
	// 高さ（-128..+127）: hold, attack, decay1, decay2, release。時間（0..127）: hold, attack, decay1, decay2, release
	int level[5] = { 0, 0, 0, 0, 0 };
	int time[5] = { 0, 0, 0, 0, 0 };
	int depth = 64;

	int    seg = 0;            // 0 ホールド、1 アタック、2 ディケイ 1、3 ディケイ 2（そのまま止まる）、4 リリース、5 終わり
	double value = 0;          // 今の高さ
	double from = 0, left = 0, len = 0;

	static double seg_samples(int t) { return t <= 0 ? 0.0 : swp50_eg::time_down(uint32_t(127 - (t > 127 ? 127 : t))); }

	void begin(int s, double start, double samples)
	{
		seg = s; from = start; value = start; len = samples; left = samples;
	}
	void key_on()  { begin(0, level[0], seg_samples(time[0])); }
	void key_off() { begin(4, value, seg_samples(time[4])); }

	double target() const { return seg == 0 ? level[0] : seg <= 3 ? level[seg] : level[4]; }

	// 1 サンプル進めて、カットオフの目盛りに足す量を返す
	double step()
	{
		for (int guard = 0; guard < 6 && left <= 0 && seg < 3; guard++)
			begin(seg + 1, seg == 0 ? level[0] : level[seg], seg_samples(time[seg + 1]));
		if (left > 0) {
			left -= 1;
			const double tg = target();
			value = from + (tg - from) * (1.0 - left / len);
		} else {
			value = target();
		}
		return steps();
	}
	double octaves() const { return value * (depth - 64) / 1024.0; }
	double steps() const { return octaves() * 25.6; }
};

// FEG を足した目盛り（0 より下は 0 で折り返す、上は 255 で止める）
static inline double cutoff_with_feg(double cutoff, double feg_steps)
{
	double c = cutoff + feg_steps;
	if (c < 0)
		c = -c;
	return c > 255 ? 255 : c;
}

} // namespace swp50_filter
