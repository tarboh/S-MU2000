// license:BSD-3-Clause
//
// The iOS app class. Same shape as ui/app_mac.h and ui/app_win.h: one window
// file (ui/window_ios.mm), one app class, one main (src/ios/smoke.mm).
//
// Sixteen pure virtuals, and this file is where the iOS answers come from.
// Most of them are stubbed with a plain reason rather than implemented, because
// step 1 of the port is pixels only: no audio device, no file dialogs. Each stub
// says what it would do when that step arrives, so the list reads as the
// remaining work rather than as unfinished business.

#ifndef S_MU2000_UI_APP_IOS_H
#define S_MU2000_UI_APP_IOS_H

#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "app.h"
#include "window_ios.h"   // open_midi_file_panel(), the way app_mac.h takes window_mac.h

namespace ui {

// The iOS sandbox, which is where a Mac-style ini would go. Kept beside the
// other settings paths so nothing has to learn a second convention.
std::string settings_file_path();

class gui_app : public app
{
public:
	explicit gui_app(bridge &b) : app(b) {}

	// ---- settings

	std::string settings_path() const override { return settings_file_path(); }

	// ---- audio
	//
	// make_audio() is where the device is made, the way app_mac.cpp does it, and
	// AVAudioEngine is the device (src/ui/audio_apple.mm). app_mac.cpp is not
	// reusable: iOS has no AudioHardware HAL. mach_absolute_time and
	// os/workgroup.h do both exist on iOS (verified against the iPhoneSimulator
	// SDK).

	void make_audio() override
	{
		// Static, like app_mac.cpp: the render block captures the impl raw, so
		// the object must outlive everything, and a static trivially does. This
		// only creates the objects and points out/ain at them; opening waits for
		// booted firmware (start_audio, called from app.mm).
		static audio_out dev_out;
		static audio_in dev_in;
		out = &dev_out;
		ain = &dev_in;
		std::fprintf(stderr, "[ios] audio objects made (not yet opened)\n");
	}
	void say_audio_opened(bool) override
	{
		std::fprintf(stderr, "[ios] audio opened: %s\n",
		             out ? out->device_name().c_str() : "(no device)");
	}
	void say_audio_running() override
	{
		std::fprintf(stderr, "[ios] audio running: %u-frame buffer%s\n",
		             out ? out->buffer_frames() : 0,
		             out && out->mmcss() ? ", real-time thread" : "");
	}
	u64 audio_drops() override { return 0; }
	void print_audio_details() override {}
	bool open_main_window(const char *, int, int) override { return true; }

	// The panel window is a UIView in window_ios.mm, and UIKit drives it from
	// CADisplayLink, so this hands control back to the run loop rather than
	// pumping in a loop like the mac and Windows shells do. Returning here is
	// correct: CADisplayLink then calls back on its own.
	void pump_window(const char *, int, int) override {}

	// ---- the status line's middle fragment
	//
	// Shared code calls this only when out && out->produced(), so with no audio
	// device it never runs. The count is still reported rather than left blank,
	// because a zero reads as "measured, nothing dropped" and is misleading.
	void format_middle(char *dst, std::size_t n) override
	{
		std::snprintf(dst, n, UI_TEXT(status_middle_mac_fmt, "late %llu"),
		              (unsigned long long)(out ? out->starved() : 0));
	}

	// Advances the machine by one display frame's worth of samples
	// (AUDIO_RATE/30 at 30 Hz tick = real time), discarding the audio.
	//
	// Without this the machine freezes one step past boot: boot() only runs until
	// mu.midi_ready(), and on desktop the audio callback keeps the machine alive
	// forever after. The LCD then shows whatever was last drawn (起動中...) because
	// the main-screen redraw happens in firmware ticks that never execute.
	//
	// Through engine::fill(), not mu.run_sample() directly: fill() is what pumps
	// the bridge (drv.publish) at the end, and the panel only ever reads the
	// bridge. run_sample on its own advances the SH2 but publishes nothing, so
	// the panel keeps showing the boot-time snapshot. fill() also drains MIDI and
	// applies panel buttons, so touch will already have somewhere to go.
	//
	// Only once booted (state == 1): fill() on any other state zeroes the buffer
	// and returns, so this guard is documentation rather than load-bearing - but
	// explicit is better when the next reader is deciding where audio goes.
	//
	// All on the main thread here; the audio callback pumps on the engine's
	// real-time thread once it runs. Both go through the same eng->fill under the
	// same card_lock, so they are mutually excluded - this method simply stops
	// being called once produced() goes non-zero (above), and resumes if audio
	// never starts. No second path to maintain: one call, two drivers.
	void pump_realtime()
	{
		if (!eng || !state || state->load() != 1)
			return;
		// Stand down once the audio device produces: its callback pumps through
		// the same eng->fill under the same card_lock, so overlap would only
		// double-advance the machine (a pitch blip, self-correcting) rather than
		// corrupt it - but there is no reason to pay for it. produced() stays
		// non-zero once audio has run, so this is a one-way handoff; if the
		// device ever stops, the machine freezes until audio returns, which is
		// device management's problem (future work) rather than this one's.
		if (out && out->produced() > 0)
			return;
		// One frame of stereo s16, repurposed every tick. Static so it never
		// reallocates; the audio backend will own a buffer like this when it lands.
		static std::vector<s16> scratch(size_t(AUDIO_RATE) / 30 * 2);
		eng->fill(scratch.data(), AUDIO_RATE / 30);
	}

	// ---- dialogs: UIAlertController and UIDocumentPickerViewController
	//
	// Not yet. Each returns empty, which the shared code reads as "cancelled",
	// so a menu item behaves rather than misbehaving until the port reaches it.

	void menu_error(const std::string &) override {}
	void menu_note(const std::string &) override {}
	std::string ask_card_open_path() override { return std::string(); }
	std::string ask_card_save_path() override { return std::string(); }
	// By name, the way app_mac.h asks window_mac.h for its open panel: the
	// window system owns the dialog. The empty return is honest rather than
	// lazy - a document picker answers later, so the path cannot come back from
	// the call that starts it, and the shared caller reads empty as "cancelled"
	// while the window layer plays the file itself when the answer arrives.
	std::string ask_midi_file_path() override
	{
		open_midi_file_panel();
		return std::string();
	}
	bool confirm_factory_reset() override { return false; }

	// An editor window comes up, or says why it could not. The five PC editors
	// each want their own window; iOS gives them one UIViewController presented
	// over the panel, so this opens the window and reports the failure in the log
	// (there is no alert yet - menu_error is the stub above).
	void open_pc_window(pc_window &w) override
	{
		std::string err;
		if (!w.show(err))
			std::fprintf(stderr, "Cannot open: %s\n", err.c_str());
	}
};

extern gui_app *g_gui;                  // set once main has made the app

} // namespace ui

#endif // S_MU2000_UI_APP_IOS_H