// license:BSD-3-Clause
//
// The macOS half of Apple audio: the questions the engine cannot answer, and
// nothing else.
//
// The render path - the render callback, the meters, the workgroup, the WAV
// writer - is audio_apple.mm, which iOS uses too. What macOS alone can do:
// enumerate devices by HAL property query, resolve a name to one (whole-name
// first for a menu selection, then a substring, case-insensitively), resize that
// device's buffer, and take it for ourselves in hog mode. Every one of those
// goes through properties on the very AudioUnit AVAudioEngine hands out, which
// is why the two halves meet at ui/audio_apple.h.
//
// The rule from doc/design.md carries over unchanged: **we own no clock**.
// CoreAudio asks for N frames and we make exactly those N.

#include "audio_out.h"
#include "audio_apple.h"
#include "compat/cli_text.h"
#include "hal_mac.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ui {


// ---- The macOS answers to the shared core (see ui/audio_apple.h) ------------
//
// One function per question ui/audio_apple.mm asks, and no branch anywhere else
// on the platform.

namespace apple {

// No session on macOS: there is no category to pick, nothing to activate and no
// permission to ask - the system default device is chosen by the system, and a
// device that goes away is answered by the HAL rather than by a notification.
// So the honest answer is yes, and the shared core takes the engine from here.
bool session_open(int, std::string &)
{
	return true;
}

std::vector<std::string> output_list()
{
	return hal::names(hal::direction::output);
}

// The system default, not output_list().front(): the list is in HAL order and has
// nothing to do with which device is default.
std::string default_output_name()
{
	return hal::name_of(hal::default_device(hal::direction::output));
}

// Which device a name means, and what to call it. Empty means the system
// default. A device that has gone leaves found false, so the caller can say so
// instead of opening whatever is left.
device_ref resolve_output(const std::string &name, bool exact)
{
	device_ref dev;
	dev.id = hal::find_device(name, exact);
	dev.found = dev.id != kAudioObjectUnknown;
	if (dev.found)
		dev.name = hal::name_of(dev.id);
	// An empty name is not a device, it is a wish: follow whatever the system
	// default is, and keep following it. The id is still resolved (the buffer
	// size is asked of that device, and the status line names it) but nothing is
	// pinned to it, which is what makes the change of default mean anything.
	dev.follow = name.empty();
	return dev;
}

// Best effort, as it always was: ask for a buffer matching the requested latency
// and report what the driver took. Zero means the write failed and the core
// falls back to whatever the first block turns out to be.
u32 request_buffer_frames(const device_ref &dev, int latency_ms, u32 requested)
{
	if (dev.id == kAudioObjectUnknown)
		return 0;
	return hal::set_buffer_frames(dev.id, latency_ms, requested);
}

// The same property this file set on a unit of its own, now set on the unit the
// engine hands out - and set only for a device that was actually asked for. An
// unnamed request is left unpinned, which is upstream's kAudioUnitSubType_
// DefaultOutput in the only terms AVAudioEngine has: the output node with no
// CurrentDevice follows the system default, so headphones move the sound. Pinning
// it to whatever was default at the time is the behaviour that stopped that, and
// the reason the change of default appeared to do nothing until the old device
// was switched off.
bool pin_output(AudioUnit unit, const device_ref &dev, std::string &err)
{
	if (unit == nullptr || dev.id == kAudioObjectUnknown || dev.follow)
		return true;
	if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice,
	                         kAudioUnitScope_Global, 0, &dev.id, sizeof(dev.id)) != noErr) {
		err = CLI_T("Cannot select the audio output", "音声の出口を選べない");
		return false;
	}
	return true;
}

void unpin_output()
{
	// Nothing is remembered here: the pin is a property of the unit, and the
	// unit dies with the engine.
}

// Hog mode, with the same two rules as before. It is claimed after IO has
// started, because claiming first can leave a device that cannot be mixed
// unopenable. And it is given back only when taking it is what got it: a device
// some other process holds is not ours to release.
device_claim take_output(const device_ref &dev, std::string &err)
{
	device_claim claim;
	if (dev.id == kAudioObjectUnknown) {
		err = CLI_T("No audio output found", "音声の出口が見つからない");
		return claim;
	}
	claim.id = dev.id;
	bool took = false;
	// took says we are the ones who have to give it back; held says it is ours
	// to use, which is also true of a device we already held.
	if (!hal::take_hog(dev.id, took)) {
		std::fprintf(stderr, "[mac] hog refused: %s\n", dev.name.c_str());
		return claim;
	}
	claim.took = took;
	claim.held = true;
	return claim;
}

void release_output(const device_claim &claim)
{
	if (claim.took && claim.id != kAudioObjectUnknown)
		hal::release_hog(claim.id);
}

// The device's own name, which is what the status line and the menu compare
// against. A device with no name (should not happen) falls back to the rate.
bool custom_output_format(const device_ref &dev, const audio_stream_options &want,
                          std::string &err, bool exclusive)
{
	(void)exclusive;   // strict is answered by the core, which knows if the hog was taken
	if (dev.id == kAudioObjectUnknown)
		return true;   // nothing to check against; the core will say if it cannot
	const u32 channels = hal::stream_channels(dev.id, hal::direction::output);
	if (!valid_audio_route(want, channels)) {
		err = CLI_T("The selected output channels are unavailable",
		            "選んだ出力チャンネルは使えない");
		return false;
	}
	// A route past the first pair needs a connection as wide as the device, which
	// a channel layout describes and a channel count does not. Without one the
	// connection is the stereo pair while the block writes the device's count into
	// it: silence rather than an error, so it is refused instead.
	if (channels > 2 && (u32(want.left) >= 2 || u32(want.right) >= 2)) {
		AudioChannelLayout layout = {};
		if (!output_channel_layout(dev, layout)) {
			err = CLI_T("This device does not say which of its outputs are which",
			            "この端末はどの出力がどれかを教えてくれない");
			return false;
		}
	}
	if (want.sample_rate) {
		const std::vector<int> rates = hal::available_rates(dev.id);
		if (!rates.empty() && std::find(rates.begin(), rates.end(), want.sample_rate) == rates.end()) {
			err = CLI_T("The selected output rate is not one of the device's",
			            "選んだ出力周波数はこの端末のものではない");
			return false;
		}
	}
	return true;
}

// The device's channel layout, for a connection wide enough to route to any of
// its outputs. False when it has none to give.
bool output_channel_layout(const device_ref &dev, AudioChannelLayout &out)
{
	return dev.id != kAudioObjectUnknown && hal::preferred_channel_layout(dev.id, out);
}

u32 output_channels(const device_ref &dev)
{
	return dev.id == kAudioObjectUnknown ? 2
	                                     : hal::stream_channels(dev.id, hal::direction::output);
}

audio_stream_info output_capabilities(const device_ref &dev, double rate)
{
	audio_stream_info info;
	if (dev.id == kAudioObjectUnknown)
		return info;
	info.rate = int(rate > 0.0 ? rate : 0.0);
	info.rates = hal::available_rates(dev.id);
	if (info.rates.empty() && info.rate > 0)
		info.rates.push_back(info.rate);
	const u32 ch = hal::stream_channels(dev.id, hal::direction::output);
	for (u32 c = 0; c < ch; c++)
		info.channels.push_back("Output " + std::to_string(c + 1));
	info.manual_buffer = true;    // the window may set it, and we honour it
	info.manual_format = true;    // as it may the rate and the channel pair
	return info;
}

std::string output_label(const device_ref &dev, double rate)
{
	if (dev.id != kAudioObjectUnknown && !dev.name.empty())
		return dev.name;
	char name[128] = {};
	std::snprintf(name, sizeof(name), "%.0f Hz", rate);
	return name;
}

} // namespace apple

} // namespace ui
