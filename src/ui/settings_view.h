// license:BSD-3-Clause
#pragma once

#include "audio_preferences.h"
#include "menu.h"
#include "midi_routing_view.h"
#include "xg_ui.h"
#include "imgui.h"
#include <array>
#include <functional>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ui {
enum class settings_page { general, audio, midi, emulation };

struct settings_state {
	settings_page page = settings_page::audio;
	audio_output_config audio;
	audio_stream_info stream;
	std::vector<std::string> outputs, inputs, midi_inputs, midi_outputs;
	midi_routing midi;
	std::string midi_error;
	std::string input, error;
	bool ready = false, busy = false, connected = false;
	bool analog = false, limiter = false, native_fx = false, native_engine = false, thin_bends = false;
	float gain = 1;
};

struct settings_actions {
	std::function<void(audio_output_config)> audio;
	std::function<void(std::string)> input;
	std::function<void(midi_routing)> midi;
	std::function<void(int)> command, language;
	std::function<void(float)> volume;
	std::function<void()> save_volume;
	std::function<void(bool)> limiter;
};

// Standalone-only, like player_view. Rendering never opens a device or a
// dialog: actions go to the app's deferred/controller paths.
class settings_view : public imgui_view {
public:
	settings_view(settings_state &s, settings_actions &a) : m_state(s), m_actions(a) {}
	const wchar_t *title() const override
	{
		return get_lang() == lang::ja ? L"S-MU2000 設定" : L"S-MU2000 Settings";
	}
	int default_width() const override { return 760; }
	int default_height() const override { return 560; }
	void draw(xg::model &, const xg_snapshot &, bridge &) override
	{
		if (!m_initialized || m_observed != m_state.audio || (m_was_busy && !m_state.busy)) {
			m_draft = m_observed = m_state.audio;
			m_initialized = true;
		}
		m_was_busy = m_state.busy;
		const auto *vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->WorkPos);
		ImGui::SetNextWindowSize(vp->WorkSize);
		ImGui::Begin("settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                ImGuiWindowFlags_NoSavedSettings);
		const float fs = ImGui::GetFontSize();
		ImGui::BeginChild("categories", ImVec2(fs * 8, 0), ImGuiChildFlags_Borders);
		for (const auto &[page, label] : std::array<std::pair<settings_page, const char *>, 4>{
		         {{settings_page::general, UI_TEXT(settings_general, "General")},
		          {settings_page::audio, UI_TEXT(settings_audio, "Audio")},
		          {settings_page::midi, UI_TEXT(settings_midi, "MIDI")},
		          {settings_page::emulation, UI_TEXT(settings_emulation, "Emulation")}}}) {
			if (ImGui::Selectable(label, m_state.page == page)) m_state.page = page;
		}
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("page", ImVec2(0, 0));
		if (m_state.page == settings_page::general) draw_general();
		else if (m_state.page == settings_page::audio) draw_audio();
		else if (m_state.page == settings_page::midi) draw_midi();
		else draw_emulation();
		ImGui::EndChild();
		ImGui::End();
	}
private:
	static bool combo(const char *label, const char *preview)
	{
		ImGui::TextUnformatted(label);
		return ImGui::BeginCombo((std::string("##") + label).c_str(), preview);
	}
	// Return names so a refreshed list cannot redirect a pending selection.
	static bool device_combo(const char *label, const std::vector<std::string> &names,
	                         std::string &value, const char *empty)
	{
		bool changed = false;
		ImGui::SetNextItemWidth(-1);
		ImGui::TextUnformatted(label);
		ImGui::PushID(label);
		if (ImGui::BeginCombo("##device", value.empty() ? empty : value.c_str())) {
			if (ImGui::Selectable(empty, value.empty())) { value.clear(); changed = true; }
			for (size_t i = 0; i < names.size(); i++) {
				ImGui::PushID(int(i));
				if (ImGui::Selectable(names[i].c_str(), value == names[i])) { value = names[i]; changed = true; }
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
		return changed;
	}
	void draw_general()
	{
		ImGui::SeparatorText(UI_TEXT(settings_general, "General"));
		if (combo(UI_TEXT(settings_language, "Language"), lang_name(get_lang()))) {
			for (int i = 0; i < NLANG; i++)
				if (ImGui::Selectable(LANG_NAMES[i], int(get_lang()) == i)) m_actions.language(i);
			ImGui::EndCombo();
		}
	}
	void draw_audio()
	{
		const audio_output_config before = m_draft;
		ImGui::SeparatorText(UI_TEXT(settings_audio, "Audio"));
		// Labels sit above their controls, so translations fit narrow windows too.
		ImGui::PushItemWidth(-1);
		ImGui::BeginDisabled(!m_state.ready || m_state.busy);
		device_combo(UI_TEXT(menu_audio_title, "Audio output device"), m_state.outputs, m_draft.device,
		             UI_TEXT(menu_audio_default, "System default"));
#if !defined(__linux__) && (!defined(__APPLE__) || TARGET_OS_OSX)
		if (ImGui::Checkbox(UI_TEXT(settings_exclusive, "Exclusive access"), &m_draft.preferences.exclusive))
			m_draft.preferences.stream.sample_rate = 0;
#endif
		// Capabilities describe the opened device, not an unverified draft.
		const bool same_device = m_draft.device == m_state.audio.device &&
		                        m_draft.preferences.exclusive == m_state.audio.preferences.exclusive;
		// No format control where the platform owns the format: offering one that
		// start() then refuses is worse than not offering it.
		ImGui::BeginDisabled(!same_device || !m_state.stream.manual_format);
		char rate_label[64];
		std::snprintf(rate_label, sizeof(rate_label), "%d Hz", m_draft.preferences.stream.sample_rate);
		if (combo(UI_TEXT(settings_rate, "Stream sample rate"),
		                     m_draft.preferences.stream.sample_rate ? rate_label : UI_TEXT(settings_auto, "Automatic"))) {
			if (ImGui::Selectable(UI_TEXT(settings_auto, "Automatic"), !m_draft.preferences.stream.sample_rate))
				m_draft.preferences.stream.sample_rate = 0;
			for (int rate : m_state.stream.rates) {
				std::snprintf(rate_label, sizeof(rate_label), "%d Hz", rate);
				if (ImGui::Selectable(rate_label, m_draft.preferences.stream.sample_rate == rate))
					m_draft.preferences.stream.sample_rate = rate;
			}
			ImGui::EndCombo();
		}
		for (int side = 0; side < 2; side++) {
			int &channel = side ? m_draft.preferences.stream.right : m_draft.preferences.stream.left;
			int &other = side ? m_draft.preferences.stream.left : m_draft.preferences.stream.right;
			const auto &names = m_state.stream.channels;
			const char *preview = names.size() == 1 ? names[0].c_str() : channel >= 0 && size_t(channel) < names.size() ? names[size_t(channel)].c_str() : "-";
			ImGui::BeginDisabled(names.size() < 2);
			if (combo(side ? UI_TEXT(settings_right, "Right output channel") : UI_TEXT(settings_left, "Left output channel"), preview)) {
				for (size_t c = 0; c < names.size(); c++)
					if (ImGui::Selectable(names[c].c_str(), channel == int(c))) {
						if (other == int(c)) other = channel;
						channel = int(c);
					}
				ImGui::EndCombo();
			}
			ImGui::EndDisabled();
		}
		ImGui::EndDisabled();
		char buffer_label[64];
		std::snprintf(buffer_label, sizeof(buffer_label), "%d", m_draft.preferences.stream.buffer_frames);
		ImGui::BeginDisabled(!m_state.stream.manual_buffer);
		if (combo(UI_TEXT(settings_buffer, "Requested buffer/period (frames)"),
		                     m_draft.preferences.stream.buffer_frames ? buffer_label : UI_TEXT(settings_auto, "Automatic"))) {
			if (ImGui::Selectable(UI_TEXT(settings_auto, "Automatic"), !m_draft.preferences.stream.buffer_frames))
				m_draft.preferences.stream.buffer_frames = 0;
			const std::vector<int> defaults{64, 128, 256, 512, 1024, 2048, 4096, 8192};
			for (int frames : same_device && !m_state.stream.buffers.empty() ? m_state.stream.buffers : defaults) {
				std::snprintf(buffer_label, sizeof(buffer_label), "%d", frames);
				if (ImGui::Selectable(buffer_label, m_draft.preferences.stream.buffer_frames == frames))
					m_draft.preferences.stream.buffer_frames = frames;
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		ImGui::BeginDisabled(m_draft.preferences.stream.buffer_frames != 0);
		ImGui::TextUnformatted(UI_TEXT(settings_latency, "Automatic buffer target (ms)"));
		ImGui::SliderInt("##latency", &m_draft.preferences.latency_ms, 5, 200);
		const bool latency_active = ImGui::IsItemActive();
		const bool latency_done = ImGui::IsItemDeactivatedAfterEdit();
		ImGui::EndDisabled();
#if defined(__APPLE__) && TARGET_OS_OSX
		ImGui::BeginDisabled(!custom_audio_format(m_draft.preferences.stream));
#endif
		const char *qualities[] = {UI_TEXT(settings_sinc, "Sinc (high quality)"), "Linear", UI_TEXT(settings_nearest, "Nearest (lo-fi)")};
		if (combo(UI_TEXT(settings_resampler, "Resampler"), qualities[int(m_draft.preferences.stream.quality)])) {
			for (int i = 0; i < 3; i++)
				if (ImGui::Selectable(qualities[i], int(m_draft.preferences.stream.quality) == i))
					m_draft.preferences.stream.quality = resampler_quality(i);
			ImGui::EndCombo();
		}
#if defined(__APPLE__) && TARGET_OS_OSX
		ImGui::EndDisabled();
#endif
		ImGui::EndDisabled();
		if ((m_draft != before && !latency_active) || latency_done) m_actions.audio(m_draft);
		if (m_state.busy) ImGui::TextUnformatted(UI_TEXT(settings_opening, "Opening audio device..."));
		if (!m_state.error.empty()) ImGui::TextWrapped("%s", m_state.error.c_str());
		ImGui::Separator();
		ImGui::BeginDisabled(!m_state.ready || m_state.busy);
		std::string input = m_state.input;
		if (device_combo(UI_TEXT(menu_ain_title, "A/D INPUT (sound to sample)"), m_state.inputs, input, UI_TEXT(menu_unused, "Unused")))
			m_actions.input(input);
		float gain = m_state.gain;
		ImGui::TextUnformatted(UI_TEXT(settings_volume, "Output volume"));
		if (ImGui::SliderFloat("##volume", &gain, 0, 1, "%.2f")) m_actions.volume(gain);
		if (ImGui::IsItemDeactivatedAfterEdit()) m_actions.save_volume();
		bool analog = m_state.analog;
		if (ImGui::Checkbox(UI_TEXT(settings_dc, "Analog output DC filtering"), &analog))
			m_actions.command(analog ? ID_OUTPUT_ANALOG : ID_OUTPUT_DIGITAL);
		bool limiter = m_state.limiter;
		if (ImGui::Checkbox(UI_TEXT(settings_limiter, "Limit output peaks"), &limiter)) m_actions.limiter(limiter);
		ImGui::EndDisabled();
		ImGui::PopItemWidth();
	}
	void draw_midi()
	{
		ImGui::SeparatorText(UI_TEXT(settings_midi_inputs, "MIDI inputs"));
		ImGui::BeginDisabled(!m_state.ready || m_state.busy);
		const float available = ImGui::GetContentRegionAvail().y;
		draw_midi_device_list(m_state.midi, m_state.midi_inputs, false, std::max(100.0f, available * 0.55f), m_actions.midi);
		ImGui::SeparatorText(UI_TEXT(settings_midi_outputs, "MIDI outputs"));
		draw_midi_device_list(m_state.midi, m_state.midi_outputs, true, std::max(80.0f, ImGui::GetContentRegionAvail().y - (m_state.midi_error.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing() * 2)), m_actions.midi);
		ImGui::EndDisabled();
		if (!m_state.midi_error.empty()) ImGui::TextWrapped("%s", m_state.midi_error.c_str());
	}
	void draw_emulation()
	{
		ImGui::SeparatorText(UI_TEXT(settings_emulation, "Emulation"));
		ImGui::BeginDisabled(!m_state.ready || m_state.busy);
		bool fx = m_state.native_fx;
		if (ImGui::Checkbox(UI_TEXT(settings_native_fx, "Play effects in C++"), &fx)) m_actions.command(ID_NATIVE_FX);
		ImGui::SetItemTooltip("%s", UI_TEXT(settings_native_fx_tip, "Runs effects in C++ to reduce CPU use.\nSome effects may sound different."));
		bool thin = m_state.thin_bends;
		if (ImGui::Checkbox(UI_TEXT(settings_thin_bends, "Lighten heavy MIDI"), &thin)) m_actions.command(ID_THIN_BENDS);
		ImGui::SetItemTooltip("%s", UI_TEXT(settings_thin_bends_tip, "During MIDI-file playback, reduces pitch-bend updates\nand skips Roland display data."));
		bool engine = m_state.native_engine;
		if (ImGui::Checkbox(UI_TEXT(settings_native_engine, "Play without the firmware"), &engine)) m_actions.command(ID_NATIVE_ENGINE);
		ImGui::SetItemTooltip("%s", UI_TEXT(settings_native_engine_tip, "Handles MIDI and notes in C++ to reduce CPU use.\nSound and feature support may differ from firmware playback."));
		ImGui::EndDisabled();
	}
	settings_state &m_state;
	settings_actions &m_actions;
	audio_output_config m_draft, m_observed;
	bool m_initialized = false, m_was_busy = false;
};
} // namespace ui
