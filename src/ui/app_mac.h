// license:BSD-3-Clause
//
// The Mac front end's app class. gui_mac.cpp keeps main(); window_mac.mm
// drives ui::app's event verbs directly (mouse_down, key, context_menu, ...),
// and this class fills in the per-platform hooks the shared program asks
// for -- dialogs, audio say-lines, window opening. The same shape as
// ui/app_win.h and ui/app_linux.h: one window-system file, one app class,
// one main.

#ifndef S_MU2000_UI_APP_MAC_H
#define S_MU2000_UI_APP_MAC_H

#pragma once

#include <cstdio>
#include <dispatch/dispatch.h>
#include <string>

#include "app.h"
#include "window_mac.h"

namespace ui {

// gui.ini lives in ~/Library/Application Support/S-MU2000/
std::string settings_file_path();

class gui_app : public app
{
public:
	// mi is MIDI IN A-D, mu2000::MIDI_PORTS of them
	explicit gui_app(bridge &b) : app(b) {}

	// Work the frame asked for, answered once the turn that drew has finished.
	// The base class drains its queue at the top of paint_main(), which is between
	// the panel's NewFrame() and Render(): a nested modal there ([alert runModal]
	// pumps the run loop) has the timer paint again inside it, and the next
	// NewFrame() then finds the previous frame unended and asserts "Forgot to call
	// Render() or EndFrame()". Windows posts a message for the same reason, and the
	// main queue is its equivalent here.
	void defer_outside_paint(std::function<void()> f) override
	{
		auto *work = new std::function<void()>(std::move(f));
		dispatch_async_f(dispatch_get_main_queue(), work, [](void *p) {
			std::function<void()> *fn = static_cast<std::function<void()> *>(p);
			auto call = std::move(*fn);
			delete fn;
			call();
		});
	}

	// ---- ui::app hooks: file dialogs, confirmations and error display are
	// AppKit's business, everything they decide is shared
	std::string settings_path() const override { return settings_file_path(); }
	void menu_error(const std::string &text) override
	{
		alert_modal("S-MU2000", text.c_str());
	}
	void menu_note(const std::string &text) override
	{
		alert_modal("S-MU2000", text.c_str());
	}
	std::string ask_card_open_path() override
	{
		return open_file_panel(UI_TEXT(dlg_card_open, "Insert a SmartMedia image"), "img");
	}
	std::string ask_card_save_path() override
	{
		return save_file_panel(UI_TEXT(dlg_card_save, "Where to save the new SmartMedia image"), "smartmedia.img", "img");
	}
	std::string ask_midi_file_path() override
	{
		return open_midi_file_panel();
	}
	bool confirm_factory_reset() override
	{
		return confirm_modal("S-MU2000",
		                     UI_TEXT(dlg_factory_text, "Reset the MU2000 to factory state and restart it.\n"
		                                             "Utility settings and remembered volume/voice settings will all be erased."),
		                     UI_TEXT(dlg_factory_ok, "Reset"));
	}

	// An editor window comes up, or says why it could not
	void open_pc_window(pc_window &w) override
	{
		std::string err;
		if (!w.show(err)) {
			char m[512];
			std::snprintf(m, sizeof(m), UI_TEXT(dlg_cannot_fmt, "Cannot open: %s"), err.c_str());
			alert_modal("S-MU2000", m);
		}
	}

	// starved() counts what Windows calls late(); output_ms is
	// WASAPI-only, so only the drop count crosses over (ui/status.h)
	void format_middle(char *dst, std::size_t n) override
	{
		std::snprintf(dst, n, UI_TEXT(status_middle_mac_fmt, "late %llu"), (unsigned long long)out->starved());
	}

	void print_audio_details() override
	{
		std::printf(CLI_T("Exclusive use: %s\n", "独り占め: %s\n"),
		            out->exclusive() ? CLI_T("taken", "取れた") : CLI_T("not taken", "取れなかった"));
	}

	// ---- the window-system shell (ui::app::run drives these)

	// Making the window and pumping it are one call on this platform
	// (run_window), so open_main_window has nothing to do
	bool open_main_window(const char *, int, int) override { return true; }
	void pump_window(const char *title, int w, int h) override
	{
		run_window(*this, title, w, h);
	}
	// The device is an object here, opened once the firmware is up
	// (start_audio, on the boot thread)
	void make_audio() override
	{
		// Both devices, like app_ios.h and the other front ends: without the
		// input object choose_ain() returned on its first line and the A/D INPUT
		// menu did nothing at all on macOS. Static for the same reason dev_out
		// is - the render block and the input tap hold their impl raw.
		static audio_out dev_out;
		static audio_in dev_in;
		out = &dev_out;
		ain = &dev_in;
	}
	void say_audio_opened(bool exclusive) override
	{
		std::printf(CLI_T("Audio output: %s\n", "音声の出口: %s\n"), out->device_name().c_str());
		// Hog mode is a request, not a guarantee: something else may hold it
		if (exclusive)
			std::printf(CLI_T("Exclusive use: %s\n", "独り占め: %s\n"),
			            out->exclusive() ? CLI_T("taken", "取れた") : CLI_T("not taken", "取れなかった"));
	}
	void say_audio_running() override
	{
		std::printf(CLI_T("Playing (latency %.1f ms, %s)\n", "鳴らしている（待ち時間 %.1f ms、%s）\n"),
		            1000.0 * out->buffer_frames() / AUDIO_RATE,
		            out->mmcss() ? CLI_T("CoreAudio real-time thread", "CoreAudio の実時間スレッド")
		                         : CLI_T("no real-time thread (may drop out)", "実時間スレッドを取れていない（途切れやすい）"));
	}
	// starved() counts what Windows calls late()
	u64 audio_drops() override { return out->starved(); }
};

extern gui_app *g_gui;                  // set once main has made the app

// A MIDI file dropped on **any** window -- the panel's, or one of the
// editor windows' -- is played
void play_dropped_file(const std::string &path);

} // namespace ui

#endif // S_MU2000_UI_APP_MAC_H
