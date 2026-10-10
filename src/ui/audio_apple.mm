// license:BSD-3-Clause
//
// The shared half of Apple audio: one AVAudioEngine, one source node, one
// render block, for macOS and iOS alike. See audio_apple.h for why the two
// platforms share this and what stays apart.
//
// The work is the same on both: call fill, convert to what the unit wants,
// count, and hand the device's workgroup to the parallel slave thread. What
// differs between the platforms is the questions in namespace apple, asked from
// below.
//
// AVFAudio directly rather than through AVFoundation's re-export: this file
// needs the engine and the source node. Verified against the SDK headers rather
// than remembered - the render block puts frameCount before the buffer list,
// initWithRealtimeSafeRenderBlock needs iOS 27 (we floor at 17), and
// connect:to:fromBus:toBus:format:error: likewise, so this uses the error-less
// connect the 17.0 target allows.
#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#include "ui/audio_apple.h"
#include "ui/audio_in.h"
#include "ui/cpu_meter.h"
#include "ui/resampler.h"
#include "ui/wav.h"

#include "compat/cli_text.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ui {

// How long the block may produce nothing before the engine is treated as stopped.
// Measured at 16 to 20 ms per callback on a 512-frame buffer, so two or three
// polls is silence and not a slow device.
static constexpr double STALL_SECONDS = 0.5;

// Mach ticks per second, for the cpu_percent/worst_ms the status line reads.
// Resolved once: the timebase does not change under a process.
static double mach_tps()
{
	static double tps = 0.0;
	if (tps == 0.0) {
		mach_timebase_info_data_t tb{};
		mach_timebase_info(&tb);
		tps = 1e9 * double(tb.denom) / double(tb.numer);
	}
	return tps;
}

static void zero_buffers(AudioBufferList *abl)
{
	if (!abl)
		return;
	for (UInt32 i = 0; i < abl->mNumberBuffers; i++)
		if (abl->mBuffers[i].mData)
			std::memset(abl->mBuffers[i].mData, 0, abl->mBuffers[i].mDataByteSize);
}

struct apple_audio_out::impl {
	fill_fn fill = nullptr;

	apple::device_ref dev;      // what the request resolved to
	apple::device_claim claim;  // and whether taking it is what got it

	AVAudioEngine *engine = nil;
	AVAudioSourceNode *src = nil;

	std::vector<s16> scratch;   // fill target at 44100 Hz; grown, never shrunk
	std::vector<s16> *cap = nullptr;   // set_capture()'s buffer, or null
	// What a capture holds: the connection's rate and channel count, which a
	// custom output format can move and the WAV header then has to say.
	u32 cap_rate = AUDIO_RATE;
	u16 cap_channels = 2;

	std::string dev_name;
	std::string want_name;        // what the request asked for; "" means system default
	bool want_exact = false;      // and whether a menu name had to match wholly
	bool pin_default = false;     // exclusive: hold this device, do not follow the default

	std::atomic<bool> running{false};
	std::atomic<bool> taken{false};   // macOS: exclusive asked for, and the device is ours
	std::atomic<u32> buffer_frames{0};    // what the last block actually was
	std::atomic<u32> dev_rate{0};         // what the pinned device runs at, 0 before start
	u32 granted_frames = 0;               // what the platform granted, pre-roll
	// The connection's layout, copied off the device's own format at start() so
	// the block below can write the buffer it is handed without asking anything.
	// Plain values, not the AVAudioFormat: this is the audio thread.
	bool conn_planar = true;      // one buffer per channel, not interleaved
	bool conn_float = true;       // float32 rather than int16
	bool conn_known = false;      // false when the device wanted something else
	std::atomic<u64> produced{0}, starved{0};
	std::atomic<u64> busy_ticks{0}, worst_ticks{0};
	cpu_meter meter;   // recent load for the display (issue #80, shared helper)

	// When the worst spike happened, and how many there were. A single 40 ms
	// callback is four times a 512-frame buffer, so the question is not whether
	// it cracks but what stalls it: cold pages on first touch of the ROM data,
	// the card_lock shared with the display-link pump, or the compiler. The
	// index and the clock are two stores on a path that already does six.
	std::atomic<u64> worst_index{0};
	std::atomic<double> worst_at{0.0};
	std::atomic<u64> spikes{0};          // callbacks over 20 ms
	std::atomic<u64> started_ticks{0};   // mach ticks, like the stamps below
	std::atomic<u64> callbacks{0};

	// The watchdog's state: what it last saw produced, and when. See watch_loop().
	std::atomic<u64> last_produced{0};
	std::atomic<u64> last_seen_ticks{0};
	std::atomic<u64> recoveries{0};
	std::atomic<bool> watch_run{false};
	std::atomic<bool> recovering{false};  // a recovery is failing; said once
	// Raised by the engine's configuration-change notification, which prompts
	// watch_loop to look rather than asking for a rebuild: the HAL posts one
	// whenever it rebuilds a device's IO proc, including on the open that just
	// succeeded. A flag, because Apple's header says not to tear the engine down
	// inside the notification.
	std::atomic<bool> reconfigure{false};
	// recover()'s back-off, in mach ticks, and the streak that sets it.
	std::atomic<u64> retry_after_ticks{0};
	std::atomic<u32> fail_streak{0};
	// The wrapper's, called after a successful recovery with the device we ended
	// up on: its capabilities belong to that device, not the one we left.
	std::function<void(const apple::device_ref &)> on_device_change;
	// Held by token: a block-based observer is not unregistered by dropping the
	// object. Removed in stop() before the engine goes.
	id reconfigure_observer = nil;
	std::atomic<bool> warned{false};       // the block guard below, said once
	// A custom output format: what the settings window asked for, and the
	// renderer that produces it. Automatic - the default - leaves the rate and
	// the channel pair at the machine's own and lets the graph convert.
	audio_stream_renderer renderer;
	bool custom = false;
	u32 out_channels = 2, out_left = 0, out_right = 1;
	double out_rate = double(AUDIO_RATE);
	std::thread watchdog;            // joined in stop()
};

apple_audio_out::apple_audio_out()
	: m(new impl)
{
}

apple_audio_out::~apple_audio_out()
{
	stop();
}

bool apple_audio_out::start(const request &r, fill_fn fill, std::string &err)
{
	if (m->running.load(std::memory_order_acquire))
		stop();
	m->fill = std::move(fill);

	// The session comes first: the category and the rate decide what the engine
	// is handed, and on iOS an unconfigured session refuses to start one at all.
	if (!apple::session_open(r.latency_ms, err))
		return false;

	AVAudioEngine *engine = [[AVAudioEngine alloc] init];
	AVAudioOutputNode *out_node = [engine outputNode];

	// The connection is at AUDIO_RATE, not at the device's rate: the engine inserts
	// a sample-rate converter of its own when a connection's format differs from
	// the hardware's, so fill(n) needs no translation. Nothing here reads the
	// device's rate to compute with; the one read is after the pin, and it
	// labels the device.

	// Which device the request means, then its buffer size. Both before the
	// engine runs: macOS writes the device's buffer frame size here, which is
	// what --latency has always meant on that side, and the unit negotiates
	// against it once it opens.
	m->want_name = r.device;   // the ask, not the answer: see recover()
	m->want_exact = r.exact;
	m->dev = apple::resolve_output(r.device, r.exact);
	// An unnamed request follows the system default, except when exclusive was
	// asked for: a hogged device cannot be mixed, so a unit following the default
	// would move off the device we hold. recover() applies the same rule.
	m->pin_default = r.exclusive;
	if (r.exclusive)
		m->dev.follow = false;
	// A custom rate or channel pair is converted here in the render block; the
	// automatic case - all of it at the defaults - is the graph's. iOS has one
	// route and one rate, so it refuses, which is what its answer says.
	m->custom = ui::custom_audio_format(r.stream);
	if (!apple::custom_output_format(m->dev, r.stream, err, r.exclusive))
		return false;
	// Unconfigured, the renderer is in direct mode and 48000 would be handed
	// 44100's worth of machine frames: 8.8% fast, with start() returning true.
	if (m->custom)
		m->renderer.configure(int((m->custom && r.stream.sample_rate)
		                              ? r.stream.sample_rate : AUDIO_RATE),
		                      r.stream.quality);
	m->out_channels = m->custom ? apple::output_channels(m->dev) : 2;
	m->out_left = m->custom ? u32(r.stream.left) : 0;
	m->out_right = m->custom ? u32(r.stream.right) : 1;
	// A custom request with no rate of its own keeps the device's, so nothing is
	// converted that does not have to be.
	m->out_rate = (m->custom && r.stream.sample_rate) ? double(r.stream.sample_rate)
	                                                 : double(AUDIO_RATE);
	m->cap_rate = u32(m->out_rate);
	m->cap_channels = u16(m->out_channels);
	if (!m->dev.found) {
		err = r.device.empty() ? CLI_T("No audio output found", "音声の出口が見つからない")
		                       : CLI_T("No audio output with that name: ", "その名前の音声の出口が見つからない: ")
		                             + r.device;
		return false;
	}
	m->granted_frames = apple::request_buffer_frames(m->dev, r.latency_ms,
	                                                u32(r.stream.buffer_frames));
	// The block's buffer, sized here because growing it in the block allocates on
	// the thread that has none. Four times the negotiated buffer: a device whose
	// IO proc the HAL rebuilds (the first open, or the restart after taking it
	// exclusively) comes back with a buffer of its own choosing.
	const size_t want_scratch = size_t(std::max(m->granted_frames, 512u) * 4) * m->out_channels;
	if (m->scratch.size() < want_scratch)
		m->scratch.resize(want_scratch);

	apple_audio_out::impl *im = m.get();
	AVAudioSourceNode *src = [[AVAudioSourceNode alloc]
		initWithRenderBlock:^OSStatus(BOOL *isSilence, const AudioTimeStamp *ts,
		                              AVAudioFrameCount n, AudioBufferList *abl) {
			(void)ts;
			im->buffer_frames.store(n, std::memory_order_relaxed);
			if (!abl || abl->mNumberBuffers < 1) {
				if (isSilence)
					*isSilence = YES;
				return noErr;
			}
			if (!im->running.load(std::memory_order_acquire) || !im->fill) {
				zero_buffers(abl);
				if (isSilence)
					*isSilence = YES;
				return noErr;
			}
			// The rate this block is being called at, and what n of it is worth in
			// the machine's own frames. Both AUDIO_RATE unless a custom output
			// format moved the connection, which is the only thing that can.
			const double block_rate = im->custom ? im->out_rate : double(AUDIO_RATE);
			// What the machine was actually asked for: a custom rate goes through
			// the renderer, which asks for as much as each chunk needs rather than n
			// of it, so this is counted. Worked out from n it agreed with itself
			// even while the renderer did nothing.
			u64 machine_frames = u64(n);
			const u64 t0 = mach_absolute_time();
			// n frames at the connection's rate: one fill() covers the callback
			// exactly, no drift and no stash. At AUDIO_RATE - the automatic case -
			// that is the machine's own frame count; a custom format asks for
			// another rate and converts above, which is what machine_frames above
			// is for.
			const u32 chans = im->out_channels;
			if (im->custom) {
				// A custom format asked for, so the machine's 44100 is converted
				// here to the rate and channel pair that was requested, and the
				// engine converts from there to the hardware. The renderer asks
				// fill() for as much as each chunk needs, so nothing is kept back.
				// Sized in start(); only a device that hands out a buffer four
				// times the one it reported gets here.
				if (im->scratch.size() < size_t(n) * chans)
					im->scratch.resize(size_t(n) * chans);
				machine_frames = 0;
				im->renderer.render(im->scratch.data(), n, chans, im->out_left,
				                    im->out_right,
				                    [im, &machine_frames](s16 *dst, unsigned need) {
					                    im->fill(dst, need);
					                    machine_frames += need;
				                    },
				                    audio_stream_renderer::pcm16);
			} else {
				if (im->scratch.size() < size_t(n) * 2)
					im->scratch.resize(size_t(n) * 2);
				im->fill(im->scratch.data(), u32(n));
			}
			// Into the layout the device asked for, which is the one the block is
			// called with: the connection is the hardware's own format at
			// AUDIO_RATE, so the engine's converter has a rate to do and nothing
			// else to do. Only the rate is ours; the layout is read off the
			// pinned device in start() and copied into conn_planar/conn_float.
			const s16 *sv = im->scratch.data();
			bool ok = false;
			const u32 need_samples = n * chans;
			if (im->conn_planar && abl->mNumberBuffers >= chans) {
				// One plane per channel, each n samples, interleaved in scratch.
				bool room = true;
				for (u32 c = 0; c < chans && room; c++)
					room = abl->mBuffers[c].mDataByteSize >= n * sizeof(float);
				if (room && im->conn_float) {
					for (u32 c = 0; c < chans; c++) {
						float *p = static_cast<float *>(abl->mBuffers[c].mData);
						for (u32 i = 0; i < n; i++)
							p[i] = float(sv[size_t(i) * chans + c]) * (1.0f / 32768.0f);
					}
					ok = true;
				} else if (room) {
					for (u32 c = 0; c < chans; c++) {
						s16 *p = static_cast<s16 *>(abl->mBuffers[c].mData);
						for (u32 i = 0; i < n; i++)
							p[i] = sv[size_t(i) * chans + c];
					}
					ok = true;
				}
			} else if (!im->conn_planar && abl->mNumberBuffers >= 1 &&
			           abl->mBuffers[0].mDataByteSize >= need_samples * sizeof(float) &&
			           im->conn_float) {
				float *f = static_cast<float *>(abl->mBuffers[0].mData);
				for (u32 i = 0; i < need_samples; i++)
					f[i] = float(sv[i]) * (1.0f / 32768.0f);
				ok = true;
			} else if (!im->conn_planar && abl->mNumberBuffers >= 1 &&
			           abl->mBuffers[0].mDataByteSize >= need_samples * sizeof(s16) &&
			           !im->conn_float) {
				std::memcpy(abl->mBuffers[0].mData, sv,
				            size_t(need_samples) * sizeof(s16));
				ok = true;
			}
			if (ok) {
				// nothing to undo
			} else {
				// Not reached unless the graph hands us something other than the
				// format we connected with - which is the device's own, and, per
				// AVAudioEngine's own note on a configuration change, what the
				// nodes keep across one. Said once, because a silent zero buffer
				// here is a freeze that looks like a mute, and that is how the
				// last two defects in this file were found.
				if (!im->warned.exchange(true, std::memory_order_relaxed))
					std::fprintf(stderr,
					             "[audio] block does not match the connected format"
					             " (%u buffers, planar=%d, float=%d); silencing it\n",
					             unsigned(abl->mNumberBuffers), int(im->conn_planar),
					             int(im->conn_float));
				zero_buffers(abl);
			}
			const u64 t1 = mach_absolute_time();
			const u64 busy = t1 - t0;
			im->busy_ticks.fetch_add(busy, std::memory_order_relaxed);
			im->meter.add(double(busy) / mach_tps(), double(n) / block_rate);
			const u64 index = im->callbacks.fetch_add(1, std::memory_order_relaxed) + 1;
			if (const double ms = 1000.0 * double(busy) / mach_tps(); ms > 20.0)
				im->spikes.fetch_add(1, std::memory_order_relaxed);
			u64 worst = im->worst_ticks.load(std::memory_order_relaxed);
			while (busy > worst &&
			       !im->worst_ticks.compare_exchange_weak(worst, busy,
			                                             std::memory_order_relaxed)) {
			}
			// Which callback was the worst, and when: printed on stop, so a
			// one-off (cold pages, first touch of the ROM data) can be told
			// apart from a stall that recurs.
			if (busy >= worst) {
				im->worst_index.store(index, std::memory_order_relaxed);
				const double stamp = double(mach_absolute_time());
				im->worst_at.store(
				    (stamp - double(im->started_ticks.load(std::memory_order_relaxed))) /
				        mach_tps() * 1000.0,
				    std::memory_order_relaxed);
			}
			// The overrun proxy CoreAudio has no better name for: we took longer
			// to make the block than the block is worth, measured against the rate
			// the block is called at - which is the machine's own unless a custom
			// format asked for another, and the device's rate is not ours to know
			// on this path either way.
			if (double(busy) / mach_tps() > double(n) / block_rate)
				im->starved.fetch_add(1, std::memory_order_relaxed);
			// What went into the connection: the machine's own signal, or what a
			// custom format converted it to. n of every one of chans, at the rate
			// and width the header in write_capture names. The only allocation on
			// this path, and only while a capture was asked for.
			if (im->cap)
				im->cap->insert(im->cap->end(), im->scratch.data(),
				                im->scratch.data() + size_t(n) * chans);
			// The machine's frame count, which is the unit every consumer divides
			// by AUDIO_RATE to get seconds. Normally n itself, because the
			// connection is at AUDIO_RATE; with a custom format the block is called
			// at that rate instead, so n is converted back. This used to be the
			// device's frame count, which on a 48 kHz output read 8.9% long.
			im->produced.fetch_add(machine_frames, std::memory_order_relaxed);
			if (isSilence)
				*isSilence = NO;
			return noErr;
		}];
	// A configuration change is the engine saying its graph was rebuilt: a device
	// that changed, a format that moved. Nothing is done here - the handler
	// raises a flag for watch_loop, because this notification is delivered on an
	// internal queue and Apple's header warns against tearing the engine down
	// inside it.
	m->reconfigure_observer = [[NSNotificationCenter defaultCenter]
	    addObserverForName:AVAudioEngineConfigurationChangeNotification
	                object:engine
	                 queue:nil
	            usingBlock:^(NSNotification *note) {
		            (void)note;
		            im->reconfigure.store(true, std::memory_order_release);
	            }];
	[engine attachNode:src];
	NSError *e = nil;
	// The device is pinned before the connection, because the format is
	// negotiated against whichever device the unit holds - and because the
	// format below is read off that device.
	if (!apple::pin_output([out_node audioUnit], m->dev, err)) {
		[engine detachNode:src];
		return false;
	}

	// The connection is the hardware's own format at AUDIO_RATE, so the rate is the
	// only difference and the engine has only the rate to convert. Asking for
	// anything else has it convert the layout too.
	//
	// The rate being produced, which is AUDIO_RATE unless a custom format asked
	// for another: this is where that rate enters the graph, and the engine
	// converts from here to the hardware. The channel count is the device's own
	// when a custom request is being honoured and two otherwise, because the
	// machine is stereo and a device with more gets the graph's own downmix. A
	// sample format that is neither float32 nor int16 gets float32 and the
	// graph's conversion after all.
	AVAudioFormat *hw_fmt = [out_node outputFormatForBus:0];
	const bool hw_int16 = hw_fmt.commonFormat == AVAudioPCMFormatInt16;
	const bool hw_planar = hw_fmt.channelCount < 1 ? true : !hw_fmt.isInterleaved;
	im->conn_float = !hw_int16;
	im->conn_planar = hw_planar;
	// Automatic is two channels - the machine is stereo and the graph downmixes a
	// wider device. A custom route can name any of the device's outputs, so then
	// the connection is as wide as the device, and the unused channels get silence.
	//
	// Wider than two needs a channel layout: AudioStreamBasicDescription carries
	// none on this SDK, and both initialisers that take a bare channel count answer
	// nil above two (1 and 2 give a format, 3 to 8 do not).
	AVAudioFormat *fmt = nil;
	AVAudioChannelLayout *layout = nil;
	if (m->out_channels > 2) {
		AudioChannelLayout raw = {};
		if (apple::output_channel_layout(m->dev, raw))
			layout = [[AVAudioChannelLayout alloc] initWithLayout:&raw];
		if (layout)
			fmt = [[AVAudioFormat alloc] initWithCommonFormat:
			    hw_int16 ? AVAudioPCMFormatInt16 : AVAudioPCMFormatFloat32
			                                         sampleRate:m->out_rate
			                                           interleaved:!hw_planar
			                                       channelLayout:layout];
	}
	if (!fmt)
		fmt = [[AVAudioFormat alloc] initWithCommonFormat:
		    hw_int16 ? AVAudioPCMFormatInt16 : AVAudioPCMFormatFloat32
			                                         sampleRate:m->out_rate
			                                           channels:2
			                                        interleaved:!hw_planar];
	if (!fmt) {
		err = "cannot describe the device's output format at " +
		      std::to_string(int(m->out_rate)) + " Hz";
		return false;
	}
	// From the format we connected with, not from the query above: when the two
	// disagreed the engine handed one buffer where two were expected and the block
	// silenced itself, which is invisible in produced().
	im->conn_float = fmt.commonFormat == AVAudioPCMFormatFloat32;
	im->conn_planar = !fmt.isInterleaved;
	[engine connect:src to:out_node fromBus:0 toBus:0 format:fmt];
	if (![engine startAndReturnError:&e]) {
		err = std::string("AVAudioEngine start: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}

	m->engine = engine;
	m->src = src;
	// Read here, not before the pin: from here on the unit holds the device, so
	// this is that device's rate rather than whatever was default when this
	// function was entered. It labels the device and says the graph is
	// converting; nothing computes with it.
	const double rate = [[out_node outputFormatForBus:0] sampleRate];
	m->dev_name = apple::output_label(m->dev, rate);   // the device's, not ours
	m->dev_rate.store(u32(rate > 0.0 ? rate + 0.5 : 0.0), std::memory_order_relaxed);
	if (rate > 0.0 && rate != double(AUDIO_RATE))
		std::fprintf(stderr, "[audio] device runs %.0f Hz, the engine converts\n", rate);
	m->running.store(true, std::memory_order_release);
	// The watchdog starts with the engine and stops with it (see watch_loop).
	// Its first sight has to be the count we are about to have, or the first tick
	// calls a fresh engine a stall. The flag is cleared too: an open posts a
	// configuration change of its own.
	m->reconfigure.store(false, std::memory_order_relaxed);
	m->fail_streak.store(0, std::memory_order_relaxed);
	m->retry_after_ticks.store(0, std::memory_order_relaxed);
	m->last_produced.store(0, std::memory_order_relaxed);
	m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
	m->watch_run.store(true, std::memory_order_release);
	m->watchdog = std::thread([this] { watch_loop(); });
	m->callbacks.store(0, std::memory_order_relaxed);
	m->spikes.store(0, std::memory_order_relaxed);
	m->started_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);

	// The device is claimed after IO has started, as it always was: claiming
	// first can leave a device that cannot be mixed unopenable, and a refused
	// claim still plays - it surfaces through exclusive(), not a failed start.
	// Taking it changes the device's mixability, so the HAL rebuilds its IO
	// under us and the engine is started again on the new one (macOS only).
	if (r.exclusive) {
		std::string hog_err;
		m->claim = apple::take_output(m->dev, hog_err);
		// held, not took: a device we already held is ours to use, and exclusive()
		// has to say so rather than report that we got nothing.
		m->taken.store(m->claim.held, std::memory_order_relaxed);
		// strict means the setting asked for exclusive and would rather fail than
		// play shared. main refused this; without the check Settings can be saved
		// as exclusive and produce shared playback.
		if (r.stream.strict && !m->claim.held) {
			err = CLI_T("The device refused exclusive access", "排他で開けない");
			apple::release_output(m->claim);
			m->claim = apple::device_claim();
			m->taken.store(false, std::memory_order_relaxed);
			stop();
			return false;
		}
		if (m->claim.took) {
			[engine stop];
			NSError *restart_err = nil;
			if (![engine startAndReturnError:&restart_err]) {
				// We took the device and cannot open it. Playing nothing while
				// holding it would be the worst of the three answers, so the
				// start fails and stop() gives the device back.
				err = std::string("Cannot play on the device taken for exclusive use: ") +
				      (restart_err ? [[restart_err localizedDescription] UTF8String] : "?");
				stop();
				return false;
			}
		}
	}
	std::fprintf(stderr, "[audio] %s\n", m->dev_name.c_str());
	return true;
}

void apple_audio_out::set_device_change_hook(std::function<void(const apple::device_ref &)> hook)
{
	m->on_device_change = std::move(hook);
}

void apple_audio_out::restart()
{
	// The session watcher (iOS's AVAudioSession, and the hook macOS's session file
	// answers) calls this: something changed that we should come back from. The
	// recovery is the same one the watchdog does, because it is the same
	// situation - the engine is not running and nobody has noticed.
	recover("the session says the device changed");
}

void apple_audio_out::watch_loop()
{
	while (m->watch_run.load(std::memory_order_acquire)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		if (!m->running.load(std::memory_order_acquire) || !m->engine)
			continue;
		// The configuration-change notification makes the stall decision below come
		// now instead of within a poll, and is not itself a reason to rebuild: the
		// HAL posts one for every IO proc it rebuilds, the successful open included.
		const u64 seen = uint64_t(mach_absolute_time());
		const bool reconfigured = m->reconfigure.exchange(false, std::memory_order_acq_rel);
		const u64 produced = m->produced.load(std::memory_order_acquire);
		if (produced != m->last_produced.load(std::memory_order_relaxed)) {
			m->last_produced.store(produced, std::memory_order_relaxed);
			m->last_seen_ticks.store(seen, std::memory_order_relaxed);
			continue;
		}
		// Two or three polls of silence is a stopped engine, and every poll past
		// that is a gap in the sound. A second used to mean a second of silence on
		// the first open of a device whose IO proc the HAL rebuilds.
		if (!reconfigured &&
		    (seen - m->last_seen_ticks.load(std::memory_order_relaxed)) / mach_tps() < STALL_SECONDS)
			continue;
		m->last_seen_ticks.store(seen, std::memory_order_relaxed);
		recover(reconfigured ? "the engine says its configuration changed"
		                     : "nothing produced for " +
		                           std::to_string(int(STALL_SECONDS * 1000)) + " ms");
	}
}

bool apple_audio_out::recover(const std::string &why)
{
	AVAudioEngine *engine = m->engine;
	if (!engine)
		return false;
	// The back-off lives here rather than in the callers, which is what the header
	// claimed and none of them did: a device that has gone elsewhere was otherwise
	// rebuilt and reported once a second until the program ended.
	const u64 now = uint64_t(mach_absolute_time());
	if (now < m->retry_after_ticks.load(std::memory_order_relaxed))
		return false;
	// Safe from this thread, which is why the watchdog has one: the engine's
	// configuration-change callback runs on an internal dispatch queue, and
	// Apple's header warns against tearing the engine down inside it.
	//
	// Re-resolve and re-pin first: the device that went away may be a different
	// one now. The connection stays at AUDIO_RATE, so a rate change needs no
	// rebuild.
	//
	// From the request, so an unnamed one follows the default - except that an
	// exclusive session names the device it holds: taking one moves the default
	// away from it, so resolving the request here would re-pin us to another
	// device. By name and wholly, so a device that has gone still comes back
	// not-found.
	apple::device_ref again = (m->pin_default && m->dev.found)
	                              ? apple::resolve_output(m->dev.name, true)
	                              : apple::resolve_output(m->want_name, m->want_exact);
	if (again.found) {
		m->dev = again;
		std::string err;
		apple::pin_output([engine outputNode].audioUnit, again, err);
	}
	[engine stop];
	NSError *e = nil;
	if ([engine startAndReturnError:&e]) {
		const u64 n = m->recoveries.fetch_add(1, std::memory_order_relaxed) + 1;
		// Read after the restart, off the node that is running now: a recovery can
		// leave us on another device at another rate, and both are what the
		// settings window shows.
		const double rate = [[engine outputNode] outputFormatForBus:0].sampleRate;
		m->dev_name = apple::output_label(m->dev, rate);
		m->dev_rate.store(u32(rate > 0.0 ? rate + 0.5 : 0.0), std::memory_order_relaxed);
		std::fprintf(stderr, "[audio] recovered (%s), %.0f Hz, %llu so far\n", why.c_str(), rate,
		             (unsigned long long)n);
		m->last_produced.store(m->produced.load(std::memory_order_acquire),
		                       std::memory_order_relaxed);
		m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
		m->recovering.store(false, std::memory_order_relaxed);
		m->fail_streak.store(0, std::memory_order_relaxed);
		m->retry_after_ticks.store(0, std::memory_order_relaxed);
		// What else the window shows belongs to the device too, and is the
		// wrapper's to hold.
		if (m->on_device_change)
			m->on_device_change(m->dev);
		return true;
	}
	// Said once per streak: a device that has gone to another application is not
	// coming back by being retried faster.
	if (!m->recovering.exchange(true, std::memory_order_relaxed))
		std::fprintf(stderr, "[audio] could not recover (%s): %s\n", why.c_str(),
		             e ? [[e localizedDescription] UTF8String] : "?");
	// A second, doubling to eight.
	const u32 streak = m->fail_streak.fetch_add(1, std::memory_order_relaxed) + 1;
	const double wait = std::min(8.0, 0.5 * double(1u << std::min<u32>(streak, 4)));
	m->retry_after_ticks.store(now + u64(wait * mach_tps()), std::memory_order_relaxed);
	return false;
}

void apple_audio_out::stop()
{
	if (!m->running.exchange(false))
		return;
	// The watchdog first, before the engine goes: it must not decide to recover
	// an engine that is on its way out, and its thread holds this impl.
	m->watch_run.store(false, std::memory_order_release);
	if (m->watchdog.joinable())
		m->watchdog.join();
	// Stop first so no new block enters, then release: a block already inside
	// still holds the raw impl pointer, the way a refCon does. The app keeps
	// this object in a static and never destroys it mid-render, so the window
	// is theoretical - but stop-before-release is what keeps it so.
	AVAudioEngine *engine = m->engine;
	// The observer first, so nothing raises the flag on an engine that is going.
	if (m->reconfigure_observer) {
		[[NSNotificationCenter defaultCenter] removeObserver:m->reconfigure_observer];
		m->reconfigure_observer = nil;
	}
	m->src = nil;
	m->engine = nil;
	if (engine)
		[engine stop];
	m->fill = nullptr;
	apple::unpin_output();
	apple::release_output(m->claim);
	m->claim = apple::device_claim();
	m->taken.store(false, std::memory_order_relaxed);
	// What the worst spike was, and when: a 40 ms callback is four times a
	// 512-frame buffer, so this line is where the crack gets explained (or not).
	const u64 calls = m->callbacks.load(std::memory_order_relaxed);
	if (calls) {
		const double worst_ms = 1000.0 * double(m->worst_ticks.load(std::memory_order_relaxed)) /
		                        mach_tps();
		std::fprintf(stderr,
		             "[audio] %llu callbacks, worst %.1f ms at #%llu (t=%.1fs), "
		             "%llu over 20 ms, %llu late\n",
		             (unsigned long long)calls, worst_ms,
		             (unsigned long long)m->worst_index.load(std::memory_order_relaxed),
		             m->worst_at.load(std::memory_order_relaxed) / 1000.0,
		             (unsigned long long)m->spikes.load(std::memory_order_relaxed),
		             (unsigned long long)m->starved.load(std::memory_order_relaxed));
	}
}

bool apple_audio_out::running() const
{
	return m->running.load(std::memory_order_relaxed);
}

u32 apple_audio_out::device_rate() const
{
	return m->dev_rate.load(std::memory_order_relaxed);
}

u32 apple_audio_out::stream_rate() const
{
	return u32(m->out_rate + 0.5);
}

const std::string &apple_audio_out::device_name() const
{
	return m->dev_name;
}

bool apple_audio_out::exclusive() const
{
	return m->taken.load(std::memory_order_relaxed);
}

void *apple_audio_out::realtime_workgroup()
{
	if (!m->engine || !m->engine.isRunning)
		return nullptr;
	// The device-owned audio workgroup, for the parallel slave thread to join
	// (Apple's parallel real-time threads pattern; the join itself is in
	// compat/realtime.h). AVAudioIONode hands out the unit the engine renders
	// through, and off that unit this is the same property audio_out_mac.cpp
	// read from a unit of its own - AUAudioUnit.osWorkgroup is bridged to it.
	// The group belongs to the device, not to the unit, so there is one
	// whichever way the output was opened: RemoteIO was never a requirement.
	//
	// +0: the C getter's contract, so unretained. The device owns the group,
	// which is what lets this be stored as a bare handle and outlive a
	// stop/start of the engine.
	__unsafe_unretained os_workgroup_t wg = nullptr;
	UInt32 size = sizeof(wg);
	if (AudioUnitGetProperty(m->engine.outputNode.audioUnit,
	                         kAudioOutputUnitProperty_OSWorkgroup,
	                         kAudioUnitScope_Global, 0, &wg, &size) == noErr)
		return (__bridge void *)wg;
	return nullptr;
}

void apple_audio_out::set_capture(const std::string &path)
{
	// Same convention as every backend: the flag says capture was asked for, so
	// the render block knows to append rather than to skip a null check.
	m_cap_path = path;
	if (path.empty()) {
		m_cap.clear();
		m->cap = nullptr;
		return;
	}
	m_cap.clear();
	m->cap = &m_cap;
}

u64 apple_audio_out::capture_frames() const
{
	return m_cap.size() / m->cap_channels;
}

bool apple_audio_out::write_capture(std::string &err)
{
	if (m_cap_path.empty()) {
		err = CLI_T("No output file was given", "書き出す先が決まっていない");
		return false;
	}
	// ui/wav.h's header, at the rate and width the capture holds - which a custom
	// output format can move off AUDIO_RATE and two.
	return write_wav(m_cap_path, m_cap, err, m->cap_rate, m->cap_channels);
}

u64 apple_audio_out::produced() const
{
	return m->produced.load(std::memory_order_relaxed);
}

u32 apple_audio_out::buffer_frames() const
{
	// What the last block actually was, once there has been one. Before the
	// first callback the granted size is the honest answer, and on iOS it used
	// to read as a 0-frame buffer because nothing had been rendered yet.
	const u32 seen = m->buffer_frames.load(std::memory_order_relaxed);
	return seen ? seen : m->granted_frames;
}

u64 apple_audio_out::starved() const
{
	return m->starved.load(std::memory_order_relaxed);
}

bool apple_audio_out::mmcss() const
{
	// The engine's render thread is real-time by construction - the counterpart
	// of registering with MMCSS on Windows, with nothing to register.
	return m->running.load(std::memory_order_relaxed);
}

double apple_audio_out::cpu_percent() const
{
	const u64 busy = m->busy_ticks.load(std::memory_order_relaxed);
	const u64 prod = m->produced.load(std::memory_order_relaxed);
	if (prod == 0)
		return 0.0;
	// Fraction of one device-rate second spent rendering, as a percent.
	// prod is a count of 44100 frames and busy is ticks to make one, so the
	// device's rate is not in this any more (see the render block).
	return 100.0 * double(busy) / (mach_tps() * (double(prod) / double(AUDIO_RATE)));
}

double apple_audio_out::worst_ms() const
{
	return 1000.0 * double(m->worst_ticks.load(std::memory_order_relaxed)) / mach_tps();
}

double apple_audio_out::cpu_recent() const
{
	return m->meter.value();
}

// A sample type as a small number, so the tap can publish what it was given in
// one store beside the rate.
static inline int sample_code(AVAudioCommonFormat f)
{
	switch (f) {
	case AVAudioPCMFormatFloat32: return 1;
	case AVAudioPCMFormatInt16:   return 2;
	case AVAudioPCMFormatInt32:   return 3;
	default:                      return 0;
	}
}

// What to call a sample type in the label the front ends print. AVAudioFormat's
// own names are longer than a status line wants.
static const char *sample_code_name(int code)
{
	switch (code) {
	case 1: return "float32";
	case 2: return "int16";
	case 3: return "int32";
	default: return "other";
	}
}

static const char *sample_name(AVAudioCommonFormat f)
{
	return sample_code_name(sample_code(f));
}

// ---- The input half ---------------------------------------------------------
//
// Recording is the same shape as playback with the arrow reversed: the engine
// hands us buffers, we convert them to 44100 Hz s16 stereo and push them into a
// ring, and pop() takes them out one pair at a time. The buffers arrive the same
// way on both platforms - tapped off the engine - so the format asked for is
// the engine's own input format rather than one this file decides.

struct apple_audio_in::impl {
	static constexpr u32 RING = 1 << 16, MASK = RING - 1;      // 約 1.5 秒
	static constexpr u32 TARGET_FRAMES = 2205;                  // 50ms
	static constexpr u32 DROP_FRAMES = 8820;                    // 200ms を超えたら捨てる

	AVAudioEngine *engine = nil;

	// The converter turns the device's format into float32 stereo at the
	// device's own rate, and the rate itself is left to ui::resampler: the tap
	// hands over whatever the node has - any sample type, any layout, any channel
	// count - and none of that needs a switch here, while the rate conversion is
	// the one our own resampler does without adding delay.
	//
	// It is not documented as real-time safe, so it is built here, once, and the
	// tap only calls it.
	using ConvBlock = AVAudioBuffer *(^)(AVAudioPacketCount, AVAudioConverterInputStatus *);
	AVAudioConverter *conv = nil;
	AVAudioPCMBuffer *conv_out = nil;   // ours to fill, reused every callback
	AVAudioBuffer *conv_pending = nil;  // what the tap was handed this time
	bool conv_have = false;             // ...and whether it is still to hand
	ConvBlock conv_block = nil;
	std::atomic<bool> warned{false};   // said once

	ui::resampler rs;
	std::vector<s16> staging;    // converted frames as s16 stereo (the resampler's input)
	std::vector<float> conv_buf;
	std::vector<s16> out16;

	// Builds conv from the device's format, and is a member because a file-scope
	// function cannot name impl.
	bool setup_converter(AVAudioFormat *from, std::string &err);

	// n frames of s16 stereo at the device's rate in, 44100 Hz s16 stereo into
	// the ring. Unchanged from before the converter: the resampler takes what it
	// can take and gives back what that produced, so nothing is kept back.
	void run_resampler(const s16 *in, u32 n)
	{
		if (rs.direct()) {
			push(in, n);
			return;
		}
		for (u32 at = 0; at < n;) {
			const u32 k = std::min<u32>(1024, n - at);
			rs.push(const_cast<s16 *>(in + size_t(at) * 2), int(k));
			at += k;
			const int ready = rs.output_available();
			if (ready <= 0)
				continue;
			conv_buf.resize(size_t(ready) * 2);
			out16.resize(size_t(ready) * 2);
			rs.pull(conv_buf.data(), ready);
			for (size_t j = 0; j < out16.size(); j++)
				out16[j] = s16(std::lround(std::clamp(conv_buf[j], -1.0f, 1.0f) * 32767.0f));
			push(out16.data(), u32(ready));
		}
	}

	std::vector<s16> m_ring = std::vector<s16>(size_t(RING) * 2);
	std::atomic<u32> m_w{0}, m_r{0};
	std::atomic<u64> m_empty{0}, m_dropped{0};
	std::atomic<bool> running{false};
	// Liveness. The tap block counts, and the watchdog below watches that count
	// rather than the ring: the ring only drains when the machine asks for input,
	// and a synth that never does would look dead.
	std::atomic<u64> taps{0};
	std::atomic<int> seen_rate{0};   // what the last tap buffer was worth
	std::atomic<int> seen_shape{0};  // its channels and sample type, packed

	// The tap's block, kept so it can be installed again against a new format.
	// A tap holds the format it was given: when the device's own rate moves, the
	// engine will not start with the old one still installed - measured,
	// kAudioUnitErr_FormatNotSupported - so the tap is what has to be replaced,
	// not just the converter beside it.
	using TapBlock = void (^)(AVAudioPCMBuffer *, AVAudioTime *);
	TapBlock tap_block = nil;
	AVAudioInputNode *tap_node = nil;

	// Installs the block on this node's input bus with the format given, after
	// taking off any tap already there.
	void install_tap(AVAudioInputNode *node, AVAudioFormat *fmt)
	{
		if (tap_node)
			[tap_node removeTapOnBus:0];
		[tap_node = node installTapOnBus:0 bufferSize:1024 format:fmt block:tap_block];
	}
	std::atomic<u64> last_rate_ticks{0};  // when we last acted on a rate change
	std::atomic<u64> last_taps{0};
	std::atomic<u64> last_seen_ticks{0};
	std::atomic<bool> watch_run{false};
	std::atomic<bool> recovering{false};
	std::thread watchdog;

	std::string dev_name, fmt_line;
	double dev_rate = double(AUDIO_RATE);
	u32 dev_channels = 0;
	AVAudioCommonFormat dev_format = AVAudioPCMFormatFloat32;

	// 44100Hz 16bit 2ch を輪に積む。溢れる分は捨てる (the tap outruns the
	// synth's pop when the machine is busy).
	void push(const s16 *frames, u32 n)
	{
		u32 wr = m_w.load(std::memory_order_relaxed);
		for (u32 i = 0; i < n; i++) {
			const u32 rd = m_r.load(std::memory_order_acquire);
			if (((wr + 1) & MASK) == rd)
				break;
			m_ring[wr * 2] = frames[i * 2];
			m_ring[wr * 2 + 1] = frames[i * 2 + 1];
			wr = (wr + 1) & MASK;
			m_w.store(wr, std::memory_order_release);
		}
	}
};

apple_audio_in::apple_audio_in()
	: m(new impl)
{
}

apple_audio_in::~apple_audio_in()
{
	stop();
}

// Builds the converter, and the block that feeds it, once. The output format is
// the machine's own - 44100, s16, interleaved, stereo - so what comes back is the
// ring's own layout and pushing it is a copy, and the hardware's rate, channel
// count, sample type and layout are the converter's problem rather than a switch
// in this file.
//
// Not primed: a conversion during setup ends the stream, and a converter that has
// been converted with answers every later call with EndOfStream and nothing.
bool apple_audio_in::impl::setup_converter(AVAudioFormat *from, std::string &err)
{
	// Same rate as the input: this is the format conversion only. The arrow to
	// 44100 is ui::resampler's job, because it adds no delay - it can ask for the
	// input ahead of the moment it needs it - where converting the rate here
	// would add about a millisecond of filter delay.
	AVAudioFormat *want = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
	                                                       sampleRate:from.sampleRate
	                                                         channels:2
	                                                      interleaved:NO];
	AVAudioConverter *made = [[AVAudioConverter alloc] initFromFormat:from toFormat:want];
	if (!made) {
		err = std::string("Cannot convert ") + [[from description] UTF8String] +
		      " to float32 stereo";
		return false;
	}
	AVAudioPCMBuffer *out = [[AVAudioPCMBuffer alloc] initWithPCMFormat:want frameCapacity:8192];
	if (!out) {
		err = "Cannot allocate the recording conversion buffer";
		return false;
	}
	// A local, then stored: a block literal cannot be assigned straight to a
	// member here, and it has to outlive this function either way.
	ConvBlock block = ^AVAudioBuffer *(AVAudioPacketCount wanted, AVAudioConverterInputStatus *status) {
		(void)wanted;
		// NoDataNow, never EndOfStream. This is the whole difference between a
		// converter that streams and one that stops after its first buffer: say
		// EndOfStream and it is finished for good, say NoDataNow and it keeps its
		// state and asks again when there is more.
		if (!conv_have) {
			*status = AVAudioConverterInputStatus_NoDataNow;
			return nil;
		}
		conv_have = false;
		*status = AVAudioConverterInputStatus_HaveData;
		return conv_pending;
	};
	conv = made;
	conv_out = out;
	conv_block = block;
	return true;
}

bool apple_audio_in::start(const std::string &device, std::string &err)
{
	stop();
	// Three questions before any engine exists, in the order they have always
	// been asked: which device did the name mean, may we record, does the
	// session take the request. A bad name is a cheaper thing to say than a
	// permission prompt.
	const apple::device_ref dev = apple::resolve_input(device, false);
	if (!dev.found) {
		err = device.empty() ? CLI_T("No recording device", "録音デバイスが無い")
		                     : CLI_T("No recording device with that name", "その名前の録音デバイスは無い");
		return false;
	}
	if (!apple::input_permission(err))
		return false;
	if (!apple::session_open_input(err))
		return false;

	AVAudioEngine *engine = [[AVAudioEngine alloc] init];
	AVAudioInputNode *node = [engine inputNode];
	// The pin comes before the format, for the same reason the output side's
	// does: what the unit is allowed to record is decided by which device it
	// holds.
	if (!apple::pin_input([node audioUnit], dev, err))
		return false;

	// The device's rate, unlike the output side: no converter goes ahead of the
	// input node, so a connection asking for a rate the hardware is not running
	// is accepted, starts, and delivers nothing. Measured, hardware at 96000,
	// 600 ms each: the input node at 44100 gave 0 buffers, at 96000 gave 6. The
	// channel count is not the constraint - 2ch against a 1ch device folded and
	// gave 6 - so ui::resampler below is here for the rate alone.
	//
	// The tap asks for the device's own format. A forced 2ch interleaved one is
	// accepted and starts, and on a mono device the block is then never called.
	// The block below folds the channels instead.
	AVAudioFormat *tap = [node inputFormatForBus:0];
	const double rate = tap.sampleRate;
	if (!(rate > 0.0) || tap.channelCount < 1) {
		err = CLI_T("Cannot read the recording device's format", "録音の形式が読めない");
		return false;
	}
	m->dev_rate = rate;
	m->dev_channels = tap.channelCount;
	m->dev_format = tap.commonFormat;
	m->rs.configure(rate, double(AUDIO_RATE));
	apple_audio_in::impl *cim = m.get();
	if (!cim->setup_converter(tap, err))
		return false;

	apple_audio_in::impl *im = m.get();
	im->tap_block = ^(AVAudioPCMBuffer *buf, AVAudioTime *when) {
		                (void)when;
		                im->taps.fetch_add(1, std::memory_order_relaxed);
		                if (!buf || buf.frameLength == 0)
			                return;
		                const UInt32 n = buf.frameLength;
		                // Convert the format, then the rate. The converter gives us
		                // float32 stereo at the device's own rate and ui::resampler
		                // takes it to 44100 without adding delay.
		                //
		                // It asks for more when the buffer it wants is bigger than the
		                // one it has, so we keep handing it ours until it says it has
		                // no data *now* - which leaves its state alone. EndOfStream
		                // instead would finish the converter for good: measured, one
		                // converter at init gave its first buffer and nothing after.
		                // What the tap is being handed is the device's own rate, and
		                // the resampler below is configured for the rate we were given
		                // at start(). If those ever differ the recording is running at
		                // the wrong speed, so the watchdog is told rather than left to
		                // notice: the tap cannot ask for anything, so this is the only
		                // place the answer exists.
		                im->seen_rate.store(int(buf.format.sampleRate + 0.5),
		                                   std::memory_order_relaxed);
		                // Channels and sample type too: a converter built for a mono
		                // float32 device is wrong for a stereo int16 one at the same
		                // rate, and nothing else would notice.
		                im->seen_shape.store((int(buf.format.channelCount) << 8) |
		                                        sample_code(buf.format.commonFormat),
		                                    std::memory_order_relaxed);
		                im->conv_pending = buf;
		                im->conv_have = true;
		                for (int guard = 0; guard < 8; guard++) {
			                im->conv_out.frameLength = 0;
			                NSError *cerr = nil;
			                const AVAudioConverterOutputStatus st =
			                    [im->conv convertToBuffer:im->conv_out error:&cerr
			                        withInputFromBlock:im->conv_block];
			                if (st == AVAudioConverterOutputStatus_Error) {
				                if (!im->warned.exchange(true, std::memory_order_relaxed))
				                        std::fprintf(stderr, "[audio] in: conversion failed: %s\n",
				                                     cerr ? [[cerr localizedDescription] UTF8String] : "?");
				                break;
			                }
			                const UInt32 got = im->conv_out.frameLength;
			                if (got > 0) {
				                const float *const *f = im->conv_out.floatChannelData;
				                if (f && f[0] && f[1]) {
					                im->staging.resize(size_t(got) * 2);
					                for (UInt32 i = 0; i < got; i++) {
						                im->staging[size_t(i) * 2] = s16(std::lround(
						                    std::clamp(f[0][i], -1.0f, 1.0f) * 32767.0f));
						                im->staging[size_t(i) * 2 + 1] = s16(std::lround(
						                    std::clamp(f[1][i], -1.0f, 1.0f) * 32767.0f));
					                }
					                im->run_resampler(im->staging.data(), got);
				                }
			                }
			                if (st != AVAudioConverterOutputStatus_InputRanDry || !im->conv_have)
				                break;
		                }
		                im->conv_pending = nil;
	                };
	im->install_tap(node, tap);
	NSError *e = nil;
	if (![engine startAndReturnError:&e]) {
		if (im->tap_node) {
			[im->tap_node removeTapOnBus:0];
			im->tap_node = nil;
		}
		err = std::string("AVAudioEngine input start: ") +
		      (e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}

	m->engine = engine;
	m->dev_name = dev.name;
	m->fmt_line = apple::input_label(dev, rate, 2, sample_name(tap.commonFormat));
	m->m_w.store(0);
	m->m_r.store(0);
	m->running.store(true, std::memory_order_release);
	// The watchdog starts with the engine and stops with it, and its first sight
	// has to be a count it can tell from zero, or the first tick would call a
	// fresh engine a stall.
	m->last_taps.store(m->taps.load(std::memory_order_acquire), std::memory_order_relaxed);
	m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
	m->last_rate_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
	m->recovering.store(false, std::memory_order_relaxed);
	m->watch_run.store(true, std::memory_order_release);
	m->watchdog = std::thread([this] { watch_loop(); });
	std::fprintf(stderr, "[audio] in: %s (%.0f Hz)\n", m->dev_name.c_str(), rate);
	return true;
}

void apple_audio_in::restart()
{
	AVAudioEngine *engine = m->engine;
	if (!engine || !m->running.load(std::memory_order_acquire))
		return;
	[engine stop];
	// Re-read the node's format before starting. Starting an engine that has stopped
	// itself with the format read before it fails - measured, error -10868 -
	// while asking the node for its format again re-opens the IO unit and the
	// start succeeds. This is the iOS path's first line of defence, where the
	// session watcher calls restart() after a route change.
	AVAudioInputNode *node = [engine inputNode];
	AVAudioFormat *now = [node inputFormatForBus:0];
	// The converter was built from the format the node had then, so anything
	// different - rate, channel count, sample type - means a new converter rather
	// than a reconfigured one.
	if (now.sampleRate > 0.0 &&
	    (now.sampleRate != m->dev_rate || now.channelCount != m->dev_channels ||
	     now.commonFormat != m->dev_format)) {
		m->dev_rate = now.sampleRate;
		m->dev_channels = now.channelCount;
		m->dev_format = now.commonFormat;
		m->rs.configure(now.sampleRate, double(AUDIO_RATE));
		std::string cerr;
		if (!m->setup_converter(now, cerr))
			std::fprintf(stderr, "[audio] in: %s\n", cerr.c_str());
		if (m->tap_block)
			m->install_tap(node, now);
	}
	NSError *e = nil;
	if ([engine startAndReturnError:&e]) {
		const bool rebuilt = m->seen_rate.load(std::memory_order_relaxed) !=
		                         int(m->dev_rate + 0.5) ||
		                     m->seen_shape.load(std::memory_order_relaxed) !=
		                         ((int(m->dev_channels) << 8) |
		                          sample_code(m->dev_format));
		m->recovering.store(false, std::memory_order_relaxed);
		m->seen_rate.store(int(m->dev_rate + 0.5), std::memory_order_relaxed);
		m->seen_shape.store((int(m->dev_channels) << 8) | sample_code(m->dev_format),
		                    std::memory_order_relaxed);
		if (rebuilt)
			std::fprintf(stderr, "[audio] in: rebuilt, %.0f Hz\n", m->dev_rate);
		m->last_taps.store(m->taps.load(std::memory_order_acquire), std::memory_order_relaxed);
		m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
		return;
	}
	// Said once per streak: a recording device that has gone to another
	// application is not going to be fixed by saying so every second.
	if (!m->recovering.exchange(true, std::memory_order_relaxed))
		std::fprintf(stderr, "[audio] in: restart failed: %s\n",
		             e ? [[e localizedDescription] UTF8String] : "?");
}

// The output half's watchdog, for the same reason and with the same excuse: the
// session watchers are iOS's, so on macOS nothing restarts a recording engine
// the clock has stopped. Its own thread because not every front end has a tick.
// The signal is the tap's own count and not the ring, which only drains when the
// machine asks for input.
void apple_audio_in::watch_loop()
{
	while (m->watch_run.load(std::memory_order_acquire)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		if (!m->running.load(std::memory_order_acquire) || !m->engine)
			continue;
		// A rate change first: the tap may well keep delivering, in which case
		// the liveness test below never fires, and the resampler would carry on
		// converting to the rate the device had when it was configured.
		// Its own clock, not last_seen_ticks: that one is refreshed every time a
		// tap arrives, which is every 250 ms, so sharing it would mean this never
		// got past the one-second gate.
		const int rate = m->seen_rate.load(std::memory_order_relaxed);
		const int shape = m->seen_shape.load(std::memory_order_relaxed);
		// Parenthesised: != binds tighter than |, and without these brackets the
		// test is a bool OR'd with an int and therefore always true, which is a
		// rebuild every second saying the rate did not change.
		const int want_shape = (int(m->dev_channels) << 8) |
		                       sample_code(m->dev_format);
		if (rate > 0 && (rate != int(m->dev_rate) || shape != want_shape)) {
			const u64 at = uint64_t(mach_absolute_time());
			if ((at - m->last_rate_ticks.load(std::memory_order_relaxed)) / mach_tps() >= 1.0) {
				m->last_rate_ticks.store(at, std::memory_order_relaxed);
				if (!m->recovering.exchange(true, std::memory_order_relaxed))
					std::fprintf(stderr,
					             "[audio] in: the device is now %.0f Hz/%u ch/%s,"
					             " was %.0f Hz/%u ch/%s; rebuilding\n",
					             double(rate), unsigned(shape >> 8),
					             sample_code_name(shape & 0xff),
					             m->dev_rate, m->dev_channels,
					             sample_name(m->dev_format));
				restart();
			}
			continue;
		}
		const u64 taps = m->taps.load(std::memory_order_acquire);
		if (taps != m->last_taps.load(std::memory_order_relaxed)) {
			m->last_taps.store(taps, std::memory_order_relaxed);
			m->last_seen_ticks.store(uint64_t(mach_absolute_time()), std::memory_order_relaxed);
			continue;
		}
		const u64 seen = uint64_t(mach_absolute_time());
		if ((seen - m->last_seen_ticks.load(std::memory_order_relaxed)) / mach_tps() < 1.0)
			continue;
		m->last_seen_ticks.store(seen, std::memory_order_relaxed);
		restart();
	}
}

void apple_audio_in::stop()
{
	if (!m->running.exchange(false))
		return;
	// The watchdog first, before the engine goes: it must not decide to restart
	// an engine that is on its way out, and its thread holds this impl.
	m->watch_run.store(false, std::memory_order_release);
	if (m->watchdog.joinable())
		m->watchdog.join();
	// Stop first so no new tap fires, then release the tap and the engine: a
	// block already inside still holds the raw impl pointer.
	AVAudioEngine *engine = m->engine;
	m->engine = nil;
	// Through tap_node, which is the one that installed it, and cleared here: the
	// engine goes below, and install_tap() takes the old tap off before putting a
	// new one on - a node belonging to a released engine is not something to send
	// a message to, and it throws rather than failing.
	if (m->tap_node) {
		[m->tap_node removeTapOnBus:0];
		m->tap_node = nil;
	}
	if (engine)
		[engine stop];
}

bool apple_audio_in::running() const
{
	return m->running.load(std::memory_order_acquire);
}

const std::string &apple_audio_in::device_name() const
{
	return m->dev_name;
}

std::string apple_audio_in::format_line() const
{
	return m->fmt_line;
}

u64 apple_audio_in::empty_count() const
{
	return m->m_empty.load(std::memory_order_relaxed);
}

u64 apple_audio_in::dropped_count() const
{
	return m->m_dropped.load(std::memory_order_relaxed);
}

void apple_audio_in::pop(s32 &l, s32 &r)
{
	apple_audio_in::impl &im = *m;
	u32 rd = im.m_r.load(std::memory_order_relaxed);
	const u32 wr = im.m_w.load(std::memory_order_acquire);
	u32 level = (wr - rd) & apple_audio_in::impl::MASK;
	if (level > apple_audio_in::impl::DROP_FRAMES) {
		// Too far behind to catch up frame by frame: jump to a sane distance and
		// say so, which is what the counter is for.
		rd = (wr - apple_audio_in::impl::TARGET_FRAMES) & apple_audio_in::impl::MASK;
		level = apple_audio_in::impl::TARGET_FRAMES;
		im.m_dropped.fetch_add(1, std::memory_order_relaxed);
	}
	if (!level) {
		l = r = 0;
		if (running())
			im.m_empty.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	l = im.m_ring[rd * 2];
	r = im.m_ring[rd * 2 + 1];
	im.m_r.store((rd + 1) & apple_audio_in::impl::MASK, std::memory_order_relaxed);
}


// ---- audio_out: the class, shared by both platforms ------------------------
//
// Every method is one line, because the work behind it is above: the render
// path, the engine, the nodes, the counters. Which platform owns a given
// question is answered in namespace apple, which is all the platform files
// contain.

struct audio_out::impl {
	std::unique_ptr<apple_audio_out> core = std::make_unique<apple_audio_out>();
};

audio_out::audio_out()
	: m_impl(std::make_unique<impl>())
{
}

audio_out::~audio_out()
{
	stop();
}

std::vector<std::string> audio_out::list()
{
	return apple::output_list();
}

std::string audio_out::default_device_name()
{
	return apple::default_output_name();
}

bool audio_out::running() const { return m_impl->core->running(); }

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err, bool exclusive,
                      const std::string &device, bool raw, bool exact)
{
	if (!valid_audio_request(m_stream)) {
		err = CLI_T("Invalid audio stream settings", "音声の設定が不正");
		return false;
	}
	// raw bypasses the system mixer, and there is nothing to bypass on either
	// platform: the system does the format conversion rather than a driver
	// mixer. It is in the signature only so both take the same call.
	(void)raw;
	apple_audio_out::request r;
	r.latency_ms = latency_ms;
	r.device = device;
	r.exact = exact;
	r.stream = m_stream;
	// exclusive asks for the device outright. macOS has hog mode and the core
	// takes it through this file's take_output(); iOS has one route that is
	// always mixed and its answer says so, so the request is simply ignored
	// there rather than failing the open - which is why this is one line here
	// and the difference lives in the answers.
	r.exclusive = exclusive;
	if (!m_impl->core->start(r, std::move(fill), err))
		return false;
	// Watch the session while we are running (see watch_output_session): the
	// engine stops itself when headphones appear or a call arrives.
	apple::watch_output_session([core = m_impl->core.get()] { core->restart(); });
	// What the settings window shows and offers: the rates and channels this
	// device can run, and the rate it is running at now, which is the one we
	// connected to.
	auto describe = [this](const apple::device_ref &dev) {
		audio_stream_info info = apple::output_capabilities(dev, double(m_impl->core->device_rate()));
		// The rate reported as running is the one we produce, which is the device's
		// own unless a custom format asked for another; buffer_rate stays the
		// device's clock, which is what the window needs to show alongside it.
		info.rate = int(m_impl->core->stream_rate());
		info.buffer_rate = int(m_impl->core->device_rate());
		return info;
	};
	m_info = describe(apple::resolve_output(device, exact));
	// Re-described after a recovery, which can leave us on another device, so the
	// window stops offering the one we were taken away from. That call is on the
	// watchdog's thread.
	m_impl->core->set_device_change_hook([this, describe](const apple::device_ref &dev) {
		const std::lock_guard<std::mutex> lock(m_info_lock);
		m_info = describe(dev);
	});
	return true;
}

void audio_out::stop()
{
	// In this order, because each step is what makes the next one safe: the
	// watcher first, then the watchdog joined inside stop(), and only then the hook
	// that thread calls into. Clearing it earlier is a std::function being written
	// while the watchdog reads it.
	apple::watch_output_session(nullptr);
	m_impl->core->stop();
	m_impl->core->set_device_change_hook(nullptr);
}

std::string audio_out::device_name() const
{
	return m_impl->core->device_name();
}

bool audio_out::exclusive() const
{
	return m_impl->core->exclusive();
}

void *audio_out::realtime_workgroup()
{
	return m_impl->core->realtime_workgroup();
}

void audio_out::set_capture(const std::string &path)
{
	// The path is outside impl on purpose (stop() throws impl away, and what was
	// captured has to outlive it), so it is set here as the Linux backend sets
	// it. The flag is Linux's own, and the core keeps its own pair - which is the
	// one the render block reads.
	m_cap_path = path;
	m_impl->core->set_capture(path);
}

u64 audio_out::capture_frames() const
{
	return m_impl->core->capture_frames();
}

bool audio_out::write_capture(std::string &err)
{
	return m_impl->core->write_capture(err);
}

u64 audio_out::produced() const
{
	return m_impl->core->produced();
}

u32 audio_out::buffer_frames() const
{
	return m_impl->core->buffer_frames();
}

u64 audio_out::starved() const
{
	return m_impl->core->starved();
}

bool audio_out::mmcss() const
{
	return m_impl->core->mmcss();
}

double audio_out::cpu_percent() const
{
	return m_impl->core->cpu_percent();
}

double audio_out::worst_ms() const
{
	return m_impl->core->worst_ms();
}

double audio_out::cpu_recent() const
{
	return m_impl->core->cpu_recent();
}


// ---- audio_in: the class, shared by both platforms ------------------------
//
// One line per method, for the same reason audio_out's are: the tap, the ring,
// the resampler and the counters are above.

struct audio_in::impl {
	std::unique_ptr<apple_audio_in> core = std::make_unique<apple_audio_in>();
};

audio_in::audio_in()
	: m_impl(std::make_unique<impl>())
{
}

audio_in::~audio_in()
{
	stop();
}

std::vector<std::string> audio_in::list()
{
	return apple::input_list();
}

bool audio_in::start(const std::string &device, std::string &err)
{
	if (!m_impl->core->start(device, err))
		return false;
	// A route change stops the input engine too (mic unplugged, category
	// flipped), so it is watched the same way output is.
	apple::watch_input_session([core = m_impl->core.get()] { core->restart(); });
	return true;
}

void audio_in::stop()
{
	apple::watch_input_session(nullptr);
	m_impl->core->stop();
}

bool audio_in::running() const
{
	return m_impl->core->running();
}

void audio_in::pop(s32 &l, s32 &r)
{
	m_impl->core->pop(l, r);
}

std::string audio_in::device_name() const
{
	return m_impl->core->device_name();
}

std::string audio_in::format_line() const
{
	return m_impl->core->format_line();
}

u64 audio_in::empty_count() const
{
	return m_impl->core->empty_count();
}

u64 audio_in::dropped_count() const
{
	return m_impl->core->dropped_count();
}

} // namespace ui
