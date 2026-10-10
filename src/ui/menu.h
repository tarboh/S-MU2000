// license:BSD-3-Clause
//
// The standalone windows' popup menus, shared by the Windows (gui.cpp) and
// macOS (gui_mac.cpp) front ends.
//
// Windows is the reference: the items, their order and their wording live
// here, so a menu added on one side cannot be missed on the other (the
// lightweight-mode toggle was). Each front end only renders menu_groups
// into its own toolkit (HMENU on Windows, NSMenu through window_mac.mm on
// macOS) and acts on the chosen id, whose numbers are shared too.
//
// Plain C++ only: gui.cpp is windows.h-heavy and window_mac.h must stay
// AppKit/gdi-free, so this header includes only <string> and <vector>.
// Header-only (inline) so no build system changes are needed anywhere.
//
// Alongside the menus this is home to the small shared pump vocabulary:
// mouse_out and the F-key codes every platform's event loop translates into.

#ifndef S_MU2000_UI_MENU_H
#define S_MU2000_UI_MENU_H

#pragma once
#include "midi_routes.h"

#include <cstdio>
#include <string>
#include <vector>

#include "ui/texts.h"

namespace ui {

// One line in a popup menu
struct menu_item {
	std::string label;
	int         id = 0;
	bool        checked = false;
	bool        enabled = true;
	bool        separator = false;
	// A shortcut hint shown after the label: after a tab on Windows, in
	// parentheses on macOS. Only the overview/editor items use it.
	std::string shortcut;
};

// A titled group of items. A group whose title is empty is drawn at the top
// level rather than as a submenu, which is what the card slot's second half
// and the editor/filter rows want
struct menu_group {
	std::string            title;
	std::vector<menu_item> items;
};

// What one mouse press on the main window did. The pump decides what still
// to do: press a panel control down (panel_pressed, feeds drag/up and the
// repaint), open a PC window (bar_window, a kind id, -1 when none) or show
// the popup (show_menu; the items come from menu_groups_for()). Lives here
// because every platform's pump reads the same struct: the Mac's bool
// mouse_down once left an opened window looking like a popup request
struct mouse_out {
	bool panel_pressed = false;
	bool opened_window = false;
	bool show_menu = false;
};

// Panel keys that are not characters: the F-keys, in a shared code space
// above ASCII that every platform translates into (virtual keys on Windows,
// SDL keycodes on Linux, Carbon key codes on macOS). button_for_char()
// ignores anything outside ASCII, so these cannot alias a panel button --
// which is what lets the one shared handler act on them
enum : int {
	KEY_F2 = 0x100,   // the PC editor window
	KEY_F3 = 0x101,   // the overview window
	KEY_F4 = 0x102,   // the native-engine toggle
	KEY_F5 = 0x103,   // layout reload
};

// Menu command numbers for picking a port. MIDI IN has four ports, laid out
// 500 apart (A 900, B 1400, C 1900, D 2400).
//
// Each port menu occupies ID_BASE .. ID_BASE+255, and the front ends take
// anything in such a range for that port. **Ranges must not overlap**: A/D
// INPUT and the card items once sat inside ID_OUTMU_BASE's 256, so picking
// an input device (or a card item) went to MIDI OUT instead and the tick
// never moved to what was picked
enum : int {
	ID_IN_NONE = 900, ID_IN_BASE = 901, ID_IN_STRIDE = 500,
	ID_OUT_NONE = 3000, ID_OUT_BASE = 3001,
	ID_OUTB_NONE = 3500, ID_OUTB_BASE = 3501,
	ID_OUTMU_NONE = 4000, ID_OUTMU_BASE = 4001,
	ID_AIN_NONE = 4500, ID_AIN_BASE = 4501,
	ID_CARD_NEW16 = 5000, ID_CARD_NEW32, ID_CARD_NEW64, ID_CARD_NEW128,
	ID_CARD_OPEN = 5010, ID_CARD_EJECT = 5011,
	ID_PLAY_FILE = 5100, ID_STOP_FILE = 5101, ID_PORTS34_FOLD = 5102, ID_PORTS34_DROP = 5103,
	ID_THIN_BENDS = 5104,    // the player lightens heavy MIDI (issue #82)
	ID_FACTORY = 5200,
	ID_SETTINGS = 5220,
	ID_RESTART = 5201,       // power the MU off and on
	ID_NATIVE_FX = 5215,     // lightweight mode (C++ effects)
	ID_NATIVE_ENGINE = 5216, // firmware を走らせない口（聞き比べ用）
	ID_PC_EDITOR = 5203,
	ID_OVERVIEW = 5202,
	ID_OUTPUT_DIGITAL = 5300, ID_OUTPUT_ANALOG = 5301, ID_OUTPUT_LIMITER = 5302,
	ID_RESET_GM = 5320, ID_RESET_GS, ID_RESET_XG, ID_MIDI_PANIC,
	ID_RATE_AUTO = 6300, ID_RATE_BASE = 6301,
	ID_AUDIO_DEFAULT = 5500, ID_AUDIO_BASE = 5501,
	ID_INE_NONE = 6000, ID_INE_BASE = 6001,     // MIDI IN E (the plug-in board's port)
};

// Checked at compile time, because the failure is silent: a menu id that lands
// in a port menu's range (ID_BASE .. ID_BASE+255) is taken for that port, so
// the chosen item never arrives and the tick never moves
static_assert([] {
	const int bases[] = { ID_IN_BASE, ID_IN_BASE + ID_IN_STRIDE, ID_IN_BASE + 2 * ID_IN_STRIDE,
	                      ID_IN_BASE + 3 * ID_IN_STRIDE,
	                      ID_OUT_BASE, ID_OUTB_BASE, ID_OUTMU_BASE, ID_AIN_BASE, ID_AUDIO_BASE, ID_INE_BASE, ID_RATE_BASE };
	const int singles[] = { ID_IN_NONE, ID_IN_NONE + ID_IN_STRIDE, ID_IN_NONE + 2 * ID_IN_STRIDE,
	                        ID_IN_NONE + 3 * ID_IN_STRIDE,
	                        ID_OUT_NONE, ID_OUTB_NONE, ID_OUTMU_NONE,
	                        ID_AIN_NONE, ID_CARD_NEW16, ID_CARD_NEW32, ID_CARD_NEW64,
	                        ID_CARD_NEW128, ID_CARD_OPEN, ID_CARD_EJECT,
	                        ID_PLAY_FILE, ID_STOP_FILE, ID_FACTORY, ID_RESTART, ID_NATIVE_FX,
	                        ID_NATIVE_ENGINE, ID_SETTINGS,
	                        ID_PORTS34_FOLD, ID_PORTS34_DROP, ID_THIN_BENDS, ID_PC_EDITOR, ID_OVERVIEW,
	                        ID_OUTPUT_DIGITAL, ID_OUTPUT_ANALOG, ID_OUTPUT_LIMITER, ID_RESET_GM, ID_RESET_GS, ID_RESET_XG, ID_MIDI_PANIC, ID_RATE_AUTO, ID_AUDIO_DEFAULT, ID_INE_NONE };
	for (size_t i = 0; i < std::size(singles); i++)
		for (size_t j = 0; j < i; j++)
			if (singles[i] == singles[j]) return false;
	for (int base : bases) {
		for (int id : singles)
			if (id >= base && id < base + 256)
				return false;
		for (int other : bases)
			if (other != base && other >= base && other < base + 256)
				return false;
	}
	return true;
}(), "menu ids overlap or fall inside another menu's ID_BASE..ID_BASE+255 range");

// The names shown in the menu and in the startup report, A B C D.
// (The gui.ini keys stay per front end with the settings code.)
// Localized: the table in ui/texts.h (menu_in_a and friends).
inline const char *in_label(int p)
{
	switch (p) {
	case 0: return UI_TEXT(menu_in_a, "MIDI IN A (parts 1-16)");
	case 1: return UI_TEXT(menu_in_b, "MIDI IN B (parts 17-32)");
	case 2: return UI_TEXT(menu_in_c, "MIDI IN C (parts 33-48)");
	case 3: return UI_TEXT(menu_in_d, "MIDI IN D (parts 49-64)");
	default: return UI_TEXT(menu_in_e, "MIDI IN E (multi-part plug-in board)");
	}
}

// Everything a menu shows. MIDI IN is four ports in A-D order; device
// choices are indices into the lists (-1 is unused). The recording device
// is matched by name (empty is unused), the card by path (empty is none).
struct menu_state {
	std::vector<std::string> midi_ins;
	std::vector<std::string> midi_outs;
	std::vector<std::string> audio_ins;
	std::vector<std::string> audio_outs;
	std::vector<int> audio_rates;
	int audio_rate = 0;
	bool limiter = false;
	std::string audio_name;  // empty selects the system default
	bool audio_ready = false; // startup has released the output to the UI
	midi_routing midi;
	std::string ain_name;
	std::string card_path;
	bool playing = false;
	std::string play_name;
	bool fold34 = true;      // ports 3+4 of a MIDI file fold onto A and B
	bool thin_bends = false; // the player lightens heavy MIDI (bends, Roland display data)
	bool ready = false;      // the firmware is up (factory reset is offered)
	bool native_fx = false;  // lightweight C++ effects are on
	bool native_engine = false;  // skip-firmware ports, toggled live for listening tests
	bool analog = false;     // the PHONES output (digital otherwise)
};

// The plug-in card/panel menus (VST3/AU/CLAP share the VST3 view on both
// platforms). Same sharing as the standalone menus above: the numbers match
// what the Windows plug-in already used, so its choice dispatch is unchanged.
enum : int {
	ID_PLUG_CARD_NEW16 = 100, ID_PLUG_CARD_NEW32, ID_PLUG_CARD_NEW64, ID_PLUG_CARD_NEW128,
	ID_PLUG_CARD_OPEN = 110, ID_PLUG_CARD_EJECT = 111,
	ID_PLUG_LIST = 120, ID_PLUG_EDITOR = 121,
};

struct plug_menu_state {
	std::string card_path;   // the SmartMedia image in the slot (empty is none)
	bool card_ready = false; // the engine is up enough to swap images
};

namespace menu_detail {

inline menu_item text(const char *label, int id, bool checked, bool enabled,
                      const char *shortcut = "")
{
	menu_item m;
	m.label = label;
	m.id = id;
	m.checked = checked;
	m.enabled = enabled;
	m.shortcut = shortcut;
	return m;
}

inline menu_item separator()
{
	menu_item m;
	m.separator = true;
	return m;
}

inline menu_group settings_shortcut(bool separated = true)
{
	menu_group g;
	if (separated) g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(settings_title, "Settings..."), ID_SETTINGS, false, true));
	return g;
}

// The file name in the slot, so it is clear which one is going out.
// Both / and \ count, because the two platforms write them differently
inline std::string basename(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace menu_detail

// One input/output picker for the quick MIDI menu.
inline menu_group menu_port_group(const char *title, const std::vector<std::string> &names,
                                  const std::vector<midi_route> &routes, int column, int id_none, int id_base, bool ready)
{
	using namespace menu_detail;
	menu_group g;
	g.title = title;
	const bool selected = std::any_of(routes.begin(), routes.end(), [&](const auto &r) { return (r.ports & (1u << column)) != 0; });
	g.items.push_back(text(UI_TEXT(menu_unused, "Unused"), id_none, !selected, ready));
	g.items.push_back(separator());
	if (names.empty()) g.items.push_back(text(UI_TEXT(menu_no_devices, "(No devices)"), 0, false, false));
	for (size_t i = 0; i < names.size() && i < 256; i++)
		g.items.push_back(text(names[i].c_str(), id_base + int(i), (midi_route_mask(routes, names[i]) & (1u << column)) != 0, ready));
	return g;
}

// The recording device the machine samples as its A/D INPUT, matched by name
inline menu_group menu_ain_group(const std::vector<std::string> &names, const std::string &ain_name)
{
	using namespace menu_detail;
	menu_group g;
	g.title = UI_TEXT(menu_ain_title, "A/D INPUT (sound to sample)");
	g.items.push_back(text(UI_TEXT(menu_unused, "Unused"), ID_AIN_NONE, ain_name.empty(), true));
	g.items.push_back(separator());
	if (names.empty()) {
		g.items.push_back(text(UI_TEXT(menu_no_ain, "(No recording devices)"), 0, false, false));
		return g;
	}
	for (size_t i = 0; i < names.size(); i++)
		g.items.push_back(text(names[i].c_str(), ID_AIN_BASE + int(i), names[i] == ain_name, true));
	return g;
}

// Enumerated afresh when opening a menu. The caller keeps this exact snapshot
// for dispatch: hotplugging must not turn an old index into another device.
inline menu_group menu_audio_output(const menu_state &s)
{
	using namespace menu_detail;
	menu_group g;
	g.title = UI_TEXT(menu_audio_title, "Audio output device");
	g.items.push_back(text(UI_TEXT(menu_audio_default, "System default"),
	                       ID_AUDIO_DEFAULT, s.audio_name.empty(), s.audio_ready));
	g.items.push_back(separator());
	if (s.audio_outs.empty())
		g.items.push_back(text(UI_TEXT(menu_no_audio, "(No playback devices)"), 0, false, false));
	for (size_t i = 0; i < s.audio_outs.size() && i < 256; i++)
		g.items.push_back(text(s.audio_outs[i].c_str(), ID_AUDIO_BASE + int(i),
		                       s.audio_outs[i] == s.audio_name, s.audio_ready));
	return g;
}

// The general right-click menu opens the editors and settings.
inline std::vector<menu_group> menu_ports(const menu_state &s)
{
	using namespace menu_detail;
	menu_group editors;
	editors.items.push_back(text(UI_TEXT(menu_open_list, "Open the list"), ID_OVERVIEW, false, true, "F3"));
	editors.items.push_back(text(UI_TEXT(menu_open_editor, "Open the editor"), ID_PC_EDITOR, false, true, "F2"));
	return { editors, settings_shortcut() };
}

// MIDI IN A is the quick menu for all MIDI ports and mode resets.
inline std::vector<menu_group> menu_midi(const menu_state &s)
{
	using namespace menu_detail;
	std::vector<menu_group> groups;
	for (int p = 0; p < 5; p++)
		groups.push_back(menu_port_group(in_label(p), s.midi_ins, s.midi.inputs, p,
		    p == 4 ? ID_INE_NONE : ID_IN_NONE + p * ID_IN_STRIDE, p == 4 ? ID_INE_BASE : ID_IN_BASE + p * ID_IN_STRIDE, s.ready));
	groups.push_back(menu_port_group(UI_TEXT(menu_out_mu, "MIDI OUT (what the MU2000 sends)"), s.midi_outs, s.midi.outputs, 2, ID_OUTMU_NONE, ID_OUTMU_BASE, s.ready));
	groups.push_back(menu_port_group(UI_TEXT(menu_thru_a, "MIDI THRU A (sends out what A receives)"), s.midi_outs, s.midi.outputs, 0, ID_OUT_NONE, ID_OUT_BASE, s.ready));
	groups.push_back(menu_port_group(UI_TEXT(menu_thru_b, "MIDI THRU B (sends out what B receives)"), s.midi_outs, s.midi.outputs, 1, ID_OUTB_NONE, ID_OUTB_BASE, s.ready));
	menu_group reset;
	reset.title = UI_TEXT(settings_reset, "Reset MIDI mode");
	reset.items = {text("GM", ID_RESET_GM, false, s.ready), text("GS / TG300B", ID_RESET_GS, false, s.ready),
	    text("XG", ID_RESET_XG, false, s.ready), text(UI_TEXT(settings_panic, "Panic"), ID_MIDI_PANIC, false, s.ready)};
	groups.push_back(reset);
	groups.push_back(settings_shortcut());
	return groups;
}

// The card slot: a fresh SmartMedia image, then the image in the slot and
// the MIDI file player
inline std::vector<menu_group> menu_card(const menu_state &s)
{
	using namespace menu_detail;
	std::vector<menu_group> groups;

	menu_group fresh;
	fresh.title = UI_TEXT(menu_card_new, "Make a new SmartMedia image");
	fresh.items.push_back(text("16MB", ID_CARD_NEW16, false, true));
	fresh.items.push_back(text("32MB", ID_CARD_NEW32, false, true));
	fresh.items.push_back(text("64MB", ID_CARD_NEW64, false, true));
	fresh.items.push_back(text("128MB", ID_CARD_NEW128, false, true));
	groups.push_back(fresh);

	menu_group g;
	g.items.push_back(text(UI_TEXT(menu_card_open, "Insert a SmartMedia image..."), ID_CARD_OPEN, false, true));
	char eject[512];
	if (!s.card_path.empty())
		std::snprintf(eject, sizeof(eject), UI_TEXT(menu_card_eject_fmt, "Eject the SmartMedia (%s)"),
		              basename(s.card_path).c_str());
	else
		std::snprintf(eject, sizeof(eject), "%s", UI_TEXT(menu_card_eject, "Eject the SmartMedia"));
	g.items.push_back(text(eject, ID_CARD_EJECT, false, !s.card_path.empty()));
	g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(menu_play_file, "Play a MIDI file..."), ID_PLAY_FILE, false, true));
	char stop[512];
	if (s.playing)
		std::snprintf(stop, sizeof(stop), UI_TEXT(menu_stop_fmt, "Stop (%s)"), s.play_name.c_str());
	else
		std::snprintf(stop, sizeof(stop), "%s", UI_TEXT(menu_stop, "Stop"));
	g.items.push_back(text(stop, ID_STOP_FILE, false, s.playing));
	g.items.push_back(text(UI_TEXT(menu_thin_bends, "Lighten heavy MIDI: thin pitch bends, drop Roland display data (unlike the real unit)"), ID_THIN_BENDS, s.thin_bends, true));
	// What to do with a MIDI file that uses ports 3 and 4
	g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(menu_fold34, "Fold ports 3+4 onto A and B (DIN ports only)"), ID_PORTS34_FOLD, s.fold34, true));
	g.items.push_back(text(UI_TEXT(menu_drop34, "Drop ports 3+4 (DIN ports only)"), ID_PORTS34_DROP, !s.fold34, true));
	groups.push_back(g);
	groups.push_back(settings_shortcut());
	return groups;
}

// The PHONES jack: digital (as S/PDIF, DPCM DC included) or analogue
// (DC removed, src/analog_out.h)
inline std::vector<menu_group> menu_phones(const menu_state &s)
{
	using namespace menu_detail;
	menu_group g;
	g.items.push_back(text(UI_TEXT(menu_out_digital, "Digital (S/PDIF; keeps DPCM DC)"),
	                       ID_OUTPUT_DIGITAL, !s.analog, true));
	g.items.push_back(text(UI_TEXT(menu_out_analog, "Analog (LINE OUT/PHONES; cuts DC)"),
	                       ID_OUTPUT_ANALOG, s.analog, true));
	menu_group rates;
	rates.title = UI_TEXT(settings_rate, "Stream sample rate");
	rates.items.push_back(text(UI_TEXT(settings_auto, "Automatic"), ID_RATE_AUTO, !s.audio_rate, s.audio_ready));
	for (size_t i = 0; i < s.audio_rates.size(); i++) {
		const std::string label = std::to_string(s.audio_rates[i]) + " Hz";
		rates.items.push_back(text(label.c_str(), ID_RATE_BASE + int(i), s.audio_rate == s.audio_rates[i], s.audio_ready));
	}
	g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(settings_limiter, "Limit output peaks"), ID_OUTPUT_LIMITER, s.limiter, s.audio_ready));
	return { menu_audio_output(s), rates, g, settings_shortcut() };
}

// The POWER switch: restart the machine
inline std::vector<menu_group> menu_power(const menu_state &s)
{
	using namespace menu_detail;
	menu_group g;
	g.items.push_back(text(UI_TEXT(menu_restart, "Restart the MU (power off and on)"), ID_RESTART, false, s.ready));
	g.items.push_back(text(UI_TEXT(menu_factory, "Factory reset..."), ID_FACTORY, false, s.ready));
	return { g, settings_shortcut() };
}

// The A/D INPUT jack on its own: the recording-device picker under its heading
inline std::vector<menu_group> menu_ain_only(const std::vector<std::string> &names,
                                             const std::string &ain_name)
{
	using namespace menu_detail;
	menu_group head = menu_ain_group(names, ain_name);
	menu_group g;
	g.items.push_back(text(head.title.c_str(), 0, false, false));
	g.items.push_back(separator());
	for (const menu_item &item : head.items)
		g.items.push_back(item);
	return { g, settings_shortcut() };
}

// The plug-in card slot: a fresh SmartMedia image, the image in the slot,
// then the PC editor windows
inline std::vector<menu_group> menu_plug_card(const plug_menu_state &s)
{
	using namespace menu_detail;
	std::vector<menu_group> groups;

	menu_group fresh;
	fresh.title = UI_TEXT(menu_card_new, "Make a new SmartMedia image");
	fresh.items.push_back(text("16MB", ID_PLUG_CARD_NEW16, false, s.card_ready));
	fresh.items.push_back(text("32MB", ID_PLUG_CARD_NEW32, false, s.card_ready));
	fresh.items.push_back(text("64MB", ID_PLUG_CARD_NEW64, false, s.card_ready));
	fresh.items.push_back(text("128MB", ID_PLUG_CARD_NEW128, false, s.card_ready));
	groups.push_back(fresh);

	menu_group g;
	g.items.push_back(text(UI_TEXT(menu_card_open, "Insert a SmartMedia image..."), ID_PLUG_CARD_OPEN, false, s.card_ready));
	char eject[512];
	if (!s.card_path.empty())
		std::snprintf(eject, sizeof(eject), UI_TEXT(menu_card_eject_fmt, "Eject the SmartMedia (%s)"),
		              basename(s.card_path).c_str());
	else
		std::snprintf(eject, sizeof(eject), "%s", UI_TEXT(menu_card_eject, "Eject the SmartMedia"));
	g.items.push_back(text(eject, ID_PLUG_CARD_EJECT, false, !s.card_path.empty()));
	g.items.push_back(separator());
	g.items.push_back(text(UI_TEXT(menu_open_list, "Open the list"), ID_PLUG_LIST, false, true));
	g.items.push_back(text(UI_TEXT(menu_open_editor, "Open the editor"), ID_PLUG_EDITOR, false, true));
	groups.push_back(g);
	return groups;
}

// A plug-in right click that missed the card slot: the PC editor windows
inline std::vector<menu_group> menu_plug_panel()
{
	using namespace menu_detail;
	menu_group g;
	g.items.push_back(text(UI_TEXT(menu_open_list, "Open the list"), ID_PLUG_LIST, false, true));
	g.items.push_back(text(UI_TEXT(menu_open_editor, "Open the editor"), ID_PLUG_EDITOR, false, true));
	return { g };
}

} // namespace ui

#endif // S_MU2000_UI_MENU_H
