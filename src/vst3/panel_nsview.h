// license:BSD-3-Clause
//
// The machine's front panel inside an NSView, for the macOS plug-in formats.
//
// There is only ever one panel. src/ui/panel.cpp draws it, compat/gdi_mac.cpp
// puts CoreGraphics underneath, and smu2000::vst3::plug_view holds the whole
// thing together and takes the mouse and the keys. What differs between the
// formats is only *how a host asks for a view*:
//
//   VST3   IPlugView::attached(), with the host's own NSView as the parent
//   AUv2   kAudioUnitProperty_CocoaUI names an AUCocoaUIBase class that is
//          asked for a view (src/au/editor_mac.mm)
//   AUv3   the AU answers requestViewController with an NSViewController whose
//          .view is the panel (src/auv3/view_controller.mm)
//
// The last two both want "an NSView holding the panel for this engine", which
// is what this file is. Keeping it in one place is what makes the AUv2 and the
// AUv3 editors literally the same editor rather than two that look alike.
//
// Objective-C++ only, and it must not pull in compat/gdi.h: Cocoa and the
// drawing layer both define BOOL, and Quickdraw defines Polygon. plug_view's
// entry points are all opaque (void *), which is what allows that.

#ifndef S_MU2000_VST3_PANEL_NSVIEW_H
#define S_MU2000_VST3_PANEL_NSVIEW_H

#pragma once

#import <Cocoa/Cocoa.h>

namespace smu2000 {
namespace vst3 {

class engine;

// The panel's own size, and the smallest a host may ask for before it is given
// the panel's size rather than a squeezed one. The same numbers and the same
// clamping the VST3 view applies in onSize() / checkSizeConstraint()
constexpr int kPanelWidth  = 1000;
constexpr int kPanelHeight = 400;
constexpr int kPanelMinW   = 640;
constexpr int kPanelMinH   = 180;

// One editor for one open instance, or nil if it cannot be built. Every call
// makes a new view, which is what both AUv2's factory and AUv3's view
// controller need. `preferred` smaller than the minimum above means "you
// choose", and the panel's own size is used -- NSZeroSize says so plainly.
//
// `owner` is whatever owns `eng`, and the view holds a strong reference to it
// for as long as it lives. A host is free to let go of the plug-in before the
// view it was handed -- auval does -- and the panel's own teardown reaches back
// into the engine (plug_view's destructor writes the card back through it), so
// without this the view would lock a mutex on freed memory and take the process
// with it. The AUv3 passes its AUAudioUnit; the AUv2's engine outlives the view
// on its own, so it passes nil.
//
// The returned view owns the plug_view inside it and lets it go when it is
// deallocated, so a caller only has to hold the NSView.
NSView *make_panel_view(engine &eng, NSSize preferred, id owner);

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_PANEL_NSVIEW_H
