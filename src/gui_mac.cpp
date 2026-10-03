// license:BSD-3-Clause
//
// Run the MU2000 behind a front panel that looks like the real machine (macOS).
//
//   gui <rom directory> [--midi n] [--midi-b n] [--midi-c n] [--midi-d n]
//       [--midiout n] [--midiout-b n] [--midiout-mu n]
//       [--latency ms] [--exclusive] [--audio <name>] [--factory] [--host-midi] [--fast-midi]
//   gui --list                             list the MIDI ports and audio devices
//   gui <rom directory> --shot image.png   write the picture without a window
//
// The Windows version of this is gui.cpp, and this is the same program: the
// same panel, the same engine, the same arguments, the same main. What the
// platform forces lives elsewhere, so a feature added on one side cannot be
// missed on the other:
//
//   * the program (state, paint, menus, bring-up, run) is ui::app (ui/app.h)
//   * the AppKit half of the app class is ui::gui_app (ui/app_mac.h), and
//     the window system is window_mac.mm
//   * drawing goes through Metal + Dear ImGui (ui/imgui_shell.h), the same
//     code that paints the Windows/Linux windows
//   * settings live in ~/Library/Application Support/S-MU2000/gui.ini
//
// Audio is produced the same way as in live: **it keeps no clock of its own**
// (doc/design.md).
//
// The mouse wheel drives the dial. The real machine has a rotary encoder in
// that spot too, and it does the same job as the VALUE -/+ buttons.

#include "compat/console.h"
#include "mu2000.h"

#include "ui/rom_locate.h"
#include "ui/app.h"
#include "ui/app_mac.h"
#include "ui/lang.h"

int main(int argc, char **argv)
{
	smu2000::init_console_utf8();

	ui::tool_args a;
	a.latency = 30;        // per-backend default; the shared parser keeps it
	ui::window_options win_opts;
	ui::engine_options eng_opts;
	ui::output_options out_opts;

	// The flags are shared (ui/tool_args.h); only latency above stays per side
	const int parsed = ui::parse_tool_args(argc, argv, a, eng_opts, out_opts, win_opts);
	// The language resolves here, from the parsed --lang (then editor.ini,
	// then the locale), before any texts() use below
	ui::init_lang(a.lang.c_str());
	if (parsed >= 0)
		return parsed;

	static ui::bridge br;
	static ui::midi_in  midi_ports[mu2000::MIDI_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;
	static ui::gui_app gui(br, midi_ports, mout, mout_b, mout_mu);
	ui::g_gui = &gui;

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

	// A MIDI file dropped on any window plays (the panel, the editor, the
	// overview). The window itself is made and pumped by ui::app::run
	ui::pc_window::set_drop_handler(ui::play_dropped_file);

	return gui.run(a, eng_opts, out_opts, win_opts);
}
