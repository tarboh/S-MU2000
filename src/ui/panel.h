// license:BSD-3-Clause
//
// 画面。実機のフロントパネルと、SOL2 風のエディタの 2 面を持つ。
//
// exe（gui.exe）と VST3 の画面で同じものを使う。どちらも Windows なので
// 描画は GDI で済ませ、外からは HDC を 1 枚渡してもらうだけにしてある。
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

#include <chrono>
#include <string>
#include <vector>

// Real GDI on Windows, the CoreGraphics shim on macOS. Either way the panel
// only ever draws in GDI's coordinates.
#include "compat/gdi.h"

namespace ui {

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
	CTL_FX_FIRST  = 500, CTL_FX_COUNT = 11,
};

// 触れる場所
enum class spot_kind { none, button, wheel, volume, knob, tab, action, part, list };

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

	page current_page() const { return m_page; }

	// 音量つまみの見え方。音源側の値をそのまま渡してもらう
	void set_volume(double v) { m_volume_now = v; }

	// 論理座標の方眼を重ねる。絵の位置を直すときの物差し（doc/panel-editing.md）
	void set_grid(bool on) { m_grid = on; }
	void set_lcd_only(bool on) { m_lcd_only = on; }

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

	// ---- 入力。窓からそのまま渡す。戻り値は「描き直しが要るか」

	bool press(int x, int y, bridge &br);
	bool drag(int x, int y, bridge &br);
	bool release(bridge &br);
	bool wheel_at(int x, int y, int delta, bridge &br);

	// 画面の糸のタイマーから、描く前に呼ぶ。音声の糸が写したワーク RAM を
	// パラメータの層に読ませる（問い合わせはしない）。戻り値は「新しい値を読んだか」
	bool tick(bridge &br);

	// 描く。status は下に小さく出す 1 行。無ければ空でよい
	void paint(HDC dc, const snapshot &s, u64 pressed, const char *status) const;

	const std::vector<spot> &spots() const { return m_spots; }

	// パラメータの層の写し。PC エディタも同じものを読み書きする（tick が回している）
	xg::model &xg() { return m_xg; }
	const xg_snapshot &ram() const { return m_ram; }

private:
	RECT scale(double x, double y, double w, double h) const;
	POINT at(double x, double y) const;
	void build_spots();
	void build_editor_spots();
	void build_effect_spots();

	void paint_front(HDC dc, const snapshot &s, u64 pressed, double volume,
	                 const char *status) const;
	void paint_editor(HDC dc, const char *status) const;
	void paint_effects(HDC dc, const char *status) const;

	void draw_lcd(HDC dc, const snapshot &s) const;
	void draw_grid(HDC dc) const;
	void draw_button(HDC dc, const spot &sp, bool down) const;
	void draw_wheel(HDC dc, int angle) const;
	void draw_volume(HDC dc, double v) const;
	void draw_tabs(HDC dc) const;
	void draw_knob(HDC dc, const spot &sp) const;
	void draw_list(HDC dc, const spot &sp) const;
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

	page m_page = page::front;
	std::vector<spot> m_spots;
	RECT m_lcd{}, m_wheel{}, m_volume{}, m_status{}, m_hint{}, m_leds[6]{};

	// 掴んでいるもの
	const spot *m_held = nullptr;
	// A momentary button stays down a minimum time once pressed. A tap shorter
	// than an audio block would otherwise never reach the firmware (which
	// samples the button matrix once per block) and never paint lit.
	// m_press_at is stamped on press; release() within MIN_HOLD only marks
	// m_release_pending, and tick() completes it. A press in between flushes
	// it first, so it can never strand a button.
	std::chrono::steady_clock::time_point m_press_at{};
	bool m_release_pending = false;
	static constexpr std::chrono::milliseconds MIN_HOLD{100};
	int  m_drag_x = 0, m_drag_y = 0, m_drag_from = 0;
	int  m_wheel_angle = 0;
	// ダイヤルを掴んで上下に動かしているとき（editor.cpp の press / drag）。まだ目盛りにならない端の画素
	double m_dial_rest = 0.0;
	double m_volume_now = 1.0;

	// ---- エディタとエフェクトの面の値。**画面では覚えない**。
	// 音源に問い合わせた返事をパラメータの層（doc/params.md）が持っていて、
	// 描くときはそこを読む。パネルや曲が変えた値もそのまま出る
	int m_part = 0;
	xg::model m_xg;
	xg_snapshot m_ram;           // 音声の糸が写した RAM（画面の糸だけが触る）
	u64 m_ram_serial = 0;

	HFONT m_font_label = nullptr, m_font_small = nullptr;
	// 目盛りの番号用。バー 1 本ぶんの幅に 2 桁を収める
	HFONT m_font_tiny  = nullptr;
	bool   m_grid = false;
	bool   m_lcd_only = false;
	layout m_lay;
};

} // namespace ui

#endif // S_MU2000_UI_PANEL_H
