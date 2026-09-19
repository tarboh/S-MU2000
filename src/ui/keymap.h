// license:BSD-3-Clause
//
// The shared keyboard map: physical key to panel button.
//
// Callers hand over a lowercase character code and get the panel button for
// it. Windows translates its virtual-key codes to characters first (letters
// arrive uppercase, punctuation arrives as OEM codes); macOS hands over the
// character with Shift already stripped, so both '=' and '+' are listed.
// Only the character domain is shared here: which keycode means which
// character stays per platform (notably OEM codes and JP layouts).
//
// Needs mu2000.h for mu2000::button (every includer already has it).

#ifndef S_MU2000_UI_KEYMAP_H
#define S_MU2000_UI_KEYMAP_H

#pragma once

#include "mu2000.h"

namespace ui {

// The one table that decides (matches gui.cpp). Takes the key code as an int
// and ignores anything outside ASCII: narrowing a 16-bit unichar to char
// first would let accented characters alias panel keys (U+0161 shares its
// low byte with 'a' = PLAY). Returns false for anything that is not a panel
// key, leaving out untouched.
inline bool button_for_char(int c, mu2000::button &out)
{
	if (c < 0 || c > 0x7f)
		return false;
	switch (c) {
	case 'a': out = mu2000::button::play;         return true;
	case 'e': out = mu2000::button::edit;         return true;
	case 'u': out = mu2000::button::util;         return true;
	case 'f': out = mu2000::button::effect;       return true;
	case 's': out = mu2000::button::mute_solo;    return true;
	case ']': out = mu2000::button::part_plus;    return true;
	case '[': out = mu2000::button::part_minus;   return true;
	case '=': case '+': out = mu2000::button::value_plus;  return true;
	case '-': out = mu2000::button::value_minus;  return true;
	case '\r': out = mu2000::button::enter;       return true;
	case 0x7f: case 0x08: out = mu2000::button::exit; return true;
	case '.': out = mu2000::button::select_right; return true;
	case ',': out = mu2000::button::select_left;  return true;
	case 'q': out = mu2000::button::seq;          return true;
	case 'z': out = mu2000::button::audition;     return true;
	case 'x': out = mu2000::button::select;       return true;
	case 'm': out = mu2000::button::sampling_mode; return true;
	default: break;
	}
	return false;
}

} // namespace ui

#endif // S_MU2000_UI_KEYMAP_H
