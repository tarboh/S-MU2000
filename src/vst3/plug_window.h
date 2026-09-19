// license:BSD-3-Clause
//
// The seam between the VST3 view and the window it is drawn in.
//
// IPlugView.attached() is handed whatever the host uses as a parent: an HWND on
// Windows, an NSView on macOS. The panel then has to be drawn inside it and fed
// its mouse and key events. Everything else about the view -- the VST3
// interface itself, the panel, the input handling -- is shared, so only the
// window part is per platform.
//
// Same constraint as the GUI: this header is included by view_mac.mm, so it
// must not mention a single Windows type, and must not pull in compat/gdi.h
// (Cocoa's headers define BOOL and Quickdraw's define Polygon).

#ifndef S_MU2000_VST3_PLUG_WINDOW_H
#define S_MU2000_VST3_PLUG_WINDOW_H

#pragma once

#include <string>

// PC で触る窓（一覧・エディタ）に渡すもの。型の中身はここでは要らない
namespace xg { class model; }
namespace ui { class bridge; struct xg_snapshot; }

namespace smu2000 {
namespace vst3 {

class plug_view;

// The panel buttons a keyboard can reach, as mu2000::button values.
//
// Each platform window maps its own key codes onto these through the shared
// ui/keymap.h table, so the meaning of a key lives in one place instead of
// once per window system.

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
};

// The platform type string this build answers to: kPlatformTypeHWND on
// Windows, kPlatformTypeNSView on macOS
const char *plug_window_type();

// Makes this platform's window. The caller owns it
plug_window *plug_window_create(plug_view &owner);

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_PLUG_WINDOW_H
