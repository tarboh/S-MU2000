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

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ui {

// gui.ini keys, in A B C D order for the MIDI IN ports.
inline constexpr const char *SET_IN_KEYS[4] = { "midi_in", "midi_in_b", "midi_in_c", "midi_in_d" };
inline constexpr const char *SET_OUT = "midi_out";
inline constexpr const char *SET_OUT_B = "midi_out_b";
inline constexpr const char *SET_OUT_MU = "midi_out_mu";
inline constexpr const char *SET_AUDIO_OUT = "audio_out";
inline constexpr const char *SET_AUDIO_IN = "audio_in";
inline constexpr const char *SET_CARD = "smartmedia";
inline constexpr const char *SET_PORTS34 = "ports34";
inline constexpr const char *SET_OUTPUT = "output";
inline constexpr const char *SET_VOLUME = "volume";

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
