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
//   AUv3   the extension's principal class is an AUViewController whose
//          .view is the panel (src/auv3/factory.mm)
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
constexpr int kPanelWidth  = 1400;
constexpr int kPanelHeight = 360;
constexpr int kPanelMinW   = 640;
constexpr int kPanelMinH   = 180;

// One editor for one open instance, or nil if it cannot be built. Every call
// makes a new view, which is what both AUv2's factory and AUv3's view
// controller need. `preferred` smaller than the minimum above means "you
// choose", and the panel's own size is used.
//
// The returned view owns the plug_view inside it and lets it go when it is
// deallocated, so a caller only has to hold the NSView.
NSView *make_panel_view(engine &eng, NSSize preferred);

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_PANEL_NSVIEW_H
