// license:BSD-3-Clause
//
// The GUI application both graphical front ends are.
//
// gui.exe and the Mac GUI keep the same state (bridge, panel, player,
// button bar, remembered ports) and paint the same picture; only the event
// pump, the window system and the dialogs differ. That shared half lives
// here so a feature added on one side cannot be missed on the other. Each
// front end keeps its window class (WndProc / window_mac.mm) and forwards to
// these from thin per-platform shells.
//
// Slice 1: state + panel paint. Input dispatch, menu actions and lifecycle
// follow in later slices.

#ifndef S_MU2000_UI_APP_H
#define S_MU2000_UI_APP_H

#pragma once

#include "compat/cli_text.h"
#include <atomic>
#include <functional>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "ui/audio_in.h"
#include "ui/audio_out.h"
#include "ui/audio_output_switch.h"
#include "ui/audio_start.h"
#include "ui/audio_device_watch.h"
#include "ui/bridge.h"
#include "ui/engine.h"
#include "ui/fx_editor.h"
#include "ui/keymap.h"
#include "ui/master_editor.h"
#include "ui/sampling_editor.h"
#include "ui/menu.h"
#include "ui/options.h"
#include "ui/overview.h"
#include "ui/panel.h"
#include "ui/part_shapes.h"
#include "ui/pc_editor.h"
#include "ui/pc_host.h"
// Linux's pc_window lives in its own header (SDL3 shell): same class name
// and shape, but it must not be declared twice in one program
#ifdef __linux__
#include "ui/pc_window_linux.h"
#else
#include "ui/pc_window.h"
#endif
#include "ui/player.h"
#include "ui/player_view.h"
#include "ui/tool_args.h"
#include "ui/settings.h"
#include "ui/settings_view.h"
#include "ui/midi_commands.h"
#include "ui/audio_session.h"
#include "ui/midi_session.h"
#include "ui/shot.h"
#include "ui/snapshot.h"
#include "ui/status.h"
#include "ui/toolbar.h"
#include "ui/user_boards.h"
#include "ui/fc_banks.h"
#include "ui/fm_banks.h"

#include "nvram.h"
#include "smartmedia.h"
#include "smf.h"
#include "voicecache.h"

#include <mutex>

namespace ui {

struct engine;
class midi_in;
class midi_out;
class audio_in;

class app
{
public:
	explicit app(bridge &b) : br(b)
	{
		// The Sampling window asks the bridge which recording device is open;
		// the answer is ours, and it changes from two places (that window's
		// combo and the panel's A/D INPUT menu), so it is a question and not a
		// value either of them writes.
		wire_settings_actions();
		b.set_ain_current_fn([this] { return ain_name; });
	}

	// A backstop, not the plan: shutdown() joins both of these, but a process
	// can leave by a path that never reaches it (see shutdown()), and a
	// std::thread destroyed while joinable calls std::terminate. Joining here
	// turns that abort into a clean exit.
	~app()
	{
		if (m_ain_lister.joinable())
			m_ain_lister.join();
		if (reboot.joinable())
			reboot.join();
	}

	// ---- shared state (both windows keep the same)

	bridge   &br;
	midi_router midi;
	midi_routing midi_routes, pending_midi;
	midi_session midi_job;
	std::string midi_error, pending_edit_out;
	bool pending_midi_menu = false;
	std::vector<std::string> midi_menu_inputs, midi_menu_outputs;
	audio_device_watch midi_inputs_watch, midi_outputs_watch;

	// The qualified type: a bare `panel panel;` member is an error under
	// GCC's -Wchanges-meaning (the native Linux build compiles this file)
	ui::panel panel;
	player play;
	toolbar bar;                     // the window button bar (not on --lcd)

	// The five PC windows both sides show (same contents, own host window)
	pc_window list{ std::make_unique<overview>() };
	pc_window pc{ std::make_unique<pc_editor>() };
	pc_window fx{ std::make_unique<fx_editor>() };
	pc_window shapes{ std::make_unique<part_shapes>() };
	pc_window master{ std::make_unique<master_editor>() };
	pc_window sampling{ std::make_unique<sampling_editor>() };
	settings_state preferences_state;
	settings_actions preferences_actions;
	pc_window settings_win{ std::make_unique<settings_view>(preferences_state, preferences_actions) };
	// MIDI プレイヤーの窓。gui だけが持つ（上の 6 つはプラグインにもある）
	pc_window player_win{ std::make_unique<player_view>(play) };
	// プラグインボードの窓。これも gui だけ（プラグインではマスターの窓に欄が出る。xgui::set_board_window）
	pc_window board_win{ std::make_unique<board_editor>() };

	struct engine *eng = nullptr;    // set once the ROMs are loaded
	std::atomic<int> *state = nullptr; // the engine's, so menus can grey out
	bool lcd_only = false;           // --lcd: the LCD on its own
	std::string layout_path;

	// Editor sending shares physical endpoints with the routing matrix.
	std::string edit_out_name;
	std::string audio_name;          // the audio device, by name
	std::vector<std::string> audio_menu_devices;
	std::vector<int> audio_menu_rates;
	std::atomic<bool> audio_ready{false};
	audio_preferences audio_settings;
	std::vector<audio_channel_route> audio_routes;
	audio_session audio_job;
	audio_output_config previous_audio, persisted_audio;
	bool audio_change_from_user = true;
	std::string audio_error, startup_song;
	bool audio_startup_completed = false;
	std::chrono::steady_clock::time_point next_devices{};
	audio_device_watch audio_devices;
	bool audio_recovery_pending = false;
	int persisted_native_fx = 0, persisted_native_engine = 0;
	bool audio_failed = false; // firmware is booted, but its output failed
	std::string ain_name;            // the recording device, by name
	std::string ain_keep;
	std::string card_path;           // the SmartMedia in the slot, by path
	bool keep_settings = false;      // --nomidi: leave the remembered alone

	audio_out *out = nullptr;        // set once the audio device is open
	audio_in  *ain = nullptr;        // set once the recording device is picked

	// Device indices being opened (-1 unused). Names above outlive them:
	// unplugging USB shifts numbers, so reconnects look the names up again
	int ain_dev = -1;
	u64 reported_drops = 0;          // MIDI drops the UI thread last reported

	std::thread reboot;              // the factory-reset reboot, while it runs
	bool m_shut = false;              // shutdown() has run (see it: it is idempotent)

	void join_reboot()
	{
		if (reboot.joinable())
			reboot.join();
	}

	// ---- shared paint (the whole panel picture, status line included)

	// The timer half of a frame: the panel tick and the PC windows. Each
	// window runs it at 30 Hz, then paints through its draw list
	// (paint_main below)
	void frame_work()
	{
		run_deferred();
		poll_audio_settings();
		if (br.take_restart_request())
			defer_outside_paint([this] { do_restart(); });
		poll();
		serve_ain_requests();
		pc_frame_all(list, pc, fx, shapes, master, sampling, panel.xg(), panel.ram(), br,
		             [this](pc_window &w) { open_pc_window(w); });
		// プレイヤーの窓も同じ刻みで。自分の ImGui の文脈に切り替えるので、パネルの文脈へ戻す（pc_host.h と同じ理由）
		{
			ImGuiContext *const panel_ctx = ImGui::GetCurrentContext();
			player_win.frame(panel.xg(), panel.ram(), br);
			board_win.frame(panel.xg(), panel.ram(), br);
			sync_settings_state();
			settings_win.frame(panel.xg(), panel.ram(), br);
			// マスターの窓の「プラグインボード...」
			if (xgui::take_board_window_request())
				open_pc_window(board_win);
			ImGui::SetCurrentContext(panel_ctx);
		}
	}

	// A full frame: timer work, status middle, panel paint through the
	// window's draw list. The middle fragment and the PC window opening
	// stay virtual (backend stats, host windows)
	void paint_main(ImDrawList *dl, const im::fonts &f, int w)
	{
		frame_work();
		char middle[64] = {};
		if (audio_ready.load() && !audio_job.busy() && out && out->produced())
			format_middle(middle, sizeof(middle));
		paint_into(dl, w, middle, f);
	}

	// The wait/drop fragment for the status line (WASAPI: 待ち + 遅れ,
	// CoreAudio: 遅れ). Called only when the device is up
	virtual void format_middle(char *dst, std::size_t n) = 0;
	// Opens one PC window (host windows differ)
	virtual void open_pc_window(pc_window &w) = 0;

	void paint_into(ImDrawList *dl, int w, const char *middle, const im::fonts &f)
	{
		snapshot s;
		br.read(s);
		const u64 pressed = br.buttons();
		char status[320] = {};
		if (audio_ready.load() && !audio_job.busy() && out && out->produced()) {
			format_status_line(status, sizeof(status),
			                   s.voices_master + s.voices_slave,
			                   out->cpu_percent(), out->worst_ms(),
			                   middle,
			                   midi_status(midi_routes.inputs, 0).c_str(),
			                   midi_status(midi_routes.outputs, 0).c_str());
		}
		else
			std::snprintf(status, sizeof(status), "%s", UI_TEXT(status_booting, "Starting..."));
		panel.set_volume(br.gain());
		panel.paint(dl, s, pressed, status);
		// The bar paints after the panel (the panel fills everything). It keeps
		// the window's fixed 16 px set: it does not scale with the panel, so the
		// panel's own sizes would only make it jump around while resizing
		bar.paint(dl, w, f.label, f.bar_px);
	}

	// ---- shared input decisions (both windows act the same way)
	// What a mouse press means. bar_window is a BAR_* id to open; menu asks
	// for the context menu at the point (each side picks which one); neither
	// set means press the panel. The strip order, the jack spots and the LCD
	// guard are the same on both, so this is decided once.
	struct mouse_hit {
		bool handled = false;
		bool menu = false;
		int bar_window = -1;
	};

	mouse_hit hit_test(int x, int y, bool right) const
	{
		mouse_hit h;
		if (lcd_only)
			return h;
		// A secondary click opens the port picker wherever it lands
		if (right) {
			h.handled = true;
			h.menu = true;
			return h;
		}
		// The strip first: it is not the panel, so nothing reaches the machine
		const int id = bar.hit(x, y);
		if (id >= 0) {
			h.handled = true;
			h.bar_window = id;
			return h;
		}
		if (y < toolbar::HEIGHT) {
			h.handled = true;            // the strip's gaps
			return h;
		}
		// The jacks and the card slot are pressed, not clicked: they open a
		// menu instead of moving a panel control
		if (panel.on_midi_jack(x, y) || panel.on_ad_input(x, y) ||
		    panel.on_card_slot(x, y) || panel.on_phones(x, y) || panel.on_power(x, y)) {
			h.handled = true;
			h.menu = true;
			return h;
		}
		return h;
	}

	// A character key (both sides extract these from their key codes).
	// True when eaten: the meaning of a letter lives in ui/keymap.h.
	bool handle_panel_key(int ch, bool down)
	{
		mu2000::button b = mu2000::button::count;
		if (!button_for_char(ch, b))
			return false;
		br.press(b, down);
		return true;
	}

	// The one key handler: every pump translates its key codes into the
	// shared space (menu.h -- KEY_F2..F5 and the panel characters) and
	// calls this. Keyups of the F-keys mean nothing; the panel characters
	// latch their button while held
	virtual void key(int code, bool down)
	{
		if (lcd_only && down)
			return;
		switch (code) {
		case ui::KEY_F2:
			if (down)
				open_window_by_kind(BAR_EDITOR);
			return;
		case ui::KEY_F3:
			if (down)
				open_window_by_kind(BAR_LIST);
			return;
		case ui::KEY_F4:
			if (down)
				toggle_engine();
			return;
		case ui::KEY_F5:
			if (down)
				reload_layout();
			return;
		default:
			handle_panel_key(code, down);
			return;
		}
	}

	// The F4 native-engine toggle both sides offer (key and menu)
	void toggle_engine()
	{
		if (eng) {
			persisted_native_engine = eng->native_engine.load() ? 0 : 1;
			eng->want_native_engine.store(persisted_native_engine);
			save_settings();
		}
	}

	// The window lost focus: let go of everything the user was holding
	virtual void focus_lost()
	{
		pressed = false;
		br.release_all();
	}

	// The main window changed size
	virtual void resized(int w, int h)
	{
		panel.resize(w, h);
	}

	// A file was dropped on the window: playing it is what a drop means on
	// every platform
	virtual void file_dropped(const std::string &path)
	{
		play_song(path);
	}

	// Panel layout from a file (F5 reads it back). Same file both sides
	void apply_layout(const std::string &path, bool quiet)
	{
		panel.lay() = layout();
		std::string err;
		if (!path.empty() && panel.lay().load(path, err)) {
			if (!quiet)
				std::printf(CLI_T("Layout: %s\n", "配置: %s\n"), path.c_str());
		} else if (!path.empty() && !quiet) {
			std::printf(CLI_T("Layout: cannot open %s. Using the built-in layout\n", "配置: %s を開けない。組み込みの配置を使う\n"), path.c_str());
		}
		if (!err.empty())
			std::fprintf(stderr, "%s", err.c_str());
		std::fflush(stdout);
		panel.resize(panel.width(), panel.height());
	}

	void reload_layout() { apply_layout(layout_path, false); }

	// Which popup the point asks for. The
	// card slot, PHONES and A/D INPUT have their own; everywhere else gets
	// the settings menu
	// Which menu a press opens. Named and public because a platform adds groups
	// of its own to some of these and not others - iOS puts Bluetooth and
	// network MIDI in the menus that carry MIDI at all, and the ROM import in
	// the card menu, since it is a storage thing - and it should not have to
	// repeat the hit test to find out which one it is looking at.
	enum class menu_kind { none, card, phones, power, ain, ports, midi };
	menu_kind menu_kind_at(int x, int y) const
	{
		if (panel.on_card_slot(x, y))
			return menu_kind::card;
		if (panel.on_phones(x, y))
			return menu_kind::phones;
		if (panel.on_power(x, y))
			return menu_kind::power;
		if (panel.on_ad_input(x, y))
			return menu_kind::ain;
		if (panel.on_midi_jack(x, y))
			return menu_kind::midi;
		return menu_kind::ports;
	}
	virtual std::vector<menu_group> context_menu(int x, int y)
	{
		switch (menu_kind_at(x, y)) {
		case menu_kind::card:
			return menu_card(menu_snapshot());
		case menu_kind::phones:
			return menu_phones(menu_snapshot());
		case menu_kind::power:
			return menu_power(menu_snapshot());
		case menu_kind::ain:
			return menu_ain_only(audio_in::list(), ain_name);
		case menu_kind::midi:
			return menu_midi(menu_snapshot());
		default:
			return menu_ports(menu_snapshot());
		}
	}

	// ---- the event verbs, in pump vocabulary. Every pump (wnd_proc, the
	// SDL loop, window_mac.mm) calls these under the same names, so a
	// meaning lives once: the lcd_only guards and the outcome struct are
	// here, and the platforms only translate their events into these calls
	virtual ui::mouse_out mouse_down(int x, int y, bool right)
	{
		mouse_out o;
		if (lcd_only)
			return o;
		const mouse_hit h = hit_test(x, y, right);
		if (h.bar_window >= 0) {
			bar.set_down(h.bar_window);
			open_window_by_kind(h.bar_window);
			o.opened_window = true;
			pressed = true;
			return o;
		}
		if (h.handled) {
			o.show_menu = h.menu;
			return o;
		}
		pressed = true;
		o.panel_pressed = true;
		panel.press(x, y, br);
		return o;
	}

	virtual bool mouse_drag(int x, int y)
	{
		if (lcd_only || !pressed)
			return false;
		return panel.drag(x, y, br);
	}

	virtual void mouse_up()
	{
		if (!pressed)
			return;
		pressed = false;
		bar.set_down(-1);
		panel.release(br);
	}

	virtual bool wheel(int x, int y, int steps)
	{
		if (lcd_only || !steps)
			return false;
		return panel.wheel_at(x, y, steps, br);
	}

	// A pointing-hand cursor where something opens
	virtual bool hand_cursor(int x, int y)
	{
		if (lcd_only)
			return false;
		return panel.on_midi_jack(x, y) || panel.on_ad_input(x, y) ||
		       panel.on_card_slot(x, y) || panel.on_phones(x, y) || panel.on_power(x, y);
	}

	// ---- per-platform acts (thin shells implement these)

	// Open a PC window by BAR_* id (F2/F3, the strip, the menus)
	void open_window_by_kind(int kind)
	{
		if (kind == BAR_SETTINGS) {
			open_pc_window(settings_win);
			return;
		}
		if (kind == BAR_PLAYER) {
			open_pc_window(player_win);
			return;
		}
		if (kind == BAR_BOARD) {
			open_pc_window(board_win);
			return;
		}
		open_pc_window(*window_for_kind(kind, list, pc, fx, shapes, master, sampling));
	}

	// ---- remembered settings (gui.ini)

	// Where the file lives differs per platform (registry side vs Library)
	virtual std::string settings_path() const = 0;

	void save_settings()
	{
		if (keep_settings)                   // --nomidi: keep the ports
			return;
		const std::string path = settings_path();
		if (path.empty())
			return;
		remembered r;
		r.midi = midi_routes;
		for (int p = 0; p < IN_PORTS; p++) r.in[p] = midi_primary(midi_routes.inputs, p);
		r.out = midi_primary(midi_routes.outputs, 0);
		r.out_b = midi_primary(midi_routes.outputs, 1);
		r.out_mu = midi_primary(midi_routes.outputs, 2);
		r.audio_out = persisted_audio.device;
		r.audio = persisted_audio.preferences;
		r.audio_routes = audio_routes;
		r.limiter = eng && eng->limit_output.load();
		r.native_fx = persisted_native_fx;
		r.native_engine = persisted_native_engine;
		r.audio_in  = ain_name.empty() ? ain_keep : ain_name;
		r.card      = card_path;
		r.volume    = br.gain();
		r.fold34    = play.fold_extra_ports();
		r.thin_bends = play.thin_bends();
		r.analog    = eng && eng->analog.load();
		r.edit_out  = edit_out_name;
		// the imaginary plug-in board stays plugged in, like a real one
		r.board      = eng ? eng->mu.virtual_board_kind() : 0;
		r.board_part = eng ? eng->mu.virtual_board_part() + 1 : 1;
		for (int i = 0; i < mu2000::PLG_SLOTS - 1; i++) {
			r.board_more[i]      = eng ? eng->mu.virtual_board_kind(i + 1) : 0;
			r.board_more_part[i] = eng ? eng->mu.virtual_board_part(i + 1) + 1 : i + 2;
		}
		r.board_dls  = eng ? eng->mu.board_dls_path() : std::string();
		user_boards::save(br);
		r.board_file = user_boards::current_path();
		fm_banks::save(br);
		r.board_fm = fm_banks::current_path();
		fc_banks::save_all(br);
		for (int i = 0; i < fc_banks::TARGETS; i++)
			r.board_fc[i] = fc_banks::current_path(i);
		r.board_fc_cc = fc_banks::cc_map() == smu2000::vboard::fc_cc_map() ? std::string() : smu2000::vboard::fc_cc_text(fc_banks::cc_map());
		write_settings_file(path, collect_settings(r));
	}

	static remembered load_remembered(const std::string &path, bool processing = true)
	{
		remembered r;
		if (path.empty())
			return r;
		settings_map kv;
		if (!read_settings_file(path, kv))
			return r;
		apply_settings(kv, r);
		if (!processing) clear_processing_settings(r);
		return r;
	}

	// ---- ports (the menus pick these)

	static std::string midi_primary(const std::vector<midi_route> &routes, int column)
	{
		for (const auto &r : routes) if (r.ports & (1u << column)) return r.device;
		return {};
	}
	static std::string midi_status(const std::vector<midi_route> &routes, int column)
	{
		const auto name = midi_column_name(routes, column);
		return name.empty() ? UI_TEXT(status_none, "none") : name;
	}
	std::string midi_startup_status(const std::vector<midi_route> &routes, int column, bool output) const
	{
		std::string text;
		for (const auto &route : routes) if (route.ports & (1u << column)) {
			if (!text.empty()) text += ", ";
			text += route.device;
			if (!midi.connected(output, route.device)) text += CLI_T(" (not found; remembered)", " (見つからない; 記憶を保持)");
		}
		return text.empty() ? UI_TEXT(status_none, "none") : text;
	}
	void request_midi(midi_routing routes, std::string editor, bool missing_ok = false, bool menu_pick = false)
	{
		if (!audio_ready.load() || audio_job.busy() || midi_job.busy() || !eng || !state || state->load() == 2) return;
		if (state->load() != 1 && !audio_failed) return;
		join_reboot();
		pending_midi_menu = menu_pick;
		pending_midi = std::move(routes);
		pending_edit_out = std::move(editor);
		midi_job.begin(*eng, midi_routes_with_editor(pending_midi, pending_edit_out), missing_ok);
	}
	void finish_midi_change(const midi_change_result &result)
	{
		midi_error = result.error;
		if (pending_midi_menu && !midi_error.empty())
			defer_outside_paint([this, text = midi_error] { menu_error(text); });
		if (result.selected) {
			midi_routes = std::move(pending_midi);
			edit_out_name = std::move(pending_edit_out);
			save_settings();
		}
	}
	void choose_midi(bool output, int column, int device)
	{
		const auto &names = output ? midi_menu_outputs : midi_menu_inputs;
		if (device >= 0 && size_t(device) >= names.size()) return;
		auto routes = midi_routes;
		auto &rows = output ? routes.outputs : routes.inputs;
		if (device < 0) clear_midi_column(rows, column);
		else {
			const auto &name = names[size_t(device)];
			set_midi_route(rows, name, midi_route_mask(rows, name) ^ (1u << column));
		}
		request_midi(std::move(routes), edit_out_name, false, true);
	}
	void choose_edit_out(int dev)
	{
		const auto names = midi_out::list();
		if (dev >= 0 && size_t(dev) >= names.size()) return;
		request_midi(midi_routes, dev < 0 ? std::string() : names[size_t(dev)], false, true);
	}
	int edit_dest(int port) const { return edit_out_name.empty() ? (port == 1 ? 1 : 0) : 2; }
	// 音色の窓に、送り先の品書きと送る道を渡す
	void wire_send_out()
	{
		xgui::out_hooks h;
		h.devices = [] { return midi_out::list(); };
		h.chosen = [this] { return edit_out_name; };
		h.panel_desc = [this] {
			return "A: " + midi_status(midi_routes.outputs, 0) + " / B: " + midi_status(midi_routes.outputs, 1);
		};
		h.choose = [this](int dev) { choose_edit_out(dev); };
		h.dest = [this](int port) { return edit_dest(port); };
		xgui::set_out_hooks(std::move(h));
	}

	// サンプリングの窓の録音デバイスの欄（bridge::set_ain_devices / request_ain）。
	// 一覧は窓が欄を開いたときと、選び直した後に作り直す（デバイスを数えるのは重いので毎コマはしない）。
	//
	// **パネルを描いている最中（frame_work の中）にデバイスを数えたり開いたりしない。** Windows の UI の糸は
	// COM の STA なので、WASAPI を初めて使うときにたまっている窓のメッセージを処理することがあり、そこで
	// パネルの WM_PAINT が入れ子で来ると、終わっていないコマの上で ImGui::NewFrame が呼ばれて assert で落ちた
	// （作り直した exe の初回の起動で、起動が遅いときに出た）。一覧は別の糸で数え、選び直しは描画の外で行う
	// （Windows は窓のメッセージで、ほかは次のコマの頭で。defer_outside_paint）
	bool m_ain_listed = false;
	std::atomic<bool> m_ain_listing{false};
	std::thread m_ain_lister;
	void list_ain_async()
	{
		if (m_ain_listing.exchange(true))
			return;
		if (m_ain_lister.joinable())
			m_ain_lister.join();
		m_ain_lister = std::thread([this] {
			br.set_ain_devices(audio_in::list());
			m_ain_listing.store(false);
		});
	}
	void serve_ain_requests()
	{
		if (br.take_ain_list_request() || !m_ain_listed) {
			m_ain_listed = true;
			list_ain_async();
		}
		const int want = br.take_ain_request();
		if (want >= -1)
			defer_outside_paint([this, want] {
				choose_ain(want);
				list_ain_async();
			});
		// サンプリングの窓の「カード」: 頼まれたカードを差し、差しているカードの場所を知らせる
		std::string card;
		if (br.take_card_request(card))
			defer_outside_paint([this, card] {
				if (insert_card(card))
					save_settings();
			});
		br.set_card_path(card_path);
	}
	// 描画の外で行う仕事。Windows は窓のメッセージで（app_win.h）、ほかは次のコマの頭で
	std::vector<std::function<void()>> m_deferred;
	virtual void defer_outside_paint(std::function<void()> f) { m_deferred.push_back(std::move(f)); }
	void run_deferred()
	{
		std::vector<std::function<void()>> todo;
		todo.swap(m_deferred);
		for (auto &f : todo)
			f();
	}

	// The engine reads A/D input through its own pointer (engine::fill() calls
	// ain->pop() once per sample), so opening a device is only half the job -
	// the machine has to be told about it. This cannot live in make_audio(),
	// because the front ends make the objects at different points: the desktops
	// wire the engine before run() makes them, iOS the other way round. It goes
	// where the device opens instead, which both paths below share.
	// open=false also covers a device that failed to open: an engine pulling
	// from an input that is not there would only count emptiness.
	void attach_ain(bool open)
	{
		if (eng)
			eng->ain = open ? ain : nullptr;
	}

	bool choose_ain(int dev, bool keep = false)
	{
		if (!keep)
			ain_keep.clear();
		if (!ain)
			return false;
		ain->stop();
		if (dev < 0) {
			ain_name.clear();
			attach_ain(false);
		} else {
			const auto names = audio_in::list();
			if (dev < int(names.size())) {
				std::string err;
				if (!ain->start(names[size_t(dev)], err)) {
					attach_ain(false);
					std::fprintf(stderr, "A/D INPUT: %s\n", err.c_str());
					if (!keep)
						menu_error(err);
				} else {
					attach_ain(true);
					std::printf(CLI_T("A/D INPUT: %s (%s)\n", "A/D INPUT: %s（%s）\n"),
					            ain->device_name().c_str(),
					            ain->format_line().c_str());
					std::fflush(stdout);
				}
				ain_name = names[size_t(dev)];
			}
		}
		save_settings();
		return true;
	}

	// ---- SmartMedia (the card slot)

	// Written-back blocks go to the file; only snapshotting stops the
	// audio thread. Called from the timers, and on eject/close/save
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

	void card_tick()
	{
		const u64 now = smu2000::perf_ticks() * 1000 / smu2000::perf_freq();
		if (now - last_flush < 2000)
			return;
		last_flush = now;
		flush_card();
	}

	// A MIDI loop (THRU fed back into an IN) overflows the guards. Said out
	// loud once a second, from the window's timer rather than the paint
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
		             CLI_T("Dropped MIDI, too much of it: THRU A %llu / THRU B %llu / received %llu bytes"
" (check for a MIDI loop)\n", "MIDI が多すぎるので捨てた: THRU A %llu / THRU B %llu / 受信 %llu バイト"
		             "（MIDI の輪ができていないか確かめる）\n"),
		             (unsigned long long)eng->guard_a.dropped(),
		             (unsigned long long)eng->guard_b.dropped(),
		             (unsigned long long)eng->mu.midi_dropped());
	}

	// The window's timer work, both sides: feed the panel, publish CPU and
	// engine state for the PC windows, flush the card file, report drops.
	// Opening/drawing the PC windows stays per side (different window types)
	void poll()
	{
		panel.tick(br);
		if (audio_ready.load() && !audio_job.busy() && out && out->produced())
			br.set_cpu(float(out->cpu_recent()));   // 直近の重さ（平均は終わりの集計に）
		br.set_engine(eng ? eng->native_engine.load() : -1);
		card_tick();
		report_drops();
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
			std::printf(CLI_T("SmartMedia removed: %s\n", "SmartMedia を抜いた: %s\n"), card_path.c_str());
		std::fflush(stdout);
		card_path.clear();
		save_settings();
	}

	// Load it first, so a file that cannot be read does not take the slot
	// away from the card that is already in it. quiet is for boot, where a
	// missing file must stay silent
	bool insert_card(const std::string &path, bool quiet = false)
	{
		if (!eng)
			return false;
		smu2000::smartmedia card;
		std::string err;
		if (!card.load(path, err)) {
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
			if (!quiet)
				menu_error(err);
			return false;
		}
		eject_card();
		{
			const std::lock_guard<std::mutex> hold(eng->card_lock);
			eng->mu.card() = std::move(card);
			eng->mu.card_swapped();   // firmware に抜けたのを見せる（前のカードの FAT を忘れさせる）
		}
		card_path = path;
		std::printf(CLI_T("SmartMedia inserted: %s (%uMB)\n", "SmartMedia を差した: %s（%uMB）\n"),
		            path.c_str(), eng->mu.card().megabytes());
		std::fflush(stdout);
		save_settings();
		return true;
	}

	// A new card, already formatted the way the machine's UTIL -> CARD ->
	// Format leaves it (smartmedia::format), so it can be saved to at once
	void new_card(u32 megabytes)
	{
		const std::string path = ask_card_save_path();
		if (path.empty())
			return;
		smu2000::smartmedia card;
		if (!card.create(megabytes) || !card.format()) {
			std::fprintf(stderr, "%s\n", UI_TEXT(dlg_card_create_fail, "Cannot create the SmartMedia image"));
			return;
		}
		std::string err;
		if (!card.save(path, err)) {
			std::fprintf(stderr, "SmartMedia: %s\n", err.c_str());
			menu_error(err);
			return;
		}
		if (insert_card(path))
			menu_note(UI_TEXT(dlg_fresh_card, "Inserted a new SmartMedia image.\n"
			                                  "It is already formatted (as UTIL → CARD → Format leaves it), so it can be saved to right away."));
	}

	void do_card_open()
	{
		const std::string path = ask_card_open_path();
		if (!path.empty() && insert_card(path))
			save_settings();
	}

	// ---- MIDI file playback

	// Plays the file picked from a menu or dropped on a window. Restarts it
	// when it is already playing
	bool play_song(const std::string &path)
	{
		std::string err;
		if (!play.start(path, br, err)) {
			std::fprintf(stderr, CLI_T("Cannot open: %s\n", "開けない: %s\n"), err.c_str());
			char m[512];
			std::snprintf(m, sizeof(m), UI_TEXT(dlg_cannot_fmt, "Cannot open: %s"), err.c_str());
			menu_error(m);
			return false;
		}
		std::printf(CLI_T("Playing: %s (%.1f s)\n", "再生: %s（%.1f 秒）\n"), path.c_str(), play.length());
		// The machine has two ports, so a four-port file is either folded
		// onto them or has its extra parts dropped
		if (play.ports_used() > 2)
			std::printf(CLI_T("  This song uses %d ports. C and D are not supported, so ports 3 and up are %s\n", "  この曲は %d 口ぶん。C・D は未対応なので、口 3 以降は%s\n"),
			            play.ports_used(),
			            play.fold_extra_ports() ? CLI_T("played on top of A and B", " A・B に重ねて鳴らす") : CLI_T("not played", "鳴らさない"));
		std::fflush(stdout);
		return true;
	}

	// The same, from bytes rather than a path: a front end that cannot hand over
	// a path - iOS reads a picked file while its grant lasts, because the grant
	// dies with the process - plays the file it read. The reporting is
	// play_song's, so a failure reads the same whichever way it arrived.
	bool play_song_from_memory(const u8 *data, size_t size, const std::string &name)
	{
		std::string err;
		if (!play.start_from_memory(data, size, name, br, err)) {
			std::fprintf(stderr, "開けない: %s\n", err.c_str());
			char m[512];
			std::snprintf(m, sizeof m, UI_TEXT(dlg_cannot_fmt, "Cannot open: %s"), err.c_str());
			menu_error(m);
			return false;
		}
		std::printf("再生: %s（%.1f 秒）\n", name.c_str(), play.length());
		if (play.ports_used() > 2)
			std::printf("  この曲は %d 口ぶん。C・D は未対応なので、口 3 以降は%s\n",
			            play.ports_used(),
			            play.fold_extra_ports() ? " A・B に重ねて鳴らす" : "鳴らさない");
		std::fflush(stdout);
		return true;
	}

	void do_midi_file()
	{
		const std::string path = ask_midi_file_path();
		if (!path.empty())
			play_song(path);
	}

	// ---- the rest of the menu

	// Same backend contract on Windows, macOS and Linux. Device names come
	// from the menu snapshot, never from a new enumeration after the click.
	void choose_audio(int dev)
	{
		if (dev >= 0 && size_t(dev) >= audio_menu_devices.size()) return;
		request_audio({dev < 0 ? std::string() : audio_menu_devices[size_t(dev)], audio_settings});
	}

	void request_audio(audio_output_config wanted, bool user = true)
	{
		if (!audio_ready.load() || audio_job.busy() || midi_job.busy() || !out || !eng || !state) return;
		if (state->load() != 1 && !audio_failed) return;
		if (reboot.joinable()) join_reboot();
		if (wanted.device != audio_name || wanted.preferences.stream.driver != audio_settings.stream.driver) {
			wanted.preferences.stream.sample_rate = 0;
			wanted.preferences.stream.left = 0;
			wanted.preferences.stream.right = 1;
			const std::string key = wanted.device.empty() ? audio_out::default_device_name(wanted.preferences.stream.driver) : audio_device_key(wanted.device);
			for (const auto &route : audio_routes) if (route.device == key && route.driver == wanted.preferences.stream.driver) {
				wanted.preferences.stream.left = route.left;
				wanted.preferences.stream.right = route.right;
			}
		}
		wanted.preferences.stream.strict = user;
		previous_audio = {audio_name, audio_settings};
		audio_change_from_user = user;
		audio_job.begin(*eng, *out, wanted, previous_audio, !audio_failed);
	}

	void complete_audio_startup()
	{
		if (audio_startup_completed || audio_failed || audio_job.busy() || !audio_ready.load()) return;
		audio_startup_completed = true;
		// Normalize a CLI substring to the full menu name on the UI thread.
		// Empty stays empty so System default continues to follow the OS.
		if (!audio_name.empty()) {
			const std::string opened = out->device_name();
			audio_name = opened;
			for (const auto &name : audio_out::list(audio_settings.stream.driver))
				if (name == opened || name.substr(0, name.find("  (")) == opened) {
					audio_name = name;
					break;
				}
		}
		start_ad();
		if (!startup_song.empty()) play_song(startup_song);
		say_audio_running();
	}

	void finish_audio_change(const audio_output_switch_result &result)
	{
		audio_failed = !result.selected && !result.restored;
		audio_error = result.selected ? std::string() : result.error;
		if (result.selected || result.restored) {
			audio_name = result.config.device;
			audio_settings = result.config.preferences;
		}
		if (result.selected) {
			if (audio_change_from_user) remember_audio_change(persisted_audio, previous_audio, result.config);
			const std::string device = out->device_name();
			auto it = std::find_if(audio_routes.begin(), audio_routes.end(), [&](const auto &r) { return r.device == device && r.driver == audio_settings.stream.driver; });
			const audio_channel_route route{device, audio_settings.stream.left, audio_settings.stream.right, audio_settings.stream.driver};
			if (audio_change_from_user) {
				if (it == audio_routes.end()) audio_routes.push_back(route); else *it = route;
				save_settings();
			}
			say_audio_opened(audio_settings.exclusive);
		}
		if (!result.selected && audio_change_from_user) {
			char error[2048];
			std::snprintf(error, sizeof(error), UI_TEXT(audio_switch_failed_fmt, "Cannot switch audio output:\n%s"), result.error.c_str());
			defer_outside_paint([this, text = std::string(error)] { menu_error(text); });
		}
		if (audio_failed) report_audio_failure();
	}

	void report_audio_failure()
	{
		eng->message = CLI_T("cannot open the audio device", "音声デバイスを開けない");
		eng->state.store(2);
		eng->publish();
	}

	void poll_audio_settings()
	{
		if (!audio_ready.load()) return;
		if (const auto result = midi_job.poll()) finish_midi_change(*result);
		if (const auto result = audio_job.poll()) finish_audio_change(*result);
		if (!audio_job.busy() && !midi_job.busy() && !out->running() && !audio_failed) {
			audio_failed = true;
			audio_error = CLI_T("Audio output disconnected", "音声出力が切断された");
			report_audio_failure();
			audio_recovery_pending = true;
		}
		if (audio_recovery_pending && !audio_job.busy() && !midi_job.busy()) {
			audio_recovery_pending = false;
			defer_outside_paint([this] { request_audio({audio_name, audio_settings}, false); });
		}
		if (!audio_startup_completed && !audio_failed && !audio_job.busy() && !midi_job.busy())
			defer_outside_paint([this] { complete_audio_startup(); });
		const auto now = std::chrono::steady_clock::now();
		if (!settings_win.visible() || now < next_devices) return;
		next_devices = now + std::chrono::seconds(1);
		defer_outside_paint([this] { refresh_audio_devices(); });
	}

	void refresh_audio_devices()
	{
		if (audio_job.busy() || midi_job.busy()) return;
		for (int d = 0; d < 3; d++) if (supported_audio_driver(audio_driver(d)))
			preferences_state.driver_outputs[size_t(d)] = audio_out::list(audio_driver(d));
		preferences_state.outputs = preferences_state.driver_outputs[size_t(audio_settings.stream.driver)];
		preferences_state.inputs = audio_in::list();
		preferences_state.midi_inputs = midi_in::list();
		preferences_state.midi_outputs = midi_out::list();
		const bool midi_changed = midi_inputs_watch.changed(preferences_state.midi_inputs, {}) |
		                          midi_outputs_watch.changed(preferences_state.midi_outputs, {});
		if (midi_changed && state->load() == 1 &&
		    midi.needs_refresh(midi_routes_with_editor(midi_routes, edit_out_name), preferences_state.midi_inputs, preferences_state.midi_outputs)) {
			request_midi(midi_routes, edit_out_name, true);
			return;
		}
		// Hog mode can move macOS's system default away from the device we hold.
		const bool follows_default = audio_name.empty() && !out->exclusive() && audio_settings.stream.driver != audio_driver::asio;
		const std::string default_name = follows_default ? audio_out::default_device_name(audio_settings.stream.driver) : std::string();
		const bool changed = audio_devices.changed(preferences_state.outputs, default_name);
		const bool default_changed = follows_default && !default_name.empty() && default_name != out->device_name();
		if (!changed || (!audio_failed && !default_changed)) return;
		audio_output_config wanted{audio_name, audio_settings};
		if (default_changed) {
			wanted.preferences.stream.sample_rate = 0;
			wanted.preferences.stream.left = 0; wanted.preferences.stream.right = 1;
			for (const auto &route : audio_routes) if (route.device == default_name && route.driver == audio_settings.stream.driver) {
				wanted.preferences.stream.left = route.left; wanted.preferences.stream.right = route.right;
			}
		}
		request_audio(wanted, false);
	}

	void sync_settings_state()
	{
		auto &s = preferences_state;
		s.ready = audio_ready.load();
		s.busy = audio_job.busy() || midi_job.busy();
		if (!s.ready) return; // all boot-thread writes precede this handshake
		s.audio = {audio_name, audio_settings};
		s.input = ain_name.empty() ? ain_keep : ain_name;
		s.error = audio_error;
		s.connected = !audio_failed && !s.busy;
		if (!s.busy && out) {
			s.stream = out->stream_info();
		}
		s.midi = midi_routes;
		s.midi_error = midi_error;
		s.gain = br.gain();
		s.analog = eng->analog.load();
		s.limiter = eng->limit_output.load();
		s.native_fx = eng->native_fx.load();
		s.native_engine = eng->native_engine.load();
		s.thin_bends = play.thin_bends();
	}

	void wire_settings_actions()
	{
		preferences_actions.language = [](int value) { xgui::set_help_lang(value); };
		preferences_actions.audio = [this](audio_output_config c) {
			defer_outside_paint([this, c = std::move(c)] { request_audio(c); });
		};
		preferences_actions.control_panel = [this] {
			audio_output_config config{audio_name, audio_settings, true};
			config.preferences.stream.sample_rate = 0;
			config.preferences.stream.buffer_frames = 0;
			defer_outside_paint([this, config] { request_audio(config); });
		};
		preferences_actions.input = [this](std::string name) {
			defer_outside_paint([this, name] {
				const int dev = find_device(audio_in::list(), name);
				if (name.empty() || dev >= 0) choose_ain(dev);
			});
		};
		preferences_actions.midi = [this](midi_routing routes) {
			defer_outside_paint([this, routes = std::move(routes)] { request_midi(routes, edit_out_name); });
		};
		preferences_actions.command = [this](int id) { defer_outside_paint([this, id] { menu_chosen(id); }); };
		preferences_actions.volume = [this](float gain) { br.set_gain(gain); };
		preferences_actions.save_volume = [this] { save_settings(); };
		preferences_actions.limiter = [this](bool on) { eng->limit_output.store(on); save_settings(); };
	}

	// Throwing the settings away means rebooting the machine, which takes
	// tens of seconds, so it runs on its own thread (joined first: two
	// boots at once would both be writing the machine)
	void do_factory_reset()
	{
		if (!eng || !state || state->load() != 1 || midi_job.busy())
			return;
		if (!confirm_factory_reset())
			return;
		play.stop();
		join_reboot();
		reboot = std::thread([this] { eng->factory_reset(); });
	}

	// Power the machine off and on. The settings carry over (as with the real
	// unit's battery-backed memory); the firmware boots from scratch, which is
	// also when it looks for plug-in boards
	void do_restart()
	{
		if (!eng || !state || state->load() != 1 || midi_job.busy())
			return;
		play.stop();
		join_reboot();
		reboot = std::thread([this] { eng->restart(); });
	}

	void set_fold34(bool on)
	{
		play.set_fold_extra_ports(on);
		save_settings();
	}

	void toggle_thin_bends()
	{
		play.set_thin_bends(!play.thin_bends());
		save_settings();
	}

	void set_analog(bool on)
	{
		if (!eng)
			return;
		// Digital matches S/PDIF (some DPCM samples keep their DC, as on
		// the hardware); analog cuts DC like LINE OUT and PHONES do
		eng->analog.store(on);
		std::printf(CLI_T("Sound output: %s\n", "音の出口: %s\n"), on ? CLI_T("analogue (DC removed)", "アナログ（直流を切る）") : CLI_T("digital", "デジタル"));
		std::fflush(stdout);
		save_settings();
	}

	void toggle_fx()
	{
		if (eng) {
			persisted_native_fx = eng->native_fx.load() ? 0 : 2;
			eng->want_native_fx.store(persisted_native_fx);
			save_settings();
		}
	}

	// What the shared menu builders (ui/menu.h) show, from this window's state
	menu_state menu_snapshot()
	{
		menu_state s;
		s.midi_ins = midi_menu_inputs = midi_in::list();
		s.midi_outs = midi_menu_outputs = midi_out::list();
		s.audio_ins = audio_in::list();
		s.audio_outs = audio_out::list(audio_settings.stream.driver);
		audio_menu_devices = s.audio_outs;
		s.audio_ready = audio_ready.load() && !audio_job.busy() && state && (state->load() == 1 || audio_failed);
		if (s.audio_ready) {
			s.audio_name = audio_name;
			s.audio_rates = audio_menu_rates = out->stream_info().rates;
			s.audio_rate = audio_settings.stream.sample_rate;
			s.limiter = eng->limit_output.load();
		}
		if (!audio_ready.load()) return s;
		s.midi = midi_routes;
		s.ain_name = ain_name;
		s.card_path = card_path;
		s.playing = play.playing();
		s.play_name = play.name();
		s.fold34 = play.fold_extra_ports();
		s.thin_bends = play.thin_bends();
		s.ready = audio_ready.load() && !audio_job.busy() && !midi_job.busy() && eng && state && state->load() == 1;
		s.native_fx = eng && eng->native_fx.load();
		s.native_engine = eng && eng->native_engine.load();
		s.analog = eng && eng->analog.load();
		return s;
	}

	// The shared dispatch for the 26 menu IDs both front ends render
	// (ui/menu.h). Only the dialogs and the error display are per-platform
	// (the hooks below); everything else is the same calls in the same order
	void menu_chosen(int id)
	{
		for (int p = 0; p < IN_PORTS; p++) {
			const int none = p == 4 ? int(ID_INE_NONE) : ID_IN_NONE + p * ID_IN_STRIDE;
			const int base = p == 4 ? int(ID_INE_BASE) : ID_IN_BASE + p * ID_IN_STRIDE;
			if (id == none)                    { choose_midi(false, p, -1); return; }
			if (id >= base && id < base + 256) { choose_midi(false, p, id - base); return; }
		}
		if (id == ID_OUT_NONE)                                        choose_midi(true, 0, -1);
		else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 256)         choose_midi(true, 0, id - ID_OUT_BASE);
		else if (id == ID_OUTMU_NONE)                                 choose_midi(true, 2, -1);
		else if (id >= ID_OUTMU_BASE && id < ID_OUTMU_BASE + 256)     choose_midi(true, 2, id - ID_OUTMU_BASE);
		else if (id == ID_OUTB_NONE)                                  choose_midi(true, 1, -1);
		else if (id >= ID_OUTB_BASE && id < ID_OUTB_BASE + 256)       choose_midi(true, 1, id - ID_OUTB_BASE);
		else if (id == ID_AIN_NONE)                                   choose_ain(-1);
		else if (id >= ID_AIN_BASE && id < ID_AIN_BASE + 256)         choose_ain(id - ID_AIN_BASE);
		else if (id == ID_AUDIO_DEFAULT)                              choose_audio(-1);
		else if (id >= ID_AUDIO_BASE && id < ID_AUDIO_BASE + 256)     choose_audio(id - ID_AUDIO_BASE);
		else if (id == ID_CARD_OPEN)                                  do_card_open();
		else if (id == ID_CARD_EJECT)                                 { eject_card(); save_settings(); }
		else if (id >= ID_CARD_NEW16 && id <= ID_CARD_NEW128)         new_card(16u << (id - ID_CARD_NEW16));
		else if (id == ID_PLAY_FILE)                                  do_midi_file();
		else if (id == ID_STOP_FILE)                                  play.stop();
		else if (id == ID_PORTS34_FOLD)                               set_fold34(true);
		else if (id == ID_PORTS34_DROP)                               set_fold34(false);
		else if (id == ID_THIN_BENDS)                                 toggle_thin_bends();
		else if (id == ID_NATIVE_FX)                                  toggle_fx();
		else if (id == ID_NATIVE_ENGINE)                              toggle_engine();
		else if (id == ID_FACTORY)                                    do_factory_reset();
		else if (id == ID_RESTART)                                    do_restart();
		else if (id == ID_RATE_AUTO || (id >= ID_RATE_BASE && id < ID_RATE_BASE + int(audio_menu_rates.size()))) {
			auto next = audio_settings;
			next.stream.sample_rate = id == ID_RATE_AUTO ? 0 : audio_menu_rates[size_t(id - ID_RATE_BASE)];
			request_audio({audio_name, next});
		}
		else if (id == ID_OUTPUT_LIMITER) { eng->limit_output.store(!eng->limit_output.load()); save_settings(); }
		else if (id >= ID_RESET_GM && id <= ID_MIDI_PANIC) { if (eng && state->load() == 1) send_midi_command(id, br); }
		else if (id == ID_SETTINGS)                                   open_window_by_kind(BAR_SETTINGS);
		else if (id == ID_PC_EDITOR)                                  open_window_by_kind(BAR_EDITOR);
		else if (id == ID_OVERVIEW)                                   open_window_by_kind(BAR_LIST);
		else if (id == ID_OUTPUT_DIGITAL || id == ID_OUTPUT_ANALOG)   set_analog(id == ID_OUTPUT_ANALOG);
	}

	// ---- bring-up and shutdown (both mains call these in order)

	// Wires the engine to the ports main() owns and the parsed engine flags
	void wire_engine(engine &eng, engine_options &o)
	{
		if (std::getenv("SMU2000_VOICECACHE"))
			o.voicecache = 1;
		apply_engine_options(eng.mu, o);
		eng.native_fx.store(o.native_fx);
	}

	// Switches the booted machine to the native engine on request (the
	// stored flag is what the overview badge reads)
	void apply_native_engine(engine &eng, const engine_options &o)
	{
		if (!o.native_engine)
			return;
		eng.mu.set_native_engine(o.native_engine);
		eng.native_engine.store(o.native_engine);
		if (o.voicecache)
			smu2000::voicecache::load(eng.mu, smu2000::voicecache::key(eng.mu));
	}

	// Loads the ROMs and wires the USB host flag. False exits with code 1
	bool load_machine(engine &eng, const tool_args &a)
	{
		if (!eng.load(a.dir)) {
			std::fprintf(stderr, "%s\n", eng.message.c_str());
			return false;
		}
		// The overview reads voice names and instrument icons from the ROM
		xgui::set_voice_rom(eng.mu.program_rom());
		// USB ports by default, as when the hardware sits on PC USB (parts
		// C/D only pass when HOST SELECT is USB; on USB, A/B go quiet on DIN
		// just like the hardware). --host-midi leaves DIN A/B only.
		// Decided before booting: reset() learns here whether to queue the
		// "a host is here" notice
		eng.mu.set_usb_host(a.usb_host);
		play.set_usb_ports(a.usb_host);        // ファイルの口 3・4 を C・D へ送るか
		std::printf(a.usb_host ? CLI_T("MIDI goes to the USB ports (A-D, 64 parts)\n", "MIDI は USB の口（A-D の 64 パート）\n")
		                     : CLI_T("--host-midi: the DIN ports A and B only (parts 1-32)\n", "--host-midi: DIN の口 A・B だけ（パート 1-32）\n"));
		return true;
	}

	// Boots the firmware to snap --shot/--mid pictures (settles the display,
	// plays the MIDI for the meters). 1 fails, 0 snaps, -1 carries on
	int run_boot_shot(engine &eng, bridge &br, const tool_args &a,
	                  const window_options &w)
	{
		if (a.shot_path.empty())
			return -1;
		if (!eng.boot()) {
			std::fprintf(stderr, "%s\n", eng.message.c_str());
			return 1;
		}
		eng.state.store(1);
		// Right after boot the display is still settling
		{
			s32 l, r;
			for (size_t i = 0; i < size_t(2.0 * AUDIO_RATE); i++)
				eng.mu.run_sample(l, r);
		}
		// With MIDI, play it first so the level meters have something to show
		if (!a.shot_mid.empty()) {
			std::vector<smf::event> evs;
			std::string err;
			if (!smf::load(a.shot_mid, evs, err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
			} else {
				std::printf(CLI_T("Feeding %zu MIDI events, %.1f s of them\n", "MIDI %zu 件を %.1f 秒ぶん流す\n"), evs.size(), a.shot_secs);
				size_t at = 0;
				s32 l, r;
				for (size_t i = 0; i < size_t(a.shot_secs * AUDIO_RATE); i++) {
					const double now = double(i) / AUDIO_RATE;
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
		return write_shot(a.shot_path, a.win_w, a.win_h, br, a.grid,
		                  w.lcd_only, a.layout_path);
	}

	// Window-side setup both mains do once the machine object exists:
	// LCD/bar/panel sizing and layout, remembered volume and output,
	// clean or remembered boot
	void setup_for_window(const tool_args &a, const window_options &w, bool factory)
	{
		lcd_only = w.lcd_only;
		wire_send_out();
		panel.set_lcd_only(lcd_only);
		if (!lcd_only) {
			bar.set_items(window_bar_items(true));
			xgui::set_board_window(true);        // マスターの窓は、ボードの欄の代わりに窓を開くボタンを出す
			panel.set_top_inset(toolbar::HEIGHT);
		}
		panel.resize(a.win_w, a.win_h);
		layout_path = a.layout_path;
		apply_layout(a.layout_path, false);
		panel.resize(a.win_w, a.win_h);
		{
			// The VOLUME knob starts where it was left
			remembered r = load_remembered(settings_path(), !keep_settings);
			br.set_gain(r.volume);
			if (eng) {
				eng->analog.store(r.analog);
				eng->limit_output.store(r.limiter);
			}
			if (r.analog)
				std::printf(CLI_T("Sound output: analogue (DC removed)\n", "音の出口: アナログ（直流を切る）\n"));
			play.set_fold_extra_ports(r.fold34);
			play.set_thin_bends(r.thin_bends);
			// The imaginary plug-in board goes in before the firmware boots, so
			// that its "Checking PLG" finds it (the boot then skips the snapshot)
			if (eng && !r.board_dls.empty()) {
				std::string err;
				if (!eng->mu.load_board_dls(r.board_dls, err))
					std::fprintf(stderr, CLI_T("DLS board: %s (%s)\n", "DLS のボード: %s（%s）\n"), err.c_str(), r.board_dls.c_str());
			}
			if (eng && !r.board_file.empty()) {
				std::string err;
				if (const auto b = user_boards::adopt(r.board_file, err))
					eng->mu.set_user_board(b, r.board_file);
				else
					std::fprintf(stderr, CLI_T("Board file: %s (%s)\n", "ボードのファイル: %s（%s）\n"), err.c_str(), r.board_file.c_str());
			}
			// FC ボードの音色の値を動かすコントロールチェンジの番号（FC ボード全部に共通）
			fc_banks::adopt_cc(smu2000::vboard::fc_cc_parse(r.board_fc_cc));
			if (eng)
				eng->mu.set_fc_cc_map(fc_banks::cc_map());
			// FC ボードの音色の組は、ボードごと（PLG-1・2・3 と 16 パートのボード）
			for (int i = 0; eng && i < fc_banks::TARGETS; i++) {
				if (r.board_fc[i].empty())
					continue;
				std::string err;
				if (const auto b = fc_banks::adopt(i, r.board_fc[i], err))
					eng->mu.set_fc_bank(b, r.board_fc[i], i);
				else
					std::fprintf(stderr, CLI_T("FC voice set: %s (%s)\n", "FC ボードの音色の組: %s（%s）\n"), err.c_str(), r.board_fc[i].c_str());
			}
			if (eng && !r.board_fm.empty()) {
				std::string err;
				if (const auto b = fm_banks::adopt(r.board_fm, err))
					eng->mu.set_fm_bank(b, r.board_fm);
				else
					std::fprintf(stderr, CLI_T("FM voice set: %s (%s)\n", "FM ボードの音色の組: %s（%s）\n"), err.c_str(), r.board_fm.c_str());
			}
			if (eng && r.board)
				eng->mu.set_virtual_board(r.board, r.board_part - 1);
			for (int i = 0; i < mu2000::PLG_SLOTS - 1; i++)
				if (eng && r.board_more[i])
					eng->mu.set_virtual_board(r.board_more[i], r.board_more_part[i] - 1, i + 1);
		}
		// Only the window boots from remembered settings: --shot must give
		// the same picture every time
		if (eng)
			eng->use_nvram = !factory;
		if (factory)
			std::printf(CLI_T("Starting from factory defaults (the remembered settings are overwritten on exit)\n", "工場出荷状態で起動する（覚えていた設定は終わるときに上書きされる）\n"));
	}

	// Opens the remembered MIDI ports by name (--midi and friends win).
	// Reports the configured routing
	void open_remembered_ports(tool_args &a, const output_options &o)	{
		remembered want = load_remembered(settings_path(), !keep_settings);
		midi_routes = want.midi;
		edit_out_name = want.edit_out;
		ain_name = want.audio_in;
		ain_keep = want.audio_in;
		if (!want.card.empty())
			insert_card(want.card, true);
		// --audio wins; otherwise the port that was opened last time
		audio_name = o.audio_dev ? std::string(o.audio_dev) : want.audio_out;
		// --audio-in wins; otherwise the recording device from last time
		// (empty is off). Opened by start_ad once the firmware is up
		if (o.audio_in_dev)
			ain_name = o.audio_in_dev;
		const auto inputs = midi_in::list(), outputs = midi_out::list();
		const auto override_column = [](auto &routes, const auto &names, int column, int device) {
			if (device == -2) return; // no CLI override
			clear_midi_column(routes, column);
			if (device >= 0 && size_t(device) < names.size()) {
				const auto &name = names[size_t(device)];
				set_midi_route(routes, name, midi_route_mask(routes, name) | (1u << column));
			}
		};
		for (int p = 0; p < IN_PORTS; p++) override_column(midi_routes.inputs, inputs, p, a.in_dev[p]);
		override_column(midi_routes.outputs, outputs, 0, a.mout_dev);
		override_column(midi_routes.outputs, outputs, 1, a.moutb_dev);
		override_column(midi_routes.outputs, outputs, 2, a.moutmu_dev);
		std::string error;
		midi.apply(midi_routes_with_editor(midi_routes, edit_out_name), true, error);
		if (!error.empty()) std::fprintf(stderr, "MIDI: %s\n", error.c_str());
		for (int p = 0; p < IN_PORTS; p++) std::printf("%s: %s\n", in_label(p), midi_startup_status(midi_routes.inputs, p, false).c_str());
		for (int p = 0; p < 3; p++) std::printf("%s: %s\n", p == 2 ? "MIDI OUT" : p == 1 ? "MIDI THRU B" : "MIDI THRU A", midi_startup_status(midi_routes.outputs, p, true).c_str());
		std::fflush(stdout);
	}

	// Opens the --editor/--list-window and friends alongside the panel
	void open_startup_windows(const window_options &w)
	{
		if (w.open_settings && !w.lcd_only) open_window_by_kind(BAR_SETTINGS);
		if (w.open_editor && !w.lcd_only)
			open_window_by_kind(BAR_EDITOR);
		if (w.open_fx && !w.lcd_only)
			open_window_by_kind(BAR_FX);
		if (w.open_list && !w.lcd_only)
			open_window_by_kind(BAR_LIST);
		if (w.open_shapes && !w.lcd_only)
			open_window_by_kind(BAR_SHAPES);
		if (w.open_master && !w.lcd_only)
			open_window_by_kind(BAR_MASTER);
		if (w.open_sampling && !w.lcd_only)
			open_window_by_kind(BAR_SAMPLING);
		if (w.open_player && !w.lcd_only)
			open_window_by_kind(BAR_PLAYER);
		if (w.open_board && !w.lcd_only)
			open_window_by_kind(BAR_BOARD);
	}

	// Starts the audio device. False parks the engine on the failure and
	// asks main to return (the device line each side prints stays per side)
	bool start_audio()
	{
		if (!out || !eng)
			return false;
		std::string err;
		audio_output_config config{audio_name, audio_settings};
		if (!start_audio_stream(*out, [this](s16 *o, u32 n) { eng->fill(o, n); }, config, err, false)) {
			std::fprintf(stderr, CLI_T("Audio: %s\n", "音声: %s\n"), err.c_str());
			audio_error = err;
			report_audio_failure();
			return false;
		}
		audio_settings = config.preferences;
		if (!err.empty()) std::fprintf(stderr, CLI_T("Audio: %s\n", "音声: %s\n"), err.c_str());
		if (audio_settings.exclusive && !out->exclusive())
			std::fprintf(stderr, CLI_T("Could not open in exclusive mode (falling back to shared)\n", "独り占めで開けなかった（共有に落とす）\n"));
#if defined(__APPLE__)
		// The parallel slave thread joins the output unit's audio workgroup
		// from here (Apple's parallel real-time threads pattern; the join
		// itself is in compat/realtime.h), and a null group leaves the thread
		// outside any workgroup. Only macOS has a group to hand over, so only it
		// asks.
		eng->mu.set_realtime_workgroup(out->realtime_workgroup());
#endif
		return true;
	}

	// Opens the remembered recording device, by name. A device that is not
	// there now keeps its name in the settings, like a MIDI port
	void start_ad()
	{
		if (ain_name.empty() || !ain)
			return;
		const auto names = audio_in::list();
		const int dev = find_device(names, ain_name);
		std::string aerr;
		if (dev >= 0 && ain->start(names[size_t(dev)], aerr)) {
			attach_ain(true);
			std::printf(CLI_T("A/D INPUT: %s (%s)\n", "A/D INPUT: %s（%s）\n"), ain->device_name().c_str(),
			            ain->format_line().c_str());
		} else {
			attach_ain(false);
			std::printf(CLI_T("A/D INPUT: none (%s)\n", "A/D INPUT: なし（%s）\n"),
			            dev < 0 ? CLI_T("device not found", "デバイスが見つからない") : aerr.c_str());
		}
		std::fflush(stdout);
		save_settings();
	}

	// Everything after the pump returns, both sides in this order: tell the
	// editor windows, drain the audio thread, all-notes-off the THRU ports,
	// keep the card and the settings, snapshot for the next boot, close up.
	// The boot thread join stays in main (it owns the thread)
	// Everything that has to happen while the process is still whole: the audio
	// thread stopped, the ports closed, the card flushed, the settings written.
	//
	// Public and idempotent because not every front end gets to run()'s
	// epilogue: AppKit's event loop ([NSApp run], window_mac.mm's run_window)
	// never returns, so closing the window there goes through terminate: and
	// exit(), which destroys statics without ever coming back here. The macOS
	// delegate therefore calls this from applicationShouldTerminate:, and the
	// flag keeps the epilogue's own call from doing the work twice - a second
	// join() on a spent thread would throw.
	void shutdown()
	{
		if (m_shut)
			return;
		m_shut = true;
		if (const auto result = midi_job.join()) finish_midi_change(*result);
		if (const auto result = audio_job.join()) finish_audio_change(*result);
		pc_shutdown_all(list, pc, fx, shapes, master, sampling, br);
		{
			ImGuiContext *const panel_ctx = ImGui::GetCurrentContext();
			player_win.shutdown(br);
			board_win.shutdown(br);
			settings_win.shutdown(br);
			ImGui::SetCurrentContext(panel_ctx);
		}
		if (m_ain_lister.joinable())
			m_ain_lister.join();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		// Stop a MIDI file first: its all-notes-off travels out through the
		// audio thread, so stopping the sound first would leave the far-end
		// gear ringing. Then give the thread a breath to flush it through
		if (play.playing()) {
			play.stop();
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
		}
		if (out)
			out->stop();
		if (ain)
			ain->stop();
		// Leaving the THRU ports open with notes still held would leave them
		// stuck on whatever is listening, so all sound off and all notes off
		// go out first
		for (int source = 0; source < 2; source++) for (int ch = 0; ch < 16; ch++)
			for (u8 v : { u8(0xb0 | ch), u8(120), u8(0), u8(0xb0 | ch), u8(123), u8(0) }) midi.send(source, v);
		join_reboot();
		flush_card();      // the sound has stopped; keep what was on the card
		save_settings();   // the ports, the A/D input and the VOLUME knob
		// The sound has stopped by now. Keep the machine's settings only if
		// it came up
		if (eng) {
			eng->settle_for_save();
			if ((eng->state.load() == 1 || (audio_ready.load() && (eng->state.load() == 0 || audio_failed))) && !smu2000::nvram::save(eng->mu))
				std::fprintf(stderr, CLI_T("Could not save the settings: %s\n", "設定を残せなかった: %s\n"),
				             smu2000::nvram::path(eng->mu).c_str());
			// Snapshot for the settings just saved, or the next boot after
			// touching them is the slow one. The window is gone, so the
			// second it takes holds nobody up; old snapshots are pruned here
			if (eng->state.load() == 1 || (audio_ready.load() && (eng->state.load() == 0 || audio_failed))) {
				if (smu2000::bootcache::refresh(eng->mu))
					std::printf(CLI_T("Made the boot copy for the next start\n", "次の起動ぶんの写しを作った\n"));
				smu2000::bootcache::prune();
			}
		}
		play.stop();
		midi.close();
	}

	// How the run ended up sounding, when it sounded at all. drops is what
	// the backend counted (WASAPI late(), CoreAudio starved())
	void print_exit_stats(u64 drops)
	{
		if (audio_ready.load() && !audio_job.busy() && out && out->produced()) {
			std::printf(CLI_T("CPU %.1f%%, worst single block %.2f ms, late %llu times\n", "CPU %.1f%%、1 回の最悪 %.2f ms、間に合わなかった %llu 回\n"),
			            out->cpu_percent(), out->worst_ms(),
			            (unsigned long long)drops);
			print_audio_details();
		}
	}

	// Backend details for the exit line (device format on WASAPI,
	// hog mode on CoreAudio). Nothing shared to say: each side says its own
	virtual void print_audio_details() = 0;

	// ---- the program itself (both mains end here)

	// iOS has its own event loop, but shares the saved audio startup choices.
	remembered initialize_audio_settings(const tool_args &a, const output_options &oo)
	{
		keep_settings = a.nomidi;
		const remembered saved = load_remembered(settings_path(), !keep_settings);
		persisted_audio = {saved.audio_out, saved.audio};
		persisted_native_fx = saved.native_fx; persisted_native_engine = saved.native_engine;
		audio_settings = saved.audio;
		if (a.latency_given) audio_settings.latency_ms = a.latency;
		if (oo.exclusive) audio_settings.exclusive = true;
		audio_settings.stream.strict = false;
		audio_routes = saved.audio_routes;
		startup_song = a.play_path;
		return saved;
	}

	// Put up the window, boot, pump events, and close down.
	int run(tool_args &a, const engine_options &eo, const output_options &oo,
	        const window_options &wo)
	{
		const remembered saved = initialize_audio_settings(a, oo);
		setup_for_window(a, wo, oo.factory);

		// The remembered ports open on this thread, while the machine boots
		// beside it: the audio device is the only thing that needs the
		// firmware. --midi and friends win over what was remembered
		open_remembered_ports(a, oo);

		// Give the panel something to read before the boot thread says
		// anything, so the window comes up showing the boot message rather
		// than a blank LCD
		eng->publish();

		if (!open_main_window("S-MU2000", a.win_w, a.win_h)) {
			std::fprintf(stderr, "%s\n", UI_TEXT(dlg_window_fail, "Cannot open the window"));
			return 1;
		}

		make_audio();

		// Boot on a separate thread, and start the audio once it is done
		std::thread boot_thread([&] {
			if (!eng->boot()) {
				eng->state.store(2);
				eng->publish();
				return;
			}
			// After boot, as always: starting needs the firmware
			engine_options startup = eo;
			if (!startup.native_fx) startup.native_fx = saved.native_fx;
			if (!startup.native_engine) startup.native_engine = saved.native_engine;
			if (startup.native_fx) { eng->mu.set_native_fx(startup.native_fx); eng->native_fx.store(startup.native_fx); }
			apply_native_engine(*eng, startup);
			eng->state.store(1);
			eng->publish();

			if (!start_audio()) {
				audio_failed = true;
				audio_ready.store(true); // PHONES can recover by picking another output
				return;
			}
			say_audio_opened(audio_settings.exclusive);
			audio_ready.store(true); // all startup writes finish before UI switching
		});

		// --editor and friends, alongside the panel
		open_startup_windows(wo);

		pump_window("S-MU2000", a.win_w, a.win_h);

		if (boot_thread.joinable())
			boot_thread.join();
		shutdown();
		print_exit_stats(audio_drops());
		return 0;
	}

	// ---- the window-system shell (thin shells implement these)

	// Creates the main window and shows it. False when it could not be made
	virtual bool open_main_window(const char *title, int w, int h) = 0;
	// Pumps events until the window closes. Blocks. The Mac's make and pump
	// are one call (run_window), so the arguments ride along unused there
	virtual void pump_window(const char *title, int w, int h) = 0;
	// Creates the audio devices and points out/ain at them. Opening the
	// device itself waits for the firmware (start_audio, on the boot thread)
	virtual void make_audio() = 0;
	// Said once the device is up. exclusive is whether hog mode was asked for
	virtual void say_audio_opened(bool exclusive) = 0;
	// Said once it is actually sounding (latency, thread class)
	virtual void say_audio_running() = 0;
	// What the backend counted (WASAPI late(), CoreAudio starved())
	virtual u64 audio_drops() = 0;

	// ---- per-platform acts (thin shells implement these)

	// Something in a menu failed. Windows remembers it for the end of the
	// command; macOS tells the user straight away
	virtual void menu_error(const std::string &text) = 0;
	// Something worth saying that is not a failure (fresh card needs Format)
	virtual void menu_note(const std::string &text) = 0;
	// File dialogs ("" means cancelled)
	virtual std::string ask_card_open_path() = 0;
	virtual std::string ask_card_save_path() = 0;
	virtual std::string ask_midi_file_path() = 0;
	// Factory reset confirmation (false keeps everything)
	virtual bool confirm_factory_reset() = 0;

protected:
	u64 last_flush = 0;                // card file last written back
	u64 last_drop_report = 0;          // MIDI drops last said out loud
	bool pressed = false;            // a panel press is in flight (drag/up)
};

// --shot without a ROM or without booting: draw the empty screen. Both
// mains do this before anything else is built
inline int empty_shot(bridge &b, const tool_args &a, const window_options &w)
{
	snapshot s;
	std::snprintf(s.message, sizeof(s.message), "S-MU2000");
	b.publish(s);
	return write_shot(a.shot_path, a.win_w, a.win_h, b, a.grid,
	                  w.lcd_only, a.layout_path);
}

} // namespace ui

#endif // S_MU2000_UI_APP_H
