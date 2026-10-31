// license:BSD-3-Clause

#include "pc_window.h"
#include "text.h"
#include "ui/texts.h"
#include "ui/lang.h"
#include "ui/font_file.h"
#include "ui/dxgi_stay.h"
#include "ui/tex.h"

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#include <commdlg.h>
#include <cstdio>
#include <d3d11.h>
#include <iterator>
#include <shellapi.h>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ui {

namespace {

const wchar_t CLASS_NAME[] = L"SMU2000PcEditor";
// ファイルの窓を描画の外で開く（pc_window::frame が頼む）
constexpr UINT WM_APP_FILE_DIALOG = WM_APP + 0x41;

// .syx の書き出し・読み込みの窓（xgui::ask_save_file・ask_open_file の頼み）。
// 描き終えたあとに開く。窓が回っている間にタイマーが別のコマを描いても、前のコマは終わっている
void file_dialog(HWND owner, xgui::file_ask ask, const std::vector<u8> &bytes)
{
	wchar_t path[MAX_PATH * 4] = {};
	if (ask == xgui::file_ask::save)
		wcscpy_s(path, L"S-MU2000.syx");
	const bool wav = ask == xgui::file_ask::open && xgui::file_ask_is_wav();
	const bool dls = ask == xgui::file_ask::open && xgui::file_ask_is_dls();
	const bool card = !dls && ask == xgui::file_ask::open && xgui::file_ask_is_card();
	// MIDI ファイル（プレイヤーの窓）。何個でも選べるので、道の入れ物を大きく取って別に開く
	if (ask == xgui::file_ask::open && xgui::file_ask_is_midi()) {
		std::vector<wchar_t> many(32768, 0);
		const std::wstring mf = dlg_filter(UI_TEXT(dlg_midi_desc, "MIDI files"), "*.mid;*.midi;*.smf;*.m3u;*.m3u8;*.rcp;*.r36",
		                                   UI_TEXT(dlg_all_files, "All files"), "*.*");
		OPENFILENAMEW m{};
		m.lStructSize = sizeof(m);
		m.hwndOwner   = owner;
		m.lpstrFilter = mf.c_str();
		m.lpstrFile   = many.data();
		m.nMaxFile    = DWORD(many.size());
		m.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
		if (!GetOpenFileNameW(&m))
			return;
		// 1 個なら道そのもの。何個かなら「フォルダー\0 名前\0 名前\0\0」
		std::vector<std::string> paths;
		const std::wstring first = many.data();
		const wchar_t *p = many.data() + first.size() + 1;
		if (!*p) {
			paths.push_back(to_utf8(first.c_str()));
		} else {
			for (; *p; p += wcslen(p) + 1)
				paths.push_back(to_utf8((first + L"\\" + p).c_str()));
		}
		xgui::give_opened_midi(std::move(paths));
		return;
	}
	// Bound here: the dialog reads the filter while it runs.
	const std::wstring filter = dls ? dlg_filter(UI_TEXT(dlg_dls_desc, "DLS sound bank"), "*.dls",
	                                             UI_TEXT(dlg_all_files, "All files"), "*.*")
	                          : card ? dlg_filter(UI_TEXT(dlg_card_or_m2a_desc, "SmartMedia image or M2A file"), "*.img;*.sm;*.m2a",
	                                              UI_TEXT(dlg_all_files, "All files"), "*.*")
	                          : wav ? dlg_filter(UI_TEXT(dlg_wav_desc, "WAV audio or SysEx"), "*.wav;*.syx",
	                                             UI_TEXT(dlg_all_files, "All files"), "*.*")
	                                 : dlg_filter(UI_TEXT(dlg_sysex_desc, "SysEx"), "*.syx",
	                                              UI_TEXT(dlg_all_files, "All files"), "*.*");
	OPENFILENAMEW o{};
	o.lStructSize = sizeof(o);
	o.hwndOwner   = owner;
	o.lpstrFilter = filter.c_str();
	o.lpstrFile   = path;
	o.nMaxFile    = DWORD(std::size(path));
	o.lpstrDefExt = wav ? L"wav" : L"syx";
	if (ask == xgui::file_ask::save) {
		o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
		if (!GetSaveFileNameW(&o))
			return;
		std::FILE *f = _wfopen(path, L"wb");
		const bool ok = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
		if (f)
			std::fclose(f);
		char note[64];
		std::snprintf(note, sizeof(note), ok ? UI_TEXT(note_exported_fmt, "Exported (%zu bytes)") : UI_TEXT(note_export_fail, "Cannot export"), bytes.size());
		xgui::set_file_note(note);
		return;
	}
	o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if (!GetOpenFileNameW(&o))
		return;
	if (dls) {
		xgui::give_opened_dls(to_utf8(path));
		return;
	}
	// カードの画像は大きい（最大 132MB）ので読まずに、場所だけを返す（読むのはサンプリングの窓）
	if (card) {
		xgui::give_opened_card(to_utf8(path));
		return;
	}
	std::vector<u8> in;
	if (std::FILE *f = _wfopen(path, L"rb")) {
		u8 buf[65536];
		size_t n;
		// WAV はサンプリング RAM（約 48 秒）の 2ch・浮動小数でも収まる大きさまで
		const size_t cap = wav ? (64u << 20) : (16u << 20);
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && in.size() < cap)
			in.insert(in.end(), buf, buf + n);
		std::fclose(f);
		xgui::give_opened_file(std::move(in));
	} else {
		xgui::set_file_note(UI_TEXT(note_import_fail, "Could not import"));
	}
}

} // namespace


pc_window::~pc_window()
{
	destroy();
}

bool pc_window::visible() const
{
	return m_hwnd && IsWindowVisible(m_hwnd);
}

void pc_window::shutdown(bridge &br)
{
	if (!m_imgui)
		return;
	ImGui::SetCurrentContext(m_imgui);
	m_view->hidden(br);
}

bool pc_window::show(HINSTANCE inst, std::string &err)
{
	if (!m_hwnd && !create(inst, err))
		return false;
	ShowWindow(m_hwnd, SW_SHOWNORMAL);
	SetForegroundWindow(m_hwnd);
	return true;
}

bool pc_window::create(HINSTANCE inst, std::string &err)
{
	xgui::set_file_dialogs(true);
	xgui::set_midi_dialog(true);
	WNDCLASSEXW wc{};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_CLASSDC;
	wc.lpfnWndProc   = proc;
	wc.hInstance     = inst;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = CLASS_NAME;
	RegisterClassExW(&wc);          // 2 回目は失敗するが、登録済みなので構わない

	// DPI 対応は宣言しない（プロセス全体に効いて、パネルの窓の見え方まで変わるため）。
	// 高 DPI の画面では Windows が窓ごと拡大する
	const float scale = 1.0f;

	m_hwnd = CreateWindowExW(0, CLASS_NAME, m_view->title(), WS_OVERLAPPEDWINDOW,
	                         CW_USEDEFAULT, CW_USEDEFAULT, int(m_view->default_width() * scale), int(m_view->default_height() * scale),
	                         nullptr, nullptr, inst, this);
	if (!m_hwnd) {
		err = UI_TEXT(dlg_editor_fail, "Cannot open the editor window");
		return false;
	}
	SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, LONG_PTR(this));
	if (s_drop)
		DragAcceptFiles(m_hwnd, TRUE);

	if (!create_device(err)) {
		destroy();
		return false;
	}

	IMGUI_CHECKVERSION();
	// 窓ごとに文脈を持つ（ImGui の口は今の文脈に付く）
	m_imgui = ImGui::CreateContext();
	ImGui::SetCurrentContext(m_imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;       // 作業フォルダに imgui.ini を撒かない

	ImGui::StyleColorsDark();
	ImGuiStyle &style = ImGui::GetStyle();
	style.ScaleAllSizes(scale);
	style.FontScaleDpi = scale;
	style.FrameRounding = 3;

	// The editor windows keep upstream's exact font: first existing file, face 0
	// (ui/font_file.h). The panel's unified walk must not serve here -- it
	// picks by weight and renders a lighter face than upstream's editors.
	add_cjk_editor_font(io.Fonts);

	ImGui_ImplWin32_Init(m_hwnd);
	ImGui_ImplDX11_Init(m_dev, m_ctx);
	return true;
}

bool pc_window::create_device(std::string &err)
{
	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount       = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow      = m_hwnd;
	sd.SampleDesc.Count  = 1;
	sd.Windowed          = TRUE;
	sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL got;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
	                                           D3D11_SDK_VERSION, &sd, &m_swap, &m_dev, &got, &m_ctx);
	if (hr == DXGI_ERROR_UNSUPPORTED)        // GPU が無い機械。ソフトウェアで描く
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
		                                   D3D11_SDK_VERSION, &sd, &m_swap, &m_dev, &got, &m_ctx);
	if (FAILED(hr)) {
		char buf[96];
		std::snprintf(buf, sizeof(buf), UI_TEXT(dlg_d3d_fail_fmt, "Cannot use Direct3D 11 (0x%08lx)"), (unsigned long)hr);
		err = buf;
		return false;
	}
	dxgi_stay(m_swap, m_hwnd);
	make_target();
	return true;
}

void pc_window::make_target()
{
	ID3D11Texture2D *back = nullptr;
	if (SUCCEEDED(m_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) {
		m_dev->CreateRenderTargetView(back, nullptr, &m_rtv);
		back->Release();
	}
}

void pc_window::drop_target()
{
	if (m_rtv) { m_rtv->Release(); m_rtv = nullptr; }
}

void pc_window::destroy()
{
	if (m_imgui) {
		ImGui::SetCurrentContext(m_imgui);
		im::drop_user_textures();      // this window's own pictures (ui/tex.h; issue #224)
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(m_imgui);
		m_imgui = nullptr;
	}
	drop_target();
	if (m_swap) { m_swap->Release(); m_swap = nullptr; }
	if (m_ctx)  { m_ctx->Release();  m_ctx = nullptr; }
	if (m_dev)  { m_dev->Release();  m_dev = nullptr; }
	if (m_hwnd) {
		SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
		DestroyWindow(m_hwnd);
		m_hwnd = nullptr;
	}
}

void pc_window::frame(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	const bool shown = m_imgui && visible() && !IsIconic(m_hwnd);
	if (m_was_visible && !shown && m_imgui) {
		ImGui::SetCurrentContext(m_imgui);
		m_view->hidden(br);                  // 閉じた・しまった。押しっぱなしを離す
	}
	m_was_visible = shown;
	if (!shown)
		return;
	ImGui::SetCurrentContext(m_imgui);
	if (m_resize_w) {
		drop_target();
		m_swap->ResizeBuffers(0, m_resize_w, m_resize_h, DXGI_FORMAT_UNKNOWN, 0);
		m_resize_w = m_resize_h = 0;
		make_target();
	}

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ensure_cjk_ui_fonts(ImGui::GetIO().Fonts);
	ImGui::NewFrame();
	m_view->draw(m, ram, br);
	xgui::drag_flush(br);          // マウスで動かしている値の、間引いた送信
	ImGui::Render();

	const float clear[4] = { 0.10f, 0.10f, 0.11f, 1.0f };
	m_ctx->OMSetRenderTargets(1, &m_rtv, nullptr);
	m_ctx->ClearRenderTargetView(m_rtv, clear);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	// 待たない。gui のタイマー（30 コマ／秒）が間隔を決める
	m_swap->Present(0, 0);

	// ファイルの窓は、ここでは開かずに自分の窓へ頼む（描画の外で開く。pc_window.h の m_file_ask）
	std::vector<u8> bytes;
	const xgui::file_ask ask = xgui::take_file_ask(bytes);
	if (ask != xgui::file_ask::none && !m_file_ask) {
		m_file_ask = int(ask);
		m_file_bytes = std::move(bytes);
		PostMessageW(m_hwnd, WM_APP_FILE_DIALOG, 0, 0);
	}
}

LRESULT CALLBACK pc_window::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	auto *self = reinterpret_cast<pc_window *>(GetWindowLongPtrW(h, GWLP_USERDATA));
	if (self && self->m_imgui) {
		ImGui::SetCurrentContext(self->m_imgui);
		// 文字を打つ箱の外では、文字（WM_CHAR）を ImGui に渡さない。鍵盤で弾こうとキーを押しっぱなしに
		// するとリピートで文字が毎秒何十も来て、ImGui がマウスの動きと交互に 1 コマずつしか進めない
		// （trickle）。マウスの軌跡が溜まって、絵の点も送る値も遅れてついてくる
		if (msg == WM_CHAR && !ImGui::GetIO().WantTextInput)
			return 0;
		// 日本語配列の ￥（スキャンコード 0x7D）は ImGui の受け口が拾わない。Oem102（JIS には無いキー）として渡す
		if ((msg == WM_KEYDOWN || msg == WM_KEYUP) && ((lp >> 16) & 0xff) == 0x7d)
			ImGui::GetIO().AddKeyEvent(ImGuiKey_Oem102, msg == WM_KEYDOWN);
		if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp))
			return 1;
	}

	switch (msg) {
	case WM_SIZE:
		if (self && wp != SIZE_MINIMIZED) {
			self->m_resize_w = LOWORD(lp);
			self->m_resize_h = HIWORD(lp);
		}
		return 0;
	case WM_SYSCOMMAND:
		if ((wp & 0xfff0) == SC_KEYMENU)     // Alt で窓の品書きに入らない
			return 0;
		break;
	case WM_APP_FILE_DIALOG:
		if (self && self->m_file_ask) {
			const xgui::file_ask ask = xgui::file_ask(self->m_file_ask);
			std::vector<u8> bytes = std::move(self->m_file_bytes);
			file_dialog(h, ask, bytes);
			self->m_file_ask = 0;
		}
		return 0;
	case WM_CLOSE:
		ShowWindow(h, SW_HIDE);              // 消さずに隠す
		return 0;
	case WM_DROPFILES: {
		// 落とされたファイルの 1 つ目だけを渡す
		const HDROP drop = HDROP(wp);
		wchar_t path[MAX_PATH * 4] = {};
		const bool got = DragQueryFileW(drop, 0, path, UINT(std::size(path))) > 0;
		DragFinish(drop);
		if (got && s_drop)
			s_drop(to_utf8(path));
		return 0;
	}
	}
	return DefWindowProcW(h, msg, wp, lp);
}

namespace lang_detail {

// The OS default locale for ui::locale_default_lang (ui/lang.h): reporting
// the tag is all this backend does (windows.h comes with the Win32 backend
// headers above). GUI processes here usually run without LANG set, so
// Japanese installs would otherwise come up English.
namespace {
std::string query_os_locale()
{
	wchar_t name[LOCALE_NAME_MAX_LENGTH];
	if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) <= 0)
		return {};
	char narrow[LOCALE_NAME_MAX_LENGTH * 3];
	std::snprintf(narrow, sizeof(narrow), "%ls", name);
	return narrow;
}
const os_locale_registrar os_locale_reg(query_os_locale);
} // namespace

} // namespace lang_detail

} // namespace ui
