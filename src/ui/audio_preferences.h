// license:BSD-3-Clause
#pragma once
#include "audio_stream.h"

namespace ui {

// ALSA menus append a human-readable description to the PCM identifier.
inline std::string audio_device_key(const std::string &name)
{
#if defined(__linux__)
	return name.substr(0, name.find("  ("));
#else
	return name;
#endif
}

struct audio_preferences {
#if defined(_WIN32)
	int latency_ms = 20;
#else
	int latency_ms = 30;
#endif
	bool exclusive = false;
	audio_stream_options stream;
	bool operator==(const audio_preferences &) const = default;
};

struct audio_output_config {
	std::string device; // empty: follow the system output
	audio_preferences preferences;
	bool control_panel = false; // one-time request, never persisted
	bool operator==(const audio_output_config &) const = default;
};

struct audio_channel_route {
	std::string device;
	int left = 0, right = 1;
	audio_driver driver = audio_driver::native;
};

// Save only fields the user changed, keeping unrelated one-run CLI overrides out.
inline void remember_audio_change(audio_output_config &saved, const audio_output_config &before,
                                  const audio_output_config &after)
{
	if (before.device != after.device) saved.device = after.device;
	auto &s = saved.preferences;
	const auto &a = before.preferences, &b = after.preferences;
	if (a.latency_ms != b.latency_ms) s.latency_ms = b.latency_ms;
	if (a.exclusive != b.exclusive) s.exclusive = b.exclusive;
	// Rate, buffer and channels describe one validated format.
	if (before.device != after.device || a.stream.driver != b.stream.driver || a.stream.sample_rate != b.stream.sample_rate ||
	    a.stream.buffer_frames != b.stream.buffer_frames || a.stream.left != b.stream.left || a.stream.right != b.stream.right) {
		s.stream.sample_rate = b.stream.sample_rate;
		s.stream.buffer_frames = b.stream.buffer_frames;
		s.stream.left = b.stream.left;
		s.stream.right = b.stream.right;
	}
	if (a.stream.driver != b.stream.driver) { s.stream.driver = b.stream.driver; saved.device = after.device; }
	if (a.stream.quality != b.stream.quality) s.stream.quality = b.stream.quality;
}
} // namespace ui
