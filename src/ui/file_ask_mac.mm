// license:BSD-3-Clause
//
// The editors' file requests, answered with AppKit panels. See file_ask_mac.h
// for the shape, and why this is not in window_mac.mm.

#import "ui/file_ask_mac.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "ui/texts.h"

#include "ui/xg_ui.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace ui {
// The editors' file requests, answered with AppKit panels.
//
// Windows answers these with the Win32 common dialogs, at the end of its own
// render (src/ui/pc_window.cpp), and iOS with document pickers
// (src/ui/file_ask_ios.mm). This is the same seam in the same place on this
// platform: in the editor window class, which the standalone and the plug-in
// share - so both get it, exactly as both do on Windows.
//
// Until it existed, file_dialogs() was false: the editors showed a path box,
// which is usable here (a typed path is reachable) but which the SysEx
// export/import and the voice library's save do without - those were simply
// missing on this platform.
//
// Same order as Windows, so a request means the same thing on both:
// save, then MIDI (many files), then DLS, the card image, and the rest as bytes.

namespace {

// One open panel for several extensions. The panels above take a single one,
// which is all the ROM and MIDI paths need; the editors ask for "*img;*.sm;
// *.m2a" and "*wav;*.syx", and a filter that leaves out a kind the machine
// writes would be the wrong kind of helpful.
std::vector<std::string> open_files(const char *title,
                                    std::initializer_list<const char *> exts,
                                    bool multiple)
{
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	[panel setCanChooseFiles:YES];
	[panel setCanChooseDirectories:NO];
	[panel setAllowsMultipleSelection:multiple ? YES : NO];
	if (title && *title)
		[panel setMessage:[NSString stringWithUTF8String:title]];
	NSMutableArray<UTType *> *types = [NSMutableArray array];
	for (const char *ext : exts) {
		UTType *type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:ext]];
		if (type)
			[types addObject:type];
	}
	if ([types count])
		[panel setAllowedContentTypes:types];
	if ([panel runModal] != NSModalResponseOK)
		return {};
	std::vector<std::string> out;
	for (NSURL *url in [panel URLs]) {
		const char *path = [[url path] UTF8String];
		if (path)
			out.emplace_back(path);
	}
	return out;
}

// Where to save a new file. window_mac.mm has one of these for the standalone's
// own paths (a new SmartMedia image), declared in window_mac.h - which a plug-in
// does not link, hence the second copy.
std::string save_panel(const char *title, const char *default_name, const char *ext)
{
	NSSavePanel *panel = [NSSavePanel savePanel];
	if (title && *title)
		[panel setMessage:[NSString stringWithUTF8String:title]];
	if (default_name && *default_name)
		[panel setNameFieldStringValue:[NSString stringWithUTF8String:default_name]];
	if (ext && *ext) {
		UTType *type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:ext]];
		if (type)
			[panel setAllowedContentTypes:@[ type ]];
	}
	if ([panel runModal] != NSModalResponseOK)
		return {};
	NSURL *url = [panel URL];
	if (!url)
		return {};
	const char *path = [[url path] UTF8String];
	return path ? std::string(path) : std::string();
}

// Read a file into the shared layer's one-shot hand-off. The caps are the
// Windows ones: WAV can be the sampling RAM's worth of float stereo (about 48
// seconds), a SysEx dump is smaller, and the card image and the DLS bank are
// handed over as a path and never read at all.
bool give_file_bytes(const std::string &path, size_t cap)
{
	std::vector<u8> in;
	if (std::FILE *f = std::fopen(path.c_str(), "rb")) {
		u8 buf[65536];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && in.size() < cap)
			in.insert(in.end(), buf, buf + n);
		std::fclose(f);
		xgui::give_opened_file(std::move(in));
		return true;
	}
	xgui::set_file_note(UI_TEXT(note_import_fail, "Could not import"));
	return false;
}

} // namespace

void enable_file_dialogs()
{
	xgui::set_file_dialogs(true);
	xgui::set_midi_dialog(true);
}

void service_file_asks()
{
	std::vector<u8> bytes;
	const xgui::file_ask ask = xgui::take_file_ask(bytes);
	if (ask == xgui::file_ask::none)
		return;

	if (ask == xgui::file_ask::save) {
		const std::string path = save_panel(UI_TEXT(dlg_sysex_desc, "SysEx"),
		                                  "S-MU2000.syx", "syx");
		if (path.empty())
			return;
		std::FILE *f = std::fopen(path.c_str(), "wb");
		const bool ok = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
		if (f)
			std::fclose(f);
		char note[64];
		std::snprintf(note, sizeof note,
		              ok ? UI_TEXT(note_exported_fmt, "Exported (%zu bytes)")
		                 : UI_TEXT(note_export_fail, "Cannot export"),
		              bytes.size());
		xgui::set_file_note(note);
		return;
	}

	if (xgui::file_ask_is_midi()) {
		// Many at once, and paths rather than bytes: the player keeps a list and
		// re-reads a song whenever it comes back to it.
		std::vector<std::string> paths = open_files(UI_TEXT(dlg_midi_desc, "MIDI files"),
		                                            { "mid", "midi", "smf", "rcp", "r36" }, true);
		if (!paths.empty())
			xgui::give_opened_midi(std::move(paths));
		return;
	}
	if (xgui::file_ask_is_dls()) {
		const std::vector<std::string> paths =
		    open_files(UI_TEXT(dlg_dls_desc, "DLS sound bank"), { "dls" }, false);
		if (!paths.empty())
			xgui::give_opened_dls(paths.front());
		return;
	}
	if (xgui::file_ask_is_card()) {
		const std::vector<std::string> paths =
		    open_files(UI_TEXT(dlg_card_or_m2a_desc, "SmartMedia image or M2A file"),
		               { "img", "sm", "m2a" }, false);
		if (!paths.empty())
			xgui::give_opened_card(paths.front());
		return;
	}
	// WAV audio and SysEx dumps share one request in the sampling window, so one
	// panel offers both - the same filter Windows builds.
	if (xgui::file_ask_is_wav()) {
		const std::vector<std::string> paths =
		    open_files(UI_TEXT(dlg_wav_desc, "WAV audio or SysEx"), { "wav", "syx" }, false);
		if (!paths.empty())
			give_file_bytes(paths.front(), 64u << 20);
		return;
	}
	const std::vector<std::string> paths =
	    open_files(UI_TEXT(dlg_sysex_desc, "SysEx"), { "syx" }, false);
	if (!paths.empty())
		give_file_bytes(paths.front(), 16u << 20);
}


} // namespace ui
