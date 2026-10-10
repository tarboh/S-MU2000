// license:BSD-3-Clause
//
// What macOS has where iOS has an AVAudioSession: nothing to watch.
//
// This is the counterpart of ui/session_ios.mm, and it exists for the same
// reason. iOS's session stops an engine behind our back - headphones appear, a
// call arrives, the category flips when the mic is picked - and nothing restarts
// it, so each half of the shared core registers a callback and calls its own
// restart() from it. The contract is the two watch_*_session hooks in
// ui/audio_apple.h, which this file answers.
//
// The answer is that there is nothing to watch: macOS has no AVAudioSession
// (every member is API_UNAVAILABLE(macos)) and nothing stops an AudioUnit behind
// our back. A device that goes away is the HAL's business, and the render
// callback keeps running into the void until the next one arrives.
//
// One file rather than one definition in each half because both directions need
// both hooks and two definitions would collide at link time - the same reason
// ui/hal_mac.h's queries are inline. It is a .cpp rather than more of
// ui/hal_mac.h because audio_apple.mm, which calls the hooks, is compiled on iOS
// too and must not see a macOS-only header.

#include "ui/audio_apple.h"

#include <functional>

namespace ui::apple {

void watch_output_session(const std::function<void()> &)
{
}

void watch_input_session(const std::function<void()> &)
{
}

} // namespace ui::apple
