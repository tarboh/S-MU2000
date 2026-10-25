// license:BSD-3-Clause
// サンプリング方式の芯（sampler.h）で、パッチ（tools/motif/mr_patch.py が作る文字ファイル）の音を 1 つ鳴らして WAV に書く。
//
//   g++ -std=c++17 -O2 -o sampler_render src/motif/sampler_render.cpp
//   sampler_render 音色.patch 鍵 ベロシティ 押さえる秒 出力.wav [--fx capture の置き場] [--no-ins] [--no-reverb] [--no-chorus] [--ins-majority]
//                  [--ctl ソース=値@秒,…] [--note-index n] [--seed s] [--events on@秒:鍵:ベロシティ,off@秒:鍵,…]
//
// --fx を付けると、パッチの fx の行のエフェクト（sampler_fx.h）を掛ける。置き場の fx/*.irx（リバーブ）と fxd/*.dsx（歪み系）を使う
// （tools/motif/mr_fx_export.py が書く）。インサーションのスイッチは、鳴ったエレメントでいちばん多いもの。
//
// --ctl は、コントロールセットのソース（0 PB・1 MW（CC1）・2/3 AT・6 RB（CC22）・7 BC（CC2）・8 AS1（CC16）・9 AS2（CC17）・10〜12 FC（CC4））を
// その時刻から値（0〜127）にする。初めは PB・RB・AS1・AS2 が 64（中心）、ほかは 0。PB はピッチベンドの幅（ctrl の行の pbup・pbdown）でも音程を動かす。
//
// --events を付けると、鍵・ベロシティ・押さえる秒の代わりにその並びを鳴らす（和音・モノ・ポルタメント・同じ鍵の重ね・発音数の奪い合い。
// 音色の voice の行の mono・same・porta・pmode・ptime・ptmode を使う。tools/motif/mr_multi.py、2026-10-25）。出力はいちばん遅い出来事から 3 秒。
//
// --note-index は、音色を選んでから何音目か（0 から。オルタネートパンの向き）。--seed を付けると、ランダムパンとランダムピッチをその種で振る
// （付けなければ 0。実機は毎回違う並びなので、1 本ずつは合わせられない）。
//
// パッチのエレメントのうち、鍵とベロシティの範囲に入るものを全部鳴らす。サンプルは、そのエレメントの区間のうち
// 鍵とベロシティを含むもの（無ければ鍵がいちばん近いもの）。出力は 44.1kHz・32bit float のステレオ。

#include "sampler.h"
#include "sampler_fx.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using kv = std::map<std::string, std::string>;

kv parse(const std::string &line)
{
	kv m;
	std::istringstream ss(line);
	std::string tok;
	ss >> tok;
	while (ss >> tok) {
		const size_t e = tok.find('=');
		if (e != std::string::npos)
			m[tok.substr(0, e)] = tok.substr(e + 1);
	}
	return m;
}

int geti(const kv &m, const char *k, int d = 0)
{
	auto it = m.find(k);
	return it == m.end() ? d : std::atoi(it->second.c_str());
}

std::vector<int> list(const kv &m, const char *k)
{
	std::vector<int> v;
	auto it = m.find(k);
	if (it == m.end())
		return v;
	std::istringstream ss(it->second);
	std::string t;
	while (std::getline(ss, t, ','))
		v.push_back(std::atoi(t.c_str()));
	return v;
}

bool load_smpx(const std::string &path, motif_sampler::sample &s)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char head[256] = {};
	if (!std::fgets(head, sizeof head, f)) {
		std::fclose(f);
		return false;
	}
	const kv m = parse(head);
	s.key = geti(m, "key", 60);
	s.loop_start = geti(m, "loop_start", -1);
	s.loop_length = geti(m, "loop_length", 0);
	s.stretch = m.count("stretch") ? std::atof(m.at("stretch").c_str()) : 1.0;
	const int n = geti(m, "frames", 0);
	std::vector<float> buf(size_t(n) * 2);
	const size_t got = std::fread(buf.data(), sizeof(float), buf.size(), f);
	std::fclose(f);
	if (got != buf.size())
		return false;
	s.L.resize(n);
	s.R.resize(n);
	for (int i = 0; i < n; i++) {
		s.L[i] = buf[2 * i];
		s.R[i] = buf[2 * i + 1];
	}
	return true;
}

struct zone { int key_lo, key_hi, vel_lo, vel_hi; std::string file; };
struct element { kv f; std::vector<zone> zones; };

void write_wav(const char *path, const std::vector<float> &l, const std::vector<float> &r)
{
	FILE *f = std::fopen(path, "wb");
	if (!f)
		return;
	const uint32_t n = uint32_t(l.size()), data = n * 8;
	auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16); u16(3); u16(2); u32(44100); u32(44100 * 8); u16(8); u16(32);
	std::fwrite("data", 1, 4, f); u32(data);
	for (uint32_t i = 0; i < n; i++) {
		std::fwrite(&l[i], 4, 1, f);
		std::fwrite(&r[i], 4, 1, f);
	}
	std::fclose(f);
}

} // namespace

// エレメントの Key On Delay（0〜127）→ ms。正弦波の雛形で、値ごとに 3 回ずつ録った音の頭の遅れ（tools/motif/mr_keydelay.py）。間はまっすぐに補間
static double key_on_delay_ms(int v)
{
	static const double x[] = {0, 1, 2, 3, 4, 5, 7, 10, 15, 20, 30, 40, 60, 80, 100, 127};
	static const double y[] = {0, 5.5, 19, 25.4, 43.4, 60.2, 93.7, 176.4, 322.2, 509.2, 925.0, 1428.5, 2627.5, 4049.2, 5654.6, 8097.7};
	if (v <= 0)
		return 0;
	if (v >= 127)
		return y[15];
	for (int i = 0; i < 15; i++)
		if (v <= x[i + 1])
			return y[i] + (v - x[i]) / (x[i + 1] - x[i]) * (y[i + 1] - y[i]);
	return y[15];
}

// Key On Delay のテンポ同期（41 ee 12 = 1）のとき、長さ（41 ee 1C）→ 拍。正弦波の雛形でテンポ 120 で測った（tools/motif/mr_keydelay_sync.py、2026-10-24）。
// 6 から上は音符の長さ（3 連 8 分・付点 16 分・8 分・3 連 4 分・付点 8 分・4 分・3 連 2 分・付点 4 分・2 分・3 連全音・付点 2 分・4 分 × 4・× 5・× 6）で、
// テンポ 60 でも同じ拍数。0〜5 はどれも 0.23 拍。20 から上は 1 拍ずつ延ばした
static double key_on_delay_beats(int v)
{
	static const double b[20] = {0.235, 0.242, 0.234, 0.230, 0.236, 0.229, 0.327, 0.351, 0.504, 0.651, 0.723, 0.992, 1.319, 1.500, 1.987, 2.660, 2.992, 3.974, 5.001, 5.994};
	if (v < 0)
		return b[0];
	return v < 20 ? b[v] : b[19] + (v - 19);
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		std::fprintf(stderr, "usage: sampler_render 音色.patch 鍵 ベロシティ 押さえる秒 出力.wav\n");
		return 1;
	}
	const std::string patch = argv[1];
	const int note = std::atoi(argv[2]), vel = std::atoi(argv[3]);
	const double hold = std::atof(argv[4]);
	const std::string dir = patch.substr(0, patch.find_last_of("/\\") + 1);

	std::string fxdir;
	struct ctl_ev { double t; int src, val; };
	std::vector<ctl_ev> ctl;
	int note_index = 0;
	long seed = -1;
	struct note_ev { double t; bool on; int key, vel; };
	std::vector<note_ev> nev;
	bool use_ins = true, use_rev = true, use_cho = true, majority = false;
	for (int i = 6; i < argc; i++) {
		if (!std::strcmp(argv[i], "--fx") && i + 1 < argc)
			fxdir = argv[++i];
		else if (!std::strcmp(argv[i], "--no-ins"))
			use_ins = false;
		else if (!std::strcmp(argv[i], "--no-reverb"))
			use_rev = false;
		else if (!std::strcmp(argv[i], "--no-chorus"))
			use_cho = false;
		else if (!std::strcmp(argv[i], "--ctl") && i + 1 < argc) {
			std::istringstream ss(argv[++i]);
			std::string t;
			while (std::getline(ss, t, ',')) {
				const size_t e = t.find('='), a = t.find('@');
				if (e == std::string::npos)
					continue;
				ctl.push_back({ a == std::string::npos ? 0.0 : std::atof(t.c_str() + a + 1), std::atoi(t.c_str()), std::atoi(t.c_str() + e + 1) });
			}
		}
		else if (!std::strcmp(argv[i], "--events") && i + 1 < argc) {
			std::istringstream ss(argv[++i]);
			std::string t;
			while (std::getline(ss, t, ',')) {
				double tt = 0;
				int k = 60, v = 100;
				if (!std::strncmp(t.c_str(), "on@", 3) && std::sscanf(t.c_str() + 3, "%lf:%d:%d", &tt, &k, &v) >= 2)
					nev.push_back({ tt, true, k, v });
				else if (!std::strncmp(t.c_str(), "off@", 4) && std::sscanf(t.c_str() + 4, "%lf:%d", &tt, &k) == 2)
					nev.push_back({ tt, false, k, 0 });
			}
		}
		else if (!std::strcmp(argv[i], "--note-index") && i + 1 < argc)
			note_index = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc)
			seed = std::atol(argv[++i]);
		else if (!std::strcmp(argv[i], "--ins-majority"))      // エレメントのスイッチの多数決で全体を 1 つの経路に通す（前の形。mr_fx_apply と同じ）
			majority = true;
	}
	kv fxline, voiceline, clfoline, ctrlline;
	std::map<int, int> ins_count;

	std::vector<element> els;
	FILE *pf = std::fopen(patch.c_str(), "r");
	if (!pf) {
		std::fprintf(stderr, "パッチを開けない: %s\n", patch.c_str());
		return 1;
	}
	char line[4096];
	while (std::fgets(line, sizeof line, pf)) {
		if (!std::strncmp(line, "fx ", 3))
			fxline = parse(line);
		else if (!std::strncmp(line, "voice ", 6))
			voiceline = parse(line);
		else if (!std::strncmp(line, "clfo ", 5))
			clfoline = parse(line);
		else if (!std::strncmp(line, "ctrl ", 5))
			ctrlline = parse(line);
		else if (!std::strncmp(line, "element ", 8))
			els.push_back({ parse(line), {} });
		else if (!std::strncmp(line, "zone ", 5) && !els.empty()) {
			const kv m = parse(line);
			std::string file = m.count("file") ? m.at("file") : "";
			while (!file.empty() && (file.back() == '\n' || file.back() == '\r'))
				file.pop_back();
			els.back().zones.push_back({ geti(m, "key_lo"), geti(m, "key_hi"), geti(m, "vel_lo"), geti(m, "vel_hi"), file });
		}
	}
	std::fclose(pf);

	std::vector<std::unique_ptr<motif_sampler::sample>> samples;
	std::vector<int> vgroup;                 // エレメントのインサーションのスイッチ（0・1・2）
	std::vector<int> vdelay;                 // エレメントの Key On Delay（サンプル）
	std::vector<int> vcbox;                  // エレメントのコントロールボックス（ビット 0〜5 = 組 1〜6）
	std::vector<motif_sampler::voice> voices;
	std::mt19937 rng(seed < 0 ? 0u : unsigned(seed));
	// コントロールセット: 組ごとに（ソース・行き先・深さ）。ソースの値が変わる時刻だけ、エレメントごとのずれを決め直す
	struct cset { int src, dest, depth; };
	std::vector<cset> sets;
	{
		std::istringstream ss(ctrlline.count("sets") ? ctrlline.at("sets") : "");
		std::string t;
		while (std::getline(ss, t, ',')) {
			int a = 0, b = 0, c = 64;
			if (std::sscanf(t.c_str(), "%d:%d:%d", &a, &b, &c) == 3)
				sets.push_back({ a, b, c });
		}
	}
	auto bipolar = [](int src) { return src == 0 || src == 6 || src == 8 || src == 9; };
	int srcval[16];
	for (int k = 0; k < 16; k++)
		srcval[k] = bipolar(k) ? 64 : 0;
	std::sort(ctl.begin(), ctl.end(), [](const ctl_ev &a, const ctl_ev &b) { return a.t < b.t; });
	// EG の行き先（50〜65）はキーオンのときのソースの値で決まる（鳴っている間に動かしても変わらない）ので、鳴らす前に時間・深さの値をずらす
	auto eg_ctrl = [&](motif_sampler::element_params &p, int cbox) {
		for (size_t k = 0; k < sets.size() && k < 6; k++) {
			const cset &c = sets[k];
			if (c.dest < 50 || c.dest > 65 || c.depth == 64 || c.src < 0 || c.src > 15 || !(cbox >> k & 1))
				continue;
			const int v = srcval[c.src];         // その音を鳴らしたときのソースの値
			motif_sampler::ctrl_eg(p, c.dest, (c.depth - 64) / 64.0 * (bipolar(c.src) ? (v - 64) / 64.0 : v / 127.0));
		}
	};
	for (const element &e : els)
		ins_count[geti(e.f, "ins")]++;      // インサーションのスイッチは、鳴る鳴らないによらず入っているエレメント全部の多数決（mr_fx_apply.apply_insertion と同じ）
	std::map<std::string, std::unique_ptr<motif_sampler::sample>> cache;
	auto get_sample = [&](const std::string &file) -> const motif_sampler::sample * {
		auto it = cache.find(file);
		if (it != cache.end())
			return it->second.get();
		auto smp = std::make_unique<motif_sampler::sample>();
		if (!load_smpx(dir + file, *smp)) {
			std::fprintf(stderr, "サンプルを読めない: %s\n", file.c_str());
			cache[file] = nullptr;
			return nullptr;
		}
		return (cache[file] = std::move(smp)).get();
	};
	std::vector<int> vnote;                  // エレメントがどの音（ノートオンの通し番号）のものか
	// 1 音ぶんのエレメントを作る。t_on はその音の鳴り始め（サンプル）、note_id は通し番号、nidx は音色を選んでから何音目か（オルタネートパン）
	auto add_note = [&](int note, int vel, int t_on, int note_id, int note_index) {
		for (const element &e : els) {
			if (note < geti(e.f, "note_lo") || note > geti(e.f, "note_hi") || vel < geti(e.f, "vel_lo") || vel > geti(e.f, "vel_hi"))
				continue;
			// 区間は、コースチューンでずらしたあとの鍵で選ぶ（実機: PBasFngDst のエレメント 1 はコース −12 で、鍵 60 なら鍵 48 で録ったサンプルの減り方になる）
			const int zk = std::max(0, std::min(127, note + geti(e.f, "coarse")));
			const zone *z = nullptr;
			int best = 1 << 30;
			for (const zone &c : e.zones) {
				if (zk >= c.key_lo && zk <= c.key_hi && vel >= c.vel_lo && vel <= c.vel_hi) {
					z = &c;
					break;
				}
				const int d = std::abs((c.key_lo + c.key_hi) / 2 - zk);
				if (d < best) {
					best = d;
					z = &c;
				}
			}
			if (!z)
				continue;
			const motif_sampler::sample *s = get_sample(z->file);
			if (!s)
				continue;
			motif_sampler::element_params p;
			p.coarse = geti(e.f, "coarse");
			p.fine = geti(e.f, "fine");
			p.level = geti(e.f, "level", 127);
			// Filter Gain は 1 あたり 0.375dB。サンプルは雛形の音色（250）で録ったので、その差だけ掛ける
			p.gain = std::pow(10.0, (swp50_filter::filter_gain_db(geti(e.f, "fgain", 250)) - swp50_filter::filter_gain_db(250)) / 20.0);
			p.pan = geti(e.f, "pan", 64);
			// 1 音ごとに変わるもの（tools/motif/mr_pernote.py、2026-10-25）
			p.pan += motif_sampler::alternate_pan_offset(geti(e.f, "apan", 64), note_index);
			if (seed >= 0) {
				p.pan += motif_sampler::random_pan_offset(geti(e.f, "rpan"), rng);
				p.fine += 12 * motif_sampler::random_pitch_steps(geti(e.f, "rpitch"), rng);     // 1 刻み 18.75 セント = ファイン 12
			}
			p.scaling_pan = geti(e.f, "span", 64);
			if (!clfoline.empty()) {
				motif_sampler::common_lfo &c = p.clfo;
				c.on = true;
				c.phase = geti(clfoline, "phase");
				c.wave = geti(clfoline, "wave");
				c.speed = geti(clfoline, "speed", 32);
				c.tspeed = geti(clfoline, "tspeed", 11);
				c.sync = geti(clfoline, "sync");
				c.delay = geti(clfoline, "delay");
				c.fadein = geti(clfoline, "fadein");
				c.reset = geti(clfoline, "reset", 1);
				c.hold = geti(clfoline, "hold", 127);
				c.fadeout = geti(clfoline, "fadeout");
				const auto ds = list(clfoline, "dest"), dp = list(clfoline, "depth"), uv = list(clfoline, "uvals");
				for (int b = 0; b < 3 && b < int(ds.size()) && b < int(dp.size()); b++) {
					c.dest[b] = ds[b];
					c.depth[b] = dp[b];
				}
				c.usteps = geti(clfoline, "usteps");
				for (int i = 0; i < 16 && i < int(uv.size()); i++)
					c.uvals[i] = uv[i];
				c.tempo = geti(fxline, "tempo", 120);
			}
			p.lbox = geti(e.f, "lbox");
			{
				const auto lr = list(e.f, "lratio");
				for (int i = 0; i < 4 && i < int(lr.size()); i++)
					p.lratio[i] = lr[i];
			}
			const auto at = list(e.f, "aegt"), al = list(e.f, "aegl");
			for (int i = 0; i < 4 && i < int(at.size()); i++)
				p.aeg.time[i] = at[i];
			for (int i = 0; i < 4 && i < int(al.size()); i++)
				p.aeg.level[i] = al[i];
			if (at.empty())
				p.aeg.time[3] = geti(e.f, "release");
			p.aeg.time_vel_sens = geti(e.f, "aegtv", 64);
			p.aeg.time_vel_segment = geti(e.f, "aegtvs", 4);
			p.aeg.time_kf = geti(e.f, "aegkf", 64);
			p.aeg.time_kf_center = geti(e.f, "aegkc", 60);
			p.level_vel_sens = geti(e.f, "lvel", 64);
			p.level_vel_curve = geti(e.f, "lcurve");
			p.level_kf = geti(e.f, "lkf", 64);
			const auto lbp = list(e.f, "lbp"), loff = list(e.f, "loff");
			for (int i = 0; i < 4 && i < int(lbp.size()); i++)
				p.level_bp[i] = lbp[i];
			for (int i = 0; i < 4 && i < int(loff.size()); i++)
				p.level_off[i] = loff[i];
			p.lfo.wave = geti(e.f, "lfow", 1);
			p.lfo.key_sync = geti(e.f, "lfosync", 1) != 0;
			p.lfo.delay = geti(e.f, "lfodelay");
			p.lfo.speed = geti(e.f, "lfospeed", 32);
			p.lfo.amod = geti(e.f, "lfoa");
			p.lfo.pmod = geti(e.f, "lfop");
			p.lfo.fmod = geti(e.f, "lfof");
			p.lfo.fade = geti(e.f, "lfofade");
			auto &fp = p.filter;
			fp.type = geti(e.f, "ftype", 21);
			fp.cutoff = geti(e.f, "cutoff", 255);
			fp.vel_sens = geti(e.f, "fvel", 64);
			fp.reso = geti(e.f, "reso");
			fp.reso_vel_sens = geti(e.f, "rvel", 64);
			fp.hpf = geti(e.f, "hpf");
			fp.distance = geti(e.f, "distance");
			fp.keyfollow = geti(e.f, "fkf", 64);
			fp.hpf_keyfollow = geti(e.f, "hkf", 64);
			const auto bp = list(e.f, "bp"), off = list(e.f, "off"), ft = list(e.f, "fegt"), fl = list(e.f, "fegl");
			for (int i = 0; i < 4 && i < int(bp.size()); i++)
				fp.bp[i] = bp[i];
			for (int i = 0; i < 4 && i < int(off.size()); i++)
				fp.offset[i] = off[i];
			for (int i = 0; i < 5 && i < int(ft.size()); i++)
				p.feg.time[i] = ft[i];
			for (int i = 0; i < 5 && i < int(fl.size()); i++)
				p.feg.level[i] = fl[i];
			p.feg.depth = geti(e.f, "fegd", 64);
			p.feg.time_vel_sens = geti(e.f, "fegtv", 64);
			p.feg.time_vel_segment = geti(e.f, "fegtvs", 4);
			p.feg.level_factor = swp50_filter::feg_level_velocity_factor(geti(e.f, "feglv", 64), geti(e.f, "feglc"), vel);
			p.feg.time_kf = geti(e.f, "fegkf", 64);
			p.feg.time_kf_center = geti(e.f, "fegkc", 60);
			const auto pt = list(e.f, "pegt"), pl = list(e.f, "pegl");
			for (int i = 0; i < 5 && i < int(pt.size()); i++)
				p.peg.time[i] = pt[i];
			for (int i = 0; i < 5 && i < int(pl.size()); i++)
				p.peg.level[i] = pl[i];
			p.peg.depth = geti(e.f, "pegd", 64);
			p.peg.time_vel_sens = geti(e.f, "pegtv", 64);
			p.peg.time_vel_segment = geti(e.f, "pegtvs", 4);
			p.peg.level_factor = swp50_filter::feg_level_velocity_factor(geti(e.f, "peglv", 64), geti(e.f, "peglc"), vel);
			p.peg.time_kf = geti(e.f, "pegkf", 64);
			p.peg.time_kf_center = geti(e.f, "pegkc", 60);
			p.pitch_kf = geti(e.f, "pkf", 96);
			p.pitch_kf_center = geti(e.f, "pkc", 60);
			p.pitch_vel_sens = geti(e.f, "pvel", 64);
			p.eq_type = geti(e.f, "eqtype", swp50_eq::THRU);
			p.eq_res = geti(e.f, "eqres");
			p.eq_f1 = geti(e.f, "eqf1", 100);
			p.eq_g1 = geti(e.f, "eqg1", 0x40);
			p.eq_f2 = geti(e.f, "eqf2", 200);
			p.eq_g2 = geti(e.f, "eqg2", 0x40);
			voices.emplace_back();
			eg_ctrl(p, geti(e.f, "cbox"));
			voices.back().start(*s, p, note, vel);
			vgroup.push_back(std::max(0, std::min(2, geti(e.f, "ins"))));
			vcbox.push_back(geti(e.f, "cbox"));
			vnote.push_back(note_id);
			// テンポ同期なら音色のテンポ（fx の tempo）で拍を秒に直す（Willows のエレメント 3 は 2 分音符・テンポ 48 で 2.5 秒遅れ、2 秒押さえでは鳴らない）
			const double kd_ms = geti(e.f, "ksync") ? key_on_delay_beats(geti(e.f, "klen")) * 60000.0 / std::max(1, geti(fxline, "tempo", 120))
			                                        : key_on_delay_ms(geti(e.f, "kdelay"));
			vdelay.push_back(t_on + int(kd_ms / 1000.0 * 44100 + 0.5));
		}
	};
	// 鳴らす音の並び（--events が無ければ、鍵・ベロシティを 0 秒から押さえる秒まで）
	if (nev.empty()) {
		nev.push_back({ 0.0, true, note, vel });
		nev.push_back({ hold, false, note, 0 });
	}
	std::stable_sort(nev.begin(), nev.end(), [](const note_ev &a, const note_ev &b) { return a.t < b.t; });
	const int n = int((nev.back().t + 3.0) * 44100);
	std::vector<float> l(n), r(n);
	std::vector<float> gl[3] = {std::vector<float>(n), std::vector<float>(n), std::vector<float>(n)};
	std::vector<float> gr[3] = {std::vector<float>(n), std::vector<float>(n), std::vector<float>(n)};
	size_t ci = 0;
	double vol_off = 0;                      // 行き先 1（Volume）: 音色の音量の値のずれ（エレメントのボックスによらない）
	std::vector<float> voff(n);
	auto apply_ctl = [&]() {
		vol_off = 0;
		for (size_t k = 0; k < sets.size() && k < 6; k++) {
			const cset &c = sets[k];
			if (c.dest != 1 || c.depth == 64 || c.src < 0 || c.src > 15)
				continue;
			const int v = srcval[c.src];
			vol_off += (c.depth - 64) / 64.0 * (bipolar(c.src) ? (v - 64) / 64.0 : v / 127.0) * motif_sampler::CTRL_VOLUME_SCALE;
		}
		for (size_t vi = 0; vi < voices.size(); vi++) {
			motif_sampler::ctrl_mod m;
			for (size_t k = 0; k < sets.size() && k < 6; k++) {
				const cset &c = sets[k];
				if (c.dest <= 1 || c.depth == 64 || c.src < 0 || c.src > 15)
					continue;
				if (c.dest >= 41 && !(vcbox[vi] >> k & 1))     // 41 から上はエレメントの行き先（36〜40 はコモン LFO などの音色全体）
					continue;
				const int v = srcval[c.src];
				const double x = bipolar(c.src) ? (v - 64) / 64.0 : v / 127.0;
				motif_sampler::ctrl_add(m, c.dest, (c.depth - 64) / 64.0 * x);
			}
			// ピッチベンド: 上は pbup − 64、下は 64 − pbdown 半音
			const int pb = srcval[0];
			if (pb != 64)
				m.cents += pb > 64 ? (pb - 64) / 63.0 * (geti(ctrlline, "pbup", 66) - 64) * 100.0 : (pb - 64) / 64.0 * (64 - geti(ctrlline, "pbdown", 62)) * 100.0;
			voices[vi].cm = m;
		}
	};
	auto next_ctl = [&](int i) {
		bool ch = i == 0;
		while (ci < ctl.size() && ctl[ci].t * 44100 <= i) {
			srcval[std::max(0, std::min(15, ctl[ci].src))] = std::max(0, std::min(127, ctl[ci].val));
			ci++;
			ch = true;
		}
		if (ch)
			apply_ctl();
	};
	// 音ごとの様子。held は鍵を押さえているか、key は鳴らした鍵、t0 は鳴り始め
	struct note_state { int key, vel, t0; bool held, alive; };
	std::vector<note_state> notes;
	const bool mono = geti(voiceline, "mono", 1) == 0, same_single = geti(voiceline, "same", 1) == 0;
	const bool porta = geti(voiceline, "porta", 0) != 0, fulltime = geti(voiceline, "pmode", 0) != 0;
	const int ptime = geti(voiceline, "ptime", 0), ptmode = geti(voiceline, "ptmode", 0);
	int last_key = -1;                       // いちばん最後に鳴らした鍵（ポルタメントの出どころ）
	std::vector<int> held_stack;             // モノで押さえている鍵の並び（後が新しい）
	int nidx = note_index;
	// ポルタメント: 時間のモード 0・2 は速さ一定（1 オクターブあたり porta_octave_seconds 秒）、1・3 は幅によらず同じ時間で着く。
	// 2・3 は 1 オクターブより遠いと、まず残り 1 オクターブの所へ跳んでから滑る（実機: 24 半音で、10%・50% を跳んで越え、90% までが 1 オクターブぶん）
	auto glide_rate = [&](double cents) {
		const double sec = std::max(1e-4, motif_sampler::porta_octave_seconds(ptime));
		return (ptmode & 1) ? std::fabs(cents) / (sec * 44100.0) : 1200.0 / (sec * 44100.0);
	};
	auto glide_from = [&](double c) { return ptmode >= 2 ? std::max(-1200.0, std::min(1200.0, c)) : c; };
	auto voices_of = [&](int id, auto fn) {
		for (size_t vi = 0; vi < voices.size(); vi++)
			if (vnote[vi] == id)
				fn(voices[vi]);
	};
	auto release_note = [&](int id) {
		notes[id].held = false;
		voices_of(id, [](motif_sampler::voice &v) { v.release(); });
	};
	auto kill_note = [&](int id) {
		notes[id].held = false;
		notes[id].alive = false;
		voices_of(id, [](motif_sampler::voice &v) { v.steal(); });
	};
	auto start_note = [&](int key, int vel, int i) {
		const size_t first = voices.size();
		const int id = int(notes.size());
		notes.push_back({ key, vel, i, true, true });
		add_note(key, vel, i, id, nidx++);
		// 同じ時刻に始まるので、コントロールセットのずれを入れ直す
		apply_ctl();
		// ポルタメント（フルタイムはいつでも、フィンガードは前の鍵を押さえたままのときだけ）、前に鳴らした鍵から滑る
		if (porta && last_key >= 0 && last_key != key) {
			bool legato = false;
			for (const note_state &ns : notes)
				if (&ns != &notes.back() && ns.held && ns.key == last_key)
					legato = true;
			if (fulltime || legato) {
				const double c = glide_from((last_key - key) * 100.0);
				for (size_t vi = first; vi < voices.size(); vi++) {
					voices[vi].glide = c;
					voices[vi].glide_to = 0;
					voices[vi].glide_rate = glide_rate(c);
				}
			}
		}
		last_key = key;
		return id;
	};
	// 発音数: エレメント 128 音。超えたら、押さえている中でいちばん低い鍵を除いて、いちばん古い音をまるごと奪う（実機: 4 エレメント × 33 鍵で
	// 2 番目に古い鍵が消え、上から弾くといちばん古い鍵から消えた）。離してリリース中の音を先に奪う
	auto make_room = [&](int need) {
		for (;;) {
			int busy = 0;
			for (const auto &v : voices)
				if (!v.done && !v.kill)
					busy++;
			if (busy + need <= 128)
				return;
			int lowest = -1;
			for (size_t k = 0; k < notes.size(); k++)
				if (notes[k].alive && notes[k].held && (lowest < 0 || notes[k].key < notes[lowest].key))
					lowest = int(k);
			int victim = -1;
			for (int pass = 0; pass < 2 && victim < 0; pass++)
				for (size_t k = 0; k < notes.size(); k++) {
					if (!notes[k].alive || int(k) == lowest || (pass == 0 && notes[k].held))
						continue;
					bool sounding = false;
					voices_of(int(k), [&](motif_sampler::voice &v) { sounding |= !v.done && !v.kill; });
					if (!sounding) {
						notes[k].alive = false;
						continue;
					}
					victim = int(k);
					break;
				}
			if (victim < 0)
				return;
			kill_note(victim);
		}
	};
	auto count_elements = [&](int key, int vel) {
		int c = 0;
		for (const element &e : els)
			if (key >= geti(e.f, "note_lo") && key <= geti(e.f, "note_hi") && vel >= geti(e.f, "vel_lo") && vel <= geti(e.f, "vel_hi"))
				c++;
		return c;
	};
	size_t ei = 0;
	for (int i = 0; i < n; i++) {
		next_ctl(i);
		voff[i] = float(vol_off);
		while (ei < nev.size() && nev[ei].t * 44100 <= i) {
			const note_ev &ev = nev[ei++];
			if (ev.on) {
				if (mono) {
					// モノ: 鳴っている音を止める。ポルタメントがあって前の鍵を押さえたままなら、EG はやり直さずに音程だけ滑らせる（レガート）
					int cur = -1;
					for (size_t k = 0; k < notes.size(); k++)
						if (notes[k].alive && notes[k].held)
							cur = int(k);
					held_stack.push_back(ev.key);
					if (cur >= 0 && porta) {
						const double c = (ev.key - notes[cur].key) * 100.0;
						voices_of(cur, [&](motif_sampler::voice &v) { v.glide = c - glide_from(c - v.glide); v.glide_to = c; v.glide_rate = glide_rate(c - v.glide); });
						last_key = ev.key;
						continue;
					}
					for (size_t k = 0; k < notes.size(); k++)
						if (notes[k].alive)
							kill_note(int(k));
					start_note(ev.key, ev.vel, i);
					continue;
				}
				if (same_single)
					for (size_t k = 0; k < notes.size(); k++)
						if (notes[k].alive && notes[k].key == ev.key)
							kill_note(int(k));
				make_room(count_elements(ev.key, ev.vel));
				start_note(ev.key, ev.vel, i);
			} else if (mono) {
				// モノ: 離した鍵が今の音なら、押さえたままの前の鍵へ戻る（ポルタメントがあれば滑って、無ければやり直して）
				auto it = std::find(held_stack.rbegin(), held_stack.rend(), ev.key);
				if (it == held_stack.rend())
					continue;
				const bool top = it == held_stack.rbegin();
				held_stack.erase(std::next(it).base());
				if (!top)
					continue;
				int cur = -1;
				for (size_t k = 0; k < notes.size(); k++)
					if (notes[k].alive && notes[k].held)
						cur = int(k);
				if (cur < 0)
					continue;
				if (held_stack.empty()) {
					release_note(cur);
					continue;
				}
				const int back = held_stack.back();
				if (porta) {
					const double c = (back - notes[cur].key) * 100.0;
					voices_of(cur, [&](motif_sampler::voice &v) { v.glide = c - glide_from(c - v.glide); v.glide_to = c; v.glide_rate = glide_rate(c - v.glide); });
					last_key = back;
				} else {
					kill_note(cur);
					start_note(back, notes[cur].vel, i);
				}
			} else {
				// 同じ鍵が重なっていれば、古い音から離す
				for (size_t k = 0; k < notes.size(); k++)
					if (notes[k].alive && notes[k].held && notes[k].key == ev.key) {
						release_note(int(k));
						break;
					}
			}
		}
		for (size_t vi = 0; vi < voices.size(); vi++) {
			// Key On Delay: 遅れが過ぎるまで鳴らさない。遅れより先に鍵を離したエレメントは鳴らない（実機で確かめた）
			if (i < vdelay[vi])
				continue;
			if (i == vdelay[vi] && !notes[vnote[vi]].held && vdelay[vi] > notes[vnote[vi]].t0)
				voices[vi].done = true;
			float a = 0, b = 0;
			voices[vi].render(a, b);
			gl[vgroup[vi]][i] += a;
			gr[vgroup[vi]][i] += b;
			l[i] += a;
			r[i] += b;
		}
	}
	// 共通の音量（0〜127）は 40·log10(値 / 127) dB（プリセット 29 音色の実機と芯の音量の差から）。実機ではインサーションの後で掛かる
	// （強く歪む Still Blue で、前に掛けると 11dB 大きく出た）ので、インサーションを通すときは前で戻して後で掛け直す
	// コントロールセットの Volume は、音量の値をずらしてから同じ式で掛ける
	std::vector<double> vgv(n);
	for (int i = 0; i < n; i++)
		vgv[i] = std::pow(std::max(0.0, std::min(127.0, geti(voiceline, "volume", 127) + double(voff[i]))) / 127.0, 2.0);
	for (int i = 0; i < n; i++) {
		l[i] = float(l[i] * vgv[i]);
		r[i] = float(r[i] * vgv[i]);
	}
	if (!fxdir.empty() && !fxline.empty()) {
		using namespace motif_fx;
		auto get = [&](const char *k) { auto it = fxline.find(k); return it == fxline.end() ? block() : parse_block(it->second); };
		const block rv = get("reverb"), ch = get("chorus"), ia = get("insa"), ib = get("insb");
		int sw = 0, best = -1;
		for (auto &kc : ins_count)
			if (kc.second > best) {
				best = kc.second;
				sw = kc.first;
			}
		buf L(l.begin(), l.end()), R(r.begin(), r.end());
		if (use_ins) {
			for (size_t i = 0; i < L.size(); i++) {
				const double g = i < vgv.size() && vgv[i] > 0 ? vgv[i] : 1.0;
				L[i] /= g;
				R[i] /= g;
			}
			if (majority)
				insertion(L, R, ia, ib, geti(fxline, "connect"), sw, fxdir + "/fxd", geti(fxline, "tempo", 120));
			else {
				// エレメントをスイッチごとの組に分けて、つなぎ方どおりに通す（--ins-majority で前の多数決に戻せる。mr_fx_apply.apply_insertion は多数決）
				buf g[3][2];
				for (int s2 = 0; s2 < 3; s2++)
					for (int c = 0; c < 2; c++) {
						const std::vector<float> &src = c == 0 ? gl[s2] : gr[s2];
						g[s2][c].assign(src.begin(), src.end());      // 組の音は共通の音量を掛ける前のもの
					}
				insertion_groups(g, ia, ib, geti(fxline, "connect"), fxdir + "/fxd", geti(fxline, "tempo", 120), L, R);
			}
			for (size_t i = 0; i < L.size(); i++) {
				const double g = i < vgv.size() ? vgv[i] : vgv.back();
				L[i] *= g;
				R[i] *= g;
			}
		}
		buf oL(L), oR(R);
		if (use_rev)
			reverb(L, R, rv, fxdir + "/fx", oL, oR, L.size());
		if (use_cho)
			system_chorus(L, R, ch, oL, oR, fxdir + "/fxd", geti(fxline, "tempo", 120));
		for (size_t i = 0; i < l.size(); i++) {
			l[i] = float(oL[i]);
			r[i] = float(oR[i]);
		}
	}
	// 実機の出口（デジタル出力）は ±1.0 で頭打ち（発振した LPF24D をフィルターゲイン 250 で鳴らすと、山が 0dBFS の四角い波になる）
	for (size_t i = 0; i < l.size(); i++) {
		l[i] = std::fmax(-1.0f, std::fmin(1.0f, l[i]));
		r[i] = std::fmax(-1.0f, std::fmin(1.0f, r[i]));
	}
	write_wav(argv[5], l, r);
	std::printf("鳴らしたエレメント %zu\n", voices.size());
	return 0;
}
