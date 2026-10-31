// license:BSD-3-Clause
//
// Linux pc_window: one SDL3 window + SDL_gpu swapchain per ImGui view.
//
// Event routing needs a window-ID lookup because SDL events are global:
// every live window registers itself, and route_event() feeds the owner.
// Closing (the X button) only hides, mirroring the other platforms.

#include "pc_window_linux.h"

#include "imgui.h"

#include "ui/font_file.h"   // the fontconfig lookup lives in there
#include "ui/sdl_popup.h"
#include "ui/texts.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace ui {

namespace {

using drop_fn = void (*)(const std::string &path);

// The editors' file requests (xgui::ask_open_wav, ask_open_dls, ask_open_card, ask_open_midi, ask_save_file...),
// answered with SDL's file panel after the frame, the way pc_window.cpp does it on Windows. Without this the
// Linux editors only had a path box (issue #210). One file at a time for MIDI (the SDL panel picks one)
void file_dialog(SDL_Window *win, xgui::file_ask ask, const std::vector<u8> &bytes)
{
	std::atomic<bool> quit{ false };
	const bool wav = ask == xgui::file_ask::open && xgui::file_ask_is_wav();
	const bool dls = ask == xgui::file_ask::open && xgui::file_ask_is_dls();
	const bool card = !dls && ask == xgui::file_ask::open && xgui::file_ask_is_card();
	const bool midi = ask == xgui::file_ask::open && xgui::file_ask_is_midi();
	using sdl_popup::file_filter;
	if (ask == xgui::file_ask::save) {
		const file_filter f[] = { { UI_TEXT(dlg_sysex_desc, "SysEx"), "syx" } };
		const std::string path = sdl_popup::save_file(win, quit, f, 1, "S-MU2000.syx");
		if (path.empty())
			return;
		std::FILE *fp = std::fopen(path.c_str(), "wb");
		const bool ok = fp && std::fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
		if (fp)
			std::fclose(fp);
		char note[64];
		std::snprintf(note, sizeof(note), ok ? UI_TEXT(note_exported_fmt, "Exported (%zu bytes)") : UI_TEXT(note_export_fail, "Cannot export"), bytes.size());
		xgui::set_file_note(note);
		return;
	}
	if (ask != xgui::file_ask::open)
		return;
	file_filter f[2];
	if (midi)
		f[0] = { UI_TEXT(dlg_midi_desc, "MIDI files"), "mid;midi;smf;rcp;r36;m3u;m3u8" };
	else if (dls)
		f[0] = { UI_TEXT(dlg_dls_desc, "DLS sound bank"), "dls" };
	else if (card)
		f[0] = { UI_TEXT(dlg_card_or_m2a_desc, "SmartMedia image or M2A file"), "img;sm;m2a" };
	else if (wav)
		f[0] = { UI_TEXT(dlg_wav_desc, "WAV audio or SysEx"), "wav;syx" };
	else
		f[0] = { UI_TEXT(dlg_sysex_desc, "SysEx"), "syx" };
	f[1] = { UI_TEXT(dlg_all_files, "All files"), "*" };
	const std::string path = sdl_popup::open_file(win, quit, f, 2, std::string());
	if (path.empty())
		return;
	if (midi) {
		xgui::give_opened_midi({ path });
		return;
	}
	if (dls) {
		xgui::give_opened_dls(path);
		return;
	}
	if (card) {
		xgui::give_opened_card(path);          // big images: only the place, the sampling window reads it
		return;
	}
	std::vector<u8> in;
	if (std::FILE *fp = std::fopen(path.c_str(), "rb")) {
		u8 buf[65536];
		size_t n;
		const size_t cap = wav ? (64u << 20) : (16u << 20);
		while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0 && in.size() < cap)
			in.insert(in.end(), buf, buf + n);
		std::fclose(fp);
		xgui::give_opened_file(std::move(in));
	} else {
		xgui::set_file_note(UI_TEXT(note_import_fail, "Could not import"));
	}
}

drop_fn &drop_handler()
{
	static drop_fn fn = nullptr;
	return fn;
}

std::mutex &registry_mutex()
{
	static std::mutex m;
	return m;
}

std::vector<pc_window *> &registry()
{
	static std::vector<pc_window *> v;
	return v;
}

// The title comes from imgui_view as UTF-32 (wchar_t is 32 bits here, unlike
// Windows). SDL wants UTF-8, so convert plainly (same helper as macOS).
// The view already picks the language (get_lang in each title()), so this
// converts whatever comes back.
std::string title_of(const imgui_view &view)
{
	std::string utf8;
	for (const wchar_t *w = view.title(); w && *w; w++) {
		const unsigned int c = unsigned(*w);
		if (c < 0x80) {
			utf8 += char(c);
		} else if (c < 0x800) {
			utf8 += char(0xc0 | (c >> 6));
			utf8 += char(0x80 | (c & 0x3f));
		} else if (c < 0x10000) {
			utf8 += char(0xe0 | (c >> 12));
			utf8 += char(0x80 | ((c >> 6) & 0x3f));
			utf8 += char(0x80 | (c & 0x3f));
		} else {
			utf8 += char(0xf0 | (c >> 18));
			utf8 += char(0x80 | ((c >> 12) & 0x3f));
			utf8 += char(0x80 | ((c >> 6) & 0x3f));
			utf8 += char(0x80 | (c & 0x3f));
		}
	}
	return utf8;
}

} // namespace

void pc_window::set_drop_handler(drop_fn fn)
{
	drop_handler() = fn;
}

pc_window::~pc_window()
{
	std::lock_guard<std::mutex> hold(registry_mutex());
	auto &v = registry();
	v.erase(std::remove(v.begin(), v.end(), this), v.end());
	destroy();
}

bool pc_window::show(std::string &err)
{
	if ((!m_win || !m_im.ctx) && !create(err))
		return false;
	SDL_ShowWindow(m_win);
	SDL_RaiseWindow(m_win);
	return true;
}

void pc_window::hide()
{
	if (m_win)
		SDL_HideWindow(m_win);
}

void pc_window::close()
{
	hide();
	destroy();
}

bool pc_window::visible() const
{
	if (!m_win)
		return false;
	const SDL_WindowFlags flags = SDL_GetWindowFlags(m_win);
	return !(flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
}

void pc_window::shutdown(bridge &br)
{
	if (!m_imgui)
		return;
	ImGui::SetCurrentContext(m_imgui);
	m_view->hidden(br);
}

bool pc_window::create(std::string &err)
{
	// the editors offer their file buttons (answered by file_dialog() above)
	xgui::set_file_dialogs(true);
	xgui::set_midi_dialog(true);
	destroy();
	m_win = SDL_CreateWindow(title_of(*m_view).c_str(), m_view->default_width(),
	                         m_view->default_height(), SDL_WINDOW_RESIZABLE);
	if (!m_win) {
		err = SDL_GetError();
		return false;
	}
	m_id = SDL_GetWindowID(m_win);

	// The same bring-up the main window uses, on this window's own swapchain
	// over the one shared device (ui/imgui_shell_sdl.h). sdl_start() makes
	// the ImGui context, so everything that needs one comes after it.
	if (!imshell::sdl_start(m_im, m_win)) {
		err = "ImGui SDL_gpu backends failed to start";
		destroy();
		return false;
	}
	m_imgui = m_im.ctx;

	ImGui::SetCurrentContext(m_imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;       // no imgui.ini littered into the working folder

	// The same look the other four windows get (pc_window.cpp,
	// pc_window_mac.mm). Without this the editor windows were the only ones
	// in ImGui's default light style with square frames.
	ImGui::StyleColorsDark();
	ImGuiStyle &st = ImGui::GetStyle();
	st.FrameRounding = 3;

	// The one shared font setup: ui/font_file.h asks fontconfig for a face,
	// checks it can draw what the panel writes, and reads it once.
	add_cjk_ui_font(io.Fonts);

	std::lock_guard<std::mutex> hold(registry_mutex());
	registry().push_back(this);
	return true;
}

void pc_window::destroy()
{
	if (m_im.ctx) {
		// sdl_stop() drops the panel's textures and destroys the context.
		// Unclaiming the window afterwards takes its swapchain with it; the
		// order matters, the swapchain outlives the ImGui context.
		SDL_Window *w = m_win;
		imshell::sdl_stop(m_im);
		m_imgui = nullptr;
		if (w && imshell::gpu_device())
			SDL_ReleaseWindowFromGPUDevice(imshell::gpu_device(), w);
	}
	if (m_win) {
		SDL_DestroyWindow(m_win);
		m_win = nullptr;
		m_id = 0;
	}
}

void pc_window::frame(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	const bool shown = visible();
	if (m_was_visible && !shown && m_imgui) {
		ImGui::SetCurrentContext(m_imgui);
		m_view->hidden(br);                  // closed or minimized: release what it held
	}
	m_was_visible = shown;
	if (!shown)
		return;
	ImGui::SetCurrentContext(m_imgui);

	// One window, one frame: sdl_begin() opens it and sdl_present() closes it
	// (Render() and the swapchain submit live in there). The main window's
	// pump draws its frame between the same two calls.
	imshell::sdl_begin(m_im);
	m_view->draw(m, ram, br);
	xgui::drag_flush(br);          // マウスで動かしている値の、間引いた送信

	imshell::sdl_present(m_im, m_win);
	// no wait: gui's timer (30 frames a second) decides the pace

	// A file panel the view asked for opens now, outside the frame (pc_window.cpp does the same on Windows)
	std::vector<u8> bytes;
	const xgui::file_ask ask = xgui::take_file_ask(bytes);
	if (ask != xgui::file_ask::none)
		file_dialog(m_win, ask, bytes);
}

bool pc_window::route_event(const SDL_Event &ev)
{
	// A file dropped on an editor window plays, the same as on the panel.
	if (ev.type == SDL_EVENT_DROP_FILE && ev.drop.data) {
		const Uint32 id = ev.drop.windowID;
		std::lock_guard<std::mutex> hold(registry_mutex());
		for (pc_window *w : registry()) {
			if (w && w->m_id == id) {
				// The name belongs to SDL3 (not freed here; see window_sdl.cpp)
				if (drop_handler())
					drop_handler()(ev.drop.data);
				return true;
			}
		}
		return false;
	}
	// Every event the panel's pump handles for itself has to be claimed here
	// when it belongs to an editor window, or the panel will act on it. SDL
	// delivers one global queue, and the panel's switch has no windowID test:
	// a WINDOW_RESIZED from the List window used to fall through to it and set
	// the panel's own ww/wh to the List window's size, so the panel redrew its
	// art to the wrong dimensions inside an unchanged window -- drawn small and
	// pushed to one corner. FOCUS_LOST had the same shape: losing focus to an
	// editor window made the panel drop its focus state.
	Uint32 id = 0;
	switch (ev.type) {
	case SDL_EVENT_MOUSE_MOTION:      id = ev.motion.windowID; break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:   id = ev.button.windowID; break;
	case SDL_EVENT_MOUSE_WHEEL:       id = ev.wheel.windowID; break;
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:            id = ev.key.windowID; break;
	case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_EXPOSED:
	case SDL_EVENT_WINDOW_MINIMIZED:
	case SDL_EVENT_WINDOW_MAXIMIZED:
	case SDL_EVENT_WINDOW_RESTORED:
	case SDL_EVENT_WINDOW_FOCUS_GAINED:
	case SDL_EVENT_WINDOW_FOCUS_LOST:
		id = ev.window.windowID;
		break;
	default:
		return false;
	}
	std::lock_guard<std::mutex> hold(registry_mutex());
	for (pc_window *w : registry()) {
		if (!w || w->m_id != id || !w->m_imgui)
			continue;
		if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
			w->hide();   // the X button hides, like the other platforms
			return true;
		}
		ImGui::SetCurrentContext(w->m_imgui);
		// Text only while a text box is active. A held key (playing notes from the keyboard)
		// repeats text, and ImGui trickles text against mouse moves, so a drag lags behind
		if (ev.type == SDL_EVENT_TEXT_INPUT && !ImGui::GetIO().WantTextInput)
			return true;
		ImGui_ImplSDL3_ProcessEvent(&ev);
		return true;
	}
	return false;
}

} // namespace ui
