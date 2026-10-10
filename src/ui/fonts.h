// license:BSD-3-Clause
//
// The six font sizes the panel draws its lettering with, and nothing else.
//
// This is its own header on purpose. im::fonts is a handful of ImFont pointers
// and their pixel sizes, so it needs imgui.h and nothing else -- in particular
// it must not drag in compat/gdi.h, which on Windows includes <windows.h>.
// vst3/view.h names im::fonts in one signature, and the VST/VST3 SDK headers
// have a member called `interface`; windows.h defines that as a macro for
// `struct` (wingdi.h), so a panel drawing header reaching the SDK's own
// translation units broke the VSTi build with "expected unqualified-id before
// 'struct'". Keep this header ImGui-only and the SDK side stays clean.

#ifndef S_MU2000_UI_FONTS_H
#define S_MU2000_UI_FONTS_H

#pragma once

#include "imgui.h"

// Windows SDK rpcndr.h (reached through the windows.h chains of every plugin
// TU on MSVC; MinGW skips it) still carries the legacy `#define small char`.
// Neutralize it before this struct names a member `small`, and the drawing
// code can then say f.small again: rpcndr is never re-included after windows.h
// once, and this header ships with the member declaration it must protect.
#if defined(_MSC_VER) && defined(small)
#undef small
#endif

namespace ui {
namespace im {

// label: the button legends and the status line
// small:  the LCD's MIC/LINE/BANK/PGM# boxes and the hint
// tiny:   the LCD's tick numbers, 4-9 px
// key:    the key-top printing
// tag:    the small legends on the panel art
// num:    the numeric readouts
//
// bar: the window's top strip, which has no slot in the panel's six (it is
// window furniture, not panel lettering) and is fixed-size by design -- it does
// not scale with the window, so a resize must not make it jump around. This is
// the Latin size; toolbar::paint scales CJK labels up from it, since one em
// cannot fill a 26 px band for both scripts.
//
// A null slot is not an error: the text helpers in draw_imgui.h fall back to
// ImGui's current font, which is the 16 px default and about twice the size the
// panel means. It only happens when build_fonts() has not run yet.
struct fonts {
	ImFont *label = nullptr, *small = nullptr, *tiny = nullptr;
	ImFont *key = nullptr, *tag = nullptr, *num = nullptr;
	float label_px = 13.0f, small_px = 8.5f, tiny_px = 6.5f;
	float key_px = 9.0f, tag_px = 8.0f, num_px = 8.0f;
	float bar_px = 16.0f;
};

} // namespace im
} // namespace ui

#endif // S_MU2000_UI_FONTS_H
