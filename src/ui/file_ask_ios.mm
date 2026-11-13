// license:BSD-3-Clause
//
// The editors' file requests, answered with document pickers. See
// file_ask_ios.h for the shape and for why each case is not what Windows does.
//
// The seam is xgui's, so this file only has to do what a picker cannot do on
// its own: copy what will be kept into Documents (a card image, a DLS bank, a
// playlist entry), and turn a save request into an export. What is only read -
// a WAV, a SysEx dump - is not copied at all: it goes straight into the machine
// and is never named again.

#import "ui/file_ask_ios.h"

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import <objc/runtime.h>

#import "ui/presenter_ios.h"
#import "ui/texts.h"

#include "compat/paths.h"
#include "ui/xg_ui.h"

#include <cstdio>
#include <string>
#include <vector>

static std::string utf8(NSString *s)
{
	const char *c = [s UTF8String];
	return c ? std::string(c) : std::string();
}

// Documents, not Application Support (which is what config_dir() names).
// Only Documents reaches the user: with UIFileSharingEnabled in Info.plist,
// Files shows this folder as "On My iPhone > S-MU2000", so a copy we keep can be
// found, replaced or deleted by hand - which is the point of keeping it. Library
// folders never appear there, and a file nobody can delete is a file nobody can
// get rid of when they picked the wrong one.
static std::string documents_dir()
{
	NSURL *url = [[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory
	                                                       inDomains:NSUserDomainMask].firstObject;
	return url ? utf8(url.path) : std::string();
}

// A folder inside it, made if it is not there yet. Callers add a file name with
// smu2000::join() - it is the one that puts the separator back.
static std::string kept_dir(const char *leaf)
{
	const std::string docs = documents_dir();
	if (docs.empty())
		return {};
	const std::string dir = smu2000::join(docs, leaf);
	std::filesystem::create_directories(std::filesystem::path(dir));
	return dir;
}

// The user picked a file we cannot keep where it is, so a copy landed in
// Documents. Say so, once, with the place - a copy the user cannot find is a
// copy they will pick again. The note goes through the shared layer like every
// other one, which is why it needs a text field (ui::texts, note_kept_fmt).
static void note_kept(const char *leaf)
{
	const std::string dir = kept_dir(leaf);
	char note[160];
	std::snprintf(note, sizeof note, UI_TEXT(note_kept_fmt, "Copied into %s (Files: On My iPhone > S-MU2000)"), leaf);
	std::fprintf(stderr, "[ios] file: kept in %s\n", dir.c_str());
	ui::xgui::set_file_note(note);
}

// What the flow in flight has to hand back when the picker answers.
enum class ask_kind {
	save,       // ask_save_file: the bytes are already out, the picker only moves them
	bytes,      // ask_open_file / ask_open_wav: give_opened_file
	path,       // ask_open_card / ask_open_dls: give_opened_card / give_opened_dls
	midi_paths, // ask_open_midi: give_opened_midi
};

// The delegate of every picker below, kept alive here for the presentation
// (UIDocumentPickerViewController.delegate is weak), released when the flow
// ends. Same shape as rom_import_ios.mm's g_importer.
static id g_flow = nil;
static ask_kind g_kind = ask_kind::bytes;


@interface SMUFileAskDelegate : NSObject <UIDocumentPickerDelegate>
@property (nonatomic, strong) UIDocumentPickerViewController *picker;
@end

// g_flow is what service_file_asks() checks to keep one picker at a time, so it
// has to be clear whenever no picker is up - including on the paths below that
// return early (a read that failed, a container that could not be written, an
// empty pick). It used to be cleared on one path only, so any of those returns
// left it set and no further picker ever opened: the second attempt to import
// anything did nothing at all, with no message. A scope guard clears it on the
// way out of the handler whichever way it goes, and the delegate it holds goes
// with it (UIDocumentPickerViewController.delegate is weak, so this is also what
// keeps the delegate alive for the presentation).
namespace {
struct flow_cleared {
	~flow_cleared() { g_flow = nil; }
};
} // namespace

@implementation SMUFileAskDelegate

- (void)documentPicker:(UIDocumentPickerViewController *)picker
didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
	(void)picker;
	flow_cleared cleared;
	switch (g_kind) {
	case ask_kind::save:
		break;   // the note was already given when the bytes were written
	case ask_kind::bytes: {
		// Read while the grant lasts: a picked file's access dies with the
		// process, and this is the only moment it exists.
		NSMutableData *in = [NSMutableData data];
		for (NSURL *url in urls) {
			const BOOL granted = [url startAccessingSecurityScopedResource];
			NSData *one = [NSData dataWithContentsOfURL:url];
			if (granted)
				[url stopAccessingSecurityScopedResource];
			if (!one) {
				ui::xgui::set_file_note(UI_TEXT(note_import_fail, "Could not import"));
				return;
			}
			[in appendData:one];
		}
		const u8 *p = (const u8 *)in.bytes;
		ui::xgui::give_opened_file(std::vector<u8>(p, p + in.length));
		break;
	}
	case ask_kind::path: {
		// Card images reach 132MB and a DLS bank is not small either, so the
		// path is what goes back - never the bytes. Which is why it has to be a
		// container path: nothing outside survives the grant.
		NSURL *url = urls.firstObject;
		if (!url)
			return;
		const char *leaf = ui::xgui::file_ask_is_dls() ? "dls" : "cards";
		const std::string dir = kept_dir(leaf);
		if (dir.empty()) {
			ui::xgui::set_file_note(UI_TEXT(note_import_fail, "Could not import"));
			return;
		}
		const std::string dest = smu2000::join(dir, utf8(url.lastPathComponent));
		const BOOL granted = [url startAccessingSecurityScopedResource];
		// The remove's own answer is not the question. It returns NO when there
		// is nothing to remove - which is the case for every first import, since
		// the name is new - so asking for both with && meant the copy ran only
		// when a file of that name was already there. Re-importing over an
		// existing file works; importing anything new does not. Removed with
		// the result ignored, as the same remove is done in view_ios.mm.
		[[NSFileManager defaultManager] removeItemAtPath:@(dest.c_str()) error:nil];
		const BOOL copied = [[NSFileManager defaultManager] copyItemAtURL:url
		                                                  toURL:[NSURL fileURLWithPath:@(dest.c_str())]
		                                                  error:nil];
		if (granted)
			[url stopAccessingSecurityScopedResource];
		if (!copied) {
			std::fprintf(stderr, "[ios] file: cannot keep %s\n", dest.c_str());
			ui::xgui::set_file_note(UI_TEXT(note_import_fail, "Could not import"));
			return;
		}
		std::fprintf(stderr, "[ios] file: %s\n", dest.c_str());
		note_kept(leaf);
		if (ui::xgui::file_ask_is_dls())
			ui::xgui::give_opened_dls(dest);
		else
			ui::xgui::give_opened_card(dest);
		break;
	}
	case ask_kind::midi_paths: {
		// The player keeps a list of paths and re-reads a song whenever it comes
		// back to it, so the picked files are copied in beside the ROMs and the
		// container's paths are what it gets.
		const std::string dir = kept_dir("midi");
		if (dir.empty()) {
			ui::xgui::set_file_note(UI_TEXT(note_import_fail, "Could not import"));
			return;
		}
		std::vector<std::string> paths;
		for (NSURL *url in urls) {
			const std::string dest = smu2000::join(dir, utf8(url.lastPathComponent));
			const BOOL granted = [url startAccessingSecurityScopedResource];
			// See the note in ask_kind::path above: the remove's NO for a name
			// that is not there yet must not gate the copy.
			[[NSFileManager defaultManager] removeItemAtPath:@(dest.c_str()) error:nil];
			const BOOL copied = [[NSFileManager defaultManager] copyItemAtURL:url
			                                                  toURL:[NSURL fileURLWithPath:@(dest.c_str())]
			                                                  error:nil];
			if (granted)
				[url stopAccessingSecurityScopedResource];
			if (copied) {
				std::fprintf(stderr, "[ios] midi: %s\n", dest.c_str());
				paths.push_back(dest);
			} else {
				std::fprintf(stderr, "[ios] midi: cannot keep %s\n", dest.c_str());
			}
		}
		if (!paths.empty()) {
			note_kept("midi");
			ui::xgui::give_opened_midi(std::move(paths));
		}
		break;
	}
	}
	// g_flow is cleared by the guard above, on this path and on every early
	// return in the switch.
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)picker
{
	(void)picker;
	// Cancelled is silence on every platform here: the shared side simply never
	// gets an answer, and its one-shot hand-off times out on the next frame.
	g_flow = nil;
}

@end

namespace ui {

void enable_file_dialogs()
{
	ui::xgui::set_file_dialogs(true);
	ui::xgui::set_midi_dialog(true);
}

void service_file_asks(UIView *view)
{
	// One picker at a time. A request that arrives while one is up stays pending
	// rather than being taken and dropped: the editors wait for an answer.
	if (g_flow)
		return;
	std::vector<u8> bytes;
	const ui::xgui::file_ask ask = ui::xgui::take_file_ask(bytes);
	if (ask == ui::xgui::file_ask::none)
		return;
	// Asked before the flow is claimed, so a request that cannot be presented is
	// left pending rather than taken and lost: the editor waits for an answer, and
	// take_file_ask() is one-shot. The check that matters is the second one -
	// presenter_for() returns something even mid-presentation, and presenting on a
	// controller that is already presenting is refused by UIKit with only a log
	// line, which is how a request used to leave g_flow set with no delegate ever
	// called and every later request skipped by the guard above.
	UIViewController *presenter = presenter_for(view);
	if (!presenter || presenter.presentedViewController ||
	    presenter.isBeingDismissed || presenter.isBeingPresented) {
		std::fprintf(stderr, "[ios] file: nothing to present a picker from; still pending\n");
		return;
	}
	SMUFileAskDelegate *delegate = [[SMUFileAskDelegate alloc] init];

	if (ask == ui::xgui::file_ask::save) {
		// No save panel on iOS: write the bytes somewhere real and let the export
		// picker say where they should end up. The name is Windows', so a file
		// exported on either platform lands the same way.
		const std::string tmp = std::string(NSTemporaryDirectory().UTF8String) + "S-MU2000.syx";
		std::FILE *f = std::fopen(tmp.c_str(), "wb");
		const bool ok = f && bytes.size() < 64u << 20
		            && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
		if (f)
			std::fclose(f);
		if (!ok) {
			ui::xgui::set_file_note(UI_TEXT(note_export_fail, "Cannot export"));
			return;
		}
		char note[64];
		std::snprintf(note, sizeof note, UI_TEXT(note_exported_fmt, "Exported (%zu bytes)"),
		              bytes.size());
		ui::xgui::set_file_note(note);
		UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
		    initForExportingURLs:@[ [NSURL fileURLWithPath:@(tmp.c_str())] ]
		                      asCopy:YES];
		delegate.picker = picker;
		picker.delegate = delegate;
		g_kind = ask_kind::save;
		g_flow = delegate;
		[presenter presentViewController:picker animated:YES completion:nil];
		return;
	}

	// asCopy:YES: a single file may be copied, and it is folders that UIKit
	// refuses to copy. We want the bytes or a copy of our own, either way.
	NSArray<UTType *> *types;
	if (ui::xgui::file_ask_is_midi()) {
		// RCP-family files are MIDI inputs too, but iOS does not identify their
		// extensions as UTTypeMIDI. Add their filename-derived types explicitly.
		NSMutableArray<UTType *> *midiTypes = [NSMutableArray arrayWithObject:UTTypeMIDI];
		for (NSString *ext in @[ @"rcp", @"r36", @"g18", @"g36" ]) {
			UTType *type = [UTType typeWithFilenameExtension:ext];
			if (type)
				[midiTypes addObject:type];
		}
		types = [midiTypes copy];
		g_kind = ask_kind::midi_paths;
	} else if (ui::xgui::file_ask_is_dls() || ui::xgui::file_ask_is_card()) {
		// .dls, .img, .sm and .m2a are all public.data to the system, so asking
		// for that is the filter; there is no UTI to be narrower with.
		types = @[ UTTypeData ];
		g_kind = ask_kind::path;
	} else if (ui::xgui::file_ask_is_wav()) {
		// WAV audio, plus public.data so a .syx is reachable too: the sampling
		// window asks for both through ask_open_wav().
		types = @[ UTTypeWAV, UTTypeData ];
		g_kind = ask_kind::bytes;
	} else {
		// SysEx dumps from a real MU2000 carry no type of their own.
		types = @[ UTTypeData ];
		g_kind = ask_kind::bytes;
	}
	UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
	    initForOpeningContentTypes:types
	                          asCopy:YES];
	picker.allowsMultipleSelection = g_kind == ask_kind::midi_paths;
	delegate.picker = picker;
	picker.delegate = delegate;
	g_flow = delegate;
	[presenter presentViewController:picker animated:YES completion:nil];
}

} // namespace ui