// license:BSD-3-Clause
//
// S-MU2000 as an Audio Unit v2 (type aumu = MusicDevice).
//
// It runs the **same engine** as the VST3 plug-in (src/vst3/plugin.cpp): making
// the audio, finding and booting the ROMs, resampling to the host's rate and
// packing the machine's state all happen there. What is here is only the AU
// side of the host interface.
//
//   engine.h         boot / MIDI / fill / state (no VST3 types appear in it)
//   vst3/plugin.cpp  the VST3 side (IComponent, IAudioProcessor, IEditController)
//   au/plugin.cpp    this file: the AU side (AudioComponentPlugInInterface)
//
// For the same reason Steinberg's public.sdk (GPLv3) was left out and only
// pluginterfaces (MIT) was brought in, Apple's AudioUnitSDK is not vendored
// either. An AUv2's wiring is a fixed table, so the parts that are needed are
// written out here.
//
// An AUv2 is never dlopen'd. A host reads the bundle in the Components
// directory, finds the name given by factoryFunction in Info.plist's
// AudioComponents entry, and calls that symbol -- SMU2000AUFactory below. The
// AudioComponentPlugInInterface it hands back has Open / Close / Lookup, and
// Lookup is the "selector number -> function" table.
//
// Checking it:
//   make au && make au-probe
//   build/aubprobe build/S-MU2000.component song.mid out.wav   (own host)
//   auval -v aumu SMU2 Trbh                                    (Apple's validator)

#include "editor.h"
#include "state.h"
#include "ui/midi_split.h"
#include "mu2000.h"
#include "vst3/engine.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

// ---- Identity. Once chosen these cannot change: a host would stop finding the
//      plug-in, and saved sessions would no longer match it
constexpr OSType kType         = 'aumu';
constexpr OSType kSubtype      = 'SMU2';
// The manufacturer code is mixed case on purpose: auval treats an all-lowercase
// manufacturer as an error ("should have at least one non-lower case
// character") and refuses to open the unit at all, however well it works
constexpr OSType kManufacturer = 'Trbh';
constexpr UInt32 kVersion      = 0x00010000;      // 0.1.0

// The one factory preset. The name PresentPreset returns and the name inside
// ClassInfo have to agree, because auval compares them
constexpr const char *kPresetName = "S-MU2000";

// aumu's element rules: the audio output is element 0 of the output scope, and
// the MIDI input is element 1 of the input scope. The latter has no stream
// format, so it never shows up as a property -- MIDI arrives through the
// MusicDevice entry points
constexpr UInt32 kOutputElement = 0;
constexpr UInt32 kGlobalElement = 0;

// ---- Parameters
//
// An AU has no MIDI-number-to-parameter convention like VST3's IMidiMapping,
// so there is nothing to map and no reason to build the VST3 side's 2096 of
// them. These are only what a host's generic panel can usefully show; MIDI goes
// in through the MusicDevice entry points instead
enum : AudioUnitParameterID {
	kParamGain   = 0,
	kParamStatus = 1,
	kParamCount  = 2,
};

constexpr UInt32 kMaxFramesDefault = 1156;
// How many MIDI messages may be waiting between two render blocks. Past this
// the oldest is dropped rather than growing the queue without limit
constexpr size_t kMidiReserveMsgs  = 512;
// MIDI OUT. The machine's own transmit buffer is 4096 bytes, so nothing longer
// than that can come out of one block; the packet list gets room for the same
// bytes plus the per-packet headers
constexpr size_t kMidiOutBytes       = 4096;
constexpr size_t kMidiOutPacketBytes = 8192;

// ---- Where MIDI from the host waits
//
// MusicDeviceMIDIEvent is not necessarily called from the audio thread, while
// the engine's midi() is audio-thread-only (it touches the pre-boot queue). So
// events are parked here and drained inside Render, which takes the lock with
// try_lock: if it is busy they are simply picked up in the next block
struct msg
{
	UInt32 offset = 0;
	std::vector<UInt8> bytes;
};

AudioStreamBasicDescription default_format()
{
	AudioStreamBasicDescription f{};
	f.mSampleRate       = smu2000::vst3::NATIVE_RATE;
	f.mFormatID         = kAudioFormatLinearPCM;
	// The two flags come from different anonymous enums, so the or needs a cast
	f.mFormatFlags      = AudioFormatFlags(kAudioFormatFlagsNativeFloatPacked) |
	                      AudioFormatFlags(kAudioFormatFlagIsNonInterleaved);
	f.mChannelsPerFrame = 2;
	f.mBitsPerChannel   = 32;
	f.mFramesPerPacket  = 1;
	f.mBytesPerFrame    = 4;
	f.mBytesPerPacket   = 4;
	return f;
}

} // namespace


// ---------------------------------------------------------------------------
// The plugin object.
//
// `iface` must stay first: the host is handed &iface, and hands that same
// pointer back as `self` for every method below, so the cast only works if the
// two share an address.

struct au_instance
{
	AudioComponentPlugInInterface iface{};
	AudioComponentInstance instance = nullptr;

	smu2000::vst3::engine eng;

	// ---- Settings
	AudioStreamBasicDescription out_format{};
	UInt32 max_frames = kMaxFramesDefault;
	UInt32 render_quality = 0;
	bool   initialized = false;
	AudioUnitParameterValue gain = 1.0f;

	// ---- Render notifies the host asked to be called back through
	struct notify { AURenderCallback proc; void *ref; };
	std::vector<notify> render_notifies;

	// ---- Property listeners. An id of 0 means "every property"
	struct watch { AudioUnitPropertyID id; AudioUnitPropertyListenerProc proc; void *ref; };
	std::vector<watch> watchers;

	// ---- The MIDI hand-off: midi_in is the host's side, midi_work the audio's
	std::mutex midi_mutex;
	std::vector<msg> midi_in;
	std::vector<msg> midi_work;

	// ---- MIDI OUT: the machine's own OUT jack (SCI ch0 of the SH7043).
	//
	// The firmware answers XG enquiries and dump requests there, and an AUv2
	// passes that on through a callback the host installs with
	// kAudioUnitProperty_MIDIOutputCallback. Nothing is sent if the host did
	// not install one, which most do not.
	//
	// The engine hands back the raw byte stream the real cable carries, so it
	// has to be cut back into messages before it can go in a MIDIPacketList --
	// that is ui::midi_split. Both buffers are sized in au_open so that the
	// render thread never allocates
	AUMIDIOutputCallbackStruct midi_out_cb{};
	bool midi_out_cb_set = false;
	std::vector<UInt8> midi_out_bytes;       // raw, straight from the engine
	std::vector<Byte>  midi_out_packets;     // the MIDIPacketList built from them
	ui::midi_split     midi_out_split;

	void notify_all(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element)
	{
		for (const watch &w : watchers)
			if (w.id == id || w.id == 0)
				w.proc(w.ref, instance, id, scope, element);
	}

	// Make one block. n never exceeds frames
	void produce(float *left, float *right, UInt32 n)
	{
		if (n == 0)
			return;
		if (eng.state() != smu2000::vst3::status::ready) {
			std::memset(left, 0, size_t(n) * sizeof(float));
			std::memset(right, 0, size_t(n) * sizeof(float));
			return;
		}
		// in_l / in_r carry this block of the A/D INPUT bus, if the host fed one
		eng.fill(left, right, int(n), in_l.empty() ? nullptr : in_l.data(),
		         in_r.empty() ? nullptr : in_r.data());
	}

	// ---- A/D INPUT
	//
	// The host feeds this with a callback on the input bus
	// (kAudioUnitProperty_SetRenderCallback). One block is pulled here and handed
	// to engine::fill(), which resamples it to the machine's rate the way the VST3
	// build resamples its input bus. Left is AD1 and right is AD2
	//
	// The bus is answerable but not counted -- ElementCount for the input scope is
	// 0. A host that counts buses therefore offers no input and the unit still
	// renders under a sandboxed host, which is what an instrument has to do; the
	// full explanation is on that case in prop_get
	AURenderCallbackStruct in_cb{};
	bool in_cb_set = false;
	std::vector<float> in_l, in_r;

	// Returns the host callback's status. A host that fails to hand over input
	// expects to hear about it from AURender, so the error is passed up rather
	// than swallowed; the block it did not fill is silenced so nothing stale is
	// played in its place
	OSStatus pull_input(UInt32 frames, const AudioTimeStamp *ts)
	{
		if (!in_cb_set || !in_cb.inputProc)
			return noErr;
		if (in_l.size() < frames) {
			in_l.resize(frames);
			in_r.resize(frames);
		}
		// AudioBufferList declares room for one buffer, so one more is added by
		// hand: the unit's input is non-interleaved 32-bit float, which is what
		// default_format() asks for and what a host rendering into it will have
		struct { AudioBufferList list; AudioBuffer second; } bl{};
		bl.list.mNumberBuffers = 2;
		bl.list.mBuffers[0].mNumberChannels = 1;
		bl.list.mBuffers[0].mDataByteSize = frames * sizeof(float);
		bl.list.mBuffers[0].mData = in_l.data();
		bl.second.mNumberChannels = 1;
		bl.second.mDataByteSize = frames * sizeof(float);
		bl.second.mData = in_r.data();
		AudioUnitRenderActionFlags f = 0;
		// The render's own timestamp travels with the pull: it says where in the
		// stream this block sits, and a host handed 0 in its place sees the input
		// arrive at the wrong time (auval: "AU is not passing time stamp correctly")
		AudioTimeStamp at = ts ? *ts : AudioTimeStamp{};
		if (!(at.mFlags & kAudioTimeStampSampleTimeValid)) {
			at.mSampleTime = 0;
			at.mFlags |= kAudioTimeStampSampleTimeValid;
		}
		const OSStatus rc = in_cb.inputProc(in_cb.inputProcRefCon, &f, &at, 0, frames, &bl.list);
		if (rc != noErr) {
			// No input this block: silence, rather than a stale one
			std::fill(in_l.begin(), in_l.begin() + frames, 0.0f);
			std::fill(in_r.begin(), in_r.begin() + frames, 0.0f);
		}
		return rc;
	}

	// Take what has piled up. midi_work is cleared first every time, so a block
	// that could not get the lock never replays the previous block
	void take_midi()
	{
		midi_work.clear();
		std::unique_lock<std::mutex> lock(midi_mutex, std::try_to_lock);
		if (lock.owns_lock() && !midi_in.empty())
			midi_work.swap(midi_in);
	}

	// ---- Which channels have actually sounded, per port.
	//
	// Stopping the transport used to send all-sound-off and all-notes-off to
	// every channel. That is 192 bytes per port, and the emulated MIDI line
	// carries them at 31250bps -- 61 ms during which anything queued behind
	// waits, so the first note after a restart arrived late and every note
	// after it was on time (issue #15, fixed for the VST3 in 90e7960).
	//
	// Remembering which channels were used costs one OR per note-on and turns
	// the burst into only what is needed. The AUv2's MIDI has no cable number,
	// so in practice only port 0 is ever set; the array is per port anyway so
	// that this reads the same as the VST3 side
	uint16_t sounded[mu2000::MIDI_PORTS] = {};

	void note_sounded(const UInt8 *bytes, size_t n, int port)
	{
		// A note-on with a non-zero velocity. Note-off and a zero-velocity
		// note-on cannot start a voice, so they do not need silencing later
		if (n >= 3 && (bytes[0] & 0xf0) == 0x90 && bytes[2])
			sounded[port] |= uint16_t(1u << (bytes[0] & 0x0f));
	}

	// Silence what has sounded, and forget it. Called when the host resets or
	// switches preset
	void hush()
	{
		uint16_t mask[mu2000::MIDI_PORTS];
		bool any = false;
		for (int p = 0; p < mu2000::MIDI_PORTS; p++) {
			mask[p] = sounded[p];
			sounded[p] = 0;
			any = any || mask[p];
		}
		if (any)
			eng.all_notes_off(mask, mu2000::MIDI_PORTS);
	}

	void queue(UInt32 offset, const UInt8 *bytes, size_t n)
	{
		note_sounded(bytes, n, 0);
		std::lock_guard<std::mutex> lock(midi_mutex);
		if (midi_in.size() >= kMidiReserveMsgs)
			midi_in.erase(midi_in.begin());     // overflow: drop the oldest
		msg m;
		m.offset = offset;
		m.bytes.assign(bytes, bytes + n);
		midi_in.push_back(std::move(m));
	}
};


// ---------------------------------------------------------------------------
// The three entry points

namespace {

OSStatus au_open(void *self, AudioComponentInstance instance)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;

	au->instance = instance;
	au->out_format = default_format();
	au->eng.set_output_rate(smu2000::vst3::NATIVE_RATE);

	// Reserve up front so the queue never makes the audio thread allocate
	au->midi_in.reserve(kMidiReserveMsgs);
	au->midi_work.reserve(kMidiReserveMsgs);
	// The same for MIDI OUT: the raw bytes and the packet list they are built
	// into are both sized once, here
	au->midi_out_bytes.assign(kMidiOutBytes, 0);
	au->midi_out_packets.assign(kMidiOutPacketBytes, 0);

	// Find and read the ROMs and start booting on another thread. Returns at once
	au->eng.start();
	return noErr;
}

OSStatus au_close(void *self)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	au->eng.set_processing(false);
	delete au;
	return noErr;
}


// ---------------------------------------------------------------------------
// Making sound

OSStatus render_block(au_instance *au, AudioUnitRenderActionFlags *flags,
                      const AudioTimeStamp *ts, UInt32 frames, AudioBufferList *io);

OSStatus au_render(void *self, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                   UInt32 bus, UInt32 frames, AudioBufferList *io)
{
	(void)bus;
	auto *au = static_cast<au_instance *>(self);
	if (!au || !io)
		return kAudio_ParamError;
	if (io->mNumberBuffers == 0)
		return noErr;
	return render_block(au, flags, ts, frames, io);
}

// What the firmware sent out of MIDI OUT during this block, handed to the host.
//
// Called once at the end of a render, on the audio thread, and only if the host
// installed a callback. **Nothing here allocates**: both buffers were sized in
// au_open, and a message too long for what is left is dropped rather than
// growing the packet list
void drain_midi_out(au_instance *au, const AudioTimeStamp *ts)
{
	if (!au->midi_out_cb_set || !au->midi_out_cb.midiOutputCallback)
		return;
	const size_t got = au->eng.midi_out(au->midi_out_bytes.data(),
	                                    au->midi_out_bytes.size());
	if (!got)
		return;

	MIDIPacketList *list = reinterpret_cast<MIDIPacketList *>(au->midi_out_packets.data());
	MIDIPacket *pkt = MIDIPacketListInit(list);

	struct ctx { MIDIPacketList *list; MIDIPacket *pkt; size_t cap; } c{ list, pkt,
	                                                                     au->midi_out_packets.size() };
	// ui::midi_split hands over one whole message at a time, which is what
	// MIDIPacketListAdd wants; a byte stream is not
	au->midi_out_split.feed(au->midi_out_bytes.data(), got,
	                        [](void *p, const uint8_t *bytes, size_t n) {
		auto *k = static_cast<ctx *>(p);
		if (!k->pkt)
			return;                    // the list filled up; the rest is dropped
		k->pkt = MIDIPacketListAdd(k->list, k->cap, k->pkt, 0, n, bytes);
	}, &c);

	if (list->numPackets)
		au->midi_out_cb.midiOutputCallback(au->midi_out_cb.userData, ts, 0, list);
}

// Let the host watch the render, before and after. Pre-render carries the action
// flags in; post-render is told what actually happened
void tell_notifies(au_instance *au, UInt32 phase, AudioUnitRenderActionFlags *flags,
                   const AudioTimeStamp *ts, UInt32 frames, AudioBufferList *io)
{
	for (const au_instance::notify &n : au->render_notifies) {
		AudioUnitRenderActionFlags f = phase;
		n.proc(n.ref, &f, ts, 0, frames, io);
		if (flags && phase == kAudioUnitRenderAction_PreRender)
			*flags |= f;
	}
}

OSStatus render_block(au_instance *au, AudioUnitRenderActionFlags *flags,
                      const AudioTimeStamp *ts, UInt32 frames, AudioBufferList *io)
{
	if (!au->initialized) {
		if (flags)
			*flags |= kAudioUnitRenderAction_OutputIsSilence;
		return kAudioUnitErr_Uninitialized;
	}
	if (frames == 0)
		return noErr;
	// A host is required to respect MaximumFramesPerSlice. Rather than quietly
	// making less, refuse (auval fails a unit that answers noErr here)
	if (frames > au->max_frames)
		return kAudioUnitErr_TooManyFramesToProcess;

	// Interleaved output (one buffer holding both channels) is made into two
	// scratch buffers and written back. They cannot hold the whole block, so it
	// goes 256 frames at a time
	const UInt32 ch0 = io->mBuffers[0].mNumberChannels;
	const bool interleaved = (io->mNumberBuffers == 1 && ch0 > 1);
	if (interleaved) {
		tell_notifies(au, kAudioUnitRenderAction_PreRender, flags, ts, frames, io);
		au->take_midi();
		const OSStatus in_rc = au->pull_input(frames, ts);
		if (in_rc != noErr) {
			// The host could not produce its input: pass its error up (auval fails a
			// unit that hides it) and hand back silence rather than a stale block
			if (io->mBuffers[0].mData)
				std::memset(io->mBuffers[0].mData, 0,
				            size_t(frames) * ch0 * sizeof(float));
			if (flags)
				*flags |= kAudioUnitRenderAction_OutputIsSilence;
			tell_notifies(au, kAudioUnitRenderAction_PostRender, nullptr, ts, frames, io);
			return in_rc;
		}

		float l[256], r[256];
		auto *dst = static_cast<float *>(io->mBuffers[0].mData);
		UInt32 done = 0;
		for (const msg &m : au->midi_work) {
			const UInt32 at = std::min<UInt32>(std::max<UInt32>(m.offset, done), frames);
			while (done < at) {
				const UInt32 n = std::min<UInt32>(256, at - done);
				au->produce(l, r, n);
				for (UInt32 i = 0; i < n; i++) {
					dst[(done + i) * ch0 + 0] = l[i] * au->gain;
					dst[(done + i) * ch0 + 1] = r[i] * au->gain;
				}
				done += n;
			}
			if (!m.bytes.empty())
				au->eng.midi(m.bytes.data(), m.bytes.size());
		}
		while (done < frames) {
			const UInt32 n = std::min<UInt32>(256, frames - done);
			au->produce(l, r, n);
			for (UInt32 i = 0; i < n; i++) {
				dst[(done + i) * ch0 + 0] = l[i] * au->gain;
				dst[(done + i) * ch0 + 1] = r[i] * au->gain;
			}
			done += n;
		}
		drain_midi_out(au, ts);
		if (flags)
			*flags &= ~kAudioUnitRenderAction_OutputIsSilence;
		tell_notifies(au, kAudioUnitRenderAction_PostRender, nullptr, ts, frames, io);
		return noErr;
	}

	auto *left  = static_cast<float *>(io->mBuffers[0].mData);
	auto *right = io->mNumberBuffers >= 2 ? static_cast<float *>(io->mBuffers[1].mData) : left;
	if (!left)
		return noErr;

	tell_notifies(au, kAudioUnitRenderAction_PreRender, flags, ts, frames, io);

	// Take the MIDI the host sent. If the lock is busy it is picked up in the
	// next block instead, and take this block of the A/D INPUT bus
	au->take_midi();
	const OSStatus in_rc = au->pull_input(frames, ts);
	if (in_rc != noErr) {
		// The host could not produce its input: pass its error up, and silence this
		// block instead of playing whatever the buffers held
		std::memset(left, 0, size_t(frames) * sizeof(float));
		if (right != left)
			std::memset(right, 0, size_t(frames) * sizeof(float));
		if (flags)
			*flags |= kAudioUnitRenderAction_OutputIsSilence;
		tell_notifies(au, kAudioUnitRenderAction_PostRender, nullptr, ts, frames, io);
		return in_rc;
	}
	if (au->midi_work.empty() && au->eng.state() != smu2000::vst3::status::ready) {
		std::memset(left, 0, size_t(frames) * sizeof(float));
		if (right != left)
			std::memset(right, 0, size_t(frames) * sizeof(float));
		if (flags)
			*flags |= kAudioUnitRenderAction_OutputIsSilence;
		tell_notifies(au, kAudioUnitRenderAction_PostRender, nullptr, ts, frames, io);
		return noErr;
	}

	std::sort(au->midi_work.begin(), au->midi_work.end(),
	          [](const msg &a, const msg &b) { return a.offset < b.offset; });

	// In time order: make up to each event, inject it, carry on
	UInt32 done = 0;
	for (const msg &m : au->midi_work) {
		const UInt32 at = std::min<UInt32>(std::max<UInt32>(m.offset, done), frames);
		if (at > done) {
			au->produce(left + done, right + done, at - done);
			done = at;
		}
		if (!m.bytes.empty())
			au->eng.midi(m.bytes.data(), m.bytes.size());
	}
	if (done < frames)
		au->produce(left + done, right + done, frames - done);

	// Output level. The engine's own gain is there for the panel's sake, so it is
	// applied here as well, where the host can hear it
	if (au->gain != 1.0f) {
		for (UInt32 i = 0; i < frames; i++) {
			left[i] *= au->gain;
			if (right != left)
				right[i] *= au->gain;
		}
	}

	// What the firmware sent back out of MIDI OUT while that was made
	drain_midi_out(au, ts);

	if (flags)
		*flags &= ~kAudioUnitRenderAction_OutputIsSilence;

	tell_notifies(au, kAudioUnitRenderAction_PostRender, nullptr, ts, frames, io);
	return noErr;
}


// ---------------------------------------------------------------------------
// Parameters

void param_name(AudioUnitParameterID id, CFStringRef *out)
{
	*out = CFStringCreateWithCString(kCFAllocatorDefault,
	                                 id == kParamGain ? "Output Level" : "Status",
	                                 kCFStringEncodingUTF8);
}

bool param_info(AudioUnitParameterID id, AudioUnitParameterInfo *out)
{
	if (id >= kParamCount)
		return false;
	std::memset(out, 0, sizeof(*out));
	out->flags = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable |
	             kAudioUnitParameterFlag_HasCFNameString |
	             kAudioUnitParameterFlag_CFNameRelease;
	param_name(id, &out->cfNameString);
	out->unit = kAudioUnitParameterUnit_LinearGain;
	out->minValue = 0.0f;
	out->maxValue = 1.0f;
	out->defaultValue = 1.0f;
	if (id == kParamStatus) {
		out->flags &= ~kAudioUnitParameterFlag_IsWritable;
		out->unit = kAudioUnitParameterUnit_Indexed;
		out->minValue = 0.0f;
		out->maxValue = 2.0f;
		out->defaultValue = 0.0f;
	}
	return true;
}

AudioUnitParameterValue param_get(au_instance *au, AudioUnitParameterID id)
{
	switch (id) {
	case kParamGain:   return au->gain;
	case kParamStatus: return au->eng.state() == smu2000::vst3::status::ready ? 1.0f
	                        : au->eng.state() == smu2000::vst3::status::failed ? 2.0f : 0.0f;
	default: break;
	}
	return 0.0f;
}

void param_set(au_instance *au, AudioUnitParameterID id, AudioUnitParameterValue v)
{
	switch (id) {
	case kParamGain:
		au->gain = std::clamp(v, 0.0f, 1.0f);
		au->eng.panel().set_gain(au->gain);
		au->notify_all(kAudioUnitProperty_ParameterStringFromValue, kAudioUnitScope_Global, id);
		break;
	default:
		break;
	}
}


// ---------------------------------------------------------------------------
// Properties
//
// As much of what AUBase does as is needed. It is written as a table lookup
// because GetPropertyInfo, GetProperty and SetProperty have to reach exactly
// the same verdict about what exists

struct prop_answer
{
	UInt32 size = 0;
	Boolean writable = false;
};

// The gate for a property that answers only in the global scope.
//
// If such a property answered in every scope, a host would read it as if the
// value belonged to that scope -- and auval says so out loud: it checks that
// Latency is *invalid* for Output/Part/Note. Returning "no such property" for
// the wrong scope is not the same answer as returning "wrong scope", and auval
// distinguishes them
// The parameter properties ask with the **parameter number in the element
// field**, so only the scope is checked here. Checking the element as well would
// make every parameter above 0 answer "no such element"
bool want_global_scope(AudioUnitScope scope, OSStatus &err)
{
	if (scope != kAudioUnitScope_Global) {
		err = kAudioUnitErr_InvalidScope;
		return false;
	}
	return true;
}

bool want_global(AudioUnitScope scope, AudioUnitElement element, OSStatus &err)
{
	if (scope != kAudioUnitScope_Global) {
		err = kAudioUnitErr_InvalidScope;
		return false;
	}
	if (element != kGlobalElement) {
		err = kAudioUnitErr_InvalidElement;
		return false;
	}
	return true;
}

OSStatus prop_info(au_instance *au, AudioUnitPropertyID id, AudioUnitScope scope,
                   AudioUnitElement element, prop_answer &out)
{
	OSStatus err = noErr;
	(void)au;

	switch (id) {
	case kAudioUnitProperty_ClassInfo:
	case kAudioUnitProperty_ClassInfoFromDocument:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(CFPropertyListRef);
		out.writable = true;
		return noErr;

	case kAudioUnitProperty_MaximumFramesPerSlice:
	case kAudioUnitProperty_RenderQuality:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(UInt32);
		out.writable = true;
		return noErr;

	case kAudioUnitProperty_Latency:
	case kAudioUnitProperty_TailTime:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(Float64);
		out.writable = false;
		return noErr;

	case kAudioUnitProperty_PresentPreset:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(AUPreset);
		out.writable = true;
		return noErr;

	// The element count is asked of every scope. Global is 1 and the audio output
	// is 1, as MusicDeviceBase answers; the input scope answers 0 -- see the long
	// note in prop_get about what that number does to an out-of-process host
	case kAudioUnitProperty_ElementCount:
		if (element != kGlobalElement)
			return kAudioUnitErr_InvalidElement;
		if (scope > kAudioUnitScope_LayerItem)
			return kAudioUnitErr_InvalidScope;
		out.size = sizeof(UInt32);
		out.writable = false;
		return noErr;

	// The host's way of feeding the A/D INPUT bus: it sets this, then this unit's
	// own Render calls the procedure once per block (pull_input). Answering here
	// matters for the same reason StreamFormat does -- a property that SetProperty
	// accepts but GetPropertyInfo calls "no such property" is refused for real,
	// and auval reports the unit as unconfigurable (kAudioUnitErr_InvalidProperty)
	case kAudioUnitProperty_SetRenderCallback:
	case kAudioUnitProperty_MakeConnection:
		if (scope != kAudioUnitScope_Input)
			return kAudioUnitErr_InvalidScope;
		if (element != 0)
			return kAudioUnitErr_InvalidElement;
		out.size = sizeof(AURenderCallbackStruct);
		out.writable = true;
		return noErr;

	case kAudioUnitProperty_ParameterList:
		if (!want_global(scope, element, err))
			return err;
		out.size = kParamCount * sizeof(AudioUnitParameterID);
		out.writable = false;
		return noErr;

	case kAudioUnitProperty_ParameterInfo:
		if (!want_global_scope(scope, err))
			return err;
		if (element >= kParamCount)
			return kAudioUnitErr_InvalidParameter;
		out.size = sizeof(AudioUnitParameterInfo);
		out.writable = false;
		return noErr;

	case kAudioUnitProperty_ParameterStringFromValue:
		if (!want_global_scope(scope, err))
			return err;
		if (element >= kParamCount)
			return kAudioUnitErr_InvalidParameter;
		out.size = sizeof(CFStringRef);
		out.writable = false;
		return noErr;

	case kAudioUnitProperty_ParameterValueFromString:
		if (!want_global_scope(scope, err))
			return err;
		if (element >= kParamCount)
			return kAudioUnitErr_InvalidParameter;
		out.size = sizeof(AudioUnitParameterValue);
		out.writable = true;
		return noErr;

	// One audio output. The global scope answers the same thing, as
	// DLSMusicDevice does. **There is no stream format for the MIDI input.**
	// Answering with a "MIDI stream" here makes auval treat it as the input
	// format and fail the unit for being initialisable at 3 channels
	case kAudioUnitProperty_StreamFormat:
		// The input bus carries the A/D INPUT, at the host's rate like the output.
		//
		// **Answering noErr here is not enough.** The caller reads `writable` and
		// `size` as well as the status, and a unit that says "that property is
		// there" and then "not writable, size 0" is treated as one that cannot be
		// configured at all: auval fails on it with "Cannot Set Input Num Channels:2
		// when unit says it can" (kAudioUnitErr_PropertyNotWritable, -10865) and
		// reports AU VALIDATION FAILED, after which most hosts refuse to instantiate
		// the unit -- no audio and no editor, which is exactly what that looks like
		if (scope == kAudioUnitScope_Input) {
			if (element != 0)
				return kAudioUnitErr_InvalidElement;
			out.size = sizeof(AudioStreamBasicDescription);
			out.writable = true;
			return noErr;
		}
		if (scope != kAudioUnitScope_Output && scope != kAudioUnitScope_Global)
			return kAudioUnitErr_InvalidScope;
		if (element != kOutputElement)
			return kAudioUnitErr_InvalidElement;
		out.size = sizeof(AudioStreamBasicDescription);
		out.writable = true;
		return noErr;

	// 2 in (the A/D INPUT bus) and 2 out
	case kAudioUnitProperty_SupportedNumChannels:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(AUChannelInfo);
		out.writable = false;
		return noErr;

	case kMusicDeviceProperty_InstrumentName:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(CFStringRef);
		out.writable = false;
		return noErr;

	// What the host reads to find the editor: a bundle and a class name in it.
	// One view class, so the size is one AudioUnitCocoaViewInfo
	case kAudioUnitProperty_CocoaUI:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(AudioUnitCocoaViewInfo);
		out.writable = false;
		return noErr;

	// Private, and read-only: the editor's way of reaching the engine it has to
	// draw (editor.h). Not something a host has any use for
	case smu2000::au::kEngineProperty:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(void *);
		out.writable = false;
		return noErr;

	// ---- MIDI OUT, the machine's own OUT jack.
	//
	// A host that wants it reads the Info property to learn how many cables
	// there are and what they are called, then writes a callback into the
	// Callback property. Most hosts do neither, and then nothing is sent
	case kAudioUnitProperty_MIDIOutputCallbackInfo:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(CFArrayRef);
		out.writable = false;
		return noErr;

	case kAudioUnitProperty_MIDIOutputCallback:
		if (!want_global(scope, element, err))
			return err;
		out.size = sizeof(AUMIDIOutputCallbackStruct);
		// write-only in practice: a host installs it and never reads it back
		out.writable = true;
		return noErr;

	default:
		break;
	}
	return kAudioUnitErr_InvalidProperty;
}

OSStatus prop_get(au_instance *au, AudioUnitPropertyID id, AudioUnitScope scope,
                  AudioUnitElement element, void *data, UInt32 *size)
{
	if (!data || !size)
		return kAudio_ParamError;

	switch (id) {
	case kAudioUnitProperty_ClassInfo:
	case kAudioUnitProperty_ClassInfoFromDocument: {
		if (*size < sizeof(CFPropertyListRef))
			return kAudioUnitErr_InvalidPropertyValue;
		// The whole machine, packed. state_pack is the same one the VST3 side uses,
		// so it fits in the same 300KB range (raw would be 6MB)
		const std::vector<u8> packed = state_pack(au->eng.save_state());
		CFDataRef d = CFDataCreate(kCFAllocatorDefault, packed.data(), CFIndex(packed.size()));
		CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
			kCFAllocatorDefault, 8, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

		// A host (and auval) reads these four as the unit's identity. Without them
		// the answer is "Class Data does not have required field: <type> ==
		// componentType". The value type is CFNumber, matching Apple's own AUs
		auto put_num = [&](const char *key, SInt32 v) {
			CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v);
			CFDictionarySetValue(dict, CFStringCreateWithCString(kCFAllocatorDefault, key,
			                                                    kCFStringEncodingUTF8), n);
			CFRelease(n);
		};
		put_num(kAUPresetTypeKey, SInt32(kType));
		put_num(kAUPresetSubtypeKey, SInt32(kSubtype));
		put_num(kAUPresetManufacturerKey, SInt32(kManufacturer));
		put_num(kAUPresetVersionKey, SInt32(kVersion));
		CFStringRef nm = CFStringCreateWithCString(kCFAllocatorDefault, kPresetName,
		                                           kCFStringEncodingUTF8);
		CFDictionarySetValue(dict, CFSTR(kAUPresetNameKey), nm);
		CFRelease(nm);

		// And this AU's own contents
		CFDictionarySetValue(dict, CFSTR("S-MU2000"), d);
		CFNumberRef g = CFNumberCreate(kCFAllocatorDefault, kCFNumberFloat32Type, &au->gain);
		CFDictionarySetValue(dict, CFSTR("S-MU2000-OutputLevel"), g);
		CFRelease(g);
		CFRelease(d);

		// The SmartMedia in the slot, by file name. The image itself is not put in
		// the preset (16 to 128 MB), the same as the VST3 side: what is saved is
		// which file was in the machine. Any blocks the machine wrote are flushed
		// to it first, so the project and the file agree
		au->eng.card_flush();
		const std::string card = au->eng.card_path();
		if (!card.empty()) {
			CFStringRef c = CFStringCreateWithCString(kCFAllocatorDefault, card.c_str(),
			                                          kCFStringEncodingUTF8);
			CFDictionarySetValue(dict, CFSTR("S-MU2000-SmartMedia"), c);
			CFRelease(c);
		}
		*static_cast<CFPropertyListRef *>(data) = dict;
		*size = sizeof(CFPropertyListRef);
		return noErr;
	}

	case kAudioUnitProperty_MaximumFramesPerSlice:
		if (*size < sizeof(UInt32))
			return kAudioUnitErr_InvalidPropertyValue;
		*static_cast<UInt32 *>(data) = au->max_frames;
		*size = sizeof(UInt32);
		return noErr;

	case kAudioUnitProperty_RenderQuality:
		if (*size < sizeof(UInt32))
			return kAudioUnitErr_InvalidPropertyValue;
		*static_cast<UInt32 *>(data) = au->render_quality;
		*size = sizeof(UInt32);
		return noErr;

	case kAudioUnitProperty_Latency: {
		if (*size < sizeof(Float64))
			return kAudioUnitErr_InvalidPropertyValue;
		const double rate = au->out_format.mSampleRate > 0.0 ? au->out_format.mSampleRate
		                                                    : smu2000::vst3::NATIVE_RATE;
		*static_cast<Float64 *>(data) = double(au->eng.latency_samples()) / rate;
		*size = sizeof(Float64);
		return noErr;
	}

	// How long the machine keeps sounding after the last note. There is a reverb
	// on it, so a host that stops rendering the moment the MIDI stops would cut
	// the tail off: the VST3 side answers the same four seconds
	// (getTailSamples). Seconds, not samples -- that is what this property is in
	case kAudioUnitProperty_TailTime:
		if (*size < sizeof(Float64))
			return kAudioUnitErr_InvalidPropertyValue;
		*static_cast<Float64 *>(data) = 4.0;
		*size = sizeof(Float64);
		return noErr;

	// The names of the MIDI OUT cables. There is one, the machine's OUT jack.
	// The array belongs to the host once it has been handed over
	case kAudioUnitProperty_MIDIOutputCallbackInfo: {
		if (*size < sizeof(CFArrayRef))
			return kAudioUnitErr_InvalidPropertyValue;
		CFStringRef name = CFSTR("MIDI Out");
		CFArrayRef arr = CFArrayCreate(kCFAllocatorDefault,
		                               reinterpret_cast<const void **>(&name), 1,
		                               &kCFTypeArrayCallBacks);
		if (!arr)
			return kAudioUnitErr_InvalidPropertyValue;
		*static_cast<CFArrayRef *>(data) = arr;
		*size = sizeof(CFArrayRef);
		return noErr;
	}

	case kAudioUnitProperty_MIDIOutputCallback: {
		if (*size < sizeof(AUMIDIOutputCallbackStruct))
			return kAudioUnitErr_InvalidPropertyValue;
		*static_cast<AUMIDIOutputCallbackStruct *>(data) = au->midi_out_cb;
		*size = sizeof(AUMIDIOutputCallbackStruct);
		return noErr;
	}

	// Where the editor is. Both references are made fresh here and belong to the
	// host afterwards; the view itself is built by editor_mac.mm
	case kAudioUnitProperty_CocoaUI: {
		if (*size < sizeof(AudioUnitCocoaViewInfo))
			return kAudioUnitErr_InvalidPropertyValue;
		CFURLRef url = nullptr;
		CFStringRef name = nullptr;
		if (!smu2000::au::view_info(&url, &name))
			return kAudioUnitErr_InvalidPropertyValue;
		auto *info = static_cast<AudioUnitCocoaViewInfo *>(data);
		info->mCocoaAUViewBundleLocation = url;
		info->mCocoaAUViewClass[0] = name;
		*size = sizeof(AudioUnitCocoaViewInfo);
		return noErr;
	}

	// Private: the engine this instance is running, for the editor
	case smu2000::au::kEngineProperty:
		if (*size < sizeof(void *))
			return kAudioUnitErr_InvalidPropertyValue;
		*static_cast<void **>(data) = &au->eng;
		*size = sizeof(void *);
		return noErr;

	case kAudioUnitProperty_ElementCount:
		// aumu's rule: Global is 1 and there is one audio output bus, and **no
		// input bus**. The A/D INPUT is still there and still works -- the input
		// scope's format and render callback are answered below, and engine::fill
		// resamples what the host feeds it -- but it is not *counted*, and that
		// one number decides whether a sandboxed host can render this unit at all.
		//
		// A host that runs out of process (AUHostingService -- every sandboxed
		// host, GarageBand among them) builds its graph from this count. An input
		// bus makes for a graph node whose input nobody connected, and then every
		// AudioUnitRender comes back kAudioUnitErr_NoConnection: no sound, and the
		// machine never advances, so the panel sits on its power-on screen.
		// Connecting a callback to that same bus makes the identical unit render
		// (measured both ways), which is the whole difference. Advertising an
		// input bus is a promise to be connected, and an instrument in GarageBand
		// never is
		if (*size < sizeof(UInt32))
			return kAudioUnitErr_InvalidPropertyValue;
		*static_cast<UInt32 *>(data) =
		    (scope == kAudioUnitScope_Global || scope == kAudioUnitScope_Output) ? 1u : 0u;
		*size = sizeof(UInt32);
		return noErr;

	case kAudioUnitProperty_SetRenderCallback:
		// Read back so a host can check what it set (the input bus only)
		if (scope != kAudioUnitScope_Input || element != 0 || *size < sizeof(AURenderCallbackStruct))
			return kAudioUnitErr_InvalidProperty;
		*static_cast<AURenderCallbackStruct *>(data) = au->in_cb;
		*size = sizeof(AURenderCallbackStruct);
		return noErr;

	case kAudioUnitProperty_PresentPreset: {
		if (*size < sizeof(AUPreset))
			return kAudioUnitErr_InvalidPropertyValue;
		// Only the one factory preset exists. A negative number marks it as not
		// coming from a bank, which is what DLSMusicDevice reports too
		auto *p = static_cast<AUPreset *>(data);
		p->presetNumber = -1;
		p->presetName = CFStringCreateWithCString(kCFAllocatorDefault, kPresetName,
		                                          kCFStringEncodingUTF8);
		*size = sizeof(AUPreset);
		return noErr;
	}

	case kAudioUnitProperty_ParameterList: {
		if (*size < kParamCount * sizeof(AudioUnitParameterID))
			return kAudioUnitErr_InvalidPropertyValue;
		auto *out = static_cast<AudioUnitParameterID *>(data);
		for (AudioUnitParameterID i = 0; i < kParamCount; i++)
			out[i] = i;
		*size = kParamCount * sizeof(AudioUnitParameterID);
		return noErr;
	}

	case kAudioUnitProperty_ParameterInfo: {
		if (*size < sizeof(AudioUnitParameterInfo))
			return kAudioUnitErr_InvalidPropertyValue;
		if (!param_info(element, static_cast<AudioUnitParameterInfo *>(data)))
			return kAudioUnitErr_InvalidParameter;
		*size = sizeof(AudioUnitParameterInfo);
		return noErr;
	}

	case kAudioUnitProperty_ParameterStringFromValue: {
		if (*size < sizeof(CFStringRef))
			return kAudioUnitErr_InvalidPropertyValue;
		if (element >= kParamCount)
			return kAudioUnitErr_InvalidParameter;
		const AudioUnitParameterValue v = param_get(au, element);
		CFStringRef s = nullptr;
		if (element == kParamGain) {
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.3f", double(v));
			s = CFStringCreateWithCString(kCFAllocatorDefault, buf, kCFStringEncodingUTF8);
		} else {
			const char *t = v == 1.0f ? "ready" : v == 2.0f ? "failed" : "loading";
			s = CFStringCreateWithCString(kCFAllocatorDefault, t, kCFStringEncodingUTF8);
		}
		*static_cast<CFStringRef *>(data) = s;
		*size = sizeof(CFStringRef);
		return noErr;
	}

	case kAudioUnitProperty_StreamFormat: {
		if (*size < sizeof(AudioStreamBasicDescription))
			return kAudioUnitErr_InvalidPropertyValue;
		// Input element 0 is the A/D INPUT bus. What it answers is the output's
		// format: the engine resamples the input at the rate the host renders at,
		// so the two are the same format by construction
		if (scope == kAudioUnitScope_Input && element != 0)
			return kAudioUnitErr_InvalidElement;
		*static_cast<AudioStreamBasicDescription *>(data) = au->out_format;
		*size = sizeof(AudioStreamBasicDescription);
		return noErr;
	}

	case kAudioUnitProperty_SupportedNumChannels: {
		if (*size < sizeof(AUChannelInfo))
			return kAudioUnitErr_InvalidPropertyValue;
		// Two channels each way: the output, and the A/D INPUT the machine samples
		// from (left is AD1 and right is AD2, as in the VST3 build). The input half
		// stays described here even though ElementCount answers 0 for that scope:
		// this is what a host sets its input format from when it does feed the bus,
		// and it is the honest answer about the unit -- what it must not do is
		// *count* a bus, which is the number an out-of-process host graphs from
		auto *out = static_cast<AUChannelInfo *>(data);
		out[0] = AUChannelInfo{ 2, 2 };
		*size = sizeof(AUChannelInfo);
		return noErr;
	}

	case kMusicDeviceProperty_InstrumentName: {
		if (*size < sizeof(CFStringRef))
			return kAudioUnitErr_InvalidPropertyValue;
		*static_cast<CFStringRef *>(data) =
			CFStringCreateWithCString(kCFAllocatorDefault, "S-MU2000 (MU2000 emulator)",
			                          kCFStringEncodingUTF8);
		*size = sizeof(CFStringRef);
		return noErr;
	}

	default:
		break;
	}
	return kAudioUnitErr_InvalidProperty;
}

// Both routes into the A/D INPUT bus -- kAudioUnitProperty_SetRenderCallback and
// the kAudioUnitProperty_MakeConnection a graph connection arrives as -- carry
// the same AURenderCallbackStruct, so whichever a host uses lands in one place
void au_set_input_cb(au_instance *au, const AURenderCallbackStruct &cb)
{
	au->in_cb = cb;
	au->in_cb_set = cb.inputProc != nullptr;
}

OSStatus prop_set(au_instance *au, AudioUnitPropertyID id, AudioUnitScope scope,
                  AudioUnitElement element, const void *data, UInt32 size)
{
	if (!data)
		return kAudio_ParamError;

	switch (id) {
	case kAudioUnitProperty_ClassInfo:
	case kAudioUnitProperty_ClassInfoFromDocument: {
		if (size < sizeof(CFPropertyListRef))
			return kAudioUnitErr_InvalidPropertyValue;
		CFPropertyListRef plist = *static_cast<CFPropertyListRef const *>(data);
		CFDataRef blob = nullptr;
		CFDictionaryRef dict = nullptr;
		if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
			dict = static_cast<CFDictionaryRef>(plist);
			blob = static_cast<CFDataRef>(const_cast<void *>(
			    CFDictionaryGetValue(dict, CFSTR("S-MU2000"))));
		} else if (plist && CFGetTypeID(plist) == CFDataGetTypeID()) {
			blob = static_cast<CFDataRef>(plist);
		}
		if (!blob)
			return kAudioUnitErr_InvalidPropertyValue;

		if (dict) {
			CFNumberRef g = static_cast<CFNumberRef>(const_cast<void *>(
			    CFDictionaryGetValue(dict, CFSTR("S-MU2000-OutputLevel"))));
			if (g && CFGetTypeID(g) == CFNumberGetTypeID()) {
				float v = 1.0f;
				CFNumberGetValue(g, kCFNumberFloat32Type, &v);
				param_set(au, kParamGain, v);
			}
		}

		std::vector<u8> raw;
		if (!state_unpack(CFDataGetBytePtr(blob), size_t(CFDataGetLength(blob)), raw))
			return kAudioUnitErr_InvalidPropertyValue;

		// Nothing to wait for. A host sets this straight after
		// AudioComponentInstanceNew, while the ROMs are still coming up, and
		// engine::load_state() keeps a restore that arrives that early and lets the
		// machine apply it once it is up. Blocking here instead would hold the
		// host's thread for as long as a cold boot takes
		au->eng.load_state(raw.data(), raw.size());

		// Put the card back in the slot, if there was one and the file is still
		// where it was. A preset with no card ejects whatever was there
		if (dict) {
			CFStringRef c = static_cast<CFStringRef>(const_cast<void *>(
			    CFDictionaryGetValue(dict, CFSTR("S-MU2000-SmartMedia"))));
			if (c && CFGetTypeID(c) == CFStringGetTypeID()) {
				char path[4096] = {};
				if (CFStringGetCString(c, path, sizeof(path), kCFStringEncodingUTF8)) {
					std::string err;
					if (!au->eng.card_insert(path, err))
						au->eng.log_line(("SmartMedia を差せない: " + err).c_str());
				}
			} else if (!au->eng.card_path().empty()) {
				au->eng.card_eject();
			}
		}
		return noErr;
	}

	case kAudioUnitProperty_MaximumFramesPerSlice:
		if (size < sizeof(UInt32))
			return kAudioUnitErr_InvalidPropertyValue;
		au->max_frames = *static_cast<const UInt32 *>(data);
		// The host is waiting to hear about this. auval changes it and fails a
		// unit that does not fire the notification
		au->notify_all(kAudioUnitProperty_MaximumFramesPerSlice, scope, element);
		return noErr;

	case kAudioUnitProperty_RenderQuality:
		if (size < sizeof(UInt32))
			return kAudioUnitErr_InvalidPropertyValue;
		au->render_quality = *static_cast<const UInt32 *>(data);
		return noErr;

	// The host's MIDI OUT callback. Writing a null one takes it away again
	case kAudioUnitProperty_MIDIOutputCallback: {
		if (size < sizeof(AUMIDIOutputCallbackStruct))
			return kAudioUnitErr_InvalidPropertyValue;
		const auto *cb = static_cast<const AUMIDIOutputCallbackStruct *>(data);
		au->midi_out_cb = *cb;
		au->midi_out_cb_set = cb->midiOutputCallback != nullptr;
		au->midi_out_split.reset();
		return noErr;
	}

	case kAudioUnitProperty_PresentPreset:
		// There is only the one factory preset, so choosing one just puts the
		// defaults back
		if (size < sizeof(AUPreset))
			return kAudioUnitErr_InvalidPropertyValue;
		param_set(au, kParamGain, 1.0f);
		au->hush();
		return noErr;

	case kAudioUnitProperty_ParameterValueFromString: {
		if (element >= kParamCount || size < sizeof(CFStringRef))
			return kAudioUnitErr_InvalidParameter;
		const CFStringRef s = *static_cast<const CFStringRef *>(data);
		const double v = s ? CFStringGetDoubleValue(s) : 0.0;
		param_set(au, element, AudioUnitParameterValue(v));
		return noErr;
	}

	case kAudioUnitProperty_SetRenderCallback:
		// The host's way of feeding the A/D INPUT bus. Kept as it stands: it is
		// called from this unit's own Render, once per block (pull_input)
		if (scope != kAudioUnitScope_Input || element != 0 || size < sizeof(AURenderCallbackStruct))
			return kAudioUnitErr_InvalidProperty;
		au_set_input_cb(au, *static_cast<const AURenderCallbackStruct *>(data));
		return noErr;

	// A connection from another node, which is also an AURenderCallbackStruct.
	// Accepting it is what lets a host wire the AU into a graph at all -- auval
	// checks this under "connection semantics" and fails the unit without it
	case kAudioUnitProperty_MakeConnection:
		if (scope != kAudioUnitScope_Input || element != 0 || size < sizeof(AURenderCallbackStruct))
			return kAudioUnitErr_InvalidProperty;
		au_set_input_cb(au, *static_cast<const AURenderCallbackStruct *>(data));
		return noErr;


	case kAudioUnitProperty_StreamFormat: {
		if (size < sizeof(AudioStreamBasicDescription))
			return kAudioUnitErr_InvalidPropertyValue;
		const auto *f = static_cast<const AudioStreamBasicDescription *>(data);

		// The input bus is only checked, not remembered: the engine resamples the
		// input to the machine's rate using its output rate, which is the rate the
		// host renders the unit at anyway (the same arrangement the VST3 uses)
		if (scope == kAudioUnitScope_Input) {
			if (element != 0)
				return kAudioUnitErr_InvalidElement;
			if (f->mFormatID != kAudioFormatLinearPCM ||
			    (f->mFormatFlags & kAudioFormatFlagIsFloat) == 0 ||
			    f->mBitsPerChannel != 32 || f->mChannelsPerFrame != 2)
				return kAudioUnitErr_FormatNotSupported;
			return noErr;
		}

		// Double precision is refused: the engine is single precision throughout, so
		// accepting it and pretending would corrupt the output. Exactly 2 channels
		// keeps this consistent with SupportedNumChannels -- letting 1 through makes
		// the unit initialisable at a format it never advertised, which auval warns
		// about
		if (f->mFormatID != kAudioFormatLinearPCM ||
		    (f->mFormatFlags & kAudioFormatFlagIsFloat) == 0 ||
		    f->mBitsPerChannel != 32 || f->mChannelsPerFrame != 2)
			return kAudioUnitErr_FormatNotSupported;

		au->out_format = *f;
		au->eng.set_output_rate(f->mSampleRate);
		au->notify_all(kAudioUnitProperty_StreamFormat, scope, element);
		return noErr;
	}

	default:
		break;
	}
	return kAudioUnitErr_InvalidProperty;
}


// ---------------------------------------------------------------------------
// The functions Lookup hands out

OSStatus au_initialize(void *self)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;

	// **Wait here for the boot to finish, before any sound is asked for.**
	//
	// Initialize is the AU's "get ready to render", and it is not the audio
	// thread, so waiting is allowed. Returning without it means every block is
	// silence until the machine comes up, and the MIDI that arrives meanwhile
	// only piles up. In a host that renders faster than realtime that silence
	// becomes the first ten-odd seconds of the song, notes and all. With the
	// boot snapshot this returns in milliseconds (doc/auv3.md).
	//
	// A machine that never came up (no ROMs, say) still initializes: it plays
	// silence and says why in the log, which is better than refusing to load
	(void)au->eng.wait_ready(120000);

	au->eng.set_processing(true);
	au->initialized = true;
	return noErr;
}

OSStatus au_uninitialize(void *self)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	au->initialized = false;
	au->eng.set_processing(false);
	return noErr;
}

OSStatus au_get_property_info(void *self, AudioUnitPropertyID id, AudioUnitScope scope,
                              AudioUnitElement element, UInt32 *size, Boolean *writable)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au || !size)
		return kAudio_ParamError;
	prop_answer a;
	const OSStatus st = prop_info(au, id, scope, element, a);
	if (st != noErr)
		return st;
	*size = a.size;
	if (writable)
		*writable = a.writable;
	return noErr;
}

OSStatus au_get_property(void *self, AudioUnitPropertyID id, AudioUnitScope scope,
                         AudioUnitElement element, void *data, UInt32 *size)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	prop_answer a;
	const OSStatus st = prop_info(au, id, scope, element, a);
	if (st != noErr)
		return st;
	return prop_get(au, id, scope, element, data, size);
}

OSStatus au_set_property(void *self, AudioUnitPropertyID id, AudioUnitScope scope,
                         AudioUnitElement element, const void *data, UInt32 size)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	prop_answer a;
	const OSStatus st = prop_info(au, id, scope, element, a);
	if (st != noErr)
		return st;
	if (!a.writable)
		return kAudioUnitErr_PropertyNotWritable;
	return prop_set(au, id, scope, element, data, size);
}

OSStatus au_add_property_listener(void *self, AudioUnitPropertyID id,
                                  AudioUnitPropertyListenerProc proc, void *ref)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au || !proc)
		return kAudio_ParamError;
	au->watchers.push_back({ id, proc, ref });
	return noErr;
}

// Two of these exist because the AU API grew a second one. This is the newer
// form, which can tell listeners of the same procedure apart; the older form
// removes every listener using that procedure
OSStatus au_remove_property_listener_ud(void *self, AudioUnitPropertyID id,
                                        AudioUnitPropertyListenerProc proc, void *ref)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au || !proc)
		return kAudio_ParamError;
	auto &w = au->watchers;
	w.erase(std::remove_if(w.begin(), w.end(), [&](const au_instance::watch &x) {
		return x.proc == proc && x.id == id && x.ref == ref;
	}), w.end());
	return noErr;
}

OSStatus au_remove_property_listener(void *self, AudioUnitPropertyID id,
                                     AudioUnitPropertyListenerProc proc)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au || !proc)
		return kAudio_ParamError;
	auto &w = au->watchers;
	w.erase(std::remove_if(w.begin(), w.end(), [&](const au_instance::watch &x) {
		return x.proc == proc && x.id == id;
	}), w.end());
	return noErr;
}

OSStatus au_add_render_notify(void *self, AURenderCallback proc, void *ref)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au || !proc)
		return kAudio_ParamError;
	au->render_notifies.push_back({ proc, ref });
	return noErr;
}

OSStatus au_remove_render_notify(void *self, AURenderCallback proc, void *ref)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	auto &v = au->render_notifies;
	v.erase(std::remove_if(v.begin(), v.end(),
	                       [&](const au_instance::notify &x) { return x.proc == proc && x.ref == ref; }),
	        v.end());
	return noErr;
}

// Parameters live in the global scope only. Both a host and auval set each
// parameter once per scope and read it back, and a unit that answers in every
// scope makes those copies look like they disagree with one another
OSStatus param_where(AudioUnitScope scope, AudioUnitElement element)
{
	if (scope != kAudioUnitScope_Global)
		return kAudioUnitErr_InvalidScope;
	if (element != kGlobalElement)
		return kAudioUnitErr_InvalidElement;
	return noErr;
}

OSStatus au_get_parameter(void *self, AudioUnitParameterID id, AudioUnitScope scope,
                          AudioUnitElement element, AudioUnitParameterValue *value)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au || !value)
		return kAudio_ParamError;
	const OSStatus st = param_where(scope, element);
	if (st != noErr)
		return st;
	if (id >= kParamCount)
		return kAudioUnitErr_InvalidParameter;
	*value = param_get(au, id);
	return noErr;
}

OSStatus au_set_parameter(void *self, AudioUnitParameterID id, AudioUnitScope scope,
                          AudioUnitElement element, AudioUnitParameterValue value, UInt32 offset)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	(void)offset;
	const OSStatus st = param_where(scope, element);
	if (st != noErr)
		return st;
	if (id >= kParamCount)
		return kAudioUnitErr_InvalidParameter;
	param_set(au, id, value);
	return noErr;
}

OSStatus au_schedule_parameters(void *self, const AudioUnitParameterEvent *events, UInt32 count)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	for (UInt32 i = 0; i < count; i++) {
		const AudioUnitParameterEvent &e = events[i];
		if (param_where(e.scope, e.element) != noErr || e.parameter >= kParamCount)
			continue;
		// A ramp is answered with its start value. This machine's output level is a
		// single multiply, so stepping it would not be audible anyway
		const AudioUnitParameterValue v = e.eventType == kParameterEvent_Ramped
		    ? e.eventValues.ramp.startValue : e.eventValues.immediate.value;
		param_set(au, e.parameter, v);
	}
	return noErr;
}

OSStatus au_reset(void *self, AudioUnitScope scope, AudioUnitElement element)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	(void)scope;
	(void)element;
	au->hush();
	return noErr;
}

OSStatus au_midi_event(void *self, UInt32 status, UInt32 d1, UInt32 d2, UInt32 offset)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	const UInt8 cmd = UInt8(status & 0xf0);
	UInt8 b[3] = { UInt8(status & 0xff), UInt8(d1 & 0x7f), UInt8(d2 & 0x7f) };
	size_t n = 3;
	if (cmd == 0xc0 || cmd == 0xd0)
		n = 2;
	au->queue(offset, b, n);
	return noErr;
}

OSStatus au_sysex(void *self, const UInt8 *data, UInt32 length)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au || !data || length == 0)
		return kAudio_ParamError;
	// By AU's rules the bytes arrive complete, F0 through F7, and the engine
	// takes them in that form (the same as vst3/plugin.cpp)
	au->queue(0, data, length);
	return noErr;
}

OSStatus au_start_note(void *self, MusicDeviceInstrumentID, MusicDeviceGroupID group,
                       NoteInstanceID *id, UInt32 offset, const MusicDeviceNoteParams *params)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	if (id)
		*id = 0;
	if (!params || params->argCount < 2)
		return kAudio_ParamError;
	const UInt8 note = UInt8(int(std::lround(params->mPitch)) & 0x7f);
	const UInt8 vel  = UInt8(int(std::lround(params->mVelocity * 127.0)) & 0x7f);
	const UInt8 b[3] = { UInt8(0x90 | (group & 0x0f)), note, vel };
	au->queue(offset, b, 3);
	return noErr;
}

OSStatus au_stop_note(void *self, MusicDeviceGroupID group, NoteInstanceID, UInt32 offset)
{
	auto *au = static_cast<au_instance *>(self);
	if (!au)
		return kAudio_ParamError;
	// Which note this refers to is not known (NoteInstanceID is not remembered),
	// so the whole channel is stopped -- all-notes-off on that channel, which is
	// what plain MIDI would do anyway
	const UInt8 b[3] = { UInt8(0xb0 | (group & 0x0f)), 123, 0 };
	au->queue(offset, b, 3);
	return noErr;
}

OSStatus au_prepare_instrument(void *self, MusicDeviceInstrumentID, MusicDeviceGroupID, UInt32)
{
	return noErr;
}

OSStatus au_release_instrument(void *self, MusicDeviceInstrumentID, MusicDeviceGroupID, UInt32)
{
	return noErr;
}

} // namespace


// ---------------------------------------------------------------------------
// The selector table. This is the whole of an AU's wiring

namespace {

AudioComponentMethod au_lookup(SInt16 selector)
{
	switch (selector) {
	case kAudioUnitInitializeSelect:            return reinterpret_cast<AudioComponentMethod>(au_initialize);
	case kAudioUnitUninitializeSelect:          return reinterpret_cast<AudioComponentMethod>(au_uninitialize);
	case kAudioUnitGetPropertyInfoSelect:       return reinterpret_cast<AudioComponentMethod>(au_get_property_info);
	case kAudioUnitGetPropertySelect:           return reinterpret_cast<AudioComponentMethod>(au_get_property);
	case kAudioUnitSetPropertySelect:           return reinterpret_cast<AudioComponentMethod>(au_set_property);
	case kAudioUnitAddPropertyListenerSelect:   return reinterpret_cast<AudioComponentMethod>(au_add_property_listener);
	case kAudioUnitRemovePropertyListenerSelect:return reinterpret_cast<AudioComponentMethod>(au_remove_property_listener);
	case kAudioUnitRemovePropertyListenerWithUserDataSelect:
		return reinterpret_cast<AudioComponentMethod>(au_remove_property_listener_ud);
	case kAudioUnitAddRenderNotifySelect:       return reinterpret_cast<AudioComponentMethod>(au_add_render_notify);
	case kAudioUnitRemoveRenderNotifySelect:    return reinterpret_cast<AudioComponentMethod>(au_remove_render_notify);
	case kAudioUnitGetParameterSelect:          return reinterpret_cast<AudioComponentMethod>(au_get_parameter);
	case kAudioUnitSetParameterSelect:          return reinterpret_cast<AudioComponentMethod>(au_set_parameter);
	case kAudioUnitScheduleParametersSelect:    return reinterpret_cast<AudioComponentMethod>(au_schedule_parameters);
	case kAudioUnitRenderSelect:                return reinterpret_cast<AudioComponentMethod>(au_render);
	case kAudioUnitResetSelect:                 return reinterpret_cast<AudioComponentMethod>(au_reset);
	case kMusicDeviceMIDIEventSelect:           return reinterpret_cast<AudioComponentMethod>(au_midi_event);
	case kMusicDeviceSysExSelect:               return reinterpret_cast<AudioComponentMethod>(au_sysex);
	case kMusicDeviceStartNoteSelect:           return reinterpret_cast<AudioComponentMethod>(au_start_note);
	case kMusicDeviceStopNoteSelect:            return reinterpret_cast<AudioComponentMethod>(au_stop_note);
	case kMusicDevicePrepareInstrumentSelect:   return reinterpret_cast<AudioComponentMethod>(au_prepare_instrument);
	case kMusicDeviceReleaseInstrumentSelect:   return reinterpret_cast<AudioComponentMethod>(au_release_instrument);
	default:
		break;
	}
	return nullptr;
}

} // namespace


// ---------------------------------------------------------------------------
// The factory. Info.plist's factoryFunction names this symbol

extern "C"
__attribute__((visibility("default")))
AudioComponentPlugInInterface *SMU2000AUFactory(const AudioComponentDescription *desc)
{
	if (desc && (desc->componentType != kType || desc->componentSubType != kSubtype))
		return nullptr;

	auto *au = new au_instance();
	au->iface.Open   = &au_open;
	au->iface.Close  = &au_close;
	au->iface.Lookup = &au_lookup;
	au->iface.reserved = nullptr;
	return &au->iface;
}

// The four-character identity, readable from outside (aubprobe uses it)
extern "C" __attribute__((visibility("default"))) OSType SMU2000AUType()      { return kType; }
extern "C" __attribute__((visibility("default"))) OSType SMU2000AUSubtype()   { return kSubtype; }
extern "C" __attribute__((visibility("default"))) OSType SMU2000AUManu()      { return kManufacturer; }
extern "C" __attribute__((visibility("default"))) UInt32 SMU2000AUVers()      { return kVersion; }
