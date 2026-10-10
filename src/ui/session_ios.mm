// license:BSD-3-Clause
//
// AVAudioSession, the half of Apple audio that exists only on iOS. See
// session_ios.h for why it is a file of its own and not part of either back
// end.
//
// A session is the only way to say what the app is doing (Playback or
// PlayAndRecord), which decides the route - and therefore whether picking the
// microphone silences the speaker - and it is what the system arbitrates with,
// so a call or Siri arrives as an interruption to resume from rather than as
// silence. Every member is API_UNAVAILABLE(macos), so the macOS half answers
// session_open() with "yes" and nothing else.

#import "ui/session_ios.h"

#import <Foundation/Foundation.h>

#include "ui/audio_apple.h"
#include "ui/audio_out.h"

#include "compat/cli_text.h"

#include <cstdio>

namespace ui::ios {

namespace {

bool fail(std::string &err, NSError *e, const char *what)
{
	err = std::string(what) + ": " + (e ? [[e localizedDescription] UTF8String] : "?");
	return false;
}

// Port names, in the order the session lists them.
std::vector<std::string> names_of(NSArray<AVAudioSessionPortDescription *> *ports)
{
	std::vector<std::string> names;
	for (AVAudioSessionPortDescription *p in ports) {
		const char *n = [[p portName] UTF8String];
		if (n)
			names.emplace_back(n);
	}
	return names;
}

// The one place that knows what the session offers: the names the menu shows
// and the port a choice is asked for come from here, so a name that cannot be
// honoured is never one we offered.
NSArray<AVAudioSessionPortDescription *> *available_inputs()
{
	return [[AVAudioSession sharedInstance] availableInputs];
}

} // namespace

bool session_open_output(int latency_ms, std::string &err)
{
	AVAudioSession *s = [AVAudioSession sharedInstance];
	NSError *e = nil;
	if (![s setCategory:AVAudioSessionCategoryPlayback error:&e])
		return fail(err, e, "AVAudioSession category");
	if (![s setPreferredSampleRate:double(AUDIO_RATE) error:&e])
		std::fprintf(stderr, "[ios] audio: 44100 Hz refused, taking what comes\n");
	if (latency_ms > 0 &&
	    ![s setPreferredIOBufferDuration:double(latency_ms) / 1000.0 error:&e])
		std::fprintf(stderr, "[ios] audio: buffer duration refused\n");
	if (![s setActive:YES error:&e])
		return fail(err, e, "AVAudioSession activate");
	return true;
}

bool session_open_input(std::string &err)
{
	AVAudioSession *s = [AVAudioSession sharedInstance];
	NSError *e = nil;
	if (![s setCategory:AVAudioSessionCategoryPlayAndRecord
	          withOptions:AVAudioSessionCategoryOptionDefaultToSpeaker
	                error:&e])
		return fail(err, e, "AVAudioSession category");
	if (![s setActive:YES error:&e])
		return fail(err, e, "AVAudioSession activate");
	return true;
}

std::vector<std::string> input_port_names()
{
	return names_of(available_inputs());
}

std::vector<std::string> output_port_names()
{
	AVAudioSession *s = [AVAudioSession sharedInstance];
	return names_of([[s currentRoute] outputs]);
}

bool set_preferred_input(const std::string &name)
{
	AVAudioSessionPortDescription *port = nil;
	for (AVAudioSessionPortDescription *p in available_inputs()) {
		const char *n = [[p portName] UTF8String];
		if (n && name == n) {
			port = p;
			break;
		}
	}
	if (!port)
		return true;   // not a name the session offers: its own choice stands
	NSError *e = nil;
	if (![[AVAudioSession sharedInstance] setPreferredInput:port error:&e]) {
		std::fprintf(stderr, "[ios] audio in: preferred input refused: %s\n",
		             e ? [[e localizedDescription] UTF8String] : "?");
		return false;
	}
	std::fprintf(stderr, "[ios] audio in: input port %s\n", name.c_str());
	return true;
}

bool request_mic_permission(std::string &err)
{
	const AVAudioApplicationRecordPermission perm =
	    AVAudioApplication.sharedInstance.recordPermission;
	if (perm == AVAudioApplicationRecordPermissionGranted)
		return true;
	if (perm == AVAudioApplicationRecordPermissionUndetermined)
		[AVAudioApplication requestRecordPermissionWithCompletionHandler:^(BOOL granted) {
			(void)granted;   // the answer arrives for the next ask, not this one
		}];
	err = (perm == AVAudioApplicationRecordPermissionDenied)
	    ? CLI_T("The microphone is denied (allow it in Settings)",
	            "マイクが拒否されている（設定アプリで許可）")
	    : CLI_T("Asked for the microphone; pick again",
	            "マイクの許可を求めた。もう一度選ぶ");
	return false;
}

// The two registers. File scope rather than members: AVAudioSession is one
// process-wide object, so one pair of observers serves every engine, and these
// are the only things that have to outlive an engine - the engines themselves
// never do, since a register is cleared in stop().
static std::function<void()> s_output_change, s_input_change;

static void call_registers()
{
	if (auto fn = s_output_change)
		fn();
	if (auto fn = s_input_change)
		fn();
}

// Installed on whichever call comes first, since both halves need both
// notifications and a route change stops whichever engine is running.
//
// On the main queue, not queue:nil. AVAudioSession posts these on whichever
// thread changed the route, and with queue:nil the block runs there - so
// call_registers() would touch s_output_change / s_input_change (a
// std::function being assigned by stop() on the main thread) and the restart it
// starts, on a thread that is not the one holding the engines. The main queue is
// where the engines were made and are stopped.
static void install_observers()
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		NSOperationQueue *main = [NSOperationQueue mainQueue];
		[[NSNotificationCenter defaultCenter]
		    addObserverForName:AVAudioSessionRouteChangeNotification
		                    object:nil
		                     queue:main
		                usingBlock:^(NSNotification *note) {
			            (void)note;
			            call_registers();
		            }];
		// Only an interruption that ended and may be resumed is worth restarting
		// for: one that began, or one the system will not resume, stays silent.
		// The one that ends without it leaves the engine stopped and nothing to
		// restart it - the app's foreground handler is what covers that.
		[[NSNotificationCenter defaultCenter]
		    addObserverForName:AVAudioSessionInterruptionNotification
		                    object:nil
		                     queue:main
		                usingBlock:^(NSNotification *note) {
			            NSNumber *type = note.userInfo[AVAudioSessionInterruptionTypeKey];
			            NSNumber *opts = note.userInfo[AVAudioSessionInterruptionOptionKey];
			            if (type && type.unsignedIntegerValue == AVAudioSessionInterruptionTypeEnded &&
			                opts && (opts.unsignedIntegerValue &
			                         AVAudioSessionInterruptionOptionShouldResume))
			                call_registers();
		            }];
	});
}

void watch_output(const std::function<void()> &on_change)
{
	install_observers();
	s_output_change = on_change;
}

void watch_input(const std::function<void()> &on_change)
{
	install_observers();
	s_input_change = on_change;
}

} // namespace ui::ios

// ---- The half of the contract these answer ---------------------------------
//
// Both directions need both watchers - a route change stops whichever engine is
// running - so they are defined once here rather than twice in the two halves.

namespace ui::apple {

void watch_output_session(const std::function<void()> &on_change)
{
	ios::watch_output(on_change);
}

void watch_input_session(const std::function<void()> &on_change)
{
	ios::watch_input(on_change);
}

} // namespace ui::apple
