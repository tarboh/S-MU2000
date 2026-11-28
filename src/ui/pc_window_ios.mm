// license:BSD-3-Clause
//
// pc_window for iOS: the XG editor host.
//
// The third of the per-platform twins (pc_window.cpp for Win32/D3D11,
// pc_window_mac.mm for AppKit/Metal, this for UIKit/Metal). Each editor gets a
// UIViewController presented full-screen over the panel, with its own Metal layer,
// its own ImGui context and its own CJK font - one context per window, as on macOS,
// because ImGui attaches to the current one.
//
// Rendering rides the main 30 Hz tick, not a second display link: ui::app's
// frame_work() calls pc_window::frame() on all five editors every frame (the same
// arrangement as the mac timer), and frame() draws only while its view
// controller is on screen. Input is per-view: each editor view feeds touches into
// its own context's io, so two windows never fight over one pointer. The editor
// views ARE ImGui widgets (unlike the panel's raw draw list), so io.MouseDown is
// the whole of it - no app verbs.
//
// A fullscreen modal has no close box, so each editor sits in a navigation
// controller whose Done button hides it. That is the only UIKit chrome in the
// file; everything else is the Metal/ImGui shell both twins share.
//
// Keyboard: the view is UIKeyInput (software keyboard follows WantTextInput,
// hardware keys feed ImGui key events, clipboard bridges UIPasteboard) - see
// the keyboard section of PCEditView and src/ios/keymap_ios.h.
 //
// Not yet: file drops (no UIDropInteraction yet - set_drop_handler is accepted
// and ignored, as on the panel).

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CADisplayLink.h>
#import <UIKit/UIKit.h>

#include <string>

#include "ui/pc_window.h"

#include "ui/keymap_ios.h"
#include "ui/font_file.h"
#include "ui/imgui_shell.h"
#include "ui/xg_ui.h"

// The per-editor view: +layerClass Metal, own touches into its own context. Same
// shape as SMUView in window_ios.mm, minus the display link (frame() drives) and
// the app pointer (editors talk to the bridge through draw(), not verbs).
//
// UIKeyInput, so the software keyboard has a first responder to show for: the
// view becomes first responder exactly while io.WantTextInput holds (frame()
// below syncs it every tick) and resigns when the field deactivates. Software
// text arrives through insertText:; hardware keys arrive as raw presses (a
// bare UIKeyInput gets no insertText: synthesis - that is full UITextInput
// only), so presses feed key events plus printable characters when a field is
// active. The traits are set in init: hex, SysEx and voice names must not
// meet autocorrect, and the keyboard stays Default (not ASCII) so CJK names
// can be typed.
@interface PCEditView : UIView <UIKeyInput>
{
@public
	ImGuiContext *ctx;
}
@property (nonatomic, readwrite) UITextAutocorrectionType autocorrectionType;
@property (nonatomic, readwrite) UITextAutocapitalizationType autocapitalizationType;
@property (nonatomic, readwrite) UITextSpellCheckingType spellCheckingType;
@property (nonatomic, readwrite) UIKeyboardType keyboardType;
@property (nonatomic, readwrite) UIReturnKeyType returnKeyType;
@end

@implementation PCEditView

+ (Class)layerClass
{
	return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame
{
	self = [super initWithFrame:frame];
	if (self) {
		self.multipleTouchEnabled = YES;
		self.backgroundColor = UIColor.blackColor;
		self.autocorrectionType = UITextAutocorrectionTypeNo;
		self.autocapitalizationType = UITextAutocapitalizationTypeNone;
		self.spellCheckingType = UITextSpellCheckingTypeNo;
		self.keyboardType = UIKeyboardTypeDefault;
		self.returnKeyType = UIReturnKeyDefault;
		// Right-click is a long-press here: the editors have no second-finger
		// chord (the panel does) and no Ctrl key, but rows open pickers on
		// right-click (BeginPopupContextItem). Movement cancels it, so drags
		// and slider gestures never right-click by accident.
		UILongPressGestureRecognizer *hold =
		    [[UILongPressGestureRecognizer alloc] initWithTarget:self
		                                                   action:@selector(longPressed:)];
		hold.minimumPressDuration = 0.5;
		hold.allowableMovement = 12;
		// The touch sequence is managed below by hand (button 0 released,
		// button 1 pressed); cancelling the touches would fight that.
		hold.cancelsTouchesInView = NO;
		[self addGestureRecognizer:hold];
	}
	return self;
}

// A held finger becomes the right button for exactly the hold: button 0 goes
// up first (a right-click is not also a left-press), button 1 goes down for
// as long as the finger stays, then up. ImGui's context-item popups fire on
// the down edge, so even a hold-and-release opens the picker.
- (void)longPressed:(UILongPressGestureRecognizer *)recognizer
{
	if (!self->ctx)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGuiIO &io = ImGui::GetIO();
	if (recognizer.state == UIGestureRecognizerStateBegan) {
		const CGPoint p = [recognizer locationInView:self];
		io.MousePos = ImVec2((float)p.x, (float)p.y);
		io.MouseDown[0] = false;
		io.MouseDown[1] = true;
	} else if (recognizer.state == UIGestureRecognizerStateEnded ||
	           recognizer.state == UIGestureRecognizerStateCancelled ||
	           recognizer.state == UIGestureRecognizerStateFailed) {
		io.MouseDown[1] = false;
	}
}

- (void)pushTouch:(UITouch *)t down:(BOOL)down
{
	if (!self->ctx)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGuiIO &io = ImGui::GetIO();
	const CGPoint p = [t locationInView:self];
	io.MousePos = ImVec2((float)p.x, (float)p.y);
	io.MouseDown[0] = down ? YES : NO;
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	for (UITouch *t in touches)
		[self pushTouch:t down:YES];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	for (UITouch *t in touches)
		[self pushTouch:t down:YES];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)event;
	for (UITouch *t in touches)
		[self pushTouch:t down:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{
	(void)touches;
	(void)event;
	if (!self->ctx)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGui::GetIO().MouseDown[0] = false;
}

// ---- keyboard ---------------------------------------------------------------
//
// First responder only while a text field is active (frame() syncs it): that
// is what shows and hides the software keyboard. Hardware keys arrive as
// presses whether or not the software keyboard is up, and feed key events;
// printable text never comes through here (insertText: below owns it).

- (BOOL)canBecomeFirstResponder
{
	return YES;
}

- (BOOL)hasText
{
	// The text lives in ImGui, not in this view; nothing here deletes or
	// selects. NO keeps the software keyboard's delete key honest about there
	// being nothing to delete in the view itself - deleteBackward below still
	// fires and reaches the ImGui field.
	return NO;
}

- (void)insertText:(NSString *)text
{
	if (!self->ctx)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGuiIO &io = ImGui::GetIO();
	// The mac twin gates text on WantTextInput for the same reason: a held key
	// playing notes repeats text, and ImGui trickles text against mouse moves,
	// so an ungated feed lags drags further and further behind.
	if (!io.WantTextInput)
		return;
	for (NSUInteger i = 0; i < [text length]; i++) {
		const unichar c = [text characterAtIndex:i];
		// The return key arrives here as newline. The character feeds
		// multiline fields; the key event feeds EnterReturnsTrue singles -
		// the same both-ways shape as the SDL backend.
		if (c == '\n' || c == '\r') {
			io.AddKeyEvent(ImGuiKey_Enter, true);
			io.AddKeyEvent(ImGuiKey_Enter, false);
		}
		io.AddInputCharacterUTF16(c);
	}
}

- (void)deleteBackward
{
	if (!self->ctx)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGuiIO &io = ImGui::GetIO();
	if (!io.WantTextInput)
		return;
	io.AddKeyEvent(ImGuiKey_Backspace, true);
	io.AddKeyEvent(ImGuiKey_Backspace, false);
}

// Paste bridges into insertText: (which gates and feeds), because the pasted
// text belongs to the ImGui field, not to this view. Copy, cut and select stay
// off: ImGui owns its selection and its own Cmd+C/X/V through the clipboard
// functions wired in create() below.
- (void)paste:(id)sender
{
	(void)sender;
	NSString *s = [UIPasteboard generalPasteboard].string;
	if (s)
		[self insertText:s];
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender
{
	(void)sender;
	if (action == @selector(paste:))
		return [UIPasteboard generalPasteboard].string != nil;
	return NO;
}

// One press to key events. Never characters: printable keys already arrived
// through insertText: above (software and Smart Keyboard alike), and feeding
// them here too doubles text. Modifiers feed every time so Ctrl+C/V/X/A and
// Shift+arrows read in ImGui exactly as on desktop.
- (void)pushPress:(UIPress *)press down:(BOOL)down
{
	if (!self->ctx || !press.key)
		return;
	ImGui::SetCurrentContext(self->ctx);
	ImGuiIO &io = ImGui::GetIO();
	feed_key_modifiers(io, press.key.modifierFlags);
	const ImGuiKey key = imgui_key_from_hid(press.key.keyCode);
	if (key != ImGuiKey_None)
		io.AddKeyEvent(key, down ? true : false);
	// Printable text, hardware keyboards only. A bare UIKeyInput responder
	// gets raw presses and no insertText: from hardware keys (the system
	// synthesizes text solely for full UITextInput, and for the software
	// keyboard) - so without this, hardware typing lands nowhere while
	// shortcuts and editing keys work. The software keyboard never reaches
	// here, so nothing doubles. Return/newline stays a key event only (the
	// filter drops \r anyway); the character feeds everything else, shifted
	// case included, exactly like the mac twin's keyDown.
	if (down && io.WantTextInput && press.key.characters) {
		NSString *chars = press.key.characters;
		for (NSUInteger i = 0; i < [chars length]; i++) {
			const unichar c = [chars characterAtIndex:i];
			if (c == '\r' || c == '\n')
				continue;
			io.AddInputCharacterUTF16(c);
		}
	}
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{
	BOOL handled = NO;
	for (UIPress *p in presses) {
		// Any key press, mapped or not: pushPress feeds modifiers every time
		// and characters when there are printable ones, so an unmapped
		// punctuation key still types. Nil-key presses (volume buttons and
		// friends) fall through to super and stay the system's.
		if (p.key) {
			[self pushPress:p down:YES];
			handled = YES;
		}
	}
	// iOS hardware keys do not autorepeat pressesBegan, so unlike the mac
	// twin no repeat guard is needed: one began is one press.
	if (!handled)
		[super pressesBegan:presses withEvent:event];
}

- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{
	BOOL handled = NO;
	for (UIPress *p in presses) {
		if (p.key) {
			[self pushPress:p down:NO];
			handled = YES;
		}
	}
	if (!handled)
		[super pressesEnded:presses withEvent:event];
}

- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event
{
	// A key held across an interruption must come up: a latched ImGui key
	// (Shift held for a range select) would otherwise stick until pressed again.
	for (UIPress *p in presses)
		[self pushPress:p down:NO];
	(void)event;
}

@end

// A view controller per editor: holds the Metal view, carries the Done button's
// navigation item, and tells the pc_window when it goes away so visible() tracks
// dismissal (including the swipe that full-screen does not have - belt and
// braces for presentation styles to come).
@interface PCEditController : UIViewController
@end

@implementation PCEditController
- (void)viewDidLoad
{
	[super viewDidLoad];
	// Plain titled button, not the system Done item: on current iOS the system
	// item renders as a blue circle-checkmark, which reads as decoration rather
	// than "close this". Text that says Done needs no interpretation.
	UIBarButtonItem *done = [[UIBarButtonItem alloc] initWithTitle:@"Done"
	                                                         style:UIBarButtonItemStyleDone
	                                                        target:self
	                                                        action:@selector(done:)];
	self.navigationItem.leftBarButtonItem = done;
}

- (void)done:(id)sender
{
	(void)sender;
	[self dismissViewControllerAnimated:YES completion:nil];
}
@end

namespace ui {

// The Objective-C side, held in a C++ struct the way pc_window_mac.mm holds its
// `host`. m_ns is a void* because pc_window.h is a C++ header and must not mention
// UIKit; under ARC the pointers below are managed.
namespace {

struct host {
	UIViewController *vc = nil;   // the navigation controller presented
	PCEditView *view = nil;
	CAMetalLayer *layer = nil;
	id<MTLDevice> dev = nil;
	id<MTLCommandQueue> queue = nil;
};

NSString *title_of_view(const imgui_view &view)
{
	// wchar_t is UTF-32 on Apple, so the title crosses as UTF-32LE bytes.
	const wchar_t *w = view.title();
	if (!w)
		return @"Editor";
	return [[NSString alloc] initWithBytes:w
	                                length:std::wcslen(w) * sizeof(wchar_t)
	                              encoding:NSUTF32LittleEndianStringEncoding];
}

// The view controller to present from: the topmost of the foreground scene,
// so an editor opened from a menu (no view at hand) still lands correctly.
UIViewController *top_presenter()
{
	// Preferred: the foreground-active scene's key window. Strictly, because a
	// background scene's window must not present.
	// Fallback: any window scene's key window. At scene-connect time (and in
	// tests that open windows from startup) nothing is foreground-active yet,
	// and refusing there turns a timing detail into a hard failure.
	UIViewController *top = nil;
	UIViewController *fallback = nil;
	for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
		if (![scene isKindOfClass:UIWindowScene.class])
			continue;
		UIWindow *win = nil;
		for (UIWindow *w in [(UIWindowScene *)scene windows]) {
			if ([w isKeyWindow]) {
				win = w;
				break;
			}
		}
		if (!win)
			continue;
		if ([scene activationState] == UISceneActivationStateForegroundActive) {
			top = win.rootViewController;
			break;
		}
		if (!fallback)
			fallback = win.rootViewController;
	}
	top = top ? top : fallback;
	while (top && [top presentedViewController])
		top = [top presentedViewController];
	return top;
}

} // namespace

pc_window::~pc_window()
{
	// Same teardown as the mac twin: the ImGui context and the panel's user
	// textures have to go before the context does, and metal_stop does both.
	if (m_imgui) {
		ImGuiContext *ctx = m_imgui;
		imshell::metal_stop(ctx);
		m_imgui = nullptr;
	}
	// The host struct is a plain C++ allocation holding ARC-managed pointers: the
	// pointers die with the struct, so this is just delete.
	delete (host *)m_ns;
	m_ns = nullptr;
}

bool pc_window::create(std::string &err)
{
	host *h = new host;
	m_ns = h;

	h->dev = MTLCreateSystemDefaultDevice();
	if (!h->dev) {
		delete h;
		m_ns = nullptr;
		err = UI_TEXT(dlg_metal_fail, "Cannot use Metal");
		return false;
	}
	h->queue = [h->dev newCommandQueue];

	// One context per window (ImGui attaches to the current one) - the same as
	// pc_window_mac.mm and pc_window.cpp. Same style, same keyboard nav, no
	// imgui.ini littered into the container.
	IMGUI_CHECKVERSION();
	m_imgui = ImGui::CreateContext();
	ImGui::SetCurrentContext(m_imgui);
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;
	// Cut/copy/paste inside the fields goes through here (UIPasteboard both
	// ways), the way the desktop front ends go through the OS clipboard. The
	// get buffer is static: ImGui uses the pointer only synchronously, and
	// every editor context runs on the main thread.
	ImGuiPlatformIO &platform_io = ImGui::GetPlatformIO();
	platform_io.Platform_SetClipboardTextFn = [](ImGuiContext *, const char *text) {
		if (text)
			[UIPasteboard generalPasteboard].string = [NSString stringWithUTF8String:text];
	};
	platform_io.Platform_GetClipboardTextFn = [](ImGuiContext *) -> const char * {
		static std::string buf;
		NSString *s = [UIPasteboard generalPasteboard].string;
		buf = s ? [s UTF8String] : "";
		return buf.c_str();
	};

	ImGui::StyleColorsDark();
	ImGuiStyle &st = ImGui::GetStyle();
	st.FrameRounding = 3;

	// The one shared font setup: ui/font_file.h asks CoreText for a face by
	// family name (never a hard-coded path), like the mac twin.
	add_cjk_font(io.Fonts);

	ImGui_ImplMetal_Init(h->dev);

	PCEditView *view = [[PCEditView alloc] initWithFrame:[UIScreen mainScreen].bounds];
	view->ctx = m_imgui;
	h->view = view;
	h->layer = (CAMetalLayer *)view.layer;
	h->layer.device = h->dev;
	h->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	h->layer.framebufferOnly = YES;

	PCEditController *vc = [[PCEditController alloc] init];
	vc.title = title_of_view(*m_view);
	// Subview pinned to the safe area, not the root view: edge-to-edge puts
	// content under the status bar and home indicator, which is where the
	// standalone's panel puts its editor-launching buttons. Same pattern, same
	// reason.
	vc.view.backgroundColor = UIColor.blackColor;
	view.translatesAutoresizingMaskIntoConstraints = NO;
	[vc.view addSubview:view];
	UILayoutGuide *safe = vc.view.safeAreaLayoutGuide;
	[NSLayoutConstraint activateConstraints:@[
		[view.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
		[view.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
		[view.topAnchor constraintEqualToAnchor:safe.topAnchor],
		[view.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
	]];
	UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
	nav.modalPresentationStyle = UIModalPresentationFullScreen;
	h->vc = nav;
	return true;
}

void pc_window::destroy()
{
	if (m_imgui) {
		ImGuiContext *ctx = m_imgui;
		imshell::metal_stop(ctx);
		m_imgui = nullptr;
	}
	if (host *h = (host *)m_ns) {
		[h->vc dismissViewControllerAnimated:YES completion:nil];
		delete h;
		m_ns = nullptr;
	}
}

bool pc_window::show(std::string &err)
{
	if (!m_ns && !create(err))
		return false;
	host *h = (host *)m_ns;
	if ([h->vc presentingViewController])
		return true;   // already up; showing it again is a no-op, not an error
	UIViewController *presenter = top_presenter();
	if (!presenter) {
		err = "no view controller to present from";
		return false;
	}
	[presenter presentViewController:h->vc animated:YES completion:nil];
	return true;
}

void pc_window::hide()
{
	if (host *h = (host *)m_ns) {
		if ([h->view isFirstResponder])
			[h->view resignFirstResponder];
		[h->vc dismissViewControllerAnimated:YES completion:nil];
	}
}

bool pc_window::visible() const
{
	// Presented (not merely created): presentingViewController is non-null
	// exactly while on screen, which also tracks user dismissal for free.
	const host *h = (const host *)m_ns;
	return h && h->vc && [h->vc presentingViewController] != nil;
}

void pc_window::shutdown(bridge &br)
{
	// Exactly the mac twin: tell the contents they closed (unmutes the overview,
	// releases what they held) under their own context.
	if (!m_imgui)
		return;
	ImGui::SetCurrentContext(m_imgui);
	m_view->hidden(br);
}

void pc_window::frame(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	host *h = (host *)m_ns;
	const bool shown = m_imgui && h && h->view && [h->vc presentingViewController] &&
	                   h->view.window;
	if (m_was_visible && !shown && m_imgui) {
		ImGui::SetCurrentContext(m_imgui);
		m_view->hidden(br);   // dismissed mid-gesture: release held-down buttons
	}
	m_was_visible = shown;
	if (!shown) {
		// Gone (dismissed or never shown): drop the keyboard if it was up for
		// a field. The view without a window cannot stay first responder, but
		// resigning here hides the keyboard on the dismissal frame rather
		// than leaving it orphaned over the panel.
		if (h && h->view && [h->view isFirstResponder])
			[h->view resignFirstResponder];
		return;
	}
	ImGui::SetCurrentContext(m_imgui);
	ImGuiIO &io = ImGui::GetIO();
	// The software keyboard follows the active field: first responder exactly
	// while WantTextInput holds, resigned the frame it clears. Tapping another
	// field keeps it up (resign+become across one frame is a no-op visually);
	// tapping outside deactivates the field and the keyboard goes with it.
	if (io.WantTextInput) {
		if (![h->view isFirstResponder])
			[h->view becomeFirstResponder];
	} else if ([h->view isFirstResponder]) {
		[h->view resignFirstResponder];
	}

	const CGRect b = [h->view bounds];
	const CGFloat scale = [h->view contentScaleFactor];
	io.DisplaySize = ImVec2(float(b.size.width), float(b.size.height));
	io.DisplayFramebufferScale = ImVec2(float(scale), float(scale));

	// The drawable decides the render pass, and the pass is wanted before
	// NewFrame (the same order the Metal example - and the mac twin - use).
	imshell::metal_sync(h->layer, h->view);
	id<CAMetalDrawable> drawable = [h->layer nextDrawable];
	if (!drawable)
		return;
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.10, 0.10, 0.11, 1.0);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;

	ImGui_ImplMetal_NewFrame(pass);
	ImGui::NewFrame();
	m_view->draw(m, ram, br);
	xgui::drag_flush(br);   // mouse-driven values, sent thinned
	ImGui::Render();

	id<MTLCommandBuffer> buf = [h->queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [buf renderCommandEncoderWithDescriptor:pass];
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), buf, enc);
	[enc endEncoding];
	[buf presentDrawable:drawable];
	[buf commit];
	// no wait: the main 30 Hz tick (frame_work) decides the pace, as on macOS
}

} // namespace ui