// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrocdr: CorelDRAW drawings through libcdr, drawn with plutovg. See nitrocdr.h.
//
// libcdr reports the drawing through librevenge::RVNGDrawingInterface (the calls the
// SVG / ODG generators get): pages, styles, paths, bitmaps (as BMP files) and text
// frames of lines and spans. The Painter below records that as a display list in
// points (y down); render() replays it with plutovg for any scale / offset, skipping
// whatever lies outside the target.
#include "nitrocdr.h"
#include "nitro_font.h"

#include <libcdr/libcdr.h>
#include <librevenge-stream/librevenge-stream.h>
#include <librevenge/librevenge.h>
#include <plutovg.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#ifndef M_PI   // (not defined by mingw's math.h)
#define M_PI 3.14159265358979323846
#endif

namespace {

using librevenge::RVNGProperty;
using librevenge::RVNGPropertyList;
using librevenge::RVNGPropertyListVector;
using librevenge::RVNGString;

// every plutovg call that may touch a face runs under nitro_font's lock: the faces are shared
// with nitrosvg and nitrodxf, and their glyph caches have no lock of their own

// --- properties --------------------------------------------------------------

double points(const RVNGProperty *p, double def = 0.0) {   // lengths: inch (default), point, twip
    if (!p) return def;
    switch (p->getUnit()) {
    case librevenge::RVNG_POINT: return p->getDouble();
    case librevenge::RVNG_TWIP: return p->getDouble() / 20.0;
    case librevenge::RVNG_PERCENT:
    case librevenge::RVNG_UNIT_ERROR: return def;
    default: return p->getDouble() * 72.0;   // inch, generic
    }
}
double number(const RVNGProperty *p, double def = 0.0) { return p ? p->getDouble() : def; }
bool is(const RVNGPropertyList &l, const char *key, const char *value) {
    return l[key] && l[key]->getStr() == value;
}

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};
Color color(const RVNGProperty *p, Color def = Color()) {   // "#rrggbb"
    if (!p) return def;
    RVNGString s = p->getStr();
    const char *c = s.cstr();
    if (*c == '#') c++;
    unsigned v = 0;
    if (sscanf(c, "%6x", &v) != 1) return def;
    Color o;
    o.r = ((v >> 16) & 255) / 255.0f;
    o.g = ((v >> 8) & 255) / 255.0f;
    o.b = (v & 255) / 255.0f;
    return o;
}

// --- display list --------------------------------------------------------------

struct Image {
    plutovg_surface_t *s = nullptr;
    ~Image() { plutovg_surface_destroy(s); }
};
// libcdr's own bitmaps: uncompressed 32-bit BMPs (B G R A, bottom-up), alpha all 0 when opaque;
// read here directly, as stb_image would (twice as fast as stb_image + plutovg's conversion)
static plutovg_surface_t *bmp32(const unsigned char *b, size_t n) {
    auto u32 = [b](size_t o) { return (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24; };
    if (n < 54 || b[0] != 'B' || b[1] != 'M' || u32(14) != 40 || (b[28] | b[29] << 8) != 32 || u32(30) != 0) return nullptr;
    uint32_t off = u32(10), w = u32(18);
    int32_t h = (int32_t)u32(22);
    uint32_t ah = h < 0 ? 0u - (uint32_t)h : (uint32_t)h;
    if (!w || !ah || w > 65535 || ah > 65535 || off > n || (n - off) / 4 / w < ah) return nullptr;
    const unsigned char *px = b + off;
    size_t count = (size_t)w * ah;
    bool alpha = false;
    for (size_t i = 0; i < count && !alpha; i++) alpha = px[i * 4 + 3] != 0;
    plutovg_surface_t *s = plutovg_surface_create((int)w, (int)ah);
    if (!s) return nullptr;
    unsigned char *data = plutovg_surface_get_data(s);
    int stride = plutovg_surface_get_stride(s);
    for (uint32_t y = 0; y < ah; y++) {
        const unsigned char *src = px + (size_t)(h > 0 ? ah - 1 - y : y) * w * 4;
        uint32_t *dst = (uint32_t *)(data + (size_t)stride * y);
        for (uint32_t x = 0; x < w; x++, src += 4) {
            uint32_t a = alpha ? src[3] : 255, r = src[2], g = src[1], bl = src[0];
            if (a != 255) {   // (premultiplied, as plutovg_convert_rgba_to_argb)
                r = r * a / 255;
                g = g * a / 255;
                bl = bl * a / 255;
            }
            dst[x] = a ? a << 24 | r << 16 | g << 8 | bl : 0;
        }
    }
    return s;
}

// A grayscale Photoshop image (8 bits, its merged image raw or PackBits compressed; embedded in
// CorelDRAW documents as OLE objects): stb_image reads RGB ones only, these were left out.
plutovg_surface_t *psdGray(const unsigned char *b, size_t n) {
    auto u16 = [b](size_t o) { return (uint32_t)(b[o] << 8 | b[o + 1]); };
    auto u32 = [b](size_t o) { return (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3]; };
    if (n < 26 || memcmp(b, "8BPS", 4) || u16(4) != 1 || u16(22) != 8 || u16(24) != 1) return nullptr;
    uint32_t channels = u16(12), h = u32(14), w = u32(18);
    if (!channels || !w || !h || w > 30000 || h > 30000) return nullptr;
    size_t o = 26;
    for (int i = 0; i < 3; i++) {   // the colour mode data, the image resources, the layers: skipped
        if (o + 4 > n) return nullptr;
        uint32_t len = u32(o);
        if (len > n - o - 4) return nullptr;
        o += 4 + len;
    }
    if (o + 2 > n) return nullptr;
    uint32_t compression = u16(o);
    o += 2;
    std::vector<unsigned char> gray((size_t)w * h);
    if (compression == 0) {
        if (n - o < gray.size()) return nullptr;
        memcpy(gray.data(), b + o, gray.size());
    } else if (compression == 1) {   // each row's byte count (all channels), then the rows
        size_t counts = o;
        if ((n - o) / 2 < (size_t)channels * h) return nullptr;
        o += (size_t)channels * h * 2;
        for (uint32_t y = 0; y < h; y++) {
            size_t end = o + u16(counts + 2 * y);
            if (end > n) return nullptr;
            unsigned char *row = gray.data() + (size_t)w * y;
            uint32_t x = 0;
            while (o < end && x < w) {
                int c = (signed char)b[o++];
                if (c >= 0) {
                    for (int i = 0; i <= c && o < end && x < w; i++) row[x++] = b[o++];
                } else if (c != -128 && o < end) {
                    unsigned char v = b[o++];
                    for (int i = 0; i <= -c && x < w; i++) row[x++] = v;
                }
            }
            o = end;
        }
    } else
        return nullptr;
    plutovg_surface_t *s = plutovg_surface_create((int)w, (int)h);
    if (!s) return nullptr;
    unsigned char *data = plutovg_surface_get_data(s);
    int stride = plutovg_surface_get_stride(s);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *dst = (uint32_t *)(data + (size_t)stride * y);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = gray[(size_t)w * y + x];
            dst[x] = 0xff000000u | v << 16 | v << 8 | v;
        }
    }
    return s;
}

std::shared_ptr<Image> image(const RVNGProperty *p) {   // a BMP (or other stb_image format) property
    const librevenge::RVNGBinaryData *d = p ? p->getBinaryData() : nullptr;
    if (!d || !d->size()) return nullptr;
    auto img = std::make_shared<Image>();
    img->s = bmp32(d->getDataBuffer(), d->size());
    if (!img->s) img->s = psdGray(d->getDataBuffer(), d->size());
    if (!img->s) img->s = plutovg_surface_load_from_image_data(d->getDataBuffer(), (int)d->size());
    return img->s ? img : nullptr;
}

enum Fill { FILL_NONE, FILL_SOLID, FILL_LINEAR, FILL_RADIAL, FILL_CONICAL, FILL_SQUARE, FILL_BITMAP };

struct Style {
    Fill fill = FILL_NONE;
    Color fillColor;
    float opacity = 1;   // of the fill
    bool evenodd = false;
    std::vector<plutovg_gradient_stop_t> stops;
    float angle = 0;           // linear: degrees (librevenge / ODF)
    float cx = .5f, cy = .5f, r = 1;   // radial: in bounding box units
    // a CorelDRAW fountain fill (libcdr:gradient-*): its stops from the start colour to the end
    // colour; angle (counter-clockwise), centre offset and edge pad (shares) of its own
    bool corel = false;
    float centerX = 0, centerY = 0, edge = 0;
    mutable std::shared_ptr<Image> gradTile;   // conical / square: drawn once over the bounding box
    std::shared_ptr<Image> tile;       // bitmap / pattern fill
    float tileW = 0, tileH = 0;        // points, or fractions of the bounding box (tileRel)
    bool tileRel = false;
    float refX = 0, refY = 0;          // tile offset (fractions of a tile)
    bool stroke = false;
    Color strokeColor;
    float strokeWidth = 0;   // 0: hairline
    plutovg_line_cap_t cap = PLUTOVG_LINE_CAP_BUTT;
    plutovg_line_join_t join = PLUTOVG_LINE_JOIN_MITER;
    std::vector<float> dashes;
    std::string markerStart, markerEnd;   // arrowheads: SVG path data in their own units (libcdr's)
    float markerUnit = 0;                 // one of those, in points; the tip at the line's end, y along it
};

struct Span {
    plutovg_font_face_t *face = nullptr;   // owned by the font cache
    float size = 12;
    float scaleX = 1;   // horizontal stretch (CorelDRAW text squeezed to fit, style:text-scale)
    float shiftX = 0, shiftY = 0;   // moved from its place, in em (up positive; libcdr:shift-x / -y)
    float rotate = 0;   // turned about its place, in degrees counter-clockwise (libcdr:rotate)
    float letterSpacing = 0, wordSpacing = 1;   // the character spacing / a space's width: shares of a
                                                // space's width (libcdr:letter-spacing / -word-spacing)
    Color color;
    bool fill = true;      // (libcdr: draw:fill none on the span: outlined characters only)
    bool stroke = false;   // the characters' outline (libcdr: draw:stroke on the span)
    Color strokeColor;
    float strokeWidth = 0;
    std::string text;
    float width = 0;
    std::string font;           // the font asked for (and its style), for characters its face lacks
    bool bold = false, italic = false;
};
// A span's text's width (unstretched): with character or word spacing the characters' own widths
// (no kerning), a share of a space's width added after each character but a space, and a space
// its word spacing less that (a word's last character and its space: the word spacing alone; as
// measured on CorelDRAW's PDF exports)
float textWidth(const Span &s, const char *text, size_t len) {
    if (!s.face) return 0;
    if (s.letterSpacing == 0 && s.wordSpacing == 1)
        return plutovg_font_face_text_extents(s.face, s.size, text, (int)len, PLUTOVG_TEXT_ENCODING_UTF8, nullptr);
    float space = plutovg_font_face_text_extents(s.face, s.size, " ", 1, PLUTOVG_TEXT_ENCODING_UTF8, nullptr);
    float w = 0;
    for (size_t i = 0; i < len;) {
        size_t e = i + 1;
        while (e < len && (text[e] & 0xc0) == 0x80) e++;
        bool blank = e - i == 1 && text[i] == ' ';
        w += blank ? space * (s.wordSpacing - s.letterSpacing)
                   : plutovg_font_face_text_extents(s.face, s.size, text + i, (int)(e - i), PLUTOVG_TEXT_ENCODING_UTF8, nullptr) +
                         space * s.letterSpacing;
        i = e;
    }
    return w;
}

struct Line {
    int align = 0;   // 0 left, 1 center, 2 right
    std::vector<Span> spans;
    float width = 0, size = 0, ascent = 0, descent = 0;
    float height = 0;   // from the previous line's baseline to its own (see layout())
    float spacing = 1;  // CorelDRAW's line spacing (1: 100%)
    float lineHeight = 0;   // or its spacing in points (> 0: instead of that)
    bool paragraph = false;   // the first line of a paragraph: before it, the paragraph spacing
    float before = 1, after = 0;   // the paragraph's spacing before / after it (1: 100%; < 0: points)
    plutovg_font_face_t *emptyFace = nullptr;   // an empty line's font: the line break's (libcdr gives
    float emptySize = 0;                        // a line break the text's size) or the empty paragraph's
};
struct Text {
    float x = 0, y = 0, w = 0, h = 0, rotate = 0;   // frame (w = h = 0: artistic text at x, y)
    std::vector<Line> lines;
};

struct Item {
    enum Kind { PATH, IMAGE, TEXT } kind = PATH;
    std::shared_ptr<Style> style;
    std::unique_ptr<plutovg_path_t, void (*)(plutovg_path_t *)> path{nullptr, plutovg_path_destroy};
    std::shared_ptr<Image> img;
    float x = 0, y = 0, w = 0, h = 0, rotate = 0;   // image rectangle (rotated about its centre)
    bool mirrorX = false, mirrorY = false;
    std::unique_ptr<Text> text;
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;   // bounding box (culling)
};

struct Page {
    double x = 0, y = 0, w = 0, h = 0;   // shown area: the drawing's bounding box (see nitrocdr_load)
    std::vector<Item> items;
};

// --- fonts -------------------------------------------------------------------

bool ends_with(const std::string &s, const char *t) {
    size_t n = strlen(t);
    return s.size() >= n && !strcasecmp(s.c_str() + s.size() - n, t);
}

// Bitstream's names of old CorelDRAW fonts: "Futura Lt BT", "AvantGarde Bk BT", "Futura XBlkCnIt BT":
// the family and the weight / italic of the abbreviations in its last word (width ones dropped)
static bool bitstream_name(std::string &family, int &weight, bool &italic) {
    if (!ends_with(family, " BT")) return false;
    family.resize(family.size() - 3);
    size_t sp = family.rfind(' ');
    if (sp == std::string::npos) return true;
    static const struct { const char *abbr; int weight; } abbrs[] = {   // weight -1: italic, 0: width
        {"XBlk", 900}, {"Blk", 900}, {"XBd", 800}, {"Bd", 700}, {"Hv", 800}, {"It", -1}, {"XLt", 200}, {"Lt", 300},
        {"Bk", 400}, {"Md", 500}, {"XCn", 0}, {"Cn", 0}, {"Ex", 0}};
    int w = weight;
    bool it = italic;
    for (size_t i = sp + 1; i < family.size();) {
        bool found = false;
        for (const auto &a : abbrs) {
            size_t n = strlen(a.abbr);
            if (!family.compare(i, n, a.abbr)) {
                if (a.weight > 0) w = std::max(a.weight, weight > 400 ? weight : 0);   // (bold stays bold)
                if (a.weight < 0) it = true;
                i += n;
                found = true;
                break;
            }
        }
        if (!found) return true;   // (an ordinary word)
    }
    family.resize(sp);
    weight = w;
    italic = it;
    return true;
}

// Families of old CorelDRAW drawings that macOS does not have: the closest one it has; and the
// Windows / macOS core fonts that Linux has only as their metric-compatible Liberation fonts.
// (Adobe Caslon Pro: Times New Roman's widths are closest, as a CD label's lettering moved one
// by one along its circle shows; with Arial it ran off the disc.)
static const char *substitute(const std::string &family) {
    static const char *const subst[][2] = {{"AvantGarde", "Avenir"}, {"Century Gothic", "Avenir"},
                                           {"Adobe Caslon Pro", "Times New Roman"},
                                           // Bitstream scripts: like ones macOS has, then Windows'
                                           {"Lydian Csv", "Apple Chancery"}, {"CommercialScript", "Snell Roundhand"},
                                           {"Apple Chancery", "Lucida Handwriting"}, {"Snell Roundhand", "Lucida Handwriting"},
                                           {"Arial", "Liberation Sans"}, {"Helvetica", "Liberation Sans"},
                                           {"Times New Roman", "Liberation Serif"}, {"Times", "Liberation Serif"},
                                           {"Courier New", "Liberation Mono"}, {"Courier", "Liberation Mono"}};
    for (const auto &e : subst)
        if (!strcasecmp(family.c_str(), e[0])) return e[1];
    return nullptr;
}

// CorelDRAW font names: "Arial", "Arial CE" (Central European variants of old fonts),
// "Times New Roman Bold", "Futura Lt BT" ... -> a face of the system fonts, falling back to
// Arial / Helvetica (Linux: Liberation Sans / DejaVu Sans).
plutovg_font_face_t *find_font(const std::string &name, bool bold, bool italic, std::string *familyOut = nullptr) {
    std::string family = name;
    int weight = bold ? 700 : 400;
    bool bitstream = bitstream_name(family, weight, italic);
    for (bool changed = true; changed;) {
        changed = false;
        static const char *const suffixes[] = {" CE", " Cyr", " Baltic", " Greek", " Tur"};
        for (const char *sfx : suffixes)
            if (ends_with(family, sfx)) { family.resize(family.size() - strlen(sfx)); changed = true; }
        static const char *const bolds[] = {" Bold", " Black", " Heavy", " Semibold"};
        for (const char *sfx : bolds)
            if (ends_with(family, sfx)) { family.resize(family.size() - strlen(sfx)); weight = std::max(weight, 700); changed = true; }
        static const char *const italics[] = {" Italic", " Oblique"};
        for (const char *sfx : italics)
            if (ends_with(family, sfx)) { family.resize(family.size() - strlen(sfx)); italic = changed = true; }
    }
    if (familyOut) *familyOut = family;
    const char *sub = substitute(family);
    const char *sub2 = sub ? substitute(sub) : nullptr;   // (a substitute's own: Linux's Liberation one)
    // a Bitstream font also under Linotype's name of the family ("Futura Lt BT": "Futura LT" Light)
    std::string linotype = bitstream ? family + " LT" : family;
    const char *tries[] = {name.c_str(), linotype.c_str(), family.c_str(), sub ? sub : family.c_str(), sub2 ? sub2 : family.c_str(),
                           "Arial", "Helvetica", "Liberation Sans", "DejaVu Sans"};   // (from [5]: the last resorts)
    for (const char *t : tries) {
        // upright text: not from a family that has only italic faces (an installed "Futura Lt BT"
        // Light Italic alone made every Futura text italic); the next candidate instead
        if (!italic && t != tries[5] && t != tries[6] && t != tries[7] && t != tries[8] &&
            !nf_has_style(t, false) && nf_has_style(t, true))
            continue;
        plutovg_font_face_t *f = nf_face(t, weight, italic);
        if (f) return f;
    }
    return nullptr;   // (a family without that style gives its closest face)
}

// A face for a character the font lacks (old Western fonts such as AvantGarde Bk BT have no ő / ű:
// their "missing glyph" box showed): the family's substitute, then common fonts; null if none has it.
plutovg_font_face_t *fallback_font(const std::string &name, bool bold, bool italic, plutovg_codepoint_t cp) {
    std::string family;
    find_font(name, bold, italic, &family);
    const char *sub = substitute(family);
    const char *tries[] = {sub ? sub : "Arial", "Arial", "Helvetica", "Lucida Grande", "Arial Unicode MS", "Liberation Sans",
                           "DejaVu Sans"};
    for (const char *t : tries) {
        plutovg_font_face_t *f = find_font(t, bold, italic);
        if (f && nf_has_glyph(f, cp)) return f;
    }
    return nullptr;
}

// --- the painter -------------------------------------------------------------

class Painter : public librevenge::RVNGDrawingInterface {
public:
    std::vector<Page> pages;

    void startDocument(const RVNGPropertyList &) override {}
    void endDocument() override {}
    void setDocumentMetaData(const RVNGPropertyList &) override {}
    void defineEmbeddedFont(const RVNGPropertyList &) override {}
    void startPage(const RVNGPropertyList &p) override {
        pages.emplace_back();
        pages.back().w = points(p["svg:width"], 595.0);
        pages.back().h = points(p["svg:height"], 842.0);
        m_inPage = true;
    }
    void endPage() override { m_inPage = false; }
    void startMasterPage(const RVNGPropertyList &) override { m_inMaster = true; }
    void endMasterPage() override { m_inMaster = false; }
    void setStyle(const RVNGPropertyList &p) override { m_style = parseStyle(p); }
    void startLayer(const RVNGPropertyList &) override {}
    void endLayer() override {}
    void startEmbeddedGraphics(const RVNGPropertyList &) override {}
    void endEmbeddedGraphics() override {}
    void openGroup(const RVNGPropertyList &) override {}
    void closeGroup() override {}

    // (libcdr draws every shape, CMX ones too, as a path: never these)
    void drawRectangle(const RVNGPropertyList &) override {}
    void drawEllipse(const RVNGPropertyList &) override {}
    void drawPolygon(const RVNGPropertyList &) override {}
    void drawPolyline(const RVNGPropertyList &) override {}
    void drawPath(const RVNGPropertyList &p) override {
        const RVNGPropertyListVector *d = p.child("svg:d");
        if (!d || !d->count()) return;
        addPath(buildPath(*d));
    }
    // a librevenge path (svg:d)
    static plutovg_path_t *buildPath(const RVNGPropertyListVector &dv) {
        const RVNGPropertyListVector *d = &dv;
        plutovg_path_t *path = plutovg_path_create();
        for (unsigned long i = 0; i < d->count(); i++) {
            const RVNGPropertyList &e = (*d)[i];
            if (!e["librevenge:path-action"]) continue;
            RVNGString a = e["librevenge:path-action"]->getStr();
            float x = (float)points(e["svg:x"]), y = (float)points(e["svg:y"]);
            switch (a.cstr()[0]) {
            case 'M': plutovg_path_move_to(path, x, y); break;
            case 'L': plutovg_path_line_to(path, x, y); break;
            case 'H': plutovg_path_line_to(path, x, currentY(path)); break;
            case 'V': plutovg_path_line_to(path, currentX(path), y); break;
            case 'C':
                plutovg_path_cubic_to(path, (float)points(e["svg:x1"]), (float)points(e["svg:y1"]), (float)points(e["svg:x2"]),
                                      (float)points(e["svg:y2"]), x, y);
                break;
            case 'Q': plutovg_path_quad_to(path, (float)points(e["svg:x1"]), (float)points(e["svg:y1"]), x, y); break;
            case 'A':
                plutovg_path_arc_to(path, (float)points(e["svg:rx"]), (float)points(e["svg:ry"]),
                                    (float)(number(e["librevenge:rotate"]) * M_PI / 180), e["librevenge:large-arc"] && e["librevenge:large-arc"]->getInt(),
                                    e["librevenge:sweep"] && e["librevenge:sweep"]->getInt(), x, y);
                break;
            case 'Z': plutovg_path_close(path); break;
            default: break;
            }
        }
        return path;
    }
    void drawGraphicObject(const RVNGPropertyList &p) override {
        if (!m_inPage || m_inMaster) return;
        std::shared_ptr<Image> img = cachedImage(p["office:binary-data"]);
        if (!img) return;
        Item it;
        it.kind = Item::IMAGE;
        it.style = m_style;
        it.img = img;
        it.x = (float)points(p["svg:x"]);
        it.y = (float)points(p["svg:y"]);
        it.w = (float)points(p["svg:width"]);
        it.h = (float)points(p["svg:height"]);
        it.rotate = (float)number(p["librevenge:rotate"]);
        it.mirrorX = p["draw:mirror-horizontal"] && p["draw:mirror-horizontal"]->getInt();
        it.mirrorY = p["draw:mirror-vertical"] && p["draw:mirror-vertical"]->getInt();
        float cx = it.x + it.w / 2, cy = it.y + it.h / 2;
        rotatedBox(it, cx, cy, it.x, it.y, it.x + it.w, it.y + it.h, it.rotate);
        // the bitmap's outline (libcdr:clip-path): it shows only inside (cropped bitmaps, the
        // pattern tiles of imported PDF plans); the clip, in page coordinates, goes in it.path
        if (const RVNGPropertyListVector *clip = p.child("libcdr:clip-path")) {
            it.path.reset(buildPath(*clip));
            plutovg_rect_t r;
            plutovg_path_extents(it.path.get(), &r, false);
            it.bx0 = std::max(it.bx0, r.x);
            it.by0 = std::max(it.by0, r.y);
            it.bx1 = std::min(it.bx1, r.x + r.w);
            it.by1 = std::min(it.by1, r.y + r.h);
            if (it.bx1 <= it.bx0 || it.by1 <= it.by0) return;   // (nothing of it shows)
        }
        add(std::move(it));
    }
    void drawConnector(const RVNGPropertyList &) override {}

    void startTextObject(const RVNGPropertyList &p) override {
        m_text.reset(new Text);
        m_paragraphOpened = false;
        m_text->x = (float)points(p["svg:x"]);
        m_text->y = (float)points(p["svg:y"]);
        m_text->w = (float)points(p["svg:width"]);
        m_text->h = (float)points(p["svg:height"]);
        m_text->rotate = (float)number(p["librevenge:rotate"]);
    }
    void endTextObject() override {
        if (!m_text) return;
        std::unique_ptr<Text> t = std::move(m_text);
        while (!t->lines.empty() && t->lines.back().spans.empty()) t->lines.pop_back();
        if (t->lines.empty() || !m_inPage || m_inMaster) return;
        // only spaces: nothing to draw (left-over texts far off the drawing would stretch its bounding box)
        bool blank = true;
        for (const Line &l : t->lines)
            for (const Span &sp : l.spans)
                if (sp.text.find_first_not_of(" \t\xC2\xA0") != std::string::npos) blank = false;
        if (blank) return;
        Item it;
        it.kind = Item::TEXT;
        it.text = std::move(t);
        layout(it);
        add(std::move(it));
    }
    void startTableObject(const RVNGPropertyList &) override {}
    void openTableRow(const RVNGPropertyList &) override {}
    void closeTableRow() override {}
    void openTableCell(const RVNGPropertyList &) override {}
    void closeTableCell() override {}
    void insertCoveredTableCell(const RVNGPropertyList &) override {}
    void endTableObject() override {}
    void insertTab() override { insertText(RVNGString("    ")); }
    void insertSpace() override { insertText(RVNGString(" ")); }
    void insertText(const RVNGString &s) override {   // (libcdr's line breaks come as insertLineBreak)
        if (m_text && !s.empty()) appendText(s.cstr());
    }
    void insertLineBreak() override { newLine(); }
    void insertField(const RVNGPropertyList &) override {}
    void openOrderedListLevel(const RVNGPropertyList &) override {}
    void openUnorderedListLevel(const RVNGPropertyList &) override {}
    void closeOrderedListLevel() override {}
    void closeUnorderedListLevel() override {}
    void openListElement(const RVNGPropertyList &) override {}
    void closeListElement() override {}
    void defineParagraphStyle(const RVNGPropertyList &) override {}
    void openParagraph(const RVNGPropertyList &p) override {
        if (!m_text) return;
        m_align = 0;
        m_spacing = 1;
        m_lineHeight = 0;
        m_before = 1;
        m_after = 0;
        auto spacing = [](const RVNGProperty *v, float &out) {   // (libcdr: as the line height, a share of
            if (!v) return;                                       // the character height or points)
            double d = v->getUnit() == librevenge::RVNG_PERCENT ? v->getDouble() : -points(v);
            if (d > -10000 && d < 100) out = (float)d;
        };
        spacing(p["fo:margin-top"], m_before);
        spacing(p["fo:margin-bottom"], m_after);
        if (const RVNGProperty *lh = p["fo:line-height"]) {
            double sp = lh->getDouble();
            if (lh->getUnit() == librevenge::RVNG_PERCENT) {
                if (sp > 0.01 && sp < 100) m_spacing = (float)sp;
            } else {
                double pt = points(lh);
                if (pt > 0.01 && pt < 10000) m_lineHeight = (float)pt;
            }
        }
        if (is(p, "fo:text-align", "center")) m_align = 1;
        else if (is(p, "fo:text-align", "end") || is(p, "fo:text-align", "right")) m_align = 2;
        m_text->lines.emplace_back();
        m_text->lines.back().align = m_align;
        m_text->lines.back().spacing = m_spacing;
        m_text->lines.back().lineHeight = m_lineHeight;
        m_text->lines.back().paragraph = true;
        m_text->lines.back().before = m_before;
        m_text->lines.back().after = m_after;
        m_paragraphOpened = true;
    }
    void closeParagraph() override {}
    void defineCharacterStyle(const RVNGPropertyList &) override {}
    void openSpan(const RVNGPropertyList &p) override {
        m_span = Span();
        m_span.size = (float)points(p["fo:font-size"], 12.0);
        double sx = number(p["style:text-scale"], 1.0);
        if (sx > 0.01 && sx < 100) m_span.scaleX = (float)sx;
        m_span.shiftX = (float)number(p["libcdr:shift-x"]);
        m_span.shiftY = (float)number(p["libcdr:shift-y"]);
        m_span.rotate = (float)number(p["libcdr:rotate"]);
        m_span.letterSpacing = (float)number(p["libcdr:letter-spacing"]);
        m_span.wordSpacing = (float)number(p["libcdr:word-spacing"], 1.0);
        m_span.color = color(p["fo:color"]);
        m_span.fill = !is(p, "draw:fill", "none");
        if (p["draw:stroke"] && !is(p, "draw:stroke", "none")) {
            m_span.stroke = true;
            m_span.strokeColor = color(p["svg:stroke-color"]);
            m_span.strokeWidth = (float)points(p["svg:stroke-width"]);
        }
        std::string font = p["style:font-name"] ? p["style:font-name"]->getStr().cstr() : "Arial";
        m_span.font = font;
        m_span.bold = is(p, "fo:font-weight", "bold");
        m_span.italic = is(p, "fo:font-style", "italic");
        std::lock_guard<std::mutex> g(nf_lock());
        m_span.face = find_font(font, m_span.bold, m_span.italic);
        // a paragraph without text yet (an empty one has only its span): as high as this font
        if (m_text && m_paragraphOpened && !m_text->lines.empty() && m_text->lines.back().spans.empty()) {
            m_text->lines.back().emptyFace = m_span.face;
            m_text->lines.back().emptySize = m_span.size;
        }
    }
    void closeSpan() override {}
    void openLink(const RVNGPropertyList &) override {}
    void closeLink() override {}

private:
    std::shared_ptr<Style> m_style = std::make_shared<Style>();
    // decoded images by their data: libcdr passes the same buffer for each use of a bitmap (a
    // label sheet repeats one 2000 x 2000 logo 72 times: decoded once, not 72 x 16 MB); the
    // kept RVNGBinaryData keeps the buffer, so its address is not reused by another image
    struct CachedImage {
        librevenge::RVNGBinaryData data;
        std::shared_ptr<Image> img;
    };
    std::map<std::pair<const unsigned char *, unsigned long>, CachedImage> m_images;
    std::shared_ptr<Image> cachedImage(const RVNGProperty *p) {
        const librevenge::RVNGBinaryData *d = p ? p->getBinaryData() : nullptr;
        if (!d || !d->size()) return nullptr;
        auto key = std::make_pair(d->getDataBuffer(), d->size());
        auto f = m_images.find(key);
        if (f != m_images.end()) return f->second.img;
        std::shared_ptr<Image> img = image(p);
        m_images[key] = CachedImage{*d, img};
        return img;
    }
    std::unique_ptr<Text> m_text;
    Span m_span;
    int m_align = 0;
    float m_spacing = 1, m_lineHeight = 0, m_before = 1, m_after = 0;
    bool m_paragraphOpened = false;   // the last line is a paragraph of libcdr's (not a line break)
    bool m_inPage = false, m_inMaster = false;

    static float currentX(const plutovg_path_t *path) {
        plutovg_point_t pt = {0, 0};
        plutovg_path_get_current_point(path, &pt.x, &pt.y);
        return pt.x;
    }
    static float currentY(const plutovg_path_t *path) {
        plutovg_point_t pt = {0, 0};
        plutovg_path_get_current_point(path, &pt.x, &pt.y);
        return pt.y;
    }

    void addPath(plutovg_path_t *path) {
        if (!m_inPage || m_inMaster || (m_style->fill == FILL_NONE && !m_style->stroke)) {
            plutovg_path_destroy(path);
            return;
        }
        Item it;
        it.kind = Item::PATH;
        it.style = m_style;
        it.path.reset(path);
        plutovg_rect_t r;
        plutovg_path_extents(path, &r, false);
        float m = m_style->stroke ? std::max(m_style->strokeWidth, 0.5f) * 2 : 0;   // (miters)
        it.bx0 = r.x - m;
        it.by0 = r.y - m;
        it.bx1 = r.x + r.w + m;
        it.by1 = r.y + r.h + m;
        add(std::move(it));
    }

    void add(Item &&it) {
        if (pages.empty()) {   // drawing before any page: give it one
            pages.emplace_back();
            pages.back().w = 595;
            pages.back().h = 842;
        }
        pages.back().items.push_back(std::move(it));
    }

    static void rotatedBox(Item &it, float cx, float cy, float x0, float y0, float x1, float y1, float deg) {
        float c = cosf(deg * (float)M_PI / 180), s = sinf(deg * (float)M_PI / 180);
        float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
        it.bx0 = it.by0 = 1e30f;
        it.bx1 = it.by1 = -1e30f;
        for (int k = 0; k < 4; k++) {
            float dx = xs[k] - cx, dy = ys[k] - cy;
            float x = cx + dx * c - dy * s, y = cy + dx * s + dy * c;
            it.bx0 = std::min(it.bx0, x);
            it.by0 = std::min(it.by0, y);
            it.bx1 = std::max(it.bx1, x);
            it.by1 = std::max(it.by1, y);
        }
    }

    void newLine() {
        if (!m_text) return;
        m_text->lines.emplace_back();
        m_text->lines.back().align = m_align;
        m_text->lines.back().spacing = m_spacing;
        m_text->lines.back().lineHeight = m_lineHeight;
        m_text->lines.back().paragraph = true;   // (libcdr: a paragraph's end, CorelDRAW's CR)
        m_text->lines.back().before = m_before;
        m_text->lines.back().after = m_after;
        m_text->lines.back().emptyFace = m_span.face;
        m_text->lines.back().emptySize = m_span.size;
        m_paragraphOpened = false;
    }
    // the text in spans of one face each: characters the span's font lacks come from a fallback font
    void appendText(const std::string &s) {
        if (m_text->lines.empty()) newLine();
        std::lock_guard<std::mutex> g(nf_lock());
        size_t start = 0;
        plutovg_font_face_t *runFace = m_span.face;
        for (size_t i = 0; i < s.size();) {
            size_t at = i;
            plutovg_codepoint_t cp = nf_utf8_next(s, i);
            plutovg_font_face_t *f = m_span.face;
            if (f && cp > 0x20 && !nf_has_glyph(f, cp)) {
                plutovg_font_face_t *fb = fallback_font(m_span.font, m_span.bold, m_span.italic, cp);
                if (fb) f = fb;
            }
            if (f != runFace) {
                if (at > start) pushSpan(s.substr(start, at - start), runFace);
                start = at;
                runFace = f;
            }
        }
        if (s.size() > start) pushSpan(s.substr(start), runFace);
    }
    void pushSpan(const std::string &text, plutovg_font_face_t *face) {
        Span sp = m_span;
        sp.text = text;
        sp.face = face;
        m_text->lines.back().spans.push_back(std::move(sp));
    }

    // Line widths, positions and the bounding box. Artistic text (no frame): the first
    // baseline at (x, y), each line aligned on x. Frame text: lines from the frame's top,
    // aligned in its width. Both turn by 'rotate' about (x, y) / the frame's centre.
    // a line's width, size, ascent and descent from its spans (locked by the caller)
    static void measure(Line &l) {
        l.width = 0;
        l.size = 0;
        l.ascent = 0;
        l.descent = 0;
        float pen = 0;
        for (Span &s : l.spans) {
            s.width = textWidth(s, s.text.data(), s.text.size()) * s.scaleX;
            l.width = std::max(l.width, pen + s.shiftX * s.size * s.scaleX + s.width);   // (shifted words reach further)
            pen += s.width;
            l.size = std::max(l.size, s.size);
            if (s.face) {
                float asc = 0, desc = 0, gap = 0;
                plutovg_font_face_get_metrics(s.face, s.size, &asc, &desc, &gap, nullptr);
                l.ascent = std::max(l.ascent, asc);
                l.descent = std::max(l.descent, -desc);
            }
        }
        if (l.spans.empty() && l.emptySize > 0) {   // an empty line: as high as the line break's font
            l.size = l.emptySize;
            if (l.emptyFace) {
                float asc = 0, desc = 0, gap = 0;
                plutovg_font_face_get_metrics(l.emptyFace, l.size, &asc, &desc, &gap, nullptr);
                l.ascent = asc;
                l.descent = -desc;
            }
        }
        if (l.size == 0) l.size = 12;
        if (l.ascent == 0) l.ascent = l.size * 0.8f;
        if (l.descent == 0) l.descent = l.size * 0.35f;
    }

    // A paragraph wider than its text frame, broken into lines at the spaces, as CorelDRAW does
    // (libcdr passes a paragraph text as written: one line a paragraph). A word longer than the
    // frame stays on a line of its own. Lines whose words are placed by shifts (imported PDF
    // text) are left as they are. (locked by the caller)
    static void wrap(const Line &l, float width, std::vector<Line> &out) {
        for (const Span &s : l.spans)
            if (s.shiftX != 0 || s.shiftY != 0) {
                out.push_back(l);
                return;
            }
        Line cur;
        cur.align = l.align;
        cur.spacing = l.spacing;
        cur.lineHeight = l.lineHeight;
        cur.paragraph = l.paragraph;
        cur.before = l.before;
        cur.after = l.after;
        float x = 0;
        auto flush = [&]() {
            while (!cur.spans.empty()) {   // (no trailing spaces: they would shift a centred line)
                std::string &t = cur.spans.back().text;
                while (!t.empty() && t.back() == ' ') t.pop_back();
                if (!t.empty()) break;
                cur.spans.pop_back();
            }
            measure(cur);
            out.push_back(std::move(cur));
            cur = Line();
            cur.align = l.align;
            cur.spacing = l.spacing;
            cur.lineHeight = l.lineHeight;
            cur.before = l.before;
            cur.after = l.after;
            x = 0;
        };
        for (const Span &s : l.spans) {
            for (size_t i = 0; i < s.text.size();) {   // words with their trailing spaces
                size_t e = s.text.find(' ', i);
                e = e == std::string::npos ? s.text.size() : s.text.find_first_not_of(' ', e);
                if (e == std::string::npos) e = s.text.size();
                std::string word = s.text.substr(i, e - i);
                i = e;
                size_t n = word.find_last_not_of(' ');   // (its width without the spaces)
                float w = textWidth(s, word.data(), n == std::string::npos ? 0 : n + 1) * s.scaleX;
                float full = textWidth(s, word.data(), word.size()) * s.scaleX;
                if (x > 0 && x + w > width) flush();
                if (cur.spans.empty() || cur.spans.back().face != s.face || cur.spans.back().size != s.size ||
                    memcmp(&cur.spans.back().color, &s.color, sizeof s.color) || cur.spans.back().font != s.font) {
                    Span part = s;
                    part.text.clear();
                    cur.spans.push_back(part);
                }
                cur.spans.back().text += word;
                x += full;
            }
        }
        if (!cur.spans.empty() || out.empty()) flush();
    }

    void layout(Item &it) {
        Text &t = *it.text;
        std::lock_guard<std::mutex> g(nf_lock());
        float maxw = 0, total = 0;
        std::vector<Line> lines;
        for (Line &l : t.lines) {
            measure(l);
            if (t.w > 0 && l.width > t.w * 1.01f && l.spans.size()) wrap(l, t.w, lines);
            else lines.push_back(std::move(l));
        }
        t.lines = std::move(lines);
        // CorelDRAW's line spacing: from the previous line's descent to this one's ascent (100%:
        // as the fonts give them; as measured on its PDF exports). In a frame a paragraph's first
        // line has the paragraph spacing instead (before it, and the one after the previous
        // paragraph); artistic text has only the line spacing. An empty line is as high as the line
        // before it, but not higher than the text's own style (its line break's size, see libcdr):
        // so CorelDRAW's previews and PDF exports (after a large title, an empty line of the text's
        // size; between its smaller characters, as they are).
        bool artistic = t.w == 0 && t.h == 0;
        for (size_t i = 0; i < t.lines.size(); i++) {
            Line &l = t.lines[i];
            if (i && l.spans.empty() && (l.emptySize <= 0 || t.lines[i - 1].size <= l.emptySize)) {
                l.size = t.lines[i - 1].size;
                l.ascent = t.lines[i - 1].ascent;
                l.descent = t.lines[i - 1].descent;
            }
            const Line &p = i ? t.lines[i - 1] : l;
            auto space = [&](float share, const Line &of) {   // (a share of a line's height, or < 0: points)
                return share < 0 ? -share : share * (of.ascent + of.descent);
            };
            if (i && l.paragraph && !artistic)
                l.height = (l.before < 0 ? -l.before : l.before * (p.descent + l.ascent)) + space(p.after, p);
            else
                l.height = l.lineHeight > 0 ? l.lineHeight : (p.descent + l.ascent) * l.spacing;
            maxw = std::max(maxw, l.width);
            if (i) total += l.height;
        }
        const Line &first = t.lines[0];
        float top = artistic ? t.y - first.ascent : t.y;
        float left = artistic ? t.x - maxw : t.x, right = artistic ? t.x + maxw : t.x + std::max(t.w, maxw);
        float cx = artistic ? t.x : t.x + t.w / 2, cy = artistic ? t.y : t.y + t.h / 2;
        rotatedBox(it, cx, cy, left, top, right, top + (first.ascent + first.descent) * first.spacing + total + first.size, t.rotate);
    }

    std::shared_ptr<Style> parseStyle(const RVNGPropertyList &p) {
        auto st = std::make_shared<Style>();
        st->opacity = (float)number(p["draw:opacity"], 1.0);
        st->evenodd = is(p, "svg:fill-rule", "evenodd");
        if (is(p, "draw:fill", "solid")) {
            st->fill = FILL_SOLID;
            st->fillColor = color(p["draw:fill-color"]);
        } else if (is(p, "draw:fill", "gradient") && p.child("libcdr:gradient-stops") &&
                   p.child("libcdr:gradient-stops")->count() > 1) {
            st->fill = is(p, "libcdr:gradient-type", "radial")    ? FILL_RADIAL
                       : is(p, "libcdr:gradient-type", "conical") ? FILL_CONICAL
                       : is(p, "libcdr:gradient-type", "square")  ? FILL_SQUARE
                                                                  : FILL_LINEAR;
            st->corel = true;
            st->angle = (float)number(p["libcdr:gradient-angle"]);
            st->centerX = (float)number(p["libcdr:gradient-center-x"]);
            st->centerY = (float)number(p["libcdr:gradient-center-y"]);
            st->edge = std::min(std::max((float)number(p["libcdr:gradient-edge"]), 0.f), .99f);
            const RVNGPropertyListVector &v = *p.child("libcdr:gradient-stops");
            for (unsigned long i = 0; i < v.count(); i++) {
                Color k = color(v[i]["svg:stop-color"]);
                plutovg_gradient_stop_t gs;
                gs.offset = (float)number(v[i]["svg:offset"]);
                gs.color.r = k.r, gs.color.g = k.g, gs.color.b = k.b, gs.color.a = 1;
                st->stops.push_back(gs);
            }
            std::stable_sort(st->stops.begin(), st->stops.end(),
                             [](const plutovg_gradient_stop_t &a, const plutovg_gradient_stop_t &b) { return a.offset < b.offset; });
        } else if (is(p, "draw:fill", "gradient")) {
            bool radial = is(p, "draw:style", "radial") || is(p, "draw:style", "square") || is(p, "draw:style", "rectangular") ||
                          is(p, "draw:style", "ellipsoid");
            bool axial = is(p, "draw:style", "axial");
            st->fill = radial ? FILL_RADIAL : FILL_LINEAR;
            st->angle = (float)number(p["draw:angle"]);
            Color c0 = color(p["draw:start-color"]), c1 = color(p["draw:end-color"], Color{1, 1, 1, 1});
            const RVNGPropertyListVector *v = p.child("svg:linearGradient");
            if (!v) v = p.child("svg:radialGradient");
            auto stop = [](float off, Color c, float a) {
                plutovg_gradient_stop_t s;
                s.offset = off;
                s.color.r = c.r;
                s.color.g = c.g;
                s.color.b = c.b;
                s.color.a = a;
                return s;
            };
            if (v && v->count()) {
                for (unsigned long i = 0; i < v->count(); i++)
                    st->stops.push_back(stop((float)number((*v)[i]["svg:offset"]), color((*v)[i]["svg:stop-color"]),
                                             (float)number((*v)[i]["svg:stop-opacity"], 1.0)));
            } else if (radial) {   // (as librevenge's SVG generator: the end colour in the centre)
                st->stops = {stop(0, c1, 1), stop(1, c0, 1)};
            } else if (axial) {
                st->stops = {stop(0, c1, 1), stop(.5f, c0, 1), stop(1, c1, 1)};
            } else {
                st->stops = {stop(0, c0, 1), stop(1, c1, 1)};
            }
            if (radial) {
                st->cx = (float)number(p["svg:cx"], .5);
                st->cy = (float)number(p["svg:cy"], .5);
                st->r = p["draw:border"] ? (float)(1 - number(p["draw:border"])) : 1.f;
                if (st->r <= 0) st->r = .01f;
            }
        } else if (is(p, "draw:fill", "bitmap")) {
            st->tile = cachedImage(p["draw:fill-image"]);
            if (st->tile) {
                st->fill = FILL_BITMAP;
                st->tileRel = p["svg:width"] && p["svg:width"]->getUnit() == librevenge::RVNG_PERCENT;
                st->tileW = st->tileRel ? (float)number(p["svg:width"]) : (float)points(p["svg:width"]);
                st->tileH = st->tileRel ? (float)number(p["svg:height"]) : (float)points(p["svg:height"]);
                st->refX = (float)number(p["draw:fill-image-ref-point-x"]);
                st->refY = (float)number(p["draw:fill-image-ref-point-y"]);
            }
        }
        if (p["draw:stroke"] && !is(p, "draw:stroke", "none")) {
            st->stroke = true;
            st->strokeColor = color(p["svg:stroke-color"]);
            st->strokeColor.a = (float)number(p["svg:stroke-opacity"], 1.0);
            st->strokeWidth = (float)points(p["svg:stroke-width"]);
            if (is(p, "svg:stroke-linecap", "round")) st->cap = PLUTOVG_LINE_CAP_ROUND;
            else if (is(p, "svg:stroke-linecap", "square")) st->cap = PLUTOVG_LINE_CAP_SQUARE;
            if (p["libcdr:marker-unit"]) {
                st->markerUnit = (float)points(p["libcdr:marker-unit"]);
                if (p["libcdr:marker-start"]) st->markerStart = p["libcdr:marker-start"]->getStr().cstr();
                if (p["libcdr:marker-end"]) st->markerEnd = p["libcdr:marker-end"]->getStr().cstr();
            }
            if (is(p, "svg:stroke-linejoin", "round")) st->join = PLUTOVG_LINE_JOIN_ROUND;
            else if (is(p, "svg:stroke-linejoin", "bevel")) st->join = PLUTOVG_LINE_JOIN_BEVEL;
            if (is(p, "draw:stroke", "dash")) {   // ODF: dots1 x dots1-length, dots2 x dots2-length, all + distance
                int n1 = p["draw:dots1"] ? p["draw:dots1"]->getInt() : 0, n2 = p["draw:dots2"] ? p["draw:dots2"]->getInt() : 0;
                float l1 = (float)points(p["draw:dots1-length"]), l2 = (float)points(p["draw:dots2-length"]);
                float gap = (float)points(p["draw:distance"]);
                for (int k = 0; k < n1 && k < 64; k++) st->dashes.insert(st->dashes.end(), {l1, gap});
                for (int k = 0; k < n2 && k < 64; k++) st->dashes.insert(st->dashes.end(), {l2, gap});
                bool visible = false;
                for (float d : st->dashes) visible |= d > 0;
                if (!visible) st->dashes.clear();
            }
        }
        return st;
    }
};

// --- drawing -----------------------------------------------------------------

void setColor(plutovg_canvas_t *c, Color col, float alpha) { plutovg_canvas_set_rgba(c, col.r, col.g, col.b, col.a * alpha); }

// Sets the fill paint for an item with bounding box (x, y, w, h).
// The colour at t (0: the start colour, 1: the end colour) of a fountain fill's stops
static plutovg_color_t stopColor(const std::vector<plutovg_gradient_stop_t> &stops, float t) {
    if (t <= stops.front().offset) return stops.front().color;
    for (size_t i = 1; i < stops.size(); i++)
        if (t <= stops[i].offset) {
            const plutovg_gradient_stop_t &a = stops[i - 1], &b = stops[i];
            float k = b.offset > a.offset ? (t - a.offset) / (b.offset - a.offset) : 1;
            return {a.color.r + (b.color.r - a.color.r) * k, a.color.g + (b.color.g - a.color.g) * k,
                    a.color.b + (b.color.b - a.color.b) * k, 1};
        }
    return stops.back().color;
}

// A CorelDRAW fountain fill over the bounding box (as its PDF exports show): the centre is the
// box's, moved by the offset (shares of the box's width / height, up positive: 0.5 is its edge);
// the angle points from the start colour to the end colour. Linear: across the box's extent along
// the angle, the edge pad (a share of it) plain start / end colour at each end; radial: a circle,
// the end colour in the centre, the start colour at the farthest corner; square: a square turned
// by the angle, likewise; the edge pad of these two shrinks them by twice its share (25: to half
// the size); conical: the end colour towards the angle, the start colour the other way, both sides
// alike. Midpoints come as a third stop.
static bool setCorelFill(plutovg_canvas_t *c, const Style &st, float x, float y, float w, float h) {
    float cx = x + w * (.5f + st.centerX), cy = y + h * (.5f - st.centerY);
    float a = (float)(st.angle * M_PI / 180), dx = cosf(a), dy = -sinf(a);
    float corners[4][2] = {{x, y}, {x + w, y}, {x, y + h}, {x + w, y + h}};
    if (st.fill == FILL_LINEAR) {
        float mx = x + w / 2, my = y + h / 2, t0 = 0, t1 = 0;
        for (auto &k : corners) {
            float t = (k[0] - mx) * dx + (k[1] - my) * dy;
            t0 = std::min(t0, t), t1 = std::max(t1, t);
        }
        float pad = (t1 - t0) * std::min(st.edge, .499f);
        t0 += pad, t1 -= pad;
        plutovg_canvas_set_linear_gradient(c, mx + dx * t0, my + dy * t0, mx + dx * t1, my + dy * t1, PLUTOVG_SPREAD_METHOD_PAD,
                                           st.stops.data(), (int)st.stops.size(), nullptr);
    } else if (st.fill == FILL_RADIAL) {
        float r = 0;
        for (auto &k : corners) r = std::max(r, hypotf(k[0] - cx, k[1] - cy));
        r *= 1 - 2 * std::min(st.edge, .499f);
        if (r <= 0) return false;
        std::vector<plutovg_gradient_stop_t> stops(st.stops.rbegin(), st.stops.rend());   // (from the centre)
        for (auto &s : stops) s.offset = 1 - s.offset;
        plutovg_canvas_set_radial_gradient(c, cx, cy, r, cx, cy, 0, PLUTOVG_SPREAD_METHOD_PAD, stops.data(), (int)stops.size(), nullptr);
    } else {
        // conical / square: no such gradient in plutovg; drawn into an image over the box, once
        // (512 pixels along its longer side: the colours change slowly)
        int iw = w >= h ? 512 : std::max(8, (int)(512 * w / h)), ih = h >= w ? 512 : std::max(8, (int)(512 * h / w));
        if (!st.gradTile || plutovg_surface_get_width(st.gradTile->s) != iw || plutovg_surface_get_height(st.gradTile->s) != ih) {
            auto img = std::make_shared<Image>();
            img->s = plutovg_surface_create(iw, ih);
            if (!img->s) return false;
            float side = 0;   // square: the half side reaching the farthest corner
            for (auto &k : corners) {
                float u = (k[0] - cx) * dx + (k[1] - cy) * dy, v = (k[0] - cx) * -dy + (k[1] - cy) * dx;
                side = std::max(side, std::max(fabsf(u), fabsf(v)));
            }
            side *= 1 - 2 * std::min(st.edge, .499f);
            unsigned char *data = plutovg_surface_get_data(img->s);
            int stride = plutovg_surface_get_stride(img->s);
            for (int j = 0; j < ih; j++) {
                uint32_t *row = (uint32_t *)(data + (size_t)j * stride);
                float py = y + (j + .5f) * h / ih - cy;
                for (int i = 0; i < iw; i++) {
                    float px = x + (i + .5f) * w / iw - cx, u = px * dx + py * dy, v = px * -dy + py * dx, t;
                    if (st.fill == FILL_SQUARE) t = side > 0 ? 1 - std::max(fabsf(u), fabsf(v)) / side : 1;
                    else t = 1 - fabsf(atan2f(v, u)) / (float)M_PI;
                    plutovg_color_t k = stopColor(st.stops, std::min(std::max(t, 0.f), 1.f));
                    row[i] = 0xff000000u | (uint32_t)(k.r * 255 + .5f) << 16 | (uint32_t)(k.g * 255 + .5f) << 8 | (uint32_t)(k.b * 255 + .5f);
                }
            }
            st.gradTile = img;
        }
        plutovg_matrix_t m;
        plutovg_matrix_init_translate(&m, x, y);
        plutovg_matrix_scale(&m, w / iw, h / ih);
        plutovg_canvas_set_texture(c, st.gradTile->s, PLUTOVG_TEXTURE_TYPE_PLAIN, st.opacity, &m);
        return true;
    }
    plutovg_canvas_set_opacity(c, st.opacity);
    return true;
}

bool setFill(plutovg_canvas_t *c, const Style &st, float x, float y, float w, float h) {
    if (st.corel && (st.fill == FILL_LINEAR || st.fill == FILL_RADIAL || st.fill == FILL_CONICAL || st.fill == FILL_SQUARE))
        return !st.stops.empty() && w > 0 && h > 0 && setCorelFill(c, st, x, y, w, h);
    switch (st.fill) {
    case FILL_SOLID: setColor(c, st.fillColor, st.opacity); return true;
    case FILL_LINEAR:
    case FILL_RADIAL: {
        if (st.stops.empty() || w <= 0 || h <= 0) return false;
        plutovg_matrix_t m;   // bounding box units (librevenge's SVG generator: objectBoundingBox)
        plutovg_matrix_init_translate(&m, x, y);
        plutovg_matrix_scale(&m, w, h);
        if (st.fill == FILL_LINEAR) {   // top to bottom, turned by -angle about the centre
            plutovg_matrix_translate(&m, .5f, .5f);
            plutovg_matrix_rotate(&m, (float)(-st.angle * M_PI / 180));
            plutovg_matrix_translate(&m, -.5f, -.5f);
            plutovg_canvas_set_linear_gradient(c, 0, 0, 0, 1, PLUTOVG_SPREAD_METHOD_PAD, st.stops.data(), (int)st.stops.size(), &m);
        } else {
            plutovg_canvas_set_radial_gradient(c, st.cx, st.cy, st.r, st.cx, st.cy, 0, PLUTOVG_SPREAD_METHOD_PAD, st.stops.data(),
                                               (int)st.stops.size(), &m);
        }
        plutovg_canvas_set_opacity(c, st.opacity);
        return true;
    }
    case FILL_BITMAP: {
        int iw = plutovg_surface_get_width(st.tile->s), ih = plutovg_surface_get_height(st.tile->s);
        float tw = st.tileRel ? st.tileW * w : st.tileW, th = st.tileRel ? st.tileH * h : st.tileH;
        if (iw <= 0 || ih <= 0 || tw <= 0 || th <= 0) return false;
        plutovg_matrix_t m;   // tiles from the bottom left corner, shifted by the reference point
        plutovg_matrix_init_translate(&m, x + st.refX * tw, y + h - th + st.refY * th);
        plutovg_matrix_scale(&m, tw / iw, th / ih);
        plutovg_canvas_set_texture(c, st.tile->s, PLUTOVG_TEXTURE_TYPE_TILED, st.opacity, &m);
        return true;
    }
    default: return false;
    }
}

// A span's text at (x, y) (its font set), filled or outlined: with character or word spacing
// character by character
void drawText(plutovg_canvas_t *c, const Span &s, float x, float y, bool stroke) {
    auto draw = stroke ? plutovg_canvas_stroke_text : plutovg_canvas_fill_text;
    if (s.letterSpacing == 0 && s.wordSpacing == 1) {
        draw(c, s.text.data(), (int)s.text.size(), PLUTOVG_TEXT_ENCODING_UTF8, x, y);
        return;
    }
    for (size_t i = 0; i < s.text.size();) {
        size_t e = i + 1;
        while (e < s.text.size() && (s.text[e] & 0xc0) == 0x80) e++;
        draw(c, s.text.data() + i, (int)(e - i), PLUTOVG_TEXT_ENCODING_UTF8, x, y);
        x += textWidth(s, s.text.data() + i, e - i);
        i = e;
    }
}

// A line's arrowheads (in its outline colour): at its first and last points, the tip
// there, pointing away from the line (along its first / last segment, or curve's tangent)
void drawMarkers(plutovg_canvas_t *c, const Style &st, const plutovg_path_t *path) {
    plutovg_point_t first = {0, 0}, second = {0, 0}, last = {0, 0}, before = {0, 0};
    bool haveFirst = false, haveSecond = false, haveLast = false;
    plutovg_path_iterator_t iter;
    plutovg_path_iterator_init(&iter, path);
    plutovg_point_t pts[3];
    auto differs = [](plutovg_point_t a, plutovg_point_t b) { return fabsf(a.x - b.x) > 1e-4f || fabsf(a.y - b.y) > 1e-4f; };
    while (plutovg_path_iterator_has_next(&iter)) {
        plutovg_path_command_t cmd = plutovg_path_iterator_next(&iter, pts);
        int n = cmd == PLUTOVG_PATH_COMMAND_CUBIC_TO ? 3 : 1;
        if (cmd == PLUTOVG_PATH_COMMAND_CLOSE) return;   // (closed: no ends)
        for (int k = 0; k < n; k++) {
            if (!haveFirst) { first = pts[k]; haveFirst = true; }
            else if (!haveSecond && differs(pts[k], first)) { second = pts[k]; haveSecond = true; }
            if (haveLast && differs(pts[k], last)) before = last;
            last = pts[k];
            haveLast = true;
        }
    }
    if (!haveSecond) return;
    auto draw = [&](const std::string &data, plutovg_point_t at, plutovg_point_t from) {
        float dx = at.x - from.x, dy = at.y - from.y, len = sqrtf(dx * dx + dy * dy);
        if (data.empty() || len <= 0) return;
        dx /= len, dy /= len;
        plutovg_path_t *m = plutovg_path_create();
        if (plutovg_path_parse(m, data.data(), (int)data.size())) {
            float k = st.markerUnit;
            plutovg_matrix_t t = {-dy * k, dx * k, dx * k, dy * k, at.x, at.y};   // (x across, y along)
            plutovg_path_transform(m, &t);
            // its closed parts are filled, its open ones (a bar across the tip, an open "<") outlined
            // with the line's own width (CorelDRAW 9's PDF export closes them, CorelDRAW does not)
            plutovg_path_t *closed = plutovg_path_create(), *open = plutovg_path_create(), *part = plutovg_path_create();
            bool inPart = false;
            plutovg_path_iterator_t mi;
            plutovg_path_iterator_init(&mi, m);
            while (plutovg_path_iterator_has_next(&mi)) {
                plutovg_path_command_t cmd = plutovg_path_iterator_next(&mi, pts);
                if (cmd == PLUTOVG_PATH_COMMAND_MOVE_TO) {
                    if (inPart) plutovg_path_add_path(open, part, nullptr);
                    plutovg_path_reset(part);
                    plutovg_path_move_to(part, pts[0].x, pts[0].y);
                    inPart = true;
                } else if (cmd == PLUTOVG_PATH_COMMAND_LINE_TO) plutovg_path_line_to(part, pts[0].x, pts[0].y);
                else if (cmd == PLUTOVG_PATH_COMMAND_CUBIC_TO) plutovg_path_cubic_to(part, pts[0].x, pts[0].y, pts[1].x, pts[1].y, pts[2].x, pts[2].y);
                else {
                    plutovg_path_close(part);
                    plutovg_path_add_path(closed, part, nullptr);
                    plutovg_path_reset(part);
                    inPart = false;
                }
            }
            if (inPart) plutovg_path_add_path(open, part, nullptr);
            plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
            plutovg_canvas_fill_path(c, closed);
            plutovg_canvas_stroke_path(c, open);
            plutovg_path_destroy(closed);
            plutovg_path_destroy(open);
            plutovg_path_destroy(part);
        }
        plutovg_path_destroy(m);
    };
    draw(st.markerStart, first, second);
    draw(st.markerEnd, last, before);
}

void drawItem(plutovg_canvas_t *c, const Item &it, float scale) {
    plutovg_canvas_save(c);
    if (it.kind == Item::PATH) {
        const Style &st = *it.style;
        plutovg_rect_t r;
        plutovg_path_extents(it.path.get(), &r, false);
        if (st.fill != FILL_NONE && setFill(c, st, r.x, r.y, r.w, r.h)) {
            plutovg_canvas_set_fill_rule(c, st.evenodd ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
            plutovg_canvas_fill_path(c, it.path.get());
        }
        if (st.stroke) {
            plutovg_canvas_set_opacity(c, 1);
            setColor(c, st.strokeColor, 1);
            // never thinner than one device pixel (as CorelDRAW and PDF viewers draw them):
            // hairlines (width 0) and the many 0.1 pt lines of architectural plans would
            // otherwise fade to a faint grey when zoomed out
            plutovg_canvas_set_line_width(c, std::max(st.strokeWidth, 1 / scale));
            plutovg_canvas_set_line_cap(c, st.cap);
            plutovg_canvas_set_line_join(c, st.join);
            if (!st.dashes.empty()) plutovg_canvas_set_dash(c, 0, st.dashes.data(), (int)st.dashes.size());
            plutovg_canvas_stroke_path(c, it.path.get());
            if (st.markerUnit > 0) {
                plutovg_canvas_set_dash(c, 0, nullptr, 0);
                drawMarkers(c, st, it.path.get());
            }
        }
    } else if (it.kind == Item::IMAGE) {
        if (it.path) plutovg_canvas_clip_path(c, it.path.get());   // (before the image's own transform)
        int iw = plutovg_surface_get_width(it.img->s), ih = plutovg_surface_get_height(it.img->s);
        float cx = it.x + it.w / 2, cy = it.y + it.h / 2;
        plutovg_canvas_translate(c, cx, cy);
        plutovg_canvas_scale(c, it.mirrorX ? -1 : 1, it.mirrorY ? -1 : 1);
        plutovg_canvas_rotate(c, (float)(it.rotate * M_PI / 180));
        plutovg_canvas_translate(c, -cx, -cy);
        plutovg_matrix_t m;
        plutovg_matrix_init_translate(&m, it.x, it.y);
        plutovg_matrix_scale(&m, it.w / iw, it.h / ih);
        plutovg_canvas_set_texture(c, it.img->s, PLUTOVG_TEXTURE_TYPE_PLAIN, 1, &m);
        plutovg_canvas_fill_rect(c, it.x, it.y, it.w, it.h);
    } else {
        const Text &t = *it.text;
        bool artistic = t.w == 0 && t.h == 0;
        float cx = artistic ? t.x : t.x + t.w / 2, cy = artistic ? t.y : t.y + t.h / 2;
        if (t.rotate != 0) {
            plutovg_canvas_translate(c, cx, cy);
            plutovg_canvas_rotate(c, (float)(t.rotate * M_PI / 180));
            plutovg_canvas_translate(c, -cx, -cy);
        }
        float base = artistic ? t.y : t.y + t.lines[0].ascent;
        for (size_t i = 0; i < t.lines.size(); i++) {
            const Line &l = t.lines[i];
            if (i) base += l.height;
            float x;
            if (artistic) x = l.align == 1 ? t.x - l.width / 2 : l.align == 2 ? t.x - l.width : t.x;
            else x = l.align == 1 ? t.x + (t.w - l.width) / 2 : l.align == 2 ? t.x + t.w - l.width : t.x;
            for (const Span &s : l.spans) {
                if (s.face) {
                    plutovg_canvas_set_font(c, s.face, s.size);
                    setColor(c, s.color, 1);
                    // (the shift in the text's stretched em; the next span starts unshifted)
                    float sx = x + s.shiftX * s.size * s.scaleX, sy = base - s.shiftY * s.size;
                    bool turned = s.scaleX != 1 || s.rotate != 0;
                    if (turned) {
                        plutovg_canvas_save(c);
                        plutovg_canvas_translate(c, sx, sy);
                        if (s.rotate != 0) plutovg_canvas_rotate(c, (float)(-s.rotate * M_PI / 180));
                        plutovg_canvas_scale(c, s.scaleX, 1);
                    }
                    float tx = turned ? 0 : sx, ty = turned ? 0 : sy;
                    if (s.fill) drawText(c, s, tx, ty, false);
                    // over the fill, a device pixel wide at least (as a shape's: CorelDRAW's hairlines
                    // show), but that only up to a twentieth of the size: small white letters came out black
                    if (s.stroke) {
                        setColor(c, s.strokeColor, 1);
                        plutovg_canvas_set_line_width(c, std::max(s.strokeWidth, std::min(1 / scale, s.size / 20)));
                        plutovg_canvas_set_line_join(c, PLUTOVG_LINE_JOIN_MITER);
                        drawText(c, s, tx, ty, true);
                    }
                    if (turned) plutovg_canvas_restore(c);
                }
                x += s.width;
            }
        }
    }
    plutovg_canvas_restore(c);
}

} // namespace

struct nitrocdr {
    std::vector<Page> pages;
    int background = 0;   // (nitrocdr_set_background)
};

nitrocdr *nitrocdr_load(const void *data, size_t len) {
    librevenge::RVNGStringStream in(static_cast<const unsigned char *>(data), (unsigned)len);
    Painter painter;
    bool ok = false;
    if (libcdr::CDRDocument::isSupported(&in)) {
        in.seek(0, librevenge::RVNG_SEEK_SET);
        ok = libcdr::CDRDocument::parse(&in, &painter);
    } else {
        in.seek(0, librevenge::RVNG_SEEK_SET);
        if (libcdr::CMXDocument::isSupported(&in)) {
            in.seek(0, librevenge::RVNG_SEEK_SET);
            ok = libcdr::CMXDocument::parse(&in, &painter);
        }
    }
    if (!ok || painter.pages.empty()) return nullptr;
    // the shown area is the drawing's bounding box (+ a small margin), not the page set in the
    // file: a small logo on an A4 page fills the view, a scan much larger than its page shows
    // in full (an empty page keeps its size). A drawing of several pages gets one area, the
    // bounding box of all its pages: what lies above each other on them (the floors of a
    // building) stays in place when paging.
    double ux0 = 1e30, uy0 = 1e30, ux1 = -1e30, uy1 = -1e30;
    std::vector<std::array<double, 4>> boxes;
    for (const Page &pg : painter.pages) {
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (const Item &it : pg.items) {
            x0 = std::min(x0, (double)it.bx0);
            y0 = std::min(y0, (double)it.by0);
            x1 = std::max(x1, (double)it.bx1);
            y1 = std::max(y1, (double)it.by1);
        }
        boxes.push_back({x0, y0, x1, y1});
        if (x1 <= x0 || y1 <= y0) continue;
        ux0 = std::min(ux0, x0);
        uy0 = std::min(uy0, y0);
        ux1 = std::max(ux1, x1);
        uy1 = std::max(uy1, y1);
    }
    for (size_t i = 0; i < painter.pages.size(); i++) {
        Page &pg = painter.pages[i];
        double x0 = boxes[i][0], y0 = boxes[i][1], x1 = boxes[i][2], y1 = boxes[i][3];
        if (painter.pages.size() > 1 && ux1 > ux0 && uy1 > uy0) {
            x0 = ux0;
            y0 = uy0;
            x1 = ux1;
            y1 = uy1;
        }
        if (x1 <= x0 || y1 <= y0) continue;
        double m = std::max(2.0, 0.015 * std::max(x1 - x0, y1 - y0));
        pg.x = x0 - m;
        pg.y = y0 - m;
        pg.w = x1 - x0 + 2 * m;
        pg.h = y1 - y0 + 2 * m;
    }
    return new nitrocdr{std::move(painter.pages), 0};
}

void nitrocdr_free(nitrocdr *cdr) {
    std::lock_guard<std::mutex> g(nf_lock());   // (surfaces / paths only; the faces are nitro_font's)
    delete cdr;
}

int nitrocdr_pages(const nitrocdr *cdr) { return (int)cdr->pages.size(); }

void nitrocdr_set_background(nitrocdr *cdr, int background) {
    if (cdr && background >= 0 && background <= 2) cdr->background = background;
}

void nitrocdr_size(const nitrocdr *cdr, int page, double *w, double *h) {
    *w = cdr->pages[page].w;
    *h = cdr->pages[page].h;
}

int nitrocdr_render(nitrocdr *cdr, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0) {
    if (page < 0 || page >= (int)cdr->pages.size() || w <= 0 || h <= 0 || pitch < (size_t)w * 4 || !(scale > 0)) return -1;
    static const uint32_t backgrounds[3] = {0xffffffff, 0xff212830, 0xff000000};   // (opaque)
    uint32_t bg = backgrounds[cdr->background];
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(bgra + (size_t)y * pitch);
        for (int x = 0; x < w; x++) row[x] = bg;
    }
    std::lock_guard<std::mutex> g(nf_lock());
    plutovg_surface_t *surface = plutovg_surface_create_for_data(bgra, w, h, (int)pitch);
    plutovg_canvas_t *c = plutovg_canvas_create(surface);
    plutovg_canvas_scale(c, (float)scale, (float)scale);
    x0 += cdr->pages[page].x;   // (the shown area's origin)
    y0 += cdr->pages[page].y;
    plutovg_canvas_translate(c, (float)-x0, (float)-y0);
    float vx0 = (float)x0, vy0 = (float)y0, vx1 = (float)(x0 + w / scale), vy1 = (float)(y0 + h / scale);
    for (const Item &it : cdr->pages[page].items)
        if (it.bx1 >= vx0 && it.bx0 <= vx1 && it.by1 >= vy0 && it.by0 <= vy1) drawItem(c, it, (float)scale);
    plutovg_canvas_destroy(c);
    plutovg_surface_destroy(surface);
    return 0;
}
