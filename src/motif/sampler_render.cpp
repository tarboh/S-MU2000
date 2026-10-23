// license:BSD-3-Clause
// サンプリング方式の芯（sampler.h）で、パッチ（tools/motif/mr_patch.py が作る文字ファイル）の音を 1 つ鳴らして WAV に書く。
//
//   g++ -std=c++17 -O2 -o sampler_render src/motif/sampler_render.cpp
//   sampler_render 音色.patch 鍵 ベロシティ 押さえる秒 出力.wav [--fx capture の置き場] [--no-ins] [--no-reverb] [--no-chorus] [--ins-majority]
//
// --fx を付けると、パッチの fx の行のエフェクト（sampler_fx.h）を掛ける。置き場の fx/*.irx（リバーブ）と fxd/*.dsx（歪み系）を使う
// （tools/motif/mr_fx_export.py が書く）。インサーションのスイッチは、鳴ったエレメントでいちばん多いもの。
//
// パッチのエレメントのうち、鍵とベロシティの範囲に入るものを全部鳴らす。サンプルは、そのエレメントの区間のうち
// 鍵とベロシティを含むもの（無ければ鍵がいちばん近いもの）。出力は 44.1kHz・32bit float のステレオ。

#include "sampler.h"
#include "sampler_fx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
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
		else if (!std::strcmp(argv[i], "--ins-majority"))      // エレメントのスイッチの多数決で全体を 1 つの経路に通す（前の形。mr_fx_apply と同じ）
			majority = true;
	}
	kv fxline, voiceline, clfoline;
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
	std::vector<motif_sampler::voice> voices;
	for (const element &e : els) {
		ins_count[geti(e.f, "ins")]++;      // インサーションのスイッチは、鳴る鳴らないによらず入っているエレメント全部の多数決（mr_fx_apply.apply_insertion と同じ）
		if (note < geti(e.f, "note_lo") || note > geti(e.f, "note_hi") || vel < geti(e.f, "vel_lo") || vel > geti(e.f, "vel_hi"))
			continue;
		const zone *z = nullptr;
		int best = 1 << 30;
		for (const zone &c : e.zones) {
			if (note >= c.key_lo && note <= c.key_hi && vel >= c.vel_lo && vel <= c.vel_hi) {
				z = &c;
				break;
			}
			const int d = std::abs((c.key_lo + c.key_hi) / 2 - note);
			if (d < best) {
				best = d;
				z = &c;
			}
		}
		if (!z)
			continue;
		auto s = std::make_unique<motif_sampler::sample>();
		if (!load_smpx(dir + z->file, *s)) {
			std::fprintf(stderr, "サンプルを読めない: %s\n", z->file.c_str());
			continue;
		}
		motif_sampler::element_params p;
		p.coarse = geti(e.f, "coarse");
		p.fine = geti(e.f, "fine");
		p.level = geti(e.f, "level", 127);
		// Filter Gain は 1 あたり 0.375dB。サンプルは雛形の音色（250）で録ったので、その差だけ掛ける
		p.gain = std::pow(10.0, (swp50_filter::filter_gain_db(geti(e.f, "fgain", 250)) - swp50_filter::filter_gain_db(250)) / 20.0);
		p.pan = geti(e.f, "pan", 64);
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
		samples.push_back(std::move(s));
		voices.emplace_back();
		voices.back().start(*samples.back(), p, note, vel);
		vgroup.push_back(std::max(0, std::min(2, geti(e.f, "ins"))));
		vdelay.push_back(int(key_on_delay_ms(geti(e.f, "kdelay")) / 1000.0 * 44100 + 0.5));
	}
	const int n = int((hold + 3.0) * 44100), off_at = int(hold * 44100);
	std::vector<float> l(n), r(n);
	std::vector<float> gl[3] = {std::vector<float>(n), std::vector<float>(n), std::vector<float>(n)};
	std::vector<float> gr[3] = {std::vector<float>(n), std::vector<float>(n), std::vector<float>(n)};
	for (int i = 0; i < n; i++) {
		if (i == off_at)
			for (auto &v : voices)
				v.release();
		for (size_t vi = 0; vi < voices.size(); vi++) {
			// Key On Delay: 遅れが過ぎるまで鳴らさない。遅れより先に鍵を離したエレメントは鳴らない（実機で確かめた）
			if (i < vdelay[vi] || vdelay[vi] >= off_at)
				continue;
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
	const double vg = std::pow(geti(voiceline, "volume", 127) / 127.0, 2.0);
	for (int i = 0; i < n; i++) {
		l[i] = float(l[i] * vg);
		r[i] = float(r[i] * vg);
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
		if (use_ins && vg > 0) {
			for (size_t i = 0; i < L.size(); i++) {
				L[i] /= vg;
				R[i] /= vg;
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
				L[i] *= vg;
				R[i] *= vg;
			}
		}
		buf oL(L), oR(R);
		if (use_rev)
			reverb(L, R, rv, fxdir + "/fx", oL, oR, L.size());
		if (use_cho)
			chorus(L, R, ch, oL, oR);
		for (size_t i = 0; i < l.size(); i++) {
			l[i] = float(oL[i]);
			r[i] = float(oR[i]);
		}
	}
	write_wav(argv[5], l, r);
	std::printf("鳴らしたエレメント %zu\n", voices.size());
	return 0;
}
