// license:BSD-3-Clause
//
// The AU's editor: the machine's front panel, in Cocoa.
//
// There is no panel in here. A host reads kAudioUnitProperty_CocoaUI, makes the
// class at the bottom of this file and calls uiViewForAudioUnit:withSize:; what
// it gets back is what src/vst3/panel_nsview.mm builds -- the *same* view the
// VST3 build shows and the *same* view the AUv3 puts in its view controller
// (src/auv3/factory.mm). All this file does is the AUv2 way of asking.
//
// Objective-C++ for the same reason view_mac.mm is: compat/gdi.h and Cocoa both
// define BOOL, and Quickdraw defines Polygon, so the drawing layer is reached
// only through panel_nsview.h and never included here.

#import <Cocoa/Cocoa.h>

#include <AudioToolbox/AudioToolbox.h>
// AUCocoaUIBase itself lives here, and AudioToolbox.h does not pull it in
#import <AudioToolbox/AUCocoaUIView.h>

#include "editor.h"

#include "vst3/engine.h"
#include "vst3/panel_nsview.h"

namespace smu2000 {
namespace au {

// Must match the @interface below: this is the string a host hands to
// NSClassFromString
const char *const kViewClassName = "SMU2000AUViewFactory";

bool view_info(CFURLRef *out_bundle_url, CFStringRef *out_class_name)
{
	*out_bundle_url  = nullptr;
	*out_class_name  = nullptr;

	// The class below lives in this bundle, so asking for it by name answers
	// with wherever the host has put us -- build/ while developing, the
	// Components directory once installed. No path is written down
	NSString *name = [NSString stringWithUTF8String:kViewClassName];
	Class cls = name ? NSClassFromString(name) : Nil;
	NSBundle *bundle = cls ? [NSBundle bundleForClass:cls] : nil;
	NSURL *url = [bundle bundleURL];
	if (!url)
		return false;

	// One reference for the host on each. The property publishes them and hosts
	// do not all release what they read, so a fresh pair is made per call
	// rather than a shared pair being handed out over and over
	CFURLRef cf_url = (CFURLRef)CFRetain((__bridge CFTypeRef)url);
	CFStringRef cf_name = CFStringCreateWithCString(kCFAllocatorDefault, kViewClassName,
	                                                kCFStringEncodingUTF8);
	if (!cf_url || !cf_name) {
		if (cf_url)
			CFRelease(cf_url);
		if (cf_name)
			CFRelease(cf_name);
		return false;
	}

	*out_bundle_url = cf_url;
	*out_class_name = cf_name;
	return true;
}

} // namespace au
} // namespace smu2000


// Builds one editor for an open instance, or nil if it cannot be built. A
// factory function: every call makes a new view, as AUCocoaUIBase requires
static NSView *make_editor(AudioUnit unit, NSSize preferred)
{
	// The engine of the open instance. AudioUnitGetProperty runs the ordinary
	// property dispatch, so plugin.cpp's answer already knows which instance
	// this is -- see the note about the handle in editor.h
	void *eng = nullptr;
	UInt32 size = sizeof(eng);
	if (AudioUnitGetProperty(unit, smu2000::au::kEngineProperty, kAudioUnitScope_Global, 0,
	                         &eng, &size) != noErr || !eng)
		return nil;

	return smu2000::vst3::make_panel_view(*static_cast<smu2000::vst3::engine *>(eng),
	                                      preferred);
}


// What kViewClassName names. A host allocs it, asks for the interface version
// (0 is the only one there has ever been), and then asks for the view
@interface SMU2000AUViewFactory : NSObject <AUCocoaUIBase>
@end

@implementation SMU2000AUViewFactory

- (unsigned)interfaceVersion { return 0; }

- (NSView *)uiViewForAudioUnit:(AudioUnit)inAudioUnit withSize:(NSSize)inPreferredSize
{
	return make_editor(inAudioUnit, inPreferredSize);
}

// Hosts show this in the menu that picks between a unit's views. The protocol
// asks for a copy rather than a static string
- (NSString *)description { return [@"S-MU2000 Panel" copy]; }

@end
