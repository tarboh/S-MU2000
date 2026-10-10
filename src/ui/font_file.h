// license:BSD-3-Clause
//
// The panel's CJK face: one regular and, where the system has one, one bold --
// the editor windows' shape (family names per platform, first match wins) with
// a bold alongside.
//
// Panel.cpp needs the bytes too and must not drag the renderer backends in, so
// this lives on its own: panel.cpp only ever draws.

#ifndef S_MU2000_UI_FONT_FILE_H
#define S_MU2000_UI_FONT_FILE_H

#pragma once

#include "imgui.h"

#include "ui/lang.h"

#include <cstddef>
#include "font_check.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// One face a platform offers: a file to read, or -- on Windows, where GDI has
// no path to give -- the bytes themselves, fetched lazily. The walk stops at
// the first face it accepts.
struct face_bytes {
	std::vector<unsigned char> data;
	int                        face = 0;    // index inside a TTC; 0 for a lone font
	float                      em = 1.0f;   // hhea span per em; stb sizes by the
	                                        // former, everyone else by the latter
};

struct face_offer {
	std::string                 path;
	std::function<face_bytes()> fetch;
	bool                        japanese = true;   // false: a Latin-only last resort (Linux)
};

// The families, most wanted first. The first five ship with every macOS since
// 10.15; the rest cover other installs, and Linux, where names vary by distro.
static const char *const cjk_families[] = {
	"Hiragino Sans",
	"Hiragino Kaku Gothic ProN",
	"Hiragino Kaku Gothic Pro",
	"Hiragino Sans GB",
	"Osaka",
	"Yu Gothic",
	"MS PGothic",
	"Noto Sans CJK JP",
	"Noto Sans CJK",
	"Noto Sans JP",
	"Source Han Sans",
	"IPAGothic",
	"IPAPGothic",
	"VL Gothic",
	"Takao Gothic",
	"MS Gothic",
};

// The weight goes in the family name, not in a weight attribute: CoreText
// resolves 400, 600 and 700 to the same face, and echoes the asked weight back
// on read, so a weight attribute can neither select nor verify a bold. A family
// with no bold name resolves to its regular file, which the panel then draws at
// the bold slots -- no worse than a machine without Japanese.

inline constexpr ImWchar cjk_fullwidth_ranges[] = { 0xFF00, 0xFFEF, 0 };
inline constexpr ImWchar cjk_english_ranges[] = { 0x20, 0xFF, 0xFF00, 0xFFEF, 0 };

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The families on this machine that can draw Shift-JIS -- EnumFontFamiliesEx
// with lfCharSet -- and the file behind a request for one of them, which is
// GetFontData with the 'ttcf' tag (the whole collection, every face in it).
//
// lfCharSet must be SHIFTJIS_CHARSET, not DEFAULT_CHARSET: with the latter, GDI
// substitutes a fallback face per glyph at draw time, and ImGui rasterizes from
// memory without ever going through that linker. Naming the charset moves the
// substitution into mapping, which puts a real Japanese face in the font object.
// Both weights use the same request otherwise, so GDI maps them to one family.

static const char *const cjk_wanted_families[] = {
	// what a Japanese Windows install has, best first
	"Yu Gothic UI", "Yu Gothic", "Meiryo UI", "Meiryo",
	// and the tail, for the installs that have none of the above
	"MS Gothic", "MS UI Gothic", "MS PGothic",
};

static int CALLBACK cjk_collect_family(const LOGFONTA *lf, const TEXTMETRICA *,
                                       DWORD, LPARAM param)
{
	auto *names = reinterpret_cast<std::vector<std::string> *>(param);
	const std::string name = lf->lfFaceName;
	if (name.empty())
		return 1;
	for (const std::string &have : *names)  // one face is reported per family
		if (have == name)
			return 1;
	names->push_back(name);
	return 1;
}

// Font files are big-endian throughout.
static unsigned cjk_tt16(const unsigned char *p)
{
	return (unsigned(p[0]) << 8) | p[1];
}

static unsigned cjk_tt32(const unsigned char *p)
{
	return (unsigned(p[0]) << 24) | (unsigned(p[1]) << 16) |
	       (unsigned(p[2]) << 8) | p[3];
}

// usWeightClass of the face starting at byte `off`, or -1 when it has no OS/2
// table to ask. 100 is thin, 400 regular, 700 bold, 900 black.
static int cjk_face_weight_at(const unsigned char *d, size_t n, size_t off)
{
	if (off + 12 > n)
		return -1;
	const unsigned tables = cjk_tt16(d + off + 4);
	for (unsigned i = 0; i < tables; i++) {
		const size_t rec = off + 12 + size_t(i) * 16;
		if (rec + 16 > n)
			return -1;
		if (cjk_tt32(d + rec) != 0x4F532F32u)   // 'OS/2'
			continue;
		const size_t at = cjk_tt32(d + rec + 8);
		if (at + 6 > n)
			return -1;
		return int(cjk_tt16(d + at + 4));
	}
	return -1;
}

// Byte offsets of every face in d: a collection lists them in its header, a
// lone font is one face at zero. Returns how many, or 0 when d is not a font.
static size_t cjk_face_offsets(const unsigned char *d, size_t n,
                               size_t *offs, size_t cap)
{
	if (n < 12 || cap == 0)
		return 0;
	if (cjk_tt32(d) != 0x74746366u) {           // not 'ttcf': one face
		offs[0] = 0;
		return 1;
	}
	if (n < 16)
		return 0;
	const unsigned count = cjk_tt32(d + 8);
	if (count == 0 || count > cap || 12 + size_t(count) * 4 > n)
		return 0;
	for (unsigned i = 0; i < count; i++) {
		const size_t off = cjk_tt32(d + 12 + size_t(i) * 4);
		if (off + 12 > n)
			return 0;
		offs[i] = off;
	}
	return count;
}

static int cjk_ts16(const unsigned char *p)
{
	return int(short((unsigned(p[0]) << 8) | p[1]));
}

// hhea span per em, for the face at `off`. stb sizes by the hhea span while
// GDI maps to the em; the ratio puts stb back on em terms. 1.0 on anything
// unexpected: an unscaled honest size beats a wild factor.
static float cjk_em_scale(const unsigned char *d, size_t n, size_t off)
{
	unsigned upm = 0;
	int ha = 0, hd = 0;
	if (off + 12 <= n) {
		const unsigned tables = cjk_tt16(d + off + 4);
		for (unsigned i = 0; i < tables; i++) {
			const size_t rec = off + 12 + size_t(i) * 16;
			if (rec + 16 > n)
				break;
			const unsigned tag = cjk_tt32(d + rec);
			const size_t at = cjk_tt32(d + rec + 8);
			if (tag == 0x68656164u && at + 20 <= n)        // 'head'
				upm = cjk_tt16(d + at + 18);
			else if (tag == 0x68686561u && at + 8 <= n) {  // 'hhea'
				ha = cjk_ts16(d + at + 4);
				hd = cjk_ts16(d + at + 6);
			}
		}
	}
	if (!upm || ha - hd <= 0)
		return 1.0f;
	const float k = float(ha - hd) / float(upm);
	return k >= 0.5f && k <= 2.0f ? k : 1.0f;
}

// One GetFontData call. The size is the plain DWORD cjBuffer of the header's own
// declaration -- wingdi.h spells it that way in every SDK generation and in
// MinGW, so the call takes it by value and there is nothing to switch on here.
static bool cjk_get_font_bytes(HDC dc, DWORD tag, std::vector<unsigned char> &data)
{
	const DWORD size = GetFontData(dc, tag, 0, nullptr, 0);
	if (!size || size == DWORD(GDI_ERROR))
		return false;
	data.resize(size);
	const bool ok = GetFontData(dc, tag, 0, data.data(), DWORD(data.size())) != GDI_ERROR;
	if (!ok)
		data.clear();
	return ok;
}

// The whole collection behind one GDI request, and which face of it GDI mapped
// to. A zero tag hands back a single face of a TTC, whose tables point outside
// themselves -- that crashed stb_truetype on Yu Gothic UI. The face goes in by
// ImFontConfig::FontNo; GDI never reports which one it picked, so the mapped
// face's own TEXTMETRIC is read back and the closest OS/2 weight wins.
static face_bytes cjk_gdi_bytes(const char *family, int weight, bool bold)
{
	HFONT font = CreateFontA(-13, 0, 0, 0, weight, FALSE, FALSE, FALSE,
	                         SHIFTJIS_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
	                         CLEARTYPE_QUALITY, VARIABLE_PITCH, family);
	if (!font)
		return {};
	face_bytes out;
	if (HDC dc = CreateCompatibleDC(nullptr)) {
		// GetFontData reads the font *selected into a DC*, never the object.
		const HGDIOBJ was = SelectObject(dc, font);
		TEXTMETRICA tm{};
		const int target = GetTextMetricsA(dc, &tm) && tm.tmWeight
		                     ? int(tm.tmWeight)
		                     : (bold ? FW_BOLD : FW_NORMAL);
		// The tag is byte-swapped relative to the file: 'ttcf' on disk is
		// 74 74 63 66, but GetFontData takes it little-endian, 0x66637474.
		// The other order compiles fine and returns GDI_ERROR for the size,
		// so the walk below silently ends at the embedded font.
		if (!cjk_get_font_bytes(dc, 0x66637474u /* 'ttcf' */, out.data))
			cjk_get_font_bytes(dc, 0, out.data);   // a lone font, not a collection
		SelectObject(dc, was);
		DeleteDC(dc);
		if (!out.data.empty()) {
			size_t offs[32];
			const size_t count =
			    cjk_face_offsets(out.data.data(), out.data.size(), offs, 32);
			if (!count)
				out.data.clear();
			else {
				size_t best = 0, best_off = offs[0];
				unsigned gap = ~0u;
				for (size_t i = 0; i < count; i++) {
					const int w = cjk_face_weight_at(out.data.data(),
					                                 out.data.size(), offs[i]);
					if (w < 0)
						continue;
					const unsigned d = unsigned(abs(w - target));
					if (d < gap) {
						gap = d;
						best = i;
						best_off = offs[i];
					}
				}
				out.em = cjk_em_scale(out.data.data(), out.data.size(), best_off);
				out.face = int(best);
			}
		}
	}
	DeleteObject(font);
	return out;
}

inline void cjk_offers(bool bold, std::vector<face_offer> &out)
{
	// A DC of its own: a null DC enumerates nothing, and the walk ends at the
	// embedded font.
	HDC dc = CreateCompatibleDC(nullptr);
	LOGFONTA filter = {};                // not LOGFONT: that is the wide one here
	filter.lfCharSet = SHIFTJIS_CHARSET;
	filter.lfWeight  = FW_DONTCARE;
	std::vector<std::string> families;
	EnumFontFamiliesExA(dc, &filter, cjk_collect_family,
	                    reinterpret_cast<LPARAM>(&families), 0);
	if (dc)
		DeleteDC(dc);
	// GDI enumerates by name, so the wanted families move up front, in order;
	// the rest follow as enumerated.
	std::vector<std::string> ordered;
	for (const char *want : cjk_wanted_families)
		for (const std::string &have : families)
			if (_stricmp(have.c_str(), want) == 0)
				ordered.push_back(have);
	for (const std::string &have : families) {
		bool promoted = false;
		for (const std::string &first : ordered)
			promoted = promoted || first == have;
		if (!promoted)
			ordered.push_back(have);
	}
	const int weight = bold ? FW_BOLD : FW_DONTCARE;
	for (const std::string &family : ordered)
		out.push_back({ std::string(),
		                [family, weight, bold] {
			                return cjk_gdi_bytes(family.c_str(), weight, bold);
		                } });
}

// The XG editor windows keep upstream's exact list: first existing file, face
// 0. The unified walk above resolves the same family but picks by weight
// (Regular 400), while upstream renders face 0 (Medium 500) -- visibly heavier
// at UI sizes, and the editors were already ImGui upstream, so they must not
// move. One deliberate fork, documented here, not drift: the panel cannot use
// files at all (its bold must pair with its regular), the editors never needed
// anything else.
// The font directory of the Windows that is running. Not "C:\\Windows\\Fonts":
// Windows can live on any drive, and on a dual-boot machine C: may hold another
// Windows whose files this one cannot read (reported on Windows 8.1 running from
// E: with Windows 10 on C: -- the file "existed" and then would not open).
inline std::string windows_font_dir()
{
	char dir[MAX_PATH] = {};
	const UINT n = GetWindowsDirectoryA(dir, sizeof(dir));
	if (n == 0 || n >= sizeof(dir))
		return {};
	return std::string(dir) + "\\Fonts\\";
}

// Can the file actually be opened and read? Existing is not enough (see above),
// and a file handed to ImGui that then fails to load is an assertion there.
inline bool font_file_readable(const std::string &path)
{
	const HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	char head[4];
	DWORD got = 0;
	const bool ok = ReadFile(h, head, sizeof(head), &got, nullptr) && got == sizeof(head);
	CloseHandle(h);
	return ok;
}

inline ImFont *add_cjk_editor_font(ImFontAtlas *atlas, float px = 16.0f)
{
	static const char *const FILES[] = { "YuGothM.ttc", "meiryo.ttc", "msgothic.ttc" };
	// A font that will not load must never take the program down: ImGui's default
	// is to treat it as a programming error and assert. Ask it not to, and fall
	// through to the next file, then to the built-in font.
	ImFontConfig cfg;
	cfg.Flags |= ImFontFlags_NoLoadError;
	cfg.GlyphRanges = ui::show_english() ? cjk_english_ranges : atlas->GetGlyphRangesJapanese();
	const std::string dir = windows_font_dir();
	for (const char *name : FILES) {
		const std::string path = dir + name;
		if (dir.empty() || !font_file_readable(path))
			continue;
		if (ImFont *font = atlas->AddFontFromFileTTF(path.c_str(), px, &cfg))
			return font;
	}
	return atlas->AddFontDefault();
}

#elif defined(__APPLE__)

#include <CoreText/CoreText.h>

// The family-name -> file walk CoreText does for us. An absent family comes
// back with no URL: a real answer, unlike fontconfig's closest match.
static std::string cjk_family_path(const char *family)
{
	CFStringRef cf = CFStringCreateWithCString(nullptr, family, kCFStringEncodingUTF8);
	if (!cf)
		return {};
	const void *keys[]   = { kCTFontFamilyNameAttribute };
	const void *values[] = { cf };
	CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, values, 1,
	                                           &kCFTypeDictionaryKeyCallBacks,
	                                           &kCFTypeDictionaryValueCallBacks);
	CFRelease(cf);
	if (!attrs)
		return {};
	CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
	CFRelease(attrs);
	if (!desc)
		return {};
	CFURLRef url = (CFURLRef)CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
	CFRelease(desc);
	if (!url)
		return {};
	char buf[1024] = {};
	std::string path;
	if (CFURLGetFileSystemRepresentation(url, true, (UInt8 *)buf, sizeof(buf)))
		path = buf;
	CFRelease(url);
	return path;
}

inline void cjk_offers(bool bold, std::vector<face_offer> &out)
{
	for (const char *family : cjk_families) {
		const std::string path =
			bold ? cjk_family_path((std::string(family) + " W6").c_str())
			     : cjk_family_path(family);
		if (!path.empty())
			out.push_back({ path, {} });
	}
}

#else

#include <fontconfig/fontconfig.h>

#include <algorithm>

// ---- picking the face -------------------------------------------------------
//
// cjk_families is a preference order, not a test: a distro can have a perfectly
// good Japanese face under a name that is not on the list. So the question is
// asked in languages rather than in names -- lang=ja for Japanese, lang=ja,en
// for Japanese and Latin together. fontconfig intersects the tags, so naming
// both asks for a face that covers both.
//
// That intersection is what keeps a CJK-only fallback out of the slot that
// draws the whole UI. A fallback such as Droid Sans Fallback covers kana and
// kanji and expects another font to supply the Latin, so putting it in the
// program's one font draws the English labels as nothing at all -- not as tofu
// boxes, because the glyphs it lacks are drawn from nowhere. It is the one case
// the merge in add_cjk_ui_font() exists to mend, and the merge source is a
// lang=ja query, which is happy to take a CJK-only face.
//
// A query has to be able to fail, which is why this is FcFontList and not
// FcFontMatch. A match never fails: FcDefaultSubstitute fills a missing family
// in with the closest thing the system has, so asking for a macOS face on Linux
// answers with DejaVu, and every fullwidth bracket and kanji outside 0x00FF
// comes out as a tofu box. An empty list is the honest answer.

// One pattern's file, added once.
static void cjk_add_file(std::vector<std::string> &out, const FcPattern *font)
{
	FcChar8 *file = nullptr;
	if (FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch || !file)
		return;
	std::string path = reinterpret_cast<const char *>(file);
	if (std::find(out.begin(), out.end(), path) == out.end())
		out.push_back(std::move(path));
}

// The files for one family name (none when the machine has no such family),
// the wanted weight first.
//
// **Every** file is returned, in the order fontconfig ranks them, not just the
// first: the first may be one the rasteriser cannot read. openSUSE Tumbleweed
// ships Noto Sans CJK as a variable font (CFF2 outlines), which stb_truetype
// rejects, and Dear ImGui asserts on a font it cannot parse (issue #135). The
// walk in cjk_face_data() tries each in turn and keeps the first that parses.
static std::vector<std::string> cjk_fontconfig_match(const char *family, bool bold)
{
	// A font's own family list holds its regional and weight names too, so
	// "Noto Sans CJK" finds the "Noto Sans CJK JP" face without any prefix
	// handling here: one family with regional files, not several families.
	//
	// Weight is an exact test, not a threshold, so a family with no bold comes
	// back empty on the first pass and is taken as it is on the second. The
	// second pass always runs now, after the first: a family whose file at the
	// wanted weight cannot be read may have another that can.
	std::vector<std::string> paths;
	for (int pass = 0; pass < 2; pass++) {
		FcPattern *pat = FcPatternCreate();
		if (!pat)
			return paths;
		FcPatternAddString(pat, FC_FAMILY,
		                   reinterpret_cast<const FcChar8 *>(family));
		if (!pass)
			FcPatternAddInteger(pat, FC_WEIGHT,
			                    bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
		FcFontSet *set = FcFontList(nullptr, pat, nullptr);
		if (set) {
			for (int i = 0; i < set->nfont; i++)
				cjk_add_file(paths, set->fonts[i]);
			FcFontSetDestroy(set);
		}
		FcPatternDestroy(pat);
	}
	return paths;
}

// The files of the faces fontconfig offers for these languages (none when it
// offers none).
static std::vector<std::string> cjk_fontconfig_scan(std::initializer_list<const char *> langs)
{
	std::vector<std::string> found;
	FcPattern *pat = FcPatternCreate();
	if (!pat)
		return found;
	FcLangSet *set = FcLangSetCreate();
	for (const char *tag : langs)
		FcLangSetAdd(set, reinterpret_cast<const FcChar8 *>(tag));
	FcPatternAddLangSet(pat, FC_LANG, set);
	FcLangSetDestroy(set);
	FcFontSet *faces = FcFontList(nullptr, pat, nullptr);
	FcPatternDestroy(pat);
	if (!faces)
		return found;
	for (int i = 0; i < faces->nfont; i++)
		cjk_add_file(found, faces->fonts[i]);
	FcFontSetDestroy(faces);
	return found;
}

// The program's one font, so it wants Latin as well as Japanese.
static std::vector<std::string> cjk_fontconfig_any()
{
	return cjk_fontconfig_scan({ "ja", "en" });
}

// Japanese alone, for the merge source, so a CJK-only face will do.
static std::vector<std::string> cjk_fontconfig_japanese()
{
	return cjk_fontconfig_scan({ "ja" });
}

// The system's own sans, for a machine with no Japanese face installed: the
// best match first, then what fontconfig would fall back to (FcFontSort), so
// there is a next one to try when the best cannot be read.
static std::vector<std::string> cjk_fontconfig_sans()
{
	std::vector<std::string> paths;
	FcPattern *pat = FcPatternCreate();
	if (!pat)
		return paths;
	FcPatternAddString(pat, FC_FAMILY,
	                   reinterpret_cast<const FcChar8 *>("sans-serif"));
	FcPatternAddDouble(pat, FC_SIZE, 16.0);
	FcConfigSubstitute(nullptr, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	FcResult res = FcResultNoMatch;
	if (FcFontSet *sorted = FcFontSort(nullptr, pat, FcTrue, nullptr, &res)) {
		for (int i = 0; i < sorted->nfont && paths.size() < 24; i++)
			cjk_add_file(paths, sorted->fonts[i]);
		FcFontSetDestroy(sorted);
	}
	FcPatternDestroy(pat);
	return paths;
}

// Whether the face the walk settles on draws Japanese. Every named family is a
// CJK face by construction and the lang query asks for it, so only the
// last-ditch sans fallback is Latin-only.
//
// inline, not static: the panel and the editor windows are separate translation
// units, and a static in a header gives each one its own copy. The panel's walk
// then set its copy and left the editors' untouched, so the merge was skipped
// and the Japanese text never arrived.
#if !defined(_WIN32) && !defined(__APPLE__)
inline bool cjk_primary_covers_japanese = true;
#endif

inline void cjk_offers(bool bold, std::vector<face_offer> &out)
{
	// No dedupe: repeats cost nothing, the walk stops at the first face.
#if !defined(_WIN32) && !defined(__APPLE__)
	cjk_primary_covers_japanese = true;
#endif
	for (const char *family : cjk_families)
		for (std::string &path : cjk_fontconfig_match(family, bold))
			out.push_back({ std::move(path), {}, true });
	// Not on the list (or none of those could be read): a face that draws
	// Japanese is still the right answer, so look for one by what it can
	// render.
	for (std::string &path : cjk_fontconfig_any())
		out.push_back({ std::move(path), {}, true });
	// And if there is no Japanese at all, a real Latin font beats the embedded
	// default. Japanese text will be tofu either way, but the rest of the UI
	// stops looking like a 1996 demo. These come last and are marked, so the
	// walk knows to merge a Japanese face in (add_cjk_ui_font).
	for (std::string &path : cjk_fontconfig_sans())
		out.push_back({ std::move(path), {}, false });
}

#endif


// ---- taking the first face that comes back ---------------------------------

static bool cjk_read_file(const std::string &path, std::vector<unsigned char> &data)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	unsigned char buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		data.insert(data.end(), buf, buf + n);
	std::fclose(f);
	return !data.empty();
}

// Whether the rasteriser can read this face. Asked on Linux only, where the
// files come from whatever the distro installed; Windows and macOS hand over
// system faces that have always parsed, and are left exactly as they were.
static bool cjk_readable(const face_bytes &got)
{
#if !defined(_WIN32) && !defined(__APPLE__)
	return ui::font_parses(got.data.data(), got.data.size(), got.face);
#else
	(void)got;
	return true;
#endif
}

static bool cjk_offer_bytes(const face_offer &offer, face_bytes &out)
{
	if (offer.fetch)
		out = offer.fetch();
	else if (!cjk_read_file(offer.path, out.data))
		return false;
	return !out.data.empty();
}

// The first face the walk turns up, read once and kept to process exit: the
// atlas reads from disk on every call without a cache, and the panel
// re-rasterizes six sizes per resize. The buffer outlives every atlas
// (FontDataOwnedByAtlas = false), which the lazy bakes need.
inline const void *cjk_face_data(bool bold, size_t &bytes, int &face, float &em)
{
	static bool walked[2] = { false, false };
	static face_bytes kept[2];
	const int slot = bold ? 1 : 0;
	if (!walked[slot]) {
		walked[slot] = true;
		std::vector<face_offer> offers;
		cjk_offers(bold, offers);
		for (face_offer &offer : offers) {
			face_bytes got;
			// Only a face the rasteriser can read: handing ImGui one it cannot
			// parse is an assert, not an error return (font_check.h)
			if (cjk_offer_bytes(offer, got) && cjk_readable(got)) {
#if !defined(_WIN32) && !defined(__APPLE__)
				if (!bold)
					cjk_primary_covers_japanese = offer.japanese;
#endif
				kept[slot] = std::move(got);
				break;
			}
		}
	}
	bytes = kept[slot].data.size();
	face = kept[slot].face;
	em = kept[slot].em;
	return kept[slot].data.empty() ? nullptr : kept[slot].data.data();
}

inline const void *cjk_font_data(size_t &bytes, int &face, float &em)
{
	return cjk_face_data(false, bytes, face, em);
}

// The bold face, the same way; null when the machine has none, and the caller
// draws the regular face instead.
inline const void *cjk_bold_font_data(size_t &bytes, int &face, float &em)
{
	return cjk_face_data(true, bytes, face, em);
}

// The stb-to-em factor for one weight: panel and toolbar sizes are multiplied
// by it so text lands at em size like GDI's. 1.0 off Windows and for the
// editors, which must not move (same rasterizer both sides there).
inline float cjk_face_em(bool bold)
{
	size_t bytes = 0;
	int face = 0;
	float em = 1.0f;
	cjk_face_data(bold, bytes, face, em);
	return em;
}

#if !defined(_WIN32) && !defined(__APPLE__)
// A face that draws Japanese, whether or not it draws Latin -- the partner for
// MergeMode below, which needs the Japanese glyphs without the Latin ones.
inline const void *cjk_japanese_only_data(size_t &bytes, int &face, float &em)
{
	static bool walked = false;
	static face_bytes kept;
	if (!walked) {
		walked = true;
		for (const std::string &path : cjk_fontconfig_japanese()) {
			face_offer o;
			o.path = path;
			face_bytes got;
			if (cjk_offer_bytes(o, got) && cjk_readable(got)) {
				kept = std::move(got);
				break;
			}
		}
	}
	bytes = kept.data.size();
	face = kept.face;
	em = kept.em;
	return kept.data.empty() ? nullptr : kept.data.data();
}
#endif

// Put one CJK face into an atlas at a given size, or ImGui's built-in when the
// machine has no Japanese font. The buffer stays ours (FontDataOwnedByAtlas =
// false); `bold` without a bold face draws the regular one.
//
// One font setup for the whole program: the panel's six sizes, the window's own
// pieces and the five PC editor windows.
inline ImFont *add_cjk_font(ImFontAtlas *atlas, float px = 16.0f, bool bold = false)
{
	size_t bytes = 0;
	int face = 0;
	float em = 1.0f;
	const void *data = bold ? cjk_bold_font_data(bytes, face, em) : cjk_font_data(bytes, face, em);
	if (!data && bold) {
		size_t regular = 0;
		data = cjk_font_data(regular, face, em);
		bytes = regular;
	}
	if (data) {
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false;
		cfg.FontNo = face;             // which face of a TTC; 0 for a lone font
		cfg.GlyphRanges = ui::show_english() ? cjk_english_ranges : atlas->GetGlyphRangesJapanese();
		if (ImFont *font = atlas->AddFontFromMemoryTTF(
		        const_cast<void *>(data), int(bytes), px, &cfg))
			return font;
	}
	return atlas->AddFontDefault();
}

// The font for text the *user* reads -- the window's button strip and the
// editor windows -- as opposed to the panel's own lettering, which comes from
// panel.txt and is English on every platform.
//
// Same face as add_cjk_font(), except that a machine whose only Japanese-capable
// font is a fallback (Droid Sans Fallback: kana and kanji, no Latin) gets that
// one's glyphs merged into the Latin face. Without the merge the walk has to
// choose, and either choice loses: the CJK-only face draws the English labels
// as nothing, and a Latin face leaves （） and the kanji as tofu.
//
// English needs fullwidth punctuation; Japanese is merged when selected.
inline ImFont *add_cjk_ui_font(ImFontAtlas *atlas, float px = 16.0f, bool bold = false)
{
	ImFont *primary = add_cjk_font(atlas, px, bold);
	if (!primary)
		return primary;
#if !defined(_WIN32) && !defined(__APPLE__)
	if (cjk_primary_covers_japanese)
		return primary;              // the face already has both scripts
	size_t bytes = 0;
	int face = 0;
	float em = 1.0f;
	const void *extra = cjk_japanese_only_data(bytes, face, em);
	if (!extra)
		return primary;
	ImFontConfig cfg;
	cfg.FontDataOwnedByAtlas = false;
	cfg.FontNo = face;
	cfg.MergeMode = true;            // into the font added just above
	cfg.GlyphRanges = ui::show_english() ? cjk_fullwidth_ranges : atlas->GetGlyphRangesJapanese();
	atlas->AddFontFromMemoryTTF(const_cast<void *>(extra), int(bytes), px, &cfg);
#endif
	return primary;
}

// Call before NewFrame. Existing windows keep their fonts when language changes.
inline void ensure_cjk_ui_fonts(ImFontAtlas *atlas)
{
	if (ui::show_english()) return;
	std::vector<ImFontConfig> pending;
	for (const auto &source : atlas->Sources) {
		if (source.GlyphRanges != cjk_fullwidth_ranges && source.GlyphRanges != cjk_english_ranges) continue;
		bool merged = false;
		for (const auto &other : atlas->Sources)
			merged |= other.DstFont == source.DstFont && other.GlyphRanges == atlas->GetGlyphRangesJapanese();
		if (!merged) pending.push_back(source);
	}
	for (const auto &source : pending) {
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false;
		cfg.FontNo = source.FontNo;
		cfg.MergeMode = true;
		cfg.DstFont = source.DstFont;
		cfg.GlyphRanges = atlas->GetGlyphRangesJapanese();
		atlas->AddFontFromMemoryTTF(source.FontData, source.FontDataSize, source.SizePixels, &cfg);
	}
}

#endif // S_MU2000_UI_FONT_FILE_H
