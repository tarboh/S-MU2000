// license:BSD-3-Clause
#pragma once
#include "audio_preferences.h"

namespace ui {
// Startup/recovery may discard an obsolete format; an explicit edit stays strict.
template <typename Output, typename Fill>
bool start_audio_stream(Output &out, Fill fill, audio_output_config &config, std::string &error,
                        bool exact = true)
{
	bool control_panel = config.control_panel;
	config.control_panel = false;
	const auto open = [&](const audio_stream_options &stream, std::string &why) {
		out.stop();
		out.set_stream_options(stream);
		out.set_control_panel(control_panel);
		control_panel = false;
		return out.start(config.preferences.latency_ms, fill, why, config.preferences.exclusive,
		                 config.device, false, exact);
	};
	if (open(config.preferences.stream, error)) return true;
	const auto &s = config.preferences.stream;
	if (s.strict || (!s.sample_rate && !s.buffer_frames && s.left == 0 && s.right == 1)) return false;
	auto automatic = s;
	automatic.sample_rate = automatic.buffer_frames = 0;
	automatic.left = 0; automatic.right = 1;
	std::string fallback_error;
	if (!open(automatic, fallback_error)) {
		error += "\n" + fallback_error;
		return false;
	}
	config.preferences.stream = automatic;
	return true;
}
} // namespace ui
