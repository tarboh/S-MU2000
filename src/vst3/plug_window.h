// license:BSD-3-Clause
//
// The seam between the VST3 view and the window it is drawn in.
//
// IPlugView.attached() is handed whatever the host uses as a parent: an HWND on
// Windows, an NSView on macOS. The panel then has to be drawn inside it and fed
// its mouse and key events. Everything else about the view -- the VST3
// interface itself, the panel, the input handling -- is shared, so only the
// window part is per platform.

#ifndef S_MU2000_VST3_PLUG_WINDOW_H
#define S_MU2000_VST3_PLUG_WINDOW_H

#pragma once

#include <string>

// PC で触る窓（一覧・エディタ）に渡すもの。型の中身はここでは要らない
namespace xg { class model; }
namespace ui { class bridge; struct xg_snapshot; class pc_window; }

namespace smu2000 {
namespace vst3 {

class plug_view;

// The panel buttons a keyboard can reach, as mu2000::button values.
//
// Each platform maps its own key codes onto these, and view.cpp maps these onto
// mu2000::button. Doing it in two steps keeps the meaning of a key in one place
// instead of once per window system. The letter keys additionally share their
// meaning with the GUI front ends through ui/keymap.h: each platform window
// turns its key codes into characters first, then into these.
enum plug_key {
	PLUG_KEY_NONE = 0,
	PLUG_KEY_PLAY,
	PLUG_KEY_EDIT,
	PLUG_KEY_UTIL,
	PLUG_KEY_EFFECT,
	PLUG_KEY_MUTE_SOLO,
	PLUG_KEY_PART_MINUS,
	PLUG_KEY_PART_PLUS,
	PLUG_KEY_VALUE_MINUS,
	PLUG_KEY_VALUE_PLUS,
	PLUG_KEY_ENTER,
	PLUG_KEY_EXIT,
	PLUG_KEY_SELECT_LEFT,
	PLUG_KEY_SELECT_RIGHT,
	PLUG_KEY_SEQ,
	PLUG_KEY_AUDITION,
	PLUG_KEY_SELECT,
	PLUG_KEY_SAMPLING_MODE,
	// Not panel buttons: these open the PC windows, the way F3 and F2 do
	// in gui.exe. The view turns them into plug_window::open_* instead of
	// a mu2000::button
	PLUG_KEY_LIST,
	PLUG_KEY_EDITOR,
	// Toggles the native engine, the way F4 does in gui.exe
	PLUG_KEY_ENGINE,
};

// Which PC window. The button bar and the keys both name them this way
enum pc_kind {
	PC_LIST = 0,      // 一覧
	PC_EDITOR,        // エディタ
	PC_FX,            // インサーションの設定
	PC_SHAPES,        // パートの音色
	PC_MASTER,        // マスター
	PC_SAMPLING,      // サンプリング
	PC_BOARDS = 7,    // プラグインボード（ui::BAR_BOARD と同じ番号。issue #221）
};


// Which PC window a kind (pc_kind, same values as the toolbar bar_window
// ids) names. One place so the two platform windows cannot map one button
// to different windows. Defaults to the overview, as both did before.
inline ui::pc_window *pc_window_for_kind(int kind, ui::pc_window &list,
                                         ui::pc_window &editor, ui::pc_window &fx,
                                         ui::pc_window &shapes, ui::pc_window &master,
                                         ui::pc_window &sampling)
{
	switch (kind) {
	case PC_EDITOR: return &editor;
	case PC_FX:     return &fx;
	case PC_SHAPES: return &shapes;
	case PC_MASTER: return &master;
	case PC_SAMPLING: return &sampling;
	default:        return &list;
	}
}

// A real window on the host's platform, holding the panel
class plug_window
{
public:
	virtual ~plug_window() = default;

	// Put the window inside the host's parent. False if it cannot be done,
	// which makes attached() report failure rather than pretend it worked
	virtual bool attach(void *parent, int w, int h) = 0;
	virtual void detach() = 0;
	virtual void set_size(int w, int h) = 0;
	// ホストの窓の拡大率（Windows: その窓の DPI / 96）。分からなければ 1
	virtual double host_scale() const { return 1.0; }

	// The SmartMedia menu, at a point inside the panel. A popup menu and a file
	// dialog are native on both platforms, so the window builds them and calls
	// back into the view's card_* methods to act on the choice
	virtual void card_menu(int x, int y) = 0;

	// Say something went wrong (the panel has nowhere to put it)
	virtual void alert(const std::string &text) = 0;

	// A right click that missed the card slot. The GUI front end opens its
	// settings menu there; a plug-in has no settings of its own, so by
	// default there is no menu. Platforms with PC windows of their own
	// offer those here instead
	virtual void panel_menu(int x, int y) {}

	// Drive the PC windows (overview, editor, insertion, part voice) one
	// frame. Same ui::pc_window as gui.exe, opened from the panel's
	// right-click menu. Called from the GUI thread at the panel's repaint
	// rate; does nothing while no window is visible
	virtual void pc_frame(::xg::model &, const ::ui::xg_snapshot &, ::ui::bridge &) {}

	// Open one of those windows, from a key (F3 / F2) or from the button
	// bar at the top of the panel. Platforms without PC windows leave this
	// alone
	virtual void open_pc_window(int kind) {}
};

// The platform type string this build answers to: kPlatformTypeHWND on
// Windows, kPlatformTypeNSView on macOS
const char *plug_window_type();

// Makes this platform's window. The caller owns it
plug_window *plug_window_create(plug_view &owner);

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_PLUG_WINDOW_H
