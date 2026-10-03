// license:BSD-3-Clause
//
// The Windows event translation for gui: the message switch, painting,
// popups, drops and buffering. gui.cpp keeps the app class instance and
// main(); everything Win32 here mirrors ui/window_mac.mm on the Mac side.

#include "window_win.h"
#include "app_win.h"

#include "ui/keymap_win.h"
#include "ui/pc_host.h"

#include "ui/imgui_shell.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"

#include "ui/rom_locate.h"

#include <cstdio>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ui {

namespace {

double lcd_aspect()
{
	const double h = g_win->panel.lay().lcd[3];
	return h > 0.0 ? g_win->panel.lay().lcd[2] / h : 1.0;
}

// What the window keeps at a fixed ratio: the LCD alone, or the whole panel
// picture (1000:400, the same ratio the VST3 view keeps). The toolbar strip
// on top is added, not scaled, so it stays out of the ratio
double body_aspect()
{
	return g_win->lcd_only ? lcd_aspect() : double(LOGICAL_W) / LOGICAL_H;
}

// Client height for a client width, strip included
int client_h_for(int client_w)
{
	return int(std::lround(client_w / body_aspect())) + g_win->panel.top_inset();
}

// Keep the client area, excluding the title bar and resize frame, at the
// body ratio. Without it the panel letterboxes, and a short, wide window
// shrinks the LCD dots to fit the height, leaving gaps at the LCD's sides
void constrain_sizing(HWND hwnd, WPARAM edge, RECT &r)
{
	RECT wr{}, cr{};
	GetWindowRect(hwnd, &wr);
	GetClientRect(hwnd, &cr);
	const int frame_w = (wr.right - wr.left) - (cr.right - cr.left);
	const int frame_h = (wr.bottom - wr.top) - (cr.bottom - cr.top);
	const int inset = g_win->panel.top_inset();
	const int min_w = g_win->lcd_only ? 200 : 500;
	int outer_w = r.right - r.left;
	int outer_h = r.bottom - r.top;

	if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
		const int body_h = std::max(int(std::lround(min_w / body_aspect())),
		                            outer_h - frame_h - inset);
		outer_h = body_h + inset + frame_h;
		outer_w = int(std::lround(body_h * body_aspect())) + frame_w;
		if (edge == WMSZ_TOP)
			r.top = r.bottom - outer_h;
		else
			r.bottom = r.top + outer_h;
		r.right = r.left + outer_w;
		return;
	}

	const int client_w = std::max(min_w, outer_w - frame_w);
	outer_w = client_w + frame_w;
	outer_h = client_h_for(client_w) + frame_h;
	if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
		r.left = r.right - outer_w;
	else
		r.right = r.left + outer_w;
	if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
		r.top = r.bottom - outer_h;
	else
		r.bottom = r.top + outer_h;
}

imshell::dx11_state g_im{};

void win_imgui_frame(HWND hwnd)
{
	RECT cr;
	GetClientRect(hwnd, &cr);
	imshell::dx11_paint(g_im, cr.right, cr.bottom, [&](ImDrawList *dl) {
		g_win->paint_main(dl, g_im.fonts, cr.right);
	});
}

} // namespace

// Menu command numbers, labels and builders are shared with gui_mac.cpp
// in ui/menu.h (Windows is the reference), so a menu added on one side
// cannot be missed on the other. Which popup a point asks for is shared
// too (ui::app::context_menu); only rendering it through ui/menu_win.h
// stays here.

// Creates and shows the main window. The window class and the drag target
// belong to the message pump's file; ui::app::run asks for them through
// open_main_window
bool make_window(const char *title, int w, int h)
{
	const HINSTANCE inst = GetModuleHandleA(nullptr);
	WNDCLASSA wc{};
	wc.lpfnWndProc   = wnd_proc;
	wc.hInstance     = inst;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = "SMU2000Panel";
	wc.hbrBackground = nullptr;
	RegisterClassA(&wc);

	const DWORD style = g_win->lcd_only ? (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX)
	                                      : WS_OVERLAPPEDWINDOW;
	// The first size follows the same ratio as dragging does; --size's
	// width wins, the height is worked out from it
	h = std::max(60, client_h_for(w));
	RECT want{ 0, 0, w, h };
	AdjustWindowRect(&want, style, FALSE);
	HWND hwnd = CreateWindowA("SMU2000Panel", title, style,
	                          CW_USEDEFAULT, CW_USEDEFAULT,
	                          want.right - want.left, want.bottom - want.top,
	                          nullptr, nullptr, inst, nullptr);
	if (!hwnd)
		return false;

	// MIDI ファイルを窓に落とせば流す（本体の窓も、エディタや一覧の窓も）
	DragAcceptFiles(hwnd, TRUE);
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);
	return true;
}

void track_menu_at(HWND hwnd, int mx, int my)
{
	POINT pt{ mx, my };
	ClientToScreen(hwnd, &pt);
	win_track_menu(hwnd, pt, g_win->context_menu(mx, my));
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (g_im.imgui) {
		ImGui::SetCurrentContext(g_im.imgui);
		// Editor windows skip WM_CHAR outside text boxes (pc_window.cpp);
		// the panel takes keys directly, so every key stays shared.
		ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
	}
	switch (msg) {
	case WM_CREATE:
		g_win->hwnd = hwnd;
		SetTimer(hwnd, 1, 33, nullptr);        // 30 コマ／秒で描き直す
		if (!imshell::dx11_start(g_im, hwnd)) {
			MessageBoxA(hwnd, "Cannot use Direct3D 11", "S-MU2000",
			            MB_OK | MB_ICONERROR);
			return -1;
		}
		// The context is up and no frame is open, so the panel can build its
		// fonts now (see panel::fonts_ready)
		g_win->panel.fonts_ready();
		return 0;

	case WM_TIMER: {
		win_imgui_frame(hwnd);   // timer work runs inside paint_main
		return 0;
	}

	case WM_DROPFILES: {
		// 窓に落とされたファイルの 1 つ目を流す。何を意味するかは app の仕事
		// (ui::app::file_dropped)、失敗の見せ方はコマンドと同じ
		const HDROP drop = HDROP(wp);
		wchar_t path[MAX_PATH * 4] = {};
		const bool got = DragQueryFileW(drop, 0, path, UINT(sizeof(path) / sizeof(path[0]))) > 0;
		DragFinish(drop);
		if (got) {
			g_win->file_dropped(ui::to_utf8(path));
			if (!g_win->last_error.empty()) {
				ui::win_error(hwnd, g_win->last_error);
				g_win->last_error.clear();
			}
		}
		return 0;
	}


	case WM_SIZE:
		g_im.resize_w = LOWORD(lp);
		g_im.resize_h = HIWORD(lp);
		g_win->resized(LOWORD(lp), HIWORD(lp));
		return 0;

	case WM_SIZING:
		constrain_sizing(hwnd, wp, *reinterpret_cast<RECT *>(lp));
		return TRUE;

	case WM_ERASEBKGND:
		return 1;                               // 全部自分で描く

	case WM_PAINT: {
		PAINTSTRUCT ps;   // Direct3D が出す。validate のためだけに閉じる
		BeginPaint(hwnd, &ps);
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_LBUTTONDOWN: {
		const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
		// Decided and mostly acted in the base; the window only shows the
		// popup and repaints
		const ui::mouse_out o = g_win->mouse_down(mx, my, false);
		if (o.panel_pressed)
			SetCapture(hwnd);
		if (o.opened_window || o.panel_pressed)
			InvalidateRect(hwnd, nullptr, FALSE);
		if (o.opened_window || !o.show_menu)
			return 0;
		track_menu_at(hwnd, mx, my);
		return 0;
	}

	case WM_RBUTTONUP: {
		const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
		track_menu_at(hwnd, mx, my);
		return 0;
	}

	case win_app::WM_APP_DEFERRED:
		// 描画の外で行う仕事（app::defer_outside_paint。録音デバイスの選び直しなど）
		g_win->run_deferred_win();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_COMMAND: {
		const UINT id = LOWORD(wp);
		g_win->last_error.clear();
		// The dispatch is shared (ui::app::menu_chosen); failures land in
		// last_error through menu_error and show below
		g_win->menu_chosen(int(id));
		if (!g_win->last_error.empty()) {
			const std::wstring w = ui::to_wide(g_win->last_error);
			MessageBoxW(hwnd, w.c_str(), L"S-MU2000", MB_OK | MB_ICONWARNING);
			g_win->last_error.clear();
		}
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_SETCURSOR: {
		// Show a hand where something opens (same spots as the Mac side)
		POINT pt;
		GetCursorPos(&pt);
		ScreenToClient(hwnd, &pt);
		if (LOWORD(lp) == HTCLIENT && g_win->hand_cursor(pt.x, pt.y)) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}

	case WM_MOUSEMOVE:
		if (g_win->mouse_drag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_LBUTTONUP:
		g_win->mouse_up();
		ReleaseCapture();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_MOUSEWHEEL: {
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(hwnd, &pt);
		const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		if (g_win->wheel(pt.x, pt.y, delta))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_KEYDOWN: {
		if (lp & (1 << 30))                     // 押しっぱなしの繰り返しは無視
			return 0;
		g_win->key(ui::key_char_of_vk(int(wp)), true);
		return 0;
	}

	case WM_KEYUP:
		g_win->key(ui::key_char_of_vk(int(wp)), false);
		return 0;

	case WM_KILLFOCUS:
		g_win->focus_lost();                    // 窓から離れたら全部離す
		return 0;

	case WM_DESTROY:
		imshell::dx11_stop(g_im);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}

// ROM の場所なしで起動したとき（ui/rom_locate.h）。窓を作る前なので、案内（MessageBox）と
// 「フォルダーの参照」の窓（SHBrowseForFolder）を出す。文字は UTF-8 から wide に
bool ask_roms_folder(const std::string &message, std::string &picked)
{
	auto wide = [](const std::string &s) {
		std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0)), L'\0');
		if (!w.empty()) {
			MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
			w.pop_back();   // 終わりの 0
		}
		return w;
	};
	std::string text = message;
	text += "\n\n";
	text += UI_TEXT(dlg_roms_ok_cancel, "OK: choose the folder.  Cancel: quit.");
	if (MessageBoxW(nullptr, wide(text).c_str(), L"S-MU2000", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
		return false;
	const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
	const std::wstring title = wide(UI_TEXT(dlg_roms_pick, "Select ROM folder..."));
	BROWSEINFOW bi{};
	bi.lpszTitle = title.c_str();
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
	wchar_t path[MAX_PATH * 4] = {};
	const bool got = id && SHGetPathFromIDListW(id, path);
	if (id)
		CoTaskMemFree(id);
	if (com)
		CoUninitialize();
	if (!got)
		return false;
	picked = to_utf8(path);
	return !picked.empty();
}

} // namespace ui
