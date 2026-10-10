// license:BSD-3-Clause
//
// パネルの面。**実機の写真から採寸して並べ直した**。
//
// 論理座標の 1000 × 385 が本体の前面ぜんたい（実機の縦横比はおよそ 2.6:1）。
// 残りの 15 は面を切り替える帯で、本体の外。
//
//   左   A/D INPUT のジャックとつまみ、VOLUME、電源、MIDI IN A、PHONES、カード
//   中   LCD、その下に PART / BANK・PGM# / VOL / EXP / PAN / REV / CHO / VAR / KEY
//        の見出しと、音色カテゴリのボタン 18 個
//   右   PLAY EDIT / UTIL EFFECT / SAMPLING SEQ の 6 個（LED 入り）、
//        MUTE PART−+ / ENTER SELECT−+ / EXIT VALUE−+ の 9 個、
//        SELECT と AUDITION、そして**大きなダイヤル**
//
// Drawing is Dear ImGui throughout; the window only hands over a draw list.
// The one thing that changes with that: ImGui antialiases its own primitives,
// so the LCD's segment shapes go straight out as polygons. The GDI version
// had to collect them into one overlay image instead, drawing 6x oversampled
// on Windows and averaging k x k back down (that machinery is gone). What is
// left of it here is the geometry itself.

#include "panel.h"
#include "draw_imgui.h"
#include "font_file.h"
#include "tex.h"     // drop_retired_textures
#include "texts.h"

#include "imgui.h"
#include "imgui_internal.h"   // WithinFrameScope, see build_fonts

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace ui {

namespace {

constexpr double PI = 3.14159265358979;

// **位置と大きさは ui::layout（src/ui/layout.*）が持っている**。
// panel.txt があればそちらで上書きされる。ここに残してあるのは
// 「どのボタンか」「札に何と書くか」だけ

struct place { mu2000::button b; const char *label; const char *sub; };

const mu2000::button CAT_B[18] = {
	mu2000::button::piano,      mu2000::button::chrom_perc, mu2000::button::organ,
	mu2000::button::guitar,     mu2000::button::bass,       mu2000::button::strings,
	mu2000::button::ensemble,   mu2000::button::brass,      mu2000::button::reed,
	mu2000::button::pipe,       mu2000::button::synth_lead, mu2000::button::synth_pad,
	mu2000::button::synth_effects, mu2000::button::ethnic,  mu2000::button::percussive,
	mu2000::button::sfx,        mu2000::button::model_excl, mu2000::button::drum,
};
const char *CAT_LABEL[18] = {
	"Piano", "Chrom. perc.", "Organ", "Guitar", "Bass", "Strings",
	"Ensemble", "Brass", "Reed", "Pipe", "Synth lead", "Synth pad",
	"Synth effects", "Ethnic", "Percussive", "SFX", "Model excl.", "Drum",
};

// LCD の下段に並ぶもの。窓の内側の左端からの割合で置く。
// 窓の下に印刷されている札も、ここから位置を取って揃える
// 位置と幅は、上の面の**点 1 つぶん**を単位にした、窓の内側の左端からの数
// （上の面は 17 桁 × 6 点 − 1 = 101 点）。
//
// 下の面の位置と幅は実機の写真から採寸した（layout.cpp の low.x / low.w）。
// 「01」「A01」の 5 桁は字間が左から 1・2・1・1 点

// n 番のバーの左端。A1 が 0、A2 が 1、パート 1 が 2 …（点の単位）
constexpr int bar_x(int i) { return (i / 2) * (CELL_W + 1) + ((i & 1) ? 3 : 0); }
constexpr int part_x(int n) { return bar_x(n + 1); }

// 実機の窓は、上の面の左に 2.7 点、右に 5.4 点ぶんの余白がある。
// 右の余白にモードの ▶ が入る（写真から採寸。単位は上の面の点の間隔）
constexpr double LCD_LEFT = 2.7, LCD_RIGHT = 5.4;
constexpr double LCD_SPAN = LCD_LEFT + (TOP_COLS * (CELL_W + 1) - 1) + LCD_RIGHT;

// 下の面の点（「01」「A01」）は上の面の点より少し小さい。間隔は上の面の 0.92 倍。
// 下の面のセグメントの高さも、この下の面の点の間隔で測ってある
constexpr double LOW_DOT = 0.92;

// 上の面と下の面のあいだの目盛りの帯の高さ（点の単位）と、その中の並び
// （帯の上端を 0、下端を 1 とした割合）。写真から採寸した
constexpr double BAND = 7.4;
constexpr double BAND_NUM[2]  = { 0.07, 0.32 };  // パート番号 A1 A2 1-32（上は目盛りの線）
constexpr double BAND_MIC[2]  = { 0.36, 0.625 }; // MIC の箱。BANK / PGM# も同じ行
constexpr double BAND_LINE[2] = { 0.655, 0.92 }; // LINE の箱

// 点と点の隙間。実機は点の間隔の 1 割ほどしかない
constexpr double DOT_GAP = 0.10;

// 右端の ▶ の高さ（下の面の上端から、点の間隔の単位）。
// いちばん上は札のない ▶、残りが XG / GS / PERFORM
constexpr double MODE_Y[4] = { -2.2, 0.6, 3.4, 6.2 };
const char *const MODE_LABEL[3] = { "XG", "GS", "PERFORM" };

// 23 桁目の制御ビット。列 A-D は bit3-bit0、行は上の桁の 0-7 と
// 下の桁の 0-7 をつないだ 0-15。番地は実測（doc/gui.md）
enum { CA = 0, CB = 1, CC = 2, CD = 3 };
bool lcd_ctl(const snapshot &s, int col, int row)
{
	if (!s.lcd_on)
		return false;
	const u8 v = s.dots[((row / 8) * LCD_COLS + TOP_COLS + 6) * CELL_H + (row % 8)];
	return BIT(v, 3 - col) != 0;
}

// キートップの記号の絵。黒い丸に白で − ＋ ◀ ▶ を抜く。縁をぼかした
// 1 枚の絵にしておくと、小さい窓でもギザギザにならない。
// 7 セグメントや扇は多角形として書ける（ImGui がぼかしてくれる）が、
// キートップは 4-8 画素まで小さく ficando、絵にしておくのがいちばん速い。
// kind: 0 − 1 ＋ 2 ◀ 3 ▶
std::vector<uint32_t> key_symbol(int kind, double R, int &S)
{
	S = int(std::ceil(2 * R + 2));
	const double c = S / 2.0;
	const double A = 0.6 * R, T = 0.14 * R;
	auto box = [](double x, double y, double hx, double hy) {
		const double qx = std::fabs(x) - hx, qy = std::fabs(y) - hy;
		const double out = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
		return out + std::min(std::max(qx, qy), 0.0);
	};
	auto tri = [&](double x, double y) {
		if (kind == 2)
			x = -x;                                     // ◀ は ▶ を裏返したもの
		const double p[3][2] = { { A, 0 }, { -A / 2, -A }, { -A / 2, A } };
		double d = 1e9;
		int pos = 0, neg = 0;                               // 3 辺のどちら側か
		for (int i = 0; i < 3; i++) {
			const double *a = p[i], *b = p[(i + 1) % 3];
			const double ex = b[0] - a[0], ey = b[1] - a[1];
			const double wx = x - a[0], wy = y - a[1];
			const double t = std::clamp((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0, 1.0);
			d = std::min(d, std::hypot(wx - ex * t, wy - ey * t));
			(ex * wy - ey * wx < 0 ? neg : pos)++;
		}
		return (pos == 3 || neg == 3) ? -d : d;
	};
	const uint32_t ink[3] = { 62, 60, 54 }, white[3] = { 236, 234, 226 };
	std::vector<uint32_t> px(size_t(S) * S);
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			const double dx = x + 0.5 - c, dy = y + 0.5 - c;
			const double ac = std::clamp(0.5 - (std::hypot(dx, dy) - R), 0.0, 1.0);
			double ds;
			if (kind == 0)      ds = box(dx, dy, A, T);
			else if (kind == 1) ds = std::min(box(dx, dy, A, T), box(dx, dy, T, A));
			else                ds = tri(dx, dy);
			const double as = std::clamp(0.5 - ds, 0.0, 1.0);
			uint32_t v = uint32_t(std::lround(ac * 255)) << 24;
			for (int k = 0; k < 3; k++)
				v |= uint32_t(std::lround(ink[k] + (double(white[k]) - ink[k]) * as)) << (16 - 8 * k);
			px[size_t(y) * S + x] = v;
		}
	return px;
}

COLORREF mix(COLORREF a, COLORREF b, double t)
{
	auto ch = [&](int x, int y) { return int(std::lround(x + (y - x) * t)); };
	return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)),
	           ch(GetBValue(a), GetBValue(b)));
}

// UTIL > SYS の Contrast（1-8）で変わる LCD の色。**値が小さいほど濃い**。
// 工場出荷の 2 がいままでの色。1 で写真（コントラストを上げて撮ったもの）の
// 濃さ（消えている点が背景と点いた点のあいだの 35% ほど）になる。
// 3 から上は薄れていき、8 では消えている点がほぼ見えず、点いた点も半分ほど
struct lcd_ink { COLORREF dot, ghost, faint; };
lcd_ink lcd_palette(int c)
{
	const COLORREF faint2 = RGB(147, 202, 45);           // 絵の区画の消え点
	if (c == 2)
		return { LCD_DOT, LCD_GHOST, faint2 };
	c = std::clamp(c, 1, 8);
	const double ghost_a = c == 1 ? 0.35 : 0.06 * (8 - c) / 6.0;
	const double dot_a   = c == 1 ? 1.0 : 1.0 - 0.08 * (c - 2);
	return { mix(LCD_BACK, LCD_DOT, dot_a),
	         mix(LCD_BACK, LCD_DOT, ghost_a),
	         mix(LCD_BACK, LCD_DOT, ghost_a * 0.3) };
}


// 窓の下に印刷されている札。どの並びの真ん中に置くか
struct column { int at; const char *label; };
const column COLUMNS[] = {
	{ LOW_PART, "PART" }, { LOW_ICON, "BANK/PGM#" }, { LOW_VOL, "VOL" },
	{ LOW_EXP,  "EXP"  }, { LOW_PAN,  "PAN"  },       { LOW_REV, "REV" },
	{ LOW_CHO,  "CHO"  }, { LOW_VAR,  "VAR"  },       { LOW_KEY, "KEY" },
};

// 右上の 6 個。丸い押しボタンで、中に LED が入っている。
// LED の番号は MAME の mulcd.lay の並び（左列 0,2,4 / 右列 1,3,5）
struct mode_button { mu2000::button b; int led; const char *label; };
const mode_button MODES[6] = {
	{ mu2000::button::play,          0, "PLAY"     },
	{ mu2000::button::edit,          1, "EDIT"     },
	{ mu2000::button::util,          2, "UTIL"     },
	{ mu2000::button::effect,        3, "EFFECT"   },
	{ mu2000::button::sampling_mode, 4, "SAMPLING" },
	{ mu2000::button::seq,           5, "SEQ"      },
};

// 右端の 9 個
const place NAV[9] = {
	{ mu2000::button::mute_solo,    "MUTE",   "SOLO" },
	{ mu2000::button::part_minus,   "PART",   "-" },
	{ mu2000::button::part_plus,    "PART",   "+" },
	{ mu2000::button::enter,        "ENTER",  "" },
	{ mu2000::button::select_left,  "SELECT", "-" },
	{ mu2000::button::select_right, "SELECT", "+" },
	{ mu2000::button::exit,         "EXIT",   "" },
	{ mu2000::button::value_minus,  "VALUE",  "-" },
	{ mu2000::button::value_plus,   "VALUE",  "+" },
};

// 音色カテゴリの右にある小さな丸ボタン 2 つ
const place ROUND[2] = {
	{ mu2000::button::select,   "SELECT",   "" },
	{ mu2000::button::audition, "AUDITION", "" },
};

// 実機の色
const COLORREF PANEL_FACE = RGB(196, 189, 170);
const COLORREF PANEL_INK  = RGB(46, 44, 40);
const COLORREF KEY_FACE   = RGB(216, 205, 165);
const COLORREF KEY_EDGE   = RGB(126, 118, 92);
const COLORREF KEY_DOWN   = RGB(150, 140, 95);
const COLORREF KEY_INK    = RGB(62, 60, 54);

} // namespace


panel::panel()
{
	resize(LOGICAL_W, LOGICAL_H);
}

panel::~panel()
{
	drop_fonts();
}

void panel::drop_fonts() const
{
	// Only when our context is still around and current. Two ways that is not
	// so: the panel was built before any window existed (gui.cpp's app is a
	// function-local static, so its panel is constructed at exit-ish time, long
	// before a context), and the panel outlives its window (the app is
	// destroyed at exit, long after dx11_stop took the context with it).
	if (!m_font_ctx || ImGui::GetCurrentContext() != m_font_ctx)
		return;
	ImFontAtlas *atlas = ImGui::GetIO().Fonts;
	ImFont *slots[6] = { m_fonts.label, m_fonts.small, m_fonts.tiny,
	                     m_fonts.key, m_fonts.tag, m_fonts.num };
	for (ImFont *f : slots)
		if (f)
			atlas->RemoveFont(f);
	m_fonts = im::fonts{};
	for (int &px : m_font_px)
		px = 0;
	m_font_ctx = nullptr;
}

RECT panel::scale(double x, double y, double w, double h) const
{
	RECT r;
	r.left   = m_ox + int(std::lround(x * m_scale));
	r.top    = m_oy + int(std::lround(y * m_scale));
	r.right  = m_ox + int(std::lround((x + w) * m_scale));
	r.bottom = m_oy + int(std::lround((y + h) * m_scale));
	return r;
}

// パネルに描いてある MIDI IN A のジャック（丸と札）を囲む枠
RECT panel::midi_jack() const
{
	return scale(92, 230, 96, 90);
}

bool panel::on_midi_jack(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = midi_jack();
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_card_slot(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = scale(m_lay.card[0], m_lay.card[1], m_lay.card[2], m_lay.card[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_ad_input(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = scale(m_lay.adin[0], m_lay.adin[1], m_lay.adin[2], m_lay.adin[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_phones(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = scale(m_lay.phones[0], m_lay.phones[1], m_lay.phones[2], m_lay.phones[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

bool panel::on_power(int x, int y) const
{
	if (m_page != page::front || m_lay.power[2] <= 0)
		return false;
	const RECT r = scale(m_lay.power[0], m_lay.power[1], m_lay.power[2], m_lay.power[3]);
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// 論理座標の点を実座標へ
POINT panel::at(double x, double y) const
{
	POINT p;
	p.x = m_ox + int(std::lround(x * m_scale));
	p.y = m_oy + int(std::lround(y * m_scale));
	return p;
}

// ---- Fonts. Every size is rounded to whole pixels: ImGui rasterizes per
// size, so leaving fractions in would rebuild the set on every resize tick.
// The six sizes come from the window scale and the LCD's own dimensions, and
// nothing is touched unless one of them actually changed.
void panel::build_fonts() const
{
	ImGuiContext *ctx = ImGui::GetCurrentContext();
	if (!ctx)
		return;                             // no context yet: nothing draws
	// **Not while a frame is open.** Adding or removing a font makes the atlas
	// repack, and when it needs a bigger texture it makes a new one and marks
	// the old for destruction at the next NewFrame. ImGui remaps the draw lists
	// for that, but only their *pending* command header -- commands already
	// flushed into the buffer keep the old texture, and they are read at the
	// next Render, by which point that texture is gone. That is the Metal
	// window's "ImDrawCmd is referring to ImTextureData that wasn't uploaded"
	// assert, and it needs a resize to set off (a resize is the only thing that
	// moves the sizes). So the hosts build the fonts right after they create
	// the context, and a resize rebuilds them between frames, where this is
	// safe; a call that lands mid-frame is dropped and the next one gets it.
	if (ctx->WithinFrameScope)
		return;
	ImFontAtlas *atlas = ImGui::GetIO().Fonts;

	// The em GDI asked for, its way. make_font() truncated px * m_scale down
	// and floored the result, and the floor is what carries small sizes: at
	// 600 px wide GDI held small at 7 where rounding gave 5, a third less text
	// on the labels that make up most of the panel. Rounding *up* is just as
	// wrong the other way -- 8.5 * 0.935 is 8, and int() gives the 7 GDI drew.
	auto gdi_em = [this](double px, int floor_px) {
		return std::max(floor_px, int(px * m_scale));
	};
	// キートップの記号。中に「SELECT」が 6 文字入る太字
	const int key_px = std::max(6, int(std::lround(9.0 * m_scale)));
	// _tick 番号用。34 個の番号をバーの真下に並べるので、思い切り小さくする
	const int tiny_px  = gdi_em(6.5, 5);
	const int label_px = gdi_em(13.0, 7);
	const int small_px = gdi_em(8.5, 7);
	// LCD のMIC / LINE の札用。箱の高さから決めるので LCD の寸法しだい
	const lcd_geom g = lcd_grid();
	const int num_px = std::max(4, int(std::lround(g.line_h * 1.0)));  // lround here
	RECT box[2];
	lcd_tag_boxes(g, box);
	const int bw = box[0].right - box[0].left, bh = box[0].bottom - box[0].top;
	// 写真では大文字の高さが箱の 75%、「MIC」の幅が箱の 71%
	const int tag_px = std::max(4, int(std::min(1.07 * bh, 0.40 * bw)));  // truncates

	// The ems above are GDI's, but the face is not: GDI asked for Segoe UI
	// and Arial, and this is one CJK family per weight (font_file.h). Hiragino
	// Sans caps stand at 78% of its em where Segoe UI's are 70%, so the same
	// em here draws about a ninth taller, and shaped differently besides.
	// cjk_face_em() does not cover that -- it only puts stb's hhea span back
	// on em terms, and Hiragino's span already is its em. Left alone on
	// purpose: panel.txt states its sizes in ems, so matching the em is what
	// keeps the layout where it was authored. A per-face cap-height factor
	// would buy the last few percent against the GDI screenshots at the cost
	// of a knob tuned to one reference typeface.

	const int want[6] = { label_px, small_px, tiny_px, key_px, tag_px, num_px };
	if (m_font_ctx == ctx && m_font_px[0] != 0) {
		bool same = true;
		for (int i = 0; i < 6; i++)
			same = same && m_font_px[i] == want[i];
		if (same)
			return;                         // 設定しだいが同じなら作り直さない
	}

	// The sizes moved, so take our six out of the atlas first (only ours)
	if (m_font_ctx == ctx)
		drop_fonts();

	// Two faces, not one. The button legends and the key-top printing want a
	// bold, the LCD's small sizes do not, and Dear ImGui's TTF loader has no
	// weight axis -- so the bold is a second face from the system rather than a
	// faked one: striking the string again a fraction of a pixel off does
	// thicken the stems, but it closes the counters and blurs. Slots 0 (label)
	// and 3 (key) are the two that take it; the other four stay regular, and a
	// machine with no bold to be had draws the regular face there too.
	//
	// Sizes are stb pixels times the em factor: stb sizes by the hhea span
	// while GDI maps to the em, so without it everything lands small on faces
	// whose hhea carries line spacing (about 25% on Yu Gothic UI, 1.0
	// elsewhere -- macOS and the editors never move).
	const float em = cjk_face_em(false), bem = cjk_face_em(true);
	ImFont *slot[6] = { add_cjk_font(atlas, label_px * bem, true), add_cjk_font(atlas, small_px * em),
	                    add_cjk_font(atlas, tiny_px * em),     add_cjk_font(atlas, key_px * bem, true),
	                    add_cjk_font(atlas, tag_px * em),      add_cjk_font(atlas, num_px * em) };
	m_fonts.label = slot[0]; m_fonts.small = slot[1]; m_fonts.tiny = slot[2];
	m_fonts.key   = slot[3]; m_fonts.tag  = slot[4]; m_fonts.num  = slot[5];
	m_fonts.label_px = float(label_px) * bem;
	m_fonts.small_px = float(small_px) * em;
	m_fonts.tiny_px  = float(tiny_px) * em;
	m_fonts.key_px   = float(key_px) * bem;
	m_fonts.tag_px   = float(tag_px) * em;
	m_fonts.num_px   = float(num_px) * em;
	for (int i = 0; i < 6; i++)
		m_font_px[i] = want[i];
	m_font_ctx = ctx;
}

// 描画の文脈ができた直後に呼ばれる（窓を作ったとき。プラグインは画面を付けるたび）。
// **前の文字は必ず忘れてから作る**: 覚えているのは「どの文脈に・どの大きさで足したか」で、文脈は番地で
// 見分けている。プラグインの画面を外してすぐ付け直すと、作り直した文脈が**同じ番地**に載ることがあり、
// そのままだと「同じ文脈・同じ大きさ」と見て、消えた文脈の文字を使い回す（FL Studio がエフェクトとして
// 読むときの attached → removed → attached で、最初の描画が落ちていた。issue #131）。
// ここへ来るときの文脈は新しく、前の 6 つは入っていないので、外す（drop_fonts）のではなく忘れるだけ
void panel::fonts_ready()
{
	m_fonts = im::fonts{};
	for (int &px : m_font_px)
		px = 0;
	m_font_ctx = nullptr;
	build_fonts();
}

void panel::resize(int w, int h)
{
	m_w = std::max(w, 200);
	m_h = std::max(h, 60);
	// **帯のぶんを差し引いてから合わせる**。当たりも描きも
	// `scale()` / `at()` を通るので、ここだけ直せば全部ついてくる
	const int body_h = std::max(m_h - m_top_inset, 60);

	if (m_lcd_only) {
		m_scale = std::min(double(m_w) / m_lay.lcd[2], double(body_h) / m_lay.lcd[3]);
		m_ox = int(-m_lay.lcd[0] * m_scale);
		m_oy = m_top_inset - int(m_lay.lcd[1] * m_scale);
	} else {
		m_scale = std::min(double(m_w) / LOGICAL_W, double(body_h) / LOGICAL_H);
		m_ox = int((m_w - LOGICAL_W * m_scale) / 2);
		m_oy = m_top_inset + int((body_h - LOGICAL_H * m_scale) / 2);
	}

	m_lcd    = scale(m_lay.lcd[0], m_lay.lcd[1], m_lay.lcd[2], m_lay.lcd[3]);
	if (m_lcd_only)
		m_lcd = RECT{ 0, m_top_inset, m_w, m_h };
	m_volume = scale(m_lay.volume[0] - m_lay.volume[2], m_lay.volume[1] - m_lay.volume[2],
	                 m_lay.volume[2] * 2, m_lay.volume[2] * 2);   // 当たりは丸で見る
	m_adgain = scale(m_lay.adgain[0] - m_lay.adgain[2], m_lay.adgain[1] - m_lay.adgain[2],
	                 m_lay.adgain[2] * 2, m_lay.adgain[2] * 2);
	m_status = scale(20, 372, 700, 13);
	m_wheel  = scale(m_lay.dial[0] - m_lay.dial[2], m_lay.dial[1] - m_lay.dial[2],
	                 m_lay.dial[2] * 2, m_lay.dial[2] * 2);
	for (int i = 0; i < 6; i++)
		m_leds[i] = scale(m_lay.mode[i][0] - m_lay.mode_r, m_lay.mode[i][1] - m_lay.mode_r,
		                  m_lay.mode_r * 2, m_lay.mode_r * 2);

	build_fonts();

	// キートップの記号（− ＋ ◀ ▶）。大きさが変わるときだけ作り直す
	{
		const int S = std::max(6, int(std::ceil(2 * std::max(3.0, 4.2 * m_scale) + 2)));
		if (S != m_key_sym_px) {
			for (int k = 0; k < 4; k++) {
				int size = 0;
				const std::vector<uint32_t> px =
					key_symbol(k, std::max(3.0, 4.2 * m_scale), size);
				m_key_sym[k] = std::make_shared<svg_art>();
				m_key_sym[k]->load_pixels(size, size, px);
			}
			m_key_sym_px = S;
		}
	}

	build_spots();
}

// 触れる場所は面ごとに違う。掴んでいる途中に作り直すと迷子になるので離す
void panel::build_spots()
{
	m_held = nullptr;
	m_spots.clear();
	if (m_lcd_only)
		return;

	// 面を選ぶつまみ。本体の外（下の帯）
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FRONT,
	                    scale(700, 386, 94, 13), UI_TEXT(tab_panel, "Panel"), "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_EDIT,
	                    scale(800, 386, 94, 13), UI_TEXT(tab_editor, "Editor"), "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FX,
	                    scale(898, 386, 94, 13), UI_TEXT(tab_effects, "Effects"), "" });

	if (m_page == page::editor) { build_editor_spots(); return; }
	if (m_page == page::effects) { build_effect_spots(); return; }

	for (int i = 0; i < 18; i++)
		m_spots.push_back({ spot_kind::button, CAT_B[i], CTL_NONE,
		                    scale(m_lay.cat_x[i % 6] - m_lay.cat_w / 2, m_lay.cat_y[i / 6],
		                          m_lay.cat_w, m_lay.cat_h),
		                    CAT_LABEL[i], "" });
	for (int i = 0; i < 6; i++)
		m_spots.push_back({ spot_kind::button, MODES[i].b, CTL_NONE,
		                    scale(m_lay.mode[i][0] - m_lay.mode_r,
		                          m_lay.mode[i][1] - m_lay.mode_r,
		                          m_lay.mode_r * 2, m_lay.mode_r * 2),
		                    MODES[i].label, "" });
	for (int i = 0; i < 9; i++)
		m_spots.push_back({ spot_kind::button, NAV[i].b, CTL_NONE,
		                    scale(m_lay.nav[i][0], m_lay.nav[i][1],
		                          m_lay.nav[i][2], m_lay.nav[i][3]),
		                    NAV[i].label, NAV[i].sub });
	for (int i = 0; i < 2; i++)
		m_spots.push_back({ spot_kind::button, ROUND[i].b, CTL_NONE,
		                    scale(m_lay.round_[i][0] - m_lay.round_[i][2] / 2,
		                          m_lay.round_[i][1] - m_lay.round_[i][3] / 2,
		                          m_lay.round_[i][2], m_lay.round_[i][3]),
		                    ROUND[i].label, ROUND[i].sub });

	m_spots.push_back({ spot_kind::wheel,  mu2000::button::count, CTL_NONE, m_wheel, "", "" });
	m_spots.push_back({ spot_kind::volume, mu2000::button::count, CTL_NONE, m_volume,
	                    "VOLUME", "" });
	if (m_lay.adgain[2] > 0)
		m_spots.push_back({ spot_kind::adgain, mu2000::button::count, CTL_NONE, m_adgain,
		                    "A/D INPUT", "" });
}

const spot *panel::hit(int x, int y) const
{
	for (const spot &s : m_spots) {
		if (x >= s.r.left && x < s.r.right && y >= s.r.top && y < s.r.bottom) {
			// 丸いものは丸の中だけ
			if (s.kind == spot_kind::wheel || s.kind == spot_kind::volume ||
			    s.kind == spot_kind::adgain) {
				const double cx = (s.r.left + s.r.right) * 0.5;
				const double cy = (s.r.top + s.r.bottom) * 0.5;
				const double rr = (s.r.right - s.r.left) * 0.5;
				if (std::hypot(x - cx, y - cy) > rr)
					continue;
			}
			return &s;
		}
	}
	return nullptr;
}


void panel::draw_tabs(ImDrawList *dl) const
{
	for (const spot &sp : m_spots) {
		if (sp.kind != spot_kind::tab)
			continue;
		const bool on = (sp.ctl == CTL_TAB_EDIT   && m_page == page::editor) ||
		                (sp.ctl == CTL_TAB_FX     && m_page == page::effects) ||
		                (sp.ctl == CTL_TAB_FRONT  && m_page == page::front);
		im::round_box(dl, sp.r, on ? RGB(70, 76, 84) : RGB(38, 41, 46),
		              on ? ACCENT : RGB(70, 74, 80), float(int(4 * m_scale)));
		im::text_in(dl, im::pos_of(sp.r), im::size_of(sp.r), sp.label,
		            on ? TEXT : TEXT_DIM, m_fonts.small, m_fonts.small_px, true, true, false);
	}
}

// ---- LCD

// The dot pitch and where everything sits inside the LCD window. The pitch is
// fractional: a dot is a few pixels, and rounding it down to whole pixels
// leaves a fifth of the window empty on the sizes you get at startup (GDI
// could not draw a fractional rect, so it drew 6x oversampled and averaged --
// ImGui antialiases, so it just draws the fraction).
panel::lcd_geom panel::lcd_grid() const
{
	lcd_geom g{};
	const int aw = m_lcd.right - m_lcd.left, ah = m_lcd.bottom - m_lcd.top;
	g.pad = std::max(1, int(1 * m_scale));

	const double fit = std::min(aw / LCD_SPAN, double(ah - g.pad * 2) / (24 + BAND));
	const int d = std::max(1, int(fit));

	// 拡大して描く倍率。点の格子を端数の大きさで描くために、点の間隔が
	// 12 画素ほどになるまで整数倍にしてから平均を取る
	//
	// 端数にできると、3.8 画素入る窓が 3 画素に丸められず、LCD の中に空く
	// 2 割の余白が消える（起動直後の大きさがちょうどそうだった）。reference は
	// GDI が図形の縁をぼかせないのでこうなったが、こちらは同じことをできる。
	// 使える大きさを k 倍に描いてから k × k で割る（supersample_lcd）
	g.k = std::min(6, (12 + d - 1) / d);
	double df = d;
	if (g.k > 1)
		df = std::max(double(d), std::floor(fit * g.k) / g.k);
	g.df = df;
	g.d = int(std::lround(df));

	// 目盛りの帯。中の並びは band_y() の割合で決まる
	g.scale_h = std::max(8, int(std::lround(BAND * df)));
	g.tick_h  = std::max(1, int(std::lround(BAND_NUM[0] * g.scale_h)));
	g.line_h  = std::max(3, int(std::lround((BAND_NUM[1] - BAND_NUM[0]) * g.scale_h)));

	// 点は正方形のままにして、余った幅は左右に振り分ける。端数は 1/k 画素に
	// 丸める（拡大した絵の画素の境目に乗るように）
	auto q = [&](double v) { return std::round(v * g.k) / g.k; };
	g.fx0 = q(m_lcd.left + std::max(0.0, (aw - LCD_SPAN * df) / 2) + LCD_LEFT * df);
	g.fy0 = q(m_lcd.top + std::max(double(g.pad), (ah - (24 * df + g.scale_h)) / 2));
	g.fsy = g.fy0 + 16 * df + g.scale_h;
	g.x0 = int(std::lround(g.fx0));
	g.y0 = int(std::lround(g.fy0));
	g.sy = int(std::lround(g.fsy));
	return g;
}

int panel::band_y(const lcd_geom &g, double f) const
{
	return int(std::lround(g.fy0 + 16 * g.df + f * g.scale_h));
}

void panel::lcd_tag_boxes(const lcd_geom &g, RECT out[2]) const
{
	// 横は A1 A2 のマス（5.6 点ぶん）。縦は目盛りの帯の中の決まった割合
	const double d = g.df;
	const int l = int(std::lround(g.fx0 - 0.1 * d));
	const int r = int(std::lround(g.fx0 + 5.5 * d));
	out[0] = RECT{ l, band_y(g, BAND_MIC[0]),  r, band_y(g, BAND_MIC[1]) };
	out[1] = RECT{ l, band_y(g, BAND_LINE[0]), r, band_y(g, BAND_LINE[1]) };
}

// Where the LCD's fills go. The dot grid and the fixed segments are whole
// pixels, so they can go either straight into the draw list (k == 1) or into a
// k-times buffer we average back down. The polygons are the exception -- they
// are the parts that stay smooth at 1:1 -- so those always go into the draw
// list, already divided by k and moved onto the screen.
struct panel::lcd_canvas {
	ImDrawList           *dl = nullptr;   // polygons always; fills too at 1:1
	std::vector<uint32_t> px;            // fills, when magnifying
	int                   w = 0, h = 0;   // px is w * h, 0xFFRRGGBB
	double                sk = 1.0;       // k-space -> screen, for the polygons
	double                ox = 0, oy = 0;

	bool magnified() const { return !px.empty(); }

	// GDI's FillRect: whole pixels, [l,r) x [t,b), clipped to the target
	void rect(int l, int t, int r, int b, COLORREF c)
	{
		if (!magnified()) {
			im::fill(dl, ImVec2(float(l), float(t)),
			         ImVec2(float(r - l), float(b - t)), c);
			return;
		}
		l = std::max(l, 0);
		t = std::max(t, 0);
		r = std::min(r, w);
		b = std::min(b, h);
		// Wholly outside. Each edge is clipped on its own, so a rect past the
		// right edge comes out with l > r, and std::fill from a first beyond
		// its last does not stop: it writes on through the heap. A minimized
		// window gets there -- the LCD is a few dozen pixels then, and the
		// dot grid (a pitch of at least one pixel) no longer fits inside it
		if (l >= r || t >= b)
			return;
		const uint32_t v = 0xff000000u | (uint32_t(GetRValue(c)) << 16) |
		                   (uint32_t(GetGValue(c)) << 8) | uint32_t(GetBValue(c));
		for (int y = t; y < b; y++)
			std::fill(px.begin() + size_t(y) * w + l, px.begin() + size_t(y) * w + r, v);
	}

	// 曲線は等倍で描くので、ここでは溜めておく。拡大した絵を貼り終えてから
	// 重ねる（先に描くと、貼った絵に覆盖されてしまう）
	struct shape {
		COLORREF        ink;
		std::vector<ImVec2> pts;
	};
	std::vector<shape> shapes;
	void flush_shapes()
	{
		for (const shape &sh : shapes)
			dl->AddConcavePolyFilled(sh.pts.data(), int(sh.pts.size()), im::col(sh.ink));
		shapes.clear();
	}
};

// Render the LCD at k times its size and average each k x k block back down.
//
// This is the reference's own trick, and it is what makes a dot pitch finer
// than a pixel expressible. GDI cannot soften the edge of a fractional
// rectangle at all, so the pitch has to become a whole number of k-pixels, and
// 1/k of a pixel is the finest that gives. ImGui softens edges, but only by a
// fixed half pixel -- too coarse for a gap of DOT_GAP * d, which is a fraction
// of a pixel, and it rounds dot widths to whole pixels as it goes. Drawing at k
// times and averaging has neither problem, and it is the same arithmetic the
// reference uses rather than an emulation of it.
//
// Returns false when there is no context or no texture, and the caller falls
// back to drawing at 1:1.
bool panel::supersample_lcd(ImDrawList *dl, const snapshot &s, const lcd_geom &g,
                            int k) const
{
	const int w = m_lcd.right - m_lcd.left, h = m_lcd.bottom - m_lcd.top;
	if (k <= 1 || w <= 0 || h <= 0)
		return false;

	lcd_canvas big;
	big.dl = dl;
	m_lcd_big.assign(size_t(w) * k * size_t(h) * k, 0xff000000u | (uint32_t(LCD_BACK) & 0xffffffu));
	big.px.swap(m_lcd_big);
	big.w = w * k;
	big.h = h * k;
	// The polygons come out of draw_lcd_body already on the screen, so the
	// mapping has to be set before it runs
	big.sk = 1.0 / k;
	big.ox = m_lcd.left;
	big.oy = m_lcd.top;

	// 拡大した座標で描く。原点は LCD の左上
	lcd_geom gk = g;
	gk.d = int(std::lround(g.df * k));
	gk.pad = g.pad * k;
	gk.x0 = int(std::lround((g.fx0 - m_lcd.left) * k));
	gk.y0 = int(std::lround((g.fy0 - m_lcd.top) * k));
	gk.sy = gk.y0 + 16 * gk.d + g.scale_h * k;
	gk.tick_h = g.tick_h * k;
	gk.line_h = g.line_h * k;
	gk.scale_h = g.scale_h * k;
	draw_lcd_body(big, s, gk, RECT{ 0, 0, big.w, big.h }, double(k));

	// k × k の平均
	m_lcd_flat.resize(size_t(w) * h);
	uint32_t *flat = m_lcd_flat.data();
	const int n = k * k;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			unsigned r = 0, gg = 0, b = 0;
			for (int yy = 0; yy < k; yy++) {
				const uint32_t *row = big.px.data() + size_t(y * k + yy) * big.w + size_t(x) * k;
				for (int xx = 0; xx < k; xx++) {
					b  += row[xx] & 0xff;
					gg += (row[xx] >> 8) & 0xff;
					r  += (row[xx] >> 16) & 0xff;
				}
			}
			flat[size_t(y) * w + x] = 0xff000000u | (((r + n / 2) / n) << 16) |
			                          (((gg + n / 2) / n) << 8) | ((b + n / 2) / n);
		}

	if (!m_lcd_tex)
		m_lcd_tex.reset(new im::tex);
	// The size follows the window, so it has to be part of the test: a texture
	// from before the resize would be stretched over the new rectangle and draw
	// the old picture in the wrong place (svg.cpp's m_cache does the same).
	const bool ok = m_lcd_tex->valid() && m_lcd_tex->width() == w && m_lcd_tex->height() == h
	                    ? m_lcd_tex->refresh(m_lcd_flat)
	                    : m_lcd_tex->upload(w, h, m_lcd_flat);
	if (!ok || !m_lcd_tex->valid())
		return false;
	dl->AddImage(m_lcd_tex->ref(), ImVec2(float(m_lcd.left), float(m_lcd.top)),
	             ImVec2(float(m_lcd.right), float(m_lcd.bottom)));
	// セグメントは描き上がった LCD の上に、画面の画素で縁をぼかして塗る。
	// 窓の大きさや拡大の有無によらず、円弧や斜めの線がギザギザにならない
	big.flush_shapes();
	m_lcd_big.swap(big.px);   // keep it for the next frame
	return true;
}

void panel::draw_lcd(ImDrawList *dl, const snapshot &s) const
{
	if (!m_lcd_only && m_lay.lcd_frame) {
		RECT bez = m_lcd;
		const int inflate = int(5 * m_scale);
		bez.left -= inflate; bez.top -= inflate;
		bez.right += inflate; bez.bottom += inflate;
		im::round_box(dl, bez, RGB(60, 58, 52), RGB(110, 106, 96), float(int(5 * m_scale)));
	}
	const lcd_geom g = lcd_grid();

	// セグメントは描き上がった LCD の上に、画面の画素で縁をぼかして塗る。
	// 窓の大きさや拡大の有無によらず、円弧や斜めの線がギザギザにならない
	if (g.k > 1 && supersample_lcd(dl, s, g, g.k)) {
		// the average and the segments are both on the screen now
	} else {
		lcd_canvas cv;
		cv.dl = dl;
		draw_lcd_body(cv, s, g, m_lcd, 1.0);
		cv.flush_shapes();
	}
	draw_lcd_labels(dl, s, g);
	draw_lcd_message(dl, s);
}

void panel::draw_lcd_message(ImDrawList *dl, const snapshot &s) const
{
	if (!s.message[0])
		return;
	im::fill(dl, m_lcd, RGB(24, 26, 22));
	im::text_in(dl, im::pos_of(m_lcd), im::size_of(m_lcd), s.message,
	            RGB(210, 220, 200), m_fonts.label, m_fonts.label_px, true, true, true);
}

void panel::draw_lcd_labels(ImDrawList *dl, const snapshot &s, const lcd_geom &g) const
{
	const lcd_ink pal = lcd_palette(s.contrast);
	const COLORREF LCD_ON = pal.dot, LCD_OFF = pal.ghost;
	// 札は等倍で描くので、端数つきの寸法から画素に丸める
	const double d = g.df, fx0 = g.fx0;
	const int tick_h = int(std::lround(g.tick_h)), line_h = int(std::lround(g.line_h));
	const int scale_y = int(std::lround(g.fy0 + 16 * d));
	// 目盛りの数字と BANK/PGM# は 1 画素ぶん下げる。reference の字より 1 画素
	// ぶん上にずれるため。大きさを求める式は reference と同じで、面板の
	// ほかの字（label / small / key）はずれない。
	//
	// ただしこれは 8 画素くらいの大きさでの差で、窓の大きさによって札の
	// 高さも変わる（Windows では 7 画素になる窓と 8 画素になる窓がある）。
	// MIC/LINE に同じ補正を入れると 1 画素ぶんの overshoot になった。
	// tag と num で差の向きが違うので、ずれた分だけ数字を足すのはやめて、
	// MIC/LINE は補正なし（下のコメント）
	const int LABEL_DY = 1;
	auto px = [](double v) { return int(std::lround(v)); };
	auto ctl = [&](int col, int row) { return lcd_ctl(s, col, row); };

	// ---- 目盛りの帯。ここも**印刷ではなくセグメント**で、点いたり消えたりする
	{
		const bool on_scale = ctl(CD, 4);          // 「1」-「32」
		const bool on_a1a2  = ctl(CD, 3);          // 「A1」「A2」
		const COLORREF ink_scale = on_scale ? LCD_ON : LCD_OFF;
		const COLORREF ink_a1a2  = on_a1a2  ? LCD_ON : LCD_OFF;

		for (int i = 0; i < TOP_COLS * 2; i++) {
			const int col = i / 2;
			// バーは 2 点ぶんの幅。番号と線はその真ん中（2 点目の右の隙間は除く）
			const int bx  = px(fx0 + col * (CELL_W + 1) * d + ((i & 1) ? 3 * d : 0)
			                   + d * (1.0 - DOT_GAP / 2));
			if (i >= 2 || on_a1a2)
				im::line(dl, bx, scale_y, bx, scale_y + tick_h,
				         on_scale ? LCD_ON : LCD_OFF, 1.0f);
			// 番号はパートの番号。**そのバーの真下**に置く
			const int part = i - 1;
			char n[8];
			std::snprintf(n, sizeof(n), i < 2 ? "A%d" : "%d", i < 2 ? i + 1 : part);
			RECT t{ bx - px(1.5 * d), scale_y + tick_h + LABEL_DY,
			        bx + px(1.5 * d), scale_y + tick_h + line_h + LABEL_DY };
			im::text_cap(dl, t, n, i < 2 ? ink_a1a2 : ink_scale,
			             m_fonts.num, m_fonts.num_px, true, true);
		}

		// MIC と LINE は左端に上下に並ぶ。実機は**黒い箱に白抜き**の字で、
		// 消えているときは箱がうっすら見え、字はそれより少し明るい
		{
			RECT box[2];
			lcd_tag_boxes(g, box);
			const char *name[2] = { "MIC", "LINE" };
			const bool on[2] = { ctl(CD, 1), ctl(CD, 2) };
			const double rad = std::max(2, int(std::lround(0.5 * d)));
			for (int k = 0; k < 2; k++) {
				const COLORREF face = on[k] ? LCD_ON : LCD_OFF;
				const COLORREF ink  = on[k] ? LCD_BACK : mix(LCD_OFF, LCD_BACK, 0.6);
				im::round_box(dl, box[k], face, face, float(rad));
				// 上下に動かさない。reference も DT_VCENTER を使わず、箱の
				// 真ん中に大文字の見える部分を合わせているので、ここも cap を
				// 真ん中に置く形が同じ。札の大きさを求める式も reference と
				// 同じで、残る差は書体の ascent と cap の割合（reference は
				// Segoe UI、こちらは別の書体）。目盛りの数字に足している
				// 補正をここにも入れると 1 画素ぶん overshoot になるので入れない
				im::text_cap(dl, box[k], name[k], ink, m_fonts.tag, m_fonts.tag_px, true);
			}
		}

		// BANK と PGM# は 2 つずつあり、パート番号の下に並んでいる。
		// 左側の組が D0、右側の組が D5 で点け消しされる
		struct { int part; const char *label; bool right; } marks[] = {
			{  3, "BANK", false }, { 11, "PGM#", false },
			{ 19, "BANK", true  }, { 27, "PGM#", true  },
		};
		for (const auto &mk : marks) {
			const int cx = px(fx0 + (part_x(mk.part) + part_x(mk.part + 1) + 2) * d / 2);
			const int w = int(26 * m_scale);
			RECT r{ cx - w / 2, band_y(g, BAND_MIC[0]) + LABEL_DY,
			       cx + w / 2, band_y(g, BAND_MIC[1]) + LABEL_DY };
			// DT_VCENTER と同じ（行の高さの真ん中に置く）
			im::text_cap(dl, r, mk.label,
			             ctl(CD, mk.right ? 5 : 0) ? LCD_ON : LCD_OFF,
			             m_fonts.tag, m_fonts.tag_px, true, true);
		}
	}
}

// 点の間隔は gk.d。点はすべて整数座標の四角形になる：reference も同じことを
// しており、拡大してから平均するなら途中の整数が同じでないと縮んだ結果も違う
void panel::draw_lcd_body(lcd_canvas &cv, const snapshot &s, const lcd_geom &g,
                          const RECT &area, double px) const
{
	cv.rect(area.left, area.top, area.right, area.bottom, LCD_BACK);
	// 色はコントラストしだい。ここから下の LCD_ON / LCD_OFF はこの色
	const lcd_ink pal = lcd_palette(s.contrast);
	const COLORREF LCD_ON = pal.dot, LCD_OFF = pal.ghost;

	// 実機の窓は、DDRAM の桁がそのまま横一列に並んでいるのではない。
	// ボタンを押して確かめた割り振りは（doc/gui.md）
	//
	//   上の面（点の並び。2 行、桁のあいだは 1 点、**行のあいだは空けない**）
	//     0-8   レベルメータ。1 マス 2 本で 18 本（A1 A2 と 1-16）
	//     9-16  文字 8 桁。1 行目が音色名、2 行目が ▶000◀001
	//   下の面
	//     行 0 の 17-18   部の番号「01」
	//     行 1 の 17-19   「A01」
	//     20-22（両行）   楽器のかたち。**点が細かく、正方形でもない**
	//     **23（両行）は絵ではない**。決まった形のセグメントを点けたり
	//     消したりする 64 個のビットが入っている（下の ctl）
	const int d = g.d, x0 = g.x0, y0 = g.y0;

	const COLORREF FAINT = pal.faint;                      // 絵の区画の消え点

	// 点 1 つ。w × h は点の間隔で、右と下に DOT_GAP ぶんの隙間を空ける。
	// 隙間が 1 画素に満たないときは、その 1 画素を背景と混ぜた色で塗る。
	// 小さい窓で隙間が丸ごと 1 画素になり、文字が薄く見えていたのを防ぐ
	const double gap = DOT_GAP * d;
	auto dotbox = [&](int l, int t, int w, int h, COLORREF ink, double gp = -1.0) {
		if (gp < 0)
			gp = gap;
		const int gi = int(gp);
		const double fr = gp - gi;
		const bool part = fr > 0.05;
		const int sw = w - gi - (part ? 1 : 0), sh = h - gi - (part ? 1 : 0);
		// 小さすぎる。隙間なしで塗る
		if (sw < 1 || sh < 1) {
			cv.rect(l, t, l + std::max(1, w), t + std::max(1, h), ink);
			return;
		}
		cv.rect(l, t, l + sw, t + sh, ink);
		if (!part)
			return;
		// 隙間が 1 画素に満たないときは、その 1 画素を背景と混ぜた色で塗る
		const COLORREF edge = mix(ink, LCD_BACK, fr);
		cv.rect(l + sw, t, l + sw + 1, t + sh, edge);
		cv.rect(l, t + sh, l + sw, t + sh + 1, edge);
		cv.rect(l + sw, t + sh, l + sw + 1, t + sh + 1,
		        mix(ink, LCD_BACK, 1.0 - (1.0 - fr) * (1.0 - fr)));
	};


	// 角度は真上が 0 度で時計回り
	struct pt { double x, y; };
	auto polar = [](double cx, double cy, double r, double deg) {
		const double a = deg * PI / 180.0;
		return pt{ cx + r * std::sin(a), cy - r * std::cos(a) };
	};
	// 帯状の弧（中心 cx cy、半径 r0-r1、a0 度から a1 度）。端は半径の向きに切る
	// An elliptical band, so it goes out as a polygon: ImGui's arcs are
	// circular, and the LCD wants an ellipse (the fans, the pan arc, the needles)
	//
	// **Both of these are concave** -- a band and a ring are, by definition --
	// so they have to go through the concave filler. AddConvexPolyFilled draws
	// the convex hull, which fills the middle in and turns every arc and every
	// ring into a solid blob.
	std::vector<ImVec2> poly_pts;
	// これらの多角形は等倍で描くので、k 倍の座標なら 1/k にして画面へ移す
	auto poly = [&](std::initializer_list<pt> ps, COLORREF ink) {
		lcd_canvas::shape sh;
		sh.ink = ink;
		for (const pt &p : ps)
			sh.pts.emplace_back(float(cv.ox + p.x * cv.sk), float(cv.oy + p.y * cv.sk));
		cv.shapes.push_back(std::move(sh));
	};
	auto ring = [&](double cx, double cy, double r0, double r1, double a0, double a1,
	                COLORREF ink) {
		// 大きく描いても角が見えないよう、2 度きざみで折る
		const int n = std::max(8, std::min(180, int((a1 - a0) / 2)));
		lcd_canvas::shape sh;
		sh.ink = ink;
		for (int i = 0; i <= n; i++) {
			const pt p = polar(cx, cy, r1, a0 + (a1 - a0) * i / n);
			sh.pts.emplace_back(float(cv.ox + p.x * cv.sk), float(cv.oy + p.y * cv.sk));
		}
		for (int i = n; i >= 0; i--) {
			const pt p = polar(cx, cy, r0, a0 + (a1 - a0) * i / n);
			sh.pts.emplace_back(float(cv.ox + p.x * cv.sk), float(cv.oy + p.y * cv.sk));
		}
		cv.shapes.push_back(std::move(sh));
	};

	auto ctl = [&](int col, int row) { return lcd_ctl(s, col, row); };

	// 1 マスぶんの点を描く。p は点の間隔（端数でもよい。各点の端を丸めて並べる）
	auto cell = [&](int row, int col, double px_, double py_, double p) {
		const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
		auto at = [](double v) { return int(std::lround(v)); };
		for (int y = 0; y < CELL_H; y++)
			for (int x = 0; x < CELL_W; x++) {
				const int l = at(px_ + x * p), t = at(py_ + y * p);
				dotbox(l, t, at(px_ + (x + 1) * p) - l, at(py_ + (y + 1) * p) - t,
				       (s.lcd_on && BIT(c[y], 4 - x)) ? LCD_ON : LCD_OFF, DOT_GAP * p);
			}
	};

	// ---- 上の面。メータ 9 マス ＋ 文字 8 桁。行のあいだは空けない
	for (int row = 0; row < LCD_ROWS; row++)
		for (int col = 0; col < TOP_COLS; col++)
			cell(row, col, x0 + col * (CELL_W + 1) * d, y0 + row * CELL_H * d, d);

	// ---- 下の面
	const int sy = g.sy;
	const int seg_h = 8 * d;                          // 文字 1 行ぶんの高さ
	auto lx = [&](int which) { return x0 + int(std::lround(m_lay.low_x[which] * d)); };
	auto lw = [&](int which) { return int(std::lround(m_lay.low_w[which] * d)); };

	// 部の番号「01」と「A01」（「 A/D1」なども同じ 5 桁）。点は下の面の大きさ。
	// 実機の字間は 1 桁目と 2 桁目が 1 点、2 桁目と 3 桁目が 2 点、あとは 1 点。
	// 2 点あくところが「01」と「A01」の境目で、low.x の 11.96（13 × 0.92）はそこから来る
	const double ld = LOW_DOT * d;
	for (int i = 0; i < 2; i++)
		cell(0, TOP_COLS + i, lx(LOW_PART) + i * (CELL_W + 1) * ld, sy, ld);
	for (int i = 0; i < 3; i++)
		cell(1, TOP_COLS + i, lx(LOW_BANK) + i * (CELL_W + 1) * ld, sy, ld);

	// 楽器のかたち。20-22 桁の両行が 1 枚の絵。**23 桁目は絵ではない**ので入れない。
	// 点は横長で、文字の点と同じくほんのわずかに隙間がある
	{
		const int ix = lx(LOW_ICON), iw = lw(LOW_ICON);
		const int first = TOP_COLS + 3, last = LCD_COLS - 1;   // 20-22
		const int nx = (last - first) * CELL_W, ny = LCD_ROWS * CELL_H;
		// ここの除算は整数のまま。reference も k 倍の座標で割ってから描く
		for (int row = 0; row < LCD_ROWS; row++)
			for (int col = first; col < last; col++) {
				const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
				for (int y = 0; y < CELL_H; y++) {
					const int yy = row * CELL_H + y;
					const int top = sy + yy * seg_h / ny;
					const int bot = sy + (yy + 1) * seg_h / ny;
					for (int x = 0; x < CELL_W; x++) {
						const int xx = (col - first) * CELL_W + x;
						const int left  = ix + xx * iw / nx;
						const int right = ix + (xx + 1) * iw / nx;
						dotbox(left, top, std::max(1, right - left), std::max(1, bot - top),
						       (s.lcd_on && BIT(c[y], 4 - x)) ? LCD_ON : FAINT);
					}
				}
			}
	}

	// ---- 決まった形のセグメント。**23 桁目のビットで点け消しする**。
	// 形と寸法は実機の写真から採った（単位は点の間隔 d）
	{
		auto ink = [&](bool on) { return on ? LCD_ON : LCD_OFF; };
		const double dd = d;
		const double dv = LOW_DOT * dd;   // 下の面の高さは下の面の点の間隔で測ってある
		// 細い線は、縮めたあとでも 1 画素（px）を下回らないようにする。
		// 下回ると色が薄まって、小さい窓では見えなくなる
		auto thick = [&](double t) { return std::max(t, px); };
		// 7 セグメント。seg は a b c d e f g の順のビット。
		// 実機のセグメントは端が斜めに切れた台形で、真ん中の g は両端がとがる
		auto seven = [&](double x, double y, double w, double h, double t, unsigned seg) {
			const double k  = std::max(0.5, 0.1 * dd);        // セグメントのあいだの隙間
			const double ym = y + h / 2, ht = t / 2;
			poly({ { x + k, y }, { x + w - k, y }, { x + w - t - k, y + t }, { x + t + k, y + t } },
			     ink(BIT(seg, 0)));                                             // a
			poly({ { x + w, y + k }, { x + w, ym - ht - k }, { x + w - ht, ym - k },
			       { x + w - t, ym - ht - k }, { x + w - t, y + t + k } },
			     ink(BIT(seg, 1)));                                             // b
			poly({ { x + w, ym + ht + k }, { x + w, y + h - k }, { x + w - t, y + h - t - k },
			       { x + w - t, ym + ht + k }, { x + w - ht, ym + k } },
			     ink(BIT(seg, 2)));                                             // c
			poly({ { x + t + k, y + h - t }, { x + w - t - k, y + h - t }, { x + w - k, y + h },
			       { x + k, y + h } },
			     ink(BIT(seg, 3)));                                             // d
			poly({ { x, ym + ht + k }, { x + ht, ym + k }, { x + t, ym + ht + k },
			       { x + t, y + h - t - k }, { x, y + h - k } },
			     ink(BIT(seg, 4)));                                             // e
			poly({ { x, y + k }, { x + t, y + t + k }, { x + t, ym - ht - k },
			       { x + ht, ym - k }, { x, ym - ht - k } },
			     ink(BIT(seg, 5)));                                             // f
			poly({ { x + ht + k, ym }, { x + t + k, ym - ht }, { x + w - t - k, ym - ht },
			       { x + w - ht - k, ym }, { x + w - t - k, ym + ht }, { x + t + k, ym + ht } },
			     ink(BIT(seg, 6)));                                             // g
		};

		// 送り量の扇。中心角 45 度の細い弧を 8 本、同じ中心で重ねたもの。
		// 中心（扇の要）は下の面の下端より少し下にある。下から N 本を点ける
		auto fan = [&](int which, const bool *on8) {
			const double cx = lx(which) + lw(which) / 2.0;
			const double cy = sy + 8.4 * dv;
			for (int k = 0; k < 8; k++) {
				const double r = (1.45 + 1.0 * k) * dd;
				ring(cx, cy, r - thick(0.42 * dd) / 2, r + thick(0.42 * dd) / 2, -22.5, 22.5, ink(on8[k]));
			}
		};

		// VOL と EXP。**行 0 の 19 桁目**に、レベルメータと同じ形で
		// 入っている（左の 2 点が VOL、右の 2 点が EXP）。
		// 実機では離れた場所に、横に長い 8 本の棒で出る
		{
			const u8 *c = s.dots + (0 * LCD_COLS + TOP_COLS + 2) * CELL_H;
			// 棒の間隔は**画素（k）の整数**に丸め、棒の上端も k 画素の
			// 境目に揃える。こうすると 8 本とも画素との位置関係が同じになり、
			// 太さが揃う。k で割り切れない間隔だと棒の上端が画素の途中に
			// 落ちてしまい、平均を取ったあと全部の棒がぼけて、間隔も
			// 1 画素ぶんずつ動く。太さの端数は、どの棒も同じだけ下の縁がぼける
			const double want = 8.3 * dv / 8;
			const double pitch = std::max(1.0, std::round(want / px)) * px;
			const int bar = std::max(int(std::lround(px)), int(std::lround(thick(0.48 * pitch))));
			const double start = std::round((sy - 0.5 * dv + 4 * (want - pitch)) / px) * px;
			for (int y = 0; y < CELL_H; y++) {
				const int top = int(std::lround(start + y * pitch));
				const int bot = top + bar;
				const bool vol = s.lcd_on && (BIT(c[y], 4) || BIT(c[y], 3));
				const bool exp = s.lcd_on && (BIT(c[y], 1) || BIT(c[y], 0));
				cv.rect(lx(LOW_VOL), top, lx(LOW_VOL) + lw(LOW_VOL), bot, ink(vol));
				cv.rect(lx(LOW_EXP), top, lx(LOW_EXP) + lw(LOW_EXP), bot, ink(exp));
			}
		}

		// パン。下の開いた円弧の中で、針が 45 度おきの 7 か所に飛ぶ。
		// D15 が左下、D12 が真上、D9 が右下
		{
			const double cx = lx(LOW_PAN) + lw(LOW_PAN) / 2.0, cy = sy + 3.75 * dv;
			const double r = 3.6 * dd;
			// 円弧は点けたり消したりしない（実機はいつも点いている）
			ring(cx, cy, r - thick(0.25 * dd), r, -124.0, 124.0, ink(s.lcd_on));
			const double r0 = 0.29 * r, r1 = 0.72 * r, ht = thick(0.36 * dd) / 2;
			for (int k = 0; k < 7; k++) {
				const bool on = ctl(CD, 15 - k);
				const double a = -135.0 + 45.0 * k;
				const pt p0 = polar(cx, cy, r0, a), p1 = polar(cx, cy, r1, a);
				const double nx = std::cos(a * PI / 180.0) * ht, ny = std::sin(a * PI / 180.0) * ht;
				poly({ { p0.x - nx, p0.y - ny }, { p1.x - nx, p1.y - ny },
				       { p1.x + nx, p1.y + ny }, { p0.x + nx, p0.y + ny } }, ink(on));
			}
		}

		// リバーブ・コーラス・バリエーションの送り量
		{
			bool rev[8], cho[8], var[8];
			for (int k = 0; k < 8; k++) {
				rev[k] = ctl(CA, 15 - k);
				cho[k] = ctl(CB, 15 - k);
				var[k] = ctl(CC, 15 - k);
			}
			fan(LOW_REV, rev);
			fan(LOW_CHO, cho);
			fan(LOW_VAR, var);
		}

		// ノートシフト。符号（横棒は常時、縦棒が点くと ＋）と 2 桁。
		// 符号の縦棒は、横棒と交わるところで少し途切れている
		{
			const double t  = thick(0.4 * dd);
			const double dh = 5.4 * dv, dy = sy + 2.2 * dv, dw = 2.6 * dd;
			const double kx = lx(LOW_KEY);
			const double sw = 2.4 * dd, cy = dy + dh / 2, vh = 0.62 * dh;
			const double vx = kx + sw / 2, cut = std::max(1.0, 0.3 * dd);
			poly({ { kx, cy - t / 2 }, { kx + sw, cy - t / 2 }, { kx + sw, cy + t / 2 },
			       { kx, cy + t / 2 } }, ink(ctl(CB, 0)));
			const bool plus = ctl(CA, 0);
			poly({ { vx - t / 2, cy - vh / 2 }, { vx + t / 2, cy - vh / 2 },
			       { vx + t / 2, cy - t / 2 - cut }, { vx - t / 2, cy - t / 2 - cut } }, ink(plus));
			poly({ { vx - t / 2, cy + t / 2 + cut }, { vx + t / 2, cy + t / 2 + cut },
			       { vx + t / 2, cy + vh / 2 }, { vx - t / 2, cy + vh / 2 } }, ink(plus));

			// 十の位は a/d/e/g がひとまとめ。f は使われない
			const bool ten_adeg = ctl(CA, 1);
			unsigned ten = 0;
			if (ten_adeg) ten |= (1u << 0) | (1u << 3) | (1u << 4) | (1u << 6);
			if (ctl(CB, 1)) ten |= 1u << 1;
			if (ctl(CA, 7)) ten |= 1u << 2;
			if (ctl(CB, 4)) ten |= 1u << 5;
			seven(kx + sw + 0.2 * dd, dy, dw, dh, t, ten);

			unsigned one = 0;
			if (ctl(CB, 6)) one |= 1u << 0;   // a
			if (ctl(CA, 6)) one |= 1u << 1;   // b
			if (ctl(CA, 3)) one |= 1u << 2;   // c
			if (ctl(CA, 2)) one |= 1u << 3;   // d
			if (ctl(CB, 2)) one |= 1u << 4;   // e
			if (ctl(CB, 7)) one |= 1u << 5;   // f
			if (ctl(CB, 3)) one |= 1u << 6;   // g
			seven(kx + sw + 0.2 * dd + dw + 0.5 * dd, dy, dw, dh, t, one);
		}

		// いちばん右の ▶ は 4 つ。上の 1 つは札がなく、点く場面をまだ見ていない。
		// 残りの 3 つが XG / TG300B(GS) / PERFORM。
		// PLG のぶんは C2 か D8 のどちらかだが、まだ決められていない
		{
			const double mx = lx(LOW_MODE);
			const double th = 1.45 * dv, tw = th * 0.9;     // 正三角形に近い
			const bool mode[4] = { false, ctl(CB, 5), ctl(CA, 4), ctl(CA, 5) };
			for (int k = 0; k < 4; k++) {
				const double cy = sy + MODE_Y[k] * dv;
				poly({ { mx, cy - th / 2 }, { mx + tw, cy }, { mx, cy + th / 2 } }, ink(mode[k]));
			}
		}

		// 下の面の上に出る ▼ のカーソル。いま何を弄っているかを示す
		{
			// 先は下の面より少し上。VOL の棒や扇のいちばん上に掛からないように
			const double cur_y = double(sy) - std::max(2, int(std::lround(1.6 * LOW_DOT * d)));
			const double hw = std::max(2, int(d));
			struct { int at; bool on; } cur[] = {
				{ LOW_VOL,  ctl(CC, 3) }, { LOW_EXP, ctl(CC, 4) },
				{ LOW_PAN,  ctl(CC, 5) }, { LOW_REV, ctl(CC, 6) },
				{ LOW_CHO,  ctl(CC, 7) }, { LOW_VAR, ctl(CD, 6) },
				{ LOW_KEY,  ctl(CD, 7) },
			};
			for (const auto &c : cur) {
				if (!c.on)
					continue;
				const double cx = lx(c.at) + lw(c.at) / 2;
				poly({ { cx - hw, cur_y - hw }, { cx + hw, cur_y - hw },
				       { cx, cur_y } }, LCD_ON);
			}
			// バンク番号とプログラム番号のカーソルは**楽器のかたちの上**。
			// バンクは 4-5 列目、プログラムは 12-13 列目の上（実機を見て教わった）
			const double ix = lx(LOW_ICON);
			const double tops[2] = { double(ix + (3 + 5) * int(d) / 2), double(ix + (11 + 13) * int(d) / 2) };
			const bool ton[2] = { ctl(CC, 1), ctl(CC, 0) };
			for (int k = 0; k < 2; k++) {
				if (!ton[k])
					continue;
				poly({ { tops[k] - hw, cur_y - hw }, { tops[k] + hw, cur_y - hw },
				       { tops[k], cur_y } }, LCD_ON);
			}
		}
	}
}

void panel::draw_button(ImDrawList *dl, const spot &sp, bool down) const
{
	im::round_box(dl, sp.r, down ? KEY_DOWN : KEY_FACE, KEY_EDGE, float(int(3 * m_scale)));
}

// 大きなダイヤル。回した角度で窪みが回る
// 差さっている SmartMedia。正面から見ると、差し込み口の中にカードの縁が見え、少し手前に出ている。
// 実機の写真の青いカードに合わせ、縁の上を明るく、下を暗くして厚みを出す
void panel::draw_card(ImDrawList *dl) const
{
	double sx = m_lay.card_slot[0], sy = m_lay.card_slot[1], sw = m_lay.card_slot[2], sh = m_lay.card_slot[3];
	if (sw <= 0) {
		sx = m_lay.card[0] + 6;
		sy = m_lay.card[1] + m_lay.card[3] + 2;
		sw = m_lay.card[2] - 12;
		sh = 7;
	}
	// 差し込み口の幅より少し細く、上に暗い隙間を残す。下は口の縁をわずかに越えて手前に出る
	const RECT r = scale(sx + sw * 0.03, sy + sh * 0.32, sw * 0.94, sh * 0.80);
	const float x0 = float(r.left), y0 = float(r.top), x1 = float(r.right), y1 = float(r.bottom);
	const float rr = std::max(1.0f, float(1.2 * m_scale));
	const float hl = std::max(1.0f, float(1.2 * m_scale));
	// 縁の影（口の中へ落ちる）
	dl->AddRectFilled(ImVec2(x0 + hl, y0 + hl), ImVec2(x1 + hl, y1 + hl), IM_COL32(0, 0, 0, 120), rr);
	dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(52, 118, 206, 255), IM_COL32(52, 118, 206, 255),
	                            IM_COL32(24, 66, 140, 255), IM_COL32(24, 66, 140, 255));
	// 手前に出た所の天面は明るく
	dl->AddRectFilled(ImVec2(x0 + rr, y0), ImVec2(x1 - rr, y0 + hl), IM_COL32(150, 192, 240, 255));
	dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(14, 34, 76, 255), rr, 0, std::max(1.0f, float(0.6 * m_scale)));
	// 左寄りの指をかける小さな凹み
	const float nx = x0 + (x1 - x0) * 0.1f, nw = std::max(3.0f, float(8 * m_scale));
	dl->AddRectFilled(ImVec2(nx, y1 - hl * 1.8f), ImVec2(nx + nw, y1), IM_COL32(20, 52, 112, 255), rr * 0.5f);
}

void panel::draw_wheel(ImDrawList *dl, int angle) const
{
	const POINT c = at(m_lay.dial[0], m_lay.dial[1]);
	const int r = int(m_lay.dial[2] * m_scale);

	// panel.txt で絵を渡されていれば、それを回して描く
	if (m_lay.dial_art) {
		m_lay.dial_art->draw(dl, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
		                     double(angle));
		return;
	}

	im::disc(dl, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(2 * m_scale)));
	const double a = angle * PI / 180.0;
	const int ox = c.x + int(std::sin(a) * r * 0.36);
	const int oy = c.y - int(std::cos(a) * r * 0.36);
	im::disc(dl, ox, oy, int(r * 0.45), RGB(186, 176, 140), RGB(146, 137, 106),
	         std::max(1, int(m_scale)));
}

// A/D INPUT のつまみ。panel.txt に adgain があるときだけ。回るだけで、まだ何にも効かない
void panel::draw_adgain(ImDrawList *dl) const
{
	if (m_lay.adgain[2] <= 0 || !m_lay.adgain_art)
		return;
	const POINT c = at(m_lay.adgain[0], m_lay.adgain[1]);
	const int r = int(m_lay.adgain[2] * m_scale);
	m_lay.adgain_art->draw(dl, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
	                       -135.0 + 270.0 * m_adgain_now);
}

// 音量つまみ
void panel::draw_volume(ImDrawList *dl, double v) const
{
	const POINT c = at(m_lay.volume[0], m_lay.volume[1]);
	const int r = int(m_lay.volume[2] * m_scale);
	const double deg = -135.0 + 270.0 * v;       // 左いっぱいから右いっぱいまで

	if (m_lay.volume_art) {
		m_lay.volume_art->draw(dl, RECT{ c.x - r, c.y - r, c.x + r, c.y + r }, deg);
	} else {
		im::disc(dl, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(m_scale)));
		const double a = deg * PI / 180.0;
		im::line(dl, c.x, c.y, c.x + int(std::sin(a) * r * 0.8),
		         c.y - int(std::cos(a) * r * 0.8), RGB(70, 64, 48),
		         float(std::max(2, int(2 * m_scale))));
	}
	if (!m_lay.labels_in_art)
		im::text_cap(dl, scale(m_lay.volume[0] - 36, m_lay.volume[1] + m_lay.volume[2] + 4,
		                       72, 12), "VOLUME", PANEL_INK,
		             m_fonts.small, m_fonts.small_px, true);
}

// キートップの印刷（絵の組みを使うとき）。実機は太字の濃い灰色の名前と、
// 黒い丸に白抜きの記号（− ＋ ◀ ▶）。MUTE/SOLO だけ 2 行
void panel::draw_key_print(ImDrawList *dl, const RECT &key, const char *label,
                           const char *sub, mu2000::button b, bool down) const
{
	const int w = key.right - key.left, h = key.bottom - key.top;
	const int dy = down ? std::max(1, int(std::lround(m_scale))) : 0;
	// 位置は実機の写真から（1 行のキーは真ん中、2 段のものは 0.33 と 0.61）。
	// 大文字の見える部分の真ん中をそこへ置く。箱の中で字を真ん中に，而不是
	// 箱そのものを狙いの位置の上下に等しく開く（cap_height を使う）
	const int em = std::max(6, int(std::lround(9.0 * m_scale)));
	auto line_at = [&](const char *txt, double f) {
		const int cy = key.top + int(std::lround(h * f)) + dy;
		RECT r{ key.left, cy - em, key.right, cy + em };
		im::text_cap(dl, r, txt, KEY_INK, m_fonts.key, m_fonts.key_px, true);
	};
	const bool two = !std::strcmp(sub, "SOLO");
	if (two) {
		line_at("MUTE/", 0.33);
		line_at("SOLO", 0.61);
		return;
	}
	if (!sub[0]) {
		line_at(label, 0.49);
		return;
	}
	line_at(label, 0.33);

	// 記号。黒い丸に白で抜く（縁をぼかした絵があればそれを貼る）
	const int cx = key.left + w / 2, cy = key.top + int(std::lround(h * 0.61)) + dy;
	const bool l = b == mu2000::button::select_left, rr = b == mu2000::button::select_right;
	const int kind = l ? 2 : rr ? 3 : (sub[0] == '+' ? 1 : 0);
	if (m_key_sym[kind] && m_key_sym[kind]->ok()) {
		// Whole pixels: GDI blitted this bitmap 1:1, and a fractional offset
		// turns the GPU's bilinear filter into a blur on an 8 px picture.
		const int S = m_key_sym_px;
		const int x = cx - S / 2, y = cy - S / 2;
		m_key_sym[kind]->draw(dl, RECT{ x, y, x + S, y + S });
		return;
	}
	// Fallback for when the picture is not there (no context, no texture)
	const int r = std::max(3, int(std::lround(4.2 * m_scale)));
	im::disc(dl, cx, cy, r, KEY_INK, KEY_INK, 1);
	const COLORREF white = RGB(236, 234, 226);
	const int a = std::max(1, int(std::lround(r * 0.6)));
	const int t = std::max(1, int(std::lround(r * 0.28)));
	if (l || rr) {
		const int s2 = rr ? 1 : -1;
		const ImVec2 tri[3] = { ImVec2(float(cx + s2 * a), float(cy)),
		                        ImVec2(float(cx - s2 * a / 2), float(cy - a)),
		                        ImVec2(float(cx - s2 * a / 2), float(cy + a)) };
		im::poly(dl, tri, 3, white, white, 1.0f);
	} else {
		// GDI の RECT は右と下を含まない。だから 1 画素広くして、
		// 幅の偶奇で中心が半画素ずれる分は (t & 1) で戻す
		im::fill(dl, ImVec2(float(cx - a), float(cy - t / 2 - (t & 1))),
		         ImVec2(float(2 * a + 1), float(t + 1)), white);
		if (sub[0] == '+')
			im::fill(dl, ImVec2(float(cx - t / 2 - (t & 1)), float(cy - a)),
			         ImVec2(float(t + 1), float(2 * a + 1)), white);
	}
}

void panel::paint_front(ImDrawList *dl, const snapshot &s, u64 pressed, double volume,
                        const char *status) const
{
	im::fill(dl, RECT{ 0, 0, m_w, m_h }, RGB(24, 26, 30));
	im::fill(dl, scale(0, 0, LOGICAL_W, m_lay.body_h), PANEL_FACE);

	// ---- 飾り。位置も色も panel.txt から来る（doc/panel-editing.md）
	for (const deco &g : m_lay.decos) {
		if (g.k == deco::text)
			im::text_dt(dl, scale(g.x, g.y, g.w, g.h), g.str.c_str(), g.a,
			            g.font ? m_fonts.label : m_fonts.small,
			            g.font ? m_fonts.label_px : m_fonts.small_px, g.align);
		else if (g.k == deco::disc) {
			const POINT c = at(g.x, g.y);
			im::disc(dl, c.x, c.y, int(g.w * m_scale), g.a, g.b,
			         std::max(1, int(g.h * m_scale)));
		} else if (g.k == deco::art) {
			if (g.pic)
				g.pic->draw(dl, scale(g.x, g.y, g.w, g.h));
		} else
			im::round_box(dl, scale(g.x, g.y, g.w, g.h), g.a, g.b,
			              float(std::max(1, int(g.radius * m_scale))));
	}

	// ---- 中

	draw_lcd(dl, s);

	// 窓の下の札は、下段の並びと同じ割合で置く。窓の中身とずれないように。
	// 札が絵に入っているときは書かない（以下の札も同じ）
	const bool labels = !m_lay.labels_in_art;
	if (labels) {
		const lcd_geom g = lcd_grid();
		const int y = at(0, m_lay.columns_y).y, h = int(12 * m_scale), w = int(64 * m_scale);
		for (const column &c : COLUMNS) {
			const int cx = int(std::lround(g.fx0 + (m_lay.low_x[c.at] + m_lay.low_w[c.at] / 2) * g.df));
			RECT r{ cx - w / 2, y, cx + w / 2, y + h };
			im::text_cap(dl, r, c.label, PANEL_INK, m_fonts.small, m_fonts.small_px, true);
		}

		// 窓の右の札。高さは液晶の中の ▶ に合わせる（横は panel.txt の modes.x）
		if (m_lay.modes_x >= 0) {
			const int x = at(m_lay.modes_x, 0).x;
			// ▶ の間隔より字が大きいと重なる（小さい窓で、字の下限が効くとき）
			const double step = (MODE_Y[2] - MODE_Y[1]) * LOW_DOT * g.df;
			ImFont *font = m_fonts.small;
			float fpx = m_fonts.small_px;
			if (step < std::max(7.0, 8.5 * m_scale)) {
				font = m_fonts.tiny;
				fpx = m_fonts.tiny_px;
			}
			for (int k = 0; k < 3; k++) {
				const int cy = int(std::lround(g.fsy + MODE_Y[k + 1] * LOW_DOT * g.df));
				RECT r{ x, cy - h, x + int(80 * m_scale), cy + h };
				im::text_in(dl, im::pos_of(r), im::size_of(r), MODE_LABEL[k], PANEL_INK,
				            font, fpx, false, true, false);
			}
		}
	}

	for (int i = 0; i < 18 && labels; i++) {
		RECT r = scale(m_lay.cat_x[i % 6] - 34, m_lay.cat_y[i / 6] - 14, 68, 14);
		im::text_cap(dl, r, CAT_LABEL[i], PANEL_INK, m_fonts.small, m_fonts.small_px, true);
	}

	// MU / PLG-1..3 の表示灯。LED は 6 番から
	{
		const char *plg[4] = { "MU", "PLG-1", "PLG-2", "PLG-3" };
		for (int i = 0; i < 4; i++) {
			const double plx = m_lay.plg[0] + i * m_lay.plg[1];
			const POINT c = at(plx, m_lay.plg[2]);
			const bool on = BIT(s.leds, 6 + i) != 0;
			if (const svg_art *pic = m_lay.plg_art.pick(on, false)) {
				const int rw = int(m_lay.plg_size[0] / 2 * m_scale);
				const int rh = int(m_lay.plg_size[1] / 2 * m_scale);
				pic->draw(dl, RECT{ c.x - rw, c.y - rh, c.x + rw, c.y + rh });
			} else {
				// 実機の表示灯は四角
				const int r = int(4 * m_scale);
				im::round_box(dl, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
				              on ? LED_ON : RGB(64, 62, 52), RGB(110, 106, 92),
				              float(std::max(1, int(m_scale))));
			}
			if (labels)
				im::text_cap(dl, scale(plx - 22, m_lay.plg[2] + 7, 44, 12), plg[i],
				             PANEL_INK, m_fonts.small, m_fonts.small_px, true);
		}
	}

	// ---- 右

	for (int i = 0; i < 6; i++) {
		const mode_button &m = MODES[i];
		const double mx = m_lay.mode[i][0], my = m_lay.mode[i][1];
		if (labels) {
			RECT r = scale(mx - 34, my - m_lay.mode_r - 19, 68, 14);
			im::text_cap(dl, r, m.label, PANEL_INK, m_fonts.small, m_fonts.small_px, true);
		}
		const POINT c = at(mx, my);
		const bool down = ((pressed >> int(m.b)) & 1) != 0;
		const bool on = BIT(s.leds, m.led) != 0;
		// panel.txt で絵を渡されていれば、ようすに合う 1 枚を貼る
		const svg_art *pic = m_lay.mode_art.pick(on, down);
		if (on && !down && m_lay.mode_on[i])
			pic = m_lay.mode_on[i].get();
		if (pic) {
			const int r = int(m_lay.mode_r * m_scale);
			pic->draw(dl, RECT{ c.x - r, c.y - r, c.x + r, c.y + r });
		} else {
			im::disc(dl, c.x, c.y, int(m_lay.mode_r * m_scale),
			         down ? KEY_DOWN : RGB(198, 188, 152), KEY_EDGE,
			         std::max(1, int(m_scale)));
			im::disc(dl, c.x, c.y, int(m_lay.mode_led_r * m_scale),
			         on ? LED_ON : RGB(74, 72, 60), RGB(110, 106, 92), 1);
		}
	}

	// 四角いボタン。名札は上に重ねる
	for (int i = 0; i < 9; i++) {
		const place &p = NAV[i];
		const double px = m_lay.nav[i][0], py = m_lay.nav[i][1];
		const double pw = m_lay.nav[i][2], ph = m_lay.nav[i][3];
		const spot *sp = nullptr;
		for (const spot &q : m_spots)
			if (q.kind == spot_kind::button && q.button == p.b) { sp = &q; break; }
		if (!sp)
			continue;
		const bool down = ((pressed >> int(p.b)) & 1) != 0;
		// 印刷まで入ったキーごとの絵があれば、それだけ貼る
		if (const svg_art *face = m_lay.nav_face[i].pick(down, down)) {
			face->draw(dl, sp->r);
			continue;
		}
		if (const svg_art *pic = m_lay.nav_art.pick(down, down))
			pic->draw(dl, sp->r);
		else
			draw_button(dl, *sp, down);
		if (m_lay.labels_in_art) {
			draw_key_print(dl, sp->r, p.label, p.sub, p.b, down);
			continue;
		}
		im::text_cap(dl, scale(px, py + 4, pw, 12), p.label, RGB(58, 53, 38),
		             m_fonts.small, m_fonts.small_px, true);
		if (p.sub[0])
			im::text_cap(dl, scale(px, py + ph - 14, pw, 12), p.sub, RGB(58, 53, 38),
			             m_fonts.small, m_fonts.small_px, true);
	}
	for (int i = 0; i < 18; i++) {
		RECT r = scale(m_lay.cat_x[i % 6] - m_lay.cat_w / 2, m_lay.cat_y[i / 6],
		               m_lay.cat_w, m_lay.cat_h);
		const bool down = ((pressed >> int(CAT_B[i])) & 1) != 0;
		if (const svg_art *pic = m_lay.cat_art.pick(down, down))
			pic->draw(dl, r);
		else
			im::round_box(dl, r, down ? KEY_DOWN : KEY_FACE, KEY_EDGE, float(int(3 * m_scale)));
	}
	for (int i = 0; i < 2; i++) {
		const place &p = ROUND[i];
		const double px = m_lay.round_[i][0], py = m_lay.round_[i][1];
		const POINT c = at(px, py);
		const bool down = ((pressed >> int(p.b)) & 1) != 0;
		const int rr = int(m_lay.round_[i][2] / 2 * m_scale);
		if (const svg_art *pic = m_lay.round_art.pick(down, down))
			pic->draw(dl, RECT{ c.x - rr, c.y - rr, c.x + rr, c.y + rr });
		else
			im::disc(dl, c.x, c.y, rr, down ? KEY_DOWN : KEY_FACE, KEY_EDGE,
			         std::max(1, int(m_scale)));
		if (labels)
			im::text_cap(dl, scale(px - 40, py - 26, 80, 12), p.label, PANEL_INK,
			             m_fonts.small, m_fonts.small_px, true);
	}

	if (s.card)
		draw_card(dl);
	draw_wheel(dl, m_wheel_angle);
	draw_volume(dl, volume);
	draw_adgain(dl);

	// 状態の行は本体の一番下（body_h の内側）に載るので、ボタンの名前と同じ濃い色で書く。
	// 前は暗い帯向けの薄い灰色で、本体の地の色に溶けて読めなかった
	if (status && status[0])
		im::text_in(dl, im::pos_of(m_status), im::size_of(m_status), status,
		            PANEL_INK, m_fonts.small, m_fonts.small_px, false, true, false);

	draw_tabs(dl);
}

// 論理座標の方眼。50 ごとに線、100 ごとに濃い線と数字を入れる。
// 絵の位置を直すときは、これを出して読み取ってから表を書き換える
void panel::draw_grid(ImDrawList *dl) const
{
	for (int x = 0; x <= LOGICAL_W; x += 50) {
		const POINT a = at(x, 0), b = at(x, LOGICAL_H);
		im::line(dl, a.x, a.y, b.x, b.y, (x % 100) ? RGB(255, 80, 80) : RGB(255, 0, 0), 1.0f);
	}
	for (int y = 0; y <= LOGICAL_H; y += 50) {
		const POINT a = at(0, y), b = at(LOGICAL_W, y);
		im::line(dl, a.x, a.y, b.x, b.y, (y % 100) ? RGB(255, 80, 80) : RGB(255, 0, 0), 1.0f);
	}
	for (int x = 0; x <= LOGICAL_W; x += 100)
		for (int y = 0; y <= LOGICAL_H; y += 100) {
			char n[32];
			std::snprintf(n, sizeof(n), "%d,%d", x, y);
			RECT r{ at(x + 2, y + 1).x, at(x + 2, y + 1).y,
			        at(x + 60, y + 12).x, at(x + 60, y + 12).y };
			im::text_in(dl, im::pos_of(r), im::size_of(r), n, RGB(200, 0, 0),
			            m_fonts.small, m_fonts.small_px);
		}
}

void panel::paint(ImDrawList *dl, const snapshot &s, u64 pressed,
                  const char *status) const
{
	// The panel draws every picture on the panel, so this is the one place all
	// of them pass through. Hand back the ones a resize replaced: the draw list
	// is empty here (GetBackgroundDrawList reset it, since this is the first
	// asking this frame) and the backend has had the whole of last frame to
	// release their graphics objects (see im::tex::retire).
	im::drop_retired_textures();
	if (m_lcd_only) {
		draw_lcd(dl, s);
		return;
	}
	if (m_page == page::editor)       paint_editor(dl, status);
	else if (m_page == page::effects) paint_effects(dl, status);
	else                              paint_front(dl, s, pressed, m_volume_now, status);
	if (m_grid)
		draw_grid(dl);
}

} // namespace ui
