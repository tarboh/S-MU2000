// license:BSD-3-Clause
//
// The macOS half of compat/gdi.h: the slice of GDI the panel draws through,
// implemented over CoreGraphics and CoreText.
//
// Coordinates and colours follow GDI, not CoreGraphics:
//
//   - every context handed out here has its origin top-left with y running
//     down, the way GDI works, so panel.cpp never has to think about flipping
//   - a pen of odd width is nudged half a pixel, which is what lands GDI's
//     rules and tick marks on one crisp column instead of smearing over two
//   - a filled rectangle covers [left, right) x [top, bottom), like GDI's
//
// Text is the one thing that cannot match exactly. GDI is asked for "Segoe UI"
// and macOS does not have it, so that request is answered with the system UI
// font. Glyph metrics differ slightly from the Windows build, which means
// layout that was tuned around text via panel.txt may want a nudge.

#include "compat/gdi.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum obj_kind { OBJ_BRUSH, OBJ_PEN, OBJ_FONT, OBJ_BITMAP, OBJ_DC };

} // namespace

// ---- The objects an HGDIOBJ can point at ---------------------------------
//
// These are declared in gdi.h and defined only here, so nothing outside this
// file can do anything with a handle except pass it back in.

struct gdi_object
{
	int kind;
	bool stock = false;          // a GetStockObject() one, so never freed

	explicit gdi_object(int k) : kind(k) {}
	virtual ~gdi_object() {}
};

struct gdi_brush : gdi_object
{
	COLORREF color = 0;
	bool     none  = false;      // NULL_BRUSH: fills nothing

	gdi_brush() : gdi_object(OBJ_BRUSH) {}
};

struct gdi_pen : gdi_object
{
	COLORREF color = 0;
	int      width = 1;
	bool     none  = false;      // NULL_PEN: outlines nothing

	gdi_pen() : gdi_object(OBJ_PEN) {}
};

struct gdi_font : gdi_object
{
	int         height = -12;    // GDI: negative means character height
	int         weight = FW_NORMAL;
	std::string face;
	CTFontRef   ct = nullptr;    // built once: the panel makes 3 of these

	gdi_font() : gdi_object(OBJ_FONT) {}
	~gdi_font() override { if (ct) CFRelease(ct); }
};

struct gdi_bitmap : gdi_object
{
	int               w = 0, h = 0;
	std::vector<BYTE> data;
	CGContextRef      ctx = nullptr;

	gdi_bitmap() : gdi_object(OBJ_BITMAP) {}
	~gdi_bitmap() override { if (ctx) CGContextRelease(ctx); }
};

struct gdi_dc : gdi_object
{
	CGContextRef ctx  = nullptr;
	bool         owns = false;   // only free what we created
	int          w = 0, h = 0;

	HGDIOBJ pen = nullptr, brush = nullptr, font = nullptr;
	COLORREF text = RGB(0, 0, 0);
	int      fill_mode = ALTERNATE;
	POINT    cur{ 0, 0 };

	gdi_dc() : gdi_object(OBJ_DC) {}
	~gdi_dc() override { if (owns && ctx) CGContextRelease(ctx); }
};

namespace {

// Every live DC, so GdiFlush() can reach them all. Drawing is single-threaded.
std::vector<gdi_dc *> g_dcs;

CGColorSpaceRef rgb_space()
{
	static CGColorSpaceRef s = CGColorSpaceCreateDeviceRGB();
	return s;
}

CGColorRef make_color(COLORREF c)
{
	const CGFloat comp[4] = { GetRValue(c) / 255.0, GetGValue(c) / 255.0,
	                          GetBValue(c) / 255.0, 1.0 };
	return CGColorCreate(rgb_space(), comp);
}

void set_fill(CGContextRef ctx, COLORREF c)
{
	CGContextSetRGBFillColor(ctx, GetRValue(c) / 255.0, GetGValue(c) / 255.0,
	                         GetBValue(c) / 255.0, 1.0);
}

void set_stroke(CGContextRef ctx, COLORREF c)
{
	CGContextSetRGBStrokeColor(ctx, GetRValue(c) / 255.0, GetGValue(c) / 255.0,
	                           GetBValue(c) / 255.0, 1.0);
}

// GDI's rectangles are left/top inclusive and right/bottom exclusive, which is
// exactly a CoreGraphics rect of width (right - left)
CGRect cg_rect(const RECT &r)
{
	return CGRectStandardize(CGRectMake(double(r.left), double(r.top),
	                                    double(r.right - r.left),
	                                    double(r.bottom - r.top)));
}

CGRect cg_rect(int left, int top, int right, int bottom)
{
	RECT r{ left, top, right, bottom };
	return cg_rect(r);
}

gdi_brush *brush_of(gdi_dc *dc)
{
	return (dc->brush && dc->brush->kind == OBJ_BRUSH)
	           ? static_cast<gdi_brush *>(dc->brush) : nullptr;
}

gdi_pen *pen_of(gdi_dc *dc)
{
	return (dc->pen && dc->pen->kind == OBJ_PEN)
	           ? static_cast<gdi_pen *>(dc->pen) : nullptr;
}

// Fills a path using the DC's brush and fill rule. GDI's default rule is
// ALTERNATE, so the even-odd one, and the SVG art depends on it for holes.
void fill_path(gdi_dc *dc, CGPathRef path)
{
	gdi_brush *br = brush_of(dc);
	if (!br || br->none)
		return;
	set_fill(dc->ctx, br->color);
	CGContextAddPath(dc->ctx, path);
	if (dc->fill_mode == ALTERNATE)
		CGContextEOFillPath(dc->ctx);
	else
		CGContextFillPath(dc->ctx);
}

void stroke_path(gdi_dc *dc, CGPathRef path)
{
	gdi_pen *pen = pen_of(dc);
	if (!pen || pen->none)
		return;

	CGContextSaveGState(dc->ctx);
	// GDI paints an odd-width line on exact pixel boundaries: a 1-pixel rule at
	// y = 5 covers row 5. Centred on y = 5 it would straddle rows 4 and 5, so
	// shift by half a pixel. Even widths already line up.
	if (pen->width & 1)
		CGContextTranslateCTM(dc->ctx, 0.5, 0.5);
	set_stroke(dc->ctx, pen->color);
	CGContextSetLineWidth(dc->ctx, std::max(1, pen->width));
	CGContextSetLineCap(dc->ctx, kCGLineCapButt);       // GDI's ends are square
	CGContextSetLineJoin(dc->ctx, kCGLineJoinMiter);
	CGContextAddPath(dc->ctx, path);
	CGContextStrokePath(dc->ctx);
	CGContextRestoreGState(dc->ctx);
}

// ---- Text ---------------------------------------------------------------

// A CFTypeRef that releases itself, so the CoreText plumbing below does not
// turn into a pile of manual CFRelease calls on every early return
template <typename T>
class cf_holder
{
public:
	explicit cf_holder(T p = nullptr) : m_p(p) {}
	~cf_holder() { if (m_p) CFRelease(m_p); }
	cf_holder(const cf_holder &) = delete;
	cf_holder &operator=(const cf_holder &) = delete;

	T get() const { return m_p; }
	explicit operator bool() const { return m_p != nullptr; }

private:
	T m_p;
};

CTFontRef default_font()
{
	static CTFontRef f = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 12.0, nullptr);
	return f;
}

CTFontRef font_of(gdi_dc *dc)
{
	if (auto *f = (dc->font && dc->font->kind == OBJ_FONT)
	                  ? static_cast<gdi_font *>(dc->font) : nullptr)
		if (f->ct)
			return f->ct;
	return default_font();
}

CTLineRef make_line(CTFontRef font, const std::u16string &s, COLORREF color)
{
	cf_holder<CFStringRef> str(CFStringCreateWithCharacters(
	    nullptr, reinterpret_cast<const UniChar *>(s.data()), CFIndex(s.size())));
	if (!str)
		return nullptr;

	cf_holder<CGColorRef> col(make_color(color));

	const void *keys[] = { kCTFontAttributeName, kCTForegroundColorAttributeName };
	const void *vals[] = { font, col.get() };
	cf_holder<CFDictionaryRef> attrs(CFDictionaryCreate(
	    nullptr, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks));
	if (!attrs)
		return nullptr;

	cf_holder<CFAttributedStringRef> as(
	    CFAttributedStringCreate(nullptr, str.get(), attrs.get()));
	if (!as)
		return nullptr;

	return CTLineCreateWithAttributedString(as.get());
}

double measure(CTFontRef font, const std::u16string &s, COLORREF color)
{
	cf_holder<CTLineRef> line(make_line(font, s, color));
	if (!line)
		return 0.0;
	return CTLineGetTypographicBounds(line.get(), nullptr, nullptr, nullptr);
}

std::vector<std::u16string> split_hard(const std::u16string &s)
{
	std::vector<std::u16string> out;
	std::u16string cur;
	for (char16_t c : s) {
		if (c == u'\n' || c == u'\r') {
			out.push_back(cur);
			cur.clear();
			continue;
		}
		cur.push_back(c);
	}
	out.push_back(cur);
	return out;
}

// CJK has no spaces, so wrapping on spaces alone would leave a Japanese
// sentence as one unbreakable line. GDI breaks between CJK characters, so
// treat each one as its own token.
bool breakable(char32_t cp)
{
	return (cp >= 0x2E80 && cp <= 0x9FFF) ||      // CJK radicals through unified
	       (cp >= 0xAC00 && cp <= 0xD7AF) ||      // Hangul syllables
	       (cp >= 0xF900 && cp <= 0xFAFF) ||      // CJK compatibility
	       (cp >= 0xFF00 && cp <= 0xFF60);        // fullwidth forms
}

// Splits into tokens: a run of non-space Latin is one token, each CJK
// character is one, and runs of spaces are their own so they can be dropped at
// a line break
std::vector<std::u16string> tokenize(const std::u16string &s)
{
	std::vector<std::u16string> out;
	std::u16string word;
	auto flush = [&] { if (!word.empty()) { out.push_back(word); word.clear(); } };

	for (size_t i = 0; i < s.size();) {
		char16_t c = s[i];
		char32_t cp = c;
		size_t step = 1;
		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() &&
		    s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
			cp = 0x10000 + ((char32_t(c) - 0xD800) << 10) + (char32_t(s[i + 1]) - 0xDC00);
			step = 2;
		}

		if (c == u' ' || c == u'\t') {
			flush();
			std::u16string sp;
			while (i < s.size() && (s[i] == u' ' || s[i] == u'\t')) {
				sp.push_back(s[i]);
				i++;
			}
			out.push_back(sp);
			continue;
		}
		if (breakable(cp)) {
			flush();
			out.push_back(s.substr(i, step));
			i += step;
			continue;
		}
		word.append(s, i, step);
		i += step;
	}
	flush();
	return out;
}

std::vector<std::u16string> wrap_text(const std::u16string &s, CTFontRef font,
                                      COLORREF color, double max_w)
{
	std::vector<std::u16string> out;
	if (max_w <= 0.0)
		return split_hard(s);

	for (const std::u16string &para : split_hard(s)) {
		std::u16string line;
		for (const std::u16string &tok : tokenize(para)) {
			const bool space = tok.find_first_not_of(u" \t") == std::u16string::npos;
			if (space && line.empty())
				continue;
			const std::u16string cand = line + tok;
			if (measure(font, cand, color) <= max_w || line.empty()) {
				line = cand;
				continue;
			}
			out.push_back(line);
			line = space ? std::u16string() : tok;
		}
		out.push_back(line);
	}
	return out;
}

} // namespace


// ---- Making objects ------------------------------------------------------

HBRUSH CreateSolidBrush(COLORREF color)
{
	auto *b = new gdi_brush();
	b->color = color;
	return b;
}

HPEN CreatePen(int style, int width, COLORREF color)
{
	auto *p = new gdi_pen();
	p->color = color;
	p->width = std::max(1, width);
	p->none  = (style == PS_NULL);
	return p;
}

HFONT CreateFontA(int height, int width, int escapement, int orientation,
                  int weight, DWORD italic, DWORD underline, DWORD strike_out,
                  DWORD charset, DWORD out_precision, DWORD clip_precision,
                  DWORD quality, DWORD pitch_and_family, const char *face)
{
	(void)width; (void)escapement; (void)orientation; (void)italic;
	(void)underline; (void)strike_out; (void)charset; (void)out_precision;
	(void)clip_precision; (void)quality; (void)pitch_and_family;

	auto *f = new gdi_font();
	f->height = height ? height : -12;
	f->weight = weight;
	f->face   = face ? face : "";

	const double px = std::max(1.0, double(std::abs(f->height)));

	CTFontRef base = nullptr;
	// "Segoe UI" is what panel.cpp asks for and it does not exist here. San
	// Francisco, the system UI font, is the nearest equivalent, and preferring
	// it keeps the Windows-authored panel.txt looking sensible on macOS.
	if (!f->face.empty() && f->face != "Segoe UI") {
		cf_holder<CFStringRef> name(CFStringCreateWithCString(
		    nullptr, f->face.c_str(), kCFStringEncodingUTF8));
		if (name)
			base = CTFontCreateWithName(name.get(), px, nullptr);
	}
	if (!base)
		base = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, px, nullptr);

	// FW_BOLD and up. panel.cpp uses FW_BOLD for the labels.
	if (base && f->weight >= FW_SEMIBOLD) {
		CTFontRef bold = CTFontCreateCopyWithSymbolicTraits(
		    base, px, nullptr, kCTFontTraitBold, kCTFontTraitBold);
		if (bold) {
			CFRelease(base);
			base = bold;
		}
	}
	f->ct = base;
	return f;
}

HGDIOBJ GetStockObject(int which)
{
	static gdi_brush white_brush = [] { gdi_brush b; b.color = RGB(255,255,255); b.stock = true; return b; }();
	static gdi_brush null_brush  = [] { gdi_brush b; b.none = true; b.stock = true; return b; }();
	static gdi_pen   black_pen   = [] { gdi_pen   p; p.color = RGB(0,0,0); p.stock = true; return p; }();
	static gdi_pen   white_pen   = [] { gdi_pen   p; p.color = RGB(255,255,255); p.stock = true; return p; }();
	static gdi_pen   null_pen    = [] { gdi_pen   p; p.none = true; p.stock = true; return p; }();

	switch (which) {
	// NULL_BRUSH and HOLLOW_BRUSH are the same value, so one case covers both
	case NULL_BRUSH: return &null_brush;
	case NULL_PEN:   return &null_pen;
	case BLACK_PEN:                     return &black_pen;
	case WHITE_PEN:                     return &white_pen;
	default:                            return &white_brush;
	}
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !obj)
		return nullptr;

	switch (obj->kind) {
	case OBJ_BRUSH: {
		HGDIOBJ old = dc->brush;
		dc->brush = obj;
		return old;
	}
	case OBJ_PEN: {
		HGDIOBJ old = dc->pen;
		dc->pen = obj;
		return old;
	}
	case OBJ_FONT: {
		HGDIOBJ old = dc->font;
		dc->font = obj;
		return old;
	}
	case OBJ_BITMAP: {
		// Binding a bitmap to a DC: this is how --shot gets a surface to draw
		// on. The bitmap keeps ownership of the context.
		auto *bm = static_cast<gdi_bitmap *>(obj);
		dc->ctx  = bm->ctx;
		dc->owns = false;
		dc->w = bm->w;
		dc->h = bm->h;
		return nullptr;
	}
	default:
		break;
	}
	return nullptr;
}

BOOL DeleteObject(HGDIOBJ obj)
{
	if (!obj)
		return FALSE;
	if (obj->stock)
		return TRUE;                 // stock objects are not ours to free
	delete obj;
	return TRUE;
}

// ---- Drawing -------------------------------------------------------------

int FillRect(HDC hdc, const RECT *r, HBRUSH brush)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx || !r || !brush)
		return 0;
	auto *br = static_cast<gdi_brush *>(brush);
	if (br->kind != OBJ_BRUSH || br->none)
		return 0;
	set_fill(dc->ctx, br->color);
	CGContextFillRect(dc->ctx, cg_rect(*r));
	return 1;
}

BOOL RoundRect(HDC hdc, int left, int top, int right, int bottom, int ew, int eh)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx)
		return FALSE;

	// GDI takes the corner ellipse's width and height; CoreGraphics takes a
	// corner radius
	const double rx = std::abs(ew) / 2.0, ry = std::abs(eh) / 2.0;
	const double radius = std::max(0.0, std::min(rx, ry));

	cf_holder<CGMutablePathRef> path(CGPathCreateMutable());
	CGPathAddRoundedRect(path.get(), nullptr,
	                     cg_rect(left, top, right, bottom), radius, radius);
	fill_path(dc, path.get());
	stroke_path(dc, path.get());
	return TRUE;
}

BOOL Ellipse(HDC hdc, int left, int top, int right, int bottom)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx)
		return FALSE;
	cf_holder<CGMutablePathRef> path(CGPathCreateMutable());
	CGPathAddEllipseInRect(path.get(), nullptr, cg_rect(left, top, right, bottom));
	fill_path(dc, path.get());
	stroke_path(dc, path.get());
	return TRUE;
}

BOOL Arc(HDC hdc, int left, int top, int right, int bottom,
         int xr1, int yr1, int xr2, int yr2)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx)
		return FALSE;

	const double cx = (left + right) / 2.0, cy = (top + bottom) / 2.0;
	const double rx = std::abs(right - left) / 2.0, ry = std::abs(bottom - top) / 2.0;
	if (rx <= 0.0 || ry <= 0.0)
		return FALSE;

	// GDI's Arc runs counterclockwise from the start point to the end point of
	// the inscribed ellipse. Angles are taken the mathematical way up (y
	// increasing upward), which is what makes "counterclockwise" mean what it
	// says; the points themselves are still in GDI's y-down space.
	auto angle_of = [&](int x, int y) {
		return std::atan2((cy - y) / ry, (x - cx) / rx);
	};
	const double a0 = angle_of(xr1, yr1);
	double a1 = angle_of(xr2, yr2);
	while (a1 <= a0 + 1e-9)
		a1 += 2.0 * 3.14159265358979323846;

	// Flattened rather than handed to CGContextAddArc: the context here is
	// y-down, and getting the angle signs right through that flip is easy to
	// get subtly wrong. Sampling is unambiguous.
	const double sweep = a1 - a0;
	const int steps = std::max(8, int(std::max(rx, ry) * sweep / 2.0) + 1);

	cf_holder<CGMutablePathRef> path(CGPathCreateMutable());
	for (int i = 0; i <= steps; i++) {
		const double a = a0 + sweep * i / steps;
		const double px = cx + std::cos(a) * rx;
		const double py = cy - std::sin(a) * ry;
		if (i == 0)
			CGPathMoveToPoint(path.get(), nullptr, px, py);
		else
			CGPathAddLineToPoint(path.get(), nullptr, px, py);
	}
	stroke_path(dc, path.get());
	return TRUE;
}

BOOL MoveToEx(HDC hdc, int x, int y, POINT *prev)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc)
		return FALSE;
	if (prev)
		*prev = dc->cur;
	dc->cur.x = x;
	dc->cur.y = y;
	return TRUE;
}

BOOL LineTo(HDC hdc, int x, int y)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx)
		return FALSE;

	cf_holder<CGMutablePathRef> path(CGPathCreateMutable());
	CGPathMoveToPoint(path.get(), nullptr, dc->cur.x, dc->cur.y);
	CGPathAddLineToPoint(path.get(), nullptr, x, y);
	stroke_path(dc, path.get());

	dc->cur.x = x;
	dc->cur.y = y;
	return TRUE;
}

BOOL Polygon(HDC hdc, const POINT *pts, int n)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx || !pts || n < 2)
		return FALSE;

	cf_holder<CGMutablePathRef> path(CGPathCreateMutable());
	CGPathMoveToPoint(path.get(), nullptr, pts[0].x, pts[0].y);
	for (int i = 1; i < n; i++)
		CGPathAddLineToPoint(path.get(), nullptr, pts[i].x, pts[i].y);
	CGPathCloseSubpath(path.get());

	fill_path(dc, path.get());
	stroke_path(dc, path.get());
	return TRUE;
}

BOOL PolyPolygon(HDC hdc, const POINT *pts, const INT *counts, int n)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx || !pts || !counts || n < 1)
		return FALSE;

	cf_holder<CGMutablePathRef> path(CGPathCreateMutable());
	int at = 0;
	for (int s = 0; s < n; s++) {
		const int c = counts[s];
		if (c >= 2) {
			CGPathMoveToPoint(path.get(), nullptr, pts[at].x, pts[at].y);
			for (int i = 1; i < c; i++)
				CGPathAddLineToPoint(path.get(), nullptr, pts[at + i].x, pts[at + i].y);
			CGPathCloseSubpath(path.get());
		}
		at += c;
	}
	fill_path(dc, path.get());
	stroke_path(dc, path.get());
	return TRUE;
}

BOOL Polyline(HDC hdc, const POINT *pts, int n)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx || !pts || n < 2)
		return FALSE;

	cf_holder<CGMutablePathRef> path(CGPathCreateMutable());
	CGPathMoveToPoint(path.get(), nullptr, pts[0].x, pts[0].y);
	for (int i = 1; i < n; i++)
		CGPathAddLineToPoint(path.get(), nullptr, pts[i].x, pts[i].y);
	stroke_path(dc, path.get());
	return TRUE;
}

BOOL smu_blit_premul(HDC hdc, int x, int y, int w, int h, const uint32_t *px)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx || !px || w <= 0 || h <= 0)
		return FALSE;

	// A copy, so the image does not depend on px outliving the draw
	cf_holder<CFDataRef> data(CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(px),
	                                       CFIndex(size_t(w) * size_t(h) * 4)));
	cf_holder<CGDataProviderRef> prov(CGDataProviderCreateWithCFData(data.get()));
	// The alpha and the byte order are separate enum types, so the combining
	// bitwise op is only spelled through uint32_t; see CreateCompatibleDC below.
	cf_holder<CGImageRef> img(CGImageCreate(size_t(w), size_t(h), 8, 32, size_t(w) * 4,
	                                        rgb_space(),
	                                        CGBitmapInfo(uint32_t(kCGImageAlphaPremultipliedFirst) |
	                                                     uint32_t(kCGBitmapByteOrder32Little)),
	                                        prov.get(), nullptr, false,
	                                        kCGRenderingIntentDefault));
	if (!img.get())
		return FALSE;
	// Our contexts run y downwards; CGContextDrawImage assumes y upwards, so
	// flip around the target rectangle or the picture lands upside down
	CGContextSaveGState(dc->ctx);
	CGContextTranslateCTM(dc->ctx, x, y + h);
	CGContextScaleCTM(dc->ctx, 1, -1);
	CGContextDrawImage(dc->ctx, CGRectMake(0, 0, w, h), img.get());
	CGContextRestoreGState(dc->ctx);
	return TRUE;
}

// ---- State ---------------------------------------------------------------

COLORREF SetTextColor(HDC hdc, COLORREF color)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc)
		return 0;
	const COLORREF old = dc->text;
	dc->text = color;
	return old;
}

int SetBkMode(HDC hdc, int mode)
{
	(void)hdc; (void)mode;   // text here is always transparent; see text_in()
	return TRANSPARENT;
}

int SetBkColor(HDC hdc, COLORREF color)
{
	(void)hdc; (void)color;
	return 0;
}

int SetPolyFillMode(HDC hdc, int mode)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc)
		return ALTERNATE;
	const int old = dc->fill_mode;
	dc->fill_mode = mode;
	return old;
}

int DrawTextW(HDC hdc, const wchar_t *text, int count, RECT *r, UINT flags)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc || !dc->ctx || !text || !r)
		return 0;
	if (count < 0)
		count = int(std::wcslen(text));
	if (count <= 0)
		return 0;

	// wchar_t here is UTF-32, so re-encode to the UTF-16 CoreText wants
	std::u16string u16;
	u16.reserve(size_t(count));
	for (int i = 0; i < count; i++) {
		char32_t cp = char32_t(text[i]);
		if (cp < 0x10000) {
			u16.push_back(char16_t(cp));
		} else {
			cp -= 0x10000;
			u16.push_back(char16_t(0xD800 + (cp >> 10)));
			u16.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
		}
	}

	CTFontRef font = font_of(dc);
	const double ascent  = CTFontGetAscent(font);
	const double descent = CTFontGetDescent(font);
	const double line_h  = ascent + descent;

	const double rect_w = double(r->right - r->left);
	const double rect_h = double(r->bottom - r->top);

	const bool single = (flags & DT_SINGLELINE) != 0;
	std::vector<std::u16string> lines =
	    (single || !(flags & DT_WORDBREAK))
	        ? split_hard(u16)
	        : wrap_text(u16, font, dc->text, rect_w);
	if (lines.empty())
		lines.emplace_back();

	const double block_h = line_h * double(lines.size());

	double y;
	if (flags & DT_VCENTER)
		y = r->top + (rect_h - block_h) / 2.0;
	else if (flags & DT_BOTTOM)
		y = r->bottom - block_h;
	else
		y = r->top;

	CGContextSaveGState(dc->ctx);
	if (!(flags & DT_NOCLIP))
		CGContextClipToRect(dc->ctx, cg_rect(*r));

	for (size_t i = 0; i < lines.size(); i++) {
		cf_holder<CTLineRef> line(make_line(font, lines[i], dc->text));
		if (!line)
			continue;

		const double w = CTLineGetTypographicBounds(line.get(), nullptr, nullptr, nullptr);
		double x;
		if (flags & DT_CENTER)
			x = r->left + (rect_w - w) / 2.0;
		else if (flags & DT_RIGHT)
			x = r->right - w;
		else
			x = r->left;

		CGContextSaveGState(dc->ctx);
		// CoreText lays text out with y up, so stand at the baseline and turn
		// the axis over for the draw. Without this the glyphs come out mirrored
		// in this y-down context.
		CGContextTranslateCTM(dc->ctx, x, y + ascent + line_h * double(i));
		CGContextScaleCTM(dc->ctx, 1.0, -1.0);
		CGContextSetTextMatrix(dc->ctx, CGAffineTransformIdentity);
		CTLineDraw(line.get(), dc->ctx);
		CGContextRestoreGState(dc->ctx);
	}
	CGContextRestoreGState(dc->ctx);

	return int(block_h);
}

int MultiByteToWideChar(UINT codepage, DWORD flags, const char *src, int src_len,
                        wchar_t *dst, int dst_len)
{
	(void)flags;
	if (!src)
		return 0;

	const bool nul_terminated = (src_len < 0);
	const auto *p = reinterpret_cast<const unsigned char *>(src);

	std::vector<char32_t> out;
	for (size_t i = 0; nul_terminated ? p[i] != 0 : int(i) < src_len; i++) {
		char32_t cp;
		if (codepage == CP_UTF8) {
			const unsigned char c = p[i];
			int extra = 0;
			if (c < 0x80)      { cp = c; }
			else if (c < 0xE0) { cp = c & 0x1F; extra = 1; }
			else if (c < 0xF0) { cp = c & 0x0F; extra = 2; }
			else               { cp = c & 0x07; extra = 3; }
			bool ok = true;
			for (int k = 0; k < extra; k++) {
				const unsigned char n = p[i + 1 + size_t(k)];
				if ((n & 0xC0) != 0x80) { ok = false; break; }
				cp = (cp << 6) | (n & 0x3F);
			}
			if (!ok) { cp = 0xFFFD; extra = 0; }
			i += size_t(extra);
		} else {
			cp = p[i];                      // treat anything else as Latin-1
		}
		out.push_back(cp);
	}
	if (nul_terminated)
		out.push_back(0);

	if (!dst)
		return int(out.size());
	if (int(out.size()) > dst_len)
		return 0;
	for (size_t i = 0; i < out.size(); i++)
		dst[i] = wchar_t(out[i]);
	return int(out.size());
}

// ---- Surfaces with no window --------------------------------------------

HDC CreateCompatibleDC(HDC like)
{
	(void)like;
	auto *dc = new gdi_dc();
	dc->pen   = GetStockObject(BLACK_PEN);      // GDI's initial DC state
	dc->brush = GetStockObject(WHITE_BRUSH);
	g_dcs.push_back(dc);
	return dc;
}

HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO *info, UINT usage,
                         void **bits, void *section, DWORD offset)
{
	(void)hdc; (void)usage; (void)section; (void)offset;
	if (!info)
		return nullptr;

	const int w = int(info->bmiHeader.biWidth);
	const int h = std::abs(int(info->bmiHeader.biHeight));
	if (w <= 0 || h <= 0)
		return nullptr;

	auto *bm = new gdi_bitmap();
	bm->w = w;
	bm->h = h;
	bm->data.assign(size_t(w) * size_t(h) * 4, 0);

	// kCGImageAlphaNoneSkipFirst with little-endian byte order puts the bytes
	// down as B, G, R, X, which is the order GDI uses for a 32-bit BI_RGB DIB
	// and the order ui::write_png expects. The unused byte is not read as
	// alpha, so a zero there is fine.
	bm->ctx = CGBitmapContextCreate(bm->data.data(), size_t(w), size_t(h), 8,
	                                size_t(w) * 4, rgb_space(),
	                                CGBitmapInfo(uint32_t(kCGImageAlphaNoneSkipFirst) |
	                                             uint32_t(kCGBitmapByteOrder32Little)));
	if (!bm->ctx) {
		delete bm;
		return nullptr;
	}

	// A bitmap context starts with its origin bottom-left; GDI starts top-left.
	// Turn it over once here so every DC this file hands out is y-down.
	CGContextTranslateCTM(bm->ctx, 0, double(h));
	CGContextScaleCTM(bm->ctx, 1, -1);

	if (bits)
		*bits = bm->data.data();
	return bm;
}

void GdiFlush(void)
{
	for (gdi_dc *dc : g_dcs)
		if (dc->ctx)
			CGContextFlush(dc->ctx);
}

BOOL DeleteDC(HDC hdc)
{
	gdi_dc *dc = static_cast<gdi_dc *>(hdc);
	if (!dc)
		return FALSE;
	g_dcs.erase(std::remove(g_dcs.begin(), g_dcs.end(), dc), g_dcs.end());
	delete dc;
	return TRUE;
}

void *smu_gdi_wrap_view_context(void *cg_context, int w, int h)
{
	if (!cg_context)
		return nullptr;

	auto *dc = new gdi_dc();
	dc->ctx  = static_cast<CGContextRef>(cg_context);
	dc->owns = false;      // AppKit's context, AppKit frees it
	dc->w = w;
	dc->h = h;
	dc->pen   = GetStockObject(BLACK_PEN);
	dc->brush = GetStockObject(WHITE_BRUSH);

	// Nothing to flip: a flipped NSView already has its origin top-left.
	g_dcs.push_back(dc);
	return dc;
}
