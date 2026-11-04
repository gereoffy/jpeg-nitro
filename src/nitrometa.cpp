// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrometa: Windows metafiles (see nitrometa.h). A player of the GDI records of a WMF or an EMF
// keeps the device context's state (the mapping of logical to device coordinates, the selected
// pen, brush and font, the clip, the saved states) and turns every drawing record into items of a
// display list in the metafile's device space: filled and stroked paths, bitmaps, hatches and
// clips; text as the outlines of its glyphs (nitro_font's faces), so that a drawn item needs no
// font and no lock. The list is drawn by plutovg at any scale: a stroke narrower than a pixel is
// drawn one pixel wide, as GDI draws its cosmetic pens.
//
// [MS-WMF] and [MS-EMF] are the references; the conventions are GDI's as the metafiles' writers
// (Visio, Office, CorelDRAW, printer drivers) rely on them: a WMF's bounding box or window is
// its frame, the window stretched onto it when the two differ; an EMF's frame is rclFrame.
#include "nitrometa.h"
#include "nitro_font.h"

#include <plutovg.h>

#include <algorithm>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846   // (not in mingw's strict cmath)
#endif
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI   // (its PS_* and BS_* macros: the GDI constants below are our own)
#include <windows.h>   // MultiByteToWideChar: Windows has no iconv
#else
#include <iconv.h>
#endif

namespace {

// --- the display list --------------------------------------------------------

using PathP = std::shared_ptr<plutovg_path_t>;
PathP newPath() { return PathP(plutovg_path_create(), plutovg_path_destroy); }

struct Surface {
    plutovg_surface_t *s = nullptr;
    ~Surface() { plutovg_surface_destroy(s); }
};
using SurfP = std::shared_ptr<Surface>;

struct Clip {
    PathP path;
    bool evenodd = false;
};

struct Item {
    enum Kind : uint8_t { FILL, STROKE, IMAGE, HATCH, CLIP } kind = FILL;
    bool evenodd = false, tiled = false, gradient = false;
    uint8_t cap = PLUTOVG_LINE_CAP_ROUND, join = PLUTOVG_LINE_JOIN_ROUND, hatch = 0;
    uint32_t rgb = 0, rgb2 = 0;   // 0xRRGGBB: the fill, the stroke, the hatch's lines; a gradient's two ends
    float opacity = 1;
    float width = 0;          // STROKE: in device units, 0 a hairline
    std::vector<float> dash;  // STROKE: in units of max(width, hairline)
    PathP path;               // FILL, STROKE, HATCH; IMAGE: the rectangle drawn in
    SurfP img;                // IMAGE; FILL with a pattern brush (tiled)
    plutovg_matrix_t m{};     // IMAGE, pattern: its pixels to device
    float g[4] = {0, 0, 0, 0};   // gradient: from (x, y) to (x, y)
    std::vector<Clip> clips;  // CLIP: the clips in force from here (none: unclipped)
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
};

}   // namespace

struct nitrometa {
    std::vector<Item> items;
    double fx0 = 0, fy0 = 0, fx1 = 0, fy1 = 0;   // the frame in device units
    double unit = 72.0 / 96;                       // points per device unit
    int background = 0;
};

namespace {

// --- little-endian reads, all bounds-checked (a damaged file reads zeros) ------

struct Bytes {
    const uint8_t *p = nullptr;
    size_t n = 0;
    bool has(size_t o, size_t k) const { return o <= n && k <= n - o; }
    uint8_t u8(size_t o) const { return o < n ? p[o] : 0; }
    uint16_t u16(size_t o) const { return has(o, 2) ? (uint16_t)(p[o] | p[o + 1] << 8) : 0; }
    int16_t i16(size_t o) const { return (int16_t)u16(o); }
    uint32_t u32(size_t o) const {
        return has(o, 4) ? (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24 : 0;
    }
    int32_t i32(size_t o) const { return (int32_t)u32(o); }
    float f32(size_t o) const {
        uint32_t v = u32(o);
        float f;
        memcpy(&f, &v, 4);
        return std::isfinite(f) && std::fabs(f) < 1e9f ? f : 0;
    }
    Bytes sub(size_t o, size_t k) const {
        if (o > n) return {p + n, 0};
        return {p + o, std::min(k, n - o)};
    }
};

// --- matrices: x' = a x + c y + e, y' = b x + d y + f (plutovg's) ---------------

struct M {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    static M translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static M scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
    static M rotate(double rad) { return {std::cos(rad), std::sin(rad), -std::sin(rad), std::cos(rad), 0, 0}; }
    double det() const { return a * d - b * c; }
    void map(double x, double y, double &ox, double &oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    plutovg_matrix_t pv() const { return {(float)a, (float)b, (float)c, (float)d, (float)e, (float)f}; }
};
M mul(const M &first, const M &then) {   // first, then then
    return {then.a * first.a + then.c * first.b, then.b * first.a + then.d * first.b, then.a * first.c + then.c * first.d,
            then.b * first.c + then.d * first.d, then.a * first.e + then.c * first.f + then.e,
            then.b * first.e + then.d * first.f + then.f};
}

uint32_t colorref(uint32_t v) { return (v & 0xff) << 16 | (v & 0xff00) | (v >> 16 & 0xff); }   // 0x00BBGGRR to 0xRRGGBB

// --- DIBs (BITMAPINFOHEADER / BITMAPCOREHEADER, palette, pixels) to premultiplied ARGB surfaces -------

// RLE8 / RLE4 to one byte per pixel, bottom-up rows as stored
bool unrle(Bytes src, int w, int h, int bpp, std::vector<uint8_t> &out) {
    out.assign((size_t)w * h, 0);
    size_t o = 0;
    int x = 0, y = 0;
    while (y < h && o + 1 < src.n) {
        unsigned count = src.u8(o), val = src.u8(o + 1);
        o += 2;
        if (count) {   // a run
            for (unsigned i = 0; i < count && x < w; i++, x++) out[(size_t)y * w + x] = bpp == 8 ? val : (i & 1 ? val & 15 : val >> 4);
        } else if (val == 0) {   // end of line
            x = 0;
            y++;
        } else if (val == 1) {
            return true;
        } else if (val == 2) {   // delta
            x += src.u8(o);
            y += src.u8(o + 1);
            o += 2;
        } else {   // absolute
            unsigned bytes = bpp == 8 ? val : (val + 1) / 2;
            for (unsigned i = 0; i < val && x < w; i++, x++)
                out[(size_t)y * w + x] = bpp == 8 ? src.u8(o + i) : (i & 1 ? src.u8(o + i / 2) & 15 : src.u8(o + i / 2) >> 4);
            o += (bytes + 1) & ~1u;
        }
    }
    return true;
}

// where a DIB's rows lie: the source coordinates of the DIB records (StretchDIBits, SetDIBitsToDevice)
// have their origin at a bottom-up DIB's lower-left corner
struct DibPlace {
    bool bottomUp = false;
    int height = 0;   // the DIB's height (its header's)
    int top = 0;      // the first decoded row among the DIB's (top-down): a band of SetDIBitsToDevice, short pixel data
};

// the pixels of one bitmap, and of all of a document's together (RLE packs 64 Mpixels into half a
// megabyte: a file of a few megabytes could hold gigabytes of surfaces)
const size_t kMaxBitmapPixels = 64u << 20, kDocumentPixels = 256u << 20;

// bits: the pixels when the record gives them apart from the header (else they follow the palette);
// alpha: a 32-bit DIB's alpha channel counts (AlphaBlend), else it is opaque as GDI draws it;
// budget: the document's pixels left (taken from); startScan, scans: SetDIBitsToDevice's band,
// the rows the bits hold (scans 0: all that are there)
SurfP decodeDib(Bytes hdr, Bytes bits, bool alpha, size_t &budget, DibPlace *place = nullptr, uint32_t startScan = 0, uint32_t scans = 0) {
    uint32_t hs = hdr.u32(0);
    int w, h, bpp;
    uint32_t comp = 0, used = 0;
    size_t palOff, palEntry;
    if (hs == 12) {   // BITMAPCOREHEADER
        w = hdr.u16(4);
        h = hdr.i16(6);
        bpp = hdr.u16(10);
        palOff = 12;
        palEntry = 3;
    } else if (hs >= 40 && hs <= 1024) {
        w = hdr.i32(4);
        h = hdr.i32(8);
        bpp = hdr.u16(14);
        comp = hdr.u32(16);
        used = hdr.u32(32);
        palOff = hs;
        palEntry = 4;
    } else
        return nullptr;
    bool topDown = h < 0;
    if (topDown) h = -h;
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || (size_t)w * h > kMaxBitmapPixels) return nullptr;
    if (place) place->bottomUp = !topDown, place->height = h;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return nullptr;
    if (comp != 0 && comp != 1 && comp != 2 && comp != 3) return nullptr;   // BI_RGB, BI_RLE8, BI_RLE4, BI_BITFIELDS
    uint32_t masks[3] = {0, 0, 0};
    if (comp == 3) {
        if (bpp != 16 && bpp != 32) return nullptr;
        size_t mo = hs >= 52 ? 40 : hs;
        for (int i = 0; i < 3; i++) masks[i] = hdr.u32(mo + 4 * i);
        if (hs < 52) palOff = hs + 12;
    } else if (bpp == 16) {
        masks[0] = 0x7c00, masks[1] = 0x3e0, masks[2] = 0x1f;
    } else if (bpp == 32) {
        masks[0] = 0xff0000, masks[1] = 0xff00, masks[2] = 0xff;
    }
    size_t entries = bpp <= 8 ? (used && used <= (1u << bpp) ? used : 1u << bpp) : used;
    if (entries > 256) entries = 256;
    uint32_t pal[256];
    for (size_t i = 0; i < entries && i < 256; i++) {
        size_t o = palOff + i * palEntry;
        pal[i] = 0xff000000u | (uint32_t)hdr.u8(o + 2) << 16 | (uint32_t)hdr.u8(o + 1) << 8 | hdr.u8(o);
    }
    for (size_t i = entries; i < 256; i++) pal[i] = 0xff000000u;
    if (!bits.p || !bits.n) bits = hdr.sub(palOff + entries * palEntry, hdr.n);
    size_t stride = ((size_t)w * bpp + 31) / 32 * 4;
    std::vector<uint8_t> rle;
    int rows = h, top = 0;   // the rows decoded, and the first of them among the DIB's (top-down)
    if (comp == 1 || comp == 2) {
        if ((comp == 1) != (bpp == 8) || (comp == 2) != (bpp == 4)) return nullptr;
        unrle(bits, w, h, bpp, rle);
        bpp = 8;
        stride = (size_t)w;
        bits = {rle.data(), rle.size()};
    } else {   // the rows there are drawn in their place: a band (scans rows from startScan, the first stored row of a
        if (bits.n < stride || startScan >= (uint32_t)h) return nullptr;   // bottom-up DIB its bottom one), or short pixel data
        rows = (int)std::min<size_t>(bits.n / stride, (size_t)h - startScan);
        if (scans && scans < (uint32_t)rows) rows = (int)scans;
        top = topDown ? (int)startScan : h - (int)startScan - rows;
    }
    if (place) place->top = top;
    int shift[3], bitsOf[3];
    uint32_t low[3];   // the mask shifted down (a mask of all 32 bits: 1 << 32 is no number)
    for (int i = 0; i < 3; i++) {
        uint32_t m = masks[i];
        shift[i] = 0;
        while (m && !(m & 1)) m >>= 1, shift[i]++;
        bitsOf[i] = 0;
        while (m & 1) m >>= 1, bitsOf[i]++;
        low[i] = bitsOf[i] >= 32 ? ~0u : (1u << bitsOf[i]) - 1;
    }
    auto comp8 = [&](uint32_t v, int i) -> uint32_t {
        if (!bitsOf[i]) return 0;
        uint32_t c = v >> shift[i] & low[i];
        return bitsOf[i] >= 8 ? c >> (bitsOf[i] - 8) : c * 255 / low[i];
    };
    if ((size_t)w * rows > budget) return nullptr;
    budget -= (size_t)w * rows;
    auto surf = std::make_shared<Surface>();
    surf->s = plutovg_surface_create(w, rows);
    if (!surf->s) return nullptr;
    uint8_t *data = plutovg_surface_get_data(surf->s);
    int dstride = plutovg_surface_get_stride(surf->s);
    for (int y = 0; y < rows; y++) {
        const uint8_t *row = bits.p + stride * (topDown ? y : rows - 1 - y);
        uint32_t *dst = (uint32_t *)(data + (size_t)dstride * y);
        for (int x = 0; x < w; x++) {
            uint32_t px;
            switch (bpp) {
            case 1: px = pal[row[x >> 3] >> (7 - (x & 7)) & 1]; break;
            case 4: px = pal[x & 1 ? row[x >> 1] & 15 : row[x >> 1] >> 4]; break;
            case 8: px = pal[row[x]]; break;
            case 16: {
                uint32_t v = row[x * 2] | row[x * 2 + 1] << 8;
                px = 0xff000000u | comp8(v, 0) << 16 | comp8(v, 1) << 8 | comp8(v, 2);
                break;
            }
            case 24: px = 0xff000000u | (uint32_t)row[x * 3 + 2] << 16 | (uint32_t)row[x * 3 + 1] << 8 | row[x * 3]; break;
            default: {
                uint32_t v = (uint32_t)row[x * 4] | (uint32_t)row[x * 4 + 1] << 8 | (uint32_t)row[x * 4 + 2] << 16 | (uint32_t)row[x * 4 + 3] << 24;
                uint32_t r = comp8(v, 0), g = comp8(v, 1), b = comp8(v, 2), a = alpha ? v >> 24 : 255;
                if (a != 255) r = r * a / 255, g = g * a / 255, b = b * a / 255;
                px = a << 24 | r << 16 | g << 8 | b;
            }
            }
            dst[x] = px;
        }
    }
    return surf;
}

// a WMF's device-dependent bitmap (BITMAP16 and its bits): monochrome ones, and the direct-colour ones
SurfP decodeDdb(Bytes b, uint32_t fg, uint32_t bg, size_t &budget) {
    int w = b.i16(2), h = b.i16(4), wb = b.u16(6), bpp = b.u8(9);
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || (size_t)w * h > kMaxBitmapPixels) return nullptr;
    if (bpp != 1 && bpp != 24 && bpp != 32) return nullptr;
    if ((size_t)wb * h + 10 > b.n || (size_t)wb < ((size_t)w * bpp + 7) / 8) return nullptr;
    if ((size_t)w * h > budget) return nullptr;
    budget -= (size_t)w * h;
    auto surf = std::make_shared<Surface>();
    surf->s = plutovg_surface_create(w, h);
    if (!surf->s) return nullptr;
    uint8_t *data = plutovg_surface_get_data(surf->s);
    int dstride = plutovg_surface_get_stride(surf->s);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = b.p + 10 + (size_t)wb * y;   // (top-down)
        uint32_t *dst = (uint32_t *)(data + (size_t)dstride * y);
        for (int x = 0; x < w; x++) {
            if (bpp == 1) dst[x] = 0xff000000u | (row[x >> 3] >> (7 - (x & 7)) & 1 ? bg : fg);
            else dst[x] = 0xff000000u | (uint32_t)row[x * bpp / 8 + 2] << 16 | (uint32_t)row[x * bpp / 8 + 1] << 8 | row[x * bpp / 8];
        }
    }
    return surf;
}

// --- the 8-bit text of a font's character set to code points ------------------

unsigned codepage(int charset) {
    switch (charset) {
    case 128: return 932;    // SHIFTJIS
    case 129: return 949;    // HANGUL
    case 130: return 1361;   // JOHAB
    case 134: return 936;    // GB2312
    case 136: return 950;    // CHINESEBIG5
    case 161: return 1253;   // GREEK
    case 162: return 1254;   // TURKISH
    case 163: return 1258;   // VIETNAMESE
    case 177: return 1255;   // HEBREW
    case 178: return 1256;   // ARABIC
    case 186: return 1257;   // BALTIC
    case 204: return 1251;   // RUSSIAN
    case 222: return 874;    // THAI
    case 238: return 1250;   // EASTEUROPE
    case 255: return 437;    // OEM
    case 77: return 10000;   // MAC
    default: return 1252;    // ANSI, DEFAULT
    }
}

void decodeText(const uint8_t *s, size_t n, int charset, std::vector<uint32_t> &out) {
    out.clear();
    if (charset == 2) {   // SYMBOL: the font's own code points (its private use area, see symbolGlyph)
        for (size_t i = 0; i < n; i++) out.push_back(s[i]);
        return;
    }
    unsigned cp = codepage(charset);
#ifdef _WIN32
    int m = MultiByteToWideChar(cp, 0, (const char *)s, (int)n, nullptr, 0);
    if (m > 0) {
        std::vector<wchar_t> wide((size_t)m);
        MultiByteToWideChar(cp, 0, (const char *)s, (int)n, wide.data(), m);
        for (int i = 0; i < m; i++) {
            uint32_t c = wide[(size_t)i];
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < m && wide[(size_t)i + 1] >= 0xdc00 && wide[(size_t)i + 1] < 0xe000)
                c = 0x10000 + ((c - 0xd800) << 10) + (wide[(size_t)++i] - 0xdc00);
            out.push_back(c);
        }
        return;
    }
#else
    // the converters stay open (a file of thousands of text records); under nitro_font's lock, as nitrometa_load runs
    static struct { unsigned cp; iconv_t cd; } cache[8];
    static unsigned cached = 0;
    iconv_t cd = (iconv_t)-1;
    bool known = false;   // (a code page iconv has no converter for is remembered too: not tried again per text)
    for (unsigned i = 0; i < cached && !known; i++)
        if (cache[i].cp == cp) cd = cache[i].cd, known = true;
    if (!known) {
        char name[16];
        snprintf(name, sizeof name, cp == 10000 ? "MACINTOSH" : "CP%u", cp);
        cd = iconv_open("UTF-8", name);
        if (cached < 8) cache[cached++] = {cp, cd};
        else if (cd != (iconv_t)-1) iconv_close(cd), cd = (iconv_t)-1;
    }
    if (cd != (iconv_t)-1) {
        std::string utf8(n * 4 + 4, '\0');
        char *in = (char *)s, *o = &utf8[0];
        size_t inLeft = n, outLeft = utf8.size();
        iconv(cd, nullptr, nullptr, nullptr, nullptr);
        bool ok = iconv(cd, &in, &inLeft, &o, &outLeft) != (size_t)-1;
        if (ok) {
            utf8.resize(utf8.size() - outLeft);
            for (size_t i = 0; i < utf8.size();) out.push_back(nf_utf8_next(utf8, i));
            return;
        }
    }
#endif
    for (size_t i = 0; i < n; i++) out.push_back(cp == 1252 && s[i] >= 0x80 && s[i] < 0xa0 ? '?' : s[i]);   // (Latin-1)
}

// the Symbol font's letters as Unicode (for a face without the private-use mapping of Windows's Symbol)
uint32_t symbolUnicode(uint32_t c) {
    static const uint16_t upper[26] = {0x391, 0x392, 0x3a7, 0x394, 0x395, 0x3a6, 0x393, 0x397, 0x399, 0x3d1, 0x39a, 0x39b, 0x39c,
                                       0x39d, 0x39f, 0x3a0, 0x398, 0x3a1, 0x3a3, 0x3a4, 0x3a5, 0x3c2, 0x3a9, 0x39e, 0x3a8, 0x396};
    static const uint16_t lower[26] = {0x3b1, 0x3b2, 0x3c7, 0x3b4, 0x3b5, 0x3c6, 0x3b3, 0x3b7, 0x3b9, 0x3d5, 0x3ba, 0x3bb, 0x3bc,
                                       0x3bd, 0x3bf, 0x3c0, 0x3b8, 0x3c1, 0x3c3, 0x3c4, 0x3c5, 0x3d6, 0x3c9, 0x3be, 0x3c8, 0x3b6};
    if (c >= 'A' && c <= 'Z') return upper[c - 'A'];
    if (c >= 'a' && c <= 'z') return lower[c - 'a'];
    switch (c) {
    case 0xa2: return 0x2032;
    case 0xa3: return 0x2264;
    case 0xa5: return 0x221e;
    case 0xa6: return 0x192;
    case 0xa7: return 0x2663;
    case 0xa8: return 0x2666;
    case 0xa9: return 0x2665;
    case 0xaa: return 0x2660;
    case 0xab: return 0x2194;
    case 0xac: return 0x2190;
    case 0xad: return 0x2191;
    case 0xae: return 0x2192;
    case 0xaf: return 0x2193;
    case 0xb0: return 0xb0;
    case 0xb1: return 0xb1;
    case 0xb2: return 0x2033;
    case 0xb3: return 0x2265;
    case 0xb4: return 0xd7;
    case 0xb5: return 0x221d;
    case 0xb6: return 0x2202;
    case 0xb7: return 0x2022;
    case 0xb8: return 0xf7;
    case 0xb9: return 0x2260;
    case 0xba: return 0x2261;
    case 0xbb: return 0x2248;
    case 0xbc: return 0x2026;
    case 0xd0: return 0x2220;
    case 0xd1: return 0x2207;
    case 0xd5: return 0x220f;
    case 0xd6: return 0x221a;
    case 0xd7: return 0x22c5;
    case 0xe5: return 0x2211;
    case 0xf2: return 0x222b;
    default: return c;
    }
}

// --- the device context -------------------------------------------------------

enum { PS_SOLID, PS_DASH, PS_DOT, PS_DASHDOT, PS_DASHDOTDOT, PS_NULL, PS_INSIDEFRAME, PS_USERSTYLE, PS_ALTERNATE };
enum { BS_SOLID, BS_NULL, BS_HATCHED, BS_PATTERN, BS_INDEXED, BS_DIBPATTERN, BS_DIBPATTERNPT, BS_PATTERN8X8, BS_DIBPATTERN8X8 };
enum { MM_TEXT = 1, MM_LOMETRIC, MM_HIMETRIC, MM_LOENGLISH, MM_HIENGLISH, MM_TWIPS, MM_ISOTROPIC, MM_ANISOTROPIC };
enum { RGN_AND = 1, RGN_OR, RGN_XOR, RGN_DIFF, RGN_COPY };
const uint32_t SRCCOPY = 0x00cc0020, SRCPAINT = 0x00ee0086, SRCAND = 0x008800c6, NOTSRCCOPY = 0x00330008,
               PATCOPY = 0x00f00021, BLACKNESS = 0x00000042, WHITENESS = 0x00ff0062, PATINVERT = 0x005a0049;

struct Pen {
    Pen() = default;
    Pen(uint32_t style, float width, uint32_t rgb) : style(style), width(width), rgb(rgb) {}
    uint32_t style = PS_SOLID;   // the whole style word (end cap, join and PS_GEOMETRIC bits too)
    float width = 0;             // logical units; 0 (or a cosmetic pen) one device pixel
    uint32_t rgb = 0;
    std::vector<float> dash;     // PS_USERSTYLE, in units of the width
    bool null() const { return (style & 15) == PS_NULL; }
};
struct Brush {
    Brush() = default;
    Brush(int style, uint32_t rgb = 0) : style(style), rgb(rgb) {}
    int style = BS_SOLID;
    uint32_t rgb = 0xffffff;
    int hatch = 0;
    SurfP pattern;
    bool null() const { return style == BS_NULL; }
};
struct Font {
    std::string face;   // UTF-8
    float height = 0;   // logical units: < 0 the em, > 0 the cell (ascent + descent)
    int escapement = 0, weight = 400, charset = 0, family = 0;
    bool italic = false, underline = false, strike = false;
};
struct Obj {
    enum Type { NONE, PEN, BRUSH, FONT, REGION, PALETTE } type = NONE;
    Pen pen;
    Brush brush;
    Font font;
    PathP region;   // device space
    std::vector<uint32_t> palette;
};

struct DC {
    int mapMode = MM_TEXT;
    double wox = 0, woy = 0, wex = 1, wey = 1, vox = 0, voy = 0, vex = 1, vey = 1;
    bool vextSet = false;   // a viewport extent set (else the window maps 1 : 1)
    M world;                // EMF
    Pen pen;
    Brush brush;
    Font font;
    std::vector<uint32_t> palette;
    uint32_t textRgb = 0, bkRgb = 0xffffff;
    int bkMode = 2, fillMode = 1, rop2 = 13, textAlign = 0, arcDir = 1, stretchMode = 0;   // (OPAQUE, ALTERNATE, R2_COPYPEN, AD_COUNTERCLOCKWISE)
    float miter = 10;
    std::vector<Clip> clips;
    double cx = 0, cy = 0;     // the current position (logical)
    double box = 0, boy = 0;   // the brush origin (device)
};

const size_t kMaxItems = 4u << 20, kMaxPoints = 1u << 22;

class Player {
public:
    Player(nitrometa &out, bool emf) : out(out), emf(emf) {
        dc.font.height = -11;   // (the system font, around 11 pixels)
    }

    // --- the mapping ---
    double dpi = 96;        // device pixels per inch (the frame's size)
    double yScale = 1;      // a device y unit in x units (an EMF reference device of different x and y resolutions)
    bool placeable = false;
    // a WMF without a bounding box: its frame is the window in force when it starts drawing (Visio
    // sets a page-sized one first, then the picture's own)
    bool haveWindow = false, drawn = false;
    double ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;

    M matrix() const {
        double sx = 1, sy = 1;
        switch (dc.mapMode) {
        case MM_LOMETRIC: sx = dpi / 254, sy = -sx; break;
        case MM_HIMETRIC: sx = dpi / 2540, sy = -sx; break;
        case MM_LOENGLISH: sx = dpi / 100, sy = -sx; break;
        case MM_HIENGLISH: sx = dpi / 1000, sy = -sx; break;
        case MM_TWIPS: sx = dpi / 1440, sy = -sx; break;
        case MM_ISOTROPIC:
        case MM_ANISOTROPIC:
            if (dc.vextSet) {
                sx = dc.wex ? dc.vex / dc.wex : 1;
                sy = dc.wey ? dc.vey / dc.wey : 1;
            } else {   // (a metafile's window maps 1 : 1 until it sets a viewport)
                sx = 1;
                sy = dc.wex * dc.wey < 0 ? -1 : 1;
            }
            if (dc.mapMode == MM_ISOTROPIC) {
                double s = std::min(std::fabs(sx), std::fabs(sy));
                sx = std::copysign(s, sx);
                sy = std::copysign(s, sy);
            }
            break;
        default: break;
        }
        M page{sx, 0, 0, sy, dc.vox - dc.wox * sx, dc.voy - dc.woy * sy};
        M m = mul(dc.world, page);
        if (yScale != 1) m = mul(m, M::scale(1, yScale));
        return m;
    }
    void windowSet() {   // a window extent or origin set: the frame of a WMF without a bounding box
        if (placeable || emf || drawn) return;
        M m = matrix();
        double x[2], y[2];
        m.map(dc.wox, dc.woy, x[0], y[0]);
        m.map(dc.wox + dc.wex, dc.woy + dc.wey, x[1], y[1]);
        double x0 = std::min(x[0], x[1]), y0 = std::min(y[0], y[1]), x1 = std::max(x[0], x[1]), y1 = std::max(y[0], y[1]);
        if (!(x1 > x0) || !(y1 > y0)) return;
        ux0 = x0, uy0 = y0, ux1 = x1, uy1 = y1;
        haveWindow = true;
    }

    // --- objects ---
    std::vector<Obj> objs;
    void create(Obj &&o, long index = -1) {   // WMF: the first free slot; EMF: the record's index
        if (index < 0) {
            for (size_t i = 0; i < objs.size(); i++)
                if (objs[i].type == Obj::NONE) {
                    objs[i] = std::move(o);
                    return;
                }
            if (objs.size() < 65536) objs.push_back(std::move(o));
            return;
        }
        if (index >= 65536) return;
        if ((size_t)index >= objs.size()) objs.resize((size_t)index + 1);
        objs[(size_t)index] = std::move(o);
    }
    void deleteObject(uint32_t i) {
        if (i < objs.size()) objs[i] = Obj();
    }
    void select(uint32_t i) {
        if (i & 0x80000000u) {   // a stock object
            switch (i & 0x7fffffff) {
            case 0: dc.brush = Brush{BS_SOLID, 0xffffff}; break;
            case 1: dc.brush = Brush{BS_SOLID, 0xc0c0c0}; break;
            case 2: dc.brush = Brush{BS_SOLID, 0x808080}; break;
            case 3: dc.brush = Brush{BS_SOLID, 0x404040}; break;
            case 4: dc.brush = Brush{BS_SOLID, 0x000000}; break;
            case 5: dc.brush = Brush{BS_NULL}; break;
            case 6: dc.pen = Pen{PS_SOLID, 0, 0xffffff}; break;
            case 7: dc.pen = Pen{PS_SOLID, 0, 0x000000}; break;
            case 8: dc.pen = Pen{PS_NULL, 0, 0}; break;
            case 10: case 11: case 12: case 13: case 14: case 16: case 17: dc.font = Font(), dc.font.height = -11; break;
            case 18: dc.brush = Brush{BS_SOLID, 0xffffff}; break;   // DC_BRUSH
            case 19: dc.pen = Pen{PS_SOLID, 0, 0x000000}; break;    // DC_PEN
            default: break;
            }
            return;
        }
        if (i >= objs.size()) return;
        const Obj &o = objs[i];
        switch (o.type) {
        case Obj::PEN: dc.pen = o.pen; break;
        case Obj::BRUSH: dc.brush = o.brush; break;
        case Obj::FONT: dc.font = o.font; break;
        case Obj::PALETTE: dc.palette = o.palette; break;
        case Obj::REGION: dc.clips.clear(), dc.clips.push_back({o.region, false}); break;   // (WMF SelectObject of a region)
        default: break;
        }
    }
    uint32_t color(uint32_t v) const {   // a COLORREF: RGB, or a palette index
        if ((v >> 24) == 1) return (v & 0xffff) < dc.palette.size() ? dc.palette[v & 0xffff] : 0;
        return colorref(v);
    }

    // --- state ---
    nitrometa &out;
    bool emf;
    DC dc;
    std::vector<DC> saved;
    void saveDC() {
        if (saved.size() < 256) saved.push_back(dc);
    }
    void restoreDC(int n) {   // negative: relative; positive: an index of the saved ones (1 the first)
        size_t k = n < 0 ? (size_t)(-(long long)n) : saved.size() >= (size_t)n && n > 0 ? saved.size() - (size_t)n + 1 : 0;
        if (!k || k > saved.size()) return;
        dc = saved[saved.size() - k];
        saved.resize(saved.size() - k);
    }

    // path construction (EMF BeginPath .. EndPath), in device space
    bool inPath = false, pathOpen = false;
    PathP path;

    // a WMF's PostScript version (Office's): GDI draws a shape's gradient or picture XORed, the
    // shape black, the same XORed again; a PostScript printer skips the GDI-only records (between
    // POSTSCRIPT_IGNORE escapes) and clips to the shape (BEGIN_PATH .. END_PATH, CLIP_TO_PATH).
    // Played that way when the file clips so: XOR can't be drawn here
    bool psClip = false, psIgnore = false, psOpen = false;
    PathP psPath;                                 // the shapes from BEGIN_PATH to END_PATH (device space)
    bool psEvenodd = true;
    std::vector<std::vector<Clip>> psSaved;       // CLIP_SAVE's

    // a transparent bitmap drawn as two blits: the mask (SRCAND), then the picture (SRCPAINT); or
    // the other way round, a 1-bit mask ORed (SRCPAINT: white where the picture shows), then the
    // picture ANDed (SRCAND: white where it does not). The first one waits for the second (and is
    // drawn alone before any other record: flushMask)
    SurfP mask, orMask;
    double maskRect[4] = {0, 0, 0, 0}, orRect[4] = {0, 0, 0, 0};
    size_t pixelBudget = kDocumentPixels;   // the bitmaps' pixels left to decode

    // --- the display list ---
    void addItem(Item &&it) {
        if (psIgnore && it.kind != Item::CLIP) return;
        drawn = true;
        if (it.kind != Item::CLIP && !(std::fabs(it.bx0) < 1e15f && std::fabs(it.by0) < 1e15f && std::fabs(it.bx1) < 1e15f && std::fabs(it.by1) < 1e15f))
            return;   // (a damaged file's coordinates beyond any view, or not numbers at all)
        if (out.items.size() < kMaxItems) out.items.push_back(std::move(it));
    }
    void setWorld(const M &w) {   // (a damaged file's transforms grow without bound: back to none)
        bool sane = true;
        for (double v : {w.a, w.b, w.c, w.d, w.e, w.f}) sane &= std::fabs(v) < 1e12;
        dc.world = sane ? w : M();
    }
    static void bbox(Item &it, float margin) {
        plutovg_rect_t r;
        plutovg_path_extents(it.path.get(), &r, false);
        it.bx0 = r.x - margin;
        it.by0 = r.y - margin;
        it.bx1 = r.x + r.w + margin;
        it.by1 = r.y + r.h + margin;
    }
    void clipItem() {
        Item it;
        it.kind = Item::CLIP;
        it.clips = dc.clips;
        addItem(std::move(it));
    }
    std::vector<Clip> lastClips;   // the clips of the last CLIP item (an unclipped list needs none at all)
    void syncClip() {
        if (dc.clips.size() == lastClips.size()) {
            bool same = true;
            for (size_t i = 0; i < dc.clips.size() && same; i++) same = dc.clips[i].path == lastClips[i].path && dc.clips[i].evenodd == lastClips[i].evenodd;
            if (same) return;
        }
        lastClips = dc.clips;
        clipItem();
    }
    bool rop2Draws(uint32_t &rgb) const {
        switch (dc.rop2) {
        case 1: rgb = 0; return true;            // R2_BLACK
        case 16: rgb = 0xffffff; return true;    // R2_WHITE
        case 11: return false;                   // R2_NOP
        default: return true;
        }
    }
    // the current brush's fill of a device-space path
    void fill(const PathP &p, const Brush &b, bool evenodd) {
        if (b.null()) return;
        uint32_t rgb = b.rgb;
        if (!rop2Draws(rgb)) return;
        syncClip();
        if (b.style == BS_HATCHED && dc.bkMode == 2) {   // (an opaque background under the hatch)
            Item bg;
            bg.kind = Item::FILL;
            bg.path = p;
            bg.evenodd = evenodd;
            bg.rgb = dc.bkRgb;
            bbox(bg, 0);
            addItem(std::move(bg));
        }
        Item it;
        it.kind = b.style == BS_HATCHED ? Item::HATCH : Item::FILL;
        it.path = p;
        it.evenodd = evenodd;
        it.rgb = rgb;
        it.hatch = (uint8_t)std::min(b.hatch, 5);
        if (b.style != BS_SOLID && b.style != BS_HATCHED) {
            if (b.pattern) {
                it.img = b.pattern;
                it.tiled = true;
                plutovg_matrix_init_translate(&it.m, (float)dc.box, (float)dc.boy);
            } else
                it.rgb = 0x808080;   // (a pattern that could not be read)
        }
        bbox(it, 0);
        addItem(std::move(it));
    }
    void stroke(const PathP &p, const Pen &pen) {
        if (pen.null()) return;
        uint32_t rgb = pen.rgb;
        if (!rop2Draws(rgb)) return;
        syncClip();
        Item it;
        it.kind = Item::STROKE;
        it.path = p;
        it.rgb = rgb;
        M m = matrix();
        bool geometric = pen.style & 0x10000;
        it.width = pen.width > 0 ? (float)(pen.width * std::hypot(m.a, m.b)) : 0;   // (a cosmetic pen's is 0: a pixel)
        if (geometric) {
            uint32_t cap = pen.style & 0xf00, join = pen.style & 0xf000;
            it.cap = cap == 0x100 ? PLUTOVG_LINE_CAP_SQUARE : cap == 0x200 ? PLUTOVG_LINE_CAP_BUTT : PLUTOVG_LINE_CAP_ROUND;
            it.join = join == 0x1000 ? PLUTOVG_LINE_JOIN_BEVEL : join == 0x2000 ? PLUTOVG_LINE_JOIN_MITER : PLUTOVG_LINE_JOIN_ROUND;
        }
        switch (pen.style & 15) {
        case PS_DASH: it.dash = {4, 2}; break;
        case PS_DOT: it.dash = {1, 1}; break;
        case PS_DASHDOT: it.dash = {4, 2, 1, 2}; break;
        case PS_DASHDOTDOT: it.dash = {4, 2, 1, 2, 1, 2}; break;
        case PS_ALTERNATE: it.dash = {1, 1}; break;
        case PS_USERSTYLE: it.dash = pen.dash; break;
        default: break;
        }
        if (!it.dash.empty() && it.width == 0 && (pen.style & 15) != PS_USERSTYLE)
            for (float &d : it.dash) d *= 2;   // (cosmetic pens: longer dashes, in pixels)
        bbox(it, std::max(it.width, 0.5f) * 2);
        addItem(std::move(it));
    }
    // a shape given in logical coordinates: drawn with the brush and / or the pen, or added to the path under construction
    void shape(plutovg_path_t *lp, bool doFill, bool doStroke) {
        M m = matrix();
        plutovg_matrix_t pm = m.pv();
        if (inPath) {
            plutovg_path_add_path(path.get(), lp, &pm);
            pathOpen = false;
            return;
        }
        PathP p = newPath();
        plutovg_path_add_path(p.get(), lp, &pm);
        if (psOpen && psPath && !psIgnore) plutovg_path_add_path(psPath.get(), p.get(), nullptr);
        if (doFill) fill(p, dc.brush, dc.fillMode == 1);
        if (doStroke) stroke(p, dc.pen);
    }
    // a line (LineTo, PolylineTo, PolyBezierTo, ArcTo) from the current position: continues the path under construction
    void lineShape(plutovg_path_t *lp, double endX, double endY) {   // lp starts at the current position
        if (inPath) {
            M m = matrix();
            plutovg_matrix_t pm = m.pv();
            if (pathOpen) {   // (continue the open subpath: drop lp's move)
                plutovg_path_t *t = plutovg_path_create();
                plutovg_path_add_path(t, lp, &pm);
                const plutovg_path_element_t *el;
                int n = plutovg_path_get_elements(t, &el);
                for (int i = 0; i < n;) {
                    int k = el[i].header.length;
                    if (el[i].header.command == PLUTOVG_PATH_COMMAND_LINE_TO) plutovg_path_line_to(path.get(), el[i + 1].point.x, el[i + 1].point.y);
                    else if (el[i].header.command == PLUTOVG_PATH_COMMAND_CUBIC_TO)
                        plutovg_path_cubic_to(path.get(), el[i + 1].point.x, el[i + 1].point.y, el[i + 2].point.x, el[i + 2].point.y, el[i + 3].point.x, el[i + 3].point.y);
                    else if (el[i].header.command == PLUTOVG_PATH_COMMAND_MOVE_TO && i > 0) plutovg_path_move_to(path.get(), el[i + 1].point.x, el[i + 1].point.y);
                    i += k;
                }
                plutovg_path_destroy(t);
            } else {
                plutovg_path_add_path(path.get(), lp, &pm);
                pathOpen = true;
            }
        } else
            shape(lp, false, true);
        dc.cx = endX;
        dc.cy = endY;
    }
    void moveTo(double x, double y) {
        dc.cx = x;
        dc.cy = y;
        if (inPath) {
            M m = matrix();
            double dx, dy;
            m.map(x, y, dx, dy);
            plutovg_path_move_to(path.get(), (float)dx, (float)dy);
            pathOpen = true;
        }
    }

    // --- the shapes, in logical coordinates ---
    plutovg_path_t *tmp = plutovg_path_create();
    ~Player() { plutovg_path_destroy(tmp); }

    void polyline(const std::vector<float> &pts, bool close, bool fillIt) {
        size_t n = pts.size() / 2;
        if (n < 2) return;
        plutovg_path_reset(tmp);
        plutovg_path_move_to(tmp, pts[0], pts[1]);
        for (size_t i = 1; i < n; i++) plutovg_path_line_to(tmp, pts[i * 2], pts[i * 2 + 1]);
        if (close) plutovg_path_close(tmp);
        shape(tmp, fillIt, true);
    }
    void polyPolygon(const std::vector<float> &pts, const std::vector<uint32_t> &counts) {
        plutovg_path_reset(tmp);
        size_t at = 0, n = pts.size() / 2;
        for (uint32_t c : counts) {
            if (c < 2 || at + c > n) break;
            plutovg_path_move_to(tmp, pts[at * 2], pts[at * 2 + 1]);
            for (size_t i = 1; i < c; i++) plutovg_path_line_to(tmp, pts[(at + i) * 2], pts[(at + i) * 2 + 1]);
            plutovg_path_close(tmp);
            at += c;
        }
        shape(tmp, true, true);
    }
    void polyBezier(const std::vector<float> &pts, bool to) {
        size_t n = pts.size() / 2;
        plutovg_path_reset(tmp);
        size_t i = 0;
        if (to) {
            if (n < 3) return;
            plutovg_path_move_to(tmp, (float)dc.cx, (float)dc.cy);
        } else {
            if (n < 4) return;
            plutovg_path_move_to(tmp, pts[0], pts[1]);
            i = 1;
        }
        for (; i + 2 < n; i += 3) plutovg_path_cubic_to(tmp, pts[i * 2], pts[i * 2 + 1], pts[i * 2 + 2], pts[i * 2 + 3], pts[i * 2 + 4], pts[i * 2 + 5]);
        if (to) lineShape(tmp, pts[(i - 1) * 2], pts[(i - 1) * 2 + 1]);
        else shape(tmp, false, true);
    }
    void polylineTo(const std::vector<float> &pts) {
        size_t n = pts.size() / 2;
        if (!n) return;
        plutovg_path_reset(tmp);
        plutovg_path_move_to(tmp, (float)dc.cx, (float)dc.cy);
        for (size_t i = 0; i < n; i++) plutovg_path_line_to(tmp, pts[i * 2], pts[i * 2 + 1]);
        lineShape(tmp, pts[(n - 1) * 2], pts[(n - 1) * 2 + 1]);
    }
    void polyDraw(const std::vector<float> &pts, const std::vector<uint8_t> &types) {
        size_t n = std::min(pts.size() / 2, types.size());
        plutovg_path_reset(tmp);
        plutovg_path_move_to(tmp, (float)dc.cx, (float)dc.cy);
        bool any = false;
        for (size_t i = 0; i < n; i++) {
            float x = pts[i * 2], y = pts[i * 2 + 1];
            switch (types[i] & 6) {
            case 6:   // PT_MOVETO
                if (any) {
                    lineShape(tmp, dc.cx, dc.cy);
                    plutovg_path_reset(tmp);
                    any = false;
                }
                moveTo(x, y);
                plutovg_path_reset(tmp);
                plutovg_path_move_to(tmp, x, y);
                break;
            case 2:   // PT_LINETO
                plutovg_path_line_to(tmp, x, y);
                any = true;
                dc.cx = x, dc.cy = y;
                break;
            case 4:   // PT_BEZIERTO (three points)
                if (i + 2 < n) {
                    plutovg_path_cubic_to(tmp, x, y, pts[i * 2 + 2], pts[i * 2 + 3], pts[i * 2 + 4], pts[i * 2 + 5]);
                    dc.cx = pts[i * 2 + 4], dc.cy = pts[i * 2 + 5];
                    i += 2;
                    any = true;
                }
                break;
            default: break;
            }
            if (types[i] & 1) plutovg_path_close(tmp);   // PT_CLOSEFIGURE
        }
        if (any) lineShape(tmp, dc.cx, dc.cy);
    }
    void rectangle(double l, double t, double r, double b) {
        plutovg_path_reset(tmp);
        plutovg_path_add_rect(tmp, (float)std::min(l, r), (float)std::min(t, b), (float)std::fabs(r - l), (float)std::fabs(b - t));
        shape(tmp, true, true);
    }
    void roundRect(double l, double t, double r, double b, double ew, double eh) {
        plutovg_path_reset(tmp);
        float w = (float)std::fabs(r - l), h = (float)std::fabs(b - t);
        plutovg_path_add_round_rect(tmp, (float)std::min(l, r), (float)std::min(t, b), w, h, std::min((float)std::fabs(ew) / 2, w / 2),
                                    std::min((float)std::fabs(eh) / 2, h / 2));
        shape(tmp, true, true);
    }
    void ellipse(double l, double t, double r, double b) {
        plutovg_path_reset(tmp);
        plutovg_path_add_ellipse(tmp, (float)((l + r) / 2), (float)((t + b) / 2), (float)std::fabs(r - l) / 2, (float)std::fabs(b - t) / 2);
        shape(tmp, true, true);
    }
    // an elliptical arc from angle a0 to a1 (radians, in logical coordinates), appended to tmp (a line
    // or move to its start first)
    static void arcTo(plutovg_path_t *p, double cx, double cy, double rx, double ry, double a0, double a1, bool move) {
        if (move) plutovg_path_move_to(p, (float)(cx + rx * std::cos(a0)), (float)(cy + ry * std::sin(a0)));
        else plutovg_path_line_to(p, (float)(cx + rx * std::cos(a0)), (float)(cy + ry * std::sin(a0)));
        int n = (int)std::ceil(std::fabs(a1 - a0) / (M_PI / 2) - 1e-6);
        if (n < 1) n = 1;
        double step = (a1 - a0) / n;
        for (int i = 0; i < n; i++) {
            double t0 = a0 + step * i, t1 = t0 + step, k = 4.0 / 3 * std::tan((t1 - t0) / 4);
            double c0 = std::cos(t0), s0 = std::sin(t0), c1 = std::cos(t1), s1 = std::sin(t1);
            plutovg_path_cubic_to(p, (float)(cx + rx * (c0 - k * s0)), (float)(cy + ry * (s0 + k * c0)), (float)(cx + rx * (c1 + k * s1)),
                                  (float)(cy + ry * (s1 - k * c1)), (float)(cx + rx * c1), (float)(cy + ry * s1));
        }
    }
    // GDI's arc of the ellipse in a rectangle, between the radials through two points, in the arc
    // direction (counterclockwise as seen on the device, by default); kind: 0 Arc, 1 ArcTo, 2 Chord, 3 Pie
    void arc(double l, double t, double r, double b, double sx, double sy, double ex, double ey, int kind) {
        double cx = (l + r) / 2, cy = (t + b) / 2, rx = std::fabs(r - l) / 2, ry = std::fabs(b - t) / 2;
        if (rx <= 0 || ry <= 0) return;
        double a0 = std::atan2((sy - cy) / ry, (sx - cx) / rx), a1 = std::atan2((ey - cy) / ry, (ex - cx) / rx);
        bool decreasing = (dc.arcDir != 2) == (matrix().det() > 0);
        if (decreasing) {
            while (a1 >= a0 - 1e-9) a1 -= 2 * M_PI;
        } else {
            while (a1 <= a0 + 1e-9) a1 += 2 * M_PI;
        }
        plutovg_path_reset(tmp);
        if (kind == 3) {
            plutovg_path_move_to(tmp, (float)cx, (float)cy);
            arcTo(tmp, cx, cy, rx, ry, a0, a1, false);
            plutovg_path_close(tmp);
            shape(tmp, true, true);
        } else if (kind == 2) {
            arcTo(tmp, cx, cy, rx, ry, a0, a1, true);
            plutovg_path_close(tmp);
            shape(tmp, true, true);
        } else if (kind == 1) {
            plutovg_path_move_to(tmp, (float)dc.cx, (float)dc.cy);
            arcTo(tmp, cx, cy, rx, ry, a0, a1, false);
            lineShape(tmp, cx + rx * std::cos(a1), cy + ry * std::sin(a1));
        } else {
            arcTo(tmp, cx, cy, rx, ry, a0, a1, true);
            shape(tmp, false, true);
        }
    }
    void angleArc(double cx, double cy, double r, double start, double sweep) {   // degrees, counterclockwise on the device
        double sgn = matrix().det() > 0 ? -1 : 1;
        double a0 = sgn * start * M_PI / 180, a1 = a0 + sgn * sweep * M_PI / 180;
        if (std::fabs(a1 - a0) > 4 * M_PI) return;
        plutovg_path_reset(tmp);
        plutovg_path_move_to(tmp, (float)dc.cx, (float)dc.cy);
        arcTo(tmp, cx, cy, r, r, a0, a1, false);
        lineShape(tmp, cx + r * std::cos(a1), cy + r * std::sin(a1));
    }
    void lineTo(double x, double y) {
        plutovg_path_reset(tmp);
        plutovg_path_move_to(tmp, (float)dc.cx, (float)dc.cy);
        plutovg_path_line_to(tmp, (float)x, (float)y);
        lineShape(tmp, x, y);
    }
    void pixel(double x, double y, uint32_t rgb) {
        plutovg_path_reset(tmp);
        plutovg_path_add_rect(tmp, (float)x, (float)y, 1, 1);
        Brush b{BS_SOLID, rgb};
        M m = matrix();
        plutovg_matrix_t pm = m.pv();
        PathP p = newPath();
        plutovg_path_add_path(p.get(), tmp, &pm);
        fill(p, b, false);
    }
    void fillRect(double l, double t, double r, double b, const Brush &brush) {   // (PatBlt, ExtTextOut's opaque rectangle)
        plutovg_path_reset(tmp);
        plutovg_path_add_rect(tmp, (float)std::min(l, r), (float)std::min(t, b), (float)std::fabs(r - l), (float)std::fabs(b - t));
        M m = matrix();
        plutovg_matrix_t pm = m.pv();
        PathP p = newPath();
        plutovg_path_add_path(p.get(), tmp, &pm);
        fill(p, brush, false);
    }

    // --- clipping ---
    PathP rectPath(double l, double t, double r, double b, const M &m) {
        plutovg_path_reset(tmp);
        plutovg_path_add_rect(tmp, (float)std::min(l, r), (float)std::min(t, b), (float)std::fabs(r - l), (float)std::fabs(b - t));
        plutovg_matrix_t pm = m.pv();
        PathP p = newPath();
        plutovg_path_add_path(p.get(), tmp, &pm);
        return p;
    }
    void intersectClipRect(double l, double t, double r, double b) {
        if (dc.clips.size() < 64) dc.clips.push_back({rectPath(l, t, r, b, matrix()), false});
    }
    void excludeClipRect(double l, double t, double r, double b) {   // everything but the rectangle: a far larger one around it, even-odd
        PathP p = rectPath(l, t, r, b, matrix());
        plutovg_rect_t e;
        plutovg_path_extents(p.get(), &e, false);
        plutovg_path_reset(tmp);
        plutovg_path_add_rect(tmp, e.x - 1e5f, e.y - 1e5f, e.w + 2e5f, e.h + 2e5f);
        plutovg_path_add_path(p.get(), tmp, nullptr);
        if (dc.clips.size() < 64) dc.clips.push_back({p, true});
    }
    void selectClip(PathP region, int mode) {   // a region (device space) by RGN_* mode; null with RGN_COPY: no clip
        if (mode == RGN_COPY) {
            dc.clips.clear();
            if (region) dc.clips.push_back({region, false});
        } else if (mode == RGN_AND && region) {
            if (dc.clips.size() < 64) dc.clips.push_back({region, false});
        }   // (OR, XOR and DIFF would need region arithmetic: the clip stays)
    }
    void offsetClip(double dx, double dy) {   // logical
        M m = matrix();
        double x0, y0, x1, y1;
        m.map(0, 0, x0, y0);
        m.map(dx, dy, x1, y1);
        plutovg_matrix_t t;
        plutovg_matrix_init_translate(&t, (float)(x1 - x0), (float)(y1 - y0));
        for (Clip &c : dc.clips) {
            PathP p = newPath();
            plutovg_path_add_path(p.get(), c.path.get(), &t);
            c.path = p;
        }
    }
    // an EMF region (RGNDATA: a header of 32 bytes and rectangles) as a path in device space
    PathP region(Bytes b) {
        uint32_t count = b.u32(8);
        if (b.u32(4) != 1 || count > b.n / 16) return nullptr;
        M m = matrix();
        plutovg_matrix_t pm = m.pv();
        PathP p = newPath();
        plutovg_path_reset(tmp);
        for (uint32_t i = 0; i < count; i++) {
            size_t o = 32 + (size_t)i * 16;
            float l = (float)b.i32(o), t = (float)b.i32(o + 4), r = (float)b.i32(o + 8), bt = (float)b.i32(o + 12);
            plutovg_path_add_rect(tmp, std::min(l, r), std::min(t, bt), std::fabs(r - l), std::fabs(bt - t));
        }
        plutovg_path_add_path(p.get(), tmp, &pm);
        return p;
    }

    // --- bitmaps ---
    // a bitmap's pixels (sx, sy, sw, sh of it) stretched to the logical rectangle x, y, w, h; a
    // raster operation's mask pair (SRCAND then SRCPAINT) drawn as one picture with transparency
    void blit(double x, double y, double w, double h, SurfP img, double sx, double sy, double sw, double sh, uint32_t rop, float opacity = 1) {
        if (!img || !img->s || !(w != 0) || !(h != 0) || !(sw != 0) || !(sh != 0)) return;
        if (rop == SRCAND) {
            if (orMask && orRect[0] == x && orRect[1] == y && orRect[2] == w && orRect[3] == h) {
                img = orThenAnd(orMask, img);
                orMask = nullptr;
            } else {
                flushMask();
                mask = img;
                maskRect[0] = x, maskRect[1] = y, maskRect[2] = w, maskRect[3] = h;
                return;
            }
        } else if (rop == SRCPAINT) {
            if (mask && maskRect[0] == x && maskRect[1] == y && maskRect[2] == w && maskRect[3] == h) {
                img = applyMask(img, mask);
                mask = nullptr;
            } else {
                flushMask();
                orMask = img;
                orRect[0] = x, orRect[1] = y, orRect[2] = w, orRect[3] = h;
                return;
            }
        } else if (rop == NOTSRCCOPY) {
            img = inverted(img);
        }
        flushMask();
        blitItem(x, y, w, h, img, sx, sy, sw, sh, opacity);
    }
    void flushMask() {   // a mask without its picture: its dark parts (SRCAND) or light ones (SRCPAINT)
        if (mask) {
            SurfP m = mask;
            mask = nullptr;
            blitItem(maskRect[0], maskRect[1], maskRect[2], maskRect[3], keyed(m, 0xffffff), 0, 0, plutovg_surface_get_width(m->s),
                     plutovg_surface_get_height(m->s), 1);
        }
        if (orMask) {   // (OR with what is there: black adds nothing)
            SurfP m = orMask;
            orMask = nullptr;
            blitItem(orRect[0], orRect[1], orRect[2], orRect[3], keyed(m, 0x000000), 0, 0, plutovg_surface_get_width(m->s),
                     plutovg_surface_get_height(m->s), 1);
        }
    }
    // the top of a DIB record's source rectangle (sy, sh in the DIB's coordinates: from the bottom of a
    // bottom-up DIB) among the decoded rows, which are top-down and may be a band of the DIB
    static double dibSrcY(const DibPlace &p, double sy, double sh) { return (p.bottomUp ? p.height - sy - sh : sy) - p.top; }
    void blitItem(double x, double y, double w, double h, SurfP img, double sx, double sy, double sw, double sh, float opacity) {
        uint32_t rgb;
        if (!rop2Draws(rgb)) return;
        syncClip();
        Item it;
        it.kind = Item::IMAGE;
        it.img = img;
        it.opacity = opacity;
        M m = matrix();
        it.path = rectPath(x, y, x + w, y + h, m);
        // pixels to device: the source rectangle onto the destination one
        M pm = mul(mul(mul(M::translate(-sx, -sy), M::scale(w / sw, h / sh)), M::translate(x, y)), m);
        it.m = pm.pv();
        bbox(it, 0);
        addItem(std::move(it));
    }
    static SurfP copySurface(const SurfP &s) {
        auto o = std::make_shared<Surface>();
        int w = plutovg_surface_get_width(s->s), h = plutovg_surface_get_height(s->s);
        o->s = plutovg_surface_create(w, h);
        if (!o->s) return nullptr;
        for (int y = 0; y < h; y++)
            memcpy(plutovg_surface_get_data(o->s) + (size_t)y * plutovg_surface_get_stride(o->s),
                   plutovg_surface_get_data(s->s) + (size_t)y * plutovg_surface_get_stride(s->s), (size_t)w * 4);
        return o;
    }
    static SurfP keyed(const SurfP &s, uint32_t rgb) {   // one colour transparent
        SurfP o = copySurface(s);
        if (!o) return s;
        int w = plutovg_surface_get_width(o->s), h = plutovg_surface_get_height(o->s), stride = plutovg_surface_get_stride(o->s);
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(plutovg_surface_get_data(o->s) + (size_t)y * stride);
            for (int x = 0; x < w; x++)
                if ((row[x] & 0xffffff) == rgb) row[x] = 0;
        }
        return o;
    }
    static SurfP inverted(const SurfP &s) {
        SurfP o = copySurface(s);
        if (!o) return s;
        int w = plutovg_surface_get_width(o->s), h = plutovg_surface_get_height(o->s), stride = plutovg_surface_get_stride(o->s);
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(plutovg_surface_get_data(o->s) + (size_t)y * stride);
            for (int x = 0; x < w; x++) row[x] = (row[x] & 0xff000000u) | (~row[x] & 0xffffff);
        }
        return o;
    }
    // the picture of a SRCAND / SRCPAINT pair: opaque where the mask is dark, transparent where it is light and the picture black
    static SurfP applyMask(const SurfP &s, const SurfP &m) {
        SurfP o = copySurface(s);
        if (!o) return s;
        int w = plutovg_surface_get_width(o->s), h = plutovg_surface_get_height(o->s), stride = plutovg_surface_get_stride(o->s);
        int mw = plutovg_surface_get_width(m->s), mh = plutovg_surface_get_height(m->s), mstride = plutovg_surface_get_stride(m->s);
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(plutovg_surface_get_data(o->s) + (size_t)y * stride);
            const uint32_t *mrow = (const uint32_t *)(plutovg_surface_get_data(m->s) + (size_t)((int64_t)y * mh / h) * mstride);
            for (int x = 0; x < w; x++) {
                uint32_t mp = mrow[(int64_t)x * mw / w];
                bool light = ((mp >> 16 & 255) + (mp >> 8 & 255) + (mp & 255)) >= 384;
                if (light && (row[x] & 0xffffff) == 0) row[x] = 0;
            }
        }
        return o;
    }
    // the picture of a SRCPAINT / SRCAND pair: opaque where the mask is light, elsewhere transparent
    // where the picture is white (ANDing white leaves what is there)
    static SurfP orThenAnd(const SurfP &m, const SurfP &s) {
        SurfP o = copySurface(s);
        if (!o) return s;
        int w = plutovg_surface_get_width(o->s), h = plutovg_surface_get_height(o->s), stride = plutovg_surface_get_stride(o->s);
        int mw = plutovg_surface_get_width(m->s), mh = plutovg_surface_get_height(m->s), mstride = plutovg_surface_get_stride(m->s);
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(plutovg_surface_get_data(o->s) + (size_t)y * stride);
            const uint32_t *mrow = (const uint32_t *)(plutovg_surface_get_data(m->s) + (size_t)((int64_t)y * mh / h) * mstride);
            for (int x = 0; x < w; x++) {
                uint32_t mp = mrow[(int64_t)x * mw / w];
                bool light = ((mp >> 16 & 255) + (mp >> 8 & 255) + (mp & 255)) >= 384;
                if (!light && (row[x] & 0xffffff) == 0xffffff) row[x] = 0;
            }
        }
        return o;
    }
    // a 1-bit mask's dark pixels cut out of a picture (MaskBlt)
    static SurfP maskedBy(const SurfP &s, const SurfP &m) {
        SurfP o = copySurface(s);
        if (!o) return s;
        int w = plutovg_surface_get_width(o->s), h = plutovg_surface_get_height(o->s), stride = plutovg_surface_get_stride(o->s);
        int mw = plutovg_surface_get_width(m->s), mh = plutovg_surface_get_height(m->s), mstride = plutovg_surface_get_stride(m->s);
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(plutovg_surface_get_data(o->s) + (size_t)y * stride);
            const uint32_t *mrow = (const uint32_t *)(plutovg_surface_get_data(m->s) + (size_t)std::min(y, mh - 1) * mstride);
            for (int x = 0; x < w; x++)
                if ((mrow[std::min(x, mw - 1)] & 0xffffff) == 0) row[x] = 0;
        }
        return o;
    }
    void gradientRect(double x0, double y0, uint32_t c0, double x1, double y1, uint32_t c1, bool vertical) {
        syncClip();
        M m = matrix();
        Item it;
        it.kind = Item::FILL;
        it.gradient = true;
        it.rgb = c0;
        it.rgb2 = c1;
        it.path = rectPath(x0, y0, x1, y1, m);
        double gx0, gy0, gx1, gy1;
        if (vertical) m.map(x0, y0, gx0, gy0), m.map(x0, y1, gx1, gy1);
        else m.map(x0, y0, gx0, gy0), m.map(x1, y0, gx1, gy1);
        it.g[0] = (float)gx0, it.g[1] = (float)gy0, it.g[2] = (float)gx1, it.g[3] = (float)gy1;
        bbox(it, 0);
        addItem(std::move(it));
    }
    void triangle(const double *x, const double *y, uint32_t rgb) {   // (a gradient triangle: its mean colour)
        plutovg_path_reset(tmp);
        plutovg_path_move_to(tmp, (float)x[0], (float)y[0]);
        plutovg_path_line_to(tmp, (float)x[1], (float)y[1]);
        plutovg_path_line_to(tmp, (float)x[2], (float)y[2]);
        plutovg_path_close(tmp);
        M m = matrix();
        plutovg_matrix_t pm = m.pv();
        PathP p = newPath();
        plutovg_path_add_path(p.get(), tmp, &pm);
        fill(p, Brush{BS_SOLID, rgb}, false);
    }

    // --- text ---
    plutovg_font_face_t *face(const Font &f, bool &symbol) {
        symbol = f.charset == 2;
        plutovg_font_face_t *fc = nullptr;
        if (!f.face.empty()) fc = nf_face(f.face.c_str(), f.weight > 0 ? f.weight : 400, f.italic);
        if (!fc) {
            const char *generic = "sans-serif";
            int family = f.family & 0xf0;
            std::string lower = f.face;
            for (char &ch : lower) ch = (char)tolower((unsigned char)ch);
            if (family == 0x10 || lower.find("times") != std::string::npos || lower.find("georgia") != std::string::npos ||
                lower.find("garamond") != std::string::npos || lower.find("book") != std::string::npos)
                generic = "serif";
            else if (family == 0x30 || lower.find("courier") != std::string::npos || lower.find("mono") != std::string::npos ||
                     lower.find("consolas") != std::string::npos)
                generic = "monospace";
            fc = nf_css_face(generic, f.weight >= 600, f.italic);
        }
        return fc;
    }
    // text at a logical point (the alignment's reference), dx: the advances in logical units (none: the font's)
    void text(double x, double y, const std::vector<uint32_t> &cps, const std::vector<int32_t> &dx, uint32_t options, const double *rect) {
        const Font &f = dc.font;
        if ((options & 2) && rect) fillRect(rect[0], rect[1], rect[2], rect[3], Brush{BS_SOLID, dc.bkRgb});   // ETO_OPAQUE
        if (cps.empty()) return;
        if (options & 0x10) return;   // ETO_GLYPH_INDEX: the font's glyph indices, not characters (a print-spooled file); nothing to draw without that font
        uint32_t rgb = dc.textRgb;
        if (!rop2Draws(rgb)) return;
        M m = matrix();
        double det = m.det();
        bool symbol;
        plutovg_font_face_t *fc = face(f, symbol);
        if (!fc) return;
        float asc1, desc1, gap1;
        plutovg_rect_t ext;
        plutovg_font_face_get_metrics(fc, 1, &asc1, &desc1, &gap1, &ext);
        desc1 = -desc1;
        double h = std::fabs(f.height);
        if (h == 0) h = 11 / std::max(std::hypot(m.c, m.d), 1e-6);
        double em = f.height > 0 && asc1 + desc1 > 0 ? h / (asc1 + desc1) : h;
        if (!(em > 0) || !(em < 1e6)) return;
        double asc = asc1 * em, desc = desc1 * em;
        // the glyphs along the baseline from 0, in a local y-down space of logical units
        plutovg_path_t *glyphs = plutovg_path_create();
        double pen = 0;
        plutovg_font_face_t *fallback = nullptr;
        for (size_t i = 0; i < cps.size(); i++) {
            uint32_t cp = cps[i];
            plutovg_font_face_t *g = fc;
            if (symbol) {
                if (nf_has_glyph(fc, 0xf000 + cp)) cp += 0xf000;
                else cp = symbolUnicode(cp);
            }
            if (!nf_has_glyph(g, cp)) {
                if (!fallback) fallback = nf_css_face("", f.weight >= 600, f.italic);
                if (fallback && nf_has_glyph(fallback, cp)) g = fallback;
            }
            float adv = plutovg_font_face_get_glyph_path(g, (float)em, (float)pen, 0, cp, glyphs);
            pen += i < dx.size() ? dx[i] : adv;
        }
        double width = pen;
        // the alignment: the reference point to the text's origin
        int align = dc.textAlign;
        double ox = (align & 6) == 6 ? -width / 2 : (align & 6) == 2 ? -width : 0;
        double oy = (align & 24) == 24 ? 0 : (align & 24) == 8 ? -desc : asc;   // TA_BASELINE, TA_BOTTOM, TA_TOP
        if (align & 1) x = dc.cx, y = dc.cy;   // TA_UPDATECP
        // local to device: the mapping's scale and rotation but not its flip (text stands upright), the escapement
        M up = det < 0 ? mul(M::scale(1, -1), m) : m;
        double rad = -(double)f.escapement * M_PI / 1800;
        double X, Y;
        m.map(x, y, X, Y);
        M local = mul(mul(M::translate(ox, oy), M::rotate(rad)), M{up.a, up.b, up.c, up.d, X, Y});
        plutovg_matrix_t pm = local.pv();
        if (dc.bkMode == 2) {   // OPAQUE: the background of the characters
            plutovg_path_reset(tmp);
            plutovg_path_add_rect(tmp, 0, (float)-asc, (float)width, (float)(asc + desc));
            PathP p = newPath();
            plutovg_path_add_path(p.get(), tmp, &pm);
            fill(p, Brush{BS_SOLID, dc.bkRgb}, false);
        }
        PathP p = newPath();
        plutovg_path_add_path(p.get(), glyphs, &pm);
        plutovg_path_destroy(glyphs);
        if (f.underline || f.strike) {
            plutovg_path_reset(tmp);
            if (f.underline) plutovg_path_add_rect(tmp, 0, (float)(em * 0.08), (float)width, (float)(em * 0.07));
            if (f.strike) plutovg_path_add_rect(tmp, 0, (float)(-em * 0.3), (float)width, (float)(em * 0.07));
            plutovg_path_add_path(p.get(), tmp, &pm);
        }
        fill(p, Brush{BS_SOLID, rgb}, false);
        if (align & 1) {   // the current position moves on
            double ex = width * std::cos(rad), ey = width * std::sin(rad);   // (local)
            double lx = up.a * ex + up.c * ey, ly = up.b * ex + up.d * ey;   // (device)
            plutovg_matrix_t pinv, pmm = m.pv();
            if (plutovg_matrix_invert(&pmm, &pinv)) {
                dc.cx += pinv.a * lx + pinv.c * ly;
                dc.cy += pinv.b * lx + pinv.d * ly;
            }
        }
    }

    // --- the frame ---
    void finish() {
        if (emf) return;
        if (placeable) return;
        if (haveWindow) {
            out.fx0 = ux0, out.fy0 = uy0, out.fx1 = ux1, out.fy1 = uy1;
            return;
        }
        itemsFrame();
    }
    void itemsFrame() {
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (const Item &it : out.items) {
            if (it.kind == Item::CLIP) continue;
            x0 = std::min(x0, (double)it.bx0), y0 = std::min(y0, (double)it.by0);
            x1 = std::max(x1, (double)it.bx1), y1 = std::max(y1, (double)it.by1);
        }
        if (x1 > x0 && y1 > y0) out.fx0 = x0, out.fy0 = y0, out.fx1 = x1, out.fy1 = y1;
    }

    // --- the records ---
    bool playWmf(Bytes b, size_t start);
    bool playEmf(Bytes b);
};

// --- WMF ----------------------------------------------------------------------

bool Player::playWmf(Bytes b, size_t o) {
    uint32_t numObjects = b.u16(o + 10);
    objs.resize(std::min<uint32_t>(numObjects, 4096));
    o += 18;
    std::vector<float> pts;
    std::vector<uint32_t> counts, cps;
    std::vector<int32_t> dx;
    std::string embedded;   // an enhanced metafile in comment records (a dual WMF)
    size_t embeddedSize = 0;
    size_t records = 0;
    for (size_t at = o; at + 8 <= b.n && !psClip && records < 8u << 20; records++) {   // (a CLIP_TO_PATH escape: played as PostScript)
        uint32_t size = b.u32(at);
        uint16_t fn = b.u16(at + 4);
        if (size < 3 || fn == 0) break;
        psClip = fn == 0x0626 && b.u16(at + 6) == 0x1001;
        at += (size_t)size * 2;
    }
    records = 0;
    while (o + 6 <= b.n) {
        uint32_t size = b.u32(o);
        uint16_t fn = b.u16(o + 4);
        if (size < 3 || fn == 0) break;
        if (++records > 8u << 20) break;
        Bytes r = b.sub(o + 6, (size_t)(size - 3) * 2);   // the parameters
        o += (size_t)size * 2;
        // (a mask waiting for its picture: drawn alone before anything else, in the state it came in)
        if ((mask || orMask) && fn != 0x0b41 && fn != 0x0940 && fn != 0x0922 && fn != 0x0f43 && fn != 0x0107 && fn != 0x0626) flushMask();
        auto pt = [&](size_t i) { return (float)r.i16(i); };
        auto readPoints = [&](size_t at, size_t n) {
            pts.clear();
            if (n > kMaxPoints || !r.has(at, n * 4)) n = std::min(n, r.n > at ? (r.n - at) / 4 : 0);
            for (size_t i = 0; i < n; i++) pts.push_back(pt(at + i * 4)), pts.push_back(pt(at + i * 4 + 2));
        };
        switch (fn) {
        case 0x001e: saveDC(); break;
        case 0x0127: restoreDC(r.i16(0)); break;
        case 0x0102: dc.bkMode = r.u16(0); break;
        case 0x0103: dc.mapMode = r.u16(0); break;
        case 0x0104: dc.rop2 = r.u16(0); break;
        case 0x0106: dc.fillMode = r.u16(0); break;
        case 0x0107: dc.stretchMode = r.u16(0); break;
        case 0x012e: dc.textAlign = r.u16(0); break;
        case 0x0201: dc.bkRgb = color(r.u32(0)); break;
        case 0x0209: dc.textRgb = color(r.u32(0)); break;
        case 0x020b: dc.woy = r.i16(0), dc.wox = r.i16(2), windowSet(); break;
        case 0x020c: dc.wey = r.i16(0), dc.wex = r.i16(2), windowSet(); break;
        case 0x020d: dc.voy = r.i16(0), dc.vox = r.i16(2); break;
        case 0x020e: dc.vey = r.i16(0), dc.vex = r.i16(2), dc.vextSet = true; break;
        case 0x020f: dc.woy += r.i16(0), dc.wox += r.i16(2); break;
        case 0x0211: dc.voy += r.i16(0), dc.vox += r.i16(2); break;
        case 0x0410:   // ScaleWindowExt: yDenom, yNum, xDenom, xNum
            if (r.i16(0) && r.i16(4)) dc.wey = dc.wey * r.i16(2) / r.i16(0), dc.wex = dc.wex * r.i16(6) / r.i16(4);
            break;
        case 0x0412:
            if (r.i16(0) && r.i16(4)) dc.vey = dc.vey * r.i16(2) / r.i16(0), dc.vex = dc.vex * r.i16(6) / r.i16(4);
            break;
        case 0x0213: lineTo(r.i16(2), r.i16(0)); break;
        case 0x0214: moveTo(r.i16(2), r.i16(0)); break;
        case 0x0324:   // Polygon
            readPoints(2, r.u16(0));
            polyline(pts, true, true);
            break;
        case 0x0325:
            readPoints(2, r.u16(0));
            polyline(pts, false, false);
            break;
        case 0x0538: {   // PolyPolygon
            uint32_t n = r.u16(0);
            counts.clear();
            size_t total = 0;
            for (uint32_t i = 0; i < n && i < 65536; i++) counts.push_back(r.u16(2 + i * 2)), total += counts.back();
            readPoints(2 + (size_t)n * 2, total);
            polyPolygon(pts, counts);
            break;
        }
        case 0x041b: rectangle(r.i16(6), r.i16(4), r.i16(2), r.i16(0)); break;
        case 0x061c: roundRect(r.i16(10), r.i16(8), r.i16(6), r.i16(4), r.i16(2), r.i16(0)); break;
        case 0x0418: ellipse(r.i16(6), r.i16(4), r.i16(2), r.i16(0)); break;
        case 0x0817: arc(r.i16(14), r.i16(12), r.i16(10), r.i16(8), r.i16(6), r.i16(4), r.i16(2), r.i16(0), 0); break;
        case 0x081a: arc(r.i16(14), r.i16(12), r.i16(10), r.i16(8), r.i16(6), r.i16(4), r.i16(2), r.i16(0), 3); break;
        case 0x0830: arc(r.i16(14), r.i16(12), r.i16(10), r.i16(8), r.i16(6), r.i16(4), r.i16(2), r.i16(0), 2); break;
        case 0x041f: pixel(r.i16(6), r.i16(4), color(r.u32(0))); break;
        case 0x061d: {   // PatBlt: rop, h, w, y, x
            uint32_t rop = r.u32(0);
            double h = r.i16(4), w = r.i16(6), y = r.i16(8), x = r.i16(10);
            if (rop == PATCOPY || rop == PATINVERT) fillRect(x, y, x + w, y + h, dc.brush);
            else if (rop == BLACKNESS) fillRect(x, y, x + w, y + h, Brush{BS_SOLID, 0});
            else if (rop == WHITENESS) fillRect(x, y, x + w, y + h, Brush{BS_SOLID, 0xffffff});
            break;
        }
        case 0x02fa: {   // CreatePenIndirect: style, width (x, y), colour
            Obj ob;
            ob.type = Obj::PEN;
            ob.pen.style = r.u16(0);
            ob.pen.width = (float)std::max<int>(r.i16(2), 0);
            ob.pen.rgb = color(r.u32(6));
            create(std::move(ob));
            break;
        }
        case 0x02fc: {   // CreateBrushIndirect
            Obj ob;
            ob.type = Obj::BRUSH;
            ob.brush.style = r.u16(0);
            ob.brush.rgb = color(r.u32(2));
            ob.brush.hatch = r.u16(6);
            if (ob.brush.style > BS_HATCHED) ob.brush.style = BS_DIBPATTERN;   // (a pattern without its bitmap)
            create(std::move(ob));
            break;
        }
        case 0x0142: {   // DibCreatePatternBrush: style, usage, DIB
            Obj ob;
            ob.type = Obj::BRUSH;
            ob.brush.style = BS_DIBPATTERN;
            ob.brush.pattern = decodeDib(r.sub(4, r.n), {}, false, pixelBudget);
            create(std::move(ob));
            break;
        }
        case 0x01f9: {   // CreatePatternBrush (a device-dependent bitmap of the old kind)
            Obj ob;
            ob.type = Obj::BRUSH;
            ob.brush.style = BS_DIBPATTERN;
            ob.brush.pattern = decodeDdb(r.sub(0, r.n), 0, 0xffffff, pixelBudget);
            create(std::move(ob));
            break;
        }
        case 0x02fb: {   // CreateFontIndirect
            Obj ob;
            ob.type = Obj::FONT;
            Font &f = ob.font;
            f.height = r.i16(0);
            f.escapement = r.i16(4);
            f.weight = r.i16(8);
            f.italic = r.u8(10) != 0;
            f.underline = r.u8(11) != 0;
            f.strike = r.u8(12) != 0;
            f.charset = r.u8(13);
            f.family = r.u8(17);
            std::vector<uint32_t> name;
            size_t len = 0;
            while (len < 32 && r.u8(18 + len)) len++;
            decodeText(r.p + std::min<size_t>(18, r.n), std::min(len, r.n > 18 ? r.n - 18 : 0), f.charset == 2 ? 0 : f.charset, name);
            for (uint32_t c : name) nf_utf8_put(f.face, c);
            create(std::move(ob));
            break;
        }
        case 0x00f7: {   // CreatePalette: start, count, entries (r, g, b, flags)
            Obj ob;
            ob.type = Obj::PALETTE;
            uint32_t n = std::min<uint32_t>(r.u16(2), 256);
            for (uint32_t i = 0; i < n; i++) ob.palette.push_back((uint32_t)r.u8(4 + i * 4) << 16 | (uint32_t)r.u8(5 + i * 4) << 8 | r.u8(6 + i * 4));
            create(std::move(ob));
            break;
        }
        case 0x06ff: {   // CreateRegion: ..., size, scan count, max scan, bounds, scan lines of (left, right) pairs
            Obj ob;
            ob.type = Obj::REGION;
            uint32_t scans = r.u16(10);
            plutovg_path_reset(tmp);
            size_t at = 22;
            for (uint32_t s = 0; s < scans && at + 6 <= r.n; s++) {
                uint32_t count = r.u16(at);
                float top = pt(at + 2), bottom = pt(at + 4);
                for (uint32_t i = 0; i + 1 < count && at + 6 + i * 2 + 4 <= r.n; i += 2)
                    plutovg_path_add_rect(tmp, pt(at + 6 + i * 2), top, pt(at + 8 + i * 2) - pt(at + 6 + i * 2), bottom - top);
                at += 6 + (size_t)count * 2 + 2;
            }
            M m = matrix();
            plutovg_matrix_t pm = m.pv();
            ob.region = newPath();
            plutovg_path_add_path(ob.region.get(), tmp, &pm);
            create(std::move(ob));
            break;
        }
        case 0x012d: select(r.u16(0)); break;
        case 0x01f0: deleteObject(r.u16(0)); break;
        case 0x0234:   // SelectPalette
            if (r.u16(0) < objs.size() && objs[r.u16(0)].type == Obj::PALETTE) dc.palette = objs[r.u16(0)].palette;
            break;
        case 0x012c: {   // SelectClipRegion
            uint32_t i = r.u16(0);
            selectClip(i < objs.size() && objs[i].type == Obj::REGION ? objs[i].region : nullptr, RGN_COPY);
            break;
        }
        case 0x0416: intersectClipRect(r.i16(6), r.i16(4), r.i16(2), r.i16(0)); break;
        case 0x0415: excludeClipRect(r.i16(6), r.i16(4), r.i16(2), r.i16(0)); break;
        case 0x0220: offsetClip(r.i16(2), r.i16(0)); break;
        case 0x0228:   // FillRegion: region, brush
        case 0x012b: {   // PaintRegion: region
            uint32_t i = r.u16(0);
            if (i < objs.size() && objs[i].type == Obj::REGION && objs[i].region) {
                Brush br = dc.brush;
                if (fn == 0x0228 && r.u16(2) < objs.size() && objs[r.u16(2)].type == Obj::BRUSH) br = objs[r.u16(2)].brush;
                fill(objs[i].region, br, false);
            }
            break;
        }
        case 0x0521: {   // TextOut: count, string, y, x
            int n = r.i16(0);
            if (n < 0 || !r.has(2, (size_t)n)) break;
            size_t at = 2 + ((size_t)n + 1 & ~(size_t)1);
            decodeText(r.p + 2, (size_t)n, dc.font.charset, cps);
            dx.clear();
            text(r.i16(at + 2), r.i16(at), cps, dx, 0, nullptr);
            break;
        }
        case 0x0a32: {   // ExtTextOut: y, x, count, options, [rect], string, [dx]
            int n = r.i16(4);
            uint32_t options = r.u16(6);
            size_t at = 8;
            double rect[4] = {0, 0, 0, 0};
            bool hasRect = (options & 6) != 0;
            if (hasRect) {
                rect[0] = r.i16(8), rect[1] = r.i16(10), rect[2] = r.i16(12), rect[3] = r.i16(14);
                at = 16;
            }
            if (n < 0 || !r.has(at, (size_t)n)) break;
            decodeText(r.p + at, (size_t)n, dc.font.charset, cps);
            at += (size_t)n + 1 & ~(size_t)1;
            dx.clear();
            if (r.has(at, (size_t)cps.size() * 2) && cps.size() == (size_t)n)
                for (int i = 0; i < n; i++) dx.push_back(r.i16(at + (size_t)i * 2));
            text(r.i16(2), r.i16(0), cps, dx, options, hasRect ? rect : nullptr);
            break;
        }
        case 0x0b41:   // DibStretchBlt: rop, srcH, srcW, srcY, srcX, [reserved], dstH, dstW, dstY, dstX, DIB
        case 0x0940:   // DibBitBlt: rop, srcY, srcX, [reserved], dstH, dstW, dstY, dstX, DIB
        case 0x0922:   // BitBlt, StretchBlt: the same with a device-dependent bitmap
        case 0x0b23:
        case 0x0f43: { // StretchDIB: rop, usage, srcH, srcW, srcY, srcX, dstH, dstW, dstY, dstX, DIB
            uint32_t rop = r.u32(0);
            bool noBitmap = size == (uint32_t)(fn >> 8) + 3;   // (the form without one has a reserved word instead)
            bool stretch = fn == 0x0b41 || fn == 0x0b23 || fn == 0x0f43;
            size_t at = fn == 0x0f43 ? 6 : 4;
            double sh = 0, sw = 0;
            if (stretch) sh = r.i16(at), sw = r.i16(at + 2), at += 4;
            double sy = r.i16(at), sx = r.i16(at + 2);
            at += noBitmap ? 6 : 4;
            double dh = r.i16(at), dw = r.i16(at + 2), dy = r.i16(at + 4), dx0 = r.i16(at + 6);
            at += 8;
            if (rop == PATCOPY || rop == BLACKNESS || rop == WHITENESS) {
                fillRect(dx0, dy, dx0 + dw, dy + dh, rop == PATCOPY ? dc.brush : Brush{BS_SOLID, rop == BLACKNESS ? 0u : 0xffffffu});
                break;
            }
            if (noBitmap) break;
            DibPlace place;
            SurfP img = fn == 0x0922 || fn == 0x0b23 ? decodeDdb(r.sub(at, r.n), dc.textRgb, dc.bkRgb, pixelBudget)
                                                     : decodeDib(r.sub(at, r.n), {}, false, pixelBudget, &place);
            if (!img) break;
            if (!stretch) sw = dw, sh = dh;
            if (fn == 0x0f43) sy = dibSrcY(place, sy, sh);   // (the others blit from a bitmap selected into a DC: top-down)
            blit(dx0, dy, dw, dh, img, sx, sy, sw, sh, rop);
            break;
        }
        case 0x0d33: {   // SetDIBitsToDevice: usage, scanCount, startScan, srcY, srcX, h, w, dstY, dstX, DIB
            double sy = r.i16(6), sx = r.i16(8), h = r.i16(10), w = r.i16(12), dy = r.i16(14), dx0 = r.i16(16);
            DibPlace place;
            SurfP img = decodeDib(r.sub(18, r.n), {}, false, pixelBudget, &place, r.u16(4), r.u16(2));
            if (img) blit(dx0, dy, w, h, img, sx, dibSrcY(place, sy, h), w, h, SRCCOPY);
            break;
        }
        case 0x0626: {   // Escape: function, byte count, data
            uint16_t esc = r.u16(0), arg = r.u16(4);
            if (psClip && (esc == 0x0026 || (esc >= 0x1000 && esc <= 0x1002))) {
                if (esc == 0x0026) psIgnore = arg != 0;   // POSTSCRIPT_IGNORE: 1 on, 0 off
                else if (psIgnore) break;
                else if (esc == 0x1000) psPath = newPath(), psOpen = true;   // BEGIN_PATH
                else if (esc == 0x1002 && psOpen) {   // END_PATH: render mode, fill mode (1 alternate, 2 winding), ...
                    psOpen = false;
                    psEvenodd = r.n < 7 || r.u8(6) != 2;
                    if (!plutovg_path_get_elements(psPath.get(), nullptr)) psPath = nullptr;
                } else if (esc == 0x1001) {   // CLIP_TO_PATH: save, restore, inclusive, exclusive
                    if (arg == 0 && psSaved.size() < 64) psSaved.push_back(dc.clips);
                    else if (arg == 1 && !psSaved.empty()) dc.clips = std::move(psSaved.back()), psSaved.pop_back();
                    else if (arg == 2 && psPath) {   // (the path may still be open: it ends here)
                        if (plutovg_path_get_elements(psPath.get(), nullptr) && dc.clips.size() < 64) dc.clips.push_back({psPath, psEvenodd});
                        psPath = nullptr, psOpen = false;
                    }
                }
                break;
            }
            // an enhanced metafile embedded in comments (a dual file): "WMFC", type 1, version,
            // checksum, flags, chunks, this chunk's size, the remaining bytes, the whole size, the chunk
            if (esc != 0x000f || r.u32(4) != 0x43464d57 || r.u32(8) != 1) break;   // MFCOMMENT, "WMFC"
            if (embedded.empty()) embeddedSize = r.u32(34);
            size_t chunk = r.u32(26);
            if (!r.has(38, chunk) || embedded.size() + chunk > embeddedSize || embeddedSize > 256u << 20) break;
            embedded.append((const char *)r.p + 38, chunk);
            break;
        }
        default: break;   // escapes, palettes, flood fills, SetRelabs, SetMapperFlags, SetTextJustification, ...
        }
    }
    flushMask();
    if (embeddedSize && embedded.size() == embeddedSize) {   // the enhanced one is the real content
        nitrometa alt;
        Player p(alt, true);
        if (p.playEmf({(const uint8_t *)embedded.data(), embedded.size()}) && !alt.items.empty()) {
            out = std::move(alt);
            emf = true;   // (its frame stands)
        }
    }
    return true;
}

// --- EMF ----------------------------------------------------------------------

bool Player::playEmf(Bytes b) {
    if (b.u32(0) != 1 || b.u32(40) != 0x464d4520 || b.u32(4) < 88) return false;
    double bl = b.i32(8), bt = b.i32(12), br = b.i32(16), bb = b.i32(20);         // rclBounds (device)
    double fl = b.i32(24), ft = b.i32(28), fr = b.i32(32), fb = b.i32(36);        // rclFrame (0.01 mm)
    uint32_t handles = b.u16(48);
    int devX = b.i32(72), devY = b.i32(76), mmX = b.i32(80), mmY = b.i32(84);
    double dpiX = devX > 0 && mmX > 0 ? devX * 25.4 / mmX : 96, dpiY = devY > 0 && mmY > 0 ? devY * 25.4 / mmY : dpiX;
    if (dpiX < 10 || dpiX > 10000) dpiX = 96;
    if (dpiY < 10 || dpiY > 10000) dpiY = dpiX;
    dpi = dpiX;
    yScale = dpiX / dpiY;
    out.unit = 72 / dpiX;
    if (fr > fl && fb > ft) {
        out.fx0 = fl * dpiX / 2540, out.fy0 = ft * dpiX / 2540, out.fx1 = fr * dpiX / 2540, out.fy1 = fb * dpiX / 2540;
    } else if (br > bl && bb > bt) {
        out.fx0 = bl, out.fy0 = bt * yScale, out.fx1 = br + 1, out.fy1 = (bb + 1) * yScale;
    }
    objs.resize(std::min<uint32_t>(handles, 65536) + 1);
    std::vector<float> pts;
    std::vector<uint32_t> counts, cps;
    std::vector<uint8_t> types;
    std::vector<int32_t> dx;
    size_t o = 0, records = 0;
    while (o + 8 <= b.n) {
        uint32_t type = b.u32(o), size = b.u32(o + 4);
        if (size < 8 || size & 3 || size > b.n - o) break;
        if (++records > 8u << 20) break;
        Bytes r = b.sub(o, size);
        o += size;
        if (type == 14) break;   // EOF
        // (a mask waiting for its picture: drawn alone before anything else, in the state it came in;
        // StretchBltMode, comments and ICM records may come between the two)
        if ((mask || orMask) && type != 76 && type != 77 && type != 81 && type != 21 && type != 70 && type != 98) flushMask();
        auto readPoints = [&](size_t at, size_t n, bool wide) {
            pts.clear();
            size_t sz = wide ? 8 : 4;
            if (n > kMaxPoints || !r.has(at, n * sz)) n = std::min(n, r.n > at ? (r.n - at) / sz : 0);
            for (size_t i = 0; i < n; i++) {
                if (wide) pts.push_back((float)r.i32(at + i * 8)), pts.push_back((float)r.i32(at + i * 8 + 4));
                else pts.push_back(r.i16(at + i * 4)), pts.push_back(r.i16(at + i * 4 + 2));
            }
        };
        auto rect = [&](size_t at, double *v) {
            for (int i = 0; i < 4; i++) v[i] = r.i32(at + (size_t)i * 4);
        };
        auto dib = [&](size_t offBmi, size_t cbBmi, size_t offBits, size_t cbBits, bool alpha, DibPlace *place = nullptr, uint32_t startScan = 0,
                       uint32_t scans = 0) {
            return decodeDib(r.sub(offBmi, cbBmi), cbBits ? r.sub(offBits, cbBits) : Bytes{}, alpha, pixelBudget, place, startScan, scans);
        };
        double v[4];
        switch (type) {
        case 2: case 85:   // PolyBezier
            readPoints(28, r.u32(24), type == 2);
            polyBezier(pts, false);
            break;
        case 3: case 86:   // Polygon
            readPoints(28, r.u32(24), type == 3);
            polyline(pts, true, true);
            break;
        case 4: case 87:   // Polyline
            readPoints(28, r.u32(24), type == 4);
            polyline(pts, false, false);
            break;
        case 5: case 88:   // PolyBezierTo
            readPoints(28, r.u32(24), type == 5);
            polyBezier(pts, true);
            break;
        case 6: case 89:   // PolylineTo
            readPoints(28, r.u32(24), type == 6);
            polylineTo(pts);
            break;
        case 7: case 90:   // PolyPolyline
        case 8: case 91: { // PolyPolygon
            uint32_t n = r.u32(24), total = r.u32(28);
            counts.clear();
            if (n > 65536) break;
            for (uint32_t i = 0; i < n; i++) counts.push_back(r.u32(32 + (size_t)i * 4));
            readPoints(32 + (size_t)n * 4, total, type == 7 || type == 8);
            if (type == 8 || type == 91) polyPolygon(pts, counts);
            else {
                size_t at = 0;
                for (uint32_t c : counts) {
                    if (at + c > pts.size() / 2) break;
                    std::vector<float> part(pts.begin() + (long)at * 2, pts.begin() + (long)(at + c) * 2);
                    polyline(part, false, false);
                    at += c;
                }
            }
            break;
        }
        case 9: dc.wex = r.i32(8), dc.wey = r.i32(12); break;
        case 10: dc.wox = r.i32(8), dc.woy = r.i32(12); break;
        case 11: dc.vex = r.i32(8), dc.vey = r.i32(12), dc.vextSet = true; break;
        case 12: dc.vox = r.i32(8), dc.voy = r.i32(12); break;
        case 13: dc.box = r.i32(8), dc.boy = r.i32(12); break;
        case 15: pixel(r.i32(8), r.i32(12), color(r.u32(16))); break;
        case 17: dc.mapMode = (int)r.u32(8); break;
        case 18: dc.bkMode = (int)r.u32(8); break;
        case 19: dc.fillMode = (int)r.u32(8); break;
        case 20: dc.rop2 = (int)r.u32(8); break;
        case 21: dc.stretchMode = (int)r.u32(8); break;
        case 22: dc.textAlign = (int)r.u32(8); break;
        case 24: dc.textRgb = color(r.u32(8)); break;
        case 25: dc.bkRgb = color(r.u32(8)); break;
        case 26: offsetClip(r.i32(8), r.i32(12)); break;
        case 27: moveTo(r.i32(8), r.i32(12)); break;
        case 29: rect(8, v), excludeClipRect(v[0], v[1], v[2], v[3]); break;
        case 30: rect(8, v), intersectClipRect(v[0], v[1], v[2], v[3]); break;
        case 31:   // ScaleViewportExtEx: xNum, xDenom, yNum, yDenom
            if (r.i32(12) && r.i32(20)) dc.vex = dc.vex * r.i32(8) / r.i32(12), dc.vey = dc.vey * r.i32(16) / r.i32(20);
            break;
        case 32:
            if (r.i32(12) && r.i32(20)) dc.wex = dc.wex * r.i32(8) / r.i32(12), dc.wey = dc.wey * r.i32(16) / r.i32(20);
            break;
        case 33: saveDC(); break;
        case 34: restoreDC(r.i32(8)); break;
        case 35:   // SetWorldTransform
            setWorld({r.f32(8), r.f32(12), r.f32(16), r.f32(20), r.f32(24), r.f32(28)});
            break;
        case 36: {   // ModifyWorldTransform
            M x{r.f32(8), r.f32(12), r.f32(16), r.f32(20), r.f32(24), r.f32(28)};
            switch (r.u32(32)) {
            case 1: dc.world = M(); break;
            case 2: setWorld(mul(x, dc.world)); break;   // MWT_LEFTMULTIPLY
            case 3: setWorld(mul(dc.world, x)); break;   // MWT_RIGHTMULTIPLY
            case 4: setWorld(x); break;
            default: break;
            }
            break;
        }
        case 37: select(r.u32(8)); break;
        case 38: {   // CreatePen: index, style, width (x, y), colour
            Obj ob;
            ob.type = Obj::PEN;
            ob.pen.style = r.u32(12);
            ob.pen.width = (float)std::max(r.i32(16), 0);
            ob.pen.rgb = color(r.u32(24));
            create(std::move(ob), r.u32(8));
            break;
        }
        case 95: {   // ExtCreatePen: index, bitmap offsets, style, width, brush style, colour, hatch, dashes
            Obj ob;
            ob.type = Obj::PEN;
            ob.pen.style = r.u32(28);
            ob.pen.width = (float)r.u32(32);
            if (!(ob.pen.style & 0x10000)) ob.pen.width = 0;   // (cosmetic: a pixel)
            ob.pen.rgb = color(r.u32(40));
            if (r.u32(36) == BS_NULL) ob.pen.style = (ob.pen.style & ~15u) | PS_NULL;
            uint32_t n = std::min<uint32_t>(r.u32(48), 64);
            for (uint32_t i = 0; i < n; i++) ob.pen.dash.push_back((float)r.u32(52 + i * 4));
            if (n & 1) ob.pen.dash.insert(ob.pen.dash.end(), ob.pen.dash.begin(), ob.pen.dash.end());
            bool any = false;
            for (float d : ob.pen.dash) any |= d > 0;
            if (!any) ob.pen.dash.clear();
            create(std::move(ob), r.u32(8));
            break;
        }
        case 39: {   // CreateBrushIndirect: index, style, colour, hatch
            Obj ob;
            ob.type = Obj::BRUSH;
            ob.brush.style = (int)r.u32(12);
            ob.brush.rgb = color(r.u32(16));
            ob.brush.hatch = (int)r.u32(20);
            if (ob.brush.style > BS_HATCHED) ob.brush.style = BS_DIBPATTERN;
            create(std::move(ob), r.u32(8));
            break;
        }
        case 93:   // CreateMonoBrush
        case 94: { // CreateDIBPatternBrushPt: index, usage, offsets
            Obj ob;
            ob.type = Obj::BRUSH;
            ob.brush.style = BS_DIBPATTERN;
            ob.brush.pattern = dib(r.u32(16), r.u32(20), r.u32(24), r.u32(28), false);
            if (type == 93 && ob.brush.pattern) {   // (a monochrome pattern in the text and background colours)
                int w = plutovg_surface_get_width(ob.brush.pattern->s), h = plutovg_surface_get_height(ob.brush.pattern->s);
                int stride = plutovg_surface_get_stride(ob.brush.pattern->s);
                for (int y = 0; y < h; y++) {
                    uint32_t *row = (uint32_t *)(plutovg_surface_get_data(ob.brush.pattern->s) + (size_t)y * stride);
                    for (int x = 0; x < w; x++) row[x] = 0xff000000u | ((row[x] & 0xffffff) ? dc.bkRgb : dc.textRgb);
                }
            }
            create(std::move(ob), r.u32(8));
            break;
        }
        case 82: {   // ExtCreateFontIndirectW: index, LOGFONTW
            Obj ob;
            ob.type = Obj::FONT;
            Font &f = ob.font;
            f.height = (float)r.i32(12);
            f.escapement = r.i32(20);
            f.weight = r.i32(28);
            f.italic = r.u8(32) != 0;
            f.underline = r.u8(33) != 0;
            f.strike = r.u8(34) != 0;
            f.charset = r.u8(35);
            f.family = r.u8(39);
            for (size_t i = 0; i < 32; i++) {
                uint32_t c = r.u16(40 + i * 2);
                if (!c) break;
                if (c >= 0xd800 && c < 0xdc00 && i + 1 < 32) {
                    uint32_t lo = r.u16(42 + i * 2);
                    if (lo >= 0xdc00 && lo < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00), i++;
                }
                nf_utf8_put(f.face, c);
            }
            create(std::move(ob), r.u32(8));
            break;
        }
        case 40: deleteObject(r.u32(8)); break;
        case 41: angleArc(r.i32(8), r.i32(12), r.u32(16), r.f32(20), r.f32(24)); break;
        case 42: rect(8, v), ellipse(v[0], v[1], v[2], v[3]); break;
        case 43: rect(8, v), rectangle(v[0], v[1], v[2], v[3]); break;
        case 44: rect(8, v), roundRect(v[0], v[1], v[2], v[3], r.i32(24), r.i32(28)); break;
        case 45: case 46: case 47: case 55:   // Arc, Chord, Pie, ArcTo
            rect(8, v);
            arc(v[0], v[1], v[2], v[3], r.i32(24), r.i32(28), r.i32(32), r.i32(36), type == 45 ? 0 : type == 55 ? 1 : type == 46 ? 2 : 3);
            break;
        case 54: lineTo(r.i32(8), r.i32(12)); break;
        case 56: case 92: {   // PolyDraw
            uint32_t n = r.u32(24);
            readPoints(28, n, type == 56);
            size_t at = 28 + (size_t)n * (type == 56 ? 8 : 4);
            types.clear();
            for (size_t i = 0; i < pts.size() / 2; i++) types.push_back(r.u8(at + i));
            polyDraw(pts, types);
            break;
        }
        case 57: dc.arcDir = (int)r.u32(8); break;
        case 58: dc.miter = r.f32(8); break;
        case 59:   // BeginPath
            inPath = true;
            pathOpen = false;
            path = newPath();
            break;
        case 60: inPath = false; break;   // EndPath
        case 61:   // CloseFigure
            if (inPath && path) plutovg_path_close(path.get()), pathOpen = false;
            break;
        case 62: case 63: case 64:   // FillPath, StrokeAndFillPath, StrokePath
            inPath = false;
            if (path) {
                if (type != 64) fill(path, dc.brush, dc.fillMode == 1);
                if (type != 62) stroke(path, dc.pen);
            }
            path = nullptr;
            break;
        case 65: case 66: break;   // FlattenPath, WidenPath
        case 67:   // SelectClipPath
            inPath = false;
            if (path) {
                int mode = (int)r.u32(8);
                if (mode == RGN_COPY) dc.clips.clear();
                if ((mode == RGN_COPY || mode == RGN_AND) && dc.clips.size() < 64) dc.clips.push_back({path, dc.fillMode == 1});
            }
            path = nullptr;
            break;
        case 68: inPath = false, path = nullptr; break;   // AbortPath
        case 71: case 74: {   // FillRgn (brush index), PaintRgn
            PathP rg = region(r.sub(type == 71 ? 32 : 28, r.u32(24)));
            if (!rg) break;
            Brush br = dc.brush;
            if (type == 71 && r.u32(28) < objs.size() && objs[r.u32(28)].type == Obj::BRUSH) br = objs[r.u32(28)].brush;
            fill(rg, br, false);
            break;
        }
        case 75: {   // ExtSelectClipRgn: size, mode, region
            uint32_t cb = r.u32(8);
            int mode = (int)r.u32(12);
            selectClip(cb ? region(r.sub(16, cb)) : nullptr, mode);
            break;
        }
        case 76: case 77: case 114: case 116: {   // BitBlt, StretchBlt, AlphaBlend, TransparentBlt
            double x = r.i32(24), y = r.i32(28), w = r.i32(32), h = r.i32(36);
            uint32_t rop = r.u32(40);
            double sx = r.i32(44), sy = r.i32(48);
            uint32_t cb = r.u32(96);
            if (type == 76 && !cb) {
                if (rop == PATCOPY || rop == PATINVERT) fillRect(x, y, x + w, y + h, dc.brush);
                else if (rop == BLACKNESS) fillRect(x, y, x + w, y + h, Brush{BS_SOLID, 0});
                else if (rop == WHITENESS) fillRect(x, y, x + w, y + h, Brush{BS_SOLID, 0xffffff});
                break;
            }
            double sw = type == 76 ? w : r.i32(100), sh = type == 76 ? h : r.i32(104);
            bool alpha = type == 114 && (rop >> 24 & 1);   // AC_SRC_ALPHA
            SurfP img = dib(r.u32(84), r.u32(88), r.u32(92), cb, alpha);
            if (!img) break;
            if (type == 116) img = keyed(img, colorref(rop));
            blit(x, y, w, h, img, sx, sy, sw, sh, type == 76 || type == 77 ? rop : SRCCOPY, type == 114 ? (rop >> 16 & 255) / 255.0f : 1);
            break;
        }
        case 78: {   // MaskBlt
            double x = r.i32(24), y = r.i32(28), w = r.i32(32), h = r.i32(36);
            double sx = r.i32(44), sy = r.i32(48);
            SurfP img = dib(r.u32(84), r.u32(88), r.u32(92), r.u32(96), false);
            if (!img) break;
            SurfP mk = dib(r.u32(112), r.u32(116), r.u32(120), r.u32(124), false);
            if (mk) img = maskedBy(img, mk);
            blit(x, y, w, h, img, sx, sy, w, h, SRCCOPY);
            break;
        }
        case 80: {   // SetDIBitsToDevice: ..., usage, start scan, scans
            double x = r.i32(24), y = r.i32(28), sx = r.i32(32), sy = r.i32(36), w = r.i32(40), h = r.i32(44);
            DibPlace place;
            SurfP img = dib(r.u32(48), r.u32(52), r.u32(56), r.u32(60), false, &place, r.u32(68), r.u32(72));
            if (img) blit(x, y, w, h, img, sx, dibSrcY(place, sy, h), w, h, SRCCOPY);
            break;
        }
        case 81: {   // StretchDIBits
            double x = r.i32(24), y = r.i32(28), sx = r.i32(32), sy = r.i32(36), sw = r.i32(40), sh = r.i32(44);
            uint32_t rop = r.u32(68);
            double w = r.i32(72), h = r.i32(76);
            DibPlace place;
            SurfP img = dib(r.u32(48), r.u32(52), r.u32(56), r.u32(60), false, &place);
            if (img) blit(x, y, w, h, img, sx, dibSrcY(place, sy, sh), sw, sh, rop);
            break;
        }
        case 83: case 84: {   // ExtTextOutA / W
            double x = r.i32(36), y = r.i32(40);
            uint32_t n = r.u32(44), offStr = r.u32(48), options = r.u32(52), offDx = r.u32(72);
            if (n > 65536) break;
            double rc[4];
            rect(56, rc);
            cps.clear();
            if (type == 84) {
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t c = r.u16(offStr + i * 2);
                    if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) {
                        uint32_t lo = r.u16(offStr + i * 2 + 2);
                        if (lo >= 0xdc00 && lo < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00), i++;
                    }
                    cps.push_back(c);
                }
            } else if (r.has(offStr, n))
                decodeText(r.p + offStr, n, dc.font.charset, cps);
            dx.clear();
            if (offDx && r.has(offDx, (size_t)n * 4) && cps.size() == n) {
                size_t step = options & 0x2000 ? 8 : 4;   // ETO_PDY: (dx, dy) pairs
                for (uint32_t i = 0; i < n; i++) dx.push_back(r.i32(offDx + i * step));
            }
            text(x, y, cps, dx, options, rc);
            break;
        }
        case 118: {   // GradientFill: vertices (x, y, r, g, b, a as 16 bits), rectangles or triangles
            uint32_t nv = r.u32(24), nt = r.u32(28), mode = r.u32(32);
            if (nv > 65536 || nt > 65536) break;
            auto vx = [&](uint32_t i) { return (double)r.i32(36 + (size_t)i * 16); };
            auto vy = [&](uint32_t i) { return (double)r.i32(40 + (size_t)i * 16); };
            auto vc = [&](uint32_t i) {
                size_t at = 44 + (size_t)i * 16;
                return (uint32_t)(r.u16(at) >> 8) << 16 | (uint32_t)(r.u16(at + 2) >> 8) << 8 | r.u16(at + 4) >> 8;
            };
            size_t at = 36 + (size_t)nv * 16;
            if (mode == 2) {
                for (uint32_t t = 0; t < nt; t++) {
                    uint32_t i[3] = {r.u32(at + t * 12), r.u32(at + t * 12 + 4), r.u32(at + t * 12 + 8)};
                    if (i[0] >= nv || i[1] >= nv || i[2] >= nv) continue;
                    double x[3] = {vx(i[0]), vx(i[1]), vx(i[2])}, y[3] = {vy(i[0]), vy(i[1]), vy(i[2])};
                    uint32_t c[3] = {vc(i[0]), vc(i[1]), vc(i[2])}, mean = 0;
                    for (int s = 0; s < 24; s += 8) mean |= (((c[0] >> s & 255) + (c[1] >> s & 255) + (c[2] >> s & 255)) / 3) << s;
                    triangle(x, y, mean);
                }
            } else {
                for (uint32_t t = 0; t < nt; t++) {
                    uint32_t i0 = r.u32(at + t * 8), i1 = r.u32(at + t * 8 + 4);
                    if (i0 >= nv || i1 >= nv) continue;
                    gradientRect(vx(i0), vy(i0), vc(i0), vx(i1), vy(i1), vc(i1), mode == 1);
                }
            }
            break;
        }
        default: break;   // comments (EMF+ among them), palettes, ICM, escapes, PolyTextOut, ...
        }
    }
    flushMask();
    if (!(out.fx1 > out.fx0) || !(out.fy1 > out.fy0)) itemsFrame();
    return true;
}

// --- drawing the list -----------------------------------------------------------

struct Cancel {
    int (*fn)(void *);
    void *ctx;
    bool operator()() const { return fn && fn(ctx); }
};
constexpr double kFar = 1 << 24;   // canvas pixels out of the view

void setColor(plutovg_canvas_t *c, uint32_t rgb, float a) {
    plutovg_canvas_set_rgba(c, (rgb >> 16 & 255) / 255.0f, (rgb >> 8 & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}

// plutovg samples a texture in 16.16 fixed point: a bitmap's pixels per canvas pixel in a sane range,
// and (a tiled pattern's) pixel coordinates over the visible part of the item below 2^15
bool textureFits(const Item &it, const plutovg_matrix_t &base, const double view[4]) {
    double sx = std::hypot(base.a, base.b) * std::hypot(it.m.a, it.m.b), sy = std::hypot(base.c, base.d) * std::hypot(it.m.c, it.m.d);
    if (!(sx > 1e-3 && sx < 1e3 && sy > 1e-3 && sy < 1e3)) return false;
    plutovg_matrix_t inv;
    if (!plutovg_matrix_invert(&it.m, &inv)) return false;
    double x0 = std::max((double)it.bx0, view[0]), y0 = std::max((double)it.by0, view[1]);
    double x1 = std::min((double)it.bx1, view[2]), y1 = std::min((double)it.by1, view[3]);
    for (int k = 0; k < 4; k++) {
        plutovg_point_t p = {(float)(k & 1 ? x1 : x0), (float)(k & 2 ? y1 : y0)}, q;
        plutovg_matrix_map_point(&inv, &p, &q);
        if (!(std::fabs(q.x) < 30000 && std::fabs(q.y) < 30000)) return false;
    }
    return true;
}

// a clip path's extents against the view: 1 clip with it, 0 it covers the view (no effect), -1 it
// lies outside the view or too far for plutovg's fixed point (nothing left to draw)
int clipFits(const plutovg_path_t *path, float hair, const double view[4]) {
    plutovg_rect_t r;
    plutovg_path_extents(path, &r, false);
    double x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    if (x1 < view[0] || x0 > view[2] || y1 < view[1] || y0 > view[3]) return -1;
    if (std::max(std::max(view[0] - x0, x1 - view[2]), std::max(view[1] - y0, y1 - view[3])) / hair <= kFar) return 1;
    return x0 <= view[0] && y0 <= view[1] && x1 >= view[2] && y1 >= view[3] ? 0 : -1;
}

// base: device to the canvas; hair: a canvas pixel in device units; view: the visible device rectangle
int drawList(const nitrometa &meta, plutovg_canvas_t *c, const plutovg_matrix_t &base, float hair, const double view[4], const Cancel &cancel) {
    plutovg_canvas_set_matrix(c, &base);
    plutovg_canvas_save(c);
    std::vector<float> dashes;
    plutovg_path_t *lines = plutovg_path_create();
    size_t n = 0;
    int rc = 0;
    for (const Item &it : meta.items) {
        if ((++n & 255) == 0 && cancel()) {
            rc = 1;
            break;
        }
        if (it.kind == Item::CLIP) {
            plutovg_canvas_restore(c);
            plutovg_canvas_save(c);
            plutovg_canvas_set_matrix(c, &base);
            for (const Clip &cl : it.clips) {
                int fit = clipFits(cl.path.get(), hair, view);
                if (fit == 0) continue;
                plutovg_canvas_set_fill_rule(c, cl.evenodd ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
                if (fit > 0) plutovg_canvas_clip_path(c, cl.path.get());
                else plutovg_canvas_clip_rect(c, 0, 0, 0, 0);
            }
            continue;
        }
        if (it.bx1 + 2 * hair < view[0] || it.bx0 - 2 * hair > view[2] || it.by1 + 2 * hair < view[1] || it.by0 - 2 * hair > view[3]) continue;
        if (std::max(std::max(view[0] - it.bx0, it.bx1 - view[2]), std::max(view[1] - it.by0, it.by1 - view[3])) / hair > kFar)
            continue;   // (plutovg's fixed point would overflow: far-off strays of a damaged file)
        plutovg_canvas_set_fill_rule(c, it.evenodd ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
        if (it.img && !textureFits(it, base, view)) continue;
        switch (it.kind) {
        case Item::FILL:
            if (it.img) {
                plutovg_canvas_set_texture(c, it.img->s, PLUTOVG_TEXTURE_TYPE_TILED, it.opacity, &it.m);
            } else if (it.gradient) {
                plutovg_gradient_stop_t stops[2];
                for (int i = 0; i < 2; i++) {
                    uint32_t rgb = i ? it.rgb2 : it.rgb;
                    stops[i].offset = (float)i;
                    stops[i].color = {(rgb >> 16 & 255) / 255.0f, (rgb >> 8 & 255) / 255.0f, (rgb & 255) / 255.0f, 1};
                }
                plutovg_canvas_set_linear_gradient(c, it.g[0], it.g[1], it.g[2], it.g[3], PLUTOVG_SPREAD_METHOD_PAD, stops, 2, nullptr);
            } else
                setColor(c, it.rgb, it.opacity);
            plutovg_canvas_fill_path(c, it.path.get());
            break;
        case Item::STROKE: {
            setColor(c, it.rgb, it.opacity);
            float w = std::max(it.width, hair);
            plutovg_canvas_set_line_width(c, w);
            plutovg_canvas_set_line_cap(c, it.width > 0 ? (plutovg_line_cap_t)it.cap : PLUTOVG_LINE_CAP_BUTT);
            plutovg_canvas_set_line_join(c, it.width > 0 ? (plutovg_line_join_t)it.join : PLUTOVG_LINE_JOIN_BEVEL);
            if (!it.dash.empty()) {
                dashes.resize(it.dash.size());
                for (size_t i = 0; i < it.dash.size(); i++) dashes[i] = std::max(it.dash[i] * w, hair);
                plutovg_canvas_set_dash(c, 0, dashes.data(), (int)dashes.size());
            } else
                plutovg_canvas_set_dash(c, 0, nullptr, 0);
            plutovg_canvas_stroke_path(c, it.path.get());
            break;
        }
        case Item::IMAGE:
            plutovg_canvas_set_texture(c, it.img->s, PLUTOVG_TEXTURE_TYPE_PLAIN, it.opacity, &it.m);
            plutovg_canvas_fill_path(c, it.path.get());
            break;
        case Item::HATCH: {   // GDI's hatches: lines 8 device pixels apart, clipped to the shape
            plutovg_canvas_save(c);
            plutovg_canvas_clip_path(c, it.path.get());
            setColor(c, it.rgb, 1);
            plutovg_canvas_set_line_width(c, hair);
            plutovg_canvas_set_dash(c, 0, nullptr, 0);
            plutovg_path_reset(lines);
            double x0 = std::max((double)it.bx0, view[0]), y0 = std::max((double)it.by0, view[1]);
            double x1 = std::min((double)it.bx1, view[2]), y1 = std::min((double)it.by1, view[3]);
            float step = std::max(8.0f, 4 * hair);
            if (it.hatch == 0 || it.hatch == 4)
                for (double y = std::floor(y0 / step) * step; y <= y1; y += step) plutovg_path_move_to(lines, (float)x0, (float)y), plutovg_path_line_to(lines, (float)x1, (float)y);
            if (it.hatch == 1 || it.hatch == 4)
                for (double x = std::floor(x0 / step) * step; x <= x1; x += step) plutovg_path_move_to(lines, (float)x, (float)y0), plutovg_path_line_to(lines, (float)x, (float)y1);
            if (it.hatch == 2 || it.hatch == 5)   // \ (downward to the right)
                for (double d = std::floor((x0 - y1) / step) * step; d <= x1 - y0; d += step)
                    plutovg_path_move_to(lines, (float)(y0 + d), (float)y0), plutovg_path_line_to(lines, (float)(y1 + d), (float)y1);
            if (it.hatch == 3 || it.hatch == 5)   // /
                for (double d = std::floor((x0 + y0) / step) * step; d <= x1 + y1; d += step)
                    plutovg_path_move_to(lines, (float)(d - y0), (float)y0), plutovg_path_line_to(lines, (float)(d - y1), (float)y1);
            plutovg_canvas_stroke_path(c, lines);
            plutovg_canvas_restore(c);
            break;
        }
        default: break;
        }
    }
    plutovg_path_destroy(lines);
    plutovg_canvas_restore(c);
    return rc;
}

}   // namespace

// --- the interface ------------------------------------------------------------

nitrometa *nitrometa_load(const void *data, size_t len) {
    if (!data || len < 18) return nullptr;
    Bytes b{(const uint8_t *)data, len};
    std::unique_ptr<nitrometa> meta(new nitrometa);
    std::lock_guard<std::mutex> g(nf_lock());   // (the glyph outlines of the text)
    bool ok = false;
    if (b.u32(0) == 1 && b.u32(40) == 0x464d4520) {
        Player p(*meta, true);
        ok = p.playEmf(b);
    } else if (b.u32(0) == 0x9ac6cdd7) {   // the placeable header: key, handle, bounding box, units per inch, reserved, checksum
        double l = b.i16(6), t = b.i16(8), r = b.i16(10), bt = b.i16(12);
        unsigned inch = b.u16(14);
        if (!inch) inch = 96;
        double w = std::fabs(r - l), h = std::fabs(bt - t);
        if (!(w > 0) || !(h > 0) || b.u16(22) > 2 || b.u16(24) != 9) return nullptr;
        Player p(*meta, false);
        p.placeable = true;
        p.dpi = inch;
        p.dc.wox = std::min(l, r), p.dc.woy = std::min(t, bt), p.dc.wex = w, p.dc.wey = h;
        p.dc.vex = w, p.dc.vey = h, p.dc.vextSet = true;
        p.dc.mapMode = MM_ANISOTROPIC;   // (as the applications play them: the file's window extents count, a negative one flips)
        meta->unit = 72.0 / inch;
        meta->fx0 = 0, meta->fy0 = 0, meta->fx1 = w, meta->fy1 = h;
        ok = p.playWmf(b, 22);
    } else if (b.u16(0) <= 2 && b.u16(2) == 9 && (b.u16(4) == 0x100 || b.u16(4) == 0x300)) {
        Player p(*meta, false);
        ok = p.playWmf(b, 0);
        p.finish();
    }
    if (!ok || meta->items.empty() || !(meta->fx1 > meta->fx0) || !(meta->fy1 > meta->fy0)) return nullptr;
    return meta.release();
}

void nitrometa_free(nitrometa *meta) { delete meta; }

void nitrometa_size(const nitrometa *meta, double *w, double *h) {
    *w = (meta->fx1 - meta->fx0) * meta->unit;
    *h = (meta->fy1 - meta->fy0) * meta->unit;
}

void nitrometa_draw(const nitrometa *meta, plutovg_canvas_t *canvas, float x, float y, float w, float h) {
    if (!meta || !(w > 0) || !(h > 0)) return;
    plutovg_canvas_save(canvas);
    plutovg_canvas_clip_rect(canvas, x, y, w, h);
    plutovg_canvas_translate(canvas, x, y);
    plutovg_canvas_scale(canvas, (float)(w / (meta->fx1 - meta->fx0)), (float)(h / (meta->fy1 - meta->fy0)));
    plutovg_canvas_translate(canvas, (float)-meta->fx0, (float)-meta->fy0);
    plutovg_matrix_t base;
    plutovg_canvas_get_matrix(canvas, &base);
    float scale = std::sqrt(std::fabs(base.a * base.d - base.b * base.c));
    float hair = scale > 0 ? 1 / scale : 1;
    double view[4] = {meta->fx0, meta->fy0, meta->fx1, meta->fy1};
    drawList(*meta, canvas, base, hair, view, Cancel{nullptr, nullptr});
    plutovg_canvas_restore(canvas);
}

void nitrometa_set_background(nitrometa *meta, int background) {
    if (meta && background >= 0 && background <= 2) meta->background = background;
}

int nitrometa_render(nitrometa *meta, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0, int (*cancelled)(void *ctx),
                     void *ctx) {
    if (!meta || w <= 0 || h <= 0 || pitch < (size_t)w * 4 || !(scale > 0)) return -1;
    static const uint32_t backgrounds[3] = {0xffffffff, 0xff212830, 0xff000000};
    uint32_t bg = backgrounds[meta->background];
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(bgra + (size_t)y * pitch);
        for (int x = 0; x < w; x++) row[x] = bg;
    }
    plutovg_surface_t *surface = plutovg_surface_create_for_data(bgra, w, h, (int)pitch);
    plutovg_canvas_t *c = plutovg_canvas_create(surface);
    double s = scale * meta->unit;   // canvas pixels per device unit
    plutovg_matrix_t base = {(float)s, 0, 0, (float)s, (float)(-scale * (meta->fx0 * meta->unit + x0)), (float)(-scale * (meta->fy0 * meta->unit + y0))};
    double vx0 = meta->fx0 + x0 / meta->unit, vy0 = meta->fy0 + y0 / meta->unit;
    double view[4] = {vx0, vy0, vx0 + w / s, vy0 + h / s};
    int rc = drawList(*meta, c, base, (float)(1 / s), view, Cancel{cancelled, ctx});
    plutovg_canvas_destroy(c);
    plutovg_surface_destroy(surface);
    return rc;
}
