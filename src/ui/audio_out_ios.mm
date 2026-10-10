// license:BSD-3-Clause
//
// The iOS half of Apple audio, playback: the questions the engine cannot
// answer, and nothing else.
//
// The render path - engine, source node, render block, resampler, meters,
// capture, workgroup - is audio_apple.mm, shared with macOS. What is left is
// what only iOS can answer, and the answer to most of it is short: the route is
// the system's, one route at a time, described by AVAudioSession.currentRoute,
// so there is no device to enumerate (list() reports the route, which is what
// the picker shows), none to pin, none to hog and no buffer size to write - a
// hand-written AudioUnit would have to get the last three right itself, and
// getting one wrong means silence. The session itself lives in
// ui/session_ios.{h,mm}, which the recording half uses too; its counterpart on
// the macOS side is ui/session_mac.cpp, which has no session to watch.
//
// iOS ships no public AudioHardware HAL: AudioObject* appears in no header,
// only in CoreAudio.tbd, so the macOS half does not compile there at all.
//
// The engine's nodes hand out the very AudioUnit a hand-written backend would
// own, so device, buffer size, stream format and workgroup are the same
// properties on both systems.

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#include "ui/session_ios.h"
#include "ui/audio_apple.h"
#include "ui/audio_out.h"
#include "compat/cli_text.h"

#include <cstdio>
#include <memory>
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

bool session_open(int latency_ms, std::string &err)
{
	return ios::session_open_output(latency_ms, err);
}

// The one route, by its port name ("iPhone Speaker", "AirPods", ...). Empty
// when nothing is attached, which the picker shows as empty rather than lying.
std::vector<std::string> output_list()
{
	return ios::output_port_names();
}

// Which device a name means: there is only ever the one route, so any name
// resolves to it and is found. A remembered name goes stale the moment AirPods
// connect, so refusing one here would turn a cosmetic mismatch into a silent
// app - the route is not ours to refuse.
device_ref resolve_output(const std::string &name, bool)
{
	device_ref dev;
	dev.id = 0;   // no HAL to name a device with
	dev.name = name;
	dev.found = true;
	return dev;
}

// Nothing to pin: the session chose the route and the unit follows it.
bool pin_output(AudioUnit, const device_ref &, std::string &)
{
	return true;
}

void unpin_output()
{
}

// No hog mode on iOS: nothing else can share the route through us, and the
// system mixer is not ours to take over. So nothing is ever given back either.
device_claim take_output(const device_ref &, std::string &)
{
	return device_claim();
}

void release_output(const device_claim &)
{
}

// The IO buffer duration was asked of the session in session_open(), and the
// device under the unit is not ours to resize. Zero says so.
u32 request_buffer_frames(const device_ref &, int, u32)
{
	return 0;
}

bool custom_output_format(const device_ref &, const audio_stream_options &want,
                          std::string &err, bool exclusive)
{
	// One route, one rate, and the session decides both, so there is nothing to
	// convert into: the machine's 44100 goes to the session and it hands the device
	// whatever the device runs at. The buffer and the access mode are the session's
	// too, and main refused both with a reason - losing them would leave the window
	// showing a setting that is not what is playing.
	if (want.buffer_frames) {
		err = CLI_T("iOS sets the output buffer, not the application",
		            "出力バッファは iOS 側が決める");
		return false;
	}
	if (exclusive && want.strict) {
		err = CLI_T("iOS has no exclusive mode to fall back from",
		            "iOS には排他がない");
		return false;
	}
	if (custom_audio_format(want)) {
		err = CLI_T("iOS controls the output sample rate and channels",
		            "出力の周波数とチャンネルは iOS 側が決める");
		return false;
	}
	return true;
}

bool output_channel_layout(const device_ref &, AudioChannelLayout &) { return false; }

// One route, so its name is both the whole list and the default - nothing here
// to confuse one with the other.
std::string default_output_name()
{
	const auto names = output_list();
	return names.empty() ? std::string() : names.front();
}

u32 output_channels(const device_ref &) { return 2; }

audio_stream_info output_capabilities(const device_ref &, double rate)
{
	// One route and one rate: the session decides both, and it can be asked for
	// neither. The window shows what is running and offers nothing else.
	audio_stream_info info;
	info.rate = int(rate > 0.0 ? rate : 0.0);
	info.rates.push_back(info.rate);
	info.channels.push_back("Output 1");
	info.channels.push_back("Output 2");
	info.manual_buffer = false;  // the session owns the IO period
	info.manual_format = false;  // and the rate and the channels with it
	return info;
}

std::string output_label(const device_ref &, double rate)
{
	char name[128] = {};
	std::snprintf(name, sizeof(name), "iOS %.0f Hz", rate);
	return name;
}

} // namespace apple

} // namespace ui
