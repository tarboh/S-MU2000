// license:BSD-3-Clause

#include "layout.h"
#include "draw.h"
#include "texts.h"
#include "compat/paths.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>

namespace ui {

namespace {

// 実機の色。名前で書けるようにしておく
struct named_color { const char *name; COLORREF c; };
const named_color COLORS[] = {
	{ "ink",       RGB( 46,  44,  40) },   // 印刷の字
	{ "face",      RGB(196, 189, 170) },   // 本体の面
	{ "key",       RGB(216, 205, 165) },   // ボタンの面
	{ "keyedge",   RGB(126, 118,  92) },   // ボタンのふち
	{ "keydown",   RGB(150, 140,  95) },   // 押したとき
	{ "jack",      RGB( 52,  48,  42) },   // ジャックの穴
	{ "jackedge",  RGB(120, 114, 100) },
	{ "socket",    RGB( 60,  56,  50) },   // MIDI の丸い口
	{ "slot",      RGB(120, 116, 104) },   // カードの差し込み口
	{ "slotedge",  RGB( 90,  86,  76) },
	{ "slotink",   RGB(232, 228, 218) },
	{ "black",     RGB(  0,   0,   0) },
	{ "white",     RGB(255, 255, 255) },
};

COLORREF color_of(const char *name, bool &ok)
{
	ok = true;
	if (name[0] == '#') {
		unsigned v = 0;
		if (std::sscanf(name + 1, "%x", &v) == 1)
			return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
	}
	for (const named_color &n : COLORS)
		if (!std::strcmp(n.name, name))
			return n.c;
	ok = false;
	return RGB(255, 0, 255);
}

const char *name_of_color(COLORREF c)
{
	for (const named_color &n : COLORS)
		if (n.c == c)
			return n.name;
	return nullptr;
}

struct named_align { const char *name; UINT a; };
const named_align ALIGNS[] = {
	{ "left",       DT_LEFT   | DT_TOP     | DT_SINGLELINE },
	{ "center",     DT_CENTER | DT_TOP     | DT_SINGLELINE },
	{ "right",      DT_RIGHT  | DT_TOP     | DT_SINGLELINE },
	{ "leftmid",    DT_LEFT   | DT_VCENTER | DT_SINGLELINE },
	{ "centermid",  DT_CENTER | DT_VCENTER | DT_SINGLELINE },
	{ "leftwrap",   DT_LEFT   | DT_TOP     | DT_WORDBREAK  },
	{ "centerwrap", DT_CENTER | DT_TOP     | DT_WORDBREAK  },
};

UINT align_of(const char *name, bool &ok)
{
	ok = true;
	for (const named_align &n : ALIGNS)
		if (!std::strcmp(n.name, name))
			return n.a;
	ok = false;
	return ALIGNS[0].a;
}

const char *name_of_align(UINT a)
{
	for (const named_align &n : ALIGNS)
		if (n.a == a)
			return n.name;
	return "left";
}

// 空白で区切って取り出す。"…" でくくれば空白入りも取れる
std::vector<std::string> split(const std::string &line)
{
	std::vector<std::string> out;
	size_t i = 0;
	while (i < line.size()) {
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			i++;
		if (i >= line.size())
			break;
		if (line[i] == '"') {
			const size_t end = line.find('"', ++i);
			out.push_back(line.substr(i, end == std::string::npos ? end : end - i));
			i = (end == std::string::npos) ? line.size() : end + 1;
		} else {
			const size_t start = i;
			while (i < line.size() && line[i] != ' ' && line[i] != '\t')
				i++;
			out.push_back(line.substr(start, i - start));
		}
	}
	return out;
}

double num(const std::string &s) { return std::atof(s.c_str()); }

// #rrggbb は色。それ以外の # から後ろは覚え書きなので落とす
bool is_hex_color(const std::string &t)
{
	if (t.size() != 7 || t[0] != '#')
		return false;
	for (int i = 1; i < 7; i++)
		if (!std::isxdigit((unsigned char)t[i]))
			return false;
	return true;
}

// 丸ボタン 6 個と四角いボタン 9 個の名前。行の頭に書く
const char *MODE_NAMES[6] = { "play", "edit", "util", "effect", "sampling", "seq" };
const char *NAV_NAMES[9]  = {
	"mute_solo", "part-", "part+", "enter", "select-", "select+",
	"exit", "value-", "value+",
};
const char *ROUND_NAMES[2] = { "select", "audition" };

} // namespace


layout::layout()
{
	body_h = 385;

	lcd[0] = 240; lcd[1] = 42; lcd[2] = 439; lcd[3] = 135;

	const double cx[6] = { 288, 354, 419, 482, 545, 607 };
	const double cy[3] = { 219, 266, 310 };
	for (int i = 0; i < 6; i++) cat_x[i] = cx[i];
	for (int i = 0; i < 3; i++) cat_y[i] = cy[i];
	cat_w = 52; cat_h = 20;

	const double md[6][2] = {
		{ 752, 66 }, { 806, 66 }, { 752, 114 }, { 806, 114 }, { 752, 162 }, { 806, 162 },
	};
	for (int i = 0; i < 6; i++) { mode[i][0] = md[i][0]; mode[i][1] = md[i][1]; }
	mode_r = 11; mode_led_r = 5;

	const double nv[9][4] = {
		{ 840,  44, 48, 34 }, { 896,  44, 48, 34 }, { 952,  44, 48, 34 },
		{ 840,  90, 48, 32 }, { 896,  90, 48, 32 }, { 952,  90, 48, 32 },
		{ 840, 134, 48, 34 }, { 896, 134, 48, 34 }, { 952, 134, 48, 34 },
	};
	for (int i = 0; i < 9; i++)
		for (int k = 0; k < 4; k++) nav[i][k] = nv[i][k];

	const double rd[2][4] = { { 694, 262, 22, 22 }, { 764, 260, 22, 22 } };
	for (int i = 0; i < 2; i++)
		for (int k = 0; k < 4; k++) round_[i][k] = rd[i][k];

	dial[0] = 893; dial[1] = 268; dial[2] = 58;
	volume[0] = 141; volume[1] = 154; volume[2] = 30;
	adgain[0] = adgain[1] = adgain[2] = 0;

	// 実機の写真から測った（上の面の点の間隔が単位）
	const double lx[11] = { 0, 11.96, 30.5, 48.5, 56.1, 62.2, 70.3, 78.3, 86.1, 92.5, 102.9 };
	const double lw[11] = { 10.12, 15.64, 15, 4, 4, 7.2, 6.8, 6.7, 6.8, 8.1, 1.3 };
	for (int i = 0; i < 11; i++) { low_x[i] = lx[i]; low_w[i] = lw[i]; }

	columns_y = 186;
	modes_x = 686;
	plg[0] = 524; plg[1] = 37; plg[2] = 341;
	plg_size[0] = plg_size[1] = 12;
	labels_in_art = false;
	lcd_frame = true;

	const double cd[4] = { 57, 336, 201, 21 };
	const double ad[4] = { 8, 44, 60, 130 };
	const double ph[4] = { 198, 238, 74, 82 };   // 丸と下の札。組み込みの絵（丸 228, 269）と art/mame の絵（丸 251, 257）の両方に当たる
	for (int i = 0; i < 4; i++) { card[i] = cd[i]; adin[i] = ad[i]; phones[i] = ph[i]; }

	// ---- 飾り。実機の写真から採寸した
	bool ok = false;
	auto C = [&](const char *n) { return color_of(n, ok); };
	auto A = [&](const char *n) { return align_of(n, ok); };

	auto add_text = [&](double x, double y, double w, double h, int font,
	                    const char *align, const char *col, const char *str) {
		deco d;
		d.k = deco::text;
		d.x = x; d.y = y; d.w = w; d.h = h;
		d.font = font; d.align = A(align); d.a = C(col); d.str = str;
		decos.push_back(d);
	};
	auto add_disc = [&](double x, double y, double r, const char *face,
	                    const char *edge, double pen) {
		deco d;
		d.k = deco::disc;
		d.x = x; d.y = y; d.w = r; d.h = pen;
		d.a = C(face); d.b = C(edge);
		decos.push_back(d);
	};
	auto add_box = [&](double x, double y, double w, double h, double radius,
	                   const char *face, const char *edge) {
		deco d;
		d.k = deco::box;
		d.x = x; d.y = y; d.w = w; d.h = h; d.radius = radius;
		d.a = C(face); d.b = C(edge);
		decos.push_back(d);
	};

	add_text(10, 6, 170, 26, 1, "leftmid", "ink", "YAMAHA");
	add_text(227, 6, 320, 26, 1, "leftmid", "ink", "MU2000    TONE GENERATOR");
	add_text(744, 8, 60, 20, 0, "leftmid", "ink", "USB");

	add_disc(32,  74, 19, "jack", "jackedge", 2);
	add_disc(32, 149, 19, "jack", "jackedge", 2);
	add_text(56, 106, 130, 14, 0, "left", "ink", "1 ....  A/D INPUT");
	add_text(56, 182, 130, 14, 0, "left", "ink", "2 ....");
	add_disc(141, 78, 30, "key", "keyedge", 1);

	add_box(8, 248, 68, 36, 3, "key", "keyedge");
	add_text(4, 288, 96, 24, 0, "leftwrap", "ink", "STANDBY / ON");
	add_disc(141, 266, 34, "socket", "jackedge", 2);
	add_text(96, 304, 90, 14, 0, "center", "ink", "MIDI IN A");
	add_disc(228, 269, 14, "jack", "jackedge", 1);
	add_text(198, 304, 60, 14, 0, "center", "ink", "PHONES");
	add_box(57, 336, 201, 21, 2, "slot", "slotedge");
	add_text(63, 338, 130, 17, 0, "leftmid", "slotink", "3.3V CARD");

	add_text(686, 44, 46, 84, 0, "centerwrap", "ink", "GM2\nXG\nPLG");
	add_text(896, 28, 104, 12, 0, "center", "ink", "...... ALL ......");
}

namespace {

// panel.txt からの相対で絵を探す
std::string beside(const std::string &panel_txt, const std::string &name)
{
	if (name.find(':') != std::string::npos || name[0] == '/' || name[0] == '\\')
		return name;
	const size_t slash = panel_txt.find_last_of("/\\\\");
	if (slash == std::string::npos)
		return name;
	return panel_txt.substr(0, slash + 1) + name;
}

} // namespace

bool layout::load(const std::string &path, std::string &err)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;

	// 飾りは 1 つでも書いてあれば、既定のものは全部捨てて置き換える
	bool deco_seen = false;
	char raw[1024];
	int lineno = 0;
	auto bad = [&](const char *why) {
		char m[256];
		std::snprintf(m, sizeof(m), UI_TEXT(layout_line_error, "line %d: %s\n"), lineno, why);
		err += m;
	};

	while (std::fgets(raw, sizeof(raw), f)) {
		lineno++;
		std::string line(raw);
		// 行末の改行を落とす。付いたままだと最後の語にくっついて、
		// 色の名前などが一致しなくなる
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
			line.pop_back();
		// 覚え書きは語に切ってから落とす。**#rrggbb は色**なので残す
		auto t = split(line);
		for (size_t i = 0; i < t.size(); i++)
			if (t[i][0] == '#' && !is_hex_color(t[i])) {
				t.resize(i);
				break;
			}
		if (t.empty())
			continue;

		const std::string &key = t[0];
		auto need = [&](size_t n) {
			if (t.size() >= n)
				return true;
			bad("数が足りない");
			return false;
		};

		if (key == "art") {
			if (!deco_seen) {
				decos.clear();
				deco_seen = true;
			}
			if (!need(6)) continue;
			deco d;
			d.k = deco::art;
			d.x = num(t[1]); d.y = num(t[2]); d.w = num(t[3]); d.h = num(t[4]);
			d.str = t[5];
			const std::string full = beside(path, d.str);
			d.pic = std::make_shared<svg_art>();
			if (!d.pic->load_file(full))
				bad("絵を開けない（または読めない形）");
			else
				decos.push_back(d);
			continue;
		}
		if (key == "text" || key == "disc" || key == "box") {
			if (!deco_seen) {
				decos.clear();
				deco_seen = true;
			}
			bool ok = true;
			deco d;
			if (key == "text") {
				if (!need(9)) continue;
				d.k = deco::text;
				d.x = num(t[1]); d.y = num(t[2]); d.w = num(t[3]); d.h = num(t[4]);
				d.font = (t[5] == "label") ? 1 : 0;
				d.align = align_of(t[6].c_str(), ok);
				if (!ok) { bad("揃え方が知らない名前"); continue; }
				d.a = color_of(t[7].c_str(), ok);
				if (!ok) { bad("色が知らない名前"); continue; }
				// 書き出すときに改行を \\n にしてあるので、戻す
				for (size_t i = 0; i < t[8].size(); i++) {
					if (t[8][i] == '\\' && i + 1 < t[8].size() && t[8][i + 1] == 'n') {
						d.str += '\n';
						i++;
					} else
						d.str += t[8][i];
				}
			} else if (key == "disc") {
				if (!need(7)) continue;
				d.k = deco::disc;
				d.x = num(t[1]); d.y = num(t[2]); d.w = num(t[3]);
				d.a = color_of(t[4].c_str(), ok);
				COLORREF e = color_of(t[5].c_str(), ok);
				if (!ok) { bad("色が知らない名前"); continue; }
				d.b = e;
				d.h = num(t[6]);
			} else {
				if (!need(8)) continue;
				d.k = deco::box;
				d.x = num(t[1]); d.y = num(t[2]); d.w = num(t[3]); d.h = num(t[4]);
				d.radius = num(t[5]);
				d.a = color_of(t[6].c_str(), ok);
				COLORREF e = color_of(t[7].c_str(), ok);
				if (!ok) { bad("色が知らない名前"); continue; }
				d.b = e;
			}
			decos.push_back(d);
			continue;
		}

		if (key == "body_h")      { if (need(2)) body_h = num(t[1]); }
		else if (key == "lcd")    { if (need(5)) for (int i = 0; i < 4; i++) lcd[i] = num(t[1 + i]); }
		else if (key == "cat.x")  { if (need(7)) for (int i = 0; i < 6; i++) cat_x[i] = num(t[1 + i]); }
		else if (key == "cat.y")  { if (need(4)) for (int i = 0; i < 3; i++) cat_y[i] = num(t[1 + i]); }
		else if (key == "cat.size") { if (need(3)) { cat_w = num(t[1]); cat_h = num(t[2]); } }
		else if (key == "mode.art" || key == "nav.art" || key == "cat.art" ||
		         key == "round.art" || key == "plg.art") {
			art_set *a = (key == "mode.art")  ? &mode_art :
			             (key == "nav.art")   ? &nav_art :
			             (key == "cat.art")   ? &cat_art :
			             (key == "round.art") ? &round_art : &plg_art;
			*a = art_set();
			for (size_t i = 1; i < t.size() && i <= 3; i++) {
				if (t[i].empty())
					continue;
				auto pic = std::make_shared<svg_art>();
				if (!pic->load_file(beside(path, t[i])))
					bad("ボタンの絵を開けない（または読めない形）");
				else {
					a->pic[i - 1] = pic;
					a->path[i - 1] = t[i];
				}
			}
		}
		else if (key == "dial" || key == "volume" || key == "adgain") {
			double *v = (key == "dial") ? dial : (key == "volume") ? volume : adgain;
			if (!need(4)) continue;
			for (int i = 0; i < 3; i++) v[i] = num(t[1 + i]);
			// 4 つ目に SVG を書くと、組み込みの絵の代わりに回して描く
			std::string &pth = (key == "dial") ? dial_art_path
			                 : (key == "volume") ? volume_art_path : adgain_art_path;
			std::shared_ptr<svg_art> &pic = (key == "dial") ? dial_art
			                              : (key == "volume") ? volume_art : adgain_art;
			pth.clear();
			pic.reset();
			if (t.size() >= 5 && !t[4].empty()) {
				auto a = std::make_shared<svg_art>();
				if (!a->load_file(beside(path, t[4])))
					bad("つまみの絵を開けない（または読めない形）");
				else { pic = a; pth = t[4]; }
			}
		}
		else if (key == "mode.r") { if (need(3)) { mode_r = num(t[1]); mode_led_r = num(t[2]); } }
		else if (key == "mode.on") {
			if (!need(3)) continue;
			int at = -1;
			for (int i = 0; i < 6; i++) if (t[1] == MODE_NAMES[i]) at = i;
			if (at < 0) { bad("そんな丸ボタンは無い"); continue; }
			auto pic = std::make_shared<svg_art>();
			if (!pic->load_file(beside(path, t[2])))
				bad("ボタンの絵を開けない（または読めない形）");
			else { mode_on[at] = pic; mode_on_path[at] = t[2]; }
		}
		else if (key == "columns.y") { if (need(2)) columns_y = num(t[1]); }
		else if (key == "modes.x")   { if (need(2)) modes_x = num(t[1]); }
		else if (key == "plg.size")  { if (need(3)) { plg_size[0] = num(t[1]); plg_size[1] = num(t[2]); } }
		else if (key == "labels")    { if (need(2)) labels_in_art = (t[1] == "art"); }
		else if (key == "lcd.frame") { if (need(2)) lcd_frame = num(t[1]) != 0; }
		else if (key == "plg")    { if (need(4)) for (int i = 0; i < 3; i++) plg[i] = num(t[1 + i]); }
		else if (key == "card")   { if (need(5)) for (int i = 0; i < 4; i++) card[i] = num(t[1 + i]); }
		else if (key == "adin")   { if (need(5)) for (int i = 0; i < 4; i++) adin[i] = num(t[1 + i]); }
		else if (key == "phones") { if (need(5)) for (int i = 0; i < 4; i++) phones[i] = num(t[1 + i]); }
		else if (key == "low.x")  { if (need(12)) for (int i = 0; i < 11; i++) low_x[i] = num(t[1 + i]); }
		else if (key == "low.w")  { if (need(12)) for (int i = 0; i < 11; i++) low_w[i] = num(t[1 + i]); }
		else if (key.rfind("mode.", 0) == 0) {
			const std::string n = key.substr(5);
			int at = -1;
			for (int i = 0; i < 6; i++) if (n == MODE_NAMES[i]) at = i;
			if (at < 0) bad("そんな丸ボタンは無い");
			else if (need(3)) { mode[at][0] = num(t[1]); mode[at][1] = num(t[2]); }
		}
		else if (key.rfind("nav.", 0) == 0) {
			const std::string n = key.substr(4);
			int at = -1;
			for (int i = 0; i < 9; i++) if (n == NAV_NAMES[i]) at = i;
			if (at < 0) bad("そんな四角いボタンは無い");
			else if (need(5)) for (int k = 0; k < 4; k++) nav[at][k] = num(t[1 + k]);
		}
		else if (key.rfind("round.", 0) == 0) {
			const std::string n = key.substr(6);
			int at = -1;
			for (int i = 0; i < 2; i++) if (n == ROUND_NAMES[i]) at = i;
			if (at < 0) bad("そんな丸ボタンは無い");
			else if (need(5)) for (int k = 0; k < 4; k++) round_[at][k] = num(t[1 + k]);
		}
		else
			bad("知らない書き出し");
	}
	std::fclose(f);
	return true;
}

bool layout::save(const std::string &path) const
{
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;

	auto col = [&](COLORREF c) {
		static char buf[16];
		const char *n = name_of_color(c);
		if (n)
			return n;
		std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
		              GetRValue(c), GetGValue(c), GetBValue(c));
		return (const char *)buf;
	};

	std::fprintf(f,
		"# S-MU2000 パネルの配置\n"
		"#\n"
		"# 座標はぜんぶ論理座標（1000 x 400）。窓の大きさに合わせて一律に\n"
		"# 伸び縮みするので、窓の大きさは気にしなくてよい。\n"
		"# # から行末は覚え書き。書き方は doc/panel-editing.md。\n"
		"#\n"
		"# 直したら gui.exe の窓で F5 を押すと読み直す。\n"
		"\n"
		"body_h %g          # 本体の高さ。この下は面を切り替える帯\n"
		"lcd    %g %g %g %g   # LCD の窓  x y 幅 高さ\n"
		"\n", body_h, lcd[0], lcd[1], lcd[2], lcd[3]);

	std::fprintf(f, "# 音色カテゴリ 18 個。6 列 3 行なので列と行だけ\n");
	std::fprintf(f, "cat.x  ");
	for (int i = 0; i < 6; i++) std::fprintf(f, "%g ", cat_x[i]);
	std::fprintf(f, "\ncat.y  ");
	for (int i = 0; i < 3; i++) std::fprintf(f, "%g ", cat_y[i]);
	std::fprintf(f, "\ncat.size %g %g    # 押すところの幅と高さ\n\n", cat_w, cat_h);

	std::fprintf(f, "# 右上の丸ボタン 6 個。中心の座標\n");
	for (int i = 0; i < 6; i++)
		std::fprintf(f, "mode.%-9s %g %g\n", MODE_NAMES[i], mode[i][0], mode[i][1]);
	std::fprintf(f, "mode.r %g %g       # ボタンの半径と、中の LED の半径\n\n",
	             mode_r, mode_led_r);

	std::fprintf(f, "# 右端の四角いボタン 9 個。x y 幅 高さ\n");
	for (int i = 0; i < 9; i++)
		std::fprintf(f, "nav.%-10s %g %g %g %g\n", NAV_NAMES[i],
		             nav[i][0], nav[i][1], nav[i][2], nav[i][3]);
	std::fprintf(f, "\n# 小さい丸ボタン 2 個\n");
	for (int i = 0; i < 2; i++)
		std::fprintf(f, "round.%-8s %g %g %g %g\n", ROUND_NAMES[i],
		             round_[i][0], round_[i][1], round_[i][2], round_[i][3]);

	std::fprintf(f, "\ndial %g %g %g%s%s%s        # 大きなダイヤル  中心 x y と半径\n",
	             dial[0], dial[1], dial[2],
	             dial_art_path.empty() ? "" : " \"",
	             dial_art_path.c_str(),
	             dial_art_path.empty() ? "" : "\"");
	std::fprintf(f, "volume %g %g %g%s%s%s      # 音量つまみ  中心 x y と半径\n",
	             volume[0], volume[1], volume[2],
	             volume_art_path.empty() ? "" : " \"",
	             volume_art_path.c_str(),
	             volume_art_path.empty() ? "" : "\"");
	if (adgain[2] > 0)
		std::fprintf(f, "adgain %g %g %g%s%s%s      # A/D INPUT のつまみ  中心 x y と半径\n",
		             adgain[0], adgain[1], adgain[2],
		             adgain_art_path.empty() ? "" : " \"",
		             adgain_art_path.c_str(),
		             adgain_art_path.empty() ? "" : "\"");
	{
		// ボタンと表示灯の絵。渡されていれば書き出す
		const struct { const char *key; const art_set *a; } sets[] = {
			{ "mode.art",  &mode_art },  { "nav.art",   &nav_art },
			{ "cat.art",   &cat_art },   { "round.art", &round_art },
			{ "plg.art",   &plg_art },
		};
		for (const auto &e : sets) {
			if (!e.a->any())
				continue;
			std::fprintf(f, "%s", e.key);
			for (int i = 0; i < 3; i++)
				if (!e.a->path[i].empty())
					std::fprintf(f, " \"%s\"", e.a->path[i].c_str());
			std::fprintf(f, "\n");
		}
		for (int i = 0; i < 6; i++)
			if (!mode_on_path[i].empty())
				std::fprintf(f, "mode.on %s \"%s\"\n", MODE_NAMES[i], mode_on_path[i].c_str());
	}
	std::fprintf(f, "plg  %g %g %g      # MU / PLG-1..3 の表示灯  左端 間隔 y\n",
	             plg[0], plg[1], plg[2]);
	std::fprintf(f, "card %g %g %g %g   # カードの差し込み口。右クリックで MIDI ファイル\n",
	             card[0], card[1], card[2], card[3]);
	std::fprintf(f, "adin %g %g %g %g     # A/D INPUT のジャック\n",
	             adin[0], adin[1], adin[2], adin[3]);
	std::fprintf(f, "phones %g %g %g %g   # PHONES のジャック。押すと音の出口（デジタル / アナログ）の品書き\n",
	             phones[0], phones[1], phones[2], phones[3]);
	std::fprintf(f, "columns.y %g        # 窓の下の札（PART VOL EXP …）の高さ\n",
	             columns_y);
	std::fprintf(f, "modes.x %g          # 右の札（XG GS PERFORM）の左端。高さは液晶の ▶ に合わせる\n",
	             modes_x);
	std::fprintf(f, "plg.size %g %g      # 表示灯の絵の幅と高さ\n", plg_size[0], plg_size[1]);
	std::fprintf(f, "labels %s           # art: 印刷された札は絵に入っている（コードで書かない）\n",
	             labels_in_art ? "art" : "code");
	std::fprintf(f, "lcd.frame %d          # 0: LCD のまわりの枠は絵に入っている\n\n", lcd_frame ? 1 : 0);

	std::fprintf(f,
		"# LCD 下段の並び。単位は上段の点 1 つぶん（doc/lcd-segments.md）。\n"
		"# 並びは 01 / A01 / 楽器 / VOL / EXP / PAN / REV / CHO / VAR / KEY / モード\n"
		"low.x ");
	for (int i = 0; i < 11; i++) std::fprintf(f, "%g ", low_x[i]);
	std::fprintf(f, "\nlow.w ");
	for (int i = 0; i < 11; i++) std::fprintf(f, "%g ", low_w[i]);

	std::fprintf(f,
		"\n\n# ---- 飾り。ボタンでも LCD でもない、ただ描くだけのもの。\n"
		"# 1 つでも書くと、ここに書いたものだけになる。上から順に描く。\n"
		"#\n"
		"#   text x y 幅 高さ 書体 揃え 色 \"文字\"\n"
		"#     書体 small label\n"
		"#     揃え left center right leftmid centermid leftwrap centerwrap\n"
		"#   disc 中心x 中心y 半径 面の色 ふちの色 線の太さ\n"
		"#   box  x y 幅 高さ 角の丸み 面の色 ふちの色\n"
		"#   art  x y 幅 高さ \"絵.svg\"   SVG をそこに嵌める\n"
		"#\n"
		"# 色は ink face key keyedge keydown jack jackedge socket slot\n"
		"# slotedge slotink black white か、#rrggbb で直に。\n"
		"# 文字の中の \\n で改行（揃えを leftwrap か centerwrap に）。\n\n");

	for (const deco &d : decos) {
		if (d.k == deco::text) {
			std::string e;
			for (char c : d.str) {
				if (c == '\n') e += "\\n";
				else e += c;
			}
			std::fprintf(f, "text %g %g %g %g %s %s %s \"%s\"\n",
			             d.x, d.y, d.w, d.h, d.font ? "label" : "small",
			             name_of_align(d.align), col(d.a), e.c_str());
		} else if (d.k == deco::disc) {
			const char *fa = col(d.a);
			std::string face(fa);
			std::fprintf(f, "disc %g %g %g %s %s %g\n",
			             d.x, d.y, d.w, face.c_str(), col(d.b), d.h);
		} else if (d.k == deco::art) {
			std::fprintf(f, "art %g %g %g %g \"%s\"\n",
			             d.x, d.y, d.w, d.h, d.str.c_str());
		} else {
			const char *fa = col(d.a);
			std::string face(fa);
			std::fprintf(f, "box %g %g %g %g %g %s %s\n",
			             d.x, d.y, d.w, d.h, d.radius, face.c_str(), col(d.b));
		}
	}
	std::fclose(f);
	return true;
}

std::string layout::find_default()
{
	auto exists = [](const std::string &p) {
		FILE *f = std::fopen(p.c_str(), "rb");
		if (!f)
			return false;
		std::fclose(f);
		return true;
	};

	// 1. いまいる場所
	if (exists("panel.txt"))
		return "panel.txt";
	// 2. exe と同じ場所
	{
		const std::string dir = smu2000::exe_dir();
		if (!dir.empty()) {
			const std::string q = dir + "panel.txt";
			if (exists(q))
				return q;
		}
	}
	// 3. 設定の置き場
	{
		const std::string dir = smu2000::config_dir();
		if (!dir.empty()) {
			const std::string q = dir + "panel.txt";
			if (exists(q))
				return q;
		}
	}
	// 4. 付属の写真調の絵（art/real）。exe の横、build/ から見た上、いまいる場所
	{
		const std::string dir = smu2000::exe_dir();
		for (const std::string &q : { dir.empty() ? std::string() : dir + "art/real/panel.txt",
		                              dir.empty() ? std::string() : dir + "../art/real/panel.txt",
		                              std::string("art/real/panel.txt") })
			if (!q.empty() && exists(q))
				return q;
	}
	// 5. プラグインの束の中（S-MU2000.vst3/Contents/Resources/panel/）。
	//    ホストの exe ではなく、この関数が入っている DLL / .so の場所から探す
	{
		const std::string dir = smu2000::module_dir(reinterpret_cast<const void *>(&layout::find_default));
		if (!dir.empty()) {
			const std::string q = dir + "/../Resources/panel/panel.txt";
			if (exists(q))
				return q;
		}
	}
	return {};
}

} // namespace ui
