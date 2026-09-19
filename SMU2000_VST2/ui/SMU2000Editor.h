// license:BSD-3-Clause
//
// iPlug2 (VST2 / CLAP) 用ネイティブ・エディタ。
//
// VST3 版（src/vst3/view.cpp）と「同じもの」を出す。中身は src/ui/panel の
// GDI 描画で、ホストがくれた親 HWND の中に子ウィンドウを 1 枚作って、そこへ
// 実機のフロントパネルを描く。IGraphics / NanoVG / OpenGL / Skia は使わない
// （だから GUI-ON でもグラフレスはそのまま = 台帳 hard rule #6 を満たす）。
//
// iPlug2 のエディタ経路は、IGraphics を使わない場合のために
// IEditorDelegate::OpenWindow(void* pParent) / CloseWindow() を空実装で用意して
// いる。VST2 は effEditOpen/effEditClose、CLAP は guiSetParent/guiDestroy から
// これを呼ぶので、この 2 つを差し替えるだけでホストの画面になる。入力（マウス・
// キー・ホイール）は子ウィンドウの WndProc が直接受け取る。VST3 版と同じ作り。

#ifndef SMU2000_EDITOR_H
#define SMU2000_EDITOR_H

#pragma once

// IGraphics を使わないビルド専用のヘッダ。IGEditorDelegate が混ざった
// （NO_IGRAPHICS でない）構成で誤ってインクルードされたら早期的に落とす。
#if !defined(NO_IGRAPHICS)
#error "SMU2000Editor.h is the native (non-IGraphics) editor; build with NO_IGRAPHICS"
#endif

#include "../../src/ui/panel.h"   // ui::panel, ui::snapshot（GDI 依存）

#include <windows.h>

namespace smu2000 {

namespace vst3 { class engine; }

// iPlug のエディタ本体。Plugin::OpenWindow/CloseWindow から呼ばれる。
class editor
{
public:
	explicit editor(vst3::engine &eng);
	~editor();

	editor(const editor &) = delete;
	editor &operator=(const editor &) = delete;

	// pParent = ホストがくれた親 HWND（VST2: effEditOpen の ptr / CLAP: guiSetParent の
	// win32）。子ウィンドウを作り直して表示し、その HWND を返す。
	void *open(void *parent);
	void  close();

	bool attached() const { return m_hwnd != nullptr; }

	// ホスト側から大きさが変わったとき（CLAP の resize / OnParentWindowResize）。
	// 実際のウィンドウは WM_SIZE で追従するので、ここでは論理寸法だけ揃える。
	void set_size(int w, int h);
	int  width()  const { return m_w; }
	int  height() const { return m_h; }

private:
	static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	void paint(HWND h);
	void card_menu(HWND h, int x, int y);
	void card_command(HWND h, UINT id);

	vst3::engine &m_engine;

	HWND       m_hwnd   = nullptr;
	ui::panel  m_panel;

	HDC        m_mem_dc = nullptr;
	HBITMAP    m_mem_bmp = nullptr;
	int        m_mem_w = 0, m_mem_h = 0;
	DWORD      m_last_flush = 0;      // SmartMedia を最後にファイルへ書き戻した時刻

	int        m_w;
	int        m_h;
};

} // namespace smu2000

#endif // SMU2000_EDITOR_H
