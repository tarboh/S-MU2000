// license:BSD-3-Clause
//
// The AUv3's way in. This is the .appex's NSExtensionPrincipalClass, and it is
// two things at once, which is how Apple's extension point is shaped:
//
//   AUAudioUnitFactory   the system asks it for one MU2000
//   AUViewController     its .view is the editor the host shows
//
// The editor is not written here. It is src/vst3/panel_nsview.mm -- the same
// panel the VST3 draws and the same one the AUv2 hands back from its
// AUCocoaUIBase class (src/au/editor_mac.mm). Only the asking differs.
//
// The view and the audio unit arrive in either order (a host may ask for the
// view first), so both paths end in attachPanel and the second one does the
// work. Views are touched on the main thread only:
// createAudioUnitWithComponentDescription: is not promised to run there.

#import "audio_unit.h"

#import <CoreAudioKit/CoreAudioKit.h>
#import <Foundation/Foundation.h>

#include "vst3/engine.h"
#include "vst3/panel_nsview.h"

@interface SMU2000ViewController : AUViewController <AUAudioUnitFactory>
@end

@implementation SMU2000ViewController {
	SMU2000AudioUnit *_au;
	NSView           *_panel;
}

// No nib: the view is made here, and the panel goes inside it once there is an
// audio unit to draw
- (void)loadView
{
	NSView *root = [[NSView alloc]
	    initWithFrame:NSMakeRect(0, 0, smu2000::vst3::kPanelWidth,
	                                   smu2000::vst3::kPanelHeight)];
	root.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	self.view = root;
	self.preferredContentSize = root.frame.size;
	[self attachPanel];
}

- (void)attachPanel
{
	if (_panel || !_au || !self.isViewLoaded)
		return;
	auto *eng = static_cast<smu2000::plug::engine *>([_au enginePointer]);
	if (!eng)
		return;
	NSView *v = smu2000::vst3::make_panel_view(*eng, self.view.bounds.size);
	if (!v)
		return;
	v.frame = self.view.bounds;
	v.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
	[self.view addSubview:v];
	_panel = v;
	// What the panel settled on, which is what the host should open the window
	// at. make_panel_view clamps the way the VST3 view does
	self.preferredContentSize = v.frame.size;
}

// ---- NSExtensionRequestHandling. An AUv3 has nothing to do here
- (void)beginRequestWithExtensionContext:(NSExtensionContext *)context
{
	(void)context;
}

// ---- AUAudioUnitFactory
- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error
{
	_au = [[SMU2000AudioUnit alloc] initWithComponentDescription:desc error:error];
	if (_au) {
		// This is not promised to be the main thread, and the view may already
		// be up (a host that asked for the editor first)
		dispatch_async(dispatch_get_main_queue(), ^{ [self attachPanel]; });
	}
	return _au;
}

@end
