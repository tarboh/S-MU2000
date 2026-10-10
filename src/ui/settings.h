// license:BSD-3-Clause
//
// Shared gui.ini keys and flat file IO for the standalone front ends
// (gui.cpp, gui_mac.cpp).
//
// The state structs stay per front end (different shapes: globals versus
// the app class, keep-lists, --nomidi handling), but the keys and the file
// format live here once, so a setting added on one side cannot be missed
// or misspelled on the other. Needs only <cstdio>, <string> and <vector>.

#ifndef S_MU2000_UI_SETTINGS_H
#define S_MU2000_UI_SETTINGS_H

#pragma once

#include "audio_preferences.h"
#include "midi_routes.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ui {

// gui.ini keys, in A B C D order for the MIDI IN ports.
// 5 つ目は口 E（マルチパートのプラグインボードが受け持つ口。mu2000::board_midi_in）
inline constexpr int IN_PORTS = 5;
inline constexpr const char *SET_IN_KEYS[IN_PORTS] = { "midi_in", "midi_in_b", "midi_in_c", "midi_in_d", "midi_in_e" };
inline constexpr const char *SET_OUT = "midi_out";
inline constexpr const char *SET_OUT_B = "midi_out_b";
inline constexpr const char *SET_OUT_MU = "midi_out_mu";
inline constexpr const char *SET_AUDIO_OUT = "audio_out";
inline constexpr const char *SET_AUDIO_IN = "audio_in";
inline constexpr const char *SET_CARD = "smartmedia";
inline constexpr const char *SET_PORTS34 = "ports34";
inline constexpr const char *SET_THIN_BENDS = "thin_bends";   // 再生でピッチベンドを間引く（1 / 0）
inline constexpr const char *SET_OUTPUT = "output";
inline constexpr const char *SET_BOARD = "board";             // 架空のプラグインボード（fc。無ければ空）
inline constexpr const char *SET_BOARD_PART = "board_part";   // そのパート（1-64）
// 差込口 2〜6（PLG-2〜PLG-6。4 から先は増設の差込口）。書き方は board と同じ
inline constexpr const char *const SET_BOARD_MORE[5] = { "board2", "board3", "board4", "board5", "board6" };
inline constexpr const char *const SET_BOARD_MORE_PART[5] = { "board2_part", "board3_part", "board4_part", "board5_part", "board6_part" };
inline constexpr const char *SET_BOARD_DLS = "board_dls";     // DLS のボードが読むファイル（道。UTF-8）
inline constexpr const char *SET_BOARD_FILE = "board_file";   // オリジナルのボードのファイル（道。src/ui/user_boards.h）
// FC ボードの音色の組のファイル（道。src/ui/fc_banks.h。無ければ初期の音色）。ボードごと: PLG-1・2・3 の 1 パートのボード、16 パートのボード
inline constexpr const char *const SET_BOARD_FC[7] = { "board_fc", "board_fc2", "board_fc3", "board_fc4", "board_fc5", "board_fc6", "board_fc16" };
// FC ボードの音色の値を動かすコントロールチェンジの番号（欄の順にコンマで。src/vboard.h の fc_cc_text。無ければ初期の番号）
inline constexpr const char *SET_BOARD_FC_CC = "board_fc_cc";
inline constexpr const char *SET_BOARD_FM = "board_fm";       // FM ボードの音色の組のファイル（道。src/ui/fm_banks.h。無ければ初期の音色）
inline constexpr const char *SET_VOLUME = "volume";
inline constexpr const char *SET_EDIT_OUT = "edit_out";   // 音色の窓の送り先（空はパネルの設定）

// The whole file as key/value pairs, in file order.
using settings_map = std::vector<std::pair<std::string, std::string>>;

// Reads the file, one key=value per line. False when the file cannot be
// opened (first run); malformed lines are skipped.
inline bool read_settings_file(const std::string &path, settings_map &out)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char line[512];
	while (std::fgets(line, sizeof(line), f)) {
		std::string t(line);
		while (!t.empty() && (t.back() == '\n' || t.back() == '\r'))
			t.pop_back();
		const size_t eq = t.find('=');
		if (eq == std::string::npos)
			continue;
		out.emplace_back(t.substr(0, eq), t.substr(eq + 1));
	}
	std::fclose(f);
	return true;
}

inline bool write_settings_file(const std::string &path, const settings_map &in)
{
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	for (const auto &kv : in)
		std::fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
	std::fclose(f);
	return true;
}

// The value for key, or null. The last match wins, as with the old per-line
// loops that kept overwriting.
inline const std::string *find_setting(const settings_map &m, const char *key)
{
	for (auto it = m.rbegin(); it != m.rend(); ++it)
		if (it->first == key)
			return &it->second;
	return nullptr;
}

// Everything gui.ini remembers, both directions. The two front ends keep
// these in different shapes (globals vs members, partial vs full loads),
// so each fills or reads one of these and collect()/apply() below do the
// file mapping once. The legacy in[]/out keys remain for migration and for
// opening the same configuration with an older build.
struct remembered {
	midi_routing midi;
	std::string in[IN_PORTS];
	std::string out, out_b, out_mu;
	std::string audio_out;
	std::string audio_in;
	audio_preferences audio;
	std::vector<audio_channel_route> audio_routes;
	bool limiter = false;
	int native_fx = 0, native_engine = 0;
	std::string card;
	float volume = 1.0f;  // the panel's VOLUME knob
	bool fold34 = true;   // ports34=fold (MIDI file ports 3+4 onto A+B)
	bool thin_bends = false; // thin_bends=1 (the player thins dense pitch bends)
	bool analog = false;  // output=analog (DC removed)
	std::string edit_out; // edit_out= (the voice window's send-to port; empty = the panel's ports)
	int board = 0;        // board=fc / fc16 (the imaginary plug-in board, mu2000::VBOARD_FC / VBOARD_FC16; 0 = none)
	int board_part = 1;   // board_part= (1-64)
	int board_more[5] = { 0, 0, 0, 0, 0 };          // board2= ... board6= (slots PLG-2 to PLG-6; 4 and up are the extra slots)
	int board_more_part[5] = { 2, 3, 4, 5, 6 };     // board2_part= ... board6_part=
	std::string board_dls; // board_dls= (the DLS file of the DLS board)
	std::string board_file; // board_file= (the user's own board, board=user / user16)
	std::string board_fm;   // board_fm= (the FM board's voice set; empty = the built-in voices)
	std::string board_fc[7]; // board_fc= board_fc2= ... board_fc6= board_fc16= (each FC board's voice set; empty = the built-in voices)
	std::string board_fc_cc; // board_fc_cc= (control change numbers of the FC board's voice parameters; empty = the defaults)
};

// --nomidi keeps the profile but skips the saved processing choices.
inline void clear_processing_settings(remembered &r)
{
	r.audio = {};
	r.audio_routes.clear();
	r.limiter = false;
	r.native_fx = r.native_engine = 0;
}

// Struct to file rows, in file order
inline settings_map collect_settings(const remembered &r)
{
	settings_map kv;
	for (int p = 0; p < IN_PORTS; p++)
		kv.emplace_back(SET_IN_KEYS[p], r.in[p]);
	kv.emplace_back(SET_OUT, r.out);
	kv.emplace_back(SET_OUT_B, r.out_b);
	kv.emplace_back(SET_OUT_MU, r.out_mu);
	kv.emplace_back("midi_routes", "1");
	for (int side = 0; side < 2; side++) {
		const auto &routes = side ? r.midi.outputs : r.midi.inputs;
		for (size_t i = 0; i < routes.size(); i++) {
			const std::string prefix = std::string(side ? "midi_output_" : "midi_input_") + std::to_string(i);
			kv.emplace_back(prefix + "_device", routes[i].device);
			kv.emplace_back(prefix + "_ports", std::to_string(routes[i].ports));
		}
	}
	kv.emplace_back(SET_AUDIO_OUT, r.audio_out);
	kv.emplace_back(SET_AUDIO_IN, r.audio_in);
	kv.emplace_back("audio_latency", std::to_string(r.audio.latency_ms));
	kv.emplace_back("audio_exclusive", r.audio.exclusive ? "1" : "0");
	kv.emplace_back("audio_resampler", std::to_string(int(r.audio.stream.quality)));
	kv.emplace_back("audio_driver", std::to_string(int(r.audio.stream.driver)));
	kv.emplace_back("audio_rate", std::to_string(r.audio.stream.sample_rate));
	kv.emplace_back("audio_buffer", std::to_string(r.audio.stream.buffer_frames));
	kv.emplace_back("audio_left", std::to_string(r.audio.stream.left));
	kv.emplace_back("audio_right", std::to_string(r.audio.stream.right));
	kv.emplace_back("output_limiter", r.limiter ? "1" : "0");
	kv.emplace_back("native_fx", std::to_string(r.native_fx));
	kv.emplace_back("native_engine", std::to_string(r.native_engine));
	for (size_t i = 0; i < r.audio_routes.size(); i++) {
		const auto &route = r.audio_routes[i];
		const std::string prefix = "audio_route_" + std::to_string(i);
		kv.emplace_back(prefix + "_device", route.device);
		kv.emplace_back(prefix + "_driver", std::to_string(int(route.driver)));
		kv.emplace_back(prefix + "_channels", std::to_string(route.left) + "," + std::to_string(route.right));
	}
	kv.emplace_back(SET_CARD, r.card);
	char vol[32];
	std::snprintf(vol, sizeof(vol), "%.3f", r.volume);
	kv.emplace_back(SET_VOLUME, vol);
	kv.emplace_back(SET_PORTS34, r.fold34 ? "fold" : "drop");
	kv.emplace_back(SET_THIN_BENDS, r.thin_bends ? "1" : "0");
	kv.emplace_back(SET_OUTPUT, r.analog ? "analog" : "digital");
	kv.emplace_back(SET_EDIT_OUT, r.edit_out);
	static const char *const BOARD_KINDS[7] = { "", "fc", "fc16", "dls", "user", "user16", "fm16" };
	kv.emplace_back(SET_BOARD, BOARD_KINDS[r.board >= 0 && r.board < 7 ? r.board : 0]);
	for (int i = 0; i < 5; i++) {
		kv.emplace_back(SET_BOARD_MORE[i], BOARD_KINDS[r.board_more[i] >= 0 && r.board_more[i] < 7 ? r.board_more[i] : 0]);
		kv.emplace_back(SET_BOARD_MORE_PART[i], std::to_string(r.board_more_part[i]));
	}
	kv.emplace_back(SET_BOARD_DLS, r.board_dls);
	kv.emplace_back(SET_BOARD_FILE, r.board_file);
	kv.emplace_back(SET_BOARD_FM, r.board_fm);
	for (int i = 0; i < 7; i++)
		kv.emplace_back(SET_BOARD_FC[i], r.board_fc[i]);
	kv.emplace_back(SET_BOARD_FC_CC, r.board_fc_cc);
	kv.emplace_back(SET_BOARD_PART, std::to_string(r.board_part));
	return kv;
}

// File rows to struct. Missing keys leave the struct's defaults, so callers
// can start from what they already have.
inline void apply_settings(const settings_map &kv, remembered &r)
{
	for (int p = 0; p < IN_PORTS; p++)
		if (const std::string *v = find_setting(kv, SET_IN_KEYS[p]))
			r.in[p] = *v;
	if (const std::string *v = find_setting(kv, SET_OUT))     r.out     = *v;
	if (const std::string *v = find_setting(kv, SET_OUT_B))   r.out_b   = *v;
	if (const std::string *v = find_setting(kv, SET_OUT_MU))  r.out_mu  = *v;
	if (const std::string *v = find_setting(kv, SET_AUDIO_OUT)) r.audio_out = *v;
	if (const std::string *v = find_setting(kv, SET_AUDIO_IN))  r.audio_in  = *v;
	const auto integer = [&](const char *key, int fallback, int min, int max) {
		const auto *v = find_setting(kv, key);
		if (!v || v->empty()) return fallback;
		char *end = nullptr;
		const long n = std::strtol(v->c_str(), &end, 10);
		return end == v->c_str() || *end || n < min || n > max ? fallback : int(n);
	};
	r.midi = {};
	if (find_setting(kv, "midi_routes")) {
		for (int side = 0; side < 2; side++) {
			auto &routes = side ? r.midi.outputs : r.midi.inputs;
			for (const auto &[key, device] : kv) {
				if (!key.starts_with(side ? "midi_output_" : "midi_input_") || !key.ends_with("_device")) continue;
				const std::string ports = key.substr(0, key.size() - 7) + "_ports";
				const unsigned mask = unsigned(integer(ports.c_str(), 0, 0, side ? 7 : 31));
				set_midi_route(routes, device, midi_route_mask(routes, device) | mask);
			}
		}
	} else {
		// Migrate single-device settings without losing shared destinations.
		for (int p = 0; p < IN_PORTS; p++) set_midi_route(r.midi.inputs, r.in[p], midi_route_mask(r.midi.inputs, r.in[p]) | (1u << p));
		const std::string names[] = {r.out, r.out_b, r.out_mu};
		for (int p = 0; p < 3; p++) set_midi_route(r.midi.outputs, names[p], midi_route_mask(r.midi.outputs, names[p]) | (1u << p));
	}
	r.audio.latency_ms = integer("audio_latency", r.audio.latency_ms, 0, 200);
	r.audio.exclusive = integer("audio_exclusive", 0, 0, 1) != 0;
	r.audio.stream.quality = resampler_quality(integer("audio_resampler", 0, 0, 2));
	r.audio.stream.driver = audio_driver(integer("audio_driver", 0, 0, 2));
	const bool unsupported_driver = !supported_audio_driver(r.audio.stream.driver);
	if (unsupported_driver) r.audio.stream.driver = audio_driver::native;
	r.audio.stream.sample_rate = integer("audio_rate", 0, 0, 192000);
	if (r.audio.stream.sample_rate && r.audio.stream.sample_rate < 8000) r.audio.stream.sample_rate = 0;
	r.audio.stream.buffer_frames = integer("audio_buffer", 0, 0, 8192);
	r.audio.stream.left = integer("audio_left", 0, 0, 63);
	r.audio.stream.right = integer("audio_right", 1, 0, 63);
	if (r.audio.stream.left == r.audio.stream.right) { r.audio.stream.left = 0; r.audio.stream.right = 1; }
	if (unsupported_driver) {
		r.audio_out.clear();
		r.audio.stream.sample_rate = r.audio.stream.buffer_frames = 0;
		r.audio.stream.left = 0; r.audio.stream.right = 1;
		r.audio.exclusive = false;
	}
	r.limiter = integer("output_limiter", 0, 0, 1) != 0;
	r.native_fx = integer("native_fx", 0, 0, 2);
	r.native_engine = integer("native_engine", 0, 0, 1);
	r.audio_routes.clear();
	for (const auto &[key, device] : kv) {
		if (!key.starts_with("audio_route_") || !key.ends_with("_device")) continue;
		const std::string channels_key = key.substr(0, key.size() - 7) + "_channels";
		const auto *v = find_setting(kv, channels_key.c_str());
		int left = 0, right = 1;
		if (v && std::sscanf(v->c_str(), "%d,%d", &left, &right) == 2 &&
		    left >= 0 && right >= 0 && left < 64 && right < 64 && left != right)
			r.audio_routes.push_back({device, left, right, audio_driver(integer((key.substr(0, key.size() - 7) + "_driver").c_str(), 0, 0, 2))});
	}
	if (const std::string *v = find_setting(kv, SET_CARD))    r.card    = *v;
	if (const std::string *v = find_setting(kv, SET_PORTS34)) r.fold34  = *v != "drop";
	if (const std::string *v = find_setting(kv, SET_THIN_BENDS)) r.thin_bends = *v == "1";
	if (const std::string *v = find_setting(kv, SET_OUTPUT))  r.analog  = *v == "analog";
	if (const std::string *v = find_setting(kv, SET_EDIT_OUT)) r.edit_out = *v;
	const auto board_kind = [](const std::string &v) { return v == "fc" ? 1 : v == "fc16" ? 2 : v == "dls" ? 3 : v == "user" ? 4 : v == "user16" ? 5 : v == "fm16" ? 6 : 0; };
	if (const std::string *v = find_setting(kv, SET_BOARD))   r.board = board_kind(*v);
	for (int i = 0; i < 5; i++) {
		if (const std::string *v = find_setting(kv, SET_BOARD_MORE[i])) r.board_more[i] = board_kind(*v);
		if (const std::string *v = find_setting(kv, SET_BOARD_MORE_PART[i])) {
			const int n = std::atoi(v->c_str());
			r.board_more_part[i] = n >= 1 && n <= 64 ? n : i + 2;
		}
	}
	if (const std::string *v = find_setting(kv, SET_BOARD_DLS)) r.board_dls = *v;
	if (const std::string *v = find_setting(kv, SET_BOARD_FILE)) r.board_file = *v;
	if (const std::string *v = find_setting(kv, SET_BOARD_FM)) r.board_fm = *v;
	for (int i = 0; i < 7; i++)
		if (const std::string *v = find_setting(kv, SET_BOARD_FC[i])) r.board_fc[i] = *v;
	if (const std::string *v = find_setting(kv, SET_BOARD_FC_CC)) r.board_fc_cc = *v;
	if (const std::string *v = find_setting(kv, SET_BOARD_PART)) {
		const int n = std::atoi(v->c_str());
		r.board_part = n >= 1 && n <= 64 ? n : 1;
	}
	if (const std::string *v = find_setting(kv, SET_VOLUME)) {
		if (!v->empty())
			r.volume = std::clamp(float(std::atof(v->c_str())), 0.0f, 1.0f);
	}
}

// A device index looked up by name, or -1 when it is not there.
inline int find_device(const std::vector<std::string> &names, const std::string &want)
{
	if (want.empty())
		return -1;
	for (size_t i = 0; i < names.size(); i++)
		if (names[i] == want)
			return int(i);
	return -1;
}

} // namespace ui

#endif // S_MU2000_UI_SETTINGS_H
