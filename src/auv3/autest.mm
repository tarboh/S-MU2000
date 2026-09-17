// license:BSD-3-Clause
//
// A host for the AUv3 that does not go through the .appex.
//
// AUAudioUnit can be registered into this very process with registerSubclass:,
// so **the ports, the sound and the MIDI in both directions** can be checked
// without the system registering anything.
//
//   build/autest [out.wav]
//   build/autest --system                  the registered .appex, out of process
//   build/autest --smf song.mid out.wav    play a file through the plug-in
//   build/autest --state out.bin           write the booted state and stop
//
// What it looks at:
//   - how many ports there are and what they are called (MAIN OUT / A/D INPUT /
//     MIDI OUT)
//   - that it plays at the host's rate (48000), i.e. that the resampler works
//   - that MIDI IN A-D (cables 0-3) each reach their own part (1, 17, 33, 49)
//   - that sound put into A/D INPUT comes back out
//   - that MIDI OUT (the firmware's replies) can be received

#import "audio_unit.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>

#include "smf.h"
#include "vst3/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double HOST_RATE = 48000.0;      // deliberately not 44100
constexpr AUAudioFrameCount BLOCK = 512;

void write_wav(const char *path, const std::vector<float> &l, const std::vector<float> &r,
               double rate)
{
	std::FILE *f = std::fopen(path, "wb");
	if (!f)
		return;
	const uint32_t n = uint32_t(l.size());
	const uint32_t bytes = n * 4;          // 16bit 2ch
	auto w32 = [&](uint32_t v) { uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8),
	                                              uint8_t(v >> 16), uint8_t(v >> 24) };
	                             std::fwrite(b, 1, 4, f); };
	auto w16 = [&](uint16_t v) { uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
	                             std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(2);
	w32(uint32_t(rate)); w32(uint32_t(rate) * 4); w16(4); w16(16);
	std::fwrite("data", 1, 4, f); w32(bytes);
	for (uint32_t i = 0; i < n; i++) {
		auto clip = [](float v) {
			const int s = int(v * 32767.0f);
			return int16_t(s < -32768 ? -32768 : s > 32767 ? 32767 : s);
		};
		w16(uint16_t(clip(l[i]))); w16(uint16_t(clip(r[i])));
	}
	std::fclose(f);
	std::printf("書き出した: %s（%.1f 秒）\n", path, double(n) / rate);
}


// Built the same way Cog's AUPlayer::sendEventList builds it. SysEx goes as
// SysEx7 (type 0x3), six bytes at a time, with F0 and F7 left off -- the status
// nibble carries that instead: 0 complete, 1 start, 2 continue, 3 end
API_AVAILABLE(macos(12.0))
static void send_ump(AUMIDIEventListBlock schedule, AUEventSampleTime when,
                     uint8_t group, const uint8_t *data, size_t length)
{
	if (!length)
		return;
	alignas(4) uint8_t storage[sizeof(MIDIEventList) + 8192];
	MIDIEventList *list = (MIDIEventList *)storage;
	MIDIEventPacket *pk = MIDIEventListInit(list, kMIDIProtocol_1_0);
	auto add = [&](const uint32_t *w, UInt32 n) {
		MIDIEventPacket *nx = MIDIEventListAdd(list, sizeof(storage), pk, 0, n, w);
		if (!nx) {
			schedule(when, group, list);
			pk = MIDIEventListInit(list, kMIDIProtocol_1_0);
			nx = MIDIEventListAdd(list, sizeof(storage), pk, 0, n, w);
		}
		pk = nx;
	};
	const uint8_t st = data[0];
	if (st == 0xF0) {
		const uint8_t *body = data + 1;
		size_t n = length - 1;
		if (n && body[n - 1] == 0xF7) --n;
		size_t at = 0;
		do {
			const size_t take = (n - at) > 6 ? 6 : (n - at);
			const bool first = (at == 0), last = (at + take >= n);
			const uint8_t s7 = (first && last) ? 0 : first ? 1 : last ? 3 : 2;
			uint32_t w[2] = { (uint32_t)0x3 << 28 | (uint32_t)group << 24 |
			                  (uint32_t)s7 << 20 | (uint32_t)take << 16, 0 };
			for (size_t k = 0; k < take; k++) {
				const uint32_t v = body[at + k];
				if (k < 2) w[0] |= v << (8 * (1 - k));
				else       w[1] |= v << (8 * (5 - k));
			}
			add(w, 2);
			at += take;
		} while (at < n);
	} else {
		const uint32_t type = (st >= 0xF1) ? 0x1 : 0x2;
		uint32_t w = type << 28 | (uint32_t)group << 24 | (uint32_t)st << 16;
		if (length >= 2) w |= (uint32_t)data[1] << 8;
		if (length >= 3) w |= (uint32_t)data[2];
		add(&w, 1);
	}
	schedule(when, group, list);
}

} // namespace


int main(int argc, const char *argv[])
{
	@autoreleasepool {
		// --system takes the .appex **the system has registered** -- loaded
		// into a sandboxed process of its own -- instead of registering one
		// here. That is the road a DAW travels, and it is also how to find out
		// whether the ROMs really made it into the bundle
		bool use_system = false;
		const char *wav = nullptr;
		const char *state_out = nullptr;      // --state <file>
		const char *smf_path = nullptr;       // --smf <MIDI file>
		bool split = false;                   // --split: two roads, as Cog uses
		bool sysex_ump = false;               // --sysex-ump: SysEx via MIDIEventList too
		int preamble = 0;                     // --pre <bits>: 1 hush, 2 GS on A, 4 GS on B
		for (int i = 1; i < argc; i++) {
			if (!std::strcmp(argv[i], "--system")) use_system = true;
			else if (!std::strcmp(argv[i], "--state") && i + 1 < argc) state_out = argv[++i];
			else if (!std::strcmp(argv[i], "--smf") && i + 1 < argc) smf_path = argv[++i];
			else if (!std::strcmp(argv[i], "--split")) split = true;
			else if (!std::strcmp(argv[i], "--sysex-ump")) sysex_ump = true;
			else if (!std::strcmp(argv[i], "--preamble")) preamble = 7;
			else if (!std::strcmp(argv[i], "--pre") && i + 1 < argc) preamble = atoi(argv[++i]);
			else wav = argv[i];
		}

		AudioComponentDescription desc{};
		desc.componentType         = kAudioUnitType_MusicDevice;   // 'aumu'
		desc.componentSubType      = 'MU2k';
		desc.componentManufacturer = 'Trbh';

		__block AUAudioUnit *au = nil;
		__block NSError *err = nil;
		if (use_system) {
			// A v3 extension does not show up in AudioComponentFindNext.
			// AVAudioUnitComponentManager is what finds it
			NSArray<AVAudioUnitComponent *> *found =
			    [[AVAudioUnitComponentManager sharedAudioUnitComponentManager]
			        componentsMatchingDescription:desc];
			if (!found.count) {
				std::fprintf(stderr, "システムに登録されていない。make install-auv3 を先に\n");
				return 1;
			}
			for (AVAudioUnitComponent *c in found)
				std::printf("見つけた: %s / %s（v3=%d）\n",
				            c.manufacturerName.UTF8String, c.name.UTF8String,
				            (int)c.audioComponentDescription.componentFlags);
			// Load it into its own sandboxed process, as a DAW does
			dispatch_semaphore_t done = dispatch_semaphore_create(0);
			[AUAudioUnit instantiateWithComponentDescription:desc
			                                          options:kAudioComponentInstantiation_LoadOutOfProcess
			                                completionHandler:^(AUAudioUnit *unit, NSError *e) {
				au = unit; err = e;
				dispatch_semaphore_signal(done);
			}];
			dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30ll * NSEC_PER_SEC));
			std::printf("（システムに登録された .appex を、別プロセスで掴んだ）\n");
		} else {
			// **Registering in-process needs a different subtype.**
			// If an .appex with the same type, subtype and manufacturer is
			// registered with the system, that one wins, and opening it
			// synchronously fails with -10863
			desc.componentSubType = 'MU2t';
			[SMU2000AudioUnit registerSubclass:[SMU2000AudioUnit class]
			            asComponentDescription:desc
			                              name:@"tarboh: MU2000 (test)"
			                           version:65536];
			au = [[AUAudioUnit alloc] initWithComponentDescription:desc error:&err];
		}
		if (!au) {
			std::fprintf(stderr, "作れない: %s\n",
			             err ? err.localizedDescription.UTF8String : "(理由なし)");
			return 1;
		}

		// ---- Print the shape of the ports
		std::printf("口:\n");
		for (NSUInteger i = 0; i < au.outputBusses.count; i++)
			std::printf("  出力 %lu  %-12s %u ch\n", (unsigned long)i,
			            [au.outputBusses objectAtIndexedSubscript:i].name.UTF8String,
			            (unsigned)[au.outputBusses objectAtIndexedSubscript:i].format.channelCount);
		for (NSUInteger i = 0; i < au.inputBusses.count; i++)
			std::printf("  入力 %lu  %-12s %u ch\n", (unsigned long)i,
			            [au.inputBusses objectAtIndexedSubscript:i].name.UTF8String,
			            (unsigned)[au.inputBusses objectAtIndexedSubscript:i].format.channelCount);
		for (NSString *n in au.MIDIOutputNames)
			std::printf("  MIDI 出   %s\n", n.UTF8String);
		// If this says 1, a host never uses MIDI IN B -- or opens a second
		// instrument for it and boots a second machine
		std::printf("  MIDI 入   ケーブル %ld 本\n", (long)au.virtualMIDICableCount);

		// ---- Run the host at 48000, so the resampler is always exercised
		AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:HOST_RATE
		                                                                   channels:2];
		if (![[au.outputBusses objectAtIndexedSubscript:0] setFormat:fmt error:&err] ||
		    ![[au.inputBusses  objectAtIndexedSubscript:0] setFormat:fmt error:&err]) {
			std::fprintf(stderr, "形式を決められない: %s\n", err.localizedDescription.UTF8String);
			return 1;
		}
		au.maximumFramesToRender = BLOCK;

		// ---- Receive MIDI OUT
		__block int midi_out_msgs = 0;
		__block int midi_out_bytes = 0;
		au.MIDIOutputEventBlock = ^OSStatus(AUEventSampleTime when, uint8_t cable,
		                                    NSInteger length, const uint8_t *bytes) {
			(void)when; (void)cable; (void)bytes;
			midi_out_msgs++;
			midi_out_bytes += int(length);
			return noErr;
		};

		if (![au allocateRenderResourcesAndReturnError:&err]) {
			std::fprintf(stderr, "器を取れない: %s\n", err.localizedDescription.UTF8String);
			return 1;
		}
		std::printf("標本化周波数 %.0f Hz / 遅れ %.2f ms\n", HOST_RATE, au.latency * 1000.0);

		AURenderBlock render = au.renderBlock;
		AUScheduleMIDIEventBlock sched = au.scheduleMIDIEventBlock;
		AUMIDIEventListBlock schedList = au.scheduleMIDIEventListBlock;
		if (!render || !sched) {
			std::fprintf(stderr, "描き出しの口が無い\n");
			return 1;
		}

		// ---- Buffers
		std::vector<float> bl(BLOCK), br(BLOCK);
		uint8_t ablmem[sizeof(AudioBufferList) + sizeof(AudioBuffer)] = {};
		AudioBufferList *abl = reinterpret_cast<AudioBufferList *>(ablmem);

		// Feed 440Hz into A/D INPUT. Whether it comes back out depends on how
		// the machine is set up, so all this checks is that it gets pulled
		__block int pulled = 0;
		__block double phase = 0.0;
		AURenderPullInputBlock pull = ^AUAudioUnitStatus(AudioUnitRenderActionFlags *flags,
		                                                 const AudioTimeStamp *ts,
		                                                 AUAudioFrameCount n,
		                                                 NSInteger bus,
		                                                 AudioBufferList *data) {
			(void)flags; (void)ts; (void)bus;
			pulled++;
			for (UInt32 b = 0; b < data->mNumberBuffers; b++) {
				float *p = static_cast<float *>(data->mBuffers[b].mData);
				for (AUAudioFrameCount i = 0; i < n; i++)
					p[i] = 0.25f * std::sin(2.0 * M_PI * 440.0 * (phase + i) / HOST_RATE);
			}
			phase += n;
			return noErr;
		};

		std::vector<float> rec_l, rec_r;
		AudioTimeStamp ts{};
		ts.mFlags = kAudioTimeStampSampleTimeValid;

		auto one_block = [&](void) {
			abl->mNumberBuffers = 2;
			abl->mBuffers[0].mNumberChannels = 1;
			abl->mBuffers[0].mDataByteSize = BLOCK * sizeof(float);
			abl->mBuffers[0].mData = bl.data();
			abl->mBuffers[1].mNumberChannels = 1;
			abl->mBuffers[1].mDataByteSize = BLOCK * sizeof(float);
			abl->mBuffers[1].mData = br.data();
			AudioUnitRenderActionFlags f = 0;
			const AUAudioUnitStatus st = render(&f, &ts, BLOCK, 0, abl, pull);
			ts.mSampleTime += BLOCK;
			return st;
		};

		// ---- --state: write out the booted state and stop.
		//
		// **Render nothing at all.** The boot runs on the engine's own thread,
		// so waiting is all that is needed. Rendering would advance the machine
		// and make a run that skipped the boot incomparable with one that did
		// not
		if (state_out) {
			[NSThread sleepForTimeInterval:10.0];
			NSData *blob = au.fullState[@"S-MU2000.nvram"];
			if (!blob.length) {
				std::fprintf(stderr, "状態が取れない（起動していない）\n");
				return 1;
			}
			[blob writeToFile:[NSString stringWithUTF8String:state_out] atomically:YES];
			std::printf("状態を書き出した: %s（%lu バイト）\n",
			            state_out, (unsigned long)blob.length);
			[au deallocateRenderResources];
			return 0;
		}

		// ---- Wait for the boot.
		//
		// **Wait in real time.** Reading the ROMs and idling the firmware runs
		// on the engine's own thread and has nothing to do with how much has
		// been rendered here. Rendering before the machine is up returns
		// silence immediately, so a render loop finishes in an instant and the
		// plug-in looks like it makes no sound (which is exactly how it looked).
		//
		// Waiting by rendering until sound appears would also mean the machine
		// has advanced by a different amount depending on how fast the boot was
		// -- instant from a snapshot, seconds without one -- and two runs could
		// no longer be compared. So: do not render, wait a fixed time
		std::printf("起動を待つ");
		std::fflush(stdout);
		for (int i = 0; i < 40; i++) {
			[NSThread sleepForTimeInterval:0.25];
			if (!(i % 4)) { std::printf("."); std::fflush(stdout); }
		}
		std::printf(" 済み\n");

		// ---- --smf: play a MIDI file through the plug-in.
		//
		// Fed in the way a host feeds it (scheduleMIDIEventBlock) and written
		// out as a WAV. Comparing that against what build/render makes of the
		// same file says whether **only the plug-in's road** is at fault
		if (smf_path) {
			std::vector<smf::event> evs;
			std::string serr;
			if (!smf::load(smf_path, evs, serr)) {
				std::fprintf(stderr, "MIDI を読めない: %s\n", serr.c_str());
				return 1;
			}
			std::printf("MIDI: %s（%zu 事象）\n", smf_path, evs.size());

			// What Cog sends before starting a song, as seen in midi-in.log:
			// all-sound-off and all-notes-off on all 16 channels of both ports,
			// then **a GS reset on both ports**
			if (preamble) {
				if (preamble & 1)
					for (int port = 0; port < 2; port++)
						for (int ch = 0; ch < 16; ch++) {
							const uint8_t m[6] = { uint8_t(0xB0 | ch), 0x78, 0x00,
							                       uint8_t(0xB0 | ch), 0x7B, 0x00 };
							sched(AUEventSampleTimeImmediate, uint8_t(port), 6, m);
						}
				const uint8_t gs[11] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40,
				                         0x00, 0x7F, 0x00, 0x41, 0xF7 };
				if (preamble & 2) sched(AUEventSampleTimeImmediate, 0, 11, gs);
				if (preamble & 4) sched(AUEventSampleTimeImmediate, 1, 11, gs);
				std::printf("（前口上 印=%d）\n", preamble);
			}
			const double seconds = wav ? 22.0 : 10.0;
			size_t next = 0;
			double t = 0.0;
			while (t < seconds) {
				// Hand over the events falling inside this block, each at its own offset
				const double t_end = t + double(BLOCK) / HOST_RATE;
				while (next < evs.size() && evs[next].time < t_end) {
					const int port = smf::mu_port(evs[next].port, true);
					if (port >= 0 && !evs[next].bytes.empty()) {
						const std::vector<u8> &b = evs[next].bytes;
						const AUAudioFrameCount off =
						    AUAudioFrameCount(std::max(0.0, (evs[next].time - t) * HOST_RATE));
						const AUEventSampleTime when =
						    AUEventSampleTimeImmediate + (off < BLOCK ? off : BLOCK - 1);
						// As the fixed Cog does: **everything down one UMP
						// road, SysEx included.** One road means a SysEx and a
						// program change stamped at the same time cannot swap
						// places
						if (split && schedList)
							send_ump(schedList, when, uint8_t(port), b.data(), b.size());
						else
							sched(when, uint8_t(port), NSInteger(b.size()), b.data());
					}
					next++;
				}
				if (one_block() != noErr) { std::fprintf(stderr, "描き出しが失敗\n"); return 1; }
				rec_l.insert(rec_l.end(), bl.begin(), bl.end());
				rec_r.insert(rec_r.end(), br.begin(), br.end());
				t = t_end;
			}
			float peak = 0.0f; double sq = 0.0;
			for (size_t i = 0; i < rec_l.size(); i++) {
				peak = std::max(peak, std::max(std::fabs(rec_l[i]), std::fabs(rec_r[i])));
				sq += double(rec_l[i]) * rec_l[i] + double(rec_r[i]) * rec_r[i];
			}
			std::printf("MAIN OUT   peak %.4f  rms %.5f（%.1f 秒）\n",
			            peak, std::sqrt(sq / double(rec_l.size() * 2)),
			            double(rec_l.size()) / HOST_RATE);
			if (wav) write_wav(wav, rec_l, rec_r, HOST_RATE);
			[au deallocateRenderResources];
			return 0;
		}

		// ---- Check each port
		auto play = [&](int cable, uint8_t note, double seconds) {
			const uint8_t on[3]  = { 0x90, note, 100 };
			const uint8_t off[3] = { 0x80, note, 0 };
			sched(AUEventSampleTimeImmediate, uint8_t(cable), 3, on);
			const int nb = int(seconds * HOST_RATE / BLOCK);
			float pk = 0.0f;
			for (int i = 0; i < nb; i++) {
				one_block();
				for (AUAudioFrameCount k = 0; k < BLOCK; k++)
					pk = std::max(pk, std::max(std::fabs(bl[k]), std::fabs(br[k])));
				rec_l.insert(rec_l.end(), bl.begin(), bl.end());
				rec_r.insert(rec_r.end(), br.begin(), br.end());
				if (i == nb / 2)
					sched(AUEventSampleTimeImmediate, uint8_t(cable), 3, off);
			}
			return pk;
		};

		// **A different voice and a different pitch down each port.**
		//
		// Channel 1 of cable n reaches part 16n+1: A is 1, B is 17, C is 33,
		// D is 49. C and D exist only over USB on real hardware. Giving each
		// port its own voice is what separates "all four work" from "all four
		// landed on the same part"
		static const struct { const char *name; uint8_t prog, note; } kPort[] = {
			{ "A", 0,  60 },     // Grand Piano
			{ "B", 19, 72 },     // Church Organ
			{ "C", 33, 48 },     // Finger Bass
			{ "D", 56, 67 },     // Trumpet
		};
		const int ports = int(au.virtualMIDICableCount);
		const int nport = std::min(ports, int(sizeof(kPort) / sizeof(kPort[0])));

		for (int p = 0; p < nport; p++) {
			const uint8_t pc[2] = { 0xC0, kPort[p].prog };
			sched(AUEventSampleTimeImmediate, uint8_t(p), 2, pc);
		}
		for (int i = 0; i < 40; i++) one_block();

		float pk[4] = {};
		for (int p = 0; p < nport; p++) {
			pk[p] = play(p, kPort[p].note, 1.5);
			for (int i = 0; i < 60; i++) one_block();
		}

		// ---- MIDI OUT. The machine answers an Identity Request (GM)
		const uint8_t ident[6] = { 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7 };
		if (sysex_ump && schedList) {
			// **Send the SysEx as UMP (SysEx7).**
			// Type 0x3, six bytes at a time, F0 and F7 left off; the status
			// nibble carries that instead:
			//   0 = complete in one / 1 = start / 2 = continue / 3 = end
			const uint8_t *body = ident + 1;               // drop the F0
			const size_t   n    = sizeof(ident) - 2;       // and the F7
			MIDIEventList list;
			MIDIEventPacket *pk = MIDIEventListInit(&list, kMIDIProtocol_1_0);
			size_t at = 0;
			while (at < n || at == 0) {
				const size_t take = std::min<size_t>(6, n - at);
				const bool first = (at == 0), last = (at + take >= n);
				const uint8_t st = first && last ? 0 : first ? 1 : last ? 3 : 2;
				uint32_t w0 = (uint32_t)0x3 << 28 | (uint32_t)0 << 24 |
				              (uint32_t)st << 20 | (uint32_t)take << 16;
				uint32_t w1 = 0;
				for (size_t k = 0; k < take; k++) {
					const uint8_t v = body[at + k];
					if (k < 2) w0 |= (uint32_t)v << (8 * (1 - k));
					else       w1 |= (uint32_t)v << (8 * (5 - k));
				}
				uint32_t words[2] = { w0, w1 };
				pk = MIDIEventListAdd(&list, sizeof(list), pk, 0, 2, words);
				at += take;
				if (last) break;
			}
			schedList(AUEventSampleTimeImmediate, 0, &list);
			std::printf("（識別要求を SysEx7 の UMP で送った）\n");
		} else {
			sched(AUEventSampleTimeImmediate, 0, 6, ident);
		}
		for (int i = 0; i < 200; i++) one_block();

		// ---- Results
		float peak = 0.0f;
		double sq = 0.0;
		for (size_t i = 0; i < rec_l.size(); i++) {
			peak = std::max(peak, std::max(std::fabs(rec_l[i]), std::fabs(rec_r[i])));
			sq += double(rec_l[i]) * rec_l[i] + double(rec_r[i]) * rec_r[i];
		}
		const double rms = std::sqrt(sq / double(rec_l.size() * 2));
		std::printf("\n");
		bool all_ports = true;
		for (int p = 0; p < nport; p++) {
			std::printf("MIDI IN %s（ケーブル %d → パート %2d） peak %.4f  %s\n",
			            kPort[p].name, p, p * 16 + 1, pk[p],
			            pk[p] > 0.0f ? "鳴った" : "**鳴らない**");
			all_ports = all_ports && pk[p] > 0.0f;
		}
		std::printf("MAIN OUT   peak %.4f  rms %.5f（%zu フレーム / %.0f Hz）\n",
		            peak, rms, rec_l.size(), HOST_RATE);
		std::printf("A/D INPUT  引かれた回数 %d\n", pulled);
		std::printf("MIDI OUT   %d メッセージ / %d バイト  %s\n",
		            midi_out_msgs, midi_out_bytes,
		            midi_out_msgs ? "（識別要求に返事が来た）" : "**返事が無い**");

		if (wav)
			write_wav(wav, rec_l, rec_r, HOST_RATE);

		[au deallocateRenderResources];
		return (peak > 0.0f && all_ports) ? 0 : 2;
	}
}
