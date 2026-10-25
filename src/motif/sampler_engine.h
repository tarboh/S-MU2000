// license:BSD-3-Clause
// サンプリング方式の芯（sampler.h）を、パッチ（tools/motif/mr_patch.py が作る文字ファイル）から鳴らす部品（2026-10-25）。
// sampler_render（オフライン）と motif_play（GUI でその場で弾く）が使う。
//
//   motif_sampler::patch pt;  pt.load("音色.patch");
//   motif_sampler::engine eg(pt);  eg.preload();
//   eg.note_on(60, 100);  eg.render(n, l, r);  eg.note_off(60);
//
// 出来事（ノートオン・オフ・コントロールセットのソースの値）は、今の位置（pos サンプル目）で効く。render は l・r に足す（音色の共通の音量は
// 掛けない。volume_gain で掛ける）。インサーションの組ごとの音（gl・gr）と、コントロールセットの Volume のずれ（voff）も要るなら渡す。
// モノ・同じ鍵・ポルタメント・発音数の奪い合いは tools/motif/mr_multi.py で測った形（doc/motif-rack-es-sampler.md）。
#pragma once

#include "sampler.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace motif_sampler {

using kv = std::map<std::string, std::string>;

inline kv parse(const std::string &line)
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

inline int geti(const kv &m, const char *k, int d = 0)
{
	auto it = m.find(k);
	return it == m.end() ? d : std::atoi(it->second.c_str());
}

inline std::vector<int> list(const kv &m, const char *k)
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

inline bool load_smpx(const std::string &path, sample &s)
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
	float peak = 0, diff = 0;
	for (int i = 0; i < n; i++) {
		s.L[i] = buf[2 * i];
		s.R[i] = buf[2 * i + 1];
		peak = std::max(peak, std::fabs(s.L[i]));
		diff = std::max(diff, std::fabs(s.L[i] - s.R[i]));
	}
	s.stereo = diff > peak * 1e-4f;
	return true;
}

struct zone { int key_lo, key_hi, vel_lo, vel_hi; std::string file; };
struct element { kv f; std::vector<zone> zones; };


// エレメントの Key On Delay（0〜127）→ ms。正弦波の雛形で、値ごとに 3 回ずつ録った音の頭の遅れ（tools/motif/mr_keydelay.py）。間はまっすぐに補間
static inline double key_on_delay_ms(int v)
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
static inline double key_on_delay_beats(int v)
{
	static const double b[20] = {0.235, 0.242, 0.234, 0.230, 0.236, 0.229, 0.327, 0.351, 0.504, 0.651, 0.723, 0.992, 1.319, 1.500, 1.987, 2.660, 2.992, 3.974, 5.001, 5.994};
	if (v < 0)
		return b[0];
	return v < 20 ? b[v] : b[19] + (v - 19);
}

// パッチ: fx・voice・clfo・ctrl の行と、エレメントごとの欄と区間。dir はサンプル（.smpx）の置き場
struct patch {
	kv fxline, voiceline, clfoline, ctrlline, arpline;
	std::vector<element> els;
	std::string dir, name;
	bool load(const std::string &path)
	{
		*this = patch();
		dir = path.substr(0, path.find_last_of("/\\") + 1);
		FILE *pf = std::fopen(path.c_str(), "r");
		if (!pf)
			return false;
		char line[4096];
		while (std::fgets(line, sizeof line, pf)) {
			if (line[0] == '#') {
				name = line + 1;
				while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
					name.pop_back();
				while (!name.empty() && name.front() == ' ')
					name.erase(name.begin());
			} else if (!std::strncmp(line, "fx ", 3))
				fxline = parse(line);
			else if (!std::strncmp(line, "voice ", 6))
				voiceline = parse(line);
			else if (!std::strncmp(line, "clfo ", 5))
				clfoline = parse(line);
			else if (!std::strncmp(line, "ctrl ", 5))
				ctrlline = parse(line);
			else if (!std::strncmp(line, "arp ", 4))
				arpline = parse(line);
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
		return true;
	}
};

struct engine_options {
	int note_index = 0;      // 音色を選んでから何音目から始めるか（オルタネートパンの向き）
	long seed = -1;          // ランダムパン・ランダムピッチの種（負なら振らない）
	int pan_offset = 0;      // マルチのパートのパン − 64（エレメントのパンに足す）
	bool fixed_pan = false;  // マルチのパートの Voice Pan off: エレメントのパンを使わず中央にする
};

// 区間のサンプルは、同じファイルなら音色（エンジン）をまたいで 1 つを使い回す（マルチで同じ波形のパートが並んでも、メモリを食わない）
inline std::shared_ptr<sample> shared_sample(const std::string &path)
{
	static std::mutex m;
	static std::map<std::string, std::weak_ptr<sample>> reg;
	std::lock_guard<std::mutex> lk(m);
	auto it = reg.find(path);
	if (it != reg.end())
		if (auto sp = it->second.lock())
			return sp;
	auto smp = std::make_shared<sample>();
	if (!load_smpx(path, *smp))
		return nullptr;
	reg[path] = smp;
	return smp;
}

class engine {
public:
	using options = engine_options;

	explicit engine(const patch &p, options o = options()) : pt(p), rng(o.seed < 0 ? 0u : unsigned(o.seed)), seed(o.seed), nidx(o.note_index), pan_off(o.pan_offset), fixed_pan(o.fixed_pan)
	{
		std::istringstream ss(pt.ctrlline.count("sets") ? pt.ctrlline.at("sets") : "");
		std::string t;
		while (std::getline(ss, t, ',')) {
			int a = 0, b = 0, c = 64;
			if (std::sscanf(t.c_str(), "%d:%d:%d", &a, &b, &c) == 3)
				sets.push_back({ a, b, c });
		}
		for (int k = 0; k < 16; k++)
			srcval[k] = bipolar(k) ? 64 : 0;
		mono = geti(pt.voiceline, "mono", 1) == 0;
		for (const element &e : pt.els)
			if (geti(e.f, "dnote", -1) >= 0)
				is_drum = true;
		same_single = geti(pt.voiceline, "same", 1) == 0;
		porta = geti(pt.voiceline, "porta", 0) != 0;
		fulltime = geti(pt.voiceline, "pmode", 0) != 0;
		ptime = geti(pt.voiceline, "ptime", 0);
		ptmode = geti(pt.voiceline, "ptmode", 0);
		apply_ctl();
	}

	// 区間のサンプルを全部先に読む（GUI の音の糸の中でディスクを読まないように）
	void preload()
	{
		for (const element &e : pt.els)
			for (const zone &z : e.zones)
				get_sample(z.file);
	}

	static bool bipolar(int src) { return src == 0 || src == 6 || src == 8 || src == 9; }

	// コントロールセットのソース（0 PB・1 MW・2/3 AT・6 RB・7 BC・8 AS1・9 AS2・10〜12 FC）を値にする
	void set_source(int src, int val)
	{
		srcval[std::max(0, std::min(15, src))] = std::max(0, std::min(127, val));
		apply_ctl();
	}
	int source(int src) const { return srcval[std::max(0, std::min(15, src))]; }

	// 共通の音量（0〜127）は 40·log10(値 / 127) dB（コントロールセットの Volume のずれ voff を足してから）
	double volume_gain(double voff = 0) const
	{
		return std::pow(std::max(0.0, std::min(127.0, geti(pt.voiceline, "volume", 127) + voff)) / 127.0, 2.0);
	}
	double volume_offset() const { return vol_off; }
	// マルチのパートのパン（− 64）。次に鳴らす音から効く
	void set_pan_offset(int o) { pan_off = o; }

	void note_on(int key, int vel)
	{
		const int i = int(pos);
		if (mono) {
			// モノ: 鳴っている音を止める。ポルタメントがあって前の鍵を押さえたままなら、EG はやり直さずに音程だけ滑らせる（レガート）
			int cur = -1;
			for (size_t k = 0; k < notes.size(); k++)
				if (notes[k].alive && notes[k].held)
					cur = int(k);
			held_stack.push_back(key);
			if (cur >= 0 && porta) {
				const double c = (key - notes[cur].key) * 100.0;
				voices_of(cur, [&](voice &v) { v.glide = c - glide_from(c - v.glide); v.glide_to = c; v.glide_rate = glide_rate(c - v.glide); });
				last_key = key;
				return;
			}
			for (size_t k = 0; k < notes.size(); k++)
				if (notes[k].alive)
					kill_note(int(k));
			start_note(key, vel, i);
			return;
		}
		if (same_single)
			for (size_t k = 0; k < notes.size(); k++)
				if (notes[k].alive && notes[k].key == key)
					kill_note(int(k));
		if (is_drum) {
			// ドラム: 同じオルタネートグループのほかの鍵を止める（ハイハットの開きと閉じ）。Assign Mode が single の鍵は、同じ鍵の前の音を止める
			const element *de = drum_element(key);
			if (de) {
				const int ag = geti(de->f, "alt");
				for (size_t k = 0; k < notes.size(); k++) {
					if (!notes[k].alive)
						continue;
					const element *oe = notes[k].key == key ? de : drum_element(notes[k].key);
					if ((notes[k].key == key && geti(de->f, "assign", 1) == 0) || (ag > 0 && notes[k].key != key && oe && geti(oe->f, "alt") == ag))
						kill_note(int(k));
				}
			}
		}
		make_room(count_elements(key, vel));
		start_note(key, vel, i);
	}

	void note_off(int key)
	{
		const int i = int(pos);
		if (mono) {
			// モノ: 離した鍵が今の音なら、押さえたままの前の鍵へ戻る（ポルタメントがあれば滑って、無ければやり直して）
			auto it = std::find(held_stack.rbegin(), held_stack.rend(), key);
			if (it == held_stack.rend())
				return;
			const bool top = it == held_stack.rbegin();
			held_stack.erase(std::next(it).base());
			if (!top)
				return;
			int cur = -1;
			for (size_t k = 0; k < notes.size(); k++)
				if (notes[k].alive && notes[k].held)
					cur = int(k);
			if (cur < 0)
				return;
			if (held_stack.empty()) {
				release_note(cur);
				return;
			}
			const int back = held_stack.back();
			if (porta) {
				const double c = (back - notes[cur].key) * 100.0;
				voices_of(cur, [&](voice &v) { v.glide = c - glide_from(c - v.glide); v.glide_to = c; v.glide_rate = glide_rate(c - v.glide); });
				last_key = back;
			} else {
				kill_note(cur);
				start_note(back, notes[cur].vel, i);
			}
			return;
		}
		// 同じ鍵が重なっていれば、古い音から離す
		for (size_t k = 0; k < notes.size(); k++)
			if (notes[k].alive && notes[k].held && notes[k].key == key) {
				release_note(int(k));
				break;
			}
	}

	// 押さえている音を全部離す
	void all_notes_off()
	{
		held_stack.clear();
		for (size_t k = 0; k < notes.size(); k++)
			if (notes[k].alive && notes[k].held)
				release_note(int(k));
	}
	// 鳴っている音を全部すぐ止める
	void all_sound_off()
	{
		held_stack.clear();
		for (size_t k = 0; k < notes.size(); k++)
			if (notes[k].alive)
				kill_note(int(k));
	}

	// エレメントごとに n サンプル描いてから、サンプルごとにエレメントの順に足す（足す順は 1 サンプルずつ全部のエレメントを回すのと同じなので、
	// 結果も同じ）。エレメントを描く所は互いに関わらないので、set_threads で糸を増やすと分けて描く
	// sends を渡すと、エレメントごとの送り（rsend・csend の欄。ドラムの鍵）を掛けて足したものを、リバーブ（sends[0]・[1]）とコーラス（[2]・[3]）の左右に足す。
	// 送るのはインサーションを通らないエレメント（ins=0）だけ（インサーションを通るものは、インサーションのあとで Ins-EF の送りを掛ける）
	void render(int n, float *l, float *r, float *const *gl = nullptr, float *const *gr = nullptr, float *voff = nullptr, float *const *sends = nullptr)
	{
		// エレメントごとの描きだめがキャッシュに乗るように、1024 サンプルずつに区切る（オフラインは次の出来事まで一度に頼むので長い）
		for (int off = 0; off < n; off += 1024) {
			const int m = std::min(1024, n - off);
			float *g2[3], *r2[3], *s2[4];
			for (int k = 0; k < 3; k++) {
				g2[k] = gl ? gl[k] + off : nullptr;
				r2[k] = gr ? gr[k] + off : nullptr;
			}
			for (int k = 0; k < 4; k++)
				s2[k] = sends ? sends[k] + off : nullptr;
			render_chunk(m, l + off, r + off, gl ? g2 : nullptr, gr ? r2 : nullptr, voff ? voff + off : nullptr, sends ? s2 : nullptr);
		}
	}

	// ドラム（エレメントに dnote の欄がある）か
	bool drum() const { return is_drum; }

private:
	void render_chunk(int n, float *l, float *r, float *const *gl, float *const *gr, float *voff, float *const *sends)
	{
		const size_t nv = voices.size();
		if (va.size() < nv) {
			va.resize(nv);
			vb.resize(nv);
			vs.resize(nv);
		}
		for (size_t vi = 0; vi < nv; vi++) {
			va[vi].resize(n);
			vb[vi].resize(n);
			vs[vi].resize(n);
		}
		block_n = n;
		if (pool && nv > 1)
			pool->run(nv, [this](size_t vi) { render_voice(vi); });
		else
			for (size_t vi = 0; vi < nv; vi++)
				render_voice(vi);
		if (voff)
			for (int j = 0; j < n; j++)
				voff[j] = float(vol_off);
		// 外側をエレメントにしても、どのサンプルでもエレメントの順に足すのは同じ
		for (size_t vi = 0; vi < nv; vi++) {
			const float *a = va[vi].data(), *b = vb[vi].data();
			const char *on = vs[vi].data();
			float *g0 = gl ? gl[vgroup[vi]] : nullptr, *h0 = gr ? gr[vgroup[vi]] : nullptr;
			for (int j = 0; j < n; j++) {
				if (!on[j])
					continue;
				if (g0)
					g0[j] += a[j];
				if (h0)
					h0[j] += b[j];
				l[j] += a[j];
				r[j] += b[j];
			}
			if (sends && vgroup[vi] == 0 && (vrs[vi] > 0 || vcs[vi] > 0)) {
				const float rs = vrs[vi], cs = vcs[vi];
				for (int j = 0; j < n; j++) {
					if (!on[j])
						continue;
					sends[0][j] += a[j] * rs;
					sends[1][j] += b[j] * rs;
					sends[2][j] += a[j] * cs;
					sends[3][j] += b[j] * cs;
				}
			}
		}
		pos += n;
	}

public:
	// エレメントを描く糸の数（1 なら今の糸だけ）
	void set_threads(int n)
	{
		pool.reset();
		if (n > 1)
			pool = std::make_unique<worker_pool>(n - 1);
	}

	// 鳴り終わったエレメントと音を片付ける（GUI で長く弾き続けるとき。オフラインの描き出しでは呼ばなくてよい）
	void compact()
	{
		std::vector<int> keep_note(notes.size(), 0);
		for (size_t vi = 0; vi < voices.size(); vi++)
			if (!voices[vi].done || int(pos) < vdelay[vi])
				keep_note[vnote[vi]] = 1;
		for (size_t k = 0; k < notes.size(); k++)
			if (notes[k].alive && notes[k].held)
				keep_note[k] = 1;
		std::vector<int> remap(notes.size(), -1);
		std::vector<note_state> nn;
		for (size_t k = 0; k < notes.size(); k++)
			if (keep_note[k]) {
				remap[k] = int(nn.size());
				nn.push_back(notes[k]);
			}
		size_t w = 0;
		for (size_t vi = 0; vi < voices.size(); vi++) {
			if (voices[vi].done && int(pos) >= vdelay[vi])
				continue;
			if (w != vi) {
				voices[w] = std::move(voices[vi]);
				vgroup[w] = vgroup[vi];
				vdelay[w] = vdelay[vi];
				vcbox[w] = vcbox[vi];
				vnoff[w] = vnoff[vi];
				vrs[w] = vrs[vi];
				vcs[w] = vcs[vi];
			}
			vnote[w] = remap[vnote[vi]];
			w++;
		}
		voices.resize(w);
		vgroup.resize(w);
		vdelay.resize(w);
		vcbox.resize(w);
		vnoff.resize(w);
		vrs.resize(w);
		vcs.resize(w);
		vnote.resize(w);
		notes.swap(nn);
	}

	size_t active_voices() const
	{
		size_t c = 0;
		for (const voice &v : voices)
			if (!v.done)
				c++;
		return c;
	}
	size_t elements_started() const { return started; }

	int64_t pos = 0;                         // 描いたサンプルの数（出来事はこの位置で効く）

private:
	struct cset { int src, dest, depth; };
	struct note_state { int key, vel, t0; bool held, alive; };

	const patch &pt;
	std::mt19937 rng;
	long seed;
	int nidx;
	int pan_off;
	bool fixed_pan;
	std::vector<cset> sets;
	int srcval[16];
	double vol_off = 0;                      // 行き先 1（Volume）: 音色の音量の値のずれ（エレメントのボックスによらない）
	std::map<std::string, std::shared_ptr<sample>> cache;
	std::vector<voice> voices;
	std::vector<int> vgroup;                 // エレメントのインサーションのスイッチ（0・1・2）
	std::vector<int> vdelay;                 // エレメントが鳴り始めるサンプル（Key On Delay を足した）
	std::vector<int> vcbox;                  // エレメントのコントロールボックス（ビット 0〜5 = 組 1〜6）
	std::vector<char> vnoff;                 // ノートオフを受けるか（ドラムの鍵の Receive Note Off。ふつうのエレメントは受ける）
	std::vector<float> vrs, vcs;             // エレメントのリバーブ・コーラスの送り（ドラムの鍵。0 なら送らない）
	bool is_drum = false;
	std::vector<int> vnote;                  // エレメントがどの音（notes の番号）のものか
	std::vector<note_state> notes;
	bool mono = false, same_single = false, porta = false, fulltime = false;
	int ptime = 0, ptmode = 0;
	int last_key = -1;                       // いちばん最後に鳴らした鍵（ポルタメントの出どころ）
	std::vector<int> held_stack;             // モノで押さえている鍵の並び（後が新しい）
	size_t started = 0;
	std::vector<std::vector<float>> va, vb;      // エレメントごとに描いた左右（render の中だけ）
	std::vector<std::vector<char>> vs;           // そのサンプルで鳴らしたか（Key On Delay の前は足さない）
	int block_n = 0;

	// エレメント vi を block_n サンプル描く
	void render_voice(size_t vi)
	{
		voice &v = voices[vi];
		float *a = va[vi].data(), *b = vb[vi].data();
		char *on = vs[vi].data();
		for (int j = 0; j < block_n; j++) {
			const int i = int(pos) + j;
			// Key On Delay: 遅れが過ぎるまで鳴らさない。遅れより先に鍵を離したエレメントは鳴らない（実機で確かめた）
			if (i < vdelay[vi]) {
				on[j] = 0;
				continue;
			}
			if (i == vdelay[vi] && !notes[vnote[vi]].held && vdelay[vi] > notes[vnote[vi]].t0)
				v.done = true;
			// 手元の変数で受けてから書く（配列の要素を直接渡すと、エレメントの中身と重なるかもしれないとみなされて 3 割遅くなった）
			float x = 0, y = 0;
			v.render(x, y);
			a[j] = x;
			b[j] = y;
			on[j] = 1;
		}
	}

public:
	// 決まった数の糸で、0〜count − 1 の仕事を分けてやる（呼んだ糸も手伝う。マルチのパートを分けて描くのにも使う）
	class worker_pool {
	public:
		explicit worker_pool(int n)
		{
			for (int k = 0; k < n; k++)
				th.emplace_back([this] { loop(); });
		}
		~worker_pool()
		{
			{
				std::lock_guard<std::mutex> lk(m);
				quit = true;
				gen++;
			}
			cv.notify_all();
			for (auto &t : th)
				t.join();
		}
		template <class F>
		void run(size_t count, F f)
		{
			{
				std::lock_guard<std::mutex> lk(m);
				job = f;
				total = count;
				next = 0;
				busy = int(th.size());
				gen++;
			}
			cv.notify_all();
			work();
			std::unique_lock<std::mutex> lk(m);
			done_cv.wait(lk, [this] { return busy == 0; });
		}

	private:
		std::vector<std::thread> th;
		std::mutex m;
		std::condition_variable cv, done_cv;
		std::function<void(size_t)> job;
		std::atomic<size_t> next{0};
		size_t total = 0;
		int busy = 0;
		unsigned gen = 0;
		bool quit = false;

		void work()
		{
			for (size_t k; (k = next.fetch_add(1)) < total;)
				job(k);
		}
		void loop()
		{
			unsigned seen = 0;
			for (;;) {
				{
					std::unique_lock<std::mutex> lk(m);
					cv.wait(lk, [&] { return gen != seen; });
					seen = gen;
					if (quit)
						return;
				}
				work();
				{
					std::lock_guard<std::mutex> lk(m);
					busy--;
				}
				done_cv.notify_one();
			}
		}
	};
private:
	std::unique_ptr<worker_pool> pool;

	const sample *get_sample(const std::string &file)
	{
		auto it = cache.find(file);
		if (it != cache.end())
			return it->second.get();
		auto smp = shared_sample(pt.dir + file);
		if (!smp)
			std::fprintf(stderr, "サンプルを読めない: %s\n", file.c_str());
		return (cache[file] = std::move(smp)).get();
	}

	// EG の行き先（50〜65）はキーオンのときのソースの値で決まる（鳴っている間に動かしても変わらない）ので、鳴らす前に時間・深さの値をずらす
	void eg_ctrl(element_params &p, int cbox)
	{
		for (size_t k = 0; k < sets.size() && k < 6; k++) {
			const cset &c = sets[k];
			if (c.dest < 50 || c.dest > 65 || c.depth == 64 || c.src < 0 || c.src > 15 || !(cbox >> k & 1))
				continue;
			const int v = srcval[c.src];         // その音を鳴らしたときのソースの値
			ctrl_eg(p, c.dest, (c.depth - 64) / 64.0 * (bipolar(c.src) ? (v - 64) / 64.0 : v / 127.0));
		}
	}

	void apply_ctl()
	{
		vol_off = 0;
		for (size_t k = 0; k < sets.size() && k < 6; k++) {
			const cset &c = sets[k];
			if (c.dest != 1 || c.depth == 64 || c.src < 0 || c.src > 15)
				continue;
			const int v = srcval[c.src];
			vol_off += (c.depth - 64) / 64.0 * (bipolar(c.src) ? (v - 64) / 64.0 : v / 127.0) * CTRL_VOLUME_SCALE;
		}
		for (size_t vi = 0; vi < voices.size(); vi++) {
			ctrl_mod m;
			for (size_t k = 0; k < sets.size() && k < 6; k++) {
				const cset &c = sets[k];
				if (c.dest <= 1 || c.depth == 64 || c.src < 0 || c.src > 15)
					continue;
				if (c.dest >= 41 && !(vcbox[vi] >> k & 1))     // 41 から上はエレメントの行き先（36〜40 はコモン LFO などの音色全体）
					continue;
				const int v = srcval[c.src];
				const double x = bipolar(c.src) ? (v - 64) / 64.0 : v / 127.0;
				ctrl_add(m, c.dest, (c.depth - 64) / 64.0 * x);
			}
			// ピッチベンド: 上は pbup − 64、下は 64 − pbdown 半音
			const int pb = srcval[0];
			if (pb != 64)
				m.cents += pb > 64 ? (pb - 64) / 63.0 * (geti(pt.ctrlline, "pbup", 66) - 64) * 100.0 : (pb - 64) / 64.0 * (64 - geti(pt.ctrlline, "pbdown", 62)) * 100.0;
			voices[vi].cm = m;
		}
	}

	// ポルタメント: 時間のモード 0・2 は速さ一定（1 オクターブあたり porta_octave_seconds 秒）、1・3 は幅によらず同じ時間で着く。
	// 2・3 は 1 オクターブより遠いと、まず残り 1 オクターブの所へ跳んでから滑る（実機: 24 半音で、10%・50% を跳んで越え、90% までが 1 オクターブぶん）
	double glide_rate(double cents) const
	{
		const double sec = std::max(1e-4, porta_octave_seconds(ptime));
		return (ptmode & 1) ? std::fabs(cents) / (sec * 44100.0) : 1200.0 / (sec * 44100.0);
	}
	double glide_from(double c) const { return ptmode >= 2 ? std::max(-1200.0, std::min(1200.0, c)) : c; }
	template <class F>
	void voices_of(int id, F fn)
	{
		for (size_t vi = 0; vi < voices.size(); vi++)
			if (vnote[vi] == id)
				fn(voices[vi]);
	}
	void release_note(int id)
	{
		notes[id].held = false;
		for (size_t vi = 0; vi < voices.size(); vi++)
			if (vnote[vi] == id && vnoff[vi])
				voices[vi].release();
	}
	void kill_note(int id)
	{
		notes[id].held = false;
		notes[id].alive = false;
		voices_of(id, [](voice &v) { v.steal(); });
	}
	int start_note(int key, int vel, int i)
	{
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
	}
	// 発音数: エレメント 128 音。超えたら、押さえている中でいちばん低い鍵を除いて、いちばん古い音をまるごと奪う（実機: 4 エレメント × 33 鍵で
	// 2 番目に古い鍵が消え、上から弾くといちばん古い鍵から消えた）。離してリリース中の音を先に奪う
	void make_room(int need)
	{
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
					voices_of(int(k), [&](voice &v) { sounding |= !v.done && !v.kill; });
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
	}
	const element *drum_element(int key) const
	{
		for (const element &e : pt.els)
			if (key >= geti(e.f, "note_lo") && key <= geti(e.f, "note_hi"))
				return &e;
		return nullptr;
	}
	int count_elements(int key, int vel) const
	{
		int c = 0;
		for (const element &e : pt.els)
			if (key >= geti(e.f, "note_lo") && key <= geti(e.f, "note_hi") && vel >= geti(e.f, "vel_lo") && vel <= geti(e.f, "vel_hi"))
				c++;
		return c;
	}

	// 1 音ぶんのエレメントを作る。t_on はその音の鳴り始め（サンプル）、note_id は通し番号、note_index は音色を選んでから何音目か（オルタネートパン）
	void add_note(int note, int vel, int t_on, int note_id, int note_index)
	{
		const kv &fxline = pt.fxline, &clfoline = pt.clfoline;
	for (const element &e : pt.els) {
		if (note < geti(e.f, "note_lo") || note > geti(e.f, "note_hi") || vel < geti(e.f, "vel_lo") || vel > geti(e.f, "vel_hi"))
			continue;
		// ドラムの鍵は、押した鍵に依らず決まった鍵（dnote）で鳴らす（音程・区間・鍵で変わるものは全部その鍵で）
		const int en = geti(e.f, "dnote", -1) >= 0 ? geti(e.f, "dnote") : note;
		// 区間は、コースチューンでずらしたあとの鍵で選ぶ（実機: PBasFngDst のエレメント 1 はコース −12 で、鍵 60 なら鍵 48 で録ったサンプルの減り方になる）
		const int zk = std::max(0, std::min(127, en + geti(e.f, "coarse")));
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
		const sample *s = get_sample(z->file);
		if (!s)
			continue;
		element_params p;
		p.coarse = geti(e.f, "coarse");
		p.fine = geti(e.f, "fine");
		p.level = geti(e.f, "level", 127);
		// Filter Gain は 1 あたり 0.375dB。サンプルは雛形の音色（250）で録ったので、その差だけ掛ける
		p.gain = std::pow(10.0, (swp50_filter::filter_gain_db(geti(e.f, "fgain", 250)) - swp50_filter::filter_gain_db(250)) / 20.0);
		p.pan = (fixed_pan ? 64 : geti(e.f, "pan", 64)) + pan_off;
		// 1 音ごとに変わるもの（tools/motif/mr_pernote.py、2026-10-25）
		p.pan += alternate_pan_offset(geti(e.f, "apan", 64), note_index);
		if (seed >= 0) {
			p.pan += random_pan_offset(geti(e.f, "rpan"), rng);
			p.fine += 12 * random_pitch_steps(geti(e.f, "rpitch"), rng);     // 1 刻み 18.75 セント = ファイン 12
		}
		p.scaling_pan = geti(e.f, "span", 64);
		if (!clfoline.empty()) {
			common_lfo &c = p.clfo;
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
		started++;
		eg_ctrl(p, geti(e.f, "cbox"));
		voices.back().start(*s, p, en, vel);
		vgroup.push_back(std::max(0, std::min(2, geti(e.f, "ins"))));
		vcbox.push_back(geti(e.f, "cbox"));
		vnoff.push_back(char(geti(e.f, "noff", 1) != 0));
		vrs.push_back(float(geti(e.f, "rsend") / 127.0));
		vcs.push_back(float(geti(e.f, "csend") / 127.0));
		vnote.push_back(note_id);
		// テンポ同期なら音色のテンポ（fx の tempo）で拍を秒に直す（Willows のエレメント 3 は 2 分音符・テンポ 48 で 2.5 秒遅れ、2 秒押さえでは鳴らない）
		const double kd_ms = geti(e.f, "ksync") ? key_on_delay_beats(geti(e.f, "klen")) * 60000.0 / std::max(1, geti(fxline, "tempo", 120))
		                                        : key_on_delay_ms(geti(e.f, "kdelay"));
		vdelay.push_back(t_on + int(kd_ms / 1000.0 * 44100 + 0.5));
	}
	}
};

} // namespace motif_sampler
