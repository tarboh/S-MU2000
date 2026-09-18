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

#include "panel.h"
#include "draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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
// 下の面のものは、上の面のパート番号と縦に揃っている。実機を見て教わった
// 対応は次のとおりで、bar_x() でその位置を出している。
//
//   VOL  15 番と左揃え     EXP  18 番と右揃え
//   REV  24 番と右揃え     CHO  25 番と左揃え     VAR  28-29 番の中央
//
// 塊のあいだは 2 点、楽器のかたちの前は 3 点あける。
// 塊の中の桁は**詰めて並べる**（上の面のように 1 点あけない）

// n 番のバーの左端。A1 が 0、A2 が 1、パート 1 が 2 …（点の単位）
constexpr int bar_x(int i) { return (i / 2) * (CELL_W + 1) + ((i & 1) ? 3 : 0); }
constexpr int part_x(int n) { return bar_x(n + 1); }


// 窓の下に印刷されている札。どの並びの真ん中に置くか
struct column { int at; const char *label; };
const column COLUMNS[] = {
	{ LOW_PART, "PART" }, { LOW_ICON, "BANK/PGM#" }, { LOW_VOL, "VOL" },
	{ LOW_EXP,  "EXP"  }, { LOW_PAN,  "PAN" },       { LOW_REV, "REV" },
	{ LOW_CHO,  "CHO"  }, { LOW_VAR,  "VAR" },       { LOW_KEY, "KEY" },
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

} // namespace


panel::panel()
{
	resize(LOGICAL_W, LOGICAL_H);
}

panel::~panel()
{
	if (m_font_label) DeleteObject(m_font_label);
	if (m_font_small) DeleteObject(m_font_small);
	if (m_font_tiny)  DeleteObject(m_font_tiny);
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

// 論理座標の点を実座標へ
POINT panel::at(double x, double y) const
{
	POINT p;
	p.x = m_ox + int(std::lround(x * m_scale));
	p.y = m_oy + int(std::lround(y * m_scale));
	return p;
}

void panel::resize(int w, int h)
{
	m_w = std::max(w, 200);
	m_h = std::max(h, 60);

	if (m_lcd_only) {
		const double margin = 5.0;
		const double view_w = m_lay.lcd[2] + margin * 2;
		const double view_h = m_lay.lcd[3] + margin * 2;
		m_scale = std::min(double(m_w) / view_w, double(m_h) / view_h);
		m_ox = int((m_w - view_w * m_scale) / 2 - (m_lay.lcd[0] - margin) * m_scale);
		m_oy = int((m_h - view_h * m_scale) / 2 - (m_lay.lcd[1] - margin) * m_scale);
	} else {
		m_scale = std::min(double(m_w) / LOGICAL_W, double(m_h) / LOGICAL_H);
		m_ox = int((m_w - LOGICAL_W * m_scale) / 2);
		m_oy = int((m_h - LOGICAL_H * m_scale) / 2);
	}

	m_lcd    = scale(m_lay.lcd[0], m_lay.lcd[1], m_lay.lcd[2], m_lay.lcd[3]);
	m_volume = scale(m_lay.volume[0] - m_lay.volume[2], m_lay.volume[1] - m_lay.volume[2],
	                 m_lay.volume[2] * 2, m_lay.volume[2] * 2);   // 当たりは丸で見る
	m_status = scale(20, 372, 700, 13);
	m_hint   = scale(20, 386, 700, 13);
	m_wheel  = scale(m_lay.dial[0] - m_lay.dial[2], m_lay.dial[1] - m_lay.dial[2],
	                 m_lay.dial[2] * 2, m_lay.dial[2] * 2);
	for (int i = 0; i < 6; i++)
		m_leds[i] = scale(m_lay.mode[i][0] - m_lay.mode_r, m_lay.mode[i][1] - m_lay.mode_r,
		                  m_lay.mode_r * 2, m_lay.mode_r * 2);

	if (m_font_label) DeleteObject(m_font_label);
	if (m_font_small) DeleteObject(m_font_small);
	if (m_font_tiny)  DeleteObject(m_font_tiny);
	auto make_font = [&](double px, int weight, int floor_px = 7) {
		return CreateFontA(-std::max(floor_px, int(px * m_scale)), 0, 0, 0, weight,
		                   FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
		                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
		                   "Segoe UI");
	};
	m_font_label = make_font(13, FW_BOLD);
	m_font_small = make_font(8.5, FW_NORMAL);
	// 目盛りは 34 個の番号をバーの真下に並べるので、思い切り小さくする
	m_font_tiny  = make_font(6.5, FW_NORMAL, 5);

	build_spots();
}

// 触れる場所は面ごとに違う。掴んでいる途中に作り直すと迷子になるので離す
void panel::build_spots()
{
	m_held = nullptr;
	m_release_pending = false;
	m_spots.clear();
	if (m_lcd_only)
		return;

	// 面を選ぶつまみ。本体の外（下の帯）
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FRONT,
	                    scale(700, 386, 94, 13), "パネル", "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_EDIT,
	                    scale(800, 386, 94, 13), "エディタ", "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FX,
	                    scale(898, 386, 94, 13), "エフェクト", "" });

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
}

const spot *panel::hit(int x, int y) const
{
	for (const spot &s : m_spots) {
		if (x >= s.r.left && x < s.r.right && y >= s.r.top && y < s.r.bottom) {
			// 丸いものは丸の中だけ
			if (s.kind == spot_kind::wheel || s.kind == spot_kind::volume) {
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


void panel::draw_tabs(HDC dc) const
{
	for (const spot &sp : m_spots) {
		if (sp.kind != spot_kind::tab)
			continue;
		const bool on = (sp.ctl == CTL_TAB_EDIT   && m_page == page::editor) ||
		                (sp.ctl == CTL_TAB_FX     && m_page == page::effects) ||
		                (sp.ctl == CTL_TAB_FRONT  && m_page == page::front);
		round_box(dc, sp.r, on ? RGB(70, 76, 84) : RGB(38, 41, 46),
		          on ? ACCENT : RGB(70, 74, 80), int(4 * m_scale));
		text_in(dc, sp.r, sp.label, on ? TEXT : TEXT_DIM, m_font_small,
		        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
}

void panel::draw_lcd(HDC dc, const snapshot &s) const
{
	RECT bez = m_lcd;
	InflateRect(&bez, int(5 * m_scale), int(5 * m_scale));
	round_box(dc, bez, RGB(60, 58, 52), RGB(110, 106, 96), int(5 * m_scale));
	fill(dc, m_lcd, LCD_BACK);

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
	const int aw = m_lcd.right - m_lcd.left, ah = m_lcd.bottom - m_lcd.top;
	const int pad = std::max(2, int(5 * m_scale));

	// 点の大きさ。上の面 17 桁ぶんが横幅に収まるように決める
	const int top_dots = TOP_COLS * (CELL_W + 1) - 1;      // 桁のあいだ 1 点
	int d = std::max(1, (aw - pad * 2) / top_dots);
	// 目盛りの帯は 2 段。上に目盛りと番号、下に MIC / LINE / BANK / PGM#
	const int tick_h = std::max(2, int(2.5 * m_scale));
	int line_h = std::max(6, int(8.5 * m_scale));
	// 目盛りの帯は 3 段。番号／MIC と BANK と PGM#／LINE。
	// **下の面はその下**。ここを詰めると「01」と MIC が重なる
	while (d > 1 && 16 * d + tick_h + line_h * 3 + 8 * d > ah - pad * 2)
		d--;
	const int scale_h = tick_h + line_h * 3;
	const int stack_h = 16 * d + scale_h + 8 * d;
	const int dot = std::max(1, d - std::max(1, d / 6));   // 点のあいだの隙間

	// 点は正方形のままにして、余った幅は左右に振り分ける
	const int x0 = m_lcd.left + std::max(pad, (aw - top_dots * d) / 2);
	const int y0 = m_lcd.top + std::max(pad, (ah - stack_h) / 2);

	HBRUSH ghost = CreateSolidBrush(LCD_GHOST);
	HBRUSH faint = CreateSolidBrush(RGB(147, 202, 45));    // 絵の区画の消え点
	HBRUSH lit   = CreateSolidBrush(LCD_DOT);

	// 23 桁目の制御ビット。列 A-D は bit3-bit0、行は上の桁の 0-7 と
	// 下の桁の 0-7 をつないだ 0-15。番地は実測（doc/gui.md）
	auto ctl = [&](int col, int row) -> bool {
		if (!s.lcd_on)
			return false;
		const u8 v = s.dots[((row / 8) * LCD_COLS + TOP_COLS + 6) * CELL_H + (row % 8)];
		return BIT(v, 3 - col) != 0;
	};
	enum { CA = 0, CB = 1, CC = 2, CD = 3 };

	// 1 マスぶんの点を描く。step は点の間隔、size は点の大きさ
	auto cell = [&](int row, int col, int px, int py, int step, int size, HBRUSH back) {
		const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
		for (int y = 0; y < CELL_H; y++)
			for (int x = 0; x < CELL_W; x++) {
				RECT r;
				r.left   = px + x * step;
				r.top    = py + y * step;
				r.right  = r.left + size;
				r.bottom = r.top  + size;
				FillRect(dc, &r, (s.lcd_on && BIT(c[y], 4 - x)) ? lit : back);
			}
	};

	// ---- 上の面。メータ 9 マス ＋ 文字 8 桁。行のあいだは空けない
	for (int row = 0; row < LCD_ROWS; row++)
		for (int col = 0; col < TOP_COLS; col++)
			cell(row, col, x0 + col * (CELL_W + 1) * d, y0 + row * CELL_H * d, d, dot, ghost);

	// ---- 目盛りの帯。ここも**印刷ではなくセグメント**で、点いたり消えたりする
	const int scale_y = y0 + 16 * d;
	const int line2 = scale_y + tick_h + line_h;
	{
		const bool on_scale = ctl(CD, 4);          // 「1」-「32」
		const bool on_a1a2  = ctl(CD, 3);          // 「A1」「A2」
		const COLORREF ink_scale = on_scale ? LCD_DOT : LCD_GHOST;
		const COLORREF ink_a1a2  = on_a1a2  ? LCD_DOT : LCD_GHOST;

		HPEN p = CreatePen(PS_SOLID, 1, on_scale ? LCD_DOT : LCD_GHOST);
		HGDIOBJ op = SelectObject(dc, p);
		for (int i = 0; i < TOP_COLS * 2; i++) {
			const int col = i / 2;
			const int bx  = x0 + col * (CELL_W + 1) * d + ((i & 1) ? 3 * d : 0) + d / 2;
			if (i >= 2 || on_a1a2) {
				MoveToEx(dc, bx, scale_y, nullptr);
				LineTo(dc, bx, scale_y + tick_h);
			}
			// 番号はパートの番号。**そのバーの真下**に置く
			const int part = i - 1;
			char n[8];
			std::snprintf(n, sizeof(n), i < 2 ? "A%d" : "%d", i < 2 ? i + 1 : part);
			RECT t{ bx - 3 * d / 2, scale_y + tick_h,
			        bx + 3 * d / 2, scale_y + tick_h + line_h };
			text_in(dc, t, n, i < 2 ? ink_a1a2 : ink_scale, m_font_tiny,
			        DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
		SelectObject(dc, op);
		DeleteObject(p);

		// MIC と LINE は左端に上下に並ぶ
		RECT mic{ x0, line2, x0 + int(20 * m_scale), line2 + line_h };
		text_in(dc, mic, "MIC", ctl(CD, 1) ? LCD_DOT : LCD_GHOST, m_font_small,
		        DT_LEFT | DT_TOP | DT_SINGLELINE);
		RECT lin{ x0, line2 + line_h, x0 + int(24 * m_scale), line2 + line_h * 2 };
		text_in(dc, lin, "LINE", ctl(CD, 2) ? LCD_DOT : LCD_GHOST, m_font_small,
		        DT_LEFT | DT_TOP | DT_SINGLELINE);

		// BANK と PGM# は 2 つずつあり、パート番号の下に並んでいる。
		// 左側の組が D0、右側の組が D5 で点け消しされる
		struct { int part; const char *label; bool right; } marks[] = {
			{  3, "BANK", false }, { 11, "PGM#", false },
			{ 19, "BANK", true  }, { 27, "PGM#", true  },
		};
		for (const auto &mk : marks) {
			const int cx = x0 + (part_x(mk.part) + part_x(mk.part + 1) + 2) * d / 2;
			const int w = int(26 * m_scale);
			RECT r{ cx - w / 2, line2, cx + w / 2, line2 + line_h };
			text_in(dc, r, mk.label,
			        ctl(CD, mk.right ? 5 : 0) ? LCD_DOT : LCD_GHOST, m_font_small,
			        DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
	}

	// ---- 下の面
	const int sy = scale_y + scale_h;
	const int seg_h = 8 * d;                       // 文字 1 行ぶんの高さ
	auto lx = [&](int which) { return x0 + m_lay.low_x[which] * d; };
	auto lw = [&](int which) { return m_lay.low_w[which] * d; };

	// 部の番号「01」と「A01」。塊の中の桁は**詰めて**並べる
	for (int i = 0; i < 2; i++)
		cell(0, TOP_COLS + i, lx(LOW_PART) + i * CELL_W * d, sy, d, dot, ghost);
	for (int i = 0; i < 3; i++)
		cell(1, TOP_COLS + i, lx(LOW_BANK) + i * CELL_W * d, sy, d, dot, ghost);

	// 楽器のかたち。20-22 桁の両行が 1 枚の絵。**23 桁目は絵ではない**ので入れない
	{
		const int ix = lx(LOW_ICON), iw = lw(LOW_ICON);
		const int first = TOP_COLS + 3, last = LCD_COLS - 1;   // 20-22
		const int nx = (last - first) * CELL_W, ny = LCD_ROWS * CELL_H;
		for (int row = 0; row < LCD_ROWS; row++)
			for (int col = first; col < last; col++) {
				const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
				for (int y = 0; y < CELL_H; y++) {
					const int yy = row * CELL_H + y;
					RECT r;
					r.top    = sy + yy * seg_h / ny;
					r.bottom = sy + (yy + 1) * seg_h / ny;
					if (r.bottom <= r.top)
						r.bottom = r.top + 1;
					for (int x = 0; x < CELL_W; x++) {
						const int xx = (col - first) * CELL_W + x;
						r.left  = ix + xx * iw / nx;
						r.right = ix + (xx + 1) * iw / nx;
						if (r.right <= r.left)
							r.right = r.left + 1;
						FillRect(dc, &r, (s.lcd_on && BIT(c[y], 4 - x)) ? lit : faint);
					}
				}
			}
	}

	// ---- 決まった形のセグメント。**23 桁目のビットで点け消しする**
	{
		HBRUSH on_b  = lit;
		HBRUSH off_b = ghost;
		const int t = std::max(1, int(1.6 * m_scale));
		auto brush = [&](bool on) { return on ? on_b : off_b; };

		// 7 セグメント。seg は a b c d e f g の順のビット
		auto seven = [&](int x, int w, int top, int h, unsigned seg, bool ganged_adeg) {
			const int mid = top + h / 2;
			auto bar = [&](bool on, int l, int tp, int r, int b) {
				RECT rc{ l, tp, r, b };
				FillRect(dc, &rc, brush(on));
			};
			const bool a = ganged_adeg ? BIT(seg, 0) : BIT(seg, 0);
			bar(a,            x,         top,          x + w,     top + t);         // a
			bar(BIT(seg, 1),  x + w - t, top,          x + w,     mid);              // b
			bar(BIT(seg, 2),  x + w - t, mid,          x + w,     top + h);          // c
			bar(BIT(seg, 3),  x,         top + h - t,  x + w,     top + h);          // d
			bar(BIT(seg, 4),  x,         mid,          x + t,     top + h);          // e
			bar(BIT(seg, 5),  x,         top,          x + t,     mid);              // f
			bar(BIT(seg, 6),  x,         mid - t / 2,  x + w,     mid + t - t / 2);  // g
		};

		// 送り量の扇。**半円ではなく、中心角 45 度くらいの細い扇**を
		// 8 枚重ねたもの。細長い Wi-Fi の印のように見える。下から N 枚を点ける
		auto fan = [&](int which, const bool *on8) {
			const int w  = lw(which);
			const int cx = lx(which) + w / 2;
			const int cy = sy + seg_h - std::max(1, int(m_scale));
			// 45 度ぶんだけ描くので、いちばん外の弧が枠いっぱいになるよう
			// 楕円は枠より大きく取る（sin 22.5 度 = 0.3827）
			const double half = 22.5 * 3.14159265 / 180.0;
			const int rx = int((w / 2) / std::sin(half));
			const int ry = seg_h - std::max(1, int(m_scale));
			for (int k = 0; k < 8; k++) {
				const int ax = std::max(2, rx * (k + 1) / 8);
				const int ay = std::max(2, ry * (k + 1) / 8);
				const int ex = int(std::sin(half) * ax);
				const int ey = int(std::cos(half) * ay);
				HPEN p = CreatePen(PS_SOLID, std::max(1, int(1.2 * m_scale)),
				                   on8[k] ? LCD_DOT : LCD_GHOST);
				HGDIOBJ op = SelectObject(dc, p);
				Arc(dc, cx - ax, cy - ay, cx + ax, cy + ay,
				    cx + ex, cy - ey, cx - ex, cy - ey);
				SelectObject(dc, op);
				DeleteObject(p);
			}
		};

		// VOL と EXP。**行 0 の 19 桁目**に、レベルメータと同じ形で
		// 入っている（左の 2 点が VOL、右の 2 点が EXP）。
		// 実機では離れた場所に出るので、切り離して描く
		{
			const u8 *c = s.dots + (0 * LCD_COLS + TOP_COLS + 2) * CELL_H;
			for (int y = 0; y < CELL_H; y++)
				for (int x = 0; x < 2; x++) {
					RECT rv{ lx(LOW_VOL) + x * d, sy + y * d,
					         lx(LOW_VOL) + x * d + dot, sy + y * d + dot };
					FillRect(dc, &rv, (s.lcd_on && BIT(c[y], 4 - x)) ? on_b : off_b);
					RECT re{ lx(LOW_EXP) + x * d, sy + y * d,
					         lx(LOW_EXP) + x * d + dot, sy + y * d + dot };
					FillRect(dc, &re, (s.lcd_on && BIT(c[y], 1 - x)) ? on_b : off_b);
				}
		}

		// パン。丸の中の針が 7 か所に飛ぶ。D9(右端) から D15(左端)
		{
			const int w  = lw(LOW_PAN);
			const int r  = std::min<int>(seg_h / 2, w / 2);
			const int cx = lx(LOW_PAN) + w / 2, cy = sy + seg_h / 2;
			HPEN p = CreatePen(PS_SOLID, std::max(1, int(1.2 * m_scale)), LCD_GHOST);
			HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
			HGDIOBJ op = SelectObject(dc, p);
			Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
			SelectObject(dc, op);
			DeleteObject(p);
			for (int k = 0; k < 7; k++) {
				// k=0 が左端（D15）、k=6 が右端（D9）
				const bool on = ctl(CD, 15 - k);
				const double ang = (-60.0 + 20.0 * k) * 3.14159265 / 180.0;
				HPEN q = CreatePen(PS_SOLID, std::max(1, int(1.4 * m_scale)),
				                   on ? LCD_DOT : LCD_GHOST);
				HGDIOBJ oq = SelectObject(dc, q);
				MoveToEx(dc, cx, cy, nullptr);
				LineTo(dc, cx + int(std::sin(ang) * (r - 1)),
				       cy - int(std::cos(ang) * (r - 1)));
				SelectObject(dc, oq);
				DeleteObject(q);
			}
			SelectObject(dc, ob);
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
		// 文字より**一回り小さく**して、モードの ▶ の左に寄せる
		const int mode_w = 3 * d;
		const int mode_x = m_lcd.right - pad - mode_w;
		{
			const int kw = 8 * d;
			const int kx = std::min(lx(LOW_KEY), mode_x - 2 * d - kw);
			const int cy = sy + seg_h / 2;
			const int sw = 2 * d;
			RECT h{ kx, cy - t / 2, kx + sw, cy + t - t / 2 };
			FillRect(dc, &h, brush(ctl(CB, 0)));
			RECT v{ kx + sw / 2 - t / 2, cy - sw, kx + sw / 2 + t - t / 2, cy + sw };
			FillRect(dc, &v, brush(ctl(CA, 0)));
			const int dw = 2 * d, dh = seg_h * 3 / 4, dy = sy + seg_h / 8;

			// 十の位は a/d/e/g がひとまとめ。f は使われない
			const bool ten_adeg = ctl(CA, 1);
			unsigned ten = 0;
			if (ten_adeg) ten |= (1u << 0) | (1u << 3) | (1u << 4) | (1u << 6);
			if (ctl(CB, 1)) ten |= 1u << 1;
			if (ctl(CA, 7)) ten |= 1u << 2;
			if (ctl(CB, 4)) ten |= 1u << 5;
			seven(kx + 3 * d, dw, dy, dh, ten, true);

			unsigned one = 0;
			if (ctl(CB, 6)) one |= 1u << 0;   // a
			if (ctl(CA, 6)) one |= 1u << 1;   // b
			if (ctl(CA, 3)) one |= 1u << 2;   // c
			if (ctl(CA, 2)) one |= 1u << 3;   // d
			if (ctl(CB, 2)) one |= 1u << 4;   // e
			if (ctl(CB, 7)) one |= 1u << 5;   // f
			if (ctl(CB, 3)) one |= 1u << 6;   // g
			seven(kx + 6 * d, dw, dy, dh, one, false);
		}

		// いちばん右。XG / TG300B(GS) / PERFORM のどれかを ▶ で示す。
		// PLG のぶんは C2 か D8 のどちらかだが、まだ決められていない
		{
			const int mx = mode_x, mw = mode_w;
			const bool mode[3] = { ctl(CB, 5), ctl(CA, 4), ctl(CA, 5) };
			for (int k = 0; k < 3; k++) {
				const int cy = sy + seg_h * (2 * k + 1) / 6;
				const int hh = std::max(2, seg_h / 8);
				const POINT tri[3] = { { mx, cy - hh }, { mx + mw, cy }, { mx, cy + hh } };
				HBRUSH bb = brush(mode[k]);
				HPEN p = CreatePen(PS_SOLID, 1, mode[k] ? LCD_DOT : LCD_GHOST);
				HGDIOBJ ob = SelectObject(dc, bb);
				HGDIOBJ op = SelectObject(dc, p);
				Polygon(dc, tri, 3);
				SelectObject(dc, ob);
				SelectObject(dc, op);
				DeleteObject(p);
			}
		}

		// 下の面の上に出る ▼ のカーソル。いま何を弄っているかを示す
		{
			const int cur_y = sy - std::max(2, int(2.5 * m_scale));
			const int hw = std::max(2, d);
			struct { int at; bool on; } cur[] = {
				{ LOW_VOL,  ctl(CC, 3) }, { LOW_EXP, ctl(CC, 4) },
				{ LOW_PAN,  ctl(CC, 5) }, { LOW_REV, ctl(CC, 6) },
				{ LOW_CHO,  ctl(CC, 7) }, { LOW_VAR, ctl(CD, 6) },
				{ LOW_KEY,  ctl(CD, 7) },
			};
			for (const auto &c : cur) {
				if (!c.on)
					continue;
				const int cx = lx(c.at) + lw(c.at) / 2;
				const POINT tri[3] = { { cx - hw, cur_y - hw }, { cx + hw, cur_y - hw },
				                       { cx, cur_y } };
				HGDIOBJ ob = SelectObject(dc, lit);
				Polygon(dc, tri, 3);
				SelectObject(dc, ob);
			}
			// バンク番号とプログラム番号のカーソルは**楽器のかたちの上**。
			// バンクは 4-5 列目、プログラムは 12-13 列目の上（実機を見て教わった）
			const int ix = lx(LOW_ICON);
			const int tops[2] = { ix + (3 + 5) * d / 2, ix + (11 + 13) * d / 2 };
			const bool ton[2] = { ctl(CC, 1), ctl(CC, 0) };
			for (int k = 0; k < 2; k++) {
				if (!ton[k])
					continue;
				const POINT tri[3] = { { tops[k] - hw, cur_y - hw },
				                       { tops[k] + hw, cur_y - hw },
				                       { tops[k], cur_y } };
				HGDIOBJ ob = SelectObject(dc, lit);
				Polygon(dc, tri, 3);
				SelectObject(dc, ob);
			}
		}
	}

	DeleteObject(ghost);
	DeleteObject(faint);
	DeleteObject(lit);

	if (s.message[0]) {
		RECT r = m_lcd;
		fill(dc, r, RGB(24, 26, 22));
		text_in(dc, r, s.message, RGB(210, 220, 200), m_font_label,
		        DT_CENTER | DT_VCENTER | DT_WORDBREAK);
	}
}

void panel::draw_button(HDC dc, const spot &sp, bool down) const
{
	round_box(dc, sp.r, down ? KEY_DOWN : KEY_FACE, KEY_EDGE, int(3 * m_scale));
}

// 大きなダイヤル。回した角度で窪みが回る
void panel::draw_wheel(HDC dc, int angle) const
{
	const POINT c = at(m_lay.dial[0], m_lay.dial[1]);
	const int r = int(m_lay.dial[2] * m_scale);

	// panel.txt で絵を渡されていれば、それを回して描く
	if (m_lay.dial_art) {
		m_lay.dial_art->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
		                     double(angle));
		return;
	}

	disc(dc, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(2 * m_scale)));
	const double a = angle * PI / 180.0;
	const int ox = c.x + int(std::sin(a) * r * 0.36);
	const int oy = c.y - int(std::cos(a) * r * 0.36);
	disc(dc, ox, oy, int(r * 0.45), RGB(186, 176, 140), RGB(146, 137, 106),
	     std::max(1, int(m_scale)));
}

// 音量つまみ
void panel::draw_volume(HDC dc, double v) const
{
	const POINT c = at(m_lay.volume[0], m_lay.volume[1]);
	const int r = int(m_lay.volume[2] * m_scale);
	const double deg = -135.0 + 270.0 * v;       // 左いっぱいから右いっぱいまで

	if (m_lay.volume_art) {
		m_lay.volume_art->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r }, deg);
	} else {
		disc(dc, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(m_scale)));
		const double a = deg * PI / 180.0;
		line(dc, c.x, c.y, c.x + int(std::sin(a) * r * 0.8),
		     c.y - int(std::cos(a) * r * 0.8), RGB(70, 64, 48),
		     std::max(2, int(2 * m_scale)));
	}
	text_in(dc, scale(m_lay.volume[0] - 36, m_lay.volume[1] + m_lay.volume[2] + 4,
	                  72, 12), "VOLUME", PANEL_INK, m_font_small,
	        DT_CENTER | DT_TOP | DT_SINGLELINE);
}


void panel::paint_front(HDC dc, const snapshot &s, u64 pressed, double volume,
                        const char *status) const
{
	RECT all{ 0, 0, m_w, m_h };
	fill(dc, all, RGB(24, 26, 30));
	fill(dc, scale(0, 0, LOGICAL_W, m_lay.body_h), PANEL_FACE);

	// ---- 飾り。位置も色も panel.txt から来る（doc/panel-editing.md）
	for (const deco &g : m_lay.decos) {
		if (g.k == deco::text)
			text_in(dc, scale(g.x, g.y, g.w, g.h), g.str.c_str(), g.a,
			        g.font ? m_font_label : m_font_small, g.align);
		else if (g.k == deco::disc) {
			const POINT c = at(g.x, g.y);
			disc(dc, c.x, c.y, int(g.w * m_scale), g.a, g.b,
			     std::max(1, int(g.h * m_scale)));
		} else if (g.k == deco::art) {
			if (g.pic)
				g.pic->draw(dc, scale(g.x, g.y, g.w, g.h));
		} else
			round_box(dc, scale(g.x, g.y, g.w, g.h), g.a, g.b,
			          std::max(1, int(g.radius * m_scale)));
	}

	// ---- 中

	draw_lcd(dc, s);

	// 窓の下の札は、下段の並びと同じ割合で置く。窓の中身とずれないように
	{
		const int pad = std::max(2, int(5 * m_scale));
		const int top_dots = TOP_COLS * (CELL_W + 1) - 1;
		const int aw = m_lcd.right - m_lcd.left;
		const int d = std::max<int>(1, (aw - pad * 2) / top_dots);
		const int inner_x = m_lcd.left + std::max(pad, (aw - top_dots * d) / 2);
		const int y = at(0, m_lay.columns_y).y, h = int(12 * m_scale), w = int(64 * m_scale);
		for (const column &c : COLUMNS) {
			const int cx = inner_x + (m_lay.low_x[c.at] + m_lay.low_w[c.at] / 2) * d;
			RECT r{ cx - w / 2, y, cx + w / 2, y + h };
			text_in(dc, r, c.label, PANEL_INK, m_font_small,
			        DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
	}

	for (int i = 0; i < 18; i++)
		text_in(dc, scale(m_lay.cat_x[i % 6] - 34, m_lay.cat_y[i / 6] - 14, 68, 14),
		        CAT_LABEL[i], PANEL_INK, m_font_small,
		        DT_CENTER | DT_TOP | DT_SINGLELINE);

	// MU / PLG-1..3 の表示灯。LED は 6 番から
	{
		const char *plg[4] = { "MU", "PLG-1", "PLG-2", "PLG-3" };
		for (int i = 0; i < 4; i++) {
			const double px = m_lay.plg[0] + i * m_lay.plg[1];
			const POINT c = at(px, m_lay.plg[2]);
			const bool on = BIT(s.leds, 6 + i) != 0;
			if (const svg_art *pic = m_lay.plg_art.pick(on, false)) {
				const int r = int(6 * m_scale);
				pic->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r });
			} else {
				// 実機の表示灯は四角
				const int r = int(4 * m_scale);
				round_box(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r },
				          on ? LED_ON : RGB(64, 62, 52), RGB(110, 106, 92), std::max(1, int(m_scale)));
			}
			text_in(dc, scale(px - 22, m_lay.plg[2] + 7, 44, 12), plg[i], PANEL_INK,
			        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
	}

	// ---- 右

	for (int i = 0; i < 6; i++) {
		const mode_button &m = MODES[i];
		const double mx = m_lay.mode[i][0], my = m_lay.mode[i][1];
		text_in(dc, scale(mx - 34, my - m_lay.mode_r - 19, 68, 14), m.label, PANEL_INK,
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		const POINT c = at(mx, my);
		const bool down = ((pressed >> int(m.b)) & 1) != 0;
		const bool on = BIT(s.leds, m.led) != 0;
		// panel.txt で絵を渡されていれば、ようすに合う 1 枚を貼る
		if (const svg_art *pic = m_lay.mode_art.pick(on, down)) {
			const int r = int(m_lay.mode_r * m_scale);
			pic->draw(dc, RECT{ c.x - r, c.y - r, c.x + r, c.y + r });
		} else {
			disc(dc, c.x, c.y, int(m_lay.mode_r * m_scale),
			     down ? KEY_DOWN : RGB(198, 188, 152), KEY_EDGE, std::max(1, int(m_scale)));
			disc(dc, c.x, c.y, int(m_lay.mode_led_r * m_scale),
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
		if (const svg_art *pic = m_lay.nav_art.pick(down, down))
			pic->draw(dc, sp->r);
		else
			draw_button(dc, *sp, down);
		text_in(dc, scale(px, py + 4, pw, 12), p.label, RGB(58, 53, 38),
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		if (p.sub[0])
			text_in(dc, scale(px, py + ph - 14, pw, 12), p.sub, RGB(58, 53, 38),
			        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
	}
	for (int i = 0; i < 18; i++) {
		RECT r = scale(m_lay.cat_x[i % 6] - m_lay.cat_w / 2, m_lay.cat_y[i / 6],
		               m_lay.cat_w, m_lay.cat_h);
		const bool down = ((pressed >> int(CAT_B[i])) & 1) != 0;
		if (const svg_art *pic = m_lay.cat_art.pick(down, down))
			pic->draw(dc, r);
		else
			round_box(dc, r, down ? KEY_DOWN : KEY_FACE, KEY_EDGE, int(3 * m_scale));
	}
	for (int i = 0; i < 2; i++) {
		const place &p = ROUND[i];
		const double px = m_lay.round_[i][0], py = m_lay.round_[i][1];
		const POINT c = at(px, py);
		const bool down = ((pressed >> int(p.b)) & 1) != 0;
		const int rr = int(m_lay.round_[i][2] / 2 * m_scale);
		if (const svg_art *pic = m_lay.round_art.pick(down, down))
			pic->draw(dc, RECT{ c.x - rr, c.y - rr, c.x + rr, c.y + rr });
		else
			disc(dc, c.x, c.y, rr, down ? KEY_DOWN : KEY_FACE, KEY_EDGE,
			     std::max(1, int(m_scale)));
		text_in(dc, scale(px - 40, py - 26, 80, 12), p.label, PANEL_INK,
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
	}

	draw_wheel(dc, m_wheel_angle);
	draw_volume(dc, volume);

	// 状態の行は本体の一番下（body_h の内側）に載るので、ボタンの名前と同じ濃い色で書く。
	// 前は暗い帯向けの薄い灰色で、本体の地の色に溶けて読めなかった
	if (status && status[0])
		text_in(dc, m_status, status, PANEL_INK, m_font_small,
		        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	text_in(dc, m_hint,
	        "大きなダイヤルはホイールで回す ／ ボタンはクリック ／ "
	        "キー: A=PLAY E=EDIT U=UTIL F=EFFECT [ ]=PART",
	        RGB(120, 124, 130), m_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	draw_tabs(dc);
}

// 論理座標の方眼。50 ごとに線、100 ごとに濃い線と数字を入れる。
// 絵の位置を直すときは、これを出して読み取ってから表を書き換える
void panel::draw_grid(HDC dc) const
{
	HPEN thin = CreatePen(PS_SOLID, 1, RGB(255, 80, 80));
	HPEN bold = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
	HGDIOBJ op = SelectObject(dc, thin);
	SetBkMode(dc, TRANSPARENT);

	for (int x = 0; x <= LOGICAL_W; x += 50) {
		SelectObject(dc, (x % 100) ? thin : bold);
		const POINT a = at(x, 0), b = at(x, LOGICAL_H);
		MoveToEx(dc, a.x, a.y, nullptr);
		LineTo(dc, b.x, b.y);
	}
	for (int y = 0; y <= LOGICAL_H; y += 50) {
		SelectObject(dc, (y % 100) ? thin : bold);
		const POINT a = at(0, y), b = at(LOGICAL_W, y);
		MoveToEx(dc, a.x, a.y, nullptr);
		LineTo(dc, b.x, b.y);
	}
	for (int x = 0; x <= LOGICAL_W; x += 100)
		for (int y = 0; y <= LOGICAL_H; y += 100) {
			char n[32];
			std::snprintf(n, sizeof(n), "%d,%d", x, y);
			RECT r{ at(x + 2, y + 1).x, at(x + 2, y + 1).y,
			        at(x + 60, y + 12).x, at(x + 60, y + 12).y };
			text_in(dc, r, n, RGB(200, 0, 0), m_font_small,
			        DT_LEFT | DT_TOP | DT_SINGLELINE);
		}

	SelectObject(dc, op);
	DeleteObject(thin);
	DeleteObject(bold);
}

void panel::paint(HDC dc, const snapshot &s, u64 pressed, const char *status) const
{
	if (m_lcd_only) {
		RECT client{ 0, 0, m_w, m_h };
		fill(dc, client, RGB(24, 25, 27));
		draw_lcd(dc, s);
		return;
	}
	if (m_page == page::editor)       paint_editor(dc, status);
	else if (m_page == page::effects) paint_effects(dc, status);
	else                              paint_front(dc, s, pressed, m_volume_now, status);
	if (m_grid)
		draw_grid(dc);
}

} // namespace ui
