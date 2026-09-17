// license:BSD-3-Clause
//
// The application the AUv3 lives inside.
//
// **macOS only recognises an .appex that sits inside an app**, so a plug-in
// cannot be shipped on its own. This application makes no sound. Launching it
// once is what makes the system find the .appex inside it and list it in a
// DAW's instrument menu.
//
// The window shows where the ROMs were found, because that is nearly always
// what is wrong when nothing plays. It asks the same way the plug-in does
// (smu2000::plug::find_rom_dir), so what it prints is what the plug-in sees --
// with one large caveat, which the window says out loud: the extension is
// sandboxed and can only read inside its own bundle, so the ROMs have to be
// built into it (make auv3 AUV3_ROMS=roms).

#import <Cocoa/Cocoa.h>

#include "vst3/engine.h"

#include <string>

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate {
	NSWindow *_window;
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
	(void)note;

	std::string tried;
	const std::string dir = smu2000::plug::find_rom_dir(tried);

	NSString *body;
	if (dir.empty()) {
		body = [NSString stringWithFormat:
		        @"No ROMs found.\n\n"
		         "Looked in, in this order:\n%s\n"
		         "Put them in one of those, or point S_MU2000_ROMS at them.\n"
		         "A rom directory is built with tools/make_roms.py.\n\n"
		         "The AUv3 extension is sandboxed and cannot read any of them:\n"
		         "build the ROMs into it with\n\n"
		         "    make auv3 AUV3_ROMS=roms\n",
		        tried.c_str()];
	} else {
		body = [NSString stringWithFormat:
		        @"ROMs: %s\n\n"
		         "Registered as an Audio Unit.\n"
		         "It appears in a DAW's instrument list as \"tarboh: MU2000\".\n\n"
		         "  output     MAIN OUT L/R\n"
		         "  input      A/D INPUT (AD1 left, AD2 right)\n"
		         "  MIDI in    cable 0 = IN A (parts 1-16)\n"
		         "             cable 1 = IN B (parts 17-32)\n"
		         "  MIDI out   MIDI OUT (the firmware's replies)\n\n"
		         "This path is what *this application* can see. The extension is\n"
		         "sandboxed and reads only inside its own bundle, so the ROMs it\n"
		         "uses are the ones built into it with\n\n"
		         "    make auv3 AUV3_ROMS=roms\n",
		        dir.c_str()];
	}

	const NSRect frame = NSMakeRect(0, 0, 620, 400);
	_window = [[NSWindow alloc]
	    initWithContentRect:frame
	              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                         NSWindowStyleMaskMiniaturizable)
	                backing:NSBackingStoreBuffered
	                  defer:NO];
	_window.title = @"S-MU2000";
	[_window center];

	NSTextView *text = [[NSTextView alloc] initWithFrame:NSInsetRect(frame, 16, 16)];
	text.editable = NO;
	text.drawsBackground = NO;
	text.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
	text.string = body;

	NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSInsetRect(frame, 16, 16)];
	scroll.documentView = text;
	scroll.hasVerticalScroller = YES;
	scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	_window.contentView = scroll;

	[_window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
	(void)app;
	return YES;
}

@end

int main(int argc, const char *argv[])
{
	(void)argc; (void)argv;
	@autoreleasepool {
		NSApplication *app = [NSApplication sharedApplication];
		AppDelegate *del = [[AppDelegate alloc] init];
		app.delegate = del;
		[app setActivationPolicy:NSApplicationActivationPolicyRegular];
		[app run];
	}
	return 0;
}
