// license:BSD-3-Clause
//
// The macOS half of Apple audio, recording side: the questions the engine
// cannot answer, and nothing else.
//
// The tap, the float-to-s16 conversion, the ring and the resampler are
// audio_apple.mm's, shared with iOS. What is left is the HAL: which devices can
// record, which one a remembered name means, and the three properties that pin
// the unit to it. The queries themselves are in ui/hal_mac.h, which both halves
// include - the same queries answer for playback and for recording, only the
// direction differs.

#include "audio_in.h"
#include "audio_out.h"          // AUDIO_RATE, shared with the output side
#include "audio_apple.h"
#include "compat/cli_text.h"
#include "hal_mac.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {

// ---- The answers, and nothing else -----------------------------------------
//
// audio_out's and audio_in's own methods are in audio_apple.mm, beside the code
// they forward to. What this file holds is the answers to the questions in
// ui/audio_apple.h that only this platform can answer.

// ---- The macOS answers to the shared core (see ui/audio_apple.h) ------------

namespace apple {

// Nothing to ask: macOS has no microphone permission call, but since macOS 14
// it has per-app access control answered by the bundle's Info.plist - which is
// why packaging/auv3-app-Info.plist carries NSMicrophoneUsageDescription. A
// bundle without the key is refused, and `live` has no bundle, so it records
// under whatever the terminal running it was granted.
bool input_permission(std::string &)
{
	return true;
}

bool session_open_input(std::string &)
{
	return true;
}

std::vector<std::string> input_list()
{
	return hal::names(hal::direction::input);
}

// The same rule as the output side: empty means the system default, and exact
// (a menu selection) accepts only a whole name so a device that has gone cannot
// quietly become another one.
device_ref resolve_input(const std::string &name, bool exact)
{
	device_ref dev;
	dev.id = hal::find_device(name, exact, hal::direction::input);
	dev.found = dev.id != kAudioObjectUnknown;
	if (dev.found)
		dev.name = hal::name_of(dev.id);
	dev.follow = name.empty();   // as on the output side: no name, no pin
	return dev;
}

// The input bus on, the output bus off, then the device - the same three
// properties in the same order as the backend this replaces, set on the unit
// AVAudioEngine hands out instead of one of our own. Without the first two the
// unit would try to play as well as record.
bool pin_input(AudioUnit unit, const device_ref &dev, std::string &err)
{
	// dev.follow, as pin_output: an unnamed request takes whatever the system
	// defaults to, now and later. The bus properties below are ours to set
	// either way - they are the unit's, not the device's.
	if (unit == nullptr || dev.id == kAudioObjectUnknown || dev.follow)
		return true;
	UInt32 on = 1, off = 0;
	if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
	                         kAudioUnitScope_Input, 1, &on, sizeof(on)) != noErr ||
	    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
	                         kAudioUnitScope_Output, 0, &off, sizeof(off)) != noErr ||
	    AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
	                         kAudioUnitScope_Global, 0, &dev.id, sizeof(dev.id)) != noErr) {
		err = CLI_T("Cannot select the recording device", "録音デバイスを選べない");
		return false;
	}
	return true;
}

// The line the front ends print under the input device. The channel count is the
// device's own, as it always was, and so is the sample type: the tap hands over
// whatever the node has, and the core converts it to s16 at 44100.
std::string input_label(const device_ref &dev, double rate, u32 channels,
                        const char *sample)
{
	const u32 ch = dev.id != kAudioObjectUnknown ? hal::input_channels(dev.id) : channels;
	char line[160] = {};
	std::snprintf(line, sizeof line, "CoreAudio / %.0f Hz %u ch %s \xe2\x86\x92 44100 Hz s16",
	              rate, ch, sample ? sample : "?");
	return line;
}

} // namespace apple

} // namespace ui
