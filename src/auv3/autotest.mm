// license:BSD-3-Clause
//
// AUv3 を .appex にせずその場で試す道具。
//
// AUAudioUnit は registerSubclass: で自分のプロセスに登録できる。システムへの
// 登録（.appex）を通さずに、**口の形と音と MIDI の行き来だけ**を確かめられる。
//
//   build/autest [出力 wav]
//
// 見るもの:
//   ・口の数と名前（MAIN OUT / A/D INPUT / MIDI OUT）
//   ・ホストの標本化周波数（48000）で鳴ること＝変換器が働くこと
//   ・MIDI IN A-D（ケーブル 0-3）がパート 1・17・33・49に届くこと
//   ・A/D INPUT に入れた音が出てくること
//   ・MIDI OUT（firmware の返事）が受け取れること

#import "audio_unit.h"

#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>

#include "smf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr double HOST_RATE = 48000.0;      // わざと 44100 から外す
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


// Cog の AUPlayer::sendEventList と同じ組み立て。SysEx は SysEx7（種別 0x3）で
// 6 バイトずつ、F0 と F7 は載せない（status が代わり: 0 完結 / 1 始 / 2 続 / 3 終）
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
		// --system を付けると、その場で登録する代わりに**システムに登録済みの
		// .appex**（砂場の中で別プロセスに読み込まれるもの）を掴む。
		// DAW が掴むのと同じ道。ROM がバンドルに入っているかもこれで分かる
		bool use_system = false;
		const char *wav = nullptr;
		const char *state_out = nullptr;      // --state <file>
		const char *smf_path = nullptr;       // --smf <MIDI ファイル>
		bool split = false;                   // --split: Cog と同じく道を 2 つに分ける
		bool sysex_ump = false;               // --sysex-ump: SysEx も MIDIEventList で送る
		int view_check = 0;                   // --view / --view2: 画面が出るかだけ見る
		int preamble = 0;                     // --pre <印>: 1 消音 / 2 A に GS / 4 B に GS
		for (int i = 1; i < argc; i++) {
			if (!std::strcmp(argv[i], "--system")) use_system = true;
			else if (!std::strcmp(argv[i], "--state") && i + 1 < argc) state_out = argv[++i];
			else if (!std::strcmp(argv[i], "--smf") && i + 1 < argc) smf_path = argv[++i];
			else if (!std::strcmp(argv[i], "--split")) split = true;
			else if (!std::strcmp(argv[i], "--sysex-ump")) sysex_ump = true;
			else if (!std::strcmp(argv[i], "--view")) view_check = 1;
			else if (!std::strcmp(argv[i], "--view2")) view_check = 2;
			else if (!std::strcmp(argv[i], "--preamble")) preamble = 7;
			else if (!std::strcmp(argv[i], "--pre") && i + 1 < argc) preamble = atoi(argv[++i]);
			else wav = argv[i];
		}

		AudioComponentDescription desc{};
		desc.componentType         = kAudioUnitType_MusicDevice;   // 'aumu'
		desc.componentSubType      = 'SMU3';
		desc.componentManufacturer = 'Trbh';

		__block AUAudioUnit *au = nil;
		__block NSError *err = nil;
		if (use_system) {
			// v3 の拡張は AudioComponentFindNext には出てこない。
			// 見つけ方は AVAudioUnitComponentManager
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
			// 砂場の中の別プロセスへ読み込ませる（DAW と同じ）
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
			// **その場で登録するときは別の subtype を使う。**
			// 同じ型・種別・製造者で .appex がシステムに登録されていると、
			// そちらが勝ち、同期で開こうとして -10863 で失敗する
			desc.componentSubType = 'SMUt';
			[SMU2000AudioUnitV3 registerSubclass:[SMU2000AudioUnitV3 class]
			            asComponentDescription:desc
			                              name:@"S-MU2000 AUv3 (test)"
			                           version:65536];
			au = [[AUAudioUnit alloc] initWithComponentDescription:desc error:&err];
		}
		if (!au) {
			std::fprintf(stderr, "作れない: %s\n",
			             err ? err.localizedDescription.UTF8String : "(理由なし)");
			return 1;
		}

		// ---- 口の形を出す
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
		// ここが 4 だと、ホストはケーブル 2・3（パート 33-64）を使わない
		// （か、口ごとに音源をもう 1 台開いて起動を何回もやる）
		std::printf("  MIDI 入   ケーブル %ld 本\n", (long)au.virtualMIDICableCount);

		// ---- ホストの周波数を 48000 にする（MU2000 は 44100 なので変換が入る）
		AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:HOST_RATE
		                                                                   channels:2];
		if (![[au.outputBusses objectAtIndexedSubscript:0] setFormat:fmt error:&err] ||
		    ![[au.inputBusses  objectAtIndexedSubscript:0] setFormat:fmt error:&err]) {
			std::fprintf(stderr, "形式を決められない: %s\n", err.localizedDescription.UTF8String);
			return 1;
		}
		au.maximumFramesToRender = BLOCK;

		// ---- MIDI OUT を受ける
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

		// ---- --view2: v2 の口から画面を頼む（kAudioUnitProperty_RequestViewController）。
		// v3 の方法で返事が無いときの切り分け用
		if (view_check == 2) {
			AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
			if (!comp) {
				std::fprintf(stderr, "v2 の口から見つからない\n");
				return 1;
			}
			AudioUnit unit = nullptr;
			if (AudioComponentInstanceNew(comp, &unit) != noErr || !unit) {
				std::fprintf(stderr, "v2 の口から開けない\n");
				return 1;
			}
			__block NSViewController *vc2 = nil;
			__block bool answered2 = false;
			void (^cb)(AUViewControllerBase *) =
			    ^(AUViewControllerBase *v) { vc2 = v; answered2 = true; };
			const OSStatus st = AudioUnitSetProperty(
			    unit, kAudioUnitProperty_RequestViewController,
			    kAudioUnitScope_Global, 0, &cb, UInt32(sizeof(cb)));
			std::printf("setProperty: %d\n", int(st));
			const double until = NSDate.timeIntervalSinceReferenceDate + 15.0;
			while (!answered2 && NSDate.timeIntervalSinceReferenceDate < until)
				[[NSRunLoop currentRunLoop] runUntilDate:
				    [NSDate dateWithTimeIntervalSinceNow:0.1]];
			std::printf("v2 経由の画面: %s\n", (vc2 && vc2.view) ? "出た" : "出ない");
			AudioComponentInstanceDispose(unit);
			[au deallocateRenderResources];
			return (vc2 && vc2.view) ? 0 : 1;
		}

		// ---- --view: パネルが出るか。音は鳴らさない
		if (view_check) {
			std::printf("requestViewController に答えられる: %d\n",
			            (int)[au respondsToSelector:@selector(requestViewControllerWithCompletionHandler:)]);
			// 主糸を止めると返事が来ないことがあるので、走らせながら待つ
			__block NSViewController *vc = nil;
			__block bool answered = false;
			[au requestViewControllerWithCompletionHandler:^(NSViewController *v) {
				vc = v;
				answered = true;
			}];
			const double until = NSDate.timeIntervalSinceReferenceDate + 15.0;
			while (!answered && NSDate.timeIntervalSinceReferenceDate < until)
				[[NSRunLoop currentRunLoop] runUntilDate:
				    [NSDate dateWithTimeIntervalSinceNow:0.1]];
			if (!vc || !vc.view) {
				std::fprintf(stderr, "画面が出ない\n");
				return 1;
			}
			const NSRect f = vc.view.frame;
			std::printf("画面  %s  %.0f x %.0f\n", vc.className.UTF8String,
			            f.size.width, f.size.height);
			[au deallocateRenderResources];
			return 0;
		}

		AURenderBlock render = au.renderBlock;
		AUScheduleMIDIEventBlock sched = au.scheduleMIDIEventBlock;
		// scheduleMIDIEventListBlock is macOS 12. On anything older this stays
		// null, and the sched() path below is what carries the notes instead.
		// (The build targets 11.0, so the 12.0 entry point has to be asked for
		// by hand rather than assumed.)
		AUMIDIEventListBlock schedList = nullptr;
		if (@available(macOS 12.0, *))
			schedList = au.scheduleMIDIEventListBlock;
		if (!render || !sched) {
			std::fprintf(stderr, "描き出しの口が無い\n");
			return 1;
		}

		// ---- 器
		std::vector<float> bl(BLOCK), br(BLOCK);
		uint8_t ablmem[sizeof(AudioBufferList) + sizeof(AudioBuffer)] = {};
		AudioBufferList *abl = reinterpret_cast<AudioBufferList *>(ablmem);

		// A/D INPUT に 440Hz を入れる。返ってくるかは実機の設定次第なので、
		// ここでは「引いてもらえるか」だけを見る
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

		// ---- --state: 起動が済んだ姿だけを書き出して終わる。
		//
		// **一切描き出さない。** 起動は engine の別スレッドで進むので、
		// 待つだけでよい。描き出すと機械が進んでしまい、
		// 「起動を飛ばした場合」と「飛ばさなかった場合」を比べられなくなる
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

		// ---- 起動を待つ。
		//
		// **実時間で待つこと。** ROM を読んで 4 秒ぶん空回しするのは engine の
		// 別スレッドで、こちらが描き出した量とは関係が無い。起動前の描き出しは
		// 無音をすぐ返すので、回すだけでは一瞬で終わってしまい、
		// 「鳴らないプラグイン」に見える（実際そうなった）
		// **描き出さずに、決め打ちの時間だけ待つ。**
		//
		// 鳴るまで描き出しながら待つと、起動が速い場合（写しから戻したとき）と
		// 遅い場合とで、測る前に機械が進んだ量が変わってしまい、2 つの回を
		// 比べられなくなる。起動は engine の別スレッドで進むので待つだけでよい
		std::printf("起動を待つ");
		std::fflush(stdout);
		for (int i = 0; i < 40; i++) {
			[NSThread sleepForTimeInterval:0.25];
			if (!(i % 4)) { std::printf("."); std::fflush(stdout); }
		}
		std::printf(" 済み\n");

		// ---- --smf: MIDI ファイルをプラグイン越しに鳴らす。
		//
		// ホストと同じ道（scheduleMIDIEventBlock）で流し込み、出てきた音を
		// WAV に書く。build/render が同じファイルを鳴らしたものと突き合わせれば、
		// **プラグインの道だけがおかしいのか**が分かる
		if (smf_path) {
			std::vector<smf::event> evs;
			std::string serr;
			if (!smf::load(smf_path, evs, serr)) {
				std::fprintf(stderr, "MIDI を読めない: %s\n", serr.c_str());
				return 1;
			}
			std::printf("MIDI: %s（%zu 事象）\n", smf_path, evs.size());

			// Cog が曲を始める前に送るもの（midi-in.log で見たとおり）:
			// 両方の口の全 16 ch に all sound off / all notes off、そのあと
			// **両方の口に GS リセット**
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
				// この塊のあいだに来る事象を、位置ごとに渡す
				const double t_end = t + double(BLOCK) / HOST_RATE;
				while (next < evs.size() && evs[next].time < t_end) {
					const int port = smf::mu_port(evs[next].port, true);
					if (port >= 0 && !evs[next].bytes.empty()) {
						const std::vector<u8> &b = evs[next].bytes;
						const AUAudioFrameCount off =
						    AUAudioFrameCount(std::max(0.0, (evs[next].time - t) * HOST_RATE));
						const AUEventSampleTime when =
						    AUEventSampleTimeImmediate + (off < BLOCK ? off : BLOCK - 1);
						// 直した Cog と同じ: **SysEx も含めて全部 UMP 1 本**で送る。
						// 道が 1 本なら、同じ時刻の SysEx と音色指定の前後が入れ替わらない
						// schedList が入说的是 12.0 以降なので、中で聞かなくても
						// ここに来ている時点で UMP の口は使える
						if (split && schedList) {
							if (@available(macOS 12.0, *))
								send_ump(schedList, when, uint8_t(port), b.data(), b.size());
							else
								sched(when, uint8_t(port), NSInteger(b.size()), b.data());
						} else
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

		// ---- 口ごとに確かめる
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

		// パート 1・17・33・49 に、耳で分かる違う音色を入れておく。
		// IN A-D の ch1 はパート 1・17・33・49 に届く決まり
		const uint8_t pcA[2] = { 0xC0, 0 };     // グランドピアノ
		const uint8_t pcB[2] = { 0xC0, 19 };    // チャーチオルガン
		const uint8_t pcC[2] = { 0xC0, 40 };    // バイオリン
		const uint8_t pcD[2] = { 0xC0, 56 };    // トランペット
		sched(AUEventSampleTimeImmediate, 0, 2, pcA);
		sched(AUEventSampleTimeImmediate, 1, 2, pcB);
		sched(AUEventSampleTimeImmediate, 2, 2, pcC);
		sched(AUEventSampleTimeImmediate, 3, 2, pcD);
		for (int i = 0; i < 40; i++) one_block();

		const float pk_a = play(0, 60, 1.5);
		for (int i = 0; i < 60; i++) one_block();
		const float pk_b = play(1, 72, 1.5);
		for (int i = 0; i < 60; i++) one_block();
		const float pk_c = play(2, 64, 1.5);
		for (int i = 0; i < 60; i++) one_block();
		const float pk_d = play(3, 76, 1.5);
		for (int i = 0; i < 60; i++) one_block();

		// ---- MIDI OUT。実機は識別要求に返事をする（GM の Identity Request）
		const uint8_t ident[6] = { 0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7 };
		if (sysex_ump && schedList) {
			if (@available(macOS 12.0, *)) {
				// **SysEx を UMP（SysEx7）で送る。**
				// 種別 0x3、6 バイトずつ、F0 と F7 は載せない（status が代わり）。
				//   status 0 = 1 つで完結 / 1 = 始まり / 2 = 続き / 3 = 終わり
				const uint8_t *body = ident + 1;               // F0 を外す
				const size_t   n    = sizeof(ident) - 2;       // F7 も外す
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
			} else
				sched(AUEventSampleTimeImmediate, 0, 6, ident);
		} else
			sched(AUEventSampleTimeImmediate, 0, 6, ident);
		for (int i = 0; i < 200; i++) one_block();

		// ---- 結果
		float peak = 0.0f;
		double sq = 0.0;
		for (size_t i = 0; i < rec_l.size(); i++) {
			peak = std::max(peak, std::max(std::fabs(rec_l[i]), std::fabs(rec_r[i])));
			sq += double(rec_l[i]) * rec_l[i] + double(rec_r[i]) * rec_r[i];
		}
		const double rms = std::sqrt(sq / double(rec_l.size() * 2));
		std::printf("\n");
		std::printf("MIDI IN A（ケーブル 0 → パート 1）  peak %.4f  %s\n",
		            pk_a, pk_a > 0.0f ? "鳴った" : "**鳴らない**");
		std::printf("MIDI IN B（ケーブル 1 → パート 17） peak %.4f  %s\n",
		            pk_b, pk_b > 0.0f ? "鳴った" : "**鳴らない**");
		std::printf("MIDI IN C（ケーブル 2 → パート 33） peak %.4f  %s\n",
		            pk_c, pk_c > 0.0f ? "鳴った" : "**鳴らない**");
		std::printf("MIDI IN D（ケーブル 3 → パート 49） peak %.4f  %s\n",
		            pk_d, pk_d > 0.0f ? "鳴った" : "**鳴らない**");
		std::printf("MAIN OUT   peak %.4f  rms %.5f（%zu フレーム / %.0f Hz）\n",
		            peak, rms, rec_l.size(), HOST_RATE);
		std::printf("A/D INPUT  引かれた回数 %d\n", pulled);
		std::printf("MIDI OUT   %d メッセージ / %d バイト  %s\n",
		            midi_out_msgs, midi_out_bytes,
		            midi_out_msgs ? "（識別要求に返事が来た）" : "**返事が無い**");

		if (wav)
			write_wav(wav, rec_l, rec_r, HOST_RATE);

		[au deallocateRenderResources];
		const bool all_cables = pk_a > 0.0f && pk_b > 0.0f && pk_c > 0.0f && pk_d > 0.0f;
		return peak > 0.0f && all_cables ? 0 : 2;
	}
}
