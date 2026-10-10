// license:BSD-3-Clause
// Exercises processing, persistence and the actual ImGui settings widgets.
#include "ui/settings.h"
#include "ui/font_file.h"
#include "ui/settings_view.h"
#include "ui/midi_commands.h"
#include "ui/output_limiter.h"
#include "ui/audio_output_switch.h"
#include "ui/audio_device_watch.h"
#include "imgui_internal.h"
#include <iostream>
#include <map>
#include <stdexcept>

static void require(bool ok, const char *why)
{
	if (!ok) throw std::runtime_error(why);
}

struct item { ImRect rect; ImGuiWindow *window; bool disabled; };
static std::map<ImGuiID, ImRect> rectangles;
static std::map<std::string, item> items;
void ImGuiTestEngineHook_ItemAdd(ImGuiContext *ctx, ImGuiID id, const ImRect &rect, const ImGuiLastItemData *data)
{
	rectangles[id] = data ? data->NavRect : rect;
	if (!ctx->CurrentWindow || ctx->CurrentWindow->IDStack.empty()) return;
	// BeginCombo supplies ItemAdd but does not emit an ItemInfo hook.
	for (const char *label : {"Resampler", "Driver", "Language", "言語", "Stream sample rate", "Left output channel", "Right output channel", "Requested buffer/period (frames)", "##device"})
		if (id && id == ctx->CurrentWindow->GetID(label[0] == '#' ? label : (std::string("##") + label).c_str()))
			items.try_emplace(label, item{rectangles[id], ctx->CurrentWindow, bool(ctx->LastItemData.ItemFlags & ImGuiItemFlags_Disabled)});
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext *ctx, ImGuiID id, const char *label, ImGuiItemStatusFlags)
{
	items.try_emplace(label, item{rectangles[id], ctx->CurrentWindow, bool(ctx->LastItemData.ItemFlags & ImGuiItemFlags_Disabled)});
}
void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}
const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID) { return nullptr; }

static void processing()
{
	ui::output_limiter limiter;
	double l = 2, r = 1;
	limiter.process(l, r, true);
	require(std::abs(l - 0.98) < 1e-9 && std::abs(r - 0.49) < 1e-9, "Limiter attack/stereo linking");
	for (int i = 0; i < 44100; i++) { l = r = 0.5; limiter.process(l, r, true); }
	require(l > 0.4999, "Limiter release");
	l = 2; r = -1; limiter.process(l, r, false);
	require(l == 2 && r == -1, "Limiter bypass must preserve samples");
	for (auto quality : {ui::resampler_quality::sinc, ui::resampler_quality::linear, ui::resampler_quality::nearest})
	for (int rate : {8000, 44100, 48000, 96000, 192000}) {
		ui::audio_stream_renderer renderer;
		renderer.configure(rate, quality);
		u64 generated = 0;
		std::vector<s16> output(size_t(rate) * 6);
		renderer.render(output.data(), unsigned(rate), 6, 4, 2,
			[&](s16 *dst, unsigned n) {
				for (unsigned i = 0; i < n; i++, generated++) {
					dst[i * 2] = s16(10000 * std::sin(2 * 3.141592653589793 * 440 * generated / 44100));
					dst[i * 2 + 1] = -dst[i * 2];
				}
			}, ui::audio_stream_renderer::pcm16);
		unsigned crossings = 0;
		for (int i = 1; i < rate; i++) {
			for (int ch : {0, 1, 3, 5}) require(output[size_t(i) * 6 + ch] == 0, "Unused channel contains audio");
			require(std::abs(int(output[size_t(i) * 6 + 4]) + output[size_t(i) * 6 + 2]) <= 1, "Stereo routing");
			if (output[size_t(i - 1) * 6 + 4] <= 0 && output[size_t(i) * 6 + 4] > 0) crossings++;
		}
		require(crossings >= 439 && crossings <= 441, "Resampling changed pitch");
		require(generated > 44090 && generated < 44180, "Resampling advanced the MU clock incorrectly");
	}
}

static void persistence()
{
	ui::remembered in, out;
	in.audio_out = "USB = audio";
	in.midi = {{{"Keyboard", 3}, {"Pads", 17}}, {{"Synth", 5}, {"Recorder", 4}}};
	in.audio.exclusive = true;
	in.audio.stream = {96000, 256, 4, 5};
	in.audio_routes = {{"USB = audio", 4, 5}, {"HDMI", 1, 0}};
	in.audio.stream.quality = ui::resampler_quality::linear;
	in.limiter = true; in.native_fx = 2; in.native_engine = 1;
	ui::apply_settings(ui::collect_settings(in), out);
	require(out.audio == in.audio && out.audio_out == in.audio_out && out.limiter && out.native_fx == 2 && out.native_engine == 1, "Audio preferences round trip");
	require(out.midi == in.midi, "MIDI routing round trip");
	ui::remembered legacy;
	ui::apply_settings({{"midi_in", "Keyboard"}, {"midi_in_b", "Keyboard"}, {"midi_out", "Synth"}, {"midi_out_mu", "Synth"}}, legacy);
	require(ui::midi_route_mask(legacy.midi.inputs, "Keyboard") == 3 && ui::midi_route_mask(legacy.midi.outputs, "Synth") == 5, "Legacy MIDI routing migration");
	require(out.audio_routes.size() == 2 && out.audio_routes[0].device == in.audio_out && out.audio_routes[0].left == 4 && out.audio_routes[1].right == 0, "Per-device routing round trip");
	in.board = 1; in.board_part = 17; in.board_more[0] = 2;
	in.card = "prepared-card.img"; in.board_file = "voices.ini"; in.board_fc[0] = "fc.ini";
	in.board_fc_cc = "custom mappings";
	in.audio_in = "Microphone"; in.in[0] = "Keyboard"; in.edit_out = "Synth";
	in.volume = 0.4f; in.analog = true; in.thin_bends = true; in.fold34 = false;
	ui::apply_settings(ui::collect_settings(in), out);
	ui::clear_processing_settings(out);
	require(out.audio == ui::audio_preferences{} && out.audio_routes.empty() && !out.limiter && !out.native_fx && !out.native_engine,
	        "--nomidi kept saved processing choices");
	require(out.board == 1 && out.board_part == 17 && out.board_more[0] == 2 && out.card == in.card &&
	        out.board_file == in.board_file && out.board_fc[0] == in.board_fc[0] && out.board_fc_cc == in.board_fc_cc &&
	        out.audio_out == in.audio_out && out.audio_in == in.audio_in && out.in[0] == in.in[0] && out.edit_out == in.edit_out &&
	        out.volume == in.volume && out.analog && out.thin_bends && !out.fold34,
	        "--nomidi discarded existing profile settings");
	ui::remembered invalid;
	ui::apply_settings({{"audio_rate", "48000garbage"}, {"audio_buffer", "-1"}, {"audio_left", "3"}, {"audio_right", "3"}}, invalid);
	require(ui::valid_audio_request(invalid.audio.stream) && invalid.audio.stream.sample_rate == 0 && invalid.audio.stream.left == 0, "Malformed settings validation");
	ui::remembered corrupt;
	ui::apply_settings({{"audio_latency", "broken"}}, corrupt);
	require(corrupt.audio.latency_ms == ui::audio_preferences{}.latency_ms, "Corrupt latency lost the platform default");
	ui::audio_output_config saved, runtime;
	runtime.preferences.exclusive = true; runtime.preferences.latency_ms = 5;
	auto edited = runtime; edited.preferences.stream.quality = ui::resampler_quality::linear;
	ui::remember_audio_change(saved, runtime, edited);
	require(!saved.preferences.exclusive && saved.preferences.latency_ms == ui::audio_preferences{}.latency_ms &&
	        saved.preferences.stream.quality == ui::resampler_quality::linear, "Audio edit saved unrelated CLI overrides");
	runtime = edited; edited.preferences.latency_ms = 40;
	ui::remember_audio_change(saved, runtime, edited);
	require(saved.preferences.latency_ms == 40, "Explicit latency edit was not saved");
	saved.preferences.stream = {96000, 1024, 6, 7};
	runtime.preferences.stream = {};
	edited = runtime; edited.device = "New stereo output";
	ui::remember_audio_change(saved, runtime, edited);
	require(saved.device == edited.device && saved.preferences.stream.sample_rate == 0 &&
	        saved.preferences.stream.buffer_frames == 0 && saved.preferences.stream.left == 0 && saved.preferences.stream.right == 1,
	        "Device change retained an obsolete saved format after recovery");
	saved.preferences.stream = {96000, 1024, 6, 7};
	runtime = edited; edited.preferences.stream.sample_rate = 48000;
	ui::remember_audio_change(saved, runtime, edited);
	require(saved.preferences.stream.sample_rate == 48000 && saved.preferences.stream.buffer_frames == 0 &&
	        saved.preferences.stream.left == 0 && saved.preferences.stream.right == 1,
	        "Rate change retained an obsolete saved channel route");
	const auto general = ui::menu_ports({});
	bool settings = false, list = false, editor = false;
	for (const auto &group : general) for (const auto &entry : group.items) {
		settings |= entry.id == ui::ID_SETTINGS;
		list |= entry.id == ui::ID_OVERVIEW && entry.shortcut == "F3";
		editor |= entry.id == ui::ID_PC_EDITOR && entry.shortcut == "F2";
		require(entry.id != ui::ID_NATIVE_FX && entry.id != ui::ID_NATIVE_ENGINE, "Redundant general menu entries");
	}
	require(settings && list && editor, "General menu lost editors or settings");
	ui::menu_state menu; menu.ready = menu.audio_ready = menu.thin_bends = menu.limiter = true;
	menu.audio_rates = {44100, 48000};
	bool thin = false, reset = false, factory = false, rate = false, peaks = false;
	for (const auto &g : ui::menu_midi(menu)) for (const auto &e : g.items) {
		require(e.id != ui::ID_THIN_BENDS, "File-player lightening appeared in MIDI input menu");
		reset |= e.id == ui::ID_RESET_XG && e.enabled;
	}
	for (const auto &g : ui::menu_card(menu)) for (const auto &e : g.items)
		thin |= e.id == ui::ID_THIN_BENDS && e.checked && e.label.find("unlike the real unit") != std::string::npos;
	for (const auto &g : ui::menu_power(menu)) for (const auto &e : g.items) factory |= e.id == ui::ID_FACTORY;
	for (const auto &g : ui::menu_phones(menu)) for (const auto &e : g.items) {
		rate |= e.id == ui::ID_RATE_BASE + 1 && e.label == "48000 Hz";
		peaks |= e.id == ui::ID_OUTPUT_LIMITER && e.checked;
	}
	require(thin && reset && factory && rate && peaks, "Quick menu control placement");
	const auto phones = ui::menu_phones(menu);
	const auto &output = phones[2].items;
	require(output.size() == 4 && output[2].separator && output[3].id == ui::ID_OUTPUT_LIMITER, "Peak limiter lacks a separator");
	const auto input = ui::menu_ain_only({}, "");
	require(input[0].items[0].label == "A/D INPUT (sound to sample)" && !input[0].items[0].enabled, "A/D INPUT heading missing");
	for (const auto &groups : {general, ui::menu_midi(menu), ui::menu_card(menu), ui::menu_phones(menu), ui::menu_power(menu), ui::menu_ain_only({}, "")}) {
		const auto &last = groups.back();
		require(last.title.empty() && last.items.size() == 2 && last.items[0].separator
			&& last.items[1].id == ui::ID_SETTINGS && last.items[1].enabled, "Quick menu lacks a separated Settings shortcut");
	}
}

struct fake_output {
	ui::audio_stream_options stream;
	int opens = 0;
	bool shared = false, fail = false;
	void stop() {}
	void set_stream_options(ui::audio_stream_options s) { stream = s; }
	template <typename Fill> bool start(int, Fill, std::string &error, bool exclusive, const std::string &, bool, bool)
	{
		opens++;
		if (fail || ui::custom_audio_format(stream) || stream.buffer_frames || (exclusive && stream.strict)) {
			error = "Unsupported format or access mode"; return false;
		}
		shared = exclusive; return true;
	}
};

static void startup_and_recovery()
{
	const auto fill = [](s16 *, u32) {};
	std::string error;
	fake_output out;
	ui::audio_output_config config;
	config.preferences.exclusive = true;
	require(ui::start_audio_stream(out, fill, config, error) && out.shared && out.opens == 1,
	        "Startup blocked exclusive-to-shared fallback");
	config.preferences.stream = {96000, 256, 4, 5}; out.opens = 0;
	require(ui::start_audio_stream(out, fill, config, error) && !ui::custom_audio_format(config.preferences.stream) && out.opens == 2,
	        "Obsolete saved format did not fall back to Auto");
	auto bad = config; bad.preferences.stream.sample_rate = 96000; bad.preferences.stream.strict = true;
	config.preferences.exclusive = false; out.opens = 0;
	const auto result = ui::switch_audio_output(out, fill, bad, config);
	require(!result.selected && result.restored && out.opens == 2, "Explicit edit did not fail and restore its prior stream");
	out.fail = true; out.opens = 0;
	const auto lost = ui::switch_audio_output(out, fill, config, config, false);
	require(!lost.selected && !lost.restored && out.opens == 1, "Lost-device recovery reopened the same failed stream twice");
	ui::audio_device_watch devices;
	require(devices.changed({"Speakers"}, "Speakers"), "First device snapshot was ignored");
	for (int i = 0; i < 100; i++)
		require(!devices.changed({"Speakers"}, "Speakers"), "Unchanged devices triggered endless retries");
	require(devices.changed({"Speakers", "USB"}, "USB") && !devices.changed({"Speakers", "USB"}, "USB"), "Device change did not permit exactly one retry");
	require(!ui::custom_audio_format({}) && ui::custom_audio_format({48000, 0, 0, 1}) && ui::custom_audio_format({0, 0, 2, 3}),
	        "CoreAudio Auto/default no longer selects the original path");
	require(ui::valid_audio_route({}, 1) && !ui::valid_audio_route({0, 0, 2, 3}, 1), "Mono output route validation");
	ui::audio_stream_renderer renderer; renderer.configure(44100);
	std::array<s16, 8> mono;
	renderer.render(mono.data(), 8, 1, 0, 1, [](s16 *dst, u32 n) {
		for (u32 i = 0; i < n; i++) { dst[i * 2] = 10000; dst[i * 2 + 1] = -20000; }
	}, ui::audio_stream_renderer::pcm16);
	for (s16 sample : mono) require(sample == 10000, "Mono output did not keep the left channel");
	require(ui::audio_stream_renderer::pcm16_truncate(10000.0f / 32768) == 9999, "WASAPI 16-bit conversion changed");
}

static void lazy_fonts()
{
	ImGui::CreateContext();
	auto *atlas = ImGui::GetIO().Fonts;
	ImFontConfig cfg; cfg.SizePixels = 16;
	auto *primary = atlas->AddFontDefaultVector(&cfg);
	cfg.MergeMode = true; cfg.GlyphRanges = cjk_fullwidth_ranges;
	atlas->AddFontDefaultVector(&cfg);
	cfg.MergeMode = false; cfg.GlyphRanges = cjk_english_ranges;
	auto *other = atlas->AddFontDefaultVector(&cfg);
	const int english_sources = atlas->Sources.Size;
	ensure_cjk_ui_fonts(atlas);
	require(atlas->Sources.Size == english_sources, "English startup loaded Japanese ranges");
	ui::set_lang(ui::lang::ja);
	ensure_cjk_ui_fonts(atlas);
	require(atlas->Sources.Size == english_sources + 2 && primary->Sources.Size == 3 && other->Sources.Size == 2,
		"Language change did not merge Japanese into the original UI font");
	ensure_cjk_ui_fonts(atlas);
	ui::set_lang(ui::lang::en); ensure_cjk_ui_fonts(atlas);
	ui::set_lang(ui::lang::ja); ensure_cjk_ui_fonts(atlas);
	require(atlas->Sources.Size == english_sources + 2, "Language changes loaded duplicate fonts");
	atlas->Build();
	ImGui::DestroyContext();
	ui::set_lang(ui::lang::en);
}

static void interface()
{
	ImGui::CreateContext();
	auto &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2(760, 560);
	io.DeltaTime = 1.0f / 60;
	io.Fonts->Build();
	GImGui->TestEngineHookItems = true;
	ui::settings_state state;
	state.ready = state.connected = true;
	state.outputs = {"Speakers", "USB"};
	state.midi_inputs = {"Keyboard", "Pads"};
	state.midi_outputs = {"Synth", "Recorder"};
	state.stream = {{44100, 48000}, {"Output 1", "Output 2", "Output 3", "Output 4"}, 44100};
	ui::audio_output_config applied;
	int calls = 0, command = 0;
	ui::settings_actions actions;
	actions.audio = [&](auto config) { applied = config; calls++; };
	actions.language = [](int value) { ui::set_lang(ui::lang(value)); };
	actions.command = [&](int id) { command = id; };
	actions.input = [](auto) {};
	actions.midi = [&](ui::midi_routing routes) { state.midi = std::move(routes); };
	int volume_updates = 0, volume_saves = 0;
	actions.volume = [&](float gain) { state.gain = gain; volume_updates++; };
	actions.save_volume = [&] { volume_saves++; };
	actions.limiter = [](bool) {};
	ui::settings_view view(state, actions);
	xg::model model;
	ui::xg_snapshot snapshot;
	ui::bridge bridge;
	const auto frame = [&] {
		items.clear(); rectangles.clear();
		ImGui::NewFrame(); view.draw(model, snapshot, bridge); ImGui::Render();
		require(ImGui::GetDrawData()->TotalVtxCount > 0, "Settings window produced no drawing");
	};
	frame(); frame();
	const auto click = [&](const std::string &label) {
		require(items.contains(label), ("Missing widget: " + label).c_str());
		auto target = items.at(label);
		if (!target.window->ClipRect.Contains(target.rect.GetCenter())) {
			ImGui::SetScrollY(target.window, target.window->Scroll.y + target.rect.Min.y - target.window->ClipRect.Min.y - 20);
			frame(); target = items.at(label);
		}
		const auto center = target.rect.GetCenter();
		io.AddMousePosEvent(center.x, center.y); frame();
		io.AddMouseButtonEvent(0, true); frame();
		io.AddMouseButtonEvent(0, false); frame();
		frame();
	};
	click("Stream sample rate"); click("48000 Hz");
	require(calls == 1 && applied.preferences.stream.sample_rate == 48000, "Rate selection did not reach controller");
	require(!items.contains("Apply audio settings") && !items.contains("Revert"), "Manual audio apply controls remained");
	state.busy = true; frame(); click("Stream sample rate");
	require(calls == 1, "Audio controls remain active during reopen");
	state.busy = false; state.audio = applied; frame();
	click("Stream sample rate"); click("44100 Hz");
	require(calls == 2, "Audio edits did not apply immediately");
	state.busy = true; frame(); state.busy = false; state.error = "Unsupported format"; frame();
	click("Stream sample rate"); click("48000 Hz");
	require(calls == 2, "Failed audio edit did not restore the displayed selection");
	click("Resampler"); click("Linear");
	require(calls == 3 && applied.preferences.stream.quality == ui::resampler_quality::linear, "Resampler selection did not apply immediately");
	state.audio = applied; frame();
	for (const auto &[id, expected] : std::vector<std::pair<int, std::vector<u8>>>{
	     {ui::ID_RESET_GM, {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7}},
	     {ui::ID_RESET_GS, {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0, 0x7f, 0, 0x41, 0xf7}},
	     {ui::ID_RESET_XG, {0xf0, 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0, 0xf7}}}) {
		ui::send_midi_command(id, bridge);
		std::vector<u8> reset; u8 byte;
		require(!bridge.take_midi(byte), "Mode reset reached MIDI THRU");
		while (bridge.take_ask(byte)) reset.push_back(byte);
		require(reset == expected, "Reset command emitted incorrect internal MIDI");
	}
	ui::send_midi_command(ui::ID_MIDI_PANIC, bridge);
	for (int port = 0; port <= mu2000::MIDI_PORTS; port++) {
		std::vector<u8> expected, actual; u8 byte;
		for (int ch = 0; ch < 16; ch++)
			for (u8 value : {u8(0xb0 | ch), u8(120), u8(0), u8(0xb0 | ch), u8(123), u8(0)}) expected.push_back(value);
		while (port == 0 ? bridge.take_midi(byte) : bridge.take_midi_port(port, byte)) actual.push_back(byte);
		require(actual == expected, "Panic did not silence every channel and port");
	}
	click("MIDI");
	require(state.page == ui::settings_page::midi, "MIDI page navigation");
	require(!items.contains("XG"), "MIDI reset remained in Settings");
	click("A##Keyboard"); click("B##Keyboard"); click("A##Pads");
	require(ui::midi_route_mask(state.midi.inputs, "Keyboard") == 3 && ui::midi_route_mask(state.midi.inputs, "Pads") == 1, "Input matrix lost multiple selections");
	click("THRU A##Synth"); click("MU OUT##Synth"); click("MU OUT##Recorder");
	require(ui::midi_route_mask(state.midi.outputs, "Synth") == 5 && ui::midi_route_mask(state.midi.outputs, "Recorder") == 4, "Output matrix lost multiple selections");
	state.busy = true; frame(); click("A##Keyboard");
	require(ui::midi_route_mask(state.midi.inputs, "Keyboard") == 3, "Routing controls active during reconfiguration");
	state.busy = false; state.midi_inputs = {"Pads", "New controller"}; frame(); frame();
	require(items.contains("A##New controller") && items.contains("B##Keyboard"), "Device refresh lost remembered routes or new devices");
	click("A##Keyboard"); click("B##Keyboard");
	require(ui::midi_route_mask(state.midi.inputs, "Keyboard") == 0, "Disconnected route could not be disabled");
	ui::menu_state quick; quick.ready = true; quick.midi = state.midi; quick.midi_ins = state.midi_inputs; quick.midi_outs = state.midi_outputs;
	const auto menus = ui::menu_midi(quick);
	const auto checked = [&](int id) {
		for (const auto &group : menus) for (const auto &entry : group.items) if (entry.id == id) return entry.checked;
		return false;
	};
	require(checked(ui::ID_IN_BASE) && checked(ui::ID_OUT_BASE) && checked(ui::ID_OUTMU_BASE)
		&& checked(ui::ID_OUTMU_BASE + 1), "Quick menu does not reflect routing matrix");
	click("Emulation");
	for (const char *label : {"Play effects in C++", "Lighten heavy MIDI", "Play without the firmware"}) {
		const auto center = items.at(label).rect.GetCenter();
		io.AddMousePosEvent(center.x, center.y);
		for (int i = 0; i < 45; i++) frame();
		bool tooltip = false;
		for (const auto *window : GImGui->Windows)
			tooltip |= (window->Flags & ImGuiWindowFlags_Tooltip) && window->Active && !window->Hidden;
		require(tooltip, (std::string("No hover help for: ") + label).c_str());
	}
	click("Play effects in C++");
	require(command == ui::ID_NATIVE_FX, "Emulation setting did not reach controller");
	click("Lighten heavy MIDI");
	require(command == ui::ID_THIN_BENDS, "MIDI lightening setting did not reach controller");
	click("General"); click("Language"); click("日本語");
	require(ui::get_lang() == ui::lang::ja && items.contains("一般"), "General language selection did not translate the interface");
	click("言語"); click("English");
	require(ui::get_lang() == ui::lang::en, "English language selection");
	click("Audio"); frame();
	// Volume changes sound during a drag, but persistence happens once on release.
	auto volume = items.at("##volume");
	ImGui::SetScrollY(volume.window, volume.window->Scroll.y + volume.rect.Min.y - volume.window->ClipRect.Min.y - 20);
	frame(); volume = items.at("##volume");
	const auto volume_center = volume.rect.GetCenter();
	io.AddMousePosEvent(volume_center.x, volume_center.y); frame();
	io.AddMouseButtonEvent(0, true); frame();
	for (int i = 0; i < 5; i++) { io.AddMousePosEvent(volume_center.x + i * 12, volume_center.y); frame(); }
	require(volume_updates > 1 && volume_saves == 0, "Volume drag did not update live or saved before release");
	io.AddMouseButtonEvent(0, false); frame(); frame();
	require(volume_saves == 1, "Volume was not saved exactly once on release");
	state.stream.manual_buffer = false; state.stream.channels.clear(); frame();
	require(items.at("Requested buffer/period (frames)").disabled && items.at("Left output channel").disabled &&
	        items.at("Right output channel").disabled, "System-controlled format offers unavailable routing or buffer controls");
	state.stream.manual_buffer = true; state.stream.channels = {"Output 1", "Output 2"}; frame();
	// Refreshing the list removes disconnected endpoints from the actual popup.
	state.outputs = {"HDMI"}; click("##device");
	require(items.contains("HDMI") && !items.contains("USB") && !items.contains("Speakers"), "Device popup did not refresh");
	ImGui::DestroyContext();
}

int main()
{
	try {
		ui::init_lang("en");
		processing(); persistence(); startup_and_recovery(); lazy_fonts(); interface();
		std::cout << "Settings processing, persistence and ImGui interactions: PASS\n";
	} catch (const std::exception &e) {
		std::cerr << e.what() << '\n'; return 1;
	}
}
