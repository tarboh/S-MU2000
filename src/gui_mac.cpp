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
// same panel, the same engine, the same arguments. The differences are the
// ones the platform forces.
//
//   * the window is AppKit (src/ui/window_mac.mm) rather than Win32, which is
//     a separate file because the Cocoa headers and compat/gdi.h cannot both
//     be visible at once
//   * the port picker is an NSMenu and choosing a MIDI file is an NSOpenPanel,
//     so both are asked for through ui::mac_app instead of built here
//   * drawing goes into the view's CGContext through the GDI shim, so panel.cpp
//     is literally the same code that paints the Windows window
//   * settings live in ~/Library/Application Support/S-MU2000/gui.ini
//
// Audio is produced the same way as in live: **it keeps no clock of its own**
// (doc/design.md).
//
// The mouse wheel drives the dial. The real machine has a rotary encoder in
// that spot too, and it does the same job as the VALUE -/+ buttons.

#include "compat/console.h"
#include "compat/gdi.h"
#include "compat/paths.h"
#include "mu2000.h"
#include "nvram.h"
#include "smartmedia.h"
#include "smf.h"
#include "voicecache.h"
#include "ui/audio_out.h"
#include "ui/bridge.h"
#include "ui/engine.h"
#include "ui/fx_editor.h"
#include "ui/layout.h"
#include "ui/midi_in.h"
#include "ui/midi_out.h"
#include "ui/overview.h"
#include "ui/panel.h"
#include "ui/master_editor.h"
#include "ui/part_shapes.h"
#include "ui/pc_editor.h"
#include "ui/pc_host.h"
#include "ui/pc_window_mac.h"
#include "ui/player.h"
#include "ui/png.h"
#include "ui/keymap.h"
#include "ui/options.h"
#include "ui/settings.h"
#include "ui/window_mac.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Menu lines and builders are shared with gui.cpp in ui/menu.h; the names
// below are unqualified for the choice dispatch (menu_chosen).
using namespace ui;

constexpr u32 RATE = ui::AUDIO_RATE;

// Menu command numbers, labels and builders are shared with gui.cpp
// in ui/menu.h (Windows is the reference), so a menu added on one
// side cannot be missed on the other. Only rendering (window_mac.mm)
// and acting on the choice (menu_chosen below) stay here.

// ---- Remember the chosen ports
//
// They are remembered by **name**, not by number. Replugging a USB device
// shifts the numbers, so a remembered number would connect to a different
// device the next time the window is opened.

std::string settings_path()
{
	const std::string dir = smu2000::ensure_config_dir();
	return dir.empty() ? std::string() : dir + "gui.ini";
}

// gui.ini keys live in ui/settings.h as ui::SET_* (shared with gui.cpp).
// Menu wording lives in ui/menu.h as ui::IN_LABELS.

struct port_names {
	std::string in[mu2000::MIDI_PORTS];     // MIDI IN A-D
	std::string out, out_b, out_mu;
	std::string audio;          // the audio device, by name
	std::string audio_in;       // the recording device feeding A/D INPUT, by name
	std::string card;           // the SmartMedia image in the slot, by path
	float       volume = 1.0f;  // the panel's VOLUME knob
	// Ports 3 and 4 of a MIDI file: true folds them onto A and B, false drops
	// them. Same key as gui.cpp's ("ports34=fold" / "ports34=drop")
	bool        fold34 = true;
	// The output: false = digital (as S/PDIF), true = analogue (DC removed, src/analog_out.h).
	// Same key as gui.cpp's ("output=digital" / "output=analog")
	bool        analog = false;
};

port_names load_settings()
{
	port_names n;
	const std::string path = settings_path();
	if (path.empty())
		return n;
	settings_map kv;
	if (!read_settings_file(path, kv))
		return n;
	for (int p = 0; p < mu2000::MIDI_PORTS; p++)
		if (const std::string *v = find_setting(kv, SET_IN_KEYS[p]))
			n.in[p] = *v;
	if (const std::string *v = find_setting(kv, SET_OUT))    n.out   = *v;
	if (const std::string *v = find_setting(kv, SET_OUT_B))  n.out_b = *v;
	if (const std::string *v = find_setting(kv, SET_OUT_MU)) n.out_mu = *v;
	if (const std::string *v = find_setting(kv, SET_AUDIO_OUT))   n.audio = *v;
	if (const std::string *v = find_setting(kv, SET_AUDIO_IN))    n.audio_in = *v;
	if (const std::string *v = find_setting(kv, SET_CARD))  n.card  = *v;
	if (const std::string *v = find_setting(kv, SET_PORTS34))     n.fold34 = *v != "drop";
	if (const std::string *v = find_setting(kv, SET_OUTPUT))      n.analog = *v == "analog";
	if (const std::string *v = find_setting(kv, SET_VOLUME)) {
		if (!v->empty())
			n.volume = std::clamp(float(std::atof(v->c_str())), 0.0f, 1.0f);
	}
	return n;
}


// ---- Keyboard. The table is shared (ui/keymap.h, same as gui.cpp).
// Letters arrive in lower case. macOS hands over the character with Shift
// already stripped, so both '=' and '+' have to be listed.
bool key_to_button(int code, mu2000::button &out)
{
	return ui::button_for_char(code, out);
}

// ---- Things handed to the window

// ---- The screen. Paints the panel, feeds it input, builds the menus

class app : public ui::mac_app
{
public:
	// mi is MIDI IN A-D, mu2000::MIDI_PORTS of them
	app(ui::bridge &b, ui::midi_in *mi,
	    ui::midi_out &mo, ui::midi_out &mob, ui::midi_out &mmu)
	    : br(b), midi(mi), mout(mo), mout_b(mob), mout_mu(mmu) {}

	ui::panel  panel;
	ui::player play;

	// The PC editor windows. Same contents as on Windows; only the window is
	// AppKit + Metal (ui/pc_window_mac.mm). F2 / F3 or right-click opens them
	ui::pc_window pc{ std::make_unique<ui::pc_editor>() };    // PC editor (F2 or right-click)
	ui::pc_window list{ std::make_unique<ui::overview>() };   // overview (F3 or right-click)
	ui::pc_window fx{ std::make_unique<ui::fx_editor>() };    // insertion settings (double-click in the overview)
	ui::pc_window shapes{ std::make_unique<ui::part_shapes>() };  // part voice (double-click a VIB/FILTER/EG/EQ cell in the overview)
	ui::pc_window master{ std::make_unique<ui::master_editor>() }; // master (double-click the MASTER row in the overview)

	std::string layout_path;

	// ---- mac_app

	void draw(void *cg, int w, int h) override
	{
		// The window's timer is where this has to happen: it touches the bridge,
		// so it must not run on the audio thread (same as gui.cpp's WM_TIMER)
		panel.tick(br);
		// the CPU load for the PC windows (the overview's top strip)
		if (out && out->produced())
			br.set_cpu(float(out->cpu_percent()));
		// the PC editor windows, where the Windows side has its WM_TIMER
		ui::pc_frame_all(list, pc, fx, shapes, master, panel.xg(), panel.ram(), br,
		                 [&](ui::pc_window &w) { open_editor_window(w); });
		card_tick();
		report_drops();

		ui::snapshot s;
		br.read(s);
		const u64 pressed = br.buttons();

		char status[320] = {};
		if (out && out->produced())
			std::snprintf(status, sizeof(status),
			              "発音 %d/128  CPU %.0f%%  最悪 %.1f ms  枯渇 %llu   IN: %s   OUT: %s"
			              "   （MIDI IN A のジャックか右クリックで口を選ぶ）",
			              s.voices_master + s.voices_slave,
			              out->cpu_percent(), out->worst_ms(),
			              (unsigned long long)out->starved(),
			              in_name[0].empty() ? "なし" : in_name[0].c_str(),
			              out_name.empty() ? "なし" : out_name.c_str());
		else
			std::snprintf(status, sizeof(status), "起動中...");

		panel.set_volume(br.gain());

		// The view's context is already top-left, y down, so it can be handed
		// to the shim as it stands
		HDC dc = static_cast<HDC>(smu_gdi_wrap_view_context(cg, w, h));
		panel.paint(dc, s, pressed, status);
		DeleteDC(dc);
	}

	void resized(int w, int h) override
	{
		panel.resize(w, h);
	}

	bool mouse_down(int x, int y, bool right) override
	{
		if (lcd_only)
			return false;
		// A secondary click opens the port picker wherever it lands; on the
		// card slot it opens the file menu instead. Same as gui.cpp does on
		// WM_RBUTTONUP
		if (right)
			return true;

		// The jacks and the card slot are pressed rather than clicked: they
		// open a menu instead of moving a panel control (the A/D INPUT jack
		// offers just its recording devices, as a left click does in gui.cpp)
		if (panel.on_midi_jack(x, y) || panel.on_ad_input(x, y) ||
		    panel.on_card_slot(x, y) || panel.on_phones(x, y))
			return true;

		m_pressed = true;
		panel.press(x, y, br);
		return false;
	}

	void mouse_drag(int x, int y) override
	{
		if (lcd_only)
			return;
		if (m_pressed)
			panel.drag(x, y, br);
	}

	void mouse_up() override
	{
		if (!m_pressed)
			return;
		m_pressed = false;
		panel.release(br);
	}

	void wheel(int x, int y, int steps) override
	{
		if (lcd_only)
			return;
		if (steps)
			panel.wheel_at(x, y, steps, br);
	}

	void key(int code, bool down) override
	{
		if (lcd_only && down)
			return;
		if (code == ui::MAC_KEY_FUNCTION_BASE + 0x60) {      // F5
			if (down)
				reload_layout();
			return;
		}
		if (down && code == ui::MAC_KEY_FUNCTION_BASE + 0x78) {   // F2
			open_editor_window(pc);
			return;
		}
		if (down && code == ui::MAC_KEY_FUNCTION_BASE + 0x63) {   // F3
			open_editor_window(list);
			return;
		}
		if (down && eng && code == ui::MAC_KEY_FUNCTION_BASE + 0x76) {   // F4
			eng->want_native_engine.store(eng->native_engine.load() ? 0 : 1);
			return;
		}
		mu2000::button b = mu2000::button::count;
		if (key_to_button(code, b))
			br.press(b, down);
	}

	void focus_lost() override
	{
		m_pressed = false;
		br.release_all();
	}

	bool hand_cursor(int x, int y) override
	{
		if (lcd_only)
			return false;
		return panel.on_midi_jack(x, y) || panel.on_ad_input(x, y) ||
		       panel.on_card_slot(x, y) || panel.on_phones(x, y);
	}

	// What the shared builders (ui/menu.h) show, from this window's state
	menu_state menu_snapshot()
	{
		static_assert(mu2000::MIDI_PORTS == 4, "shared menu IDs lay out 4 MIDI IN ports");
		menu_state s;
		s.midi_ins = ui::midi_in::list();
		s.midi_outs = ui::midi_out::list();
		s.audio_ins = ui::audio_in::list();
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			s.in_dev[p] = in_dev[p];
		s.out_dev = out_dev;
		s.out_dev_b = out_dev_b;
		s.out_dev_mu = out_dev_mu;
		s.ain_name = ain_name;
		s.card_path = card_path;
		s.playing = play.playing();
		s.play_name = play.name();
		s.fold34 = play.fold_extra_ports();
		s.ready = ready();
		s.native_fx = eng && eng->native_fx.load();
		s.native_engine = eng && eng->native_engine.load();
		return s;
	}

	std::vector<ui::menu_group> context_menu(int x, int y) override
	{
		if (lcd_only)
			return {};
		if (panel.on_card_slot(x, y))
			return ui::menu_card(menu_snapshot());
		// The PHONES jack is about the output, as in gui.cpp
		if (panel.on_phones(x, y))
			return ui::menu_phones(eng && eng->analog.load());
		// The A/D INPUT jack offers just its recording devices, as in gui.cpp
		if (panel.on_ad_input(x, y))
			return ui::menu_ain_only(ui::audio_in::list(), ain_name);
		return ui::menu_ports(menu_snapshot());
	}

	// Whether the firmware has finished booting. The engine lives in main(), so
	// it is its state that is pointed at here rather than copied
	bool ready() const { return state && state->load() == 1; }

	void menu_chosen(int id) override
	{
		for (int p = 0; p < mu2000::MIDI_PORTS; p++) {
			const int none = ID_IN_NONE + p * ID_IN_STRIDE, base = ID_IN_BASE + p * ID_IN_STRIDE;
			if (id == none)                        { choose_in(p, -1); return; }
			if (id >= base && id < base + 256)     { choose_in(p, id - base); return; }
		}
		if (id == ID_OUT_NONE)                                        choose_out(-1);
		else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 256)         choose_out(id - ID_OUT_BASE);
		else if (id == ID_OUTMU_NONE)                                 choose_out_mu(-1);
		else if (id >= ID_OUTMU_BASE && id < ID_OUTMU_BASE + 256)     choose_out_mu(id - ID_OUTMU_BASE);
		else if (id == ID_OUTB_NONE)                                  choose_out_b(-1);
		else if (id >= ID_OUTB_BASE && id < ID_OUTB_BASE + 256)       choose_out_b(id - ID_OUTB_BASE);
		else if (id == ID_AIN_NONE)                                   choose_ain(-1);
		else if (id >= ID_AIN_BASE && id < ID_AIN_BASE + 256)         choose_ain(id - ID_AIN_BASE);
		else if (id == ID_CARD_OPEN)                                  open_card();
		else if (id == ID_CARD_EJECT)                                 eject_card();
		else if (id >= ID_CARD_NEW16 && id <= ID_CARD_NEW128)         new_card(16u << (id - ID_CARD_NEW16));
		else if (id == ID_PORTS34_FOLD)                               set_fold34(true);
		else if (id == ID_PORTS34_DROP)                               set_fold34(false);
		else if (id == ID_PC_EDITOR)                                  open_editor_window(pc);
		else if (id == ID_OVERVIEW)                                   open_editor_window(list);
		else if (id == ID_NATIVE_FX && eng)
			eng->want_native_fx.store(eng->native_fx.load() ? 0 : 2);
		else if (id == ID_NATIVE_ENGINE && eng)
			eng->want_native_engine.store(eng->native_engine.load() ? 0 : 1);
		else if (id == ID_FACTORY)                                    factory_reset();
		else if ((id == ID_OUTPUT_DIGITAL || id == ID_OUTPUT_ANALOG) && eng) {
			eng->analog.store(id == ID_OUTPUT_ANALOG);
			std::printf("音の出口: %s\n", id == ID_OUTPUT_ANALOG ? "アナログ（直流を切る）" : "デジタル");
			std::fflush(stdout);
			save_settings();
		}
		else if (id == ID_PLAY_FILE) {
			const std::string path = ui::open_midi_file_panel();
			if (!path.empty())
				play_song(path);
		}
		else if (id == ID_STOP_FILE) play.stop();
	}

	void reload_layout() override
	{
		apply_layout(layout_path, false);
	}

	// ---- the rest

	// An editor window comes up, or says why it could not. Windows' open_window
	// with its MessageBoxW, in AppKit clothing
	void open_editor_window(ui::pc_window &w)
	{
		std::string err;
		if (!w.show(err))
			ui::alert_modal("S-MU2000", ("開けない: " + err).c_str());
	}

	void set_layout(const std::string &path)
	{
		layout_path = path;
		panel.lay() = ui::layout();
		std::string err;
		if (!path.empty() && !panel.lay().load(path, err))
			std::printf("配置: %s を開けない。組み込みの配置を使う\n", path.c_str());
		if (!err.empty())
			std::fprintf(stderr, "%s", err.c_str());
		panel.resize(panel.width(), panel.height());
	}

	void apply_layout(const std::string &path, bool quiet)
	{
		panel.lay() = ui::layout();
		std::string err;
		if (!path.empty() && panel.lay().load(path, err)) {
			if (!quiet)
				std::printf("配置: %s\n", path.c_str());
		} else if (!path.empty() && !quiet) {
			std::printf("配置: %s を開けない。組み込みの配置を使う\n", path.c_str());
		}
		if (!err.empty())
			std::fprintf(stderr, "%s", err.c_str());
		std::fflush(stdout);
		panel.resize(panel.width(), panel.height());
	}

	void play_song(const std::string &path)
	{
		std::string err;
		if (!play.start(path, br, err)) {
			std::fprintf(stderr, "開けない: %s\n", err.c_str());
			return;
		}
		std::printf("再生: %s（%.1f 秒）\n", path.c_str(), play.length());
		// The machine has two ports, so a file that uses four is either folded
		// onto them or has its extra parts dropped. Say which, as gui.cpp does
		if (play.ports_used() > 2)
			std::printf("  この曲は %d 口ぶん。C・D は未対応なので、口 3 以降は%s\n",
			            play.ports_used(),
			            play.fold_extra_ports() ? " A・B に重ねて鳴らす" : "鳴らさない");
		std::fflush(stdout);
	}

	// A file dropped on the window is played, which is what gui.cpp's
	// WM_DROPFILES handler does with one. The window only hands the path over:
	// what a drop means is the app's business
	void file_dropped(const std::string &path) override
	{
		play_song(path);
	}

	// A MIDI loop (THRU fed back into an IN) overflows the guards. gui.cpp says
	// so once a second rather than once a block; the same here, from the window's
	// timer rather than from the paint
	void report_drops()
	{
		if (!eng)
			return;
		const u64 now = smu2000::perf_ticks() * 1000 / smu2000::perf_freq();
		if (now - last_drop_report < 1000)
			return;
		last_drop_report = now;
		const u64 drops = eng->guard_a.dropped() + eng->guard_b.dropped() +
		                  eng->mu.midi_dropped();
		if (drops == reported_drops)
			return;
		reported_drops = drops;
		std::fprintf(stderr,
		             "MIDI が多すぎるので捨てた: THRU A %llu / THRU B %llu / 受信 %llu バイト"
		             "（MIDI の輪ができていないか確かめる）\n",
		             (unsigned long long)eng->guard_a.dropped(),
		             (unsigned long long)eng->guard_b.dropped(),
		             (unsigned long long)eng->mu.midi_dropped());
	}

	// Open what the menu picked. On failure it falls back to "unused".
	// keep is true only while starting up: the name that was asked for is then
	// kept even if the port is not there yet (see save_settings())
	// port is 0-3 for MIDI IN A-D
	void choose_in(int port, int dev, bool keep = false)
	{
		if (port < 0 || port >= mu2000::MIDI_PORTS)
			return;
		if (!keep)
			in_keep[port].clear();
		std::string err;
		if (!midi[port].open(dev, err)) {
			std::fprintf(stderr, "%s: %s\n", IN_LABELS[port], err.c_str());
			midi[port].open(-1, err);
			dev = -1;
		}
		in_dev[port]  = midi[port].is_open() ? dev : -1;
		in_name[port] = midi[port].device_name();
		save_settings();
	}

	void choose_out(int dev, bool keep = false)
	{
		if (!keep)
			out_keep.clear();
		std::string err;
		if (!mout.open(dev, err)) {
			std::fprintf(stderr, "MIDI 出力: %s\n", err.c_str());
			mout.open(-1, err);
			dev = -1;
		}
		out_dev  = mout.is_open() ? dev : -1;
		out_name = mout.device_name();
		save_settings();
	}

	void choose_out_b(int dev, bool keep = false)
	{
		if (!keep)
			out_keep_b.clear();
		std::string err;
		if (!mout_b.open(dev, err)) {
			std::fprintf(stderr, "MIDI 出力 B: %s\n", err.c_str());
			mout_b.open(-1, err);
			dev = -1;
		}
		out_dev_b  = mout_b.is_open() ? dev : -1;
		out_name_b = mout_b.device_name();
		save_settings();
	}

	// The machine's own MIDI OUT: what the firmware sends out by itself (a
	// dump reply, the sequencer). Pointed at a virtual port it is how an
	// external editor reads and writes the settings
	void choose_out_mu(int dev, bool keep = false)
	{
		if (!keep)
			out_keep_mu.clear();
		std::string err;
		if (!mout_mu.open(dev, err)) {
			std::fprintf(stderr, "MIDI 出力（本体の OUT）: %s\n", err.c_str());
			mout_mu.open(-1, err);
			dev = -1;
		}
		out_dev_mu  = mout_mu.is_open() ? dev : -1;
		out_name_mu = mout_mu.device_name();
		save_settings();
	}

	// ---- A/D INPUT (the recording device the machine samples)
	//
	// Same as gui.cpp's choose_ain: "no device" stops the capture and leaves
	// nothing feeding the machine, which then samples silence
	void choose_ain(int dev, bool keep = false)
	{
		if (!keep)
			ain_keep.clear();
		if (!ain)
			return;
		ain->stop();
		if (dev < 0) {
			ain_name.clear();
			ain_dev = -1;
			std::printf("A/D INPUT: なし\n");
			std::fflush(stdout);
			save_settings();
			return;
		}
		const auto names = ui::audio_in::list();
		if (size_t(dev) >= names.size()) {
			std::fprintf(stderr, "A/D INPUT: %d 番のデバイスが無い\n", dev);
			return;
		}
		std::string err;
		if (!ain->start(names[size_t(dev)], err)) {
			std::fprintf(stderr, "A/D INPUT: %s\n", err.c_str());
			ain_name.clear();
			ain_dev = -1;
			return;
		}
		std::printf("A/D INPUT: %s（%s）\n", ain->device_name().c_str(), ain->format_line().c_str());
		std::fflush(stdout);
		// The chosen name is kept even if it was the default that opened: the
		// menu shows which entry is ticked, and the entry is a name
		ain_name = names[size_t(dev)];
		ain_dev  = dev;
		save_settings();
	}

	// ---- SmartMedia (the card slot)
	//
	// The image is a file, and what the machine writes has to go back into it.
	// engine::card_lock is held while the machine itself is touched, because the
	// audio thread is running the machine from the other side (see ui/engine.h)
	void flush_card()
	{
		if (!eng || card_path.empty())
			return;
		std::vector<smu2000::smartmedia::block> blocks;
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card().take_dirty_blocks(blocks);
		}
		if (blocks.empty())
			return;
		std::string err;
		if (!smu2000::smartmedia::write_blocks(card_path, blocks, err))
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
	}

	void eject_card()
	{
		if (!eng)
			return;
		flush_card();
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card().eject();
		}
		if (!card_path.empty())
			std::printf("SmartMedia を抜いた: %s\n", card_path.c_str());
		card_path.clear();
		save_settings();
	}

	// Load it first, so a file that cannot be read does not take the slot away
	// from the card that is already in it
	bool insert_card(const std::string &path)
	{
		if (!eng)
			return false;
		smu2000::smartmedia card;
		std::string err;
		if (!card.load(path, err)) {
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
			return false;
		}
		eject_card();
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card() = std::move(card);
		}
		card_path = path;
		std::printf("SmartMedia を差した: %s（%uMB）\n", path.c_str(), eng->mu.card().megabytes());
		std::fflush(stdout);
		save_settings();
		return true;
	}

	// An empty card, in the physical layout a new one comes in. It has to be
	// formatted by the machine (UTIL -> CARD -> Format) before it holds anything
	void new_card(u32 megabytes)
	{
		const std::string path = ui::save_file_panel("新しい SmartMedia の保存先", "smartmedia.img", "img");
		if (path.empty())
			return;
		smu2000::smartmedia card;
		if (!card.create(megabytes)) {
			std::fprintf(stderr, "SmartMedia を作れない\n");
			return;
		}
		std::string err;
		if (!card.save(path, err)) {
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
			return;
		}
		// A fresh card only has the physical layout on it, so the machine still
		// has to format it before it holds anything (gui.cpp says this too)
		if (insert_card(path))
			ui::alert_modal("S-MU2000",
			                "空の SmartMedia を差しました。\n"
			                "使う前に、本体の UTIL → CARD → Format で書式化してください。");
	}

	void open_card()
	{
		const std::string path = ui::open_file_panel("差す SmartMedia", "img");
		if (!path.empty())
			insert_card(path);
	}

	// Called from the window's timer. The machine writes to the card while it
	// runs, so the file is brought up to date every couple of seconds: that is
	// what keeps a crash from losing more than the last two seconds. Ejecting,
	// closing the window and saving all flush as well
	void card_tick()
	{
		const u64 now = smu2000::perf_ticks() * 1000 / smu2000::perf_freq();
		if (now - last_flush < 2000)
			return;
		last_flush = now;
		flush_card();
	}

	// Throwing the settings away means booting the machine again, which takes
	// tens of seconds, so it runs on its own thread. The previous one is joined
	// first: two boots at once would both be writing the machine
	void factory_reset()
	{
		if (!ready() || !eng)
			return;
		if (!ui::confirm_modal("S-MU2000",
		                       "MU2000 を工場出荷状態に戻して、電源を入れ直します。\n"
		                       "ユーティリティの設定や、覚えている音量・音色の設定はすべて消えます。",
		                       "戻す"))
			return;
		play.stop();
		join_reboot();
		reboot = std::thread([this] { eng->factory_reset(); });
	}

	void join_reboot()
	{
		if (reboot.joinable())
			reboot.join();
	}

	std::thread reboot;                // the factory-reset boot, while it runs

	// Folding ports 3 and 4 of a MIDI file onto A and B, and remembering it
	void set_fold34(bool on)
	{
		play.set_fold_extra_ports(on);
		save_settings();
	}

	// Remembered by name rather than number (see the note on settings_path).
	// A port that would not open keeps the name it was asked for, so a virtual
	// port that is not up yet is not forgotten by the next start
	void save_settings()
	{
		// --nomidi must not write empty port names over the remembered ones
		if (keep_settings)
			return;
		const std::string path = settings_path();
		if (path.empty())
			return;
		settings_map kv;
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			kv.emplace_back(SET_IN_KEYS[p],
			                in_name[p].empty() ? in_keep[p] : in_name[p]);
		kv.emplace_back(SET_OUT,    out_name.empty()    ? out_keep    : out_name);
		kv.emplace_back(SET_OUT_B,  out_name_b.empty()  ? out_keep_b  : out_name_b);
		kv.emplace_back(SET_OUT_MU, out_name_mu.empty() ? out_keep_mu : out_name_mu);
		kv.emplace_back(SET_AUDIO_OUT,   audio_name);
		// The recording device is kept by name even when it is not open, the
		// same way the MIDI ports are: a device that is not there yet must not
		// be forgotten. An empty name means "not used", which the menu sets
		kv.emplace_back(SET_AUDIO_IN,    ain_name.empty() ? ain_keep : ain_name);
		kv.emplace_back(SET_CARD,     card_path);
		kv.emplace_back(SET_PORTS34,     play.fold_extra_ports() ? "fold" : "drop");
		kv.emplace_back(SET_OUTPUT,      eng && eng->analog.load() ? "analog" : "digital");
		// The panel's VOLUME knob. On the real machine it is the analogue one behind
		// the DAC, so the firmware's RAM does not hold it and it is kept here
		char vol[32];
		std::snprintf(vol, sizeof(vol), "%.3f", br.gain());
		kv.emplace_back(SET_VOLUME, vol);
		write_settings_file(path, kv);
	}

	int in_dev[mu2000::MIDI_PORTS] = { -1, -1, -1, -1 };   // MIDI IN A-D; -1 is unused
	int out_dev = -1, out_dev_b = -1, out_dev_mu = -1;
	int ain_dev = -1;
	// MIDI thrown away by the THRU guards, and when that was last said out loud
	u64  reported_drops = 0;
	u64  last_drop_report = 0;
	bool keep_settings = false;        // --nomidi: leave the remembered ports alone
	std::string in_name[mu2000::MIDI_PORTS];
	std::string out_name, out_name_b, out_name_mu;
	// The name to fall back on when a port could not be opened. Cleared when the
	// menu is used, so a deliberate "unused" is not undone on the next start
	std::string in_keep[mu2000::MIDI_PORTS];
	std::string out_keep, out_keep_b, out_keep_mu, ain_keep;
	std::string audio_name;            // the audio device, by name (empty = default)
	std::string ain_name;              // the recording device, by name (empty = unused)
	std::string card_path;             // the SmartMedia in the slot, by path (empty = none)

	ui::audio_out *out = nullptr;      // set once the audio device is open
	ui::audio_in  *ain = nullptr;      // set once the recording device is picked
	ui::engine    *eng = nullptr;      // set once the ROMs are loaded
	std::atomic<int> *state = nullptr; // the engine's, so menu items can be greyed
	bool lcd_only = false;             // --lcd: the LCD on its own, as in gui.cpp

private:
	ui::bridge   &br;
	ui::midi_in  *midi;                // MIDI IN A-D (mu2000::MIDI_PORTS of them)
	ui::midi_out &mout, &mout_b, &mout_mu;
	bool m_pressed = false;
	u64 last_flush = 0;                // when the card file was last written back
};


// ---- Write just the picture, with no window. Used to check the looks

int shot(const std::string &path, int w, int h, ui::bridge &br, bool grid,
         const std::string &layout_path)
{
	ui::panel p;
	std::string lerr;
	if (!layout_path.empty() && !p.lay().load(layout_path, lerr))
		std::fprintf(stderr, "配置: %s を開けない\n", layout_path.c_str());
	if (!lerr.empty())
		std::fprintf(stderr, "%s", lerr.c_str());
	p.resize(w, h);
	p.set_grid(grid);

	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;                 // top down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void *bits = nullptr;
	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!bmp) {
		std::fprintf(stderr, "画面を作れない\n");
		return 1;
	}
	SelectObject(dc, bmp);

	ui::snapshot s;
	br.read(s);
	p.set_volume(0.8);
	p.paint(dc, s, 0, "");
	GdiFlush();

	const bool ok = ui::write_png(path, static_cast<const u8 *>(bits), w, h, w * 4);

	DeleteObject(bmp);
	DeleteDC(dc);

	std::printf(ok ? "書き出した: %s（%d×%d）\n" : "書き出せない: %s\n",
	            path.c_str(), w, h);
	return ok ? 0 : 1;
}

} // namespace


// A MIDI file dropped on **any** window -- the panel's, or one of the editor
// windows' -- is played. Windows' play_dropped_file (gui.cpp), in UTF-8
app *g_gui = nullptr;                  // set once main has made the app

void play_dropped_file(const std::string &path)
{
	if (g_gui)
		g_gui->play_song(path);
}

int main(int argc, char **argv)
{
	smu2000::init_console_utf8();

	std::string dir, shot_path, dump_layout, play_path;
	std::string layout_path;
	ui::window_options win_opts;     // --editor/--lcd etc., shared (ui/options.h)
	// MIDI IN A-D. -2 unset (use the remembered one) / -1 unused
	int in_dev[mu2000::MIDI_PORTS] = { -2, -2, -2, -2 };
	// Start as the machine does with HOST SELECT = USB, which is what makes ports
	// C and D usable. --host-midi turns it off (the DIN ports A and B only)
	bool usb_host = true;
	ui::engine_options eng_opts;     // --fast-midi/--native-fx*, shared (ui/options.h)
	int mout_dev = -2;
	int moutb_dev = -2;
	int moutmu_dev = -2;               // the machine's own MIDI OUT
	int latency = 30;
	ui::output_options out_opts;
	int win_w = 1400, win_h = 360;
	bool size_given = false;
	bool grid = false;
	bool boot_for_shot = false;
	bool nomidi = false;               // --nomidi: open and remember no MIDI port
	std::string shot_mid;
	double shot_secs = 0.0;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--list")) {
			const auto ins = ui::midi_in::list();
			std::printf("MIDI 入力（--midi 番号 / 画面からも選べる）:\n");
			for (size_t k = 0; k < ins.size(); k++)
				std::printf("  %zu: %s\n", k, ins[k].c_str());
			if (ins.empty())
				std::printf("  （なし）\n");
			const auto outs = ui::midi_out::list();
			std::printf("MIDI 出力（--midiout 番号 / 受けたものをそのまま外へ）:\n");
			for (size_t k = 0; k < outs.size(); k++)
				std::printf("  %zu: %s\n", k, outs[k].c_str());
			if (outs.empty())
				std::printf("  （なし）\n");
			// Same as gui.cpp: the names --audio takes are matched as substrings
			const auto aouts = ui::audio_out::list();
			std::printf("音声の出口（--audio に名前の一部）:\n");
			for (size_t k = 0; k < aouts.size(); k++)
				std::printf("  %zu: %s\n", k, aouts[k].c_str());
			const auto ains = ui::audio_in::list();
			std::printf("A/D INPUT（録音デバイス。画面から選ぶ）:\n");
			for (size_t k = 0; k < ains.size(); k++)
				std::printf("  %zu: %s\n", k, ains[k].c_str());
			if (ains.empty())
				std::printf("  （なし）\n");
			return 0;
		}
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) in_dev[0] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-b") && i + 1 < argc) in_dev[1] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-c") && i + 1 < argc) in_dev[2] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-d") && i + 1 < argc) in_dev[3] = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--usb")) usb_host = true;
		else if (!std::strcmp(argv[i], "--host-midi")) usb_host = false;
		else if (!std::strcmp(argv[i], "--midiout") && i + 1 < argc) mout_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-b") && i + 1 < argc) moutb_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-mu") && i + 1 < argc) moutmu_dev = std::atoi(argv[++i]);
		else if (ui::consume_engine_option(argv[i], eng_opts)) {}
		else if (!std::strcmp(argv[i], "--nomidi")) {
			// Nothing is opened and nothing is remembered: this is for tests,
			// which must leave the real settings file the way they found it.
			// The app itself is made further down, so the flag is carried there
			for (int &d : in_dev) d = -1;
			mout_dev = moutb_dev = moutmu_dev = -1;
			nomidi = true;
		}
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) latency = std::atoi(argv[++i]);
		else if (ui::consume_output_option(argv, argc, i, out_opts)) {}
		else if (ui::consume_window_option(argv[i], win_opts)) {}
		else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!std::strcmp(argv[i], "--boot")) boot_for_shot = true;
		else if (!std::strcmp(argv[i], "--grid")) grid = true;
		else if (!std::strcmp(argv[i], "--layout") && i + 1 < argc) layout_path = argv[++i];
		else if (!std::strcmp(argv[i], "--play") && i + 1 < argc) play_path = argv[++i];
		else if (!std::strcmp(argv[i], "--dump-layout") && i + 1 < argc) dump_layout = argv[++i];
		else if (!std::strcmp(argv[i], "--mid") && i + 2 < argc) {
			shot_mid = argv[++i];
			shot_secs = std::atof(argv[++i]);
			boot_for_shot = true;
		}
		else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) {
			if (std::sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2) { win_w = 1400; win_h = 360; }
			size_given = true;
		}
		else if (dir.empty()) dir = argv[i];
	}
	if (win_opts.lcd_only && !size_given) {
		win_w = 898;
		win_h = 290;
	}

	// Without --layout, look through the usual places in order
	if (layout_path.empty())
		layout_path = ui::layout::find_default();

	if (!dump_layout.empty()) {
		ui::layout l;
		std::string lerr;
		if (!layout_path.empty())
			l.load(layout_path, lerr);
		if (!l.save(dump_layout)) {
			std::fprintf(stderr, "%s に書けない\n", dump_layout.c_str());
			return 1;
		}
		std::printf("いまの配置を書き出した: %s\n", dump_layout.c_str());
		std::printf("直したら --layout で渡すか、窓で F5 を押す\n");
		return 0;
	}

	static ui::bridge br;
	static ui::midi_in  midi_ports[mu2000::MIDI_PORTS];
	static ui::midi_out mout, mout_b, mout_mu;

	// Picture only. An empty screen can be drawn even without any ROMs.
	if (!shot_path.empty() && (dir.empty() || !boot_for_shot)) {
		ui::snapshot s;
		std::snprintf(s.message, sizeof(s.message), "S-MU2000");
		br.publish(s);
		return shot(shot_path, win_w, win_h, br, grid, layout_path);
	}

	if (dir.empty()) {
		std::fprintf(stderr,
			"使い方: gui <rom ディレクトリ> [--midi 番号] [--midi-b 番号] [--midi-c 番号] [--midi-d 番号]"
			" [--midiout 番号] [--midiout-b 番号] [--midiout-mu 番号]"
			" [--latency ミリ秒] [--exclusive] [--layout panel.txt] [--play 曲.mid] [--host-midi] [--fast-midi] [--lcd]\n"
			"        [--factory]   覚えている設定を捨てて工場出荷状態で起動する\n"
			"        [--editor]    PC エディタも開く（窓では F2 か右クリック）\n"
			"        [--list-window] 一覧の窓も開く（窓では F3 か右クリック）\n"
			"        [--fx-window] インサーションの設定の窓も開く（一覧でインサーションの欄をダブルクリック）\n"
			"        [--shapes-window] パートの音色の窓も開く（一覧で VIB などの絵をダブルクリック）\n"
			"        [--master-window] マスターの窓も開く（一覧でマスターの行をダブルクリック）\n"
			"        gui --dump-layout panel.txt   いまの配置を書き出す\n"
			"        gui --list\n"
			"        gui [<rom ディレクトリ> --boot] --shot 絵.png [--size 1400x440]\n");
		return 1;
	}

	static ui::engine eng(br, midi_ports[0]);
	if (std::getenv("SMU2000_VOICECACHE"))
		eng_opts.voicecache = 1;
	ui::apply_engine_options(eng.mu, eng_opts);
	eng.native_fx.store(eng_opts.native_fx);
	for (int p = 1; p < mu2000::MIDI_PORTS; p++)
		eng.midi_p[p] = &midi_ports[p];
	eng.mout_b = &mout_b;
	eng.mout_mu = &mout_mu;
	eng.mout = &mout;
	if (!eng.load(dir)) {
		std::fprintf(stderr, "%s\n", eng.message.c_str());
		return 1;
	}
	// the overview reads voice names and instrument icons from the user's ROM (xg/voices.h)
	ui::xgui::set_voice_rom(eng.mu.program_rom());

	// **USB by default**, the way the machine is set up when it is connected to a
	// computer. The firmware passes ports C and D only when HOST SELECT is USB,
	// and then A and B arrive over USB as well. --host-midi gives the DIN ports A
	// and B only.
	//
	// Decided before any boot: reset() keys the host-present message on this,
	// and the --shot boot below returns early. Same move as gui.cpp.
	eng.mu.set_usb_host(usb_host);
	std::printf(usb_host ? "MIDI は USB の口（A-D の 64 パート）\n"
	                     : "--host-midi: DIN の口 A・B だけ（パート 1-32）\n");

	// Picture only, but taken after boot so the LCD has something on it
	if (!shot_path.empty()) {
		if (!eng.boot()) { std::fprintf(stderr, "%s\n", eng.message.c_str()); return 1; }
		eng.state.store(1);

		// The display is still settling right after boot. Idle a little to calm it.
		{
			s32 l, r;
			for (size_t i = 0; i < size_t(2.0 * RATE); i++)
				eng.mu.run_sample(l, r);
		}

		// The level meters need signal, so stream MIDI first when one was given
		if (!shot_mid.empty()) {
			std::vector<smf::event> evs;
			std::string err;
			if (!smf::load(shot_mid, evs, err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
			} else {
				std::printf("MIDI %zu 件を %.1f 秒ぶん流す\n", evs.size(), shot_secs);
				size_t at = 0;
				s32 l, r;
				for (size_t i = 0; i < size_t(shot_secs * RATE); i++) {
					const double now = double(i) / RATE;
					while (at < evs.size() && evs[at].time <= now) {
						for (u8 b : evs[at].bytes)
							eng.mu.midi_in(b);
						at++;
					}
					eng.mu.run_sample(l, r);
				}
			}
		}

		eng.publish();
		return shot(shot_path, win_w, win_h, br, grid, layout_path);
	}

	// ---- Put the window up

	static app gui(br, midi_ports, mout, mout_b, mout_mu);
	g_gui = &gui;
	// a MIDI file dropped on any window plays (the panel, the editor, the overview)
	ui::pc_window::set_drop_handler(play_dropped_file);
	gui.keep_settings = nomidi;
	gui.eng = &eng;
	gui.state = &eng.state;
	gui.lcd_only = win_opts.lcd_only;
	gui.panel.set_lcd_only(win_opts.lcd_only);
	gui.panel.resize(win_w, win_h);
	gui.set_layout(layout_path);
	gui.panel.resize(win_w, win_h);

	// Only the window uses the remembered settings: --shot has to give the same
	// picture every time
	eng.use_nvram = !out_opts.factory;
	if (out_opts.factory)
		std::printf("工場出荷状態で起動する（覚えていた設定は終わるときに上書きされる）\n");

	// Look up the previously chosen ports by name. --midi / --midiout win.
	//
	// Opening the ports here rather than on the boot thread keeps the names
	// settled before the window starts reading them for the status line
	// The machine's A/D INPUT. Declared here so the boot thread below can start
	// it; the engine only samples it through the pointer
	static ui::audio_in ain;
	gui.ain = &ain;
	eng.ain = &ain;

	{
		const port_names want = load_settings();
		br.set_gain(want.volume);
		// set before set_fold34, which writes the settings back through save_settings()
		eng.analog.store(want.analog);
		if (want.analog)
			std::printf("音の出口: アナログ（直流を切る）\n");
		gui.set_fold34(want.fold34);
		// --audio wins; otherwise the port that was opened last time
		gui.audio_name = out_opts.audio_dev ? std::string(out_opts.audio_dev) : want.audio;
		// A/D INPUT is remembered by name too. It is opened in the boot thread,
		// once the machine is up
		gui.ain_name = want.audio_in;
		gui.ain_keep = want.audio_in;
		if (!want.card.empty())
			gui.insert_card(want.card);
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			if (in_dev[p] == -2)
				in_dev[p] = find_device(ui::midi_in::list(), want.in[p]);
		if (mout_dev == -2)  mout_dev   = find_device(ui::midi_out::list(), want.out);
		if (moutb_dev == -2)  moutb_dev  = find_device(ui::midi_out::list(), want.out_b);
		if (moutmu_dev == -2) moutmu_dev = find_device(ui::midi_out::list(), want.out_mu);

		// A port that is not there yet keeps its name in the settings
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			gui.in_keep[p] = want.in[p];
		gui.out_keep    = want.out;
		gui.out_keep_b  = want.out_b;
		gui.out_keep_mu = want.out_mu;
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			gui.choose_in(p, in_dev[p], true);
		gui.choose_out(mout_dev, true);
		gui.choose_out_b(moutb_dev, true);
		gui.choose_out_mu(moutmu_dev, true);
		// Show the name that was remembered when the port could not be opened,
		// so it is visible that the choice was not lost
		auto show = [](const char *label, const std::string &now, const std::string &keep) {
			if (!now.empty())
				std::printf("%s: %s\n", label, now.c_str());
			else if (!keep.empty())
				std::printf("%s: なし（「%s」が見つからないか開けない。覚えたままにしてある）\n",
				            label, keep.c_str());
			else
				std::printf("%s: なし\n", label);
		};
		for (int p = 0; p < mu2000::MIDI_PORTS; p++)
			show(ui::IN_LABELS[p], gui.in_name[p], gui.in_keep[p]);
		show("MIDI OUT",   gui.out_name_mu, gui.out_keep_mu);
		show("MIDI THRU A", gui.out_name,    gui.out_keep);
		show("MIDI THRU B", gui.out_name_b,  gui.out_keep_b);
		std::fflush(stdout);
	}

	// Give the panel something to read before the boot thread says anything, so
	// the window comes up showing the boot message rather than a blank LCD
	eng.publish();

	// Boot on a separate thread, and start the audio once it is done
	static ui::audio_out out;
	gui.out = &out;
	std::thread boot_thread([&] {
		if (!eng.boot()) {
			eng.state.store(2);
			eng.publish();
			return;
		}
		// After boot, as in gui.cpp: starting needs the firmware
		if (eng_opts.native_engine) {
			eng.mu.set_native_engine(eng_opts.native_engine);
			if (eng_opts.voicecache)
				smu2000::voicecache::load(eng.mu, smu2000::voicecache::key(eng.mu));
		}
		eng.state.store(1);
		eng.publish();

		std::string err;
		if (!out.start(latency, [](s16 *o, u32 n) { eng.fill(o, n); }, err, out_opts.exclusive,
		               gui.audio_name)) {
			std::fprintf(stderr, "音声: %s\n", err.c_str());
			eng.message = "音声デバイスを開けない";
			eng.state.store(2);
			eng.publish();
			return;
		}
		// Remember the port that was actually opened, by name. **After** the MIDI
		// ports were settled above, or the settings written here would carry an
		// empty MIDI name and the next start would come up with no ports
		gui.audio_name = out.device_name();
		std::printf("音声の出口: %s\n", out.device_name().c_str());
		// A/D INPUT: open the recording device that was picked last time. A
		// device that cannot be opened now keeps its name in the settings, the
		// same as a MIDI port (gui.cpp does this here too)
		if (!gui.ain_name.empty()) {
			const auto names = ui::audio_in::list();
			const int dev = find_device(names, gui.ain_name);
			std::string aerr;
			if (dev >= 0 && ain.start(names[size_t(dev)], aerr)) {
				gui.ain_dev = dev;
				std::printf("A/D INPUT: %s（%s）\n", ain.device_name().c_str(), ain.format_line().c_str());
			} else
				std::printf("A/D INPUT: なし（%s）\n",
				            dev < 0 ? "デバイスが見つからない" : aerr.c_str());
		}
		// Hog mode is a request, not a guarantee: something else may hold it
		if (out_opts.exclusive)
			std::printf("独り占め: %s\n", out.exclusive() ? "取れた" : "取れなかった");
		gui.save_settings();
		// With --play, start streaming as soon as it begins to sound
		if (!play_path.empty())
			gui.play_song(play_path);
		std::printf("鳴らしている（待ち時間 %.1f ms、%s）\n",
		            1000.0 * out.buffer_frames() / RATE,
		            out.mmcss() ? "CoreAudio の実時間スレッド"
		                        : "実時間スレッドを取れていない（途切れやすい）");
		std::fflush(stdout);
	});

	// with --editor and friends, open those windows with the panel (same order as gui.cpp)
	if (win_opts.open_editor && !win_opts.lcd_only)
		gui.open_editor_window(gui.pc);
	if (win_opts.open_fx && !win_opts.lcd_only)
		gui.open_editor_window(gui.fx);
	if (win_opts.open_list && !win_opts.lcd_only)
		gui.open_editor_window(gui.list);
	if (win_opts.open_shapes && !win_opts.lcd_only)
		gui.open_editor_window(gui.shapes);
	if (win_opts.open_master && !win_opts.lcd_only)
		gui.open_editor_window(gui.master);

	ui::run_window(gui, "S-MU2000", win_w, win_h);

	// tell the editor windows we are closing (unmute the overview, restore its
	// receive channels, ...). The audio thread drains what we sent, so pause
	// a moment before stopping it
	ui::pc_shutdown_all(gui.list, gui.pc, gui.fx, gui.shapes, gui.master, br);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	out.stop();
	ain.stop();
	// Leaving the THRU ports open with notes still held would leave them stuck
	// on whatever is listening, so all sound off and all notes off go out first
	for (ui::midi_out *thru : { &mout, &mout_b }) {
		if (!thru->is_open())
			continue;
		for (int ch = 0; ch < 16; ch++) {
			for (u8 v : { u8(0xb0 | ch), u8(120), u8(0), u8(0xb0 | ch), u8(123), u8(0) })
				thru->send(v);
		}
	}
	if (boot_thread.joinable())
		boot_thread.join();
	gui.join_reboot();
	gui.flush_card();        // the sound has stopped; keep what was written to the card
	gui.save_settings();          // the audio port, the A/D input and the VOLUME knob's position
	// The sound has stopped by now. Keep the machine's settings only if it came up
	eng.settle_for_save();
	if (eng.state.load() == 1 && !smu2000::nvram::save(eng.mu))
		std::fprintf(stderr, "設定を残せなかった: %s\n", smu2000::nvram::path(eng.mu).c_str());
	gui.play.stop();
	for (ui::midi_in &m : midi_ports)
		m.close();
	mout.close();
	mout_b.close();
	mout_mu.close();

	if (out.produced())
		std::printf("CPU %.1f%%、1 回の最悪 %.2f ms、枯渇 %llu 回\n",
		            out.cpu_percent(), out.worst_ms(),
		            (unsigned long long)out.starved());
	return 0;
}
