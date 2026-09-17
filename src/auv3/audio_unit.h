// license:BSD-3-Clause
//
// The AUv3's audio unit. What makes the sound is src/vst3/engine.h -- the same
// engine the VST3 and the AUv2 run (doc/auv3.md).

#ifndef S_MU2000_AUV3_AUDIO_UNIT_H
#define S_MU2000_AUV3_AUDIO_UNIT_H

#pragma once

#import <AudioToolbox/AudioToolbox.h>

NS_ASSUME_NONNULL_BEGIN

/// One MU2000. **Its ports are the real machine's jacks:**
///
///   output 0   MAIN OUT L/R (PHONES and DIGITAL OUT carry the same signal)
///   input 0    A/D INPUT (AD1 on the left, AD2 on the right)
///   MIDI in    cable 0 = MIDI IN A (parts 1-16), cable 1 = MIDI IN B (17-32)
///   MIDI out   MIDI OUT (SCI ch0 of the SH7043); the firmware's replies
@interface SMU2000AudioUnit : AUAudioUnit

/// The engine this instance is running, as a smu2000::plug::engine *.
///
/// **Not for hosts.** It is how the view controller in this same extension
/// (factory.mm) reaches the engine to build the panel; nothing outside the
/// extension can use the pointer, since an AUv3 usually runs in another
/// process. The AUv2 answers the same question through a property instead
/// (src/au/editor.h), because there the host's handle is not the instance.
- (nullable void *)enginePointer NS_RETURNS_INNER_POINTER;

@end

NS_ASSUME_NONNULL_END

#endif // S_MU2000_AUV3_AUDIO_UNIT_H
