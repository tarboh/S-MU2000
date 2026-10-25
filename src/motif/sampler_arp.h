// MOTIF-RACK ES のアルペジオ（2026-10-25）。
// 型ごとのフレーズは、実機のアルペジオの MIDI 出力を録って tools/motif/mr_arp_fit.py が作る表（capture/arp_prog.txt）から読む。
// 表の型（kind=table）は、押さえた音の数 n（1〜6・8）ごとに 1 周ぶんのノートを持ち、音は「押さえた音を低い順に並べた順位とオクターブ」で書いてある
// （実機は押さえた音の高さや転回に依らず、順位で置き換える。数が足りないときの置き換え方は型ごとに違うので、n ごとに録ったものをそのまま使う）。
// 歩く型（kind=walk、Seq の UpOct など）は、周をまたいでも数え続ける声部（上り・下り・往復・乱れ）で作る。
// 表はヤマハのデータから作ったものなので配らない。
#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace motif_arp {

constexpr int PPQ = 480;

// 音の決め方: r = 押さえた音の順位 a からオクターブ b、a = 鍵 a そのまま、s = いちばん低い押さえた音から a 半音
struct note_spec {
	char kind = 'r';
	int a = 0, b = 0;
};

struct pev {
	int tick = 0, gate = 60, vel = 100;
	note_spec s;
};

struct plist {
	std::string name;                    // n1〜n6・n8・d1・cmin・c7・n3v40・n3v127・n3rev
	std::vector<int> keys;
	int vel = 100;
	std::vector<pev> ev;
};

// 歩く声部。kind: 0 上り・1 下り・2 往復（端を 2 度）・3 往復（端を 1 度）・4 道のり・5 乱れ
struct wvoice {
	int kind = 0, lo = 0, O = 1, order = 0, rev = 0, start = 0;
	std::vector<std::array<int, 4>> segs;   // 道のり: オクターブ・端（0 低い / 1 高い）・オクターブ・端
};

struct rhythm_ev {
	int tick = 0, gate = 60, vel = 100;
};

struct ctl_ev {
	int tick = 0, st = 0xB0, a = 0, b = 0;
};

struct type_prog {
	int index = -1, len = 1920;
	std::string cat, name;
	bool walk = false;
	std::vector<wvoice> voices;
	std::vector<rhythm_ev> rhythm;
	std::vector<plist> lists;
	std::vector<ctl_ev> ctl;
	bool chordy = false;                 // 押さえた音のどれでもない音を鳴らす（和音の種類で変わる型）

	const plist *find(const std::string &n) const
	{
		for (auto &l : lists)
			if (l.name == n)
				return &l;
		return nullptr;
	}
};

inline int walk_index(int kind, long s, int m)
{
	if (m <= 0)
		return 0;
	switch (kind) {
	case 0: return int(s % m);
	case 1: return m - 1 - int(s % m);
	case 2: {
		const long q = s % (2L * m);
		return int(q < m ? q : 2L * m - 1 - q);
	}
	default: {
		if (m == 1)
			return 0;
		const long q = s % (2L * m - 2);
		return int(q < m ? q : 2L * m - 2 - q);
	}
	}
}

// 声部 v の s 歩目の鍵（押さえた音 held は鳴らす順に並んだもの）。乱れ（5）は rnd で選ぶ
template <class R>
int walk_note(const wvoice &v, const std::vector<int> &held, long s, R &rnd)
{
	const int n = int(held.size());
	if (!n)
		return -1;
	if (v.kind == 4) {
		std::vector<int> q;
		for (auto &g : v.segs) {
			const int a = g[0] * n + (g[1] ? n - 1 : 0), b = g[2] * n + (g[3] ? n - 1 : 0);
			if (b >= a)
				for (int i = a; i <= b; i++)
					q.push_back(i);
			else
				for (int i = a; i >= b; i--)
					q.push_back(i);
		}
		if (q.empty())
			return -1;
		const int i = q[size_t(s % long(q.size()))];
		const int o = i >= 0 ? i / n : -((-i + n - 1) / n);
		return held[size_t(i - o * n)] + 12 * o;
	}
	std::vector<int> E;
	if (v.order == 0) {
		for (int o = v.lo; o < v.lo + v.O; o++)
			for (int h : held)
				E.push_back(h + 12 * o);
	} else {
		for (int h : held)
			for (int o = v.lo; o < v.lo + v.O; o++)
				E.push_back(h + 12 * o);
	}
	if (v.kind == 5)
		return E[std::uniform_int_distribution<size_t>(0, E.size() - 1)(rnd)];
	const int oc = std::min(std::max(0, v.lo), v.lo + v.O - 1);
	int i0 = v.order == 0 ? (oc - v.lo) * n + (v.start ? n - 1 : 0) : (v.start ? n - 1 : 0) * v.O + (oc - v.lo);
	const int m = int(E.size());
	if (i0 < 0 || i0 >= m)
		return -1;
	if (v.rev) {
		std::reverse(E.begin(), E.end());
		i0 = m - 1 - i0;
	}
	for (int k = 0; k < 2 * m; k++)
		if (walk_index(v.kind, k, m) == i0)
			return E[size_t(walk_index(v.kind, k + s, m))];
	return -1;
}

class library {
public:
	std::vector<type_prog> types;        // 型の番号の順（0〜1786）。録っていない型は index が −1

	bool load(const std::string &path)
	{
		FILE *f = std::fopen(path.c_str(), "rb");
		if (!f)
			return false;
		types.clear();
		char line[4096];
		type_prog *cur = nullptr;
		plist *cl = nullptr;
		while (std::fgets(line, sizeof line, f)) {
			std::istringstream ss(line);
			std::string tag;
			ss >> tag;
			if (tag == "type") {
				int idx = 0;
				std::string cat, name, tok;
				ss >> idx >> cat >> name;
				if (idx < 0 || idx > 4096)
					continue;
				if (int(types.size()) <= idx)
					types.resize(size_t(idx) + 1);
				cur = &types[size_t(idx)];
				*cur = type_prog();
				cur->index = idx;
				cur->cat = cat;
				cur->name = name;
				cl = nullptr;
				while (ss >> tok) {
					const size_t e = tok.find('=');
					if (e == std::string::npos)
						continue;
					const std::string k = tok.substr(0, e), v = tok.substr(e + 1);
					if (k == "len")
						cur->len = std::max(1, std::atoi(v.c_str()));
					else if (k == "kind")
						cur->walk = v == "walk";
					else if (k == "voices")
						parse_voices(v, cur->voices);
				}
			} else if (tag == "list" && cur) {
				cur->lists.emplace_back();
				cl = &cur->lists.back();
				ss >> cl->name;
				std::string tok;
				while (ss >> tok) {
					if (!tok.compare(0, 5, "keys=")) {
						std::istringstream ks(tok.substr(5));
						std::string k;
						while (std::getline(ks, k, ','))
							cl->keys.push_back(std::atoi(k.c_str()));
					} else if (!tok.compare(0, 4, "vel="))
						cl->vel = std::atoi(tok.c_str() + 4);
				}
			} else if (tag == "e" && cur) {
				int t = 0, g = 60, v = 100;
				ss >> t >> g >> v;
				std::string sp;
				if (cur->walk || !(ss >> sp)) {
					cur->rhythm.push_back({t, g, v});
					continue;
				}
				if (!cl)
					continue;
				pev e;
				e.tick = t;
				e.gate = g;
				e.vel = v;
				e.s.kind = sp[0];
				if (sp[0] == 'r')
					std::sscanf(sp.c_str() + 1, "%d:%d", &e.s.a, &e.s.b);
				else
					e.s.a = std::atoi(sp.c_str() + 1);
				if (sp[0] == 's')
					cur->chordy = true;
				cl->ev.push_back(e);
			} else if (tag == "c" && cur) {
				ctl_ev c;
				ss >> c.tick >> c.st >> c.a >> c.b;
				cur->ctl.push_back(c);
			}
		}
		std::fclose(f);
		for (auto &t : types) {
			for (auto &l : t.lists)
				std::stable_sort(l.ev.begin(), l.ev.end(), [](const pev &a, const pev &b) { return a.tick < b.tick; });
			std::stable_sort(t.ctl.begin(), t.ctl.end(), [](const ctl_ev &a, const ctl_ev &b) { return a.tick < b.tick; });
		}
		return !types.empty();
	}

	const type_prog *get(int t) const
	{
		if (t < 0 || t >= int(types.size()) || types[size_t(t)].index < 0)
			return nullptr;
		return &types[size_t(t)];
	}

private:
	static void parse_voices(const std::string &s, std::vector<wvoice> &out)
	{
		std::istringstream vs(s);
		std::string one;
		while (std::getline(vs, one, ';')) {
			std::vector<std::string> f;
			std::istringstream fs(one);
			std::string x;
			while (std::getline(fs, x, ','))
				f.push_back(x);
			if (f.empty())
				continue;
			wvoice v;
			static const char *names[] = {"up", "down", "updn_a", "updn_b", "path", "rand"};
			for (int k = 0; k < 6; k++)
				if (f[0] == names[k])
					v.kind = k;
			if (v.kind == 4) {
				// 「0b0t」のような区間（オクターブ・端・オクターブ・端）
				for (size_t i = 1; i < f.size(); i++) {
					int o1 = 0, o2 = 0;
					char e1 = 'b', e2 = 'b';
					if (std::sscanf(f[i].c_str(), "%d%c%d%c", &o1, &e1, &o2, &e2) == 4)
						v.segs.push_back({o1, e1 == 't', o2, e2 == 't'});
				}
			} else if (v.kind == 5) {
				if (f.size() >= 3) {
					v.lo = std::atoi(f[1].c_str());
					v.O = std::atoi(f[2].c_str());
				}
			} else if (f.size() >= 6) {
				v.lo = std::atoi(f[1].c_str());
				v.O = std::atoi(f[2].c_str());
				v.order = std::atoi(f[3].c_str());
				v.rev = std::atoi(f[4].c_str());
				v.start = std::atoi(f[5].c_str());
			}
			out.push_back(v);
		}
	}
};

// 音色のアルペジオの設定（パッチの arp 行。ダンプの 40 70 00 の 0x14〜）
struct settings {
	int type = 0;
	bool sw = false;
	int hold = 1;                        // 0 sync-off・1 off・2 on
	int unit = 3;                        // 0〜6: 50%・66%・75%・100%・133%・150%・200%
	int note_lo = 0, note_hi = 127, vel_lo = 1, vel_hi = 127;
	int key = 0;                         // 0 sort・1 thru・2 direct・3 sort+direct・4 thru+direct
	int vmode = 0;                       // 0 original・1 thru
	int qv = 2, qs = 0;                  // クオンタイズの値（0〜6: 32 分〜4 分）と強さ（%）
	int vrate = 100, grate = 100, swing = 0;
	int tempo = 120;
	int sf[5] = {0, 0, 0, 0, 0};
};

inline double unit_factor(int u)
{
	static const double f[7] = {0.5, 2.0 / 3.0, 0.75, 1.0, 4.0 / 3.0, 1.5, 2.0};
	return f[std::min(6, std::max(0, u))];
}

inline int quantize_ticks(int qv)
{
	static const int q[7] = {60, 80, 120, 160, 240, 320, 480};
	return q[std::min(6, std::max(0, qv))];
}

// 出力の種類
enum { OUT_OFF = 0, OUT_ON = 1, OUT_CC = 2, OUT_PB = 3 };

class arpeggiator {
public:
	void set_library(const library *l) { lib = l; }

	void set(const settings &s)
	{
		const bool type_changed = !prog || s.type != st.type;
		st = s;
		prog = lib ? lib->get(st.type) : nullptr;
		if (type_changed)
			scan = long(clock);          // 型を替えたら、今の位置から新しい型で続ける
	}
	const settings &get() const { return st; }
	const type_prog *program() const { return prog; }
	bool running() const { return run; }

	// 鍵を受ける。アルペジオが受けたら true（鳴らすのはアルペジオ）。範囲の外・スイッチ off なら false（そのまま鳴らす）
	bool key_on(int key, int vel)
	{
		if (!st.sw || !prog)
			return false;
		if (key < st.note_lo || key > st.note_hi || vel < st.vel_lo || vel > st.vel_hi)
			return false;
		// ホールド on: 鍵を全部離したあとに押したら、押さえた音を入れ替える
		if (st.hold == 2 && physical == 0)
			held.clear();
		physical++;
		held.erase(std::remove_if(held.begin(), held.end(), [&](const hkey &h) { return h.key == key; }), held.end());
		held.push_back({key, vel, order++});
		last_vel = vel;
		step = 0;                            // 押さえた音が増えると、歩く型は数え直す（実機で、遅れて届いた鍵で頭から歩き直した）
		if (!run) {
			run = true;
			if (!(st.hold == 0 && started)) {
				clock = 0;
				scan = 0;
			}
			started = true;
		}
		return !direct_only();
	}

	bool key_off(int key)
	{
		if (!st.sw || !prog)
			return false;
		auto it = std::find_if(held.begin(), held.end(), [&](const hkey &h) { return h.key == key; });
		if (it == held.end())
			return false;
		physical = std::max(0, physical - 1);
		if (st.hold != 2) {
			held.erase(it);
			if (held.empty())
				stop_notes = true;           // 次の process で鳴っている音を止める
		}
		return !direct_only();
	}

	// direct: アルペジオはノートを鳴らさず（CC・PB だけ）、押さえた音はそのまま鳴らす
	bool direct_only() const { return st.key == 2; }
	bool passes_through() const { return st.key >= 2; }

	void all_off()
	{
		held.clear();
		physical = 0;
		stop_notes = true;
		run = false;
		started = false;
	}

	// frames サンプルぶん進め、出す音を out(サンプルの位置, 種類, a, b) で返す
	template <class F>
	void process(int frames, double rate, F &&out)
	{
		if (stop_notes) {
			for (auto &s : sounding)
				out(0, OUT_OFF, s.key, 0);
			sounding.clear();
			stop_notes = false;
			if (held.empty() && st.hold != 0)
				run = false;
		}
		if (!run || !prog || frames <= 0) {
			if (run && st.hold == 0)
				clock += frames * ticks_per_sample(rate);
			return;
		}
		const double tps = ticks_per_sample(rate);
		const double end = clock + frames * tps;
		auto at = [&](double t) { return std::min(frames - 1, std::max(0, int((t - clock) / tps))); };
		const bool sound = !held.empty();
		for (;;) {
			// 次のフレーズの出来事（scan 以降の最初のもの）
			long next = -1;
			if (sound)
				next = next_event_tick();
			double off_t = 1e300;
			for (auto &s : sounding)
				off_t = std::min(off_t, s.end);
			if (off_t < end && (next < 0 || off_t <= double(next))) {
				for (size_t i = 0; i < sounding.size();)
					if (sounding[i].end <= off_t) {
						out(at(off_t), OUT_OFF, sounding[i].key, 0);
						sounding.erase(sounding.begin() + long(i));
					} else
						i++;
				continue;
			}
			if (next < 0 || double(next) >= end)
				break;
			fire(next, at(double(next)), out);
			scan = next + 1;
		}
		clock = end;
		if (!sound)
			scan = long(clock);
	}

private:
	struct hkey {
		int key, vel;
		long order;
	};
	struct sound_note {
		int key;
		double end;
	};
	const library *lib = nullptr;
	const type_prog *prog = nullptr;
	settings st;
	std::vector<hkey> held;
	std::vector<sound_note> sounding;
	long order = 0, scan = 0, step = 0;
	double clock = 0;
	int physical = 0, last_vel = 100;
	bool run = false, started = false, stop_notes = false;
	std::mt19937 rnd{12345};

	double ticks_per_sample(double rate) const
	{
		return st.tempo / 60.0 * PPQ / rate / unit_factor(st.unit);
	}

	// 鳴らす順に並べた押さえた音（sort は低い順、thru は押した順）
	std::vector<int> ordered() const
	{
		std::vector<hkey> h = held;
		if (st.key == 1 || st.key == 4)
			std::sort(h.begin(), h.end(), [](const hkey &a, const hkey &b) { return a.order < b.order; });
		else
			std::sort(h.begin(), h.end(), [](const hkey &a, const hkey &b) { return a.key < b.key; });
		std::vector<int> k;
		for (auto &x : h)
			k.push_back(x.key);
		return k;
	}

	// 押さえた音に合う表
	const plist *pick_list(const std::vector<int> &keys) const
	{
		const int n = int(keys.size());
		if (prog->chordy && n >= 3) {
			std::vector<int> s = keys;
			std::sort(s.begin(), s.end());
			std::vector<int> iv;
			for (int k : s)
				iv.push_back((k - s[0]) % 12);
			std::sort(iv.begin(), iv.end());
			iv.erase(std::unique(iv.begin(), iv.end()), iv.end());
			if (iv == std::vector<int>{0, 3, 7})
				if (auto l = prog->find("cmin"))
					return l;
			if (iv == std::vector<int>{0, 4, 7, 10})
				if (auto l = prog->find("c7"))
					return l;
		}
		for (int k = std::min(n, 8); k >= 1; k--) {
			char nm[8];
			std::snprintf(nm, sizeof nm, "n%d", k == 7 ? 8 : k);
			if (auto l = prog->find(nm))
				return l;
			if (k == 7)
				if (auto l = prog->find("n6"))
					return l;
		}
		return prog->lists.empty() ? nullptr : &prog->lists[0];
	}

	int adjust_tick(int t) const
	{
		const int g = quantize_ticks(st.qv);
		const long idx = std::lround(double(t) / g);
		if (st.qs > 0)
			t += int(std::lround((idx * g - t) * st.qs / 100.0));
		if (st.swing && (idx & 1))
			t += st.swing;
		return std::max(0, t);
	}

	// scan 以降の最初の出来事の位置（周をまたいだ通しの拍の位置）
	long next_event_tick() const
	{
		const long len = prog->len;
		long best = -1;
		auto consider = [&](int tick) {
			const long cyc = scan / len;
			for (long c = cyc; c <= cyc + 1; c++) {
				const long t = c * len + adjust_tick(tick);
				if (t >= scan && (best < 0 || t < best))
					best = t;
			}
		};
		if (prog->walk) {
			for (auto &r : prog->rhythm)
				consider(r.tick);
		} else if (const plist *l = pick_list(ordered())) {
			for (auto &e : l->ev)
				consider(e.tick);
		}
		for (auto &c : prog->ctl)
			consider(c.tick);
		return best;
	}

	template <class F>
	void play(int pos, double t, int key, int vel, int gate, F &out)
	{
		if (key < 0 || key > 127 || direct_only())
			return;
		if (st.vmode == 1)
			vel = last_vel;
		vel = std::min(127, std::max(1, int(std::lround(vel * st.vrate / 100.0))));
		gate = std::max(1, int(std::lround(gate * st.grate / 100.0)));
		// 同じ鍵が鳴っていたら、先に止める
		for (size_t i = 0; i < sounding.size(); i++)
			if (sounding[i].key == key) {
				out(pos, OUT_OFF, key, 0);
				sounding.erase(sounding.begin() + long(i));
				break;
			}
		out(pos, OUT_ON, key, vel);
		sounding.push_back({key, t + gate});
	}

	template <class F>
	void fire(long t, int pos, F &out)
	{
		const long len = prog->len;
		const int local = int(t % len);
		const std::vector<int> keys = ordered();
		for (auto &c : prog->ctl)
			if (adjust_tick(c.tick) == local)
				out(pos, c.st == 0xE0 ? OUT_PB : OUT_CC, c.a, c.b);
		if (keys.empty())
			return;
		if (prog->walk) {
			for (auto &r : prog->rhythm)
				if (adjust_tick(r.tick) % len == local) {
					for (auto &v : prog->voices)
						play(pos, double(t), walk_note(v, keys, step, rnd), r.vel, r.gate, out);
					step++;
				}
			return;
		}
		const plist *l = pick_list(keys);
		if (!l)
			return;
		const int n = int(keys.size());
		for (auto &e : l->ev) {
			if (adjust_tick(e.tick) % len != local)
				continue;
			int key = -1;
			if (e.s.kind == 'r') {
				if (e.s.a >= n)
					continue;                // 押さえた数より上の順位（8 音の表を 7 音で使ったとき）
				key = keys[size_t(e.s.a)] + 12 * e.s.b;
			} else if (e.s.kind == 'a')
				key = e.s.a;
			else
				key = *std::min_element(keys.begin(), keys.end()) + e.s.a;
			play(pos, double(t), key, e.vel, e.gate, out);
		}
	}
};

// パッチの arp 行から
template <class KV>
settings settings_from(const KV &m)
{
	auto g = [&](const char *k, int d) {
		auto it = m.find(k);
		return it == m.end() ? d : std::atoi(it->second.c_str());
	};
	settings s;
	s.type = g("type", 0);
	s.sw = g("sw", 0) != 0;
	s.hold = g("hold", 1);
	s.unit = g("unit", 3);
	s.key = g("key", 0);
	s.vmode = g("vmode", 0);
	s.qv = g("qv", 2);
	s.qs = g("qs", 0);
	s.vrate = g("vrate", 100);
	s.grate = g("grate", 100);
	s.swing = g("swing", 0);
	s.tempo = std::max(1, g("tempo", 120));
	auto pair = [&](const char *k, int &a, int &b) {
		auto it = m.find(k);
		if (it != m.end())
			std::sscanf(it->second.c_str(), "%d,%d", &a, &b);
	};
	pair("note", s.note_lo, s.note_hi);
	pair("vel", s.vel_lo, s.vel_hi);
	auto it = m.find("sf");
	if (it != m.end())
		std::sscanf(it->second.c_str(), "%d,%d,%d,%d,%d", &s.sf[0], &s.sf[1], &s.sf[2], &s.sf[3], &s.sf[4]);
	return s;
}

}  // namespace motif_arp
