// license:BSD-3-Clause
//
// The shared half of the VST3 view: the VST3 interface itself, the panel, and
// the handling of mouse and key input. The window that holds it is per
// platform (view_win.cpp, view_mac.mm), reached through plug_window.h.
//
// This file is plain C++ and includes compat/gdi.h, which is what paints the
// panel on both platforms.

#include "view.h"
#include "plug_window.h"

#include "compat/gdi.h"
#include "compat/platform.h"
#include "engine.h"
#include "smartmedia.h"
#include "ui/bridge.h"
#include "ui/layout.h"
#include "ui/panel.h"
#include "ui/toolbar.h"

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

using namespace Steinberg;

namespace smu2000 {
namespace vst3 {

namespace {

// plug_key -> mu2000::button: the one place that decides. Each platform maps
// its own key codes onto plug_key, so this stays the single answer to "what
// does this key do", and it matches gui.cpp
mu2000::button button_of(int code, bool &ok)
{
	ok = true;
	switch (code) {
	case PLUG_KEY_PLAY:          return mu2000::button::play;
	case PLUG_KEY_EDIT:          return mu2000::button::edit;
	case PLUG_KEY_UTIL:          return mu2000::button::util;
	case PLUG_KEY_EFFECT:        return mu2000::button::effect;
	case PLUG_KEY_MUTE_SOLO:     return mu2000::button::mute_solo;
	case PLUG_KEY_PART_PLUS:     return mu2000::button::part_plus;
	case PLUG_KEY_PART_MINUS:    return mu2000::button::part_minus;
	case PLUG_KEY_VALUE_PLUS:    return mu2000::button::value_plus;
	case PLUG_KEY_VALUE_MINUS:   return mu2000::button::value_minus;
	case PLUG_KEY_ENTER:         return mu2000::button::enter;
	case PLUG_KEY_EXIT:          return mu2000::button::exit;
	case PLUG_KEY_SELECT_RIGHT:  return mu2000::button::select_right;
	case PLUG_KEY_SELECT_LEFT:   return mu2000::button::select_left;
	case PLUG_KEY_SEQ:           return mu2000::button::seq;
	case PLUG_KEY_AUDITION:      return mu2000::button::audition;
	case PLUG_KEY_SELECT:        return mu2000::button::select;
	case PLUG_KEY_SAMPLING_MODE: return mu2000::button::sampling_mode;
	default: break;
	}
	ok = false;
	return mu2000::button::count;
}

} // namespace

// mu2000::button -> plug_key for the panel keys: the reverse of button_of(),
// for platform windows that share their letter map through ui/keymap.h
// (shared characters become mu2000::button there, then these here).
// Returns PLUG_KEY_NONE for anything that is not a panel key.
plug_key plug_key_of_button(int button)
{
	const auto b = mu2000::button(button);
	switch (b) {
	case mu2000::button::play:          return PLUG_KEY_PLAY;
	case mu2000::button::edit:          return PLUG_KEY_EDIT;
	case mu2000::button::util:          return PLUG_KEY_UTIL;
	case mu2000::button::effect:        return PLUG_KEY_EFFECT;
	case mu2000::button::mute_solo:     return PLUG_KEY_MUTE_SOLO;
	case mu2000::button::part_plus:     return PLUG_KEY_PART_PLUS;
	case mu2000::button::part_minus:    return PLUG_KEY_PART_MINUS;
	case mu2000::button::value_plus:    return PLUG_KEY_VALUE_PLUS;
	case mu2000::button::value_minus:   return PLUG_KEY_VALUE_MINUS;
	case mu2000::button::enter:         return PLUG_KEY_ENTER;
	case mu2000::button::exit:          return PLUG_KEY_EXIT;
	case mu2000::button::select_right:  return PLUG_KEY_SELECT_RIGHT;
	case mu2000::button::select_left:   return PLUG_KEY_SELECT_LEFT;
	case mu2000::button::seq:           return PLUG_KEY_SEQ;
	case mu2000::button::audition:      return PLUG_KEY_AUDITION;
	case mu2000::button::select:        return PLUG_KEY_SELECT;
	case mu2000::button::sampling_mode: return PLUG_KEY_SAMPLING_MODE;
	default: break;
	}
	return PLUG_KEY_NONE;
}

// The panel lives here so that view.h can stay free of compat/gdi.h
struct plug_view::impl
{
	engine  &eng;
	ui::panel panel;
	// **窓を開くボタンの帯**。F3・F2 をホストが先に食う
	// DAW でも、ここからなら確実に開ける（ui/toolbar.h）
	ui::toolbar bar;

#if defined(_WIN32)
	// Double buffered: the host repaints at 30 frames a second and drawing
	// straight into the window would flicker
	HDC     mem_dc = nullptr;
	HBITMAP mem_bmp = nullptr;
	int     mem_w = 0, mem_h = 0;
#endif

	explicit impl(engine &e) : eng(e)
	{
		// The bar ids are the pc_kind the view dispatches (open_pc_window)
		static_assert(int(ui::BAR_LIST) == PC_LIST && int(ui::BAR_EDITOR) == PC_EDITOR &&
		              int(ui::BAR_FX) == PC_FX && int(ui::BAR_SHAPES) == PC_SHAPES &&
		              int(ui::BAR_MASTER) == PC_MASTER, "bar ids are pc_kind");
		bar.set_items(ui::window_bar_items());
		panel.set_top_inset(ui::toolbar::HEIGHT);
	}

	void paint_panel(HDC dc)
	{
		ui::snapshot s;
		eng.panel().read(s);

		char status[160];
		std::snprintf(status, sizeof(status), "%s", eng.message().c_str());

		panel.set_volume(eng.panel().gain());
		panel.paint(dc, s, eng.panel().buttons(), status);
		// 帯はパネルの**あと**に描く（パネルは全面を塗る）
		bar.paint(dc, panel.width());
	}

	void forget_backing()
	{
#if defined(_WIN32)
		if (mem_bmp) { DeleteObject(mem_bmp); mem_bmp = nullptr; }
		if (mem_dc)  { DeleteDC(mem_dc); mem_dc = nullptr; }
		mem_w = mem_h = 0;
#endif
	}
};


plug_view::plug_view(engine &eng)
	: m_impl(new impl(eng)), m_engine(eng)
{
	// パネルの配置。%LOCALAPPDATA%\S-MU2000\panel.txt があれば読む。無ければ
	// 束の中の写真調の絵（Resources/panel）、それも無ければ組み込みの配置
	// （doc/panel-editing.md）
	//
	// find_default() now searches the per-user settings directory on either
	// platform (~/Library/Application Support/S-MU2000 on macOS)
	const std::string lay = ui::layout::find_default();
	if (!lay.empty()) {
		std::string err;
		m_impl->panel.lay().load(lay, err);
	}
	// 画面（パネルと PC の窓）で XG の値を触ったら、プラグインの口からホストのオートメーションへ伝える
	m_impl->panel.xg().set_edit_listener([&eng](const xg::param &p, int part, int value) {
		eng.notify_edit(p, part, value);
	});
	m_impl->panel.xg().set_raw_listener([&eng](u32 addr, int size, int value) {
		eng.notify_edit_raw(addr, size, value);
	});
	// 絵は 1000:400、その上の帯（一覧・エディタ…）は比の外に足す
	m_h = m_w * ui::LOGICAL_H / ui::LOGICAL_W + m_impl->panel.top_inset();
	m_impl->panel.resize(m_w, m_h);
}

plug_view::~plug_view()
{
	removed();
}

tresult PLUGIN_API plug_view::queryInterface(const TUID iid, void **obj)
{
	if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) ||
	    FUnknownPrivate::iidEqual(iid, IPlugView::iid)) {
		addRef();
		*obj = static_cast<IPlugView *>(this);
		return kResultOk;
	}
	*obj = nullptr;
	return kNoInterface;
}

uint32 PLUGIN_API plug_view::addRef()  { return uint32(FUnknownPrivate::atomicAdd(m_refs, 1)); }

uint32 PLUGIN_API plug_view::release()
{
	if (FUnknownPrivate::atomicAdd(m_refs, -1) == 0) { delete this; return 0; }
	return uint32(m_refs);
}

tresult PLUGIN_API plug_view::isPlatformTypeSupported(FIDString type)
{
	return (type && !std::strcmp(type, plug_window_type())) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API plug_view::attached(void *parent, FIDString type)
{
	if (isPlatformTypeSupported(type) != kResultTrue || !parent)
		return kResultFalse;
	if (m_window)
		removed();

	m_window = plug_window_create(*this);
	if (!m_window || !m_window->attach(parent, m_w, m_h)) {
		delete m_window;
		m_window = nullptr;
		return kResultFalse;
	}
	m_impl->panel.resize(m_w, m_h);
	return kResultOk;
}

tresult PLUGIN_API plug_view::removed()
{
	// The card file is the project's data, so the last of it is written back
	// before the window goes: a host that closes the editor and never saves
	// still keeps what the machine wrote
	m_engine.card_flush();
	if (m_window) {
		m_window->detach();
		delete m_window;
		m_window = nullptr;
	}
	m_engine.notify_idle(true);
	m_impl->forget_backing();
	return kResultOk;
}

// ホスト経由の入力は使わない。子ウィンドウが本物のメッセージを受け取る
tresult PLUGIN_API plug_view::onWheel(float)                          { return kResultFalse; }
tresult PLUGIN_API plug_view::onKeyDown(char16, int16, int16)         { return kResultFalse; }
tresult PLUGIN_API plug_view::onKeyUp(char16, int16, int16)           { return kResultFalse; }
tresult PLUGIN_API plug_view::onFocus(TBool)                          { return kResultOk; }

tresult PLUGIN_API plug_view::getSize(ViewRect *size)
{
	if (!size)
		return kInvalidArgument;
	size->left = 0; size->top = 0;
	size->right = m_w; size->bottom = m_h;
	return kResultOk;
}

tresult PLUGIN_API plug_view::onSize(ViewRect *r)
{
	if (!r)
		return kInvalidArgument;
	m_w = std::max<int32>(r->getWidth(), 640);
	m_h = std::max<int32>(r->getHeight(), 180);
	if (m_window)
		m_window->set_size(m_w, m_h);
	m_impl->panel.resize(m_w, m_h);
	return kResultOk;
}

tresult PLUGIN_API plug_view::setFrame(IPlugFrame *frame)
{
	m_frame = frame;
	return kResultOk;
}

tresult PLUGIN_API plug_view::canResize() { return kResultTrue; }

tresult PLUGIN_API plug_view::checkSizeConstraint(ViewRect *rect)
{
	if (!rect)
		return kInvalidArgument;
	// 横に長い機械なので、縦横比はこちらで決めてしまう。上の帯は比の外に
	// 足す（gui.exe の窓と同じ）。足さないと絵が高さで決まって左右が余る
	const int w = std::max<int32>(rect->getWidth(), 640);
	const int h = std::max<int32>(w * ui::LOGICAL_H / ui::LOGICAL_W + m_impl->panel.top_inset(), 180);
	rect->right = rect->left + w;
	rect->bottom = rect->top + h;
	return kResultTrue;
}


// ---- Called by the platform window

void plug_view::repaint(void *native, int w, int h)
{
	if (!native || w <= 0 || h <= 0)
		return;

	// パラメータの層: 音源の返事を読み、見えている面の読み返しを頼む
	// (this is the GUI thread -- the Win32 timer and the macOS one both arrive
	//  here, so the polling happens once per frame on either platform)
	m_impl->panel.tick(m_engine.panel());
	// PC で触る窓（一覧・エディタ）。見えていなければ何もしない
	if (m_window)
		m_window->pc_frame(m_impl->panel.xg(), m_impl->panel.ram(), m_engine.panel());
	// 触っている最中の値の操作（ホストへの beginEdit / endEdit）を、しばらく触られていなければ終える
	m_engine.notify_idle(false);

#if defined(_WIN32)
	HDC dst = static_cast<HDC>(native);
	if (!m_impl->mem_dc || m_impl->mem_w != w || m_impl->mem_h != h) {
		m_impl->forget_backing();
		m_impl->mem_dc  = CreateCompatibleDC(dst);
		m_impl->mem_bmp = CreateCompatibleBitmap(dst, w, h);
		SelectObject(m_impl->mem_dc, m_impl->mem_bmp);
		m_impl->mem_w = w;
		m_impl->mem_h = h;
	}
	m_impl->paint_panel(m_impl->mem_dc);
	BitBlt(dst, 0, 0, w, h, m_impl->mem_dc, 0, 0, SRCCOPY);
#elif defined(__APPLE__)
	// The subview is flipped, so the context is already top-left with y down
	// and only has to be wrapped -- no flipping, same as the GUI window
	CGContextRef ctx = static_cast<CGContextRef>(native);
	HDC dc = static_cast<HDC>(smu_gdi_wrap_view_context(ctx, w, h));
	m_impl->paint_panel(dc);
	DeleteDC(dc);
#else
	// Linux headless build (plug_window_linux): the stub window never paints.
	// The editor view arrives in a later phase (doc/porting-linux-gui.md).
	(void)native; (void)w; (void)h;
#endif

	card_tick();
}

void plug_view::mouse_down(int x, int y)
{
	// **帯が先**。ここはパネルでは無いので、機器には何も伝えない
	const int id = m_impl->bar.hit(x, y);
	if (id >= 0) {
		m_impl->bar.set_down(id);
		if (m_window)
			m_window->open_pc_window(id);
		return;
	}
	if (y < ui::toolbar::HEIGHT)
		return;                  // 帯の隙間

	// The card slot is not a button but a menu: a click there is about the image
	// in the slot, and the machine is told nothing
	if (card_slot_at(x, y)) {
		if (m_window)
			m_window->card_menu(x, y);
		return;
	}
	m_impl->panel.press(x, y, m_engine.panel());
}

void plug_view::mouse_right(int x, int y)
{
	if (!m_window)
		return;
	// The card slot answers both buttons with its menu. Anywhere else the
	// window is offered the click instead (the GUI front end opens its own
	// settings menu there)
	if (card_slot_at(x, y))
		m_window->card_menu(x, y);
	else
		m_window->panel_menu(x, y);
}

void plug_view::mouse_drag(int x, int y) { m_impl->panel.drag(x, y, m_engine.panel()); }

void plug_view::mouse_up()
{
	m_impl->bar.set_down(-1);
	m_impl->panel.release(m_engine.panel());
}

void plug_view::wheel(int x, int y, int steps)
{
	if (steps)
		m_impl->panel.wheel_at(x, y, steps, m_engine.panel());
}

void plug_view::key(plug_key code, bool down)
{
	// **F4 で native の口を入切**（gui.exe と同じ。6.207）。
	// 切り替えは音声の糸がつぎの区間の頭で行う
	if (code == PLUG_KEY_ENGINE) {
		if (down)
			m_engine.request_native_engine(m_engine.native_engine() ? 0 : 1);
		return;
	}
	// 窓を開くキーはパネルのボタンでは無いので、押したときだけ見る
	if (code == PLUG_KEY_LIST || code == PLUG_KEY_EDITOR) {
		if (down && m_window)
			m_window->open_pc_window(code == PLUG_KEY_LIST ? PC_LIST : PC_EDITOR);
		return;
	}
	bool ok = false;
	const mu2000::button b = button_of(code, ok);
	if (ok)
		m_engine.panel().press(b, down);
}

void plug_view::focus_lost() { m_engine.panel().release_all(); }

// The window has no logger of its own, and what it wants to record is about the
// host rather than the panel -- see the click note in view_mac.mm
void plug_view::log_line(const char *text) { m_engine.log_line(text); }


// ---- SmartMedia (the card slot)

bool plug_view::card_slot_at(int x, int y) const { return m_impl->panel.on_card_slot(x, y); }

bool plug_view::card_ready() const { return m_engine.state() == status::ready; }

std::string plug_view::card_path() const { return m_engine.card_path(); }

// A failure has nowhere to go on the panel itself, so it goes to the log
// (engine) and to the user (the window's alert)
void plug_view::card_error(const std::string &err)
{
	m_engine.log_line(err.c_str());
	if (m_window)
		m_window->alert(err);
}

void plug_view::card_make(const std::string &path, int mb)
{
	// An empty card, in the physical layout a new one comes in: the machine
	// still has to format it (UTIL -> CARD -> Format) before it stores anything
	std::string err;
	smartmedia card;
	if (!card.create(u32(mb)) || !card.save(path, err)) {
		card_error(err.empty() ? UI_TEXT(dlg_card_create_fail, "Cannot create the SmartMedia image") : err);
		return;
	}
	if (!m_engine.card_insert(path, err)) {
		card_error(err);
		return;
	}
	// A fresh card only carries the physical layout, so it has to be formatted
	// on the machine before it holds anything. gui.cpp says the same thing when
	// one is made there
	if (m_window)
		m_window->alert(UI_TEXT(dlg_fresh_card, "Inserted a blank SmartMedia image.\n"
		                                        "Before use, format it on the machine: UTIL → CARD → Format."));
}

void plug_view::card_insert_path(const std::string &path)
{
	std::string err;
	if (!m_engine.card_insert(path, err))
		card_error(err);
}

void plug_view::card_eject() { m_engine.card_eject(); }

// Called once a frame, by whichever platform is painting. The machine writes to
// the card while it runs, so the file is brought up to date every couple of
// seconds instead of only when a project is saved; that is what keeps a crash
// from losing more than the last two seconds. Saving and closing flush as well
// (the host interface on save, and removed() when the window goes)
void plug_view::card_tick()
{
	const uint64_t now = smu2000::perf_ticks() * 1000 / smu2000::perf_freq();
	if (now - m_last_flush < 2000)
		return;
	m_last_flush = now;
	m_engine.card_flush();
}

} // namespace vst3
} // namespace smu2000
