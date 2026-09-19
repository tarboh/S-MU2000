// license:BSD-3-Clause
//
// The seam between the macOS app and the window that shows it.
//
// It exists because the two cannot live in one translation unit: Cocoa's
// headers define BOOL and Quickdraw's define Polygon, and compat/gdi.h has to
// declare both of those to keep panel.cpp unchanged. So the Cocoa half is a
// .mm that never includes gdi.h, and everything else stays plain C++.
//
// Nothing in this header mentions AppKit. window_mac.mm implements it; that is
// the only file in the project compiled as Objective-C++.

#ifndef S_MU2000_UI_WINDOW_MAC_H
#define S_MU2000_UI_WINDOW_MAC_H

#pragma once

#include "menu.h"

#include <string>
#include <vector>

namespace ui {

// Function keys arrive as MAC_KEY_FUNCTION_BASE + the Carbon key code, because
// the app maps keys to panel buttons and has no business knowing about NSEvent
constexpr int MAC_KEY_FUNCTION_BASE = 0x10000;

// The menu lines themselves (menu_item/menu_group) are shared with the
// Windows front end in menu.h; only showing them is AppKit's business.

// What the window asks the app to do
class mac_app
{
public:
	virtual ~mac_app() = default;

	// Paint the whole panel into this context. The context is already
	// top-left origin with y running down, so it is handed straight to
	// compat/gdi.h's smu_gdi_wrap_view_context().
	virtual void draw(void *cg_context, int w, int h) = 0;

	virtual void resized(int w, int h) = 0;

	// Mouse. right is true for a secondary click, which is how the port
	// picker is reached on this platform.
	//
	// Returns true when this press should open a popup rather than press a
	// panel button -- the MIDI jack and the card slot are pressed, not clicked,
	// and a secondary click anywhere opens the port picker. The window shows
	// whatever context_menu() then describes
	virtual bool mouse_down(int x, int y, bool right) = 0;
	virtual void mouse_drag(int x, int y) = 0;
	virtual void mouse_up() = 0;
	virtual void wheel(int x, int y, int steps) = 0;

	// Keys: a character where there is one, otherwise the sentinel above
	virtual void key(int code, bool down) = 0;

	virtual void focus_lost() = 0;

	// A file was dropped onto the window (a MIDI file, in practice). Not pure:
	// a front end that has nothing to do with a drop does not have to say so
	virtual void file_dropped(const std::string &path) { (void)path; }

	// Draw a pointing hand here, i.e. this spot opens something
	virtual bool hand_cursor(int x, int y) = 0;

	// The popup for this point. Empty means show nothing
	virtual std::vector<menu_group> context_menu(int x, int y) = 0;
	virtual void menu_chosen(int id) = 0;

	virtual void reload_layout() = 0;

	// How often to repaint, in milliseconds
	virtual int frame_ms() const { return 33; }
};

// Runs the open panel and returns the chosen path, or "" if it was cancelled.
// A file panel belongs to the window system, so the app asks for it by name
// rather than reaching for AppKit itself
std::string open_midi_file_panel();

// The same for a file of some other kind. ext is a filename extension (no dot)
// and is only a hint: the panel lets anything through, because the files the
// machine writes are not registered with the system
std::string open_file_panel(const char *title, const char *ext);

// Asks where to save a new file of some other kind. "" if it was cancelled
std::string save_file_panel(const char *title, const char *default_name, const char *ext);

// Asks a yes/no question and returns true only when the user accepts. Used
// before the settings are thrown away, so the buttons are ordered for the
// answer that changes nothing: Cancel is the default, and Return picks it
bool confirm_modal(const char *title, const char *message, const char *ok_label);

// Tells the user something they only have to acknowledge. confirm_modal's
// counterpart for a message with no choice in it (inserting a blank
// SmartMedia, for instance)
void alert_modal(const char *title, const char *message);

// Makes the window and pumps events until it closes. Blocks
void run_window(mac_app &app, const char *title, int w, int h);

} // namespace ui

#endif // S_MU2000_UI_WINDOW_MAC_H
