// license:BSD-3-Clause
// サンプリング方式の芯（sampler.h）で、パッチ（tools/motif/mr_patch.py が作る文字ファイル）の音を 1 つ鳴らして WAV に書く。
//
//   g++ -std=c++17 -O2 -o sampler_render src/motif/sampler_render.cpp
//   sampler_render 音色.patch 鍵 ベロシティ 押さえる秒 出力.wav
//
// パッチのエレメントのうち、鍵とベロシティの範囲に入るものを全部鳴らす。サンプルは、そのエレメントの区間のうち
// 鍵とベロシティを含むもの（無ければ鍵がいちばん近いもの）。出力は 44.1kHz・32bit float のステレオ。

#include "sampler.h"

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

	std::vector<element> els;
	FILE *pf = std::fopen(patch.c_str(), "r");
	if (!pf) {
		std::fprintf(stderr, "パッチを開けない: %s\n", patch.c_str());
		return 1;
	}
	char line[4096];
	while (std::fgets(line, sizeof line, pf)) {
		if (!std::strncmp(line, "element ", 8))
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
	std::vector<motif_sampler::voice> voices;
	for (const element &e : els) {
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
		p.pan = geti(e.f, "pan", 64);
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
		p.feg.level_factor = swp50_filter::feg_level_velocity_factor(geti(e.f, "feglv", 64), geti(e.f, "feglc"), vel);
		p.feg.time_kf = geti(e.f, "fegkf", 64);
		p.feg.time_kf_center = geti(e.f, "fegkc", 60);
		p.eq_type = geti(e.f, "eqtype", swp50_eq::THRU);
		p.eq_res = geti(e.f, "eqres");
		p.eq_f1 = geti(e.f, "eqf1", 100);
		p.eq_g1 = geti(e.f, "eqg1", 0x40);
		p.eq_f2 = geti(e.f, "eqf2", 200);
		p.eq_g2 = geti(e.f, "eqg2", 0x40);
		samples.push_back(std::move(s));
		voices.emplace_back();
		voices.back().start(*samples.back(), p, note, vel);
	}
	const int n = int((hold + 3.0) * 44100), off_at = int(hold * 44100);
	std::vector<float> l(n), r(n);
	for (int i = 0; i < n; i++) {
		if (i == off_at)
			for (auto &v : voices)
				v.release();
		for (auto &v : voices)
			v.render(l[i], r[i]);
	}
	write_wav(argv[5], l, r);
	std::printf("鳴らしたエレメント %zu\n", voices.size());
	return 0;
}
