// MOTIF-RACK ES のボイスを画面でエディットするための表と、パッチの書き出し（2026-10-31。motif_play のボイスモード）。
// パッチ（sampler_engine.h の patch）の欄をそのまま書き換える。エレメントの欄は engine が音を鳴らすたびに読むので、次に鳴らす音から効く。
// 共通の欄のうち voice 行・ctrl 行は engine を作るときに読むので、替えたら engine を作り直す（rebuild が真のもの）。
// 並びは Data List の VOICE（共通 → エレメントの Oscillator・Pitch・Filter・Amplitude・LFO・EQ）に寄せた。名前は mr_patch.py の欄の名前
#pragma once
#include "sampler_engine.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace motif_voice_edit {

using motif_sampler::kv;

// 1 つのつまみ。idx は「a,b,c」の並びの何番目か（−1 なら 1 つの値）。画面の値 = 欄の値 − off、範囲は画面の値で lo〜hi。
// names があれば選ぶ欄にする（画面の値 lo + i の名前。nullptr の所は番号）
struct param {
	const char *key;
	int idx;
	const char *label;
	int lo, hi, def;
	int off = 0;
	const char *const *names = nullptr;
	bool rebuild = false;
};
struct group {
	const char *title;
	std::vector<param> ps;
	bool open = false;      // 初めから開いておく
};

inline const char *const ONOFF[] = {"切", "入"};
inline const char *const MONO_POLY[] = {"モノ", "ポリ"};
inline const char *const SAME_KEY[] = {"1 音", "重ねる"};
inline const char *const PORTA_MODE[] = {"フィンガード", "フルタイム"};
inline const char *const PORTA_TMODE[] = {"レート", "時間"};
inline const char *const INS_SW[] = {"素通し", "INS A", "INS B"};
inline const char *const FILTER_TYPE[] = {"LPF24D", "LPF24A", "LPF18", "LPF18s", "LPF12+HPF12", "LPF6+HPF12", "HPF24D", "HPF12", "BPF12D", "（9）",
                                          "BPFw", "BPF6", "BEF12", "BEF6", "Dual LPF", "Dual HPF", "Dual BPF", "Dual BEF", "（18）", "LPF12+BPF6", "（20）", "Thru"};
inline const char *const EQ_TYPE[] = {"Low/High", "P.EQ", "Boost6", "Boost12", "Boost18", "Thru"};
inline const char *const ELFO_WAVE[] = {"Saw", "Tri", "Square"};
inline const char *const CLFO_WAVE[] = {"Tri", "Tri+", "Saw Up", "Saw Down", "Squ 1/4", "Squ 1/3", "Squ", "Squ 2/3", "Squ 3/4", "Trapezoid", "S/H 1", "S/H 2", "User"};
inline const char *const CLFO_RESET[] = {"切", "1 音目", "毎音"};

// 共通（voice 行・ctrl 行・clfo 行。line は 0 = voice、1 = ctrl、2 = clfo）
struct common_group {
	int line;
	group g;
};
inline const std::vector<common_group> &common_groups()
{
	static const std::vector<common_group> G = {
		{0, {"共通", {
			{"volume", -1, "音量", 0, 127, 127},
			{"mono", -1, "モノ / ポリ", 0, 1, 1, 0, MONO_POLY, true},
			{"same", -1, "同じ鍵", 0, 1, 1, 0, SAME_KEY, true},
			{"mts", -1, "マイクロチューニング", 0, 127, 0},
		}, true}},
		{0, {"ポルタメント", {
			{"porta", -1, "スイッチ", 0, 1, 0, 0, ONOFF, true},
			{"pmode", -1, "モード", 0, 1, 0, 0, PORTA_MODE, true},
			{"ptime", -1, "時間", 0, 127, 0, 0, nullptr, true},
			{"ptmode", -1, "時間の決め方", 0, 1, 0, 0, PORTA_TMODE, true},
		}, true}},
		{1, {"ピッチベンド", {
			{"pbup", -1, "上（半音）", -48, 24, 2, 64, nullptr, true},
			{"pbdown", -1, "下（半音）", -48, 24, -2, 64, nullptr, true},
		}, true}},
		{2, {"コモン LFO", {
			{"wave", -1, "波形", 0, 12, 0, 0, CLFO_WAVE},
			{"speed", -1, "速さ", 0, 63, 32},
			{"sync", -1, "テンポ同期", 0, 1, 0, 0, ONOFF},
			{"tspeed", -1, "同期の長さ", 0, 21, 11},
			{"phase", -1, "位相", 0, 3, 0},
			{"reset", -1, "キーオンリセット", 0, 2, 1, 0, CLFO_RESET},
			{"delay", -1, "遅れ", 0, 127, 0},
			{"fadein", -1, "フェードイン", 0, 127, 0},
			{"hold", -1, "ホールド", 0, 127, 127},
			{"fadeout", -1, "フェードアウト", 0, 127, 0},
			{"dest", 0, "箱 1 の行き先", 0, 127, 0}, {"depth", 0, "深さ", 0, 127, 0},
			{"dest", 1, "箱 2 の行き先", 0, 127, 0}, {"depth", 1, "深さ", 0, 127, 0},
			{"dest", 2, "箱 3 の行き先", 0, 127, 0}, {"depth", 2, "深さ", 0, 127, 0},
		}}},
	};
	return G;
}

// エレメント（element 行）
inline const std::vector<group> &element_groups()
{
	static const std::vector<group> G = {
		{"鍵域・ベロシティ", {
			{"note_lo", -1, "鍵の下", 0, 127, 0}, {"note_hi", -1, "鍵の上", 0, 127, 127},
			{"vel_lo", -1, "ベロシティの下", 1, 127, 1}, {"vel_hi", -1, "ベロシティの上", 1, 127, 127},
			{"vxf", -1, "ベロシティのクロスフェード", 0, 127, 0},
			{"ins", -1, "インサーション", 0, 2, 1, 0, INS_SW},
			{"kdelay", -1, "キーオンディレイ", 0, 127, 0},
			{"ksync", -1, "ディレイのテンポ同期", 0, 1, 0, 0, ONOFF}, {"klen", -1, "同期の長さ", 0, 19, 11},
		}, true},
		{"音程", {
			{"coarse", -1, "コース（半音）", -48, 48, 0}, {"fine", -1, "ファイン", -64, 63, 0},
			{"pvel", -1, "ベロシティ感度", -64, 63, 0, 64},
			{"pkf", -1, "キーフォロー", -64, 63, 32, 64}, {"pkc", -1, "キーフォローの中心", 0, 127, 60},
			{"rpitch", -1, "ランダムピッチ", 0, 127, 0},
		}},
		{"PEG（ピッチの EG）", {
			{"pegt", 0, "時間 H", 0, 127, 0}, {"pegt", 1, "A", 0, 127, 0}, {"pegt", 2, "D1", 0, 127, 0}, {"pegt", 3, "D2", 0, 127, 0}, {"pegt", 4, "R", 0, 127, 0},
			{"pegl", 0, "高さ H", -128, 127, 0}, {"pegl", 1, "A", -128, 127, 0}, {"pegl", 2, "D1", -128, 127, 0}, {"pegl", 3, "D2", -128, 127, 0}, {"pegl", 4, "R", -128, 127, 0},
			{"pegd", -1, "深さ", -64, 63, 0, 64},
			{"pegtv", -1, "時間のベロシティ感度", -64, 63, 0, 64}, {"pegtvs", -1, "感度の掛かる所", 0, 4, 4},
			{"peglv", -1, "高さのベロシティ感度", -64, 63, 0, 64}, {"peglc", -1, "高さの感度のカーブ", 0, 4, 0},
			{"pegkf", -1, "時間のキーフォロー", -64, 63, 0, 64}, {"pegkc", -1, "その中心", 0, 127, 60},
		}},
		{"フィルター", {
			{"ftype", -1, "型", 0, 21, 21, 0, FILTER_TYPE},
			{"cutoff", -1, "カットオフ", 0, 255, 255}, {"fvel", -1, "カットオフのベロシティ感度", -64, 63, 0, 64},
			{"reso", -1, "レゾナンス", 0, 127, 0}, {"rvel", -1, "レゾナンスのベロシティ感度", -64, 63, 0, 64},
			{"hpf", -1, "HPF", 0, 255, 0}, {"distance", -1, "ディスタンス", 0, 255, 0},
			{"fkf", -1, "カットオフのキーフォロー", -64, 63, 0, 64}, {"hkf", -1, "HPF のキーフォロー", -64, 63, 0, 64},
			{"fgain", -1, "フィルターゲイン", 0, 255, 250},
			{"bp", 0, "スケーリングの点 1", 0, 127, 0}, {"bp", 1, "2", 0, 127, 0}, {"bp", 2, "3", 0, 127, 0}, {"bp", 3, "4", 0, 127, 0},
			{"off", 0, "ずれ 1", -128, 127, 0, 128}, {"off", 1, "2", -128, 127, 0, 128}, {"off", 2, "3", -128, 127, 0, 128}, {"off", 3, "4", -128, 127, 0, 128},
		}},
		{"FEG（フィルターの EG）", {
			{"fegt", 0, "時間 H", 0, 127, 0}, {"fegt", 1, "A", 0, 127, 0}, {"fegt", 2, "D1", 0, 127, 0}, {"fegt", 3, "D2", 0, 127, 0}, {"fegt", 4, "R", 0, 127, 0},
			{"fegl", 0, "高さ H", -128, 127, 0}, {"fegl", 1, "A", -128, 127, 0}, {"fegl", 2, "D1", -128, 127, 0}, {"fegl", 3, "D2", -128, 127, 0}, {"fegl", 4, "R", -128, 127, 0},
			{"fegd", -1, "深さ", -64, 63, 0, 64},
			{"fegtv", -1, "時間のベロシティ感度", -64, 63, 0, 64}, {"fegtvs", -1, "感度の掛かる所", 0, 4, 4},
			{"feglv", -1, "高さのベロシティ感度", -64, 63, 0, 64}, {"feglc", -1, "高さの感度のカーブ", 0, 4, 0},
			{"fegkf", -1, "時間のキーフォロー", -64, 63, 0, 64}, {"fegkc", -1, "その中心", 0, 127, 60},
		}},
		{"音量・パン", {
			{"level", -1, "エレメントの音量", 0, 127, 127},
			{"pan", -1, "パン", -63, 63, 0, 64}, {"span", -1, "スケーリングパン", -64, 63, 0, 64},
			{"apan", -1, "オルタネートパン", -64, 63, 0, 64}, {"rpan", -1, "ランダムパン", 0, 127, 0},
			{"lvel", -1, "音量のベロシティ感度", -64, 63, 0, 64}, {"lcurve", -1, "そのカーブ", 0, 4, 0},
			{"lkf", -1, "音量のキーフォロー", -64, 63, 0, 64},
			{"lbp", 0, "スケーリングの点 1", 0, 127, 0}, {"lbp", 1, "2", 0, 127, 0}, {"lbp", 2, "3", 0, 127, 0}, {"lbp", 3, "4", 0, 127, 0},
			{"loff", 0, "ずれ 1", -128, 127, 0, 128}, {"loff", 1, "2", -128, 127, 0, 128}, {"loff", 2, "3", -128, 127, 0, 128}, {"loff", 3, "4", -128, 127, 0, 128},
		}, true},
		{"AEG（音量の EG）", {
			{"aegt", 0, "時間 A", 0, 127, 0}, {"aegt", 1, "D1", 0, 127, 0}, {"aegt", 2, "D2", 0, 127, 0}, {"aegt", 3, "R", 0, 127, 64},
			{"aegl", 0, "高さ I", 0, 127, 0}, {"aegl", 1, "A", 0, 127, 127}, {"aegl", 2, "D1", 0, 127, 127}, {"aegl", 3, "D2", 0, 127, 127},
			{"aegtv", -1, "時間のベロシティ感度", -64, 63, 0, 64}, {"aegtvs", -1, "感度の掛かる所", 0, 4, 4},
			{"aegkf", -1, "時間のキーフォロー", -64, 63, 0, 64}, {"aegkc", -1, "その中心", 0, 127, 60},
		}, true},
		{"エレメント LFO", {
			{"lfow", -1, "波形", 0, 2, 1, 0, ELFO_WAVE}, {"lfosync", -1, "キーオンシンク", 0, 1, 1, 0, ONOFF},
			{"lfospeed", -1, "速さ", 0, 63, 32}, {"lfodelay", -1, "遅れ", 0, 127, 0}, {"lfofade", -1, "フェードイン", 0, 127, 0},
			{"lfop", -1, "ピッチの深さ", 0, 127, 0}, {"lfof", -1, "フィルターの深さ", 0, 127, 0}, {"lfoa", -1, "音量の深さ", 0, 127, 0},
		}},
		{"コモン LFO・コントロールの箱", {
			{"lbox", -1, "コモン LFO の箱（ビット 0〜2）", 0, 7, 0}, {"lratio", 0, "位相のずらし", 0, 5, 0},
			{"lratio", 1, "箱 1 の割合", 0, 127, 127}, {"lratio", 2, "箱 2", 0, 127, 127}, {"lratio", 3, "箱 3", 0, 127, 127},
			{"cbox", -1, "コントロールの箱（ビット 0〜5 = 組 1〜6）", 0, 63, 63},
		}},
		{"エレメント EQ", {
			{"eqtype", -1, "型", 0, 5, 5, 0, EQ_TYPE}, {"eqres", -1, "レゾナンス", 0, 127, 16},
			{"eqf1", -1, "周波数 1", 0, 255, 100}, {"eqg1", -1, "ゲイン 1", -64, 63, 0, 64},
			{"eqf2", -1, "周波数 2", 0, 255, 200}, {"eqg2", -1, "ゲイン 2", -64, 63, 0, 64},
		}},
	};
	return G;
}

// 「a,b,c」を数の並びに / 並びを「a,b,c」に
inline std::vector<std::string> split(const std::string &s, char sep = ',')
{
	std::vector<std::string> v;
	size_t a = 0;
	for (;;) {
		const size_t b = s.find(sep, a);
		v.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
		if (b == std::string::npos)
			return v;
		a = b + 1;
	}
}
inline std::string join(const std::vector<std::string> &v, char sep = ',')
{
	std::string s;
	for (size_t i = 0; i < v.size(); i++)
		s += (i ? std::string(1, sep) : std::string()) + v[i];
	return s;
}

// 画面の値（欄に無ければ def）
inline int get(const kv &m, const param &p)
{
	auto it = m.find(p.key);
	if (it == m.end())
		return p.def;
	if (p.idx < 0)
		return std::atoi(it->second.c_str()) - p.off;
	const auto v = split(it->second);
	return p.idx < int(v.size()) ? std::atoi(v[size_t(p.idx)].c_str()) - p.off : p.def;
}
// 画面の値 x を書く（並びが短ければ 0 で延ばす）。AEG のリリースは release の欄も同じ値に（engine は aegt が無いときだけ release を読む）
inline void set(kv &m, const param &p, int x)
{
	const std::string s = std::to_string(x + p.off);
	if (p.idx < 0)
		m[p.key] = s;
	else {
		auto it = m.find(p.key);
		std::vector<std::string> v = it == m.end() || it->second.empty() ? std::vector<std::string>() : split(it->second);
		while (int(v.size()) <= p.idx)
			v.push_back("0");
		v[size_t(p.idx)] = s;
		m[p.key] = join(v);
	}
	if (!std::strcmp(p.key, "aegt") && p.idx == 3 && m.count("release"))
		m["release"] = s;
}

// コントロールセット（ctrl 行の sets=src:dest:depth,…、6 組）
struct cset { int src = 0, dest = 0, depth = 64; };
inline std::vector<cset> get_sets(const kv &ctrl)
{
	std::vector<cset> v;
	auto it = ctrl.find("sets");
	if (it != ctrl.end() && !it->second.empty())
		for (const std::string &t : split(it->second)) {
			cset c;
			if (std::sscanf(t.c_str(), "%d:%d:%d", &c.src, &c.dest, &c.depth) == 3)
				v.push_back(c);
		}
	while (v.size() < 6)
		v.push_back(cset());
	return v;
}
inline void set_sets(kv &ctrl, const std::vector<cset> &v)
{
	std::vector<std::string> t;
	for (const cset &c : v)
		t.push_back(std::to_string(c.src) + ":" + std::to_string(c.dest) + ":" + std::to_string(c.depth));
	ctrl["sets"] = join(t);
}

// 行を書き直す: もとの行の欄の並びのまま今の値を書き、無くなった欄は落とし、増えた欄は後ろに足す
inline std::string rewrite_line(const std::string &head, const std::string &line, const kv &m)
{
	std::string out = head;
	std::vector<std::string> seen;
	for (const std::string &tok : split(line.substr(head.size()), ' ')) {
		const size_t e = tok.find('=');
		if (e == std::string::npos)
			continue;
		const std::string k = tok.substr(0, e);
		auto it = m.find(k);
		if (it == m.end())
			continue;
		out += " " + k + "=" + it->second;
		seen.push_back(k);
	}
	for (const auto &[k, v] : m)
		if (std::find(seen.begin(), seen.end(), k) == seen.end())
			out += " " + k + "=" + v;
	return out;
}

// パッチを書き出す。もとのファイル src の行の並び・区間はそのままに、名前と fx・voice・clfo・ctrl・arp・element の行を p の値で書く。
// 区間のサンプル（file=）は、書き出す置き場から p.dir への相対で書く（engine は置き場 + file で読む）。
// もとに無かった行（clfo・ctrl など）は、element の前に足す
inline bool save_patch(const motif_sampler::patch &p, const std::string &src, const std::string &out, std::string &err)
{
	namespace fs = std::filesystem;
	FILE *f = std::fopen(src.c_str(), "rb");
	if (!f) {
		err = "もとのパッチを開けない: " + src;
		return false;
	}
	std::vector<std::string> lines;
	{
		std::string cur;
		int c;
		while ((c = std::fgetc(f)) != EOF) {
			if (c == '\n') {
				lines.push_back(cur);
				cur.clear();
			} else if (c != '\r')
				cur += char(c);
		}
		if (!cur.empty())
			lines.push_back(cur);
		std::fclose(f);
	}
	std::string from = p.dir;
	while (!from.empty() && (from.back() == '/' || from.back() == '\\'))
		from.pop_back();
	std::error_code ec;
	fs::create_directories(fs::path(out).parent_path(), ec);
	const fs::path from_c = fs::weakly_canonical(fs::path(from), ec), out_c = fs::weakly_canonical(fs::absolute(fs::path(out), ec).parent_path(), ec);
	fs::path prefix = from_c.lexically_relative(out_c);
	if (prefix.empty())
		prefix = from_c;     // 別のドライブ: そのまま
	auto zone_file = [&](const std::string &f) { return (prefix / f).lexically_normal().generic_string(); };
	struct L { const char *head; const kv *m; bool done; };
	L ls[] = {{"fx", &p.fxline, false}, {"voice", &p.voiceline, false}, {"clfo", &p.clfoline, false}, {"ctrl", &p.ctrlline, false}, {"arp", &p.arpline, false}};
	std::vector<std::string> o;
	size_t ei = 0, zi = 0;
	bool named = false;
	auto starts = [](const std::string &s, const char *h) { const size_t n = std::strlen(h); return s.size() > n && !s.compare(0, n, h) && s[n] == ' '; };
	for (const std::string &s : lines) {
		if (!s.empty() && s[0] == '#') {
			o.push_back("# " + p.name);
			named = true;
			continue;
		}
		bool hit = false;
		for (L &l : ls)
			if (starts(s, l.head)) {
				if (!l.m->empty())
					o.push_back(rewrite_line(l.head, s, *l.m));
				l.done = hit = true;
			}
		if (hit)
			continue;
		if (starts(s, "element")) {
			if (ei == 0)
				for (L &l : ls)
					if (!l.done && !l.m->empty()) {
						o.push_back(rewrite_line(l.head, l.head, *l.m));
						l.done = true;
					}
			if (ei < p.els.size())
				o.push_back(rewrite_line("element", s, p.els[ei].f));
			ei++;
			zi = 0;
		} else if (starts(s, "zone")) {
			// 区間の file は読んだパッチのもの（p.dir からの名前）に置き場のずれを足す（もとの行の file は、もう一度書き出すと二重になるので使わない）
			const size_t k = s.find(" file=");
			if (k != std::string::npos && ei > 0 && ei <= p.els.size() && zi < p.els[ei - 1].zones.size())
				o.push_back(s.substr(0, k + 6) + zone_file(p.els[ei - 1].zones[zi].file));
			else
				o.push_back(s);
			zi++;
		} else
			o.push_back(s);
	}
	if (!named)
		o.insert(o.begin(), "# " + p.name);
	if (ei != p.els.size()) {
		err = "もとのパッチとエレメントの数が合わない";
		return false;
	}
	FILE *w = std::fopen(out.c_str(), "wb");
	if (!w) {
		err = "書けない: " + out;
		return false;
	}
	for (const std::string &s : o) {
		std::fputs(s.c_str(), w);
		std::fputc('\n', w);
	}
	const bool ok = std::fclose(w) == 0;
	if (!ok)
		err = "書けない: " + out;
	return ok;
}

// 置き場の中で、まだ無い「ED_nnn.patch」
inline std::string next_edit_file(const std::string &dir)
{
	namespace fs = std::filesystem;
	for (int n = 0; n < 1000; n++) {
		char b[32];
		std::snprintf(b, sizeof b, "ED_%03d.patch", n);
		if (!fs::exists(fs::path(dir) / b))
			return (fs::path(dir) / b).string();
	}
	return std::string();
}

} // namespace motif_voice_edit
