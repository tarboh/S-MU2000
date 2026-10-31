// license:BSD-3-Clause
//
// The Windows front end's app: ui::app answering the Win32 window (ui/
// window_win.cpp). gui.cpp keeps main(); the Mac's twin is ui/app_mac.h.

#ifndef S_MU2000_UI_APP_WIN_H
#define S_MU2000_UI_APP_WIN_H

#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

#include <windows.h>

#include "app.h"
#include "menu.h"
#include "menu_win.h"
#include "pc_window.h"
#include "text.h"
#include "window_win.h"

namespace ui {

// gui.ini lives under %LOCALAPPDATA%
std::string settings_file_path();

class win_app : public app
{
public:
	explicit win_app(bridge &b) : app(b) {}

	HWND hwnd = nullptr;             // set at WM_CREATE, for message boxes
	// 描画の外で行う仕事（app::defer_outside_paint）。WM_APP_DEFERRED を受けた wnd_proc が実行する
	static constexpr UINT WM_APP_DEFERRED = WM_APP + 0x31;
	std::vector<std::function<void()>> deferred_win;
	void defer_outside_paint(std::function<void()> f) override
	{
		deferred_win.push_back(std::move(f));
		if (hwnd)
			PostMessageW(hwnd, WM_APP_DEFERRED, 0, 0);
	}
	void run_deferred_win()
	{
		std::vector<std::function<void()>> todo;
		todo.swap(deferred_win);
		for (auto &f : todo)
			f();
	}
	// Choosing from a menu failed: shown at the end of the command
	std::string last_error;

	void open_window_by_kind(int kind) { app::open_window_by_kind(kind); }

	// Opens an existing PC window, warning when it cannot be done
	void open_pc_window(pc_window &w) override
	{
		win_open_window(hwnd, w);
	}

	// The wait/drop fragment is what WASAPI measures (ui/status.h);
	// output_ms and late() are Windows-only (CoreAudio has no wait number)
	void format_middle(char *dst, std::size_t n) override
	{
		std::snprintf(dst, n, UI_TEXT(status_middle_win_fmt, "wait %.0f ms  late %llu"),
		              out->output_ms(), (unsigned long long)out->late());
	}

	void print_audio_details() override
	{
		std::printf("%s\n%s\n", out->format_line().c_str(), out->latency_line().c_str());
	}

	// ui::app hooks: file dialogs, confirmations and error display are
	// Win32's business (ui/window_win.h), everything they decide is shared
	std::string settings_path() const override { return settings_file_path(); }
	void menu_error(const std::string &text) override { last_error = text; }
	void menu_note(const std::string &text) override
	{
		win_note(hwnd, text);
	}
	std::string ask_card_open_path() override
	{
		const std::wstring title = to_wide(UI_TEXT(dlg_card_open, "Insert a SmartMedia image"));
		const std::wstring filter = dlg_filter(UI_TEXT(dlg_smartmedia_desc, "SmartMedia image"), "*.img",
		                                       UI_TEXT(dlg_all_files, "All files"), "*.*");
		return win_open_file(hwnd, title.c_str(), filter.c_str(), L"img");
	}
	std::string ask_card_folder_path() override
	{
		return win_pick_folder(hwnd, to_wide(UI_TEXT(dlg_card_folder, "Folder to use as the SmartMedia")).c_str());
	}
	std::string ask_card_save_path() override
	{
		const std::wstring title = to_wide(UI_TEXT(dlg_card_save, "Where to save the new SmartMedia image"));
		const std::wstring filter = dlg_filter(UI_TEXT(dlg_smartmedia_desc, "SmartMedia image"), "*.img",
		                                       UI_TEXT(dlg_all_files, "All files"), "*.*");
		return win_save_file(hwnd, title.c_str(), filter.c_str(),
		                         L"img", L"smartmedia.img");
	}
	std::string ask_midi_file_path() override
	{
		const std::wstring title = to_wide(UI_TEXT(dlg_midi_open, "MIDI file to play"));
		const std::wstring filter = dlg_filter(UI_TEXT(dlg_midi_desc, "MIDI files"), "*.mid;*.midi;*.rcp;*.r36",
		                                       UI_TEXT(dlg_all_files, "All files"), "*.*");
		return win_open_file(hwnd, title.c_str(), filter.c_str(),
		                         nullptr);
	}
	bool confirm_factory_reset() override
	{
		return win_confirm(hwnd, UI_TEXT(dlg_factory_text, "Reset the MU2000 to factory state and restart it.\n"
		                                                 "Utility settings and remembered volume/voice settings will all be erased."));
	}

	// ---- the window-system shell (ui::app::run drives these)

	bool open_main_window(const char *title, int w, int h) override
	{
		return make_window(title, w, h);
	}
	void pump_window(const char *, int, int) override
	{
		MSG msg;
		while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}
	// The devices are objects here (WASAPI needs the input side wired into
	// the engine before boot); opening the device itself waits for the
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
		std::printf(CLI_T("Audio output: %s\n%s\n", "音声の出口: %s\n%s\n"), out->device_name().c_str(),
		            out->format_line().c_str());
	}
	void say_audio_running() override
	{
		std::printf(CLI_T("Playing (latency %.1f ms, MMCSS %s)\n", "鳴らしている（待ち時間 %.1f ms、MMCSS %s）\n"),
		            1000.0 * out->buffer_frames() / AUDIO_RATE,
		            out->mmcss() ? CLI_T("registered", "登録できた") : CLI_T("not registered (may drop out)", "登録できない（途切れやすい）"));
	}
	u64 audio_drops() override { return out->late(); }
};

extern win_app *g_win;

void play_dropped_file(const std::string &path);

} // namespace ui

#endif // S_MU2000_UI_APP_WIN_H
