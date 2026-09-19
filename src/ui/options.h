// license:BSD-3-Clause
//
// Engine flags shared by every tool's command line (gui, live, render, ...).
//
// Each main() delegates these first, so a new engine flag needs no per-tool
// edits: add the flag here and every tool accepts and applies it the same
// way. Tool-specific flags stay in each main. Needs mu2000.h (every tool
// already includes it).

#ifndef S_MU2000_UI_OPTIONS_H
#define S_MU2000_UI_OPTIONS_H

#pragma once

#include <cstring>

#include "mu2000.h"

namespace ui {

// Engine flags shared by all tools: fast MIDI serial pacing, the
// lightweight C++ effects (0 off, 1 added alongside the MEG, 2 replacing
// it), the native ports (0 off, 1 on), and the voice-snapshot files (0 off,
// nonzero on). The effects flags apply before loading; the native ports and
// snapshots only after boot, at a point each tool picks itself, so only
// their parsing is shared.
struct engine_options {
	bool fast_midi = false;
	int  native_fx = 0;
	int  native_engine = 0;
	int  voicecache = 0;
};

// Takes a single argv entry. True when it was a shared engine flag.
inline bool consume_engine_option(const char *arg, engine_options &o)
{
	if (!std::strcmp(arg, "--fast-midi")) { o.fast_midi = true; return true; }
	if (!std::strcmp(arg, "--native-fx")) { o.native_fx = 1; return true; }
	if (!std::strcmp(arg, "--native-fx-full")) { o.native_fx = 2; return true; }
	if (!std::strcmp(arg, "--native-engine")) { o.native_engine = 1; return true; }
	if (!std::strcmp(arg, "--voicecache")) { o.voicecache = 1; return true; }
	if (!std::strcmp(arg, "--no-voicecache")) { o.voicecache = 0; return true; }
	return false;
}

// Applies the flags, as every tool does before loading.
inline void apply_engine_options(mu2000 &mu, const engine_options &o)
{
	mu.set_fast_midi(o.fast_midi);
	if (o.native_fx)
		mu.set_native_fx(o.native_fx);
}

// Output and startup flags shared by the tools that open real-time audio
// (gui, gui_mac, live; render has none of these). Only parsing is shared;
// applying stays per tool.
struct output_options {
	bool        exclusive = false;
	const char *audio_dev = nullptr;  // substring match, null = remembered/default
	bool        factory = false;      // drop stored settings, boot clean
};

// Takes argv[i], advancing i past a taken value. True when consumed.
inline bool consume_output_option(char **argv, int argc, int &i, output_options &o)
{
	const char *arg = argv[i];
	if (!std::strcmp(arg, "--exclusive")) { o.exclusive = true; return true; }
	if (!std::strcmp(arg, "--factory")) { o.factory = true; return true; }
	if (!std::strcmp(arg, "--audio") && i + 1 < argc) { o.audio_dev = argv[++i]; return true; }
	return false;
}

// Startup flags shared by the two graphical front ends (gui, gui_mac).
// live/render have no windows, so these stay out of engine_options.
struct window_options {
	bool open_editor = false;
	bool open_list = false;
	bool open_fx = false;
	bool open_shapes = false;
	bool open_master = false;
	bool lcd_only = false;   // the LCD on its own
};

// Takes a single argv entry. True when it was a shared window flag.
inline bool consume_window_option(const char *arg, window_options &o)
{
	if (!std::strcmp(arg, "--editor")) { o.open_editor = true; return true; }
	if (!std::strcmp(arg, "--list-window")) { o.open_list = true; return true; }
	if (!std::strcmp(arg, "--fx-window")) { o.open_fx = true; return true; }
	if (!std::strcmp(arg, "--shapes-window")) { o.open_shapes = true; return true; }
	if (!std::strcmp(arg, "--master-window")) { o.open_master = true; return true; }
	if (!std::strcmp(arg, "--lcd")) { o.lcd_only = true; return true; }
	return false;
}

} // namespace ui

#endif // S_MU2000_UI_OPTIONS_H
