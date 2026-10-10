// license:BSD-3-Clause
//
// Shared Dear ImGui plumbing for every window that paints the panel: the
// main windows (window_sdl.cpp / window_win.cpp / window_mac.mm), the
// plug-in windows (vst3/view_win.cpp / vst3/view_mac.mm) and the headless
// shot (ui/shot.h). Contexts and GPU targets stay per window (they cannot
// be shared); only the code is. The PC editor windows (ui/pc_window*)
// keep their own older setup and are untouched.
//
// The SDL family lives in imgui_shell_sdl.h next to this file: macOS
// plug-ins must not gain an SDL dependency through this header.

#ifndef S_MU2000_UI_IMGUI_SHELL_H
#define S_MU2000_UI_IMGUI_SHELL_H

#pragma once

#include "ui/draw_imgui.h"
#include "ui/font_file.h"
#include "ui/tex.h"

#include "imgui.h"

#include <functional>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "dxgi_stay.h"
#include <d3d11.h>
#include <windows.h>
#elif defined(__APPLE__)
// The Metal definitions, but ONLY for Objective-C++: this header is also read by
// plain C++ translation units (ui/app.h -> ui/shot.h reaches it from gui_mac.cpp),
// and Metal.h pulls in Foundation, which is not C++-clean - including it from a
// .cpp fails in NSObjCRuntime.h with "expected unqualified-id" and broke the mac
// build. The __OBJC__ section further down only forward-declares CAMetalLayer for
// C++, which is all plain C++ ever needs from here; the metal_* helpers that use
// the real types live inside #ifdef __OBJC__ themselves.
#ifdef __OBJC__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif
#include "backends/imgui_impl_metal.h"
#else
#include <fontconfig/fontconfig.h>
#endif

#ifdef __OBJC__
@class NSView;
@class UIView;
@class CAMetalLayer;
@protocol CAMetalDrawable;
@protocol MTLCommandQueue;
#endif

// rpcndr's legacy `#define small char` is re-live in TUs that reached the
// windows.h chain only through the includes above; fonts.h's guard can help
// before, this one helps at every f.small below (see the comment there).
#if defined(_MSC_VER) && defined(small)
#undef small
#endif

namespace ui {
namespace imshell {

// ---- fonts ---------------------------------------------------------------
//
// One CJK font in all three slots at 16 px, from ui/font_file.h. Falls back
// to the embedded font (slots stay null: English only). Adds to the current
// context, so call it after SetCurrentContext (new_context below does both).
//
// **This set is only for the window's own pieces** -- the button strip, the
// popups. The panel picture does not use it: panel.cpp rasterizes its own six
// sizes whenever the window changes size, because the LCD lettering is 4-9 px
// and one 16 px font scaled down is unreadable there.
inline im::fonts panel_fonts()
{
	im::fonts f{};
	const float em = cjk_face_em(false);
	f.label = f.small = f.tiny = add_cjk_ui_font(ImGui::GetIO().Fonts, 16.0f * em);
	f.label_px = 13.0f * em;
	f.small_px = 8.5f * em;
	f.tiny_px = 6.5f * em;
	// The strip at the top of the window: its own size, not a panel slot.
	//
	// This is GDI's 13 (toolbar.h), the same number as the panel's label slot,
	// and it is what the strip draws at on macOS and for Latin everywhere.
	// Windows CJK labels are scaled up inside toolbar::paint, because kanji
	// fill the em and Latin caps reach only 0.70 of it, so one size cannot suit
	// both in a 26 px band. It is a window fixed size: the strip does not scale
	// with the window, so a resize must not make it jump around.
	f.bar_px = 13.0f * em;
	return f;
}

// A context for one more panel window, current on return.
inline ImGuiContext *new_context()
{
	IMGUI_CHECKVERSION();
	ImGuiContext *ctx = ImGui::CreateContext();
	ImGui::SetCurrentContext(ctx);
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;    // no imgui.ini beside the ROMs or project
	return ctx;
}

#ifdef _WIN32

// ---- Direct3D 11 family: main window, plug-in window, headless shot -------

struct dx11_state {
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	IDXGISwapChain *swap = nullptr;      // null for the headless shot
	ID3D11RenderTargetView *rtv = nullptr;
	ImGuiContext *imgui = nullptr;
	im::fonts fonts{};
	int resize_w = 0, resize_h = 0;
};

// A windowed device for hwnd, or a WARP device when hwnd is null (shot).
// The shot sets its own target; windows get theirs from the swap chain.
inline bool dx11_start(dx11_state &st, HWND hwnd)
{
	if (hwnd) {
		DXGI_SWAP_CHAIN_DESC sd{};
		sd.BufferCount       = 2;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow      = hwnd;
		sd.SampleDesc.Count  = 1;
		sd.Windowed          = TRUE;
		sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL got;
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		                                           levels, 2, D3D11_SDK_VERSION,
		                                           &sd, &st.swap, &st.dev, &got, &st.ctx);
		if (hr == DXGI_ERROR_UNSUPPORTED)    // no GPU: software rasterizer
			hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
			                                   levels, 2, D3D11_SDK_VERSION,
			                                   &sd, &st.swap, &st.dev, &got, &st.ctx);
		if (FAILED(hr))
			return false;
		ID3D11Texture2D *back = nullptr;
		if (SUCCEEDED(st.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) {
			st.dev->CreateRenderTargetView(back, nullptr, &st.rtv);
			back->Release();
		}
	} else {
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL got;
		if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
		                             levels, 2, D3D11_SDK_VERSION, &st.dev, &got, &st.ctx)))
			return false;
	}

	ui::dxgi_stay(st.swap, hwnd);
	st.imgui = new_context();
	st.fonts = panel_fonts();
	if (hwnd)
		ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(st.dev, st.ctx);
	return true;
}

inline void dx11_stop(dx11_state &st)
{
	if (st.imgui) {
		ImGui::SetCurrentContext(st.imgui);
		// 絵のテクスチャは panel が見ている。まだ panel があるうちに
		// 登録を消さないと、窓のあとで panel が残る場合に参照が残る
		im::drop_user_textures();
		ImGui_ImplDX11_Shutdown();
		if (st.swap)
			ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(st.imgui);
		st.imgui = nullptr;
	}
	if (st.rtv) { st.rtv->Release(); st.rtv = nullptr; }
	if (st.swap) { st.swap->Release(); st.swap = nullptr; }
	if (st.ctx) { st.ctx->Release(); st.ctx = nullptr; }
	if (st.dev) { st.dev->Release(); st.dev = nullptr; }
	st.fonts = im::fonts{};
}

// Applies a pending resize, paints the caller's callback into the
// background list, submits. Presents windowed; headless leaves the
// caller-set target for readback.
inline void dx11_paint(dx11_state &st, int w, int h,
                       const std::function<void(ImDrawList *)> &paint)
{
	ImGui::SetCurrentContext(st.imgui);
	if (st.swap && st.resize_w) {
		if (st.rtv) { st.rtv->Release(); st.rtv = nullptr; }
		st.swap->ResizeBuffers(0, st.resize_w, st.resize_h, DXGI_FORMAT_UNKNOWN, 0);
		st.resize_w = st.resize_h = 0;
		ID3D11Texture2D *back = nullptr;
		if (SUCCEEDED(st.swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back))) {
			st.dev->CreateRenderTargetView(back, nullptr, &st.rtv);
			back->Release();
		}
	}
	ImGui_ImplDX11_NewFrame();
	if (st.swap)
		ImGui_ImplWin32_NewFrame();
	else
		ImGui::GetIO().DisplaySize = ImVec2(float(w), float(h));
	ensure_cjk_ui_fonts(ImGui::GetIO().Fonts);
	ImGui::NewFrame();
	paint(ImGui::GetBackgroundDrawList());
	ImGui::Render();
	const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	st.ctx->OMSetRenderTargets(1, &st.rtv, nullptr);
	st.ctx->ClearRenderTargetView(st.rtv, clear);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	if (st.swap)
		st.swap->Present(0, 0);   // no wait: the 30 Hz timers decide the pace
}

#endif // defined(_WIN32)

#ifdef __OBJC__

// ---- Metal family: main view, plug-in view (.mm only) ----------------------
//
// Only metal_attach and metal_sync mention a platform view class. metal_paint,
// metal_stop, new_context and panel_fonts below take a layer or nothing at all,
// and are shared as they stand: they speak CAMetalLayer/MTL and no AppKit.

#if TARGET_OS_IPHONE

// iOS: the layer comes from +layerClass rather than being set afterwards.
// UIView honours the override (it has to - that is the documented way to get a
// Metal-backed view), so there is no setLayer/wantsLayer dance here.
inline CAMetalLayer *metal_attach(UIView *view, id<MTLDevice> __strong &dev,
                                  id<MTLCommandQueue> __strong &queue)
{
	dev = MTLCreateSystemDefaultDevice();
	if (!dev)
		return nil;
	queue = [dev newCommandQueue];
	CAMetalLayer *layer = (CAMetalLayer *)view.layer;
	if (![layer isKindOfClass:CAMetalLayer.class])
		return nil;
	layer.device = dev;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = YES;
	return layer;
}

// UIKit has no window.backingScaleFactor, and a view's own contentScaleFactor
// is the same number: the backing store multiplier for this screen.
inline void metal_sync(CAMetalLayer *layer, UIView *view)
{
	if (!layer)
		return;
	const CGRect b = [view bounds];
	const CGFloat s = [view contentScaleFactor];
	layer.drawableSize = CGSizeMake(b.size.width * s, b.size.height * s);
}

#else // TARGET_OS_IPHONE

// macOS: the hosted layer (not +layerClass: AppKit's backing layer does not always
// honour the override) with its device and queue. Returns the layer, or nil.
inline CAMetalLayer *metal_attach(NSView *view, id<MTLDevice> __strong &dev,
                                  id<MTLCommandQueue> __strong &queue)
{
	dev = MTLCreateSystemDefaultDevice();
	if (!dev)
		return nil;
	queue = [dev newCommandQueue];
	CAMetalLayer *layer = [CAMetalLayer layer];
	layer.device = dev;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = YES;
	[view setLayer:layer];
	[view setWantsLayer:YES];
	return layer;
}

inline void metal_sync(CAMetalLayer *layer, NSView *view)
{
	if (!layer)
		return;
	const NSRect b = [view bounds];
	const CGFloat s = [view.window backingScaleFactor];
	layer.drawableSize = CGSizeMake(b.size.width * s, b.size.height * s);
}

#endif // TARGET_OS_IPHONE

// One frame at the view's size: the caller's paint into the background
// list, then submit + present. Quietly skips when no drawable is ready.
inline void metal_paint(ImGuiContext *ctx, CAMetalLayer *layer,
                        id<MTLCommandQueue> queue, float w, float h, float scale,
                        const std::function<void(ImDrawList *)> &paint)
{
	if (!ctx || !layer)
		return;
	ImGui::SetCurrentContext(ctx);
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(w, h);
	io.DisplayFramebufferScale = ImVec2(scale, scale);
	id<CAMetalDrawable> drawable = [layer nextDrawable];
	if (!drawable)
		return;
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	ImGui_ImplMetal_NewFrame(pass);
	ensure_cjk_ui_fonts(ImGui::GetIO().Fonts);
	ImGui::NewFrame();
	paint(ImGui::GetBackgroundDrawList());
	ImGui::Render();
	id<MTLCommandBuffer> buf = [queue commandBuffer];
	id<MTLRenderCommandEncoder> enc = [buf renderCommandEncoderWithDescriptor:pass];
	ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), buf, enc);
	[enc endEncoding];
	[buf presentDrawable:drawable];
	[buf commit];
}

inline void metal_stop(ImGuiContext *&ctx)
{
	if (!ctx)
		return;
	ImGui::SetCurrentContext(ctx);
	im::drop_user_textures();      // the panel's textures, see dx11_stop
	ImGui_ImplMetal_Shutdown();
	ImGui::DestroyContext(ctx);
	ctx = nullptr;
}

#endif // defined(__OBJC__)

} // namespace imshell
} // namespace ui

#endif // S_MU2000_UI_IMGUI_SHELL_H
