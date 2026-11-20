// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrosvg: C interface to lunasvg. See nitrosvg.h.
#include "nitrosvg.h"
#include "nitro_draw.h"
#include "nitro_font.h"
#include "nitro_xmltag.h"

#include <lunasvg.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <mutex>
#include <vector>

struct nitrosvg {
    std::unique_ptr<lunasvg::Document> doc;
    double w, h;     // intrinsic size
    double ox, oy;   // its top-left corner in document units (bounding box case)
    int background = 0;   // (nitrosvg_set_background)
};

// LunaSVG's font and glyph resolvers use the shared index under nf_lock().
static plutovg_font_face_t *resolve_font(void *, const char *family, int weight, bool italic) { return nf_css_face_weight(family, weight, italic); }
static plutovg_font_face_t *resolve_glyph(void *, const char *families, int weight, bool italic, uint32_t cp) {
    return nf_css_face_list(families, weight, italic, cp);
}
static void set_resolver() {   // (locked by the caller)
    static bool done;
    if (!done) {
        lunasvg_set_weighted_font_face_resolver(resolve_font, nullptr);
        lunasvg_set_glyph_face_resolver(resolve_glyph, nullptr);
        done = true;
    }
}

// Fixes applied to the text before lunasvg parses it (only when the file needs them):
//
// "<svg:svg xmlns:svg=...>" (LibreOffice): valid XML with a namespace prefix, but lunasvg only
// knows plain element names: the "svg:" prefix of the elements is dropped.
static std::string unprefix(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        out += s[i];
        if (s[i] == '<') {
            if (i + 1 < s.size() && s[i + 1] == '/') out += s[++i];
            if (!s.compare(i + 1, 4, "svg:")) i += 4;
        }
    }
    return out;
}

// CSS custom properties, "var(--vscode-editor-background, #1E1E1E)" (VS Code's images): nothing
// defines the property in a standalone file, so a browser uses the fallback, or without one the
// inherited value (these files have fill="none" on the root: invisible); lunasvg doesn't know
// var() at all (black squares). Replaced by the fallback, or by "inherit".
// CSS light-dark(a, b) (draw.io's exports colour every shape with one): its light colour a, as the pages are
// shown in their light scheme (lunasvg knows no light-dark() and drew such a shape black)
static std::string light_colours(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    for (size_t v; (v = s.find("light-dark(", i)) != std::string::npos;) {
        out.append(s, i, v - i);
        size_t j = v + 11, comma = std::string::npos;
        int depth = 1;
        for (; j < s.size() && depth; j++) {
            if (s[j] == '(') depth++;
            else if (s[j] == ')') depth--;
            else if (s[j] == ',' && depth == 1 && comma == std::string::npos) comma = j;
            else if (s[j] == '"' || s[j] == '<' || s[j] == ';') break;   // not a well-formed light-dark()
        }
        if (depth || comma == std::string::npos) {   // damaged: leave it
            out.append(s, v, 11);
            i = v + 11;
            continue;
        }
        size_t a = v + 11, b = comma;
        while (a < b && isspace((unsigned char)s[a])) a++;
        while (b > a && isspace((unsigned char)s[b - 1])) b--;
        out += light_colours(s.substr(a, b - a));
        i = j;
    }
    out.append(s, i, std::string::npos);
    return out;
}

static std::string var_fallbacks(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    for (size_t v; (v = s.find("var(--", i)) != std::string::npos;) {
        out.append(s, i, v - i);
        size_t j = v + 4, comma = std::string::npos;
        int depth = 1;
        for (; j < s.size() && depth; j++) {
            if (s[j] == '(') depth++;
            else if (s[j] == ')') depth--;
            else if (s[j] == ',' && depth == 1 && comma == std::string::npos) comma = j;
            else if (s[j] == '"' || s[j] == '<' || s[j] == ';') break;   // not a well-formed var()
        }
        if (depth) {   // damaged: leave it
            out.append(s, v, 4);
            i = v + 4;
            continue;
        }
        if (comma == std::string::npos) {   // no fallback: the inherited value
            out += "inherit";
            i = j;
            continue;
        }
        size_t a = comma + 1, b = j - 1;   // fallback: (comma, closing parenthesis)
        while (a < b && isspace((unsigned char)s[a])) a++;
        while (b > a && isspace((unsigned char)s[b - 1])) b--;
        out += var_fallbacks(s.substr(a, b - a));   // the fallback may be a var() itself
        i = j;
    }
    out.append(s, i, std::string::npos);
    return out;
}

// Line breaks in text: SVG 1.1 (xml:space="default") removes them, newer renderers (lunasvg,
// resvg, CSS white-space) turn them into spaces. Old exporters (LibreOffice) split words into
// tspans on separate lines, "N\n...\népszínház" -> "N épszínház": removed, as SVG 1.1 says.
// Not when the file uses xml:space="preserve" (or 'preserve') anywhere (there they do become spaces).
static std::string text_newlines(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    int depth = 0;   // inside <text> elements
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '<') {
            size_t e = s.find('>', i);
            if (e == std::string::npos) e = s.size() - 1;
            bool close = i + 1 < s.size() && s[i + 1] == '/';
            size_t n = i + 1 + close;
            if (!s.compare(n, 4, "svg:")) n += 4;
            if (!s.compare(n, 4, "text") && n + 4 < s.size() &&
                (isspace((unsigned char)s[n + 4]) || s[n + 4] == '>' || s[n + 4] == '/')) {
                if (close) depth = depth > 0 ? depth - 1 : 0;
                else if (s[e - 1] != '/') depth++;
            }
            out.append(s, i, e - i + 1);
            i = e;
            continue;
        }
        if (depth > 0 && (c == '\n' || c == '\r')) continue;
        out += c;
    }
    return out;
}

// A sprite: an SVG of <symbol>s alone, which other documents take their icons from; by itself it
// draws nothing. Made into a sheet of its symbols: each in a cell (fitted, by its viewBox) over its
// id, 16 to a row, as many rows as it takes; the document's own content stays (the symbols, their
// gradients and styles), the root's size is the sheet's. false: not one (something of it is drawn,
// or it has no symbol with an id).
static bool sprite_sheet(const std::string &src, std::string &out) {
    static const char *const hidden[] = {"symbol", "defs", "metadata", "style", "title", "desc", "clipPath", "mask", "pattern",
                                         "linearGradient", "radialGradient", "filter", "marker", "script"};
    struct Symbol {
        std::string id;
        bool box;
    };
    std::vector<Symbol> symbols;
    nxt::Tag t, root;
    size_t at = 0, last_close = std::string::npos;
    int depth = 0;   // inside how many elements that are not drawn where they stand
    bool have_root = false;
    while (nxt::next(src.data(), src.size(), at, t)) {
        if (!have_root) {
            if (t.closing || t.empty || t.name != "svg") return false;
            root = t, have_root = true;
            continue;
        }
        bool container = false;
        for (const char *h : hidden) container |= t.name == h;
        if (t.closing) {
            if (container && depth > 0) depth--;
            if (t.name == "svg" && !depth) last_close = t.begin;
            continue;
        }
        if (!container && !depth) return false;   // (drawn: a picture, not a sprite)
        if (t.name == "symbol") {   // (wherever it stands: often inside <defs>)
            const std::string *id = t.attr("id");
            if (id && !id->empty() && symbols.size() < 20000) symbols.push_back({*id, t.attr("viewBox") != nullptr});
        }
        if (container && !t.empty) depth++;
    }
    if (symbols.empty() || last_close == std::string::npos) return false;
    const int COLS = 16, CELL_W = 96, CELL_H = 112, ICON = 64, MARGIN = 32;
    int cols = (int)std::min<size_t>(COLS, symbols.size()), rows = (int)((symbols.size() + COLS - 1) / COLS);
    int w = 2 * MARGIN + cols * CELL_W, h = 2 * MARGIN + rows * CELL_H;
    out = "<svg";
    bool ns = false, xlink = false;
    for (const auto &a : root.attrs) {
        static const char *const drop[] = {"width", "height", "viewBox", "preserveAspectRatio", "style", "class", "display", "visibility", "x", "y", "hidden", "aria-hidden"};
        bool skip = false;
        for (const char *d : drop) skip |= a.first == d;
        if (skip) continue;
        ns |= a.first == "xmlns", xlink |= a.first == "xmlns:xlink";
        out += " " + a.first + "=\"" + nxt::escape(a.second) + "\"";
    }
    if (!ns) out += " xmlns=\"http://www.w3.org/2000/svg\"";
    if (!xlink) out += " xmlns:xlink=\"http://www.w3.org/1999/xlink\"";
    char b[400];
    snprintf(b, sizeof b, " width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">", w, h, w, h);
    out += b;
    out.append(src, root.end, last_close - root.end);
    out += "<g fill=\"#222\" color=\"#222\" font-family=\"sans-serif\" font-size=\"9\" text-anchor=\"middle\">";
    for (size_t k = 0; k < symbols.size(); k++) {
        int x = MARGIN + (int)(k % COLS) * CELL_W, y = MARGIN + (int)(k / COLS) * CELL_H;
        std::string id = nxt::escape(symbols[k].id), label = symbols[k].id;
        if (label.size() > 18) {   // (cut at a character's start)
            size_t cut = 17;
            while (cut > 0 && ((unsigned char)label[cut] & 0xc0) == 0x80) cut--;
            label = label.substr(0, cut) + "\xe2\x80\xa6";
        }
        snprintf(b, sizeof b, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" fill=\"none\" stroke=\"#c8d4e4\" stroke-width=\"0.5\"/>", x, y, CELL_W, CELL_H);
        out += b;
        // (a symbol without a viewBox is drawn at its own size: kept within its cell)
        snprintf(b, sizeof b, "<svg x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\">", x + (CELL_W - ICON) / 2, y + 14, ICON, ICON);
        out += b;
        out += "<use xlink:href=\"#" + id + "\"" + (symbols[k].box ? " width=\"64\" height=\"64\"" : "") + "/></svg>";
        snprintf(b, sizeof b, "<text x=\"%d\" y=\"%d\" fill=\"#777\">", x + CELL_W / 2, y + CELL_H - 12);
        out += b + nxt::escape(label) + "</text>";
    }
    out += "</g></svg>";
    return true;
}

nitrosvg *nitrosvg_load(const void *data, size_t len) {
    std::string src(static_cast<const char *>(data), len);
    if (src.find("<svg:svg") != std::string::npos) src = unprefix(src);
    if (src.find("<symbol") != std::string::npos) {
        std::string sheet;
        if (sprite_sheet(src, sheet)) src = std::move(sheet);
    }
    if (src.find("var(--") != std::string::npos) src = var_fallbacks(src);
    if (src.find("light-dark(") != std::string::npos) src = light_colours(src);
    if (src.find("<text") != std::string::npos && src.find("xml:space=\"preserve\"") == std::string::npos &&
        src.find("xml:space='preserve'") == std::string::npos)
        src = text_newlines(src);
    const char *s = src.data();
    len = src.size();
    std::lock_guard<std::mutex> g(nf_lock());
    set_resolver();
    auto doc = lunasvg::Document::loadFromData(s, len);
    if (!doc) return nullptr;
    double w = doc->width(), h = doc->height(), ox = 0, oy = 0;
    if (!(w > 0 && h > 0)) {   // no width / height / viewBox: what it draws decides
        lunasvg::Box b = doc->boundingBox();
        if (!(b.w > 0 && b.h > 0)) return nullptr;
        w = b.w;
        h = b.h;
        ox = b.x;
        oy = b.y;
    }
    return new nitrosvg{std::move(doc), w, h, ox, oy, 0};
}

void nitrosvg_free(nitrosvg *svg) {
    if (!svg) return;
    std::lock_guard<std::mutex> g(nf_lock());
    delete svg;
}

void nitrosvg_set_background(nitrosvg *svg, int background) {
    if (svg && background >= 0 && background <= 3) svg->background = background;
}

void nitrosvg_size(const nitrosvg *svg, double *w, double *h) {
    *w = svg->w;
    *h = svg->h;
}

int nitrosvg_render(nitrosvg *svg, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0) {
    if (!nd::renderArgs(w, h, pitch, scale)) return -1;
    nd::fill(bgra, w, h, pitch, nd::background(svg->background));
    std::lock_guard<std::mutex> g(nf_lock());
    lunasvg::Bitmap bitmap(bgra, w, h, (int)pitch);   // ARGB32 premultiplied = BGRA bytes (little endian)
    if (bitmap.isNull()) return -1;
    lunasvg::Matrix m((float)scale, 0, 0, (float)scale, (float)(-(x0 + svg->ox) * scale), (float)(-(y0 + svg->oy) * scale));
    svg->doc->render(bitmap, m);
    return 0;
}
