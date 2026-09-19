// license:BSD-3-Clause
//
// Windows HMENU rendering for the shared ui/menu.h content, used by gui.exe
// and the plug-ins (src/vst3/view_win.cpp).
//
// Windows-only: includes <windows.h>. A submenu whose items are all disabled
// is grayed as a whole (the plug-in card menu wants that while no card can
// be swapped in); the standalone menus never hit it, so they render exactly
// as their hand-built predecessors did.

#ifndef S_MU2000_UI_MENU_WIN_H
#define S_MU2000_UI_MENU_WIN_H

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "menu.h"
#include "text.h"

namespace ui {

// The sources are UTF-8, so menus go through the W calls (the A calls would
// read them as CP932)
inline void menu_add_item(HMENU m, UINT flags, UINT_PTR id, const char *utf8)
{
	const std::wstring w = to_wide(utf8);
	AppendMenuW(m, flags, id, w.c_str());
}

inline void append_menu_items(HMENU m, const std::vector<menu_item> &items)
{
	for (const menu_item &it : items) {
		if (it.separator) {
			AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
			continue;
		}
		std::string label = it.label;
		if (!it.shortcut.empty())
			label += "\t" + it.shortcut;
		menu_add_item(m, MF_STRING | (it.checked ? MF_CHECKED : 0) | (it.enabled ? 0 : MF_GRAYED),
		              UINT_PTR(it.id), label.c_str());
	}
}

inline HMENU render_menu(const std::vector<menu_group> &groups)
{
	HMENU top = CreatePopupMenu();
	for (const menu_group &g : groups) {
		if (g.title.empty()) {
			append_menu_items(top, g.items);
			continue;
		}
		HMENU sub = CreatePopupMenu();
		append_menu_items(sub, g.items);
		bool any_enabled = false;
		for (const menu_item &it : g.items)
			any_enabled |= !it.separator && it.enabled;
		menu_add_item(top, MF_POPUP | (any_enabled ? 0 : MF_GRAYED),
		              UINT_PTR(sub), g.title.c_str());
	}
	return top;
}

inline void track_menu(HWND hwnd, POINT screen, HMENU top)
{
	TrackPopupMenu(top, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
	               screen.x, screen.y, 0, hwnd, nullptr);
	DestroyMenu(top);
}

} // namespace ui

#endif // S_MU2000_UI_MENU_WIN_H
