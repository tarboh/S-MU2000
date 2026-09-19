// license:BSD-3-Clause
//
// The Cocoa half of the macOS front end.
//
// This is the only file compiled as Objective-C++, and on purpose it does not
// include compat/gdi.h: Cocoa's headers define BOOL and Quickdraw's define
// Polygon, both of which that header has to declare so panel.cpp can stay
// unchanged. Keeping the two apart is cheaper than renaming GDI.
//
// So this file knows about windows, events, menus and file panels and nothing
// about the synth. Everything it needs from the app it asks for through
// ui::mac_app in window_mac.h.

#include "window_mac.h"

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <string>

// Carbon's virtual key code for F5. Not worth pulling in <Carbon/Carbon.h>
// for one constant; the app is told about function keys through the sentinel
// in window_mac.h
static const unsigned short kKeyCodeF5 = 0x60;

// ---------------------------------------------------------------------------
// The panel itself. A flipped NSView, so the context handed to the app already
// has its origin top-left with y running down -- the same space GDI uses, and
// the space compat/gdi.h's macOS side assumes.

@interface SMUView : NSView
{
@public
	ui::mac_app *_app;
@private
	NSTimer *_timer;
	CGFloat  _scroll_accum;
}
- (instancetype)initWithFrame:(NSRect)frame app:(ui::mac_app *)app;
- (void)tick:(NSTimer *)timer;
- (void)showMenu:(NSEvent *)event;
- (int)codeForEvent:(NSEvent *)event;
@end

@implementation SMUView

- (instancetype)initWithFrame:(NSRect)frame app:(ui::mac_app *)app
{
	self = [super initWithFrame:frame];
	if (self) {
		_app = app;
		_scroll_accum = 0.0;
		// This is the initializer the window actually uses, so the drag types
		// have to be registered here: a file dragged in from the Finder only
		// reaches performDragOperation() if the view agreed to take it
		[self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
	}
	return self;
}

- (void)dealloc
{
	[_timer invalidate];
}

- (BOOL)isFlipped { return YES; }

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }

// ---- dropping a file on the window
//
// The Windows side gets a MIDI file dropped on it through WM_DROPFILES; this is
// the same thing, asked for by registering the file URL type. What to do with
// the path is the app's business (ui::mac_app::file_dropped)

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
	if (!_app)
		return NSDragOperationNone;
	NSPasteboard *pb = [sender draggingPasteboard];
	return [[pb types] containsObject:NSPasteboardTypeFileURL] ? NSDragOperationCopy
	                                                          : NSDragOperationNone;
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
	(void)sender;
	return _app != nil;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
	NSPasteboard *pb = [sender draggingPasteboard];
	NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[ [NSURL class] ]
	                                          options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
	NSURL *url = [urls firstObject];
	if (!url || !_app)
		return NO;
	const char *path = [[url path] UTF8String];
	if (!path)
		return NO;
	_app->file_dropped(std::string(path));
	return YES;
}

- (void)updateTrackingAreas
{
	for (NSTrackingArea *area in [self trackingAreas])
		[self removeTrackingArea:area];

	NSTrackingAreaOptions opts = NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
	                             NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect;
	NSTrackingArea *area = [[NSTrackingArea alloc] initWithRect:NSZeroRect
	                                                   options:opts
	                                                     owner:self
	                                                  userInfo:nil];
	[self addTrackingArea:area];
	[super updateTrackingAreas];
}

- (void)setFrameSize:(NSSize)newSize
{
	[super setFrameSize:newSize];
	if (_app)
		_app->resized((int)newSize.width, (int)newSize.height);
}

- (void)drawRect:(NSRect)dirty
{
	(void)dirty;
	CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
	if (!ctx || !_app)
		return;
	const NSRect b = [self bounds];
	_app->draw((void *)ctx, (int)b.size.width, (int)b.size.height);
}

// ---- repainting
//
// A timer rather than a display link: the panel updates at 30 frames a second
// on Windows and matching that is enough. Common run loop modes so it keeps
// ticking while a menu is open or the window is being resized.

- (void)tick:(NSTimer *)timer
{
	(void)timer;
	[self setNeedsDisplay:YES];
}

- (void)viewDidMoveToWindow
{
	[super viewDidMoveToWindow];
	if ([self window]) {
		if (!_timer) {
			_timer = [NSTimer timerWithTimeInterval:(_app ? _app->frame_ms() : 33) / 1000.0
			                                 target:self
			                               selector:@selector(tick:)
			                               userInfo:nil
			                                repeats:YES];
			[[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
		}
	} else {
		[_timer invalidate];
		_timer = nil;
	}
}

// ---- mouse

- (void)mouseDown:(NSEvent *)event
{
	if (!_app)
		return;
	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	if (_app->mouse_down((int)p.x, (int)p.y, false))
		[self showMenu:event];
}

- (void)rightMouseDown:(NSEvent *)event
{
	if (!_app)
		return;
	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	if (_app->mouse_down((int)p.x, (int)p.y, true))
		[self showMenu:event];
}

- (void)mouseDragged:(NSEvent *)event
{
	if (!_app)
		return;
	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	_app->mouse_drag((int)p.x, (int)p.y);
}

- (void)rightMouseDragged:(NSEvent *)event
{
	[self mouseDragged:event];
}

- (void)mouseUp:(NSEvent *)event
{
	(void)event;
	if (_app)
		_app->mouse_up();
}

- (void)rightMouseUp:(NSEvent *)event
{
	(void)event;
	if (_app)
		_app->mouse_up();
}

- (void)mouseMoved:(NSEvent *)event
{
	if (!_app)
		return;
	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	if (_app->hand_cursor((int)p.x, (int)p.y))
		[[NSCursor pointingHandCursor] set];
	else
		[[NSCursor arrowCursor] set];
}

- (void)scrollWheel:(NSEvent *)event
{
	if (!_app)
		return;

	CGFloat dy = [event scrollingDeltaY];
	// Honour the user's "natural scrolling" setting so the dial turns the way
	// their fingers moved
	if ([event isDirectionInvertedFromDevice])
		dy = -dy;

	int steps = 0;
	if ([event hasPreciseScrollingDeltas]) {
		// A trackpad reports points, not notches, so collect them until a
		// sensible amount has built up before clicking the dial over
		_scroll_accum += dy;
		steps = (int)(_scroll_accum / 10.0);
		_scroll_accum -= steps * 10.0;
	} else {
		steps = (int)dy;
	}
	if (!steps)
		return;

	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	_app->wheel((int)p.x, (int)p.y, steps);
}

// ---- keys

// The app maps keys to panel buttons, so it is given a character where there
// is one and the sentinel from window_mac.h where there is not. F5 is called
// out by name because it is the layout reload and has no character at all.
- (int)codeForEvent:(NSEvent *)event
{
	if ([event keyCode] == kKeyCodeF5)
		return ui::MAC_KEY_FUNCTION_BASE + kKeyCodeF5;

	NSString *chars = [[event charactersIgnoringModifiers] lowercaseString];
	if ([chars length] >= 1) {
		const unichar c = [chars characterAtIndex:0];
		// Printable ASCII, plus Return and Delete, which are two of the panel's
		// buttons. Everything else is an arrow or a function key
		if (c == '\r' || c == 0x7f || c == 0x08 || (c >= 0x20 && c < 0x7f))
			return (int)c;
	}
	return ui::MAC_KEY_FUNCTION_BASE + (int)[event keyCode];
}

- (void)keyDown:(NSEvent *)event
{
	if (!_app) {
		[super keyDown:event];
		return;
	}
	// Holding a key down must not repeat: the panel's buttons latch while held,
	// so autorepeat would read as a stream of presses
	if ([event isARepeat])
		return;
	_app->key([self codeForEvent:event], true);
}

- (void)keyUp:(NSEvent *)event
{
	if (!_app) {
		[super keyUp:event];
		return;
	}
	_app->key([self codeForEvent:event], false);
}

- (void)flagsChanged:(NSEvent *)event
{
	(void)event;      // modifiers are not panel buttons
}

// ---- the popup menus

- (void)menuItemChosen:(id)sender
{
	if (_app)
		_app->menu_chosen((int)[(NSMenuItem *)sender tag]);
}

- (void)showMenu:(NSEvent *)event
{
	if (!_app)
		return;
	NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
	std::vector<ui::menu_group> groups = _app->context_menu((int)p.x, (int)p.y);
	if (groups.empty())
		return;

	NSMenu *menu = [[NSMenu alloc] init];
	[menu setAutoenablesItems:NO];

	for (const ui::menu_group &g : groups) {
		// An untitled group goes straight into the popup; a titled one becomes
		// a submenu, which is how the four MIDI ports are presented
		NSMenu *into = menu;
		if (!g.title.empty()) {
			NSMenuItem *head = [[NSMenuItem alloc] init];
			[head setTitle:[NSString stringWithUTF8String:g.title.c_str()]];
			NSMenu *sub = [[NSMenu alloc] init];
			[sub setAutoenablesItems:NO];
			[head setSubmenu:sub];
			[menu addItem:head];
			into = sub;
		}

		for (const ui::menu_item &item : g.items) {
			if (item.separator) {
				[into addItem:[NSMenuItem separatorItem]];
				continue;
			}
			NSMenuItem *mi = [[NSMenuItem alloc] init];
			NSString *title = [NSString stringWithUTF8String:item.label.c_str()];
			if (!item.shortcut.empty()) {
				NSString *hint = [NSString stringWithUTF8String:item.shortcut.c_str()];
				title = [[title stringByAppendingString:@"（"] stringByAppendingString:hint];
				title = [title stringByAppendingString:@"）"];
			}
			[mi setTitle:title];
			[mi setTag:item.id];
			[mi setTarget:self];
			[mi setAction:@selector(menuItemChosen:)];
			[mi setEnabled:item.enabled];
			[mi setState:item.checked ? NSControlStateValueOn : NSControlStateValueOff];
			[into addItem:mi];
		}
	}

	[NSMenu popUpContextMenu:menu withEvent:event forView:self];
}

@end

// ---------------------------------------------------------------------------
// Window and application

@interface SMUDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
{
@public
	ui::mac_app *_app;
	NSWindow    *_window;
	SMUView     *_view;
	int          _w, _h;
	const char  *_title;
}
@end

@implementation SMUDelegate

- (instancetype)initWithApp:(ui::mac_app *)app title:(const char *)title w:(int)w h:(int)h
{
	self = [super init];
	if (self) {
		_app = app;
		_title = title;
		_w = w;
		_h = h;
	}
	return self;
}

- (void)buildMenuBar
{
	NSMenu *bar = [[NSMenu alloc] init];

	// A normal application menu, so Cmd+Q and Cmd+H do what they always do.
	// A tool that runs without a bundle still has to behave like a Mac app
	NSString *name = [NSString stringWithUTF8String:_title ? _title : "S-MU2000"];
	NSMenu *appMenu = [[NSMenu alloc] init];
	[appMenu addItemWithTitle:[@"About " stringByAppendingString:name]
	                   action:@selector(orderFrontStandardAboutPanel:)
	            keyEquivalent:@""];
	[appMenu addItem:[NSMenuItem separatorItem]];
	[appMenu addItemWithTitle:[@"Hide " stringByAppendingString:name]
	                   action:@selector(hide:)
	            keyEquivalent:@"h"];
	[appMenu addItem:[NSMenuItem separatorItem]];
	[appMenu addItemWithTitle:[@"Quit " stringByAppendingString:name]
	                   action:@selector(terminate:)
	            keyEquivalent:@"q"];

	NSMenuItem *appItem = [[NSMenuItem alloc] init];
	[appItem setTitle:name];
	[appItem setSubmenu:appMenu];
	[bar addItem:appItem];

	[NSApp setMainMenu:bar];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
	(void)note;

	[self buildMenuBar];

	NSRect frame = NSMakeRect(0, 0, _w, _h);
	NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
	                          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

	_window = [[NSWindow alloc] initWithContentRect:frame
	                                      styleMask:style
	                                        backing:NSBackingStoreBuffered
	                                          defer:NO];
	[_window setTitle:[NSString stringWithUTF8String:_title ? _title : "S-MU2000"]];
	[_window setDelegate:self];
	// Keep panel.txt's aspect ratio sensible: below this the labels collide
	[_window setContentMinSize:NSMakeSize(500, 200)];

	_view = [[SMUView alloc] initWithFrame:frame app:_app];
	[_window setContentView:_view];
	[_window makeFirstResponder:_view];
	[_window center];
	[_window makeKeyAndOrderFront:nil];

	[NSApp activateIgnoringOtherApps:YES];
}

- (void)windowWillClose:(NSNotification *)note
{
	(void)note;
	[NSApp terminate:nil];
}

- (void)windowDidResignKey:(NSNotification *)note
{
	(void)note;
	// The synth must not keep a button held because the window lost focus
	if (_app)
		_app->focus_lost();
}

@end

// ---------------------------------------------------------------------------

namespace ui {

std::string open_midi_file_panel()
{
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	[panel setCanChooseFiles:YES];
	[panel setCanChooseDirectories:NO];
	[panel setAllowsMultipleSelection:NO];
	[panel setMessage:@"流す MIDI ファイル"];
	if (@available(macOS 11.0, *)) {
		UTType *mid  = [UTType typeWithFilenameExtension:@"mid"];
		UTType *midi = [UTType typeWithFilenameExtension:@"midi"];
		NSMutableArray *types = [NSMutableArray array];
		if (mid)  [types addObject:mid];
		if (midi) [types addObject:midi];
		if ([types count])
			[panel setAllowedContentTypes:types];
	}

	if ([panel runModal] != NSModalResponseOK)
		return {};

	NSURL *url = [[panel URLs] firstObject];
	if (!url)
		return {};
	const char *path = [[url path] UTF8String];
	return path ? std::string(path) : std::string();
}

std::string open_file_panel(const char *title, const char *ext)
{
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	[panel setCanChooseFiles:YES];
	[panel setCanChooseDirectories:NO];
	[panel setAllowsMultipleSelection:NO];
	if (title && *title)
		[panel setMessage:[NSString stringWithUTF8String:title]];
	if (ext && *ext) {
		// setAllowedContentTypes is macOS 11; asking for the type outside the
		// guard would be calling it on a system that has no such selector
		if (@available(macOS 11.0, *)) {
			UTType *type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:ext]];
			if (type)
				[panel setAllowedContentTypes:@[ type ]];
		}
	}
	if ([panel runModal] != NSModalResponseOK)
		return {};
	NSURL *url = [[panel URLs] firstObject];
	if (!url)
		return {};
	const char *path = [[url path] UTF8String];
	return path ? std::string(path) : std::string();
}

std::string save_file_panel(const char *title, const char *default_name, const char *ext)
{
	NSSavePanel *panel = [NSSavePanel savePanel];
	if (title && *title)
		[panel setMessage:[NSString stringWithUTF8String:title]];
	if (default_name && *default_name)
		[panel setNameFieldStringValue:[NSString stringWithUTF8String:default_name]];
	if (ext && *ext) {
		// macOS 11, same as open_file_panel above
		if (@available(macOS 11.0, *)) {
			UTType *type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:ext]];
			if (type)
				[panel setAllowedContentTypes:@[ type ]];
		}
	}
	if ([panel runModal] != NSModalResponseOK)
		return {};
	NSURL *url = [panel URL];
	if (!url)
		return {};
	const char *path = [[url path] UTF8String];
	return path ? std::string(path) : std::string();
}

bool confirm_modal(const char *title, const char *message, const char *ok_label)
{
	@autoreleasepool {
		NSAlert *alert = [[NSAlert alloc] init];
		[alert setAlertStyle:NSAlertStyleWarning];
		[alert setMessageText:[NSString stringWithUTF8String:title]];
		[alert setInformativeText:[NSString stringWithUTF8String:message]];
		// The accepting button is added second so it is not the default one:
		// Return picks Cancel, and the machine only reboots on a deliberate click
		[alert addButtonWithTitle:@"キャンセル"];
		[alert addButtonWithTitle:[NSString stringWithUTF8String:ok_label]];
		return [alert runModal] == NSAlertSecondButtonReturn;
	}
}

void alert_modal(const char *title, const char *message)
{
	@autoreleasepool {
		NSAlert *alert = [[NSAlert alloc] init];
		[alert setAlertStyle:NSAlertStyleInformational];
		[alert setMessageText:[NSString stringWithUTF8String:title]];
		[alert setInformativeText:[NSString stringWithUTF8String:message]];
		[alert addButtonWithTitle:@"OK"];
		[alert runModal];
	}
}

void run_window(mac_app &app, const char *title, int w, int h)
{
	@autoreleasepool {
		NSApplication *nsapp = [NSApplication sharedApplication];
		// Regular rather than accessory: the panel belongs in the Dock and its
		// window should be able to take focus
		[nsapp setActivationPolicy:NSApplicationActivationPolicyRegular];

		SMUDelegate *delegate = [[SMUDelegate alloc] initWithApp:&app title:title w:w h:h];
		[nsapp setDelegate:delegate];
		[nsapp run];
	}
}

} // namespace ui
