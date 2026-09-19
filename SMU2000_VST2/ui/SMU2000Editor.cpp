// license:BSD-3-Clause
//
// src/vst3/view.cpp の iPlug2 移植。描画・入力の流れは VST3 版とまったく同じ。
// 違うのは土台だけ —— Steinberg IPlugView の代わりに iPlug2 の
// IEditorDelegate::OpenWindow から呼ばれる（VST2 effEditOpen / CLAP guiSetParent）。

#include "SMU2000Editor.h"

#include "../config.h"               // PLUG_WIDTH / PLUG_HEIGHT (editor default size)
#include "../../src/vst3/engine.h"
#include "../../src/ui/bridge.h"
#include "../../src/ui/layout.h"
#include "../../src/ui/text.h"
#include "../../src/smartmedia.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <windowsx.h>
#include <commdlg.h>

namespace smu2000 {

namespace {

const char *kClassName = "SMU2000IPlugView";

// この DLL 自身（静的 CRT でも効くようアドレスから取る）。
// gHINSTANCE は IPlug_include_in_plug_src.h の DllMain が Setting するが、
// TU 間の結合を減らすためここでは使わず自前で解決する。
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

// ホストによってはキーがこちらに回ってくる。gui.exe / VST3 と同じ割り当て
mu2000::button key_to_button(WPARAM vk, bool &ok)
{
	ok = true;
	switch (vk) {
	case 'A': return mu2000::button::play;
	case 'E': return mu2000::button::edit;
	case 'U': return mu2000::button::util;
	case 'F': return mu2000::button::effect;
	case 'S': return mu2000::button::mute_solo;
	case VK_OEM_6: return mu2000::button::part_plus;
	case VK_OEM_4: return mu2000::button::part_minus;
	case VK_OEM_PLUS:  return mu2000::button::value_plus;
	case VK_OEM_MINUS: return mu2000::button::value_minus;
	case VK_BACK:   return mu2000::button::exit;
	case VK_RETURN: return mu2000::button::enter;
	case VK_OEM_PERIOD: return mu2000::button::select_right;
	case VK_OEM_COMMA:  return mu2000::button::select_left;
	case 'Q': return mu2000::button::seq;
	case 'Z': return mu2000::button::audition;
	case 'X': return mu2000::button::select;
	case 'M': return mu2000::button::sampling_mode;
	default: break;
	}
	ok = false;
	return mu2000::button::count;
}

// ---- SmartMedia（カードの差し込み口）。gui.exe / VST3 の品書きと同じ

enum : UINT { ID_CARD_NEW16 = 100, ID_CARD_NEW32, ID_CARD_NEW64, ID_CARD_NEW128, ID_CARD_OPEN = 110, ID_CARD_EJECT = 111 };

void add_item(HMENU m, UINT flags, UINT_PTR id, const char *utf8)
{
	const std::wstring w = ui::to_wide(utf8);
	AppendMenuW(m, flags, id, w.c_str());
}

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


editor::editor(vst3::engine &eng)
	: m_engine(eng)
	, m_w(PLUG_WIDTH)
	, m_h(PLUG_HEIGHT)
{
	// パネルの配置。%LOCALAPPDATA%\S-MU2000\panel.txt があれば読む
	// （doc/panel-editing.md）。無ければ組み込みの配置のまま
	const std::string lay = ui::layout::find_default();
	if (!lay.empty()) {
		std::string err;
		m_panel.lay().load(lay, err);
	}
	m_panel.resize(m_w, m_h);
}

editor::~editor()
{
	close();
}

void *editor::open(void *parent)
{
	if (!parent)
		return nullptr;
	if (m_hwnd)
		close();

	register_class(&editor::wnd_proc);
	m_hwnd = CreateWindowExA(0, kClassName, "", WS_CHILD | WS_VISIBLE,
	                         0, 0, m_w, m_h, HWND(parent), nullptr,
	                         this_module(), nullptr);
	if (!m_hwnd)
		return nullptr;

	SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, LONG_PTR(this));
	SetTimer(m_hwnd, 1, 33, nullptr);        // 30 コマ／秒
	m_panel.resize(m_w, m_h);
	return m_hwnd;
}

void editor::close()
{
	if (m_hwnd) {
		KillTimer(m_hwnd, 1);
		SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, 0);
		DestroyWindow(m_hwnd);
		m_hwnd = nullptr;
	}
	if (m_mem_bmp) { DeleteObject(m_mem_bmp); m_mem_bmp = nullptr; }
	if (m_mem_dc)  { DeleteDC(m_mem_dc); m_mem_dc = nullptr; }
	m_mem_w = m_mem_h = 0;
}

void editor::set_size(int w, int h)
{
	m_w = std::max(w, 640);
	m_h = std::max(h, 180);
	if (m_hwnd)
		MoveWindow(m_hwnd, 0, 0, m_w, m_h, TRUE);
	m_panel.resize(m_w, m_h);
}


LRESULT CALLBACK editor::wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	editor *self = reinterpret_cast<editor *>(GetWindowLongPtrA(h, GWLP_USERDATA));
	if (!self)
		return DefWindowProcA(h, msg, wp, lp);
	return self->handle(h, msg, wp, lp);
}

void editor::paint(HWND h)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(h, &ps);
	RECT cr;
	GetClientRect(h, &cr);
	const int w = cr.right, hh = cr.bottom;

	if (!m_mem_dc || m_mem_w != w || m_mem_h != hh) {
		if (m_mem_bmp) DeleteObject(m_mem_bmp);
		if (m_mem_dc)  DeleteDC(m_mem_dc);
		m_mem_dc = CreateCompatibleDC(dc);
		m_mem_bmp = CreateCompatibleBitmap(dc, w, hh);
		SelectObject(m_mem_dc, m_mem_bmp);
		m_mem_w = w;
		m_mem_h = hh;
	}

	ui::snapshot s;
	m_engine.panel().read(s);

	char status[160];
	std::snprintf(status, sizeof(status), "%s", m_engine.message().c_str());

	m_panel.set_volume(m_engine.panel().gain());
	m_panel.paint(m_mem_dc, s, m_engine.panel().buttons(), status);
	BitBlt(dc, 0, 0, w, hh, m_mem_dc, 0, 0, SRCCOPY);
	EndPaint(h, &ps);
}

LRESULT editor::handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	ui::bridge &br = m_engine.panel();

	switch (msg) {
	case WM_TIMER:
		// パラメータの層: 音源の返事を読み、見えている面の読み返しを頼む
		m_panel.tick(br);
		InvalidateRect(h, nullptr, FALSE);
		// SmartMedia に書いたものを 2 秒ごとにファイルへ書き戻す
		if (GetTickCount() - m_last_flush > 2000) {
			m_last_flush = GetTickCount();
			m_engine.card_flush();
		}
		return 0;

	case WM_RBUTTONUP:
		if (m_panel.on_card_slot(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
			card_menu(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		return 0;

	case WM_COMMAND:
		card_command(h, LOWORD(wp));
		return 0;

	case WM_ERASEBKGND:
		return 1;

	case WM_PAINT:
		paint(h);
		return 0;

	case WM_SIZE:
		m_panel.resize(LOWORD(lp), HIWORD(lp));
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_GETMINMAXINFO: {
		// 横に長い機械なので、縦横比はこちらで決める（論理 1000 × 400）
		MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO *>(lp);
		const int min_w = 640;
		const int min_h = min_w * ui::LOGICAL_H / ui::LOGICAL_W;
		mmi->ptMinTrackSize.x = min_w;
		mmi->ptMinTrackSize.y = min_h;
		return 0;
	}

	case WM_LBUTTONDOWN:
		// カードの差し込み口は SmartMedia の品書き
		if (m_panel.on_card_slot(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {
			card_menu(h, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
			return 0;
		}
		SetCapture(h);
		if (m_panel.press(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), br))
			InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_MOUSEMOVE:
		if (m_panel.drag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), br))
			InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_LBUTTONUP:
		m_panel.release(br);
		ReleaseCapture();
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_MOUSEWHEEL: {
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(h, &pt);
		const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		if (delta && m_panel.wheel_at(pt.x, pt.y, delta, br))
			InvalidateRect(h, nullptr, FALSE);
		return 0;
	}

	case WM_KEYDOWN: {
		if (lp & (1 << 30))
			return 0;
		bool ok = false;
		const mu2000::button b = key_to_button(wp, ok);
		if (ok) br.press(b, true);
		return 0;
	}

	case WM_KEYUP: {
		bool ok = false;
		const mu2000::button b = key_to_button(wp, ok);
		if (ok) br.press(b, false);
		return 0;
	}

	case WM_KILLFOCUS:
		br.release_all();
		return 0;
	}
	return DefWindowProcA(h, msg, wp, lp);
}


void editor::card_menu(HWND h, int x, int y)
{
	const std::string path = m_engine.card_path();
	HMENU m = CreatePopupMenu();
	HMENU mnew = CreatePopupMenu();
	add_item(mnew, MF_STRING, ID_CARD_NEW16, "16MB");
	add_item(mnew, MF_STRING, ID_CARD_NEW32, "32MB");
	add_item(mnew, MF_STRING, ID_CARD_NEW64, "64MB");
	add_item(mnew, MF_STRING, ID_CARD_NEW128, "128MB");
	const UINT ready = m_engine.state() == vst3::status::ready ? 0 : MF_GRAYED;
	add_item(m, MF_POPUP | ready, UINT_PTR(mnew), "新しい SmartMedia を作って差す");
	add_item(m, MF_STRING | ready, ID_CARD_OPEN, "SmartMedia を差す...");
	std::string eject = "SmartMedia を抜く";
	if (!path.empty())
		eject += "（" + path.substr(path.find_last_of("\\/") + 1) + "）";
	add_item(m, MF_STRING | (path.empty() ? MF_GRAYED : 0), ID_CARD_EJECT, eject.c_str());
	POINT pt{ x, y };
	ClientToScreen(h, &pt);
	TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
	DestroyMenu(m);
}

void editor::card_command(HWND h, UINT id)
{
	std::string err;
	if (id >= ID_CARD_NEW16 && id <= ID_CARD_NEW128) {
		const std::string path = ask_card_path(h, true);
		if (path.empty())
			return;
		smartmedia card;
		card.create(16u << (id - ID_CARD_NEW16));
		if (card.save(path, err) && m_engine.card_insert(path, err)) {
			MessageBoxW(h, L"空の SmartMedia を差しました。\n"
			               L"使う前に、本体の UTIL → CARD → Format で書式化してください。",
			            L"S-MU2000", MB_OK | MB_ICONINFORMATION);
			return;
		}
	} else if (id == ID_CARD_OPEN) {
		const std::string path = ask_card_path(h, false);
		if (path.empty() || m_engine.card_insert(path, err))
			return;
	} else if (id == ID_CARD_EJECT) {
		m_engine.card_eject();
		return;
	} else {
		return;
	}
	MessageBoxW(h, ui::to_wide(err).c_str(), L"S-MU2000", MB_OK | MB_ICONWARNING);
}

} // namespace smu2000
