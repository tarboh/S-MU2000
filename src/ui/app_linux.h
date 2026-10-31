// license:BSD-3-Clause
//
// The Linux front end's app: ui::app answering the SDL3 window
// (ui/app_linux.cpp, the pump in gui_linux.cpp). gui_linux.cpp keeps main();
// the Windows and macOS twins are ui/app_win.h and ui/app_mac.h.
//
// The class lives in files of its own rather than beside the window system
// code, so SDL3's headers stay out of the shared app header.
// The layout is the three files every platform has: window system (window_sdl,
// sdl_popup, pc_window_linux), app class (this), main (gui_linux.cpp).
//
// The panel language comes from ui::lang (--lang, editor.ini, locale:
// Japanese iff the locale says ja, English otherwise). The UI strings
// (ui/texts.h) and the ImGui help (ui/xg_ui.cpp) follow the same choice.

#ifndef S_MU2000_UI_APP_LINUX_H
#define S_MU2000_UI_APP_LINUX_H

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "app.h"
#include "menu.h"
#include "sdl_popup.h"
#include "window_sdl.h"    // run_window, called from pump_window

namespace ui {

// gui.ini lives under the per-user settings directory (compat/paths.h)
std::string settings_file_path();

class linux_app : public app
{
public:
	// mi is MIDI IN A-D, mu2000::MIDI_PORTS of them
	explicit linux_app(bridge &b) : app(b) {}

	// ---- the SDL window the pump in gui_linux.cpp maintains

	SDL_Window   *win = nullptr;      // for dialogs, message boxes and popups
	int           ww = 0, wh = 0;
	// No renderer member: the window draws through SDL_gpu, and the state
	// that needs (device, swapchain) lives with the window pump in
	// window_sdl.cpp. This class only ever handed `ren` to the popup menu,
	// which now presents through imshell::sdl_present().
	// Live ImGui state for the main window, set by run_window.
	// run_list's menu draws through the same frame.
	ImGuiContext *imgui_ctx = nullptr;
	im::fonts     imgui_fonts{};

	// ---- ui::app hooks: dialogs, confirmations and error display are
	// SDL's business (ui/sdl_popup), everything they decide is shared

	std::string settings_path() const override { return settings_file_path(); }
	void menu_error(const std::string &text) override
	{
		sdl_popup::alert(win, "S-MU2000", text);
	}
	void menu_note(const std::string &text) override
	{
		sdl_popup::alert(win, "S-MU2000", text);
	}
	std::string ask_card_open_path() override
	{
		static const sdl_popup::file_filter f[] = {
			{ "SmartMedia images", "img" },
			{ UI_TEXT(dlg_all_files, "All files"), "*" },
		};
		return sdl_popup::open_file(win, quit, f, 2, "");
	}
	std::string ask_card_folder_path() override
	{
		return sdl_popup::open_folder(win, quit);
	}
	std::string ask_card_save_path() override
	{
		static const sdl_popup::file_filter f[] = {
			{ "SmartMedia images", "img" },
		};
		return sdl_popup::save_file(win, quit, f, 1, "smartmedia.img");
	}
	std::string ask_midi_file_path() override
	{
		static const sdl_popup::file_filter f[] = {
			{ UI_TEXT(dlg_midi_desc, "MIDI files"), "mid;midi;rcp;r36" },
			{ "All files", "*" },
		};
		return sdl_popup::open_file(win, quit, f, 2, "");
	}
	bool confirm_factory_reset() override
	{
		return sdl_popup::confirm(win, "S-MU2000",
		                          UI_TEXT(dlg_factory_text, "Reset the MU2000 to factory state and restart it.\n"
		                                                  "Utility settings and remembered volume/voice settings will all be erased."),
		                          UI_TEXT(dlg_factory_ok, "Reset"));
	}

	// ---- the shared popups, rendered through ui/sdl_popup

	// The groups ui::app::context_menu builds, shown over the live panel.
	// A group whose title is empty pours its items into the top level; a
	// titled group becomes a submenu entry that opens one flat list -- two
	// levels at most, exactly what menu_win.h renders from the same groups
	int show_popup(const std::vector<menu_group> &groups, int x, int y)
	{
		std::vector<sdl_popup::item> top;
		std::vector<const menu_group *> subs;
		for (const menu_group &g : groups) {
			if (g.title.empty()) {
				append_group_items(top, g.items);
				continue;
			}
			sdl_popup::item it;
			it.label = g.title;
			it.submenu = true;
			it.sub = int(subs.size());
			subs.push_back(&g);
			top.push_back(it);
		}
		int sub = -1;
		const int id = run_list(top, x, y, sub);
		if (sub < 0 || sub >= int(subs.size()))
			return id;
		// The submenu body: the group's title as a disabled head, then its
		// items -- the way the old hand-built Linux submenus drew it
		std::vector<sdl_popup::item> body;
		sdl_popup::item head;
		head.label = subs[size_t(sub)]->title;
		head.enabled = false;
		body.push_back(head);
		sdl_popup::item sep;
		sep.separator = true;
		body.push_back(sep);
		append_group_items(body, subs[size_t(sub)]->items);
		int dummy = -1;
		const int sx = std::min(x + 200, std::max(0, ww - 300));
		return run_list(body, sx, y, dummy);
	}

	// ---- the window-system shell (ui::app::run drives these)

	// Making the window and pumping it are one job on this platform (the
	// SDL loop in gui_linux.cpp), so open_main_window has nothing to do
	bool open_main_window(const char *, int, int) override { return true; }
	void pump_window(const char *title, int w, int h) override
	{
		run_window(*this, title, w, h);
	}
	// The devices are objects here (ALSA capture wants the input side wired
	// into the engine before boot); opening the device itself waits for the
	// firmware (start_audio, on the boot thread)
	void make_audio() override
	{
		static audio_out dev_out;
		static audio_in  dev_in;
		out = &dev_out;
		ain = &dev_in;
		if (eng)
			eng->ain = &dev_in;
	}
	void say_audio_opened(bool) override
	{
		std::printf(CLI_T("Audio output: %s\n", "音声の出口: %s\n"), out->device_name().c_str());
	}
	void say_audio_running() override
	{
		std::printf(CLI_T("Playing (latency %.1f ms)\n", "鳴らしている（待ち時間 %.1f ms）\n"),
		            1000.0 * out->buffer_frames() / AUDIO_RATE);
	}
	// ALSA underruns are this platform's drop counter (ui/status.h);
	// output_ms/late()/format_line() are WASAPI-only
	u64 audio_drops() override { return out->starved(); }
	void print_audio_details() override
	{
		std::printf(CLI_T("Exclusive use: %s\n", "独り占め: %s\n"),
		            out->exclusive() ? CLI_T("taken", "取れた") : CLI_T("not taken", "取れなかった"));
	}

	// The status middle: what ALSA measures (ui/status.h keeps the shape)
	void format_middle(char *dst, std::size_t n) override
	{
		std::snprintf(dst, n, UI_TEXT(status_middle_linux_fmt, "starved %llu"), (unsigned long long)out->starved());
	}

	// An editor window comes up, or says why it could not
	void open_pc_window(pc_window &w) override
	{
		std::string err;
		if (!w.show(err)) {
			char m[512];
			std::snprintf(m, sizeof(m), UI_TEXT(dlg_cannot_fmt, "Cannot open: %s"), err.c_str());
			sdl_popup::alert(win, "S-MU2000", m);
		}
	}

	// The pump's quit flag; the dialogs must be able to abandon their wait
	std::atomic<bool> quit{ false };

	// --seconds N: a timed run for smoke tests and demos (0 runs until the
	// window closes). Parsed by gui_linux.cpp's main, applied by the pump
	double seconds_limit = 0.0;

private:
	static void append_group_items(std::vector<sdl_popup::item> &dst,
	                               const std::vector<menu_item> &src)
	{
		for (const menu_item &it : src) {
			sdl_popup::item o;
			o.label = it.label;
			o.id = it.id;
			o.checked = it.checked;
			o.enabled = it.enabled;
			o.separator = it.separator;
			dst.push_back(o);
		}
	}

	// Modal popup over the live panel: the implementation is in window_sdl.cpp,
	// which hands sdl_popup a callback that paints the live frame again so the
	// menu has the panel behind it. sub_chosen returns the submenu marker the
	// user hovered
	int run_list(const std::vector<sdl_popup::item> &items, int x, int y,
	             int &sub_chosen);
};

extern linux_app *g_linux;

// A MIDI file dropped on **any** window -- the panel's or an editor's -- is
// played
void play_dropped_file(const std::string &path);

} // namespace ui

#endif // S_MU2000_UI_APP_LINUX_H
