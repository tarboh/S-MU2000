// license:BSD-3-Clause
//
// The Linux front end: main() and nothing else. The shared program (bridge,
// panel, player, remembered ports, menus, PC windows, bring-up and shutdown)
// is ui::app, answered by ui::linux_app (ui/app_linux.h); the SDL3 window
// and pump live in ui/window_sdl.* like the Win32 and AppKit sides live in
// ui/window_win.* and ui/window_mac.*. The Windows and macOS twins of this
// file are gui.cpp and gui_mac.cpp, and the three mains are the same shape.
//
//   gui <rom directory> [--midi N] [--midi-b N] ...   (ui/tool_args.h)
//   gui [<rom directory> --boot] --shot image.png [--size 1000x400]
//   gui --list | gui --dump-layout panel.txt
//
// --seconds stays Linux-only here, taken out of argv before the shared
// parser: timed runs under SDL_VIDEODRIVER=dummy.

#include "compat/console.h"
#include "mu2000.h"

#include "ui/app_linux.h"
#include "ui/bridge.h"
#include "ui/engine.h"
#include "ui/lang.h"
#include "ui/midi_in.h"
#include "ui/midi_out.h"
#include "ui/rom_locate.h"
#include "ui/tool_args.h"
#include "ui/window_sdl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv)
{
	smu2000::init_console_utf8();

	// --seconds N stays a Linux-only flag (timed runs under
	// SDL_VIDEODRIVER=dummy): taken out of argv before the shared parser
	double seconds = 0.0;
	int kept = 1;
	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) {
			seconds = std::atof(argv[++i]);
			continue;
		}
		argv[kept++] = argv[i];
	}

	ui::tool_args a;
	a.latency = 30;        // 溜める目標 (per-backend default; the shared parser keeps it)
	ui::output_options out_opts;
	ui::window_options win_opts;
	ui::engine_options eng_opts;

	// The flags are shared (ui/tool_args.h); only latency above stays per side
	const int parsed = ui::parse_tool_args(kept, argv, a, eng_opts, out_opts, win_opts);
	// The language resolves here, from the parsed --lang (then editor.ini,
	// then the locale), before any texts() use below
	ui::init_lang(a.lang.c_str());
	if (parsed >= 0)
		return parsed;

	static ui::bridge br;
	static ui::midi_in  midi_ports[mu2000::MIDI_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;
	static ui::linux_app gui(br, midi_ports, mout, mout_b, mout_mu);
	ui::g_linux = &gui;
	gui.seconds_limit = seconds;

	// Picture only. An empty screen can be drawn even without any ROMs.
	if (!a.shot_path.empty() && (a.dir.empty() || !a.boot_for_shot))
		return ui::empty_shot(br, a, win_opts);

	// ROM の場所を渡されなければ（ダブルクリックなど）、プラグインと同じ順番で探し、無ければ選んでもらう
	// （ui/rom_locate.h、issue #89）
	if (a.dir.empty())
		a.dir = ui::locate_roms_for_gui(smu2000::exe_dir());
	if (a.dir.empty()) {
		ui::print_usage();
		return 1;
	}

	static ui::engine eng(br, midi_ports[0]);
	gui.wire_engine(eng, eng_opts);
	gui.eng = &eng;
	gui.state = &eng.state;
	if (!gui.load_machine(eng, a))
		return 1;
	const int shot = gui.run_boot_shot(eng, br, a, win_opts);
	if (shot >= 0)
		return shot;

	// A MIDI file dropped on any window plays (the panel, the editor, the overview)
	ui::pc_window::set_drop_handler(ui::play_dropped_file);

	// The window, the boot thread, the pump and the shutdown are shared
	// (ui::app::run); the shell in ui/window_sdl.cpp fills in the SDL3 side
	return gui.run(a, eng_opts, out_opts, win_opts);
}
