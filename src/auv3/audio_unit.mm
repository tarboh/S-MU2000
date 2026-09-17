// license:BSD-3-Clause
//
// The AUv3 itself. The sound is src/vst3/engine.h's engine -- the same one the
// VST3 and the AUv2 run -- so what is here is only the AUv3 way of asking.
//
// **The ports are the real machine's jacks.**
//
//   output 0   MAIN OUT L/R. PHONES and DIGITAL OUT carry the same signal
//   input 0    A/D INPUT. AD1 on the left, AD2 on the right (it feeds sampling
//              and the A/D chain)
//   MIDI in    cable 0 = MIDI IN A (parts 1-16), 1 = B (17-32),
//              2 = C (33-48), 3 = D (49-64). C and D reach the machine over
//              the USB port, which is what the firmware itself uses for them
//   MIDI out   MIDI OUT (SCI ch0 of the SH7043); what the firmware sent
//
// The MU2000 only ever runs at 44100Hz, so the conversion to the host's rate is
// the engine's own windowed sinc. Whatever it costs is reported as latency.
//
// **Nothing is allocated and no lock is taken inside the render block.** The
// buffers are taken in allocateRenderResources.

#import "audio_unit.h"

#import <AVFoundation/AVFoundation.h>

#include "mu2000.h"
#include "ui/midi_split.h"
#include "vst3/engine.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace {

constexpr AUAudioFrameCount MAX_FRAMES = 4096;
// MIDI IN A, B, C and D. Taken from the machine rather than written out, so
// that a change to how many ports it has widens this with it
constexpr int PORTS = mu2000::MIDI_PORTS;

// Room used inside the render block. Nothing here allocates: allocateRender-
// Resources does that once
struct scratch {
	std::vector<float> out_l, out_r;         // for a host that hands over no buffer
	std::vector<float> in_l, in_r;           // where A/D INPUT is pulled into
	// The AudioBufferList the A/D INPUT is pulled with (room for 2 channels)
	uint8_t            in_abl_mem[sizeof(AudioBufferList) + sizeof(AudioBuffer)] = {};
	std::vector<uint8_t> tx;                 // raw MIDI OUT bytes
	ui::midi_split       split;              // which cuts them into messages

	AudioBufferList *in_abl() { return reinterpret_cast<AudioBufferList *>(in_abl_mem); }

	void allocate(AUAudioFrameCount n)
	{
		out_l.assign(n, 0.0f);  out_r.assign(n, 0.0f);
		in_l.assign(n, 0.0f);   in_r.assign(n, 0.0f);
		tx.assign(4096, 0);
		split.reset();
	}
};

// What one message needs to reach MIDIOutputEventBlock
struct emit_ctx {
	AUMIDIOutputEventBlock __unsafe_unretained block;
	AUEventSampleTime when;
};

void emit_one(void *ctx, const uint8_t *bytes, size_t n)
{
	emit_ctx *e = static_cast<emit_ctx *>(ctx);
	if (e->block)
		e->block(e->when, 0, NSInteger(n), bytes);
}

} // namespace


@implementation SMU2000AudioUnit {
	std::unique_ptr<smu2000::plug::engine> _engine;
	std::unique_ptr<scratch>               _scratch;
	AUAudioUnitBusArray                   *_inputBusArray;
	AUAudioUnitBusArray                   *_outputBusArray;
	AUAudioUnitBus                        *_inputBus;
	AUAudioUnitBus                        *_outputBus;
	// Which channels have actually sounded, per port. Silencing every channel
	// on reset costs 192 bytes a port on a 31250bps line -- 61 ms that the next
	// note waits behind (issue #15). Written in the render block, read by reset
	std::atomic<uint16_t>                  _sounded[PORTS];
}

- (instancetype)initWithComponentDescription:(AudioComponentDescription)desc
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError **)outError
{
	self = [super initWithComponentDescription:desc options:options error:outError];
	if (!self)
		return nil;

	AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
	                                                                   channels:2];

	// MAIN OUT. PHONES and DIGITAL OUT carry the same signal
	_outputBus = [[AUAudioUnitBus alloc] initWithFormat:fmt error:nil];
	_outputBus.maximumChannelCount = 2;
	_outputBus.name = @"Main Out";

	// A/D INPUT, the jacks on the back. It feeds sampling and the A/D chain.
	// The machine plays with nothing plugged in, so a host that offers no
	// input is treated as silence rather than refused
	_inputBus = [[AUAudioUnitBus alloc] initWithFormat:fmt error:nil];
	_inputBus.maximumChannelCount = 2;
	_inputBus.name = @"A/D Input";

	_outputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
	                                                         busType:AUAudioUnitBusTypeOutput
	                                                          busses:@[_outputBus]];
	_inputBusArray  = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
	                                                         busType:AUAudioUnitBusTypeInput
	                                                          busses:@[_inputBus]];

	for (std::atomic<uint16_t> &v : _sounded)
		v.store(0, std::memory_order_relaxed);

	_engine = std::make_unique<smu2000::plug::engine>();
	_scratch = std::make_unique<scratch>();

	// Finding and reading the ROMs and booting happen on another thread; this
	// returns at once. allocateRenderResources is where it is waited for
	_engine->start();

	self.maximumFramesToRender = MAX_FRAMES;
	return self;
}

- (void)dealloc
{
	_engine.reset();
	_scratch.reset();
}

- (void *)enginePointer { return _engine.get(); }

- (AUAudioUnitBusArray *)inputBusses  { return _inputBusArray; }
- (AUAudioUnitBusArray *)outputBusses { return _outputBusArray; }

// The machine's MIDI OUT. There is one
- (NSArray<NSString *> *)MIDIOutputNames { return @[@"MIDI Out"]; }

// The real machine has no MPE
- (BOOL)supportsMPE { return NO; }

// **The real machine has four MIDI IN ports.** A and B are the DIN jacks; C and
// D exist only over USB, which is how the firmware sees them too. Saying
// anything less here makes a host believe there are fewer and never use the
// higher cables, so parts 17-64 become unreachable. Hosts that play files
// sometimes open *one instrument per port* instead, which boots one MU2000 per
// port (Cog's AUPlayer reads this to decide how many to open)
- (NSInteger)virtualMIDICableCount { return PORTS; }

// The rate conversion is what the sound is late by
- (NSTimeInterval)latency
{
	const double rate = _outputBus.format.sampleRate;
	if (!_engine || rate <= 0.0)
		return 0.0;
	return double(_engine->latency_samples()) / rate;
}

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError
{
	if (![super allocateRenderResourcesAndReturnError:outError])
		return NO;

	// Input and output at different rates would make the pulled block the wrong
	// length for the one being produced
	if (_inputBus.format.sampleRate != _outputBus.format.sampleRate) {
		if (outError)
			*outError = [NSError errorWithDomain:NSOSStatusErrorDomain
			                                code:kAudioUnitErr_FormatNotSupported
			                            userInfo:nil];
		return NO;
	}

	_engine->set_output_rate(_outputBus.format.sampleRate);
	_scratch->allocate(self.maximumFramesToRender);

	// **Wait here for the boot to finish, before any sound is asked for.**
	//
	// This is not the realtime thread, so waiting is allowed. Starting without
	// it means silence until the machine comes up, and the MIDI that arrives
	// meanwhile only piles up. In a host that runs faster than realtime (the
	// file-playing kind) that silence becomes the first ten-odd seconds of the
	// song, notes and all. With the boot snapshot this returns in milliseconds
	// (doc/auv3.md).
	//
	// A machine that did not come up (no ROMs, say) still gets its buffers: it
	// simply plays silence, and the reason is in the log
	(void)_engine->wait_ready(120000);

	_engine->set_processing(true);
	return YES;
}

- (void)deallocateRenderResources
{
	_engine->set_processing(false);
	[super deallocateRenderResources];
}

- (AUInternalRenderBlock)internalRenderBlock
{
	// Captured as plain pointers so the render block touches no Objective-C
	smu2000::plug::engine *eng = _engine.get();
	scratch               *sc  = _scratch.get();
	std::atomic<uint16_t> *sounded = _sounded;
	// Captured weakly so ARC does not make a cycle: this block belongs to self
	__unsafe_unretained SMU2000AudioUnit *unowned_self = self;

	return ^AUAudioUnitStatus(AudioUnitRenderActionFlags *actionFlags,
	                          const AudioTimeStamp       *timestamp,
	                          AUAudioFrameCount           frameCount,
	                          NSInteger                   outputBusNumber,
	                          AudioBufferList            *outputData,
	                          const AURenderEvent        *realtimeEventListHead,
	                          AURenderPullInputBlock      pullInputBlock)
	{
		(void)actionFlags; (void)outputBusNumber;
		if (frameCount > MAX_FRAMES)
			return kAudioUnitErr_TooManyFramesToProcess;

		// ---- Output. A host does not always hand over mData (then it is ours)
		float *out_l = nullptr, *out_r = nullptr;
		if (outputData->mNumberBuffers >= 1) {
			if (!outputData->mBuffers[0].mData) {
				outputData->mBuffers[0].mData = sc->out_l.data();
				outputData->mBuffers[0].mDataByteSize = frameCount * sizeof(float);
			}
			out_l = static_cast<float *>(outputData->mBuffers[0].mData);
		}
		if (outputData->mNumberBuffers >= 2) {
			if (!outputData->mBuffers[1].mData) {
				outputData->mBuffers[1].mData = sc->out_r.data();
				outputData->mBuffers[1].mDataByteSize = frameCount * sizeof(float);
			}
			out_r = static_cast<float *>(outputData->mBuffers[1].mData);
		} else {
			out_r = sc->out_r.data();     // asked for mono: the right is dropped
		}
		if (!out_l)
			return kAudioUnitErr_InvalidParameter;

		// ---- A/D INPUT. Silent if nothing is plugged in
		const float *in_l = nullptr, *in_r = nullptr;
		if (pullInputBlock) {
			AudioBufferList *abl = sc->in_abl();
			abl->mNumberBuffers = 2;
			abl->mBuffers[0].mNumberChannels = 1;
			abl->mBuffers[0].mDataByteSize   = frameCount * sizeof(float);
			abl->mBuffers[0].mData           = sc->in_l.data();
			abl->mBuffers[1].mNumberChannels = 1;
			abl->mBuffers[1].mDataByteSize   = frameCount * sizeof(float);
			abl->mBuffers[1].mData           = sc->in_r.data();

			AudioUnitRenderActionFlags f = 0;
			if (pullInputBlock(&f, timestamp, frameCount, 0, abl) == noErr) {
				in_l = static_cast<const float *>(abl->mBuffers[0].mData);
				in_r = abl->mNumberBuffers >= 2
				           ? static_cast<const float *>(abl->mBuffers[1].mData)
				           : in_l;
			}
		}

		// ---- Made in slices with the MIDI in between, so an event lands on the
		//      sample it was stamped for
		const AURenderEvent *e = realtimeEventListHead;
		AUAudioFrameCount done = 0;
		while (done < frameCount) {
			// Everything stamped at or before `done` goes in now
			while (e) {
				AUEventSampleTime off = e->head.eventSampleTime - timestamp->mSampleTime;
				if (off < 0)
					off = 0;                       // arrived late: right now
				if (AUAudioFrameCount(off) > done)
					break;
				if (e->head.eventType == AURenderEventMIDI ||
				    e->head.eventType == AURenderEventMIDISysEx) {
					const AUMIDIEvent *m = &e->MIDI;
					// cable 0 is MIDI IN A, 1 is B; anything else folds onto A
					const int port = (m->cable < PORTS) ? int(m->cable) : 0;
					if (m->length) {
						// A note-on with a velocity is the only thing that can
						// start a voice, so it is the only thing reset has to
						// silence later
						if (m->length >= 3 && (m->data[0] & 0xf0) == 0x90 && m->data[2])
							sounded[port].fetch_or(uint16_t(1u << (m->data[0] & 0x0f)),
							                       std::memory_order_relaxed);
						eng->midi(m->data, m->length, port);
					}
				}
				e = e->head.next;
			}

			// Then in one go up to the next event
			AUAudioFrameCount upto = frameCount;
			if (e) {
				AUEventSampleTime off = e->head.eventSampleTime - timestamp->mSampleTime;
				if (off < 0)
					off = 0;
				if (AUAudioFrameCount(off) < upto)
					upto = AUAudioFrameCount(off);
			}
			if (upto <= done)
				upto = done + 1;                   // never stand still
			if (upto > frameCount)
				upto = frameCount;

			const int n = int(upto - done);
			eng->fill(out_l + done, out_r + done, n,
			          in_l ? in_l + done : nullptr,
			          in_r ? in_r + done : nullptr);
			done = upto;
		}

		// ---- MIDI OUT. What the firmware sent, cut back into messages
		AUMIDIOutputEventBlock outBlock = unowned_self.MIDIOutputEventBlock;
		if (outBlock) {
			const size_t got = eng->midi_out(sc->tx.data(), sc->tx.size());
			if (got) {
				emit_ctx ctx{ outBlock, AUEventSampleTime(timestamp->mSampleTime) };
				sc->split.feed(sc->tx.data(), got, emit_one, &ctx);
			}
		}
		return noErr;
	};
}

// ---- What the DAW's project remembers: the machine's own state

static NSString *const kStateKey = @"S-MU2000.nvram";

- (NSDictionary<NSString *, id> *)fullState
{
	NSMutableDictionary *d = [[super fullState] mutableCopy] ?: [NSMutableDictionary dictionary];
	if (_engine) {
		std::vector<uint8_t> blob = _engine->save_state();
		if (!blob.empty())
			d[kStateKey] = [NSData dataWithBytes:blob.data() length:blob.size()];
	}
	return d;
}

- (void)setFullState:(NSDictionary<NSString *, id> *)state
{
	[super setFullState:state];
	NSData *d = state[kStateKey];
	if (_engine && [d isKindOfClass:[NSData class]] && d.length)
		_engine->load_state(static_cast<const uint8_t *>(d.bytes), d.length);
}

// Stop anything still sounding (the host asked)
- (void)reset
{
	if (_engine) {
		// Only the channels that sounded, so the burst is as short as it can
		// be; the line carries it at 31250bps and everything after it waits
		uint16_t mask[PORTS];
		bool any = false;
		for (int p = 0; p < PORTS; p++) {
			mask[p] = _sounded[p].exchange(0, std::memory_order_relaxed);
			any = any || mask[p];
		}
		if (any)
			_engine->all_notes_off(mask, PORTS);
		_engine->flush_resampler();
	}
	if (_scratch)
		_scratch->split.reset();
}

@end
