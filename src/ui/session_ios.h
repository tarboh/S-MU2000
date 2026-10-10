// license:BSD-3-Clause
//
// What only iOS has, in one place: AVAudioSession and the microphone permission
// that comes with it.
//
// Both halves of the iOS audio back end need it and neither owns it - the
// output half plays under Playback, the input half under PlayAndRecord - and
// they need the same things from it: a category and an activation, the
// notifications that say the route changed (the engine stops itself when
// headphones appear and nothing restarts it), the ports - which are also how a
// device is chosen, since there is no HAL on iOS - and the permission recording
// needs. So this file is the counterpart of ui/hal_mac.h, which is where macOS
// keeps what only it can answer: there is no AudioObject on iOS, and there is no
// AVAudioSession on macOS (every member is API_UNAVAILABLE(macos)), so each
// platform has exactly one such file and both halves of it share that one.

#ifndef S_MU2000_IOS_SESSION_IOS_H
#define S_MU2000_IOS_SESSION_IOS_H

#import <AVFAudio/AVFAudio.h>

#include <functional>
#include <string>
#include <vector>

namespace ui::ios {

// Playback, at the machine's rate and with a buffer of about latency_ms.
// Asked, not promised: the session may grant another rate (hardware at 48k),
// and the shared core's resampler covers the difference rather than failing to
// open.
bool session_open_output(int latency_ms, std::string &err);

// PlayAndRecord with DefaultToSpeaker, which is what keeps output where
// Playback had it: without it the speaker goes quiet and sound moves to the
// earpiece the moment recording starts. The switch can blip output; that is
// the OS re-routing, not a bug here.
bool session_open_input(std::string &err);

// ---- The ports, which are also how a device is chosen ---------------------
//
// iOS has no HAL, so a "device" is one of the session's ports: the names the
// menu shows come from here and the choice comes back here, which is why both
// live in this file - one place that knows what the session offers, rather than
// each half walking the list itself and disagreeing.

// The available inputs, by port name. Not the current route's inputs, which
// are empty while the session is Playback (output started at boot, before any
// input was ever picked): availableInputs is category-independent hardware
// capability, so the microphone lists with no permission and no session change.
std::vector<std::string> input_port_names();

// The current route's outputs, by port name: one route at a time, so this is
// the one device there is to play through.
std::vector<std::string> output_port_names();

// Ask the session to record from that port. True when it took the request, and
// also when the name is not one the session offers - the session's own choice
// then stands, which is what the menu expects when a remembered device is gone.
// macOS pins a device by setting a property on the unit the engine hands out;
// here the session is the only thing that can be asked.
bool set_preferred_input(const std::string &name);

// May we record? Asked here and only here, so a launch that never records never
// prompts. Undetermined asks and fails this pick with "pick again"; denied
// fails with where to re-allow.
bool request_mic_permission(std::string &err);

// ---- Watching the session, which restarts an engine that stopped ------------
//
// The session stops an engine and leaves it stopped: the route changes
// (headphones plugged, the category flipped by picking the mic) or an
// interruption ends and may be resumed (a call, or Siri). Nothing restarts the
// engine by itself, so each half of the back end registers a callback and calls
// its own restart() from it - see watch_output_session / watch_input_session in
// ui/audio_apple.h for the side of the contract this answers.
//
// Two registers rather than one callback, because the two engines are
// independent: output can be running while input is closed, and clearing one
// must not silence the other. A function rather than an observer token the
// caller holds because the session is process-wide - a token kept per engine
// would mean a block outliving the engine it points at. Installed once per
// process and re-pointed on every call, so start() registers and stop() clears
// with no lifetime left dangling either way. Passing an empty function is how a
// caller says it is done.
void watch_output(const std::function<void()> &on_change);
void watch_input(const std::function<void()> &on_change);

} // namespace ui::ios

#endif // S_MU2000_IOS_SESSION_IOS_H