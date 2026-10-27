// MOTIF-RACK ES のマルチ（16 パート）を、サンプリング方式の芯（sampler_engine.h）で鳴らす（2026-10-25）。
// 設定は tools/motif/mr_multi_export.py がマルチのダンプから書く .multi（capture/multis/）。
//   パートごと: 音色（バンクと番号 → パッチ）・受信チャンネル・モノ/ポリ・ベロシティと鍵の範囲・ピッチベンドの幅・ベロシティの効き（深さと
//   オフセット）・音量・パン・リバーブとコーラスへの送り・ドライのレベル・ノートシフト・ポルタメント・Voice Pan・アルペジオのスイッチ
//   マルチ全体: リバーブとコーラス（パートの送りを足したものに掛ける）、インサーションを使うパート（8 つまで。パートの音色のインサーションを掛ける）、
//   アルペジオ（1 つ。スイッチの入ったパートのうち、鍵を受けたチャンネルのものが鳴らす）
// ドラムのパートは、鍵ごとのリバーブ・コーラスの送り（とインサーションを通った音の Ins-EF の送り）に、パートの送り / 127 を掛けて送る（実機では測っていない）。
// プログラムチェンジ: そのチャンネルのパート（Receive Program Change が入ったもの）の音色を、チャンネルのバンク（CC0・CC32、Receive Bank Select）と番号で替える。
// 音色は画面の糸で読んでから入れ替える（読む間は前の音色が鳴る）。
// インサーションを掛けるパートは、画面でパートごとに入り切りできる（set_part_ins・sync_ins）。実機の「8 パートまで」は外した。
// パートの EQ（37 pp 2E〜38）はインサーションのあとで掛ける（swp50_eq::part_eq）。
// パートのカットオフ・レゾナンス・AEG の時間のずらし（37 pp 17・18・23・25・26）は engine_options で足す。
// マスター EQ（36 10 00）はリバーブ・コーラスを足したあとで掛ける（swp50_eq::master_eq）。
// まだ無いもの: パートの AEG のサステイン・FEG のずらし（22・27〜2C）・デチューン、マスターエフェクト、
// コーラスからリバーブへの送り、パートの出口の割り当て。ベロシティの効き（深さ × 値 / 64 + オフセット − 64）は Data List の説明からで、実機では測っていない。
#pragma once

#include "sampler_arp.h"
#include "sampler_engine.h"
#include "sampler_fx_rt.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace motif_multi {

using motif_sampler::kv;
using motif_sampler::geti;

// サステイン（CC64）とソステヌート（CC66）。離すのを後回しにした回数を鍵ごとに数え、ペダルを上げたらその回数だけ離す
// （前は鍵ごとの「後回しにしたか」だけで、ペダルを踏んだまま同じ鍵を弾き直すと、上げたときに古い音しか離れず、新しい音が鳴りっぱなしになった）
struct pedals {
	bool sus = false, sos = false;
	bool sos_key[128] = {};                // ソステヌートを踏んだときに押さえていた鍵
	int owed[128] = {};                    // 後回しにした離す回数

	bool hold(int k) const { return sus || (sos && sos_key[k]); }
	// 鍵 k を離したとき: 後回しにするなら true
	bool defer(int k)
	{
		if (k < 0 || k > 127 || !hold(k))
			return false;
		owed[k]++;
		return true;
	}
	template <class Off>
	void flush(Off off)
	{
		for (int k = 0; k < 128; k++)
			if (owed[k] && !hold(k))
				for (; owed[k] > 0; owed[k]--)
					off(k);
	}
	template <class Off>
	void set_sus(bool on, Off off)
	{
		sus = on;
		if (!on)
			flush(off);
	}
	// held(k): 今その鍵を押さえているか
	template <class Held, class Off>
	void set_sos(bool on, Held held, Off off)
	{
		if (on && !sos)
			for (int k = 0; k < 128; k++)
				sos_key[k] = held(k);
		sos = on;
		if (!on) {
			for (bool &b : sos_key)
				b = false;
			flush(off);
		}
	}
	void clear()
	{
		sus = sos = false;
		for (int k = 0; k < 128; k++) {
			sos_key[k] = false;
			owed[k] = 0;
		}
	}
};

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
	std::vector<int> meq;                // マスター EQ（36 10 00 の 20 バイト。無ければ空）
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
			else if (!t.compare(0, 4, "meq ")) {
				auto m = motif_sampler::parse(t);
				auto it = m.find("v");
				meq.clear();
				if (it != m.end())
					for (const char *c = it->second.c_str(); *c;) {
						meq.push_back(std::atoi(c));
						const char *n = std::strchr(c, ',');
						if (!n)
							break;
						c = n + 1;
					}
			}
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

// インサーションの型（tools/motif/mr_ins_types.py が書く capture/ins_types.txt）: 番号（MSB × 128 + LSB）・名前・プリセットで使っている数・初期値（パラメーター 1〜16）
struct ins_type {
	int code = 0;
	std::string name, params;
	int uses = 0;
};

inline std::vector<ins_type> load_ins_types(const std::string &path)
{
	std::vector<ins_type> v;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return v;
	char line[512];
	while (std::fgets(line, sizeof line, f)) {
		if (line[0] == '#')
			continue;
		char name[64] = {}, params[256] = {};
		ins_type t;
		if (std::sscanf(line, "%d %63s %d %255s", &t.code, name, &t.uses, params) == 4) {
			t.name = name;
			for (char &c : t.name)
				if (c == '_')
					c = ' ';
			t.params = params;
			v.push_back(t);
		}
	}
	std::fclose(f);
	return v;
}

// エフェクトのパラメーターの名前と範囲（tools/motif/mr_fx_params.py が Data List の Effect Parameter List から書く capture/fx_params.txt）。
// 1 行に 1 つ: 型の番号・パラメーターの番号（1〜16）・値の下と上・名前|表示の範囲
struct fx_param {
	int no = 0, min = 0, max = 127;
	std::string name, range;
};

inline std::map<int, std::vector<fx_param>> load_fx_params(const std::string &path)
{
	std::map<int, std::vector<fx_param>> m;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return m;
	char line[512];
	while (std::fgets(line, sizeof line, f)) {
		if (line[0] == '#')
			continue;
		int code = 0, n = 0;
		fx_param p;
		if (std::sscanf(line, "%d %d %d %d %n", &code, &p.no, &p.min, &p.max, &n) < 4)
			continue;
		std::string rest = line + n;
		while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r'))
			rest.pop_back();
		const size_t bar = rest.find('|');
		p.name = rest.substr(0, bar);
		p.range = bar == std::string::npos ? std::string() : rest.substr(bar + 1);
		m[code].push_back(p);
	}
	std::fclose(f);
	return m;
}

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

// パートの音色（パッチ・エンジン・インサーション）。プログラムチェンジでは、読む糸で作ってから mtx の中で入れ替える
struct part_voice {
	std::unique_ptr<motif_sampler::patch> pt;
	std::unique_ptr<motif_sampler::engine> eg;
	std::unique_ptr<motif_fx_rt::insertion_rt> ins;
	bool has_ins = false;
	std::string label, ins_name;
	double ins_rev = 0, ins_cho = 0;
};

// プログラムチェンジで替える頼み（パート・バンク・番号）
struct pc_request {
	int part, msb, lsb, prog, pan;
};

// バンクの MSB・LSB と番号 → パッチのファイル名の頭（mr_multi_export.py の BANKS と同じ）と、一覧の名前。無いバンクなら空
inline std::string patch_of(int msb, int lsb, int prog, std::string &bank)
{
	static const struct { int msb, lsb; const char *mm, *name; } t[] = {
		{63, 0, "00", "PRE1"}, {63, 1, "01", "PRE2"}, {63, 2, "02", "PRE3"}, {63, 3, "03", "PRE4"}, {63, 4, "04", "PRE5"}, {63, 5, "05", "PRE6"},
		{0, 0, "06", "GM"}, {63, 8, "08", "USR1"}, {63, 9, "09", "USR2"}, {63, 10, "0A", "USR3"},
		{63, 32, "20", "PREDR"}, {63, 40, "28", "USRDR"}, {127, 0, "21", "GMDR"}};
	for (auto &e : t)
		if (e.msb == msb && e.lsb == lsb) {
			char b[32], f[16];
			// GM のドラムは 1 つだけ（番号に依らない）
			const int p = msb == 127 ? 0 : prog;
			std::snprintf(b, sizeof b, "%s:%03d", e.name, p + 1);
			std::snprintf(f, sizeof f, "%s_%03d", e.mm, p);
			bank = b;
			return f;
		}
	return std::string();
}

struct part {
	part_cfg cfg;
	std::unique_ptr<motif_sampler::patch> pt;
	std::unique_ptr<motif_sampler::engine> eg;
	std::unique_ptr<motif_fx_rt::insertion_rt> ins;
	bool has_ins = false;
	std::string label, ins_name;
	// 鳴らす糸と画面の糸で触る（mtx の中で）
	int ch = 0, vol = 100, pan = 64, rev = 0, cho = 0, dry = 127, exp = 127;
	bool mute = false, arpsw = false;
	pedals ped;
	int down[128] = {};                    // 押さえている鍵（ノートシフトのあと。ソステヌートが見る）
	int rpn_msb = 127, rpn_lsb = 127;      // RPN（CC101・100）。0・0 ベンドの幅・0・1 Fine Tune・0・2 Coarse Tune（engine::rpn_data）
	std::atomic<int> voices{0};
	std::atomic<float> peak{0};
	double ins_rev = 0, ins_cho = 0;      // ドラム: インサーションを通った音のリバーブ・コーラスへの送り（/ 127）
	int ins_quiet = 0;                     // 入る音も出る音も無いまま続いたサンプルの数（鳴らす糸だけ）
	swp50_eq::part_eq peq;                 // パートの EQ（37 pp 2E〜38）
	// 描いた音（render の中だけ）。rs・cs はリバーブ・コーラスへ送る音（ふつうの音色は ol・orr と同じ、ドラムは鍵ごとの送りを掛けたもの）
	float ol[256] = {}, orr[256] = {}, rsl[256] = {}, rsr[256] = {}, csl[256] = {}, csr[256] = {};

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
	bool use_ins = true, use_rev = true, use_cho = true, use_meq = true;
	swp50_eq::master_eq meq;               // マスター EQ（リバーブ・コーラスを足したあと、全体の音量の前）

	// 読む（読む糸で）。パッチはパッチの置き場、エフェクトの応答は capture の置き場（fx/・fxd/）から
	bool load(const config &c, const std::string &patch_dir, const std::string &cap, int threads)
	{
		cfg = c;
		// インサーションを掛けるパート（初めはマルチの設定のもの。実機は 8 パートまでだが、ここでは数を限らない）
		for (int i = 0; i < 16; i++)
			ins_on[size_t(i)] = std::find(cfg.ins.begin(), cfg.ins.end(), i) != cfg.ins.end();
		pdir = patch_dir;
		capdir = cap;
		tempo = std::max(1, geti(cfg.fxline, "tempo", 120));
		if (cfg.meq.size() >= 20)
			meq.set(cfg.meq.data());
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
			P->peq.set(pc.get("eqlf", 0x40), pc.get("eqlg", 0x40), pc.get("eqmf", 0x80), pc.get("eqmg", 0x40), pc.get("eqmq", 0x10),
			           pc.get("eqhf", 0xC0), pc.get("eqhg", 0x40));
			part_voice v = build_voice(i, pc.voice, pc.bank, P->pan);
			swap_voice(*P, v);
			parts[size_t(i)] = std::move(P);
			// チャンネルのバンクは、そのチャンネルの若い番号のパートのもの（プログラムチェンジだけ来たときに使う）
			const int ch = parts[size_t(i)]->ch;
			if (ch >= 0 && ch < 16 && bank_msb[ch] < 0) {
				bank_msb[ch] = pc.get("bank_msb", 63);
				bank_lsb[ch] = pc.get("bank_lsb", 0);
			}
		}
		auto get = [&](const char *k) {
			auto it = cfg.fxline.find(k);
			return it == cfg.fxline.end() ? motif_fx::block() : motif_fx::parse_block(it->second);
		};
		const motif_fx::block rv = get("reverb"), ch = get("chorus");
		char t[96];
		rev.init(rv, cap + "/fx", geti(cfg.fxline, "chorev"));
		std::snprintf(t, sizeof t, "%02X %02X", rv.msb, rv.lsb);
		rev_name = rev.active() ? t : std::string(t) + "（応答が無い）";
		cho.init(ch, tempo, cap + "/fxd");
		std::snprintf(t, sizeof t, "%02X %02X", ch.msb, ch.lsb);
		cho_name = cho.active() ? (cho.approx() ? std::string(t) + "（近似）" : std::string(t)) : std::string(t) + "（この型はまだ）";
		if (threads > 1)
			pool = std::make_unique<motif_sampler::engine::worker_pool>(threads - 1);
		return true;
	}

	// パート i の音色 voice（パッチのファイル名の頭）を作る（読む糸で呼んでよい。パートの設定で鳴らし方を上書きする）
	part_voice build_voice(int i, const std::string &voice, const std::string &bank, int pan) const
	{
		part_voice v;
		const part_cfg &pc = cfg.part[size_t(i)];
		if (voice == "-" || voice.empty()) {
			v.label = bank + "（パッチが無い）";
			return v;
		}
		auto pt = std::make_unique<motif_sampler::patch>();
		if (!pt->load(pdir + "/" + voice + ".patch")) {
			v.label = bank + "（パッチが無い: " + voice + "）";
			return v;
		}
		// パートの設定で音色の鳴らし方を上書きする（モノ/ポリ・ポルタメント・ピッチベンドの幅）
		pt->voiceline["mono"] = std::to_string(pc.get("mode", 1));
		pt->voiceline["porta"] = std::to_string(pc.get("porta", 0));
		pt->voiceline["ptime"] = std::to_string(pc.get("ptime", 0));
		pt->voiceline["pmode"] = std::to_string(pc.get("pmode", 0));
		pt->ctrlline["pbup"] = std::to_string(pc.get("pbup", 64));
		pt->ctrlline["pbdown"] = std::to_string(pc.get("pbdown", 64));
		motif_sampler::engine_options o;
		o.pan_offset = pan - 64;
		o.fixed_pan = pc.get("voicepan", 1) == 0;
		// パートのフィルター・AEG のずらし。ノイズの音色で CC74・71 を細かく動かした音と比べると、パートの 1 目盛りはカットオフで CC の約 1.2 倍
		// （パラメーターの 2.4 目盛り）、レゾナンスで約 0.8 倍（1.6 目盛り）。AEG の時間は CC73・75・72 と見分けが付かなかったので同じ 2 目盛り
		// （tools/motif/mr_part_offset.py、2026-10-27。ノイズの揺れで 1 点ごとに ±0.2 倍ほどばらつく）
		o.part_cutoff = int(std::lround(2.4 * (pc.get("cutoff", 64) - 64)));
		o.part_reso = int(std::lround(1.6 * (pc.get("reso", 64) - 64)));
		o.part_atk = 2 * (pc.get("aeg_atk", 64) - 64);
		o.part_dec = 2 * (pc.get("aeg_dec", 64) - 64);
		o.part_rel = 2 * (pc.get("aeg_rel", 64) - 64);
		v.pt = std::move(pt);
		v.eg = std::make_unique<motif_sampler::engine>(*v.pt, o);
		v.eg->preload();
		v.label = bank + " " + v.pt->name;
		v.ins_rev = geti(v.pt->fxline, "insrev") / 127.0;
		v.ins_cho = geti(v.pt->fxline, "inscho") / 127.0;
		v.ins = std::make_unique<motif_fx_rt::insertion_rt>();
		if (ins_on[size_t(i)]) {
			v.ins = make_ins(*v.pt, v.ins_name);
			v.has_ins = true;
		}
		return v;
	}

	// 音色のインサーション（A・B とつなぎ方）を作る（読む糸・画面の糸で）
	std::unique_ptr<motif_fx_rt::insertion_rt> make_ins(const motif_sampler::patch &pt, std::string &name) const
	{
		auto ins = std::make_unique<motif_fx_rt::insertion_rt>();
		auto get = [&](const char *k) {
			auto it = pt.fxline.find(k);
			return it == pt.fxline.end() ? motif_fx::block() : motif_fx::parse_block(it->second);
		};
		const motif_fx::block ia = get("insa"), ib = get("insb");
		ins->init(ia, ib, geti(pt.fxline, "connect"), tempo, capdir + "/fxd");
		char t[64];
		std::snprintf(t, sizeof t, "%02X %02X・%02X %02X", ia.msb, ia.lsb, ib.msb, ib.lsb);
		name = t;
		return ins;
	}

	// ---- インサーションを掛けるパート（画面の糸だけが触る。鳴らす糸へは sync_ins で渡す） ----
	void set_part_ins(int i, bool on)
	{
		if (i >= 0 && i < 16)
			ins_on[size_t(i)] = on;
	}
	bool part_ins(int i) const { return i >= 0 && i < 16 && ins_on[size_t(i)]; }

	// パート i のインサーションの型を替える（画面の糸で）。which: 0 = A、1 = B（value は「型,パラメーター 1〜16」）、2 = つなぎ方（0 並列・1 A→B・2 B→A）。
	// 音色の設定（パッチの fx 行）を書き換えるので、プログラムチェンジで音色が替わると、その音色のものに戻る
	template <class Mutex>
	void set_ins_block(Mutex &mtx, int i, int which, const std::string &value)
	{
		part &P = *parts[size_t(i)];
		if (!P.pt)
			return;
		{
			std::lock_guard<Mutex> lk(mtx);
			P.pt->fxline[which == 0 ? "insa" : which == 1 ? "insb" : "connect"] = value;
		}
		if (!P.has_ins)
			return;
		std::string name;
		auto ins = make_ins(*P.pt, name);
		std::lock_guard<Mutex> lk(mtx);
		std::swap(P.ins, ins);
		P.ins_name = name;
	}
	// エフェクトのブロックの値（型, パラメーター 1〜16 [, Return, 送り]）。which: 0 = パート i の A、1 = B、2 = マルチのリバーブ、3 = コーラス。無ければ空
	std::vector<int> block_values(int i, int which) const
	{
		const kv *m = nullptr;
		const char *key = which == 0 ? "insa" : which == 1 ? "insb" : which == 2 ? "reverb" : "chorus";
		if (which >= 2)
			m = &cfg.fxline;
		else if (parts[size_t(i)]->pt)
			m = &parts[size_t(i)]->pt->fxline;
		std::vector<int> v;
		if (!m)
			return v;
		auto it = m->find(key);
		if (it == m->end())
			return v;
		for (const char *c = it->second.c_str(); *c;) {
			v.push_back(std::atoi(c));
			while (*c && *c != ',')
				c++;
			if (*c)
				c++;
		}
		return v;
	}
	// 値を書いて、その効果を作り直す（画面の糸で。作るのは鍵の外、入れ替えは中）
	template <class Mutex>
	void set_block_values(Mutex &mtx, int i, int which, const std::vector<int> &v)
	{
		std::string str;
		for (size_t k = 0; k < v.size(); k++)
			str += (k ? "," : "") + std::to_string(v[k]);
		if (which < 2) {
			set_ins_block(mtx, i, which, str);
			return;
		}
		cfg.fxline[which == 2 ? "reverb" : "chorus"] = str;
		const motif_fx::block b = motif_fx::parse_block(str);
		char t[64];
		std::snprintf(t, sizeof t, "%02X %02X", b.msb, b.lsb);
		if (which == 2) {
			motif_fx_rt::reverb_rt r;
			r.init(b, capdir + "/fx");
			std::lock_guard<Mutex> lk(mtx);
			std::swap(rev, r);
			rev_name = rev.active() ? t : std::string(t) + "（応答が無い）";
		} else {
			motif_fx_rt::chorus_rt c;
			c.init(b, tempo, capdir + "/fxd");
			std::lock_guard<Mutex> lk(mtx);
			std::swap(cho, c);
			cho_name = cho.active() ? (cho.approx() ? std::string(t) + "（近似）" : std::string(t)) : std::string(t) + "（この型はまだ）";
		}
	}

	// パート i の A・B の型の番号（−1 は音色が無い）
	int ins_code(int i, int which) const
	{
		const part &P = *parts[size_t(i)];
		if (!P.pt)
			return -1;
		auto it = P.pt->fxline.find(which ? "insb" : "insa");
		return it == P.pt->fxline.end() ? 0 : std::atoi(it->second.c_str());
	}
	// 割り当てとパートのインサーションがずれていたら直す（画面の糸で毎回。作るのは mtx の外、入れ替えは中）
	template <class Mutex>
	void sync_ins(Mutex &mtx)
	{
		for (size_t i = 0; i < parts.size(); i++) {
			part &P = *parts[i];
			const bool want = ins_on[i];
			if (!P.pt || want == P.has_ins)
				continue;
			std::unique_ptr<motif_fx_rt::insertion_rt> ins;
			std::string name;
			if (want)
				ins = make_ins(*P.pt, name);
			std::lock_guard<Mutex> lk(mtx);
			if (want) {
				std::swap(P.ins, ins);
				P.ins_name = name;
			}
			P.has_ins = want;
		}
	}

	// パートの音色を v と入れ替える（mtx の中で）。前の音色は v に残るので、mtx の外で捨てる。コントローラーの値は引き継ぐ
	// （プログラムチェンジのすぐあとに来た CC73・75・72 などは、読み終わる前の前の音色が受けている。ドラムキットは読むのに時間がかかるので、
	// 引き継がないと曲の頭の EG のずらしが全部消えていた）
	static void swap_voice(part &P, part_voice &v)
	{
		if (P.eg && v.eg) {
			for (int s = 0; s < 16; s++)
				v.eg->set_source(s, P.eg->source(s));
			v.eg->take_controls(*P.eg);
		}
		std::swap(P.pt, v.pt);
		std::swap(P.eg, v.eg);
		std::swap(P.ins, v.ins);
		std::swap(P.has_ins, v.has_ins);
		std::swap(P.label, v.label);
		std::swap(P.ins_name, v.ins_name);
		std::swap(P.ins_rev, v.ins_rev);
		std::swap(P.ins_cho, v.ins_cho);
		P.ped.clear();
		for (int &d : P.down)
			d = 0;
		if (v.eg)
			v.eg->all_sound_off();
	}

	// プログラムチェンジ（mtx の中で）: そのチャンネルを受けるパートのうち、Receive Program Change（37 pp 1F のビット 4）が入ったものに頼みを積む。
	// 読むのは画面の糸（take_requests → build_voice → swap_voice）
	void program_change(int ch, int prog)
	{
		if (ch < 0 || ch >= 16)
			return;
		for (size_t i = 0; i < parts.size(); i++) {
			part &P = *parts[i];
			if (P.ch != ch || !(P.cfg.get("rx1", 127) >> 4 & 1))
				continue;
			// 同じパートの前の頼みは捨てる（続けて送られたら最後のものだけ）
			requests.erase(std::remove_if(requests.begin(), requests.end(), [&](const pc_request &r) { return r.part == int(i); }), requests.end());
			requests.push_back({int(i), bank_msb[ch] < 0 ? 63 : bank_msb[ch], bank_lsb[ch] < 0 ? 0 : bank_lsb[ch], prog, P.pan});
		}
	}
	std::vector<pc_request> take_requests()
	{
		std::vector<pc_request> r;
		r.swap(requests);
		return r;
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
			case 0:
				if (P.cfg.get("rx1", 127) >> 3 & 1)       // Receive Bank Select（ビット 3）
					bank_msb[ch] = val;
				break;
			case 32:
				if (P.cfg.get("rx1", 127) >> 3 & 1)
					bank_lsb[ch] = val;
				break;
			case 7: P.vol = val; break;
			case 11: P.exp = val; break;
			case 10:
				P.pan = val;
				if (P.eg)
					P.eg->control(10, val);
				break;
			case 91: P.rev = val; break;
			case 93: P.cho = val; break;
			case 64: P.ped.set_sus(val >= 64, [&](int k) { if (P.eg) P.eg->note_off(k); }); break;
			case 66: P.ped.set_sos(val >= 64, [&](int k) { return P.down[k] > 0; }, [&](int k) { if (P.eg) P.eg->note_off(k); }); break;
			case 101: P.rpn_msb = val; break;
			case 100: P.rpn_lsb = val; break;
			case 99:
			case 98: P.rpn_msb = P.rpn_lsb = 127; break;   // NRPN は受けない
			case 6:
			case 38:
				if (P.rpn_msb == 0 && P.eg)
					P.eg->rpn_data(P.rpn_lsb, num, val);
				break;
			case 5:
			case 65:
			case 71:
			case 72:
			case 73:
			case 74:
			case 75:
			case 126:
			case 127:
				if (P.eg)
					P.eg->control(num, val);
				break;
			case 120:
				if (P.eg)
					P.eg->all_sound_off();
				break;
			case 121:
				for (int s = 0; s < 16; s++)
					src(P, s, motif_sampler::engine::bipolar(s) ? 64 : 0);
				P.exp = 127;
				P.ped.set_sus(false, [&](int k) { if (P.eg) P.eg->note_off(k); });
				P.ped.set_sos(false, [&](int k) { return P.down[k] > 0; }, [&](int k) { if (P.eg) P.eg->note_off(k); });
				P.rpn_msb = P.rpn_lsb = 127;
				if (P.eg)
					P.eg->reset_controls();
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
			p->ped.clear();
			for (int &d : p->down)
				d = 0;
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
		if (pool) {
			// エレメントの多いパートは、パートの中のエレメントを糸の束で分けて 1 つずつ描き、残りのパートを束で並べて描く
			// （1 つのパートに音が集まると 1 本の糸では間に合わず、ブツブツ切れた。BigStrings の 17 音（51 エレメント）で 1 ブロックの
			// 持ち時間の 60%、遅いブロックは 134%。2026-10-28）。足す順は変わらないので、音は同じ
			size_t light[16];
			size_t nl = 0;
			for (size_t i = 0; i < parts.size(); i++) {
				part &P = *parts[i];
				if (P.eg && P.eg->active_voices() >= HEAVY_VOICES) {
					P.eg->use_pool(pool.get());
					work(i);
					P.eg->use_pool(nullptr);
				} else
					light[nl++] = i;
			}
			pool->run(nl, [&](size_t k) { work(light[k]); });
		} else
			for (size_t i = 0; i < parts.size(); i++)
				work(i);
		for (int j = 0; j < n; j++) {
			double ml = 0, mr = 0, rl = 0, rr = 0, cl = 0, cr = 0;
			for (auto &p : parts) {
				const double d = p->dry / 127.0, s1 = p->rev / 127.0, s2 = p->cho / 127.0;
				ml += p->ol[j] * d;
				mr += p->orr[j] * d;
				rl += p->rsl[j] * s1;
				rr += p->rsr[j] * s1;
				cl += p->csl[j] * s2;
				cr += p->csr[j] * s2;
			}
			// コーラスを先に回し、Return の前の wet をリバーブへ（コーラスからリバーブへの送り）。足す順は前と同じ（リバーブ → コーラス）
			double xl = 0, xr = 0, ql = 0, qr = 0, pl = 0, pr = 0;
			if (cho.active())
				cho.tick(cl, cr, ql, qr, &pl, &pr);
			if (rev.active()) {
				rev.tick(rl, rr, xl, xr, pl, pr);
				if (use_rev)
					ml += xl, mr += xr;
			}
			if (cho.active() && use_cho)
				ml += ql, mr += qr;
			if (use_meq)
				meq.process(ml, mr);
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
	static constexpr int INS_IDLE_SAMPLES = 5 * 44100;   // 鳴っていないパートのインサーションを止めるまでの無音
	static constexpr size_t HEAVY_VOICES = 12;    // これ以上のエレメントが鳴っているパートは、中を分けて描く
	std::string pdir, capdir;
	int tempo = 120;
	int bank_msb[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
	int bank_lsb[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
	std::vector<pc_request> requests;
	std::array<std::atomic<bool>, 16> ins_on{};    // パートにインサーションを掛けるか（読む糸の build_voice も見る）
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
		P.down[k]++;
		P.eg->note_on(k, std::min(127, std::max(1, v)));
	}
	static void part_off(part &P, int key)
	{
		if (!P.eg)
			return;
		const int k = key + P.cfg.get("shift", 64) - 64;
		if (k < 0 || k > 127)
			return;
		if (P.down[k] > 0)
			P.down[k]--;
		if (P.ped.defer(k))
			return;
		P.eg->note_off(k);
	}

	// パート 1 つを描いて、インサーション・音色の音量・パートの音量（CC7 と同じ 2 乗）まで掛けたものを P.ol・P.orr に
	void render_part(part &P, int n, const arp_event *evs, int ne)
	{
		float l[256] = {}, r[256] = {}, voff[256] = {};
		float g0[256] = {}, g1[256] = {}, g2[256] = {}, h0[256] = {}, h1[256] = {}, h2[256] = {};
		float s0[256] = {}, s1[256] = {}, s2[256] = {}, s3[256] = {};
		if (!P.eg) {
			for (float *b : {P.ol, P.orr, P.rsl, P.rsr, P.csl, P.csr})
				std::fill(b, b + n, 0.0f);
			return;
		}
		const bool drum = P.eg->drum();
		int at = 0;
		for (int e = 0; e <= ne; e++) {
			const int upto = e < ne ? std::min(n, std::max(at, evs[e].pos)) : n;
			if (upto > at) {
				float *gl[3] = {g0 + at, g1 + at, g2 + at}, *gr[3] = {h0 + at, h1 + at, h2 + at};
				float *sb[4] = {s0 + at, s1 + at, s2 + at, s3 + at};
				P.eg->render(upto - at, l + at, r + at, gl, gr, voff + at, drum ? sb : nullptr);
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
		// 鳴っていないパートのインサーションは、入る音が無く、出る音も 5 秒続けて 1e-8 未満なら回さない（2026-10-28。無音でも毎サンプル
		// 回していて、マルチ 41_000 では音を 1 つも鳴らさずに 1 本の糸で 1 ブロックの持ち時間の 63% かかっていた）。捨てるのは 1e-8 未満の尾だけ。
		// 止めている間は揺れる型の LFO も進まない
		bool in_silent = P.eg->active_voices() == 0;
		for (int j = 0; j < n && in_silent; j++)
			if (l[j] != 0.0f || r[j] != 0.0f || g0[j] != 0.0f || g1[j] != 0.0f || g2[j] != 0.0f || h0[j] != 0.0f || h1[j] != 0.0f || h2[j] != 0.0f)
				in_silent = false;
		const bool ins_idle = in_silent && P.ins_quiet >= INS_IDLE_SAMPLES;
		double ins_peak = 0;
		for (int j = 0; j < n; j++) {
			double dl, dr, il = 0, ir = 0;
			if (P.has_ins && use_ins && ins_idle) {
				dl = dr = 0.0;
			} else if (P.has_ins && use_ins) {
				const double xl[3] = {g0[j], g1[j], g2[j]}, xr[3] = {h0[j], h1[j], h2[j]};
				P.ins->tick(xl, xr, dl, dr, il, ir);
				ins_peak = std::max(ins_peak, std::max(std::max(std::fabs(dl), std::fabs(dr)), std::max(std::fabs(il), std::fabs(ir))));
			} else {
				dl = l[j];
				dr = r[j];
			}
			const double g = P.eg->volume_gain(voff[j]) * pg;
			// パートの EQ はインサーションのあと（ドラムの鍵ごとの送りには掛けていない）
			P.peq.process(dl, dr);
			P.ol[j] = float(dl * g);
			P.orr[j] = float(dr * g);
			if (drum) {
				P.rsl[j] = float((s0[j] + il * P.ins_rev) * g);
				P.rsr[j] = float((s1[j] + ir * P.ins_rev) * g);
				P.csl[j] = float((s2[j] + il * P.ins_cho) * g);
				P.csr[j] = float((s3[j] + ir * P.ins_cho) * g);
			} else {
				P.rsl[j] = P.csl[j] = P.ol[j];
				P.rsr[j] = P.csr[j] = P.orr[j];
			}
			pk = std::max(pk, std::max(std::fabs(P.ol[j]), std::fabs(P.orr[j])));
		}
		P.peak = std::max(pk, P.peak.load() * 0.95f);
		if (!in_silent || ins_peak >= 1e-8)
			P.ins_quiet = 0;
		else if (P.ins_quiet < INS_IDLE_SAMPLES)
			P.ins_quiet += n;
	}
};

}  // namespace motif_multi
