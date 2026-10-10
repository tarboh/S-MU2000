// license:BSD-3-Clause
//
// 画面。実機のフロントパネルと、SOL2 風のエディタの 2 面を持つ。
//
// exe（gui.exe）と VST3 の画面で同じものを使う。描画は Dear ImGui で、
// 窓側は ImDrawList を渡してもらうだけにしてある。
//
// 配置は論理座標（LOGICAL_W × LOGICAL_H）で持ち、窓の大きさに合わせて
// 一律に拡大縮小する。実機の寸法をそのまま写したものではなく、
// 実機の並び（LCD が左、音色カテゴリが真ん中、VALUE が右）に倣った配置。
//
// 入力も面ごとに違うので、窓側は press/drag/release/wheel_at をそのまま
// 渡すだけでよい。中で MIDI が要るものは bridge に積む。

#ifndef S_MU2000_UI_PANEL_H
#define S_MU2000_UI_PANEL_H

#pragma once

#include "bridge.h"
#include "layout.h"
#include "snapshot.h"
#include "xg/model.h"
#include "ui/draw_imgui.h"
#include "ui/tex.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

// Geometry and colors (RECT, COLORREF); drawing itself is Dear ImGui.
#include "compat/gdi.h"

struct ImDrawList;
struct ImGuiContext;

namespace ui {

namespace im {
struct fonts;
}

enum class page { front, editor, effects };

// 触れる場所が何を動かすか
enum : int {
	CTL_NONE      = -1,
	CTL_KNOB      = 200,   // +0..17 エディタのつまみ（editor.cpp の KNOBS の並び）
	CTL_PART      = 300,   // +0..31
	CTL_TAB_FRONT = 400,
	CTL_TAB_EDIT  = 401,
	CTL_TAB_FX    = 404,
	CTL_XG_RESET  = 402,
	CTL_ALL_OFF   = 403,

	// エフェクト面。XG のシステムエフェクトと MU の インサーション 2 系統
	CTL_REV_TYPE  = 500, CTL_REV_RET  = 501,
	CTL_CHO_TYPE  = 502, CTL_CHO_RET  = 503,
	CTL_VAR_TYPE  = 504, CTL_VAR_CONN = 505, CTL_VAR_PART = 506,
	CTL_INS1_TYPE = 507, CTL_INS1_PART = 508,
	CTL_INS2_TYPE = 509, CTL_INS2_PART = 510,
};

// 触れる場所
enum class spot_kind { none, button, wheel, volume, adgain, knob, tab, action, part, list };

struct spot {
	spot_kind      kind = spot_kind::none;
	mu2000::button button = mu2000::button::count;
	int            ctl = CTL_NONE;
	RECT           r{};
	const char    *label = "";
	const char    *sub   = "";     // 小さく添える字。無ければ空
};

class panel
{
public:
	panel();
	~panel();

	// 窓の大きさが変わったら呼ぶ
	void resize(int w, int h);
	int  width() const  { return m_w; }
	int  height() const { return m_h; }

	// Make the panel's fonts. ImGui's context has to exist and no frame may be
	// open, so the window code calls this once its context is up: on macOS the
	// window settles on its final size inside setFrameSize:, before the context
	// exists, so resize() alone would leave the panel with no fonts at all (and
	// every label then falls back to ImGui's 16 px default). Safe to repeat --
	// build_fonts() does nothing unless a size actually moved.
	void fonts_ready();


	// 音量つまみの見え方。音源側の値をそのまま渡してもらう
	void set_volume(double v) { m_volume_now = v; }

	// 論理座標の方眼を重ねる。絵の位置を直すときの物差し（doc/panel-editing.md）
	void set_grid(bool on) { m_grid = on; }
	void set_lcd_only(bool on) { m_lcd_only = on; }


	// **上に空ける高さ**（画素）。窓の最上段にボタンの帯を出す
	// ときに使う（`ui/toolbar.h`）。絵は 1000 × 385 の全面を使っていて
	// 空きが無いので、重ねると絵が隠れてしまう。**`resize()` をやり直すこと**
	void set_top_inset(int px) { m_top_inset = px < 0 ? 0 : px; }
	int  top_inset() const { return m_top_inset; }

	// 配置。**作り直さずに文字ファイルで直せる**（doc/panel-editing.md）。
	// 読み直したら resize() をやり直すこと
	layout       &lay()       { return m_lay; }
	const layout &lay() const { return m_lay; }

	// 実際の窓の座標から、触れる場所を探す
	const spot *hit(int x, int y) const;

	// パネルに描いてある MIDI IN A のジャック。窓側はここを押されたら
	// 入出力の口を選ぶ品書きを出す
	RECT midi_jack() const;
	bool on_midi_jack(int x, int y) const;
	// カードの差し込み口と A/D INPUT のジャック。押すと品書きが出る
	bool on_card_slot(int x, int y) const;
	bool on_ad_input(int x, int y) const;
	// PHONES のジャック。押すと音の出口（デジタル / アナログ）を選ぶ品書きが出る
	bool on_phones(int x, int y) const;
	// 電源スイッチ。押すと「起動し直す」の品書きが出る（絵に無いレイアウトでは当たらない）
	bool on_power(int x, int y) const;

	// ---- 入力。窓からそのまま渡す。戻り値は「描き直しが要るか」

	bool press(int x, int y, bridge &br);
	bool drag(int x, int y, bridge &br);
	bool release(bridge &br);
	bool wheel_at(int x, int y, int delta, bridge &br);

	// 画面の糸のタイマーから、描く前に呼ぶ。音声の糸が写したワーク RAM を
	// パラメータの層に読ませる（問い合わせはしない）。戻り値は「新しい値を読んだか」
	bool tick(bridge &br);

	// 描く。status は下に小さく出す 1 行。無ければ空でよい。
	// 字は面板が持つ im::fonts（resize() が作る）。窓側の帯と品書きは
	// 16 px の固定の組を使うので，这里的 f は渡さない
	void paint_front(ImDrawList *dl, const snapshot &s, u64 pressed,
	                 double volume, const char *status) const;
	void paint_editor(ImDrawList *dl, const char *status) const;
	void paint_effects(ImDrawList *dl, const char *status) const;
	// LCD だけを出すモード（--lcd-only）。それ以外は面のどれか
	void paint(ImDrawList *dl, const snapshot &s, u64 pressed, const char *status) const;

	// パラメータの層の写し。PC エディタも同じものを読み書きする（tick が回している）
	xg::model &xg() { return m_xg; }
	const xg_snapshot &ram() const { return m_ram; }

private:
	RECT scale(double x, double y, double w, double h) const;
	POINT at(double x, double y) const;
	void build_spots();
	void build_editor_spots();
	void build_effect_spots();

	// ---- The panel's own fonts. Six sizes, worked out from the window scale
	// and the LCD's dimensions; nothing is rebuilt unless one of them moved
	// (ImGui rasterizes per size).
	//
	// **Also called while painting.** On macOS the window settles on its final
	// size during setFrameSize:, before the ImGui context exists, so a panel
	// that only ever built them from resize() ended up with none at all -- and
	// every label then fell back to ImGui's 16 px default, which is about twice
	// the size the panel means. Painting is the first moment a context is
	// guaranteed to be there.
	void build_fonts() const;
	void drop_fonts() const;

	// ---- LCD
	// The dimensions inside the LCD window. d is the dot pitch, x0/y0 the top
	// left of the upper face and sy the top of the lower one; fx0/fy0/fsy are
	// the same numbers unrounded. The legends below the window take their
	// positions from the same numbers, so the legend and the thing it names
	// cannot drift apart.
	//
	// df is the pitch the grid is actually laid out on, a multiple of 1/k, and
	// k is how many times the LCD is drawn before being averaged back down (see
	// supersample_lcd). Rounding df to whole pixels would leave up to a whole
	// dot of blank margin -- a fifth of the window at the sizes you get at
	// startup.
	struct lcd_geom {
		int    d, pad;                 // dot pitch, border
		double df;                     // dot pitch, a multiple of 1/k
		int    k;                      // the magnification df is a multiple of
		double fx0, fy0, fsy;
		int    x0, y0, sy;             // the same, rounded to whole pixels
		int    tick_h, line_h, scale_h;
	};
	lcd_geom lcd_grid() const;
	// Height in the tick band, as a fraction: 0 is the band's top, 1 its bottom
	int band_y(const lcd_geom &g, double f) const;
	// The MIC and LINE boxes (MIC on top)
	void lcd_tag_boxes(const lcd_geom &g, RECT out[2]) const;

	// Where the LCD's fills land: the draw list at 1:1, or a k-times buffer
	// that draw_lcd averages back down. The curved segments go into the draw
	// list either way, so they stay smooth at 1:1.
	struct lcd_canvas;
	// Draw the LCD at k times its size and average each k x k block back down.
	// Returns false if there is nowhere to put the result.
	bool supersample_lcd(ImDrawList *dl, const snapshot &s, const lcd_geom &g,
	                     int k) const;
	// The averaged LCD picture. Sized on resize, re-filled every frame.
	mutable std::unique_ptr<im::tex> m_lcd_tex;
	// Its scratch too. These are big (w*k by h*k at k=3 is a couple of MB) and
	// only change shape on a resize, so they are kept rather than allocated and
	// freed every frame. Upstream creates two DIBs per frame instead.
	mutable std::vector<uint32_t> m_lcd_big, m_lcd_flat;

	void draw_lcd(ImDrawList *dl, const snapshot &s) const;
	void draw_lcd_body(lcd_canvas &cv, const snapshot &s, const lcd_geom &g,
	                   const RECT &area, double px) const;
	void draw_lcd_labels(ImDrawList *dl, const snapshot &s, const lcd_geom &g) const;
	void draw_lcd_message(ImDrawList *dl, const snapshot &s) const;

	void draw_grid(ImDrawList *dl) const;
	void draw_button(ImDrawList *dl, const spot &sp, bool down) const;
	// Key-top printing, for when the labels come from the art instead of the
	// code (panel.txt labels_in_art)
	void draw_key_print(ImDrawList *dl, const RECT &key, const char *label,
	                    const char *sub, mu2000::button b, bool down) const;
	void draw_wheel(ImDrawList *dl, int angle) const;
	void draw_volume(ImDrawList *dl, double v) const;
	void draw_adgain(ImDrawList *dl) const;
	void draw_card(ImDrawList *dl) const;   // 差さっている SmartMedia の縁
	void draw_tabs(ImDrawList *dl) const;
	void draw_knob(ImDrawList *dl, const spot &sp) const;
	void draw_list(ImDrawList *dl, const spot &sp) const;
	std::string fx_text(int ctl) const;
	void fx_bounds(int ctl, bool &at_min, bool &at_max) const;
	void step_fx(int ctl, int step, bridge &br);
	// ダイヤルを掴んで上下に動かす（editor.cpp）
	bool dial_follow(int y, bridge &br);

	const xg::param *knob_param(int ctl) const;
	bool value_of(int ctl, int &v) const;
	void set_value(int ctl, int v, bridge &br);
	// A still-pending minimum-hold release, completed now (press calls this first)
	void flush_release(bridge &br);

	int m_w = LOGICAL_W, m_h = LOGICAL_H;
	double m_scale = 1.0;
	int m_ox = 0, m_oy = 0;      // 縦横比を保つための余白
	int m_top_inset = 0;         // 帯のために上へ空ける高さ

	page m_page = page::front;
	std::vector<spot> m_spots;
	RECT m_adgain{};
	RECT m_lcd{}, m_wheel{}, m_volume{}, m_status{}, m_leds[6]{};

	// 掴んでいるもの
	const spot *m_held = nullptr;
	// A momentary button stays down a minimum time once pressed. A tap shorter
	// than an audio block would otherwise never reach the firmware (which
	// samples the button matrix once per block) and never paint lit.
	// m_press_at is stamped on press; release() within MIN_HOLD only marks
	// m_release_pending, and tick() completes it. A press in between flushes
	// it first, so it can never strand a button.
	// The pending release remembers the button by value (m_release_btn), not
	// via m_held: build_spots() nulls m_held on every layout rebuild, and a
	// pointer-keyed pending release would strand there.
	std::chrono::steady_clock::time_point m_press_at{};
	mu2000::button m_release_btn = mu2000::button::count;
	bool m_release_pending = false;
	static constexpr std::chrono::milliseconds MIN_HOLD{100};
	int  m_drag_x = 0, m_drag_y = 0, m_drag_from = 0;
	int  m_wheel_angle = 0;
	// ダイヤルを掴んで上下に動かしているとき（editor.cpp の press / drag）。まだ目盛りにならない端の画素
	double m_dial_rest = 0.0;
	double m_volume_now = 1.0;
	// The A/D INPUT knob (0-1). It only turns for now, it drives nothing
	double m_adgain_now = 0.45;

	// ---- エディタとエフェクトの面の値。**画面では覚えない**。
	// 音源に問い合わせた返事をパラメータの層（doc/params.md）が持っていて、
	// 描くときはそこを読む。パネルや曲が変えた値もそのまま出る
	int m_part = 0;
	xg::model m_xg;
	xg_snapshot m_ram;           // 音声の糸が写した RAM（画面の糸だけが触る）
	u64 m_ram_serial = 0;

	// ---- 字
	mutable im::fonts m_fonts;
	mutable int m_font_px[6] = {};        // label small tiny key tag num。0 ならまだ無い
	mutable ImGuiContext *m_font_ctx = nullptr;   // the context the six were added to

	// キートップの記号（− ＋ ◀ ▶）。縁をぼかした絵。大きさが変わるたびに作る
	std::shared_ptr<svg_art> m_key_sym[4];
	int m_key_sym_px = 0;


	bool   m_grid = false;
	bool   m_lcd_only = false;
	layout m_lay;
};

} // namespace ui

#endif // S_MU2000_UI_PANEL_H
