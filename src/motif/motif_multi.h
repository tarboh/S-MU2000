// MOTIF-RACK ES のマルチ（16 パート）を、サンプリング方式の芯（sampler_engine.h）で鳴らす（2026-10-25）。
// 設定は tools/motif/mr_multi_export.py がマルチのダンプから書く .multi（capture/multis/）。
//   パートごと: 音色（バンクと番号 → パッチ）・受信チャンネル・モノ/ポリ・ベロシティと鍵の範囲・ピッチベンドの幅・ベロシティの効き（深さと
//   オフセット）・音量・パン・リバーブとコーラスへの送り・ドライのレベル・ノートシフト・ポルタメント・Voice Pan・アルペジオのスイッチ
//   マルチ全体: リバーブとコーラス（パートの送りを足したものに掛ける）、インサーションを使うパート（8 つまで。パートの音色のインサーションを掛ける）、
//   アルペジオ（1 つ。スイッチの入ったパートのうち、鍵を受けたチャンネルのものが鳴らす）
// まだ無いもの: ドラムボイスのパート（鳴らない）、パートのフィルターと EG のずらし・デチューン、マスター EQ・マスターエフェクト、
// コーラスからリバーブへの送り、パートの出口の割り当て。ベロシティの効き（深さ × 値 / 64 + オフセット − 64）は Data List の説明からで、実機では測っていない。
#pragma once

#include "sampler_arp.h"
#include "sampler_engine.h"
#include "sampler_fx_rt.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace motif_multi {

using motif_sampler::kv;
using motif_sampler::geti;

// アルペジオの出したノート（ブロックの中の位置つき）
struct arp_event {
	int pos, type, a, b;
};

struct part_cfg {
	std::string voice;                   // パッチのファイル名の頭（00_016 など）。ドラム・不明は "-"
	std::string bank;                    // 表示用（PRE3:017 など）
	kv f;                                // part 行のほかの欄（ch・vol・pan・rev・cho・dry・shift…）
	int get(const char *k, int d = 0) const { return geti(f, k, d); }
};

struct config {
	std::string name, file;
	kv arpline, fxline;
	std::vector<int> ins;                // インサーションを使うパート（0〜15）
	std::array<part_cfg, 16> part;

	bool load(const std::string &path)
	{
		FILE *fp = std::fopen(path.c_str(), "rb");
		if (!fp)
			return false;
		file = path;
		char line[4096];
		while (std::fgets(line, sizeof line, fp)) {
			std::string t = line;
			while (!t.empty() && (t.back() == '\n' || t.back() == '\r'))
				t.pop_back();
			if (t.empty())
				continue;
			if (t[0] == '#') {
				name = t.substr(1);
				while (!name.empty() && name.front() == ' ')
					name.erase(name.begin());
			} else if (!t.compare(0, 4, "arp "))
				arpline = motif_sampler::parse(t);
			else if (!t.compare(0, 3, "fx "))
				fxline = motif_sampler::parse(t);
			else if (!t.compare(0, 4, "ins ")) {
				const kv m = motif_sampler::parse(t);
				auto it = m.find("parts");
				if (it != m.end())
					for (const char *c = it->second.c_str(); *c;) {
						ins.push_back(std::atoi(c));
						while (*c && *c != ',')
							c++;
						if (*c)
							c++;
					}
			} else if (!t.compare(0, 5, "part ")) {
				const int p = std::atoi(t.c_str() + 5);
				if (p < 0 || p >= 16)
					continue;
				part[size_t(p)].f = motif_sampler::parse(t);
				auto v = part[size_t(p)].f.find("voice");
				part[size_t(p)].voice = v == part[size_t(p)].f.end() ? "-" : v->second;
				auto b = part[size_t(p)].f.find("bank");
				part[size_t(p)].bank = b == part[size_t(p)].f.end() ? "" : b->second;
			}
		}
		std::fclose(fp);
		return !name.empty();
	}
};

// 置き場の .multi を（ファイル, 名前）の並びで
inline std::vector<std::pair<std::string, std::string>> scan(const std::filesystem::path &dir)
{
	std::vector<std::pair<std::string, std::string>> v;
	std::error_code ec;
	for (const auto &de : std::filesystem::directory_iterator(dir, ec)) {
		if (de.path().extension() != ".multi")
			continue;
		FILE *f = std::fopen(de.path().string().c_str(), "rb");
		char line[256] = {};
		std::string name;
		if (f && std::fgets(line, sizeof line, f) && line[0] == '#') {
			name = line + 1;
			while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
				name.pop_back();
			while (!name.empty() && name.front() == ' ')
				name.erase(name.begin());
		}
		if (f)
			std::fclose(f);
		v.push_back({de.path().string(), de.path().stem().string() + "  " + name});
	}
	std::sort(v.begin(), v.end());
	return v;
}

struct part {
	part_cfg cfg;
	std::unique_ptr<motif_sampler::patch> pt;
	std::unique_ptr<motif_sampler::engine> eg;
	motif_fx_rt::insertion_rt ins;
	bool has_ins = false;
	std::string label, ins_name;
	// 鳴らす糸と画面の糸で触る（mtx の中で）
	int ch = 0, vol = 100, pan = 64, rev = 0, cho = 0, dry = 127, exp = 127;
	bool mute = false, arpsw = false;
	bool sustain = false, sustained[128] = {};
	std::atomic<int> voices{0};
	std::atomic<float> peak{0};
	// 描いた音（render の中だけ）
	float ol[256] = {}, orr[256] = {};

	bool sounds() const { return eg != nullptr; }
};

class multi {
public:
	config cfg;
	std::array<std::unique_ptr<part>, 16> parts;
	motif_fx_rt::reverb_rt rev;
	motif_fx_rt::chorus_rt cho;
	std::string rev_name, cho_name;
	motif_arp::arpeggiator arp;
	bool use_ins = true, use_rev = true, use_cho = true;

	// 読む（読む糸で）。パッチはパッチの置き場、エフェクトの応答は capture の置き場（fx/・fxd/）から
	bool load(const config &c, const std::string &patch_dir, const std::string &cap, int threads)
	{
		cfg = c;
		const int tempo = std::max(1, geti(cfg.fxline, "tempo", 120));
		for (int i = 0; i < 16; i++) {
			auto P = std::make_unique<part>();
			P->cfg = cfg.part[size_t(i)];
			const part_cfg &pc = P->cfg;
			P->ch = pc.get("ch", i);
			P->vol = pc.get("vol", 100);
			P->pan = pc.get("pan", 64);
			P->rev = pc.get("rev", 0);
			P->cho = pc.get("cho", 0);
			P->dry = pc.get("dry", 127);
			P->arpsw = pc.get("arpsw", 0) != 0;
			if (pc.voice != "-" && !pc.voice.empty()) {
				auto pt = std::make_unique<motif_sampler::patch>();
				if (pt->load(patch_dir + "/" + pc.voice + ".patch")) {
					// パートの設定で音色の鳴らし方を上書きする（モノ/ポリ・ポルタメント・ピッチベンドの幅）
					pt->voiceline["mono"] = std::to_string(pc.get("mode", 1));
					pt->voiceline["porta"] = std::to_string(pc.get("porta", 0));
					pt->voiceline["ptime"] = std::to_string(pc.get("ptime", 0));
					pt->voiceline["pmode"] = std::to_string(pc.get("pmode", 0));
					pt->ctrlline["pbup"] = std::to_string(pc.get("pbup", 64));
					pt->ctrlline["pbdown"] = std::to_string(pc.get("pbdown", 64));
					motif_sampler::engine_options o;
					o.pan_offset = P->pan - 64;
					o.fixed_pan = pc.get("voicepan", 1) == 0;
					P->pt = std::move(pt);
					P->eg = std::make_unique<motif_sampler::engine>(*P->pt, o);
					P->eg->preload();
					P->label = pc.bank + " " + P->pt->name;
					if (std::find(cfg.ins.begin(), cfg.ins.end(), i) != cfg.ins.end()) {
						auto get = [&](const char *k) {
							auto it = P->pt->fxline.find(k);
							return it == P->pt->fxline.end() ? motif_fx::block() : motif_fx::parse_block(it->second);
						};
						const motif_fx::block ia = get("insa"), ib = get("insb");
						P->ins.init(ia, ib, geti(P->pt->fxline, "connect"), tempo, cap + "/fxd");
						P->has_ins = true;
						char t[64];
						std::snprintf(t, sizeof t, "%02X %02X・%02X %02X", ia.msb, ia.lsb, ib.msb, ib.lsb);
						P->ins_name = t;
					}
				} else
					P->label = pc.bank + "（パッチが無い: " + pc.voice + "）";
			} else
				P->label = pc.bank + "（ドラムはまだ鳴らない）";
			parts[size_t(i)] = std::move(P);
		}
		auto get = [&](const char *k) {
			auto it = cfg.fxline.find(k);
			return it == cfg.fxline.end() ? motif_fx::block() : motif_fx::parse_block(it->second);
		};
		const motif_fx::block rv = get("reverb"), ch = get("chorus");
		char t[96];
		rev.init(rv, cap + "/fx");
		std::snprintf(t, sizeof t, "%02X %02X", rv.msb, rv.lsb);
		rev_name = rev.active() ? t : std::string(t) + "（応答が無い）";
		cho.init(ch, tempo, cap + "/fxd");
		std::snprintf(t, sizeof t, "%02X %02X", ch.msb, ch.lsb);
		cho_name = cho.active() ? (cho.approx() ? std::string(t) + "（近似）" : std::string(t)) : std::string(t) + "（この型はまだ）";
		if (threads > 1)
			pool = std::make_unique<motif_sampler::engine::worker_pool>(threads - 1);
		return true;
	}

	// ---- 受けたもの（mtx の中で） ----
	void note_on(int ch, int key, int vel)
	{
		if (vel <= 0) {
			note_off(ch, key);
			return;
		}
		// アルペジオ: スイッチの入ったパートがこのチャンネルにあれば、アルペジオが受ける
		if (arp_channel_has(ch)) {
			if (arp.get().sw && arp.program() && (arp_ch < 0 || arp_ch == ch || arp_idle())) {
				if (arp.key_on(key, vel)) {
					arp_ch = ch;
					if (!arp.passes_through())
						return;
				}
			}
		}
		for (auto &p : parts)
			if (p->ch == ch)
				part_on(*p, key, vel);
	}
	void note_off(int ch, int key)
	{
		if (ch == arp_ch && arp.key_off(key) && !arp.passes_through())
			return;
		for (auto &p : parts)
			if (p->ch == ch)
				part_off(*p, key);
	}
	void control(int ch, int num, int val)
	{
		for (auto &p : parts) {
			if (p->ch != ch)
				continue;
			part &P = *p;
			switch (num) {
			case 1: src(P, 1, val); break;
			case 2: src(P, 7, val); break;
			case 4: src(P, 10, val); src(P, 11, val); src(P, 12, val); break;
			case 16: src(P, 8, val); break;
			case 17: src(P, 9, val); break;
			case 22: src(P, 6, val); break;
			case 7: P.vol = val; break;
			case 11: P.exp = val; break;
			case 10:
				P.pan = val;
				if (P.eg)
					P.eg->set_pan_offset(val - 64);
				break;
			case 91: P.rev = val; break;
			case 93: P.cho = val; break;
			case 64: sustain(P, val >= 64); break;
			case 120:
				if (P.eg)
					P.eg->all_sound_off();
				break;
			case 121:
				for (int s = 0; s < 16; s++)
					src(P, s, motif_sampler::engine::bipolar(s) ? 64 : 0);
				P.exp = 127;
				sustain(P, false);
				break;
			case 123:
				if (P.eg)
					P.eg->all_notes_off();
				break;
			}
		}
	}
	void aftertouch(int ch, int v)
	{
		for (auto &p : parts)
			if (p->ch == ch) {
				src(*p, 2, v);
				src(*p, 3, v);
			}
	}
	void pitch_bend(int ch, int v)
	{
		for (auto &p : parts)
			if (p->ch == ch)
				src(*p, 0, v);
	}
	void all_off()
	{
		arp.all_off();
		arp_ch = -1;
		for (auto &p : parts) {
			p->sustain = false;
			for (bool &s : p->sustained)
				s = false;
			if (p->eg)
				p->eg->all_sound_off();
		}
	}

	// ---- 描く（mtx の中で、n ≤ 256） ----
	void render(int n, float *outl, float *outr, float master, float &peak)
	{
		// アルペジオの出す音（位置つき）。CC・PB はブロックの頭で
		arp_event evs[64];
		int ne = 0;
		arp.process(n, 44100.0, [&](int pos, int type, int a, int b) {
			if (type == motif_arp::OUT_CC || type == motif_arp::OUT_PB) {
				if (arp_ch >= 0) {
					if (type == motif_arp::OUT_CC)
						control(arp_ch, a, b);
					else
						pitch_bend(arp_ch, b);
				}
			} else if (ne < 64)
				evs[ne++] = {pos, type, a, b};
		});
		auto work = [&](size_t i) {
			part &P = *parts[i];
			const bool from_arp = P.arpsw && P.ch == arp_ch;
			render_part(P, n, from_arp ? evs : nullptr, from_arp ? ne : 0);
		};
		if (pool)
			pool->run(parts.size(), work);
		else
			for (size_t i = 0; i < parts.size(); i++)
				work(i);
		for (int j = 0; j < n; j++) {
			double ml = 0, mr = 0, rl = 0, rr = 0, cl = 0, cr = 0;
			for (auto &p : parts) {
				const double a = p->ol[j], b = p->orr[j];
				const double d = p->dry / 127.0, s1 = p->rev / 127.0, s2 = p->cho / 127.0;
				ml += a * d;
				mr += b * d;
				rl += a * s1;
				rr += b * s1;
				cl += a * s2;
				cr += b * s2;
			}
			double xl = 0, xr = 0;
			if (rev.active()) {
				rev.tick(rl, rr, xl, xr);
				if (use_rev)
					ml += xl, mr += xr;
			}
			if (cho.active()) {
				cho.tick(cl, cr, xl, xr);
				if (use_cho)
					ml += xl, mr += xr;
			}
			// 実機の出口は ±1.0 で頭打ち
			const float a = float(std::fmax(-1.0, std::fmin(1.0, ml * master))), b = float(std::fmax(-1.0, std::fmin(1.0, mr * master)));
			peak = std::max(peak, std::max(std::fabs(a), std::fabs(b)));
			outl[j] = a;
			outr[j] = b;
		}
		if (++compact_count >= 16) {
			compact_count = 0;
			for (auto &p : parts)
				if (p->eg) {
					p->eg->compact();
					p->voices = int(p->eg->active_voices());
				}
		}
	}

	int active_voices() const
	{
		int c = 0;
		for (auto &p : parts)
			c += p->voices.load();
		return c;
	}

private:
	std::unique_ptr<motif_sampler::engine::worker_pool> pool;
	int arp_ch = -1;
	int compact_count = 0;

	bool arp_channel_has(int ch) const
	{
		for (auto &p : parts)
			if (p->arpsw && p->ch == ch)
				return true;
		return false;
	}
	bool arp_idle() const { return !arp.running(); }

	static void src(part &P, int s, int v)
	{
		if (P.eg)
			P.eg->set_source(s, v);
	}

	// パートの鍵の範囲・ベロシティの範囲・ノートシフト・ベロシティの効き
	static void part_on(part &P, int key, int vel)
	{
		if (!P.eg)
			return;
		const int lo = P.cfg.get("note_lo", 0), hi = P.cfg.get("note_hi", 127);
		const bool in = lo <= hi ? (key >= lo && key <= hi) : (key <= hi || key >= lo);
		if (!in || vel < P.cfg.get("vel_lo", 1) || vel > P.cfg.get("vel_hi", 127))
			return;
		const int k = key + P.cfg.get("shift", 64) - 64;
		if (k < 0 || k > 127)
			return;
		const int v = int(std::lround(vel * P.cfg.get("vsd", 64) / 64.0 + P.cfg.get("vso", 64) - 64));
		P.sustained[k] = false;
		P.eg->note_on(k, std::min(127, std::max(1, v)));
	}
	static void part_off(part &P, int key)
	{
		if (!P.eg)
			return;
		const int k = key + P.cfg.get("shift", 64) - 64;
		if (k < 0 || k > 127)
			return;
		if (P.sustain) {
			P.sustained[k] = true;
			return;
		}
		P.eg->note_off(k);
	}
	static void sustain(part &P, bool on)
	{
		P.sustain = on;
		if (on || !P.eg)
			return;
		for (int k = 0; k < 128; k++)
			if (P.sustained[k]) {
				P.sustained[k] = false;
				P.eg->note_off(k);
			}
	}

	// パート 1 つを描いて、インサーション・音色の音量・パートの音量（CC7 と同じ 2 乗）まで掛けたものを P.ol・P.orr に
	void render_part(part &P, int n, const arp_event *evs, int ne)
	{
		float l[256] = {}, r[256] = {}, voff[256] = {};
		float g0[256] = {}, g1[256] = {}, g2[256] = {}, h0[256] = {}, h1[256] = {}, h2[256] = {};
		if (!P.eg) {
			std::fill(P.ol, P.ol + n, 0.0f);
			std::fill(P.orr, P.orr + n, 0.0f);
			return;
		}
		int at = 0;
		for (int e = 0; e <= ne; e++) {
			const int upto = e < ne ? std::min(n, std::max(at, evs[e].pos)) : n;
			if (upto > at) {
				float *gl[3] = {g0 + at, g1 + at, g2 + at}, *gr[3] = {h0 + at, h1 + at, h2 + at};
				P.eg->render(upto - at, l + at, r + at, gl, gr, voff + at);
			}
			at = upto;
			if (e < ne) {
				if (evs[e].type == motif_arp::OUT_ON)
					part_on(P, evs[e].a, evs[e].b);
				else if (evs[e].type == motif_arp::OUT_OFF)
					part_off(P, evs[e].a);
			}
		}
		const double pg = P.mute ? 0.0 : std::pow(P.vol / 127.0, 2.0) * std::pow(P.exp / 127.0, 2.0);
		float pk = 0;
		for (int j = 0; j < n; j++) {
			double dl, dr;
			if (P.has_ins && use_ins) {
				const double xl[3] = {g0[j], g1[j], g2[j]}, xr[3] = {h0[j], h1[j], h2[j]};
				P.ins.tick(xl, xr, dl, dr);
			} else {
				dl = l[j];
				dr = r[j];
			}
			const double g = P.eg->volume_gain(voff[j]) * pg;
			P.ol[j] = float(dl * g);
			P.orr[j] = float(dr * g);
			pk = std::max(pk, std::max(std::fabs(P.ol[j]), std::fabs(P.orr[j])));
		}
		P.peak = std::max(pk, P.peak.load() * 0.95f);
	}
};

}  // namespace motif_multi
