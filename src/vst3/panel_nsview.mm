// license:BSD-3-Clause
//
// The panel in an NSView. See panel_nsview.h for why this is its own file.
//
// There is no second copy of the panel in here: what is built is the *same*
// view the VST3 build shows -- smu2000::vst3::plug_view, which is
// src/vst3/view.cpp drawing through compat/gdi_mac.cpp, inside the NSView from
// src/vst3/view_mac.mm. Only the way a host asks for it differs, which is the
// whole reason the three formats can share one engine and one editor.

#import "panel_nsview.h"

#include "engine.h"
#include "plug_window.h"
#include "view.h"

// The frame a host is handed. It is only that: the panel, the child view that
// paints it and the input handling all belong to the plug_view inside, which
// this holds for as long as it lives
@interface SMU2000PanelView : NSView
{
@public
	smu2000::vst3::plug_view *_plug;
}
@end

@implementation SMU2000PanelView

- (BOOL)isFlipped { return YES; }

- (void)dealloc
{
	// plug_view counts its own references (it implements FUnknown's addRef /
	// release); make_panel_view took one, and this gives it back
	if (_plug)
		_plug->release();
}

// A host that lets the editor window be resized moves this view's frame. The
// panel and the child view inside have to follow it, which is plug_view::onSize
- (void)setFrameSize:(NSSize)size
{
	[super setFrameSize:size];
	if (!_plug)
		return;
	Steinberg::ViewRect r(0, 0, (Steinberg::int32)size.width, (Steinberg::int32)size.height);
	_plug->onSize(&r);
}

@end


namespace smu2000 {
namespace vst3 {

NSView *make_panel_view(engine &eng, NSSize preferred)
{
	plug_view *plug = new plug_view(eng);

	int w = (int)preferred.width;
	int h = (int)preferred.height;
	if (w < kPanelMinW || h < kPanelMinH) {
		w = kPanelWidth;
		h = kPanelHeight;
	}
	// onSize clamps the way the VST3 host's size is clamped, and resizes the
	// panel to match, so the frame below is what the panel was laid out for
	Steinberg::ViewRect r(0, 0, w, h);
	plug->onSize(&r);
	w = plug->width();
	h = plug->height();

	SMU2000PanelView *view = [[SMU2000PanelView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
	// attached() answers a VST3 tresult, where kResultOk is 0 -- so this is a
	// comparison and not a truth test, or a view that attached perfectly would
	// be thrown away
	if (plug->attached((__bridge void *)view, plug_window_type()) != Steinberg::kResultOk) {
		plug->release();
		return nil;
	}
	view->_plug = plug;
	return view;
}

} // namespace vst3
} // namespace smu2000
