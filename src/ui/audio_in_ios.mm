// license:BSD-3-Clause
//
// The iOS half of Apple audio, recording: the questions the engine cannot
// answer, and nothing else.
//
// The tap, the ring, the resampler and the counters are audio_apple.mm's,
// shared with macOS. What is left is what only iOS has, and almost all of that
// is the session (ui/session_ios.{h,mm}, which the playback half uses as well):
// the ports are the session's, not a HAL's, so the device is chosen with
// setPreferredInput: rather than by writing a property on a unit. So this file
// asks that one file and forwards - the permission prompt, the port list, the
// choice, the category - and the only answers written out in full here are the
// two that are not session work: which name means which port, and the line the
// front ends print.
//
// Four iOS-only facts, all reached from start():
// - The microphone needs permission (NSMicrophoneUsageDescription in the
//   plist). Undetermined asks and fails this pick with "pick again"; denied
//   fails with where to re-allow. The menu's list() asks early (fire and
//   forget) so the prompt is usually answered before the pick.
// - Input needs the PlayAndRecord category; output runs Playback. Switching
//   re-routes output to the earpiece unless DefaultToSpeaker is set, so it
//   is - otherwise picking the mic silences the speaker.
// - Names are the session's input ports ("Built-in Microphone", ...); empty
//   means the first one, the same default rule as the mac backend.
// - The input engine is separate from the output engine: stopping output must
//   not kill recording and vice versa.

#include "ui/session_ios.h"
#include "ui/audio_apple.h"
#include "ui/audio_in.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {

// ---- The answers, and nothing else -----------------------------------------
//
// audio_out's and audio_in's own methods are in audio_apple.mm, beside the code
// they forward to. What this file holds is the answers to the questions in
// ui/audio_apple.h that only this platform can answer.

// ---- The iOS answers to the shared core (see ui/audio_apple.h) --------------

namespace apple {

bool input_permission(std::string &err)
{
	return ios::request_mic_permission(err);
}

bool session_open_input(std::string &err)
{
	return ios::session_open_input(err);
}

// Nothing connects here: permission, category and capture all wait for start().
std::vector<std::string> input_list()
{
	return ios::input_port_names();
}

// Which port a name means. Empty means the first one, the default rule of every
// backend here; an unknown name is refused rather than quietly recorded, which
// is what the menu expects when the remembered device is gone.
device_ref resolve_input(const std::string &name, bool)
{
	const std::vector<std::string> names = input_list();
	device_ref dev;
	if (name.empty()) {
		if (names.empty())
			return dev;                    // found stays false: nothing to record from
		dev.name = names.front();
		return dev;
	}
	if (std::find(names.begin(), names.end(), name) == names.end())
		return dev;
	dev.name = name;
	return dev;
}

// Ask the session for the port that was picked. macOS pins a device by setting
// a property on the unit the engine hands out; iOS has no HAL, so the session is
// asked instead. The walk over the available ports is session_ios's, so the
// names on offer and the ports a choice can name are one list.
bool pin_input(AudioUnit, const device_ref &dev, std::string &)
{
	return ios::set_preferred_input(dev.name);
}

std::string input_label(const device_ref &, double rate, u32 channels,
                        const char *sample)
{
	char line[160] = {};
	std::snprintf(line, sizeof line, "iOS / %.0f Hz %uch %s \xe2\x86\x92 44100 Hz s16",
	              rate, channels, sample ? sample : "?");
	return line;
}

} // namespace apple

} // namespace ui
