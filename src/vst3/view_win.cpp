// license:BSD-3-Clause
//
// The VST3 view's window on Windows: a child HWND inside the host's parent,
// which is what gives the panel real mouse, wheel and key messages. The VST3
// interface, the panel and the input semantics are all in view.cpp; this is
// only the window.
//
// Extracted from view.cpp when the macOS port arrived, so that view.cpp could
// stop including windows.h -- view_mac.mm has to include view.h next to Cocoa,
// and the two cannot see the same BOOL.

#include "plug_window.h"
#include "view.h"

#include "ui/fx_editor.h"
#include "ui/master_editor.h"
#include "ui/keymap.h"
#include "ui/keymap_win.h"
#include "ui/menu.h"
#include "ui/menu_win.h"
#include "ui/part_shapes.h"
#include "ui/pc_editor.h"
#include "ui/overview.h"
#include "ui/pc_host.h"
#include "ui/pc_window.h"
#include "ui/text.h"

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <cwchar>
#include <string>

namespace smu2000 {
namespace vst3 {

const char *plug_window_type() { return Steinberg::kPlatformTypeHWND; }

namespace {

const char *kClassName = "SMU2000PlugView";

// 窓のクラスはこの DLL で 1 度だけ登録する
HINSTANCE this_module()
{
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   reinterpret_cast<LPCSTR>(&this_module), &self);
	return HINSTANCE(self);
}

void register_class(WNDPROC proc)
{
	static bool done = false;
	if (done)
		return;
	WNDCLASSA wc{};
	wc.lpfnWndProc   = proc;
	wc.hInstance     = this_module();
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = kClassName;
	wc.hbrBackground = nullptr;
	RegisterClassA(&wc);
	done = true;
}

// Hosts sometimes pass keys through. The table is shared (ui/keymap.h,
// same as gui.exe); only VK codes become characters here.
// Returns a mu2000::button value, or -1 for anything else.
int button_key_of(WPARAM vk)
{
	mu2000::button b = mu2000::button::count;
	if (!ui::button_for_char(ui::key_char_of_vk(int(vk)), b))
		return -1;
	return int(b);
}

} // namespace


class win_window : public plug_window
{
public:
	explicit win_window(plug_view &owner) : m_owner(owner) {}
	~win_window() override { detach(); }

	bool attach(void *parent, int w, int h) override;
	void detach() override;
	void set_size(int w, int h) override;
	void card_menu(int x, int y) override;
	void panel_menu(int x, int y) override;
	void alert(const std::string &text) override;
	void pc_frame(::xg::model &m, const ::ui::xg_snapshot &ram, ::ui::bridge &br) override;

private:
	static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	void card_command(UINT id);
	void open_pc(ui::pc_window &w);

	plug_view &m_owner;
	HWND m_hwnd = nullptr;

	// PC で触る窓。gui.exe と同じ中身（ui::overview など）を、同じ ui::pc_window に
	// 載せる。**プラグインなので自分の窓を持つ**: ホストがくれた親の中には
	// パネルしか入らない。閉じても消さずに隠すだけなので、開き直すと同じ姿で出る
	ui::pc_window m_list{ std::make_unique<ui::overview>() };
	ui::pc_window m_editor{ std::make_unique<ui::pc_editor>() };
	ui::pc_window m_fx{ std::make_unique<ui::fx_editor>() };
	ui::pc_window m_shapes{ std::make_unique<ui::part_shapes>() };
	ui::pc_window m_master{ std::make_unique<ui::master_editor>() };
};

bool win_window::attach(void *parent, int w, int h)
{
	if (m_hwnd || !parent)
		return false;

	register_class(&win_window::wnd_proc);
	m_hwnd = CreateWindowExA(0, kClassName, "", WS_CHILD | WS_VISIBLE,
	                         0, 0, w, h, reinterpret_cast<HWND>(parent), nullptr,
	                         this_module(), nullptr);
	if (!m_hwnd)
		return false;

	// The window procedure has to find its way back to this object
	SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, LONG_PTR(this));
	SetTimer(m_hwnd, 1, 33, nullptr);        // 30 コマ／秒
	return true;
}

void win_window::detach()
{
	if (!m_hwnd)
		return;
	KillTimer(m_hwnd, 1);
	SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, 0);
	DestroyWindow(m_hwnd);
	m_hwnd = nullptr;
}

void win_window::set_size(int w, int h)
{
	if (m_hwnd)
		MoveWindow(m_hwnd, 0, 0, w, h, TRUE);
}

LRESULT CALLBACK win_window::wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	auto *self = reinterpret_cast<win_window *>(GetWindowLongPtrA(h, GWLP_USERDATA));
	if (!self)
		return DefWindowProcA(h, msg, wp, lp);
	return self->handle(h, msg, wp, lp);
}

// ---- SmartMedia (the card slot). Content is shared with the macOS plug-in
// in ui/menu.h; the numbers below match it, so both sides choose the same way

namespace {

// Card image sizes, state, and file dialogs stay here: they are Win32's business
std::string ask_card_path(HWND h, bool create)
{
	wchar_t file[MAX_PATH] = {};
	if (create)
		wcscpy(file, L"smartmedia.img");
	OPENFILENAMEW o{};
	o.lStructSize = sizeof(o);
	o.hwndOwner = h;
	o.lpstrFilter = L"SmartMedia の中身 (*.img)\0*.img\0すべて (*.*)\0*.*\0";
	o.lpstrFile = file;
	o.nMaxFile = MAX_PATH;
	o.lpstrDefExt = L"img";
	if (create) {
		o.lpstrTitle = L"新しい SmartMedia の保存先";
		o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetSaveFileNameW(&o))
			return {};
	} else {
		o.lpstrTitle = L"差す SmartMedia";
		o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetOpenFileNameW(&o))
			return {};
	}
	return ui::to_utf8(file);
}

} // namespace

void win_window::alert(const std::string &text)
{
	const std::wstring w = ui::to_wide(text);
	MessageBoxW(m_hwnd, w.c_str(), L"S-MU2000", MB_OK | MB_ICONWARNING);
}

void win_window::card_menu(int x, int y)
{
	// A card menu needs somewhere to send the choice, and this window is it:
	// WM_COMMAND comes back to handle() below with the same ids
	ui::plug_menu_state s;
	s.card_path = m_owner.card_path();
	s.card_ready = m_owner.card_ready();
	HMENU m = ui::render_menu(ui::menu_plug_card(s));
	POINT pt{ x, y };
	ClientToScreen(m_hwnd, &pt);
	ui::track_menu(m_hwnd, pt, m);
}

void win_window::panel_menu(int x, int y)
{
	// A right click that missed the card slot: the PC windows, as on macOS.
	// The choice comes back through WM_COMMAND, so card_command() handles it
	HMENU m = ui::render_menu(ui::menu_plug_panel());
	POINT pt{ x, y };
	ClientToScreen(m_hwnd, &pt);
	ui::track_menu(m_hwnd, pt, m);
}

void win_window::card_command(UINT id)
{
	if (id >= ui::ID_PLUG_CARD_NEW16 && id <= ui::ID_PLUG_CARD_NEW128) {
		const std::string path = ask_card_path(m_hwnd, true);
		if (!path.empty())
			m_owner.card_make(path, 16 << (id - ui::ID_PLUG_CARD_NEW16));
	} else if (id == ui::ID_PLUG_CARD_OPEN) {
		const std::string path = ask_card_path(m_hwnd, false);
		if (!path.empty())
			m_owner.card_insert_path(path);
	} else if (id == ui::ID_PLUG_CARD_EJECT) {
		m_owner.card_eject();
	} else if (id == ui::ID_PLUG_LIST) {
		open_pc(m_list);
	} else if (id == ui::ID_PLUG_EDITOR) {
		open_pc(m_editor);
	}
}

void win_window::open_pc(ui::pc_window &w)
{
	std::string err;
	if (!w.show(this_module(), err))
		alert(err.empty() ? std::string("窓を出せない") : err);
}

// パネルを描き直すのと同じ周期で呼ばれる。見えていない窓は何もしない
void win_window::pc_frame(::xg::model &m, const ::ui::xg_snapshot &ram, ::ui::bridge &br)
{
	ui::pc_frame_all(m_list, m_editor, m_fx, m_shapes, m_master, m, ram, br,
	                 [this](ui::pc_window &w) { open_pc(w); });
}

LRESULT win_window::handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_TIMER:
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_COMMAND:
		card_command(LOWORD(wp));
		return 0;

	case WM_ERASEBKGND:
		return 1;                        // 全部自分で描く

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(h, &ps);
		RECT cr;
		GetClientRect(h, &cr);
		m_owner.repaint(dc, cr.right, cr.bottom);
		EndPaint(h, &ps);
		return 0;
	}

	case WM_SIZE:
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_LBUTTONDOWN:
		SetCapture(h);
		m_owner.mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_RBUTTONUP:
		m_owner.mouse_right(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		return 0;

	case WM_MOUSEMOVE:
		m_owner.mouse_drag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		return 0;

	case WM_LBUTTONUP:
		m_owner.mouse_up();
		ReleaseCapture();
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_MOUSEWHEEL: {
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(h, &pt);
		const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		if (delta)
			m_owner.wheel(pt.x, pt.y, delta);
		return 0;
	}

	case WM_KEYDOWN: {
		if (lp & (1 << 30))              // 押しっぱなしの繰り返しは無視
			return 0;
		const int k = button_key_of(wp);
		if (k >= 0)
			m_owner.key(k, true);
		return 0;
	}

	case WM_KEYUP: {
		const int k = button_key_of(wp);
		if (k >= 0)
			m_owner.key(k, false);
		return 0;
	}

	case WM_KILLFOCUS:
		m_owner.focus_lost();
		return 0;
	}
	return DefWindowProcA(h, msg, wp, lp);
}


plug_window *plug_window_create(plug_view &owner)
{
	return new win_window(owner);
}

} // namespace vst3
} // namespace smu2000
