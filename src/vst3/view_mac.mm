// license:BSD-3-Clause
//
// The VST3 view's window on macOS: an NSView added to whatever view the host
// hands over in IPlugView::attached(). The VST3 interface, the panel and the
// input semantics all live in view.cpp; this is only the window.
//

#include "plug_window.h"
#include "view.h"

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "ui/fx_editor.h"
#include "ui/keymap.h"
#include "ui/master_editor.h"
#include "ui/menu.h"
#include "ui/overview.h"
#include "ui/part_shapes.h"
#include "ui/pc_editor.h"
#include "ui/pc_host.h"
#include "ui/pc_window.h"
#include "ui/xg_ui.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

using namespace Steinberg;

namespace smu2000 {
namespace vst3 {

const char *plug_window_type() { return kPlatformTypeNSView; }

namespace {

// plug_key_of()'s counterpart for this platform. A character where the key has
// one, which is what the GUI front end maps too, so the same physical key is
// the same panel button in both programs. The panel letters share their
// meaning with the GUI front end through ui/keymap.h; only the function keys
// (private-use characters) are listed here.
plug_key plug_key_of_char(int c)
{
	// The same keys gui.exe uses, as their private-use characters
	// (NSF3FunctionKey / NSF2FunctionKey / NSF4FunctionKey)
	switch (c) {
	case 0xf706: return PLUG_KEY_LIST;
	case 0xf705: return PLUG_KEY_EDITOR;
	case 0xf707: return PLUG_KEY_ENGINE;
	default: break;
	}
	mu2000::button b = mu2000::button::count;
	if (!ui::button_for_char(c, b))
		return PLUG_KEY_NONE;
	return plug_key_of_button(int(b));
}

} // namespace
} // namespace vst3
} // namespace smu2000

// The panel's view is declared at global scope on purpose: clang accepts an
// Objective-C class declared inside a namespace, but its ivars stop resolving
// there, and every method of this one touches them. The name the methods need
// is pulled in by hand, since unqualified lookup from here cannot see into
// the namespaces above
using smu2000::vst3::plug_view;
using smu2000::vst3::plug_key;
using smu2000::vst3::plug_key_of_char;
using smu2000::vst3::PLUG_KEY_NONE;
using smu2000::vst3::PC_LIST;
using smu2000::vst3::PC_EDITOR;

// mac_window, defined below: the card menu's choices open its PC windows
namespace smu2000 { namespace vst3 { class mac_window; } }

// The panel's view. Flipped, so the CGContext AppKit hands to drawRect already
// has its origin top-left with y running down -- the space compat/gdi.h assumes
// and the space the panel's hit testing is written in.
@interface SMUPlugView : NSView
{
@public
	plug_view *_owner;
@private
	NSTimer *_timer;
	int _clicks;
	int _moves;
	int _ticks;
}
- (instancetype)initWithOwner:(plug_view *)owner width:(int)w height:(int)h;
- (void)tick:(NSTimer *)timer;
- (void)ensureTimer;
- (plug_key)plugKeyForEvent:(NSEvent *)event;
@end

@implementation SMUPlugView

- (instancetype)initWithOwner:(plug_view *)owner width:(int)w height:(int)h
{
	self = [super initWithFrame:NSMakeRect(0, 0, w, h)];
	if (self)
		_owner = owner;
	return self;
}

- (void)dealloc
{
	[_timer invalidate];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

// A host that keeps a plug-in's editor in a panel which does not take key focus
// (Waveform does) makes every click into it a "first mouse" click, and AppKit
// hands that click to the window to activate rather than to the view -- so the
// panel draws and animates but no button ever fires. Saying yes here is what
// lets the click through as well as activating the window
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
	(void)event;
	return YES;
}

- (void)drawRect:(NSRect)dirty
{
	(void)dirty;
	if (!_owner)
		return;
	// The timer normally starts in viewDidMoveToWindow, but some hosts move
	// the view around in ways that leave it windowless there and never move
	// it again: with no timer the panel paints once and freezes. Drawing
	// always runs on the main thread with a window in place, so a missing
	// timer is remade here instead of staying missing
	if (!_timer && [self window])
		[self ensureTimer];
	CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
	if (!ctx)
		return;
	const NSRect b = [self bounds];
	_owner->repaint((void *)ctx, (int)b.size.width, (int)b.size.height);
}

// Repaint at the same 30 frames a second the Win32 window uses, so the two
// panels animate alike. Common modes so it keeps ticking during a live resize
- (void)tick:(NSTimer *)timer
{
	(void)timer;
	if (_ticks < 3) {
		_ticks++;
		if (_owner && _ticks == 1)
			_owner->log_line("panel timer: first tick");
	}
	[self setNeedsDisplay:YES];
}

// Start the repaint timer unless one already runs. Safe to call twice
- (void)ensureTimer
{
	if (_timer)
		return;
	_timer = [NSTimer timerWithTimeInterval:1.0 / 30.0
	                                 target:self
	                               selector:@selector(tick:)
	                               userInfo:nil
	                                repeats:YES];
	[[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)viewDidMoveToWindow
{
	[super viewDidMoveToWindow];
	NSWindow *win = [self window];
	// The key notification is observed per window, and the window is only known
	// here: a plug-in view is built before the host has put it anywhere, so
	// asking for [self window] while attaching answers nil -- and nil there means
	// "every window", which is not the question being asked
	[[NSNotificationCenter defaultCenter] removeObserver:self
	                                                name:NSWindowDidResignKeyNotification
	                                              object:nil];
	if (win) {
		if (_moves < 4) {
			_moves++;
			if (_owner) {
				char b[96];
				std::snprintf(b, sizeof(b), "panel timer: moved to window (%s timer)",
				              _timer ? "keeping" : "starting");
				_owner->log_line(b);
			}
		}
		[self ensureTimer];
		[[NSNotificationCenter defaultCenter] addObserver:self
		                                         selector:@selector(resignKeyWindow:)
		                                             name:NSWindowDidResignKeyNotification
		                                           object:win];

		// Keyboard focus is asked for here and not in attached(), because this is
		// the first moment the window is known: a view is built before the host
		// has put it anywhere, so asking [self window] there answers nil and the
		// ask goes nowhere. Only when the window itself owns the focus -- a host
		// that keeps another control in the same window keeps it
		id first = [win firstResponder];
		if (!first || first == win)
			[win makeFirstResponder:self];
	} else {
		if (_moves < 4) {
			_moves++;
			if (_owner)
				_owner->log_line("panel timer: moved out of window (timer stopped)");
		}
		[_timer invalidate];
		_timer = nil;
	}
}

// Window notifications arrive as a message to the observer, and the selector is
// named above. **NSView has no -resignKeyWindow** (NSWindow does), so without
// this the notification would send an unrecognised selector and take the host
// down with it the first time the editor window lost focus
- (void)resignKeyWindow:(NSNotification *)note
{
	(void)note;
	if (_owner)
		_owner->focus_lost();
}

// ---- mouse

// What a host did with a click is not visible from outside the view: the panel
// draws and animates either way, so "it is on screen, updating, and answering
// nothing" can only be told apart by writing down what arrived. The first few
// clicks go to the log with the three answers that name the cases:
//
//   key no       the window never takes key focus, so AppKit spends the click
//                activating it and the view is never asked -- this is the one
//                -acceptsFirstMouse fixes, and the one that leaves no line here
//                at all when the host is stubborn
//   main no      the click came in on a thread that must not touch views
//   hit other    something the host put on top took the click first
- (void)noteClick:(NSEvent *)event
{
	if (!_owner || _clicks >= 8)
		return;
	_clicks++;

	NSWindow *win = [self window];
	NSView *hit = win ? [[win contentView] hitTest:[event locationInWindow]] : nil;
	char b[200];
	std::snprintf(b, sizeof(b), "クリック %d 回目: 窓 %s、key %s、主の糸 %s、当たった先 %s",
	              _clicks, win ? "あり" : "なし",
	              (win && [win isKeyWindow]) ? "yes" : "no",
	              [NSThread isMainThread] ? "yes" : "no",
	              hit ? NSStringFromClass([hit class]).UTF8String : "なし");
	_owner->log_line(b);
}

- (void)mouseDown:(NSEvent *)event
{
	if (!_owner)
		return;
	[self noteClick:event];
	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	_owner->mouse_down((int)p.x, (int)p.y);
	[self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)event
{
	if (!_owner)
		return;
	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	_owner->mouse_drag((int)p.x, (int)p.y);
	[self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)event
{
	(void)event;
	if (!_owner)
		return;
	_owner->mouse_up();
	[self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent *)event
{
	if (!_owner)
		return;

	CGFloat dy = [event scrollingDeltaY];
	if ([event isDirectionInvertedFromDevice])
		dy = -dy;

	int steps = [event hasPreciseScrollingDeltas] ? (int)(dy / 10.0) : (int)dy;
	if (!steps)
		return;

	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	_owner->wheel((int)p.x, (int)p.y, steps);
	[self setNeedsDisplay:YES];
}

// ---- keys

// A plug_key for the key, or PLUG_KEY_NONE. The panel letters share their
// meaning with the GUI front end (ui/keymap.h); F3/F2/F4 open the PC
// windows / toggle the native engine, the way gui.exe does
- (plug_key)plugKeyForEvent:(NSEvent *)event
{
	NSString *chars = [[event charactersIgnoringModifiers] lowercaseString];
	if ([chars length] < 1)
		return PLUG_KEY_NONE;
	return plug_key_of_char((int)[chars characterAtIndex:0]);
}

- (void)keyDown:(NSEvent *)event
{
	if (!_owner) {
		[super keyDown:event];
		return;
	}
	// Buttons latch while held, so autorepeat would read as a stream of presses
	if ([event isARepeat])
		return;
	const plug_key k = [self plugKeyForEvent:event];
	if (k != PLUG_KEY_NONE)
		_owner->key(k, true);
}

- (void)keyUp:(NSEvent *)event
{
	if (!_owner)
		return;
	const plug_key k = [self plugKeyForEvent:event];
	if (k != PLUG_KEY_NONE)
		_owner->key(k, false);
}

- (void)rightMouseDown:(NSEvent *)event
{
	if (!_owner)
		return;
	[self noteClick:event];
	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	_owner->mouse_right((int)p.x, (int)p.y);
}

@end


// The card menu's target. NSMenu sends each choice to one object, and the size
// items are told apart by their tag; this object turns that into a call on the
// view (the same four jobs view.cpp's card_* methods do for either platform)
@interface SMUCardMenu : NSObject
{
@public
	plug_view *_owner;
	smu2000::vst3::mac_window *_win;
}
- (void)choose:(id)sender;
@end


namespace smu2000 {
namespace vst3 {

class mac_window : public plug_window
{
public:
	explicit mac_window(plug_view &owner) : m_owner(owner) {}
	~mac_window() override { detach(); }

	bool attach(void *parent, int w, int h) override;
	void detach() override;
	void set_size(int w, int h) override;
	void card_menu(int x, int y) override;
	void panel_menu(int x, int y) override;
	void alert(const std::string &text) override;
	void pc_frame(::xg::model &m, const ::ui::xg_snapshot &ram, ::ui::bridge &br) override;

	// Open a PC window (overview/editor), showing an alert when it fails
	void open_pc(ui::pc_window &w);
	void open_pc_window(int kind) override
	{
		open_pc(*pc_window_for_kind(kind, m_list, m_editor, m_fx, m_shapes, m_master));
	}

private:
	plug_view &m_owner;
	SMUPlugView *m_view = nil;
	// The PC windows gui.exe shows (overview, editor, insertion, part voice,
	// master). Same content as on Windows; only the hosting window differs
	ui::pc_window m_list{ std::make_unique<ui::overview>() };
	ui::pc_window m_editor{ std::make_unique<ui::pc_editor>() };
	ui::pc_window m_fx{ std::make_unique<ui::fx_editor>() };
	ui::pc_window m_shapes{ std::make_unique<ui::part_shapes>() };
	ui::pc_window m_master{ std::make_unique<ui::master_editor>() };
};

// Objective-C lives at global scope (see the note on SMUPlugView above);
// the mac_window methods resume inside the namespaces below
} // namespace vst3
} // namespace smu2000

@implementation SMUCardMenu

- (void)choose:(id)sender
{
	if (!_owner)
		return;
	const int tag = (int)[sender tag];

	if (tag >= ui::ID_PLUG_CARD_NEW16 && tag <= ui::ID_PLUG_CARD_NEW128) {
		NSSavePanel *panel = [NSSavePanel savePanel];
		[panel setTitle:[NSString stringWithUTF8String:UI_TEXT(dlg_card_save, "Where to save the new SmartMedia image")]];
		[panel setNameFieldStringValue:@"smartmedia.img"];
		UTType *img = [UTType typeWithFilenameExtension:@"img"];
		if (img)
			[panel setAllowedContentTypes:@[ img ]];
		if ([panel runModal] != NSModalResponseOK)
			return;
		// 16 / 32 / 64 / 128 MB, in the shared ID order
		_owner->card_make(std::string([[[panel URL] path] UTF8String]),
		                  16 << (tag - ui::ID_PLUG_CARD_NEW16));
		return;
	}

	if (tag == ui::ID_PLUG_CARD_OPEN) {                          // 差す
		NSOpenPanel *panel = [NSOpenPanel openPanel];
		[panel setTitle:[NSString stringWithUTF8String:UI_TEXT(dlg_card_open, "Insert a SmartMedia image")]];
		[panel setCanChooseFiles:YES];
		[panel setCanChooseDirectories:NO];
		[panel setAllowsMultipleSelection:NO];
		if ([panel runModal] != NSModalResponseOK)
			return;
		_owner->card_insert_path(std::string([[[panel URL] path] UTF8String]));
		return;
	}

	if (tag == ui::ID_PLUG_CARD_EJECT)                           // 抜く
		_owner->card_eject();
	else if (tag == ui::ID_PLUG_LIST && _win)                      // 一覧
		_win->open_pc_window(PC_LIST);
	else if (tag == ui::ID_PLUG_EDITOR && _win)                    // エディタ
		_win->open_pc_window(PC_EDITOR);
}

@end

namespace smu2000 {
namespace vst3 {

void mac_window::alert(const std::string &text)
{
	NSAlert *a = [[NSAlert alloc] init];
	[a setMessageText:@"S-MU2000"];
	[a setInformativeText:[NSString stringWithUTF8String:text.c_str()]];
	[a addButtonWithTitle:@"OK"];
	[a runModal];
}

// NSMenu rendering for the shared ui/menu.h content. A titled group becomes
// a submenu; the standalone window (ui/window_mac.mm) renders them the same
// way from the same structs
NSMenu *plug_menu(const std::vector<ui::menu_group> &groups, SMUCardMenu *target)
{
	NSMenu *m = [[NSMenu alloc] init];
	[m setAutoenablesItems:NO];
	for (const ui::menu_group &g : groups) {
		NSMenu *into = m;
		if (!g.title.empty()) {
			NSMenuItem *head = [[NSMenuItem alloc] init];
			[head setTitle:[NSString stringWithUTF8String:g.title.c_str()]];
			NSMenu *sub = [[NSMenu alloc] init];
			[sub setAutoenablesItems:NO];
			[head setSubmenu:sub];
			[m addItem:head];
			into = sub;
		}
		for (const ui::menu_item &item : g.items) {
			if (item.separator) {
				[into addItem:[NSMenuItem separatorItem]];
				continue;
			}
			NSMenuItem *mi = [[NSMenuItem alloc] init];
			[mi setTitle:[NSString stringWithUTF8String:item.label.c_str()]];
			[mi setTag:item.id];
			[mi setTarget:target];
			[mi setAction:@selector(choose:)];
			[mi setEnabled:item.enabled];
			[mi setState:item.checked ? NSControlStateValueOn : NSControlStateValueOff];
			[into addItem:mi];
		}
	}
	return m;
}

// The card slot's menu, offered as a native popup. A card menu needs a target
// to receive the choice, so one is made per call and released as the menu goes
void mac_window::card_menu(int x, int y)
{
	if (!m_view)
		return;

	SMUCardMenu *target = [[SMUCardMenu alloc] init];
	target->_owner = &m_owner;
	target->_win = this;

	ui::plug_menu_state s{ m_owner.card_path(), m_owner.card_ready() };
	NSMenu *m = plug_menu(ui::menu_plug_card(s), target);

	// In the view's own coordinates. The view is flipped, which is the space the
	// panel's hit testing already worked in
	[m popUpMenuPositioningItem:nil atLocation:NSMakePoint(x, y) inView:m_view];
}

void mac_window::panel_menu(int x, int y)
{
	if (!m_view)
		return;

	SMUCardMenu *target = [[SMUCardMenu alloc] init];
	target->_owner = &m_owner;
	target->_win = this;

	NSMenu *m = plug_menu(ui::menu_plug_panel(), target);

	[m popUpMenuPositioningItem:nil atLocation:NSMakePoint(x, y) inView:m_view];
}

bool mac_window::attach(void *parent, int w, int h)
{
	if (m_view || !parent)
		return false;

	// The host hands over its view as a bare pointer; on this platform that is
	// NSView (kPlatformTypeNSView). Never nil in practice, but a bad pointer
	// here would fault rather than fail, so check the obvious
	NSView *host = (__bridge NSView *)parent;
	if (!host || ![host isKindOfClass:[NSView class]])
		return false;

	m_view = [[SMUPlugView alloc] initWithOwner:&m_owner width:w height:h];
	if (!m_view)
		return false;

	// Subview, not the content view: the host owns the window and may put other
	// things around us
	[host addSubview:m_view];
	[m_view setFrame:NSMakeRect(0, 0, w, h)];
	// First responder is asked for from viewDidMoveToWindow: the window is not
	// known here yet (this runs before the host has shown the view), and asking
	// then answers nil. Waiting until the view is somewhere can tell a host's own
	// window from ours, which is what keeps the editor from pulling the keyboard
	// away from whatever the host holds focus with

	// Losing key focus must not leave a panel button held down. The observer is
	// registered by the view itself in viewDidMoveToWindow, which is where the
	// window it belongs to is known
	return true;
}

void mac_window::detach()
{
	// The hosted PC windows go with the panel: left open they would keep
	// drawing from an engine that is being torn down
	m_list.hide();
	m_editor.hide();
	m_fx.hide();
	m_shapes.hide();
	m_master.hide();
	if (m_view) {
		[[NSNotificationCenter defaultCenter] removeObserver:m_view];
		[m_view removeFromSuperview];
		m_view->_owner = nullptr;
		m_view = nil;
	}
}

void mac_window::set_size(int w, int h)
{
	if (m_view)
		[m_view setFrame:NSMakeRect(0, 0, w, h)];
}

void mac_window::open_pc(ui::pc_window &w)
{
	std::string err;
	if (!w.show(err))
		alert(err.empty() ? std::string("the window cannot be opened") : err);
}

// Driven at the panel's repaint rate. Hidden windows cost nothing
void mac_window::pc_frame(::xg::model &m, const ::ui::xg_snapshot &ram, ::ui::bridge &br)
{
	ui::pc_frame_all(m_list, m_editor, m_fx, m_shapes, m_master, m, ram, br,
	                 [this](ui::pc_window &w) { open_pc(w); });
}


plug_window *plug_window_create(plug_view &owner)
{
	return new mac_window(owner);
}

} // namespace vst3
} // namespace smu2000
