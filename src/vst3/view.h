// license:BSD-3-Clause
//
// VST3 の画面（IPlugView）。ホストから渡された親ウィンドウの中に
// 子ウィンドウを 1 枚作り、gui.exe と同じ ui::panel で描く。
//
// SDK の土台（public.sdk / VSTGUI）は使っていないので、ここは素の Win32。
//
// The child window itself now lives per platform behind plug_window.h, so this
// header mentions no window system at all.
//

#ifndef S_MU2000_VST3_VIEW_H
#define S_MU2000_VST3_VIEW_H

#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include "plug_window.h"

#include "ui/fonts.h"      // ui::im::fonts, ImGui only -- NOT ui/draw_imgui.h,
                           // which needs compat/gdi.h and so <windows.h> on
                           // Windows, where `interface` is a macro for `struct`
                           // and the SDK headers have a member by that name

#include "imgui.h"

#include <cstdint>
#include <memory>
#include <string>

namespace smu2000 {
namespace vst3 {

class engine;
class plug_window;

// mu2000::button value -> plug_key for the panel keys (view.cpp): the reverse
// of its button_of(), for platform windows that share their letter map through
// ui/keymap.h. int because mu2000.h is too heavy for this header (MAME CPU
// headers next to Objective-C). Returns PLUG_KEY_NONE for anything that is not
// a panel key.
plug_key plug_key_of_button(int button);

class plug_view : public Steinberg::IPlugView
{
public:
	// owner は engine を持つプラグイン本体（VST3 の IEditController）。画面が生きている間は参照を
	// 1 つ持って、本体（と engine）を先に消させない。ホストが画面より先に本体を手放しても、
	// 画面のタイマーや removed() が消えた engine に触らないように（VST2 は自分で順番を守るので null）
	explicit plug_view(engine &eng, Steinberg::FUnknown *owner = nullptr);
	virtual ~plug_view();

	// ---- FUnknown
	Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
	Steinberg::uint32 PLUGIN_API addRef() override;
	Steinberg::uint32 PLUGIN_API release() override;

	// ---- IPlugView
	Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
	Steinberg::tresult PLUGIN_API attached(void *parent, Steinberg::FIDString type) override;
	Steinberg::tresult PLUGIN_API removed() override;
	Steinberg::tresult PLUGIN_API onWheel(float distance) override;
	Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 code,
	                                        Steinberg::int16 modifiers) override;
	Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 code,
	                                      Steinberg::int16 modifiers) override;
	Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect *size) override;
	Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect *newSize) override;
	Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) override;
	Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame *frame) override;
	Steinberg::tresult PLUGIN_API canResize() override;
	Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect *rect) override;

	// ---- Called by the platform window (view_win.cpp / view_mac.mm),
	// already inside a frame: dl is the background draw list to paint into.
	int  width() const { return m_w; }
	int  height() const { return m_h; }
	// 開く前の大きさ（VST2 は窓を作る前に聞いてくる）。1000 × (400 + 上の帯)
	static int default_width();
	static int default_height();

	// The panel's fonts need an ImGui context and no open frame; the platform
	// window calls this once its context is up (see panel::fonts_ready)
	void fonts_ready();

	void repaint(ImDrawList *dl, const ui::im::fonts &fonts, int w, int h);
	void mouse_down(int x, int y);
	void mouse_drag(int x, int y);
	void mouse_up();
	void wheel(int x, int y, int steps);
	void key(plug_key code, bool down);
	void focus_lost();
	void mouse_right(int x, int y);         // the card slot answers a right click
	void log_line(const char *text);        // one line to the engine's log

	// ---- SmartMedia (the card slot on the front panel)
	//
	// The card itself belongs to the engine (see its card_insert / card_eject).
	// What is here is what both platforms do the same way: the hit test for the
	// slot, making an empty image, and writing dirty blocks back on a timer.
	// The menu that offers those, and the file dialogs behind it, are the
	// window's -- see plug_window::card_menu
	bool card_slot_at(int x, int y) const;
	bool card_ready() const;
	std::string card_path() const;
	void card_make(const std::string &path, int mb);   // create an empty image, then insert it
	void card_insert_path(const std::string &path);
	void card_eject();
	void card_tick();                                  // flush every 2 seconds

private:
	void card_error(const std::string &err);           // log it and tell the user

	struct impl;                            // the panel and its toolbar
	std::unique_ptr<impl> m_impl;

	engine &m_engine;
	Steinberg::FUnknown *m_owner = nullptr;   // 参照を持っているプラグイン本体
	plug_window *m_window = nullptr;

	// When the card was last written back, in milliseconds
	uint64_t m_last_flush = 0;
	// gui.exe と同じ既定の幅。高さはコンストラクタで 1000:400 ＋ 上の帯
	int m_w = 1000, m_h = 400;
	// The host resized the editor while it was open: the width is written
	// to the settings folder when the window goes (view.cpp, last_width)
	bool m_resized = false;
	Steinberg::int32 m_refs = 1;
	Steinberg::IPlugFrame *m_frame = nullptr;
};

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_VIEW_H
