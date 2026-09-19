// license:BSD-3-Clause
//
// Windows virtual-key codes translated to the characters the shared keymap
// (ui/keymap.h) wants. Mirrors ui/menu_win.h: menus render there, keys here.
//
// Windows-only: includes <windows.h> for the VK_ codes. Header-only (inline)
// so no build system changes are needed.

#ifndef S_MU2000_UI_KEYMAP_WIN_H
#define S_MU2000_UI_KEYMAP_WIN_H

#pragma once

#include <windows.h>

namespace ui {

// A virtual-key code translated to the character the shared keymap wants:
// letters lowercased, punctuation from its OEM code, 0 when it is no panel
// key. Stays within ASCII, so button_for_char() takes it as-is.
inline char key_char_of_vk(int vk)
{
	if (vk >= 'A' && vk <= 'Z')
		return char(vk - 'A' + 'a');
	switch (vk) {
	case VK_OEM_6: return ']';
	case VK_OEM_4: return '[';
	case VK_OEM_PLUS: return '=';
	case VK_OEM_MINUS: return '-';
	case VK_OEM_PERIOD: return '.';
	case VK_OEM_COMMA: return ',';
	case VK_BACK: return '\b';
	case VK_RETURN: return '\r';
	default: break;
	}
	return 0;
}

} // namespace ui

#endif // S_MU2000_UI_KEYMAP_WIN_H
