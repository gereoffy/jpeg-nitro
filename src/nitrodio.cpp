// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodio: draw.io diagrams (see nitrodio.h). The file: <mxfile> with a <diagram> per page,
// each holding an <mxGraphModel> or its compressed text; or a bare <mxGraphModel>. The model's
// <root> lists the cells in drawing order: layers (children of the root cell), vertices and
// edges, their geometry (<mxGeometry>, relative to the parent vertex; an edge's label cells
// placed along it) and a style string ("name;key=value;..." over draw.io's default stylesheet).
// The connectors are routed here as mxGraph routes them (see Builder::edge): straight through
// their waypoints, or by the edge style's orthogonal connector.
#include "nitrodio.h"
#include "nitro_draw.h"
#include "nitro_font.h"
#include "nitro_num.h"   // (nn_strtod: numbers read the same in every locale)
#include "nitrojpeg.h"   // (the pictures a diagram holds as data: URIs)
#include "nitrosvg.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <plutovg.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Pt {
    double x = 0, y = 0;
};
struct Box {
    double x = 0, y = 0, w = 0, h = 0;
    Pt centre() const { return {x + w / 2, y + h / 2}; }
};

using PathP = std::shared_ptr<plutovg_path_t>;
PathP newPath() { return PathP(plutovg_path_create(), plutovg_path_destroy); }

// --- styles ------------------------------------------------------------------

using Style = std::unordered_map<std::string, std::string>;

// draw.io's default stylesheet (default.xml): the defaults of vertices and edges, and the named
// styles a style string may start with
void applyNamed(Style &s, const std::string &name) {
    static const std::map<std::string, std::vector<std::pair<const char *, const char *>>> named = {
        {"text", {{"fillColor", "none"}, {"gradientColor", "none"}, {"strokeColor", "none"}, {"align", "left"}, {"verticalAlign", "top"}}},
        {"edgeLabel", {{"fillColor", "none"}, {"gradientColor", "none"}, {"strokeColor", "none"}, {"align", "left"}, {"verticalAlign", "top"},
                       {"labelBackgroundColor", "#ffffff"}, {"fontSize", "11"}}},
        {"label", {{"fontStyle", "1"}, {"align", "left"}, {"verticalAlign", "middle"}, {"spacing", "2"}, {"spacingLeft", "52"}}},
        {"icon", {{"align", "center"}, {"verticalAlign", "top"}, {"verticalLabelPosition", "bottom"}}},
        {"swimlane", {{"shape", "swimlane"}, {"fontSize", "12"}, {"fontStyle", "1"}, {"startSize", "23"}}},
        {"group", {{"verticalAlign", "top"}, {"fillColor", "none"}, {"strokeColor", "none"}, {"gradientColor", "none"}}},
        {"ellipse", {{"shape", "ellipse"}, {"perimeter", "ellipsePerimeter"}}},
        {"rhombus", {{"shape", "rhombus"}, {"perimeter", "rhombusPerimeter"}}},
        {"triangle", {{"shape", "triangle"}, {"perimeter", "trianglePerimeter"}}},
        {"line", {{"shape", "line"}, {"strokeWidth", "4"}, {"labelBackgroundColor", "#ffffff"}, {"verticalAlign", "top"}, {"spacingTop", "8"}}},
        {"image", {{"shape", "image"}, {"labelBackgroundColor", "#ffffff"}, {"verticalAlign", "top"}, {"verticalLabelPosition", "bottom"}}},
        {"roundImage", {{"shape", "image"}, {"verticalAlign", "top"}, {"verticalLabelPosition", "bottom"}}},
        {"rhombusImage", {{"shape", "image"}, {"verticalAlign", "top"}, {"verticalLabelPosition", "bottom"}}},
        {"arrow", {{"shape", "arrow"}, {"edgeStyle", "none"}, {"fillColor", "#ffffff"}}},
        {"fancy", {{"shadow", "1"}, {"glass", "1"}}},
    };
    // (default.xml's colour pairs: plain-<colour>, and <colour> = fancy (shadow, glass) with them)
    static const std::map<std::string, std::array<const char *, 3>> colours = {
        {"gray", {"#F5F5F5", "#B3B3B3", "#666666"}},   {"blue", {"#DAE8FC", "#7EA6E0", "#6C8EBF"}},   {"green", {"#D5E8D4", "#97D077", "#82B366"}},
        {"turquoise", {"#D5E8D4", "#67AB9F", "#6A9153"}}, {"yellow", {"#FFF2CC", "#FFD966", "#D6B656"}}, {"orange", {"#FFCD28", "#FFA500", "#D79B00"}},
        {"red", {"#F8CECC", "#EA6B66", "#B85450"}},     {"pink", {"#E6D0DE", "#B5739D", "#996185"}},   {"purple", {"#E1D5E7", "#8C6C9C", "#9673A6"}},
    };
    bool plain = name.compare(0, 6, "plain-") == 0;
    auto c = colours.find(plain ? name.substr(6) : name);
    if (c != colours.end()) {
        if (!plain) s["shadow"] = "1", s["glass"] = "1";
        s["fillColor"] = c->second[0], s["gradientColor"] = c->second[1], s["strokeColor"] = c->second[2];
        return;
    }
    auto f = named.find(name);
    if (f == named.end()) {
        if (name == "rounded") s["rounded"] = "1";
        else if (name == "shadow") s["shadow"] = "1";
        else if (!name.empty() && !s.count("shape") && name.compare(0, 7, "mxgraph") == 0) s["shape"] = name;
        return;
    }
    for (auto &kv : f->second) s[kv.first] = kv.second;
}

Style parseStyle(const std::string &str, bool edge) {
    Style s;
    s["fontSize"] = edge ? "11" : "12";
    s["fontFamily"] = "Helvetica";
    s["fontColor"] = "#000000";
    s["strokeColor"] = "#000000";
    s["align"] = "center";
    s["verticalAlign"] = "middle";
    if (edge) {
        s["endArrow"] = "classic";
        s["rounded"] = "1";   // (default.xml's defaultEdge: edges are rounded unless they say rounded=0)
        s["labelBackgroundColor"] = "#ffffff";
    } else
        s["fillColor"] = "#ffffff";
    size_t i = 0;
    while (i <= str.size()) {
        size_t e = str.find(';', i);
        if (e == std::string::npos) e = str.size();
        std::string tok = str.substr(i, e - i);
        size_t eq = tok.find('=');
        if (tok.compare(0, 7, "base64,") == 0 && s.count("image")) s["image"] += ";" + tok;   // (data:...;base64, : its = padding no key)
        else if (eq == std::string::npos) applyNamed(s, tok);
        else s[tok.substr(0, eq)] = tok.substr(eq + 1);
        i = e + 1;
    }
    return s;
}

const std::string &str(const Style &s, const char *k) {
    static const std::string empty;
    auto f = s.find(k);
    return f == s.end() ? empty : f->second;
}
double num(const Style &s, const char *k, double def) {
    const std::string &v = str(s, k);
    if (v.empty()) return def;
    char *end = nullptr;
    double d = nn_strtod(v.c_str(), &end);
    return end != v.c_str() && std::isfinite(d) && std::fabs(d) < 1e7 ? d : def;   // (a damaged file's huge numbers: the default)
}

// a colour (0xAARRGGBB; 0: none): #rgb, #rrggbb, a few names, rgb(...), light-dark(a, b): a
uint32_t parseColour(std::string v, uint32_t def) {
    if (v.empty()) return def;
    if (v.compare(0, 11, "light-dark(") == 0) v = v.substr(11, v.find(',') - 11);
    while (!v.empty() && v[0] == ' ') v.erase(0, 1);
    if (v == "none" || v == "transparent") return 0;
    if (v == "default") return def;
    if (v[0] == '#') {
        unsigned long n = strtoul(v.c_str() + 1, nullptr, 16);
        if (v.size() == 4) n = (n & 0xf00) * 0x1100 | (n & 0xf0) * 0x110 | (n & 0xf) * 0x11;
        return 0xff000000u | (uint32_t)(n & 0xffffff);
    }
    if (v.compare(0, 4, "rgb(") == 0 || v.compare(0, 5, "rgba(") == 0) {   // (rgba: its alpha; fully transparent: 0x00000001,
        int r = 0, g = 0, b = 0;                                              // a colour of its own, not none)
        double al = 1;
        int n = sscanf(v.c_str() + (v[3] == 'a' ? 5 : 4), "%d , %d , %d , %lf", &r, &g, &b, &al);
        if (n >= 3) {
            uint32_t a = n == 4 && std::isfinite(al) ? (uint32_t)std::lround(std::clamp(al, 0.0, 1.0) * 255) : 255;
            uint32_t c = a << 24 | (uint32_t)(std::clamp(r, 0, 255) << 16 | std::clamp(g, 0, 255) << 8 | std::clamp(b, 0, 255));
            return c ? c : 1;
        }
        return def;
    }
    static const std::map<std::string, uint32_t> names = {
        {"white", 0xffffff}, {"black", 0x000000}, {"red", 0xff0000},    {"green", 0x008000}, {"blue", 0x0000ff},  {"yellow", 0xffff00},
        {"orange", 0xffa500}, {"gray", 0x808080}, {"grey", 0x808080},   {"silver", 0xc0c0c0}, {"purple", 0x800080}, {"lightgray", 0xd3d3d3},
        {"darkgray", 0xa9a9a9}, {"navy", 0x000080}, {"maroon", 0x800000}, {"cyan", 0x00ffff}, {"magenta", 0xff00ff},
    };
    std::string l = v;
    for (char &c : l) c = (char)tolower((unsigned char)c);
    auto f = names.find(l);
    return f != names.end() ? 0xff000000u | f->second : def;
}
uint32_t colour(const Style &s, const char *k, uint32_t def) { return parseColour(str(s, k), def); }
// (labelBackgroundColor=default: the page's background, white)
uint32_t labelBackground(const Style &s) {
    const std::string &v = str(s, "labelBackgroundColor");
    return v == "default" ? 0xffffffffu : parseColour(v, 0);
}
uint32_t withOpacity(uint32_t c, double opacity) {   // (0..100)
    if (opacity >= 100) return c;
    uint32_t a = (uint32_t)std::lround((c >> 24) * std::clamp(opacity, 0.0, 100.0) / 100);
    return a << 24 | (c & 0xffffff);
}

// --- the display list ----------------------------------------------------------

struct Run {   // a piece of a label's line in one font
    plutovg_font_face_t *face = nullptr;
    float size = 12;
    uint32_t rgba = 0xff000000;
    float x = 0, y = 0;   // its baseline's start
    std::string text;
    float underline = 0;   // (its width when underlined)
    float strike = 0;      // (its width when struck through)
};
struct Label {
    struct Rect {
        float x, y, w, h;
        uint32_t rgba;
    };
    std::vector<Run> runs;
    std::vector<Rect> rects;   // (an HTML table's backgrounds and borders: under the text)
    uint32_t background = 0;   // (0: none) behind the text box
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the text box
    bool turned = false;   // rotated about (cx, cy)
    float cx = 0, cy = 0, angle = 0;
    bool clip = false;     // (overflow=hidden: drawn only inside cx0, cy0 - cx1, cy1, in its own frame (before turning))
    float clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
};
struct Picture {   // premultiplied BGRA
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};
struct Item {
    PathP path;
    uint32_t fill = 0, fill2 = 0, stroke = 0;   // fill2: a gradient's end (fill at gx0, gy0 to fill2 at gx1, gy1)
    float gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
    float width = 1;
    std::vector<float> dash;
    std::shared_ptr<Label> label;
    float hachure = 0, hachureAngle = -41, hachureWeight = 1;   // (sketch: the fill as lines hachure apart, at the angle)
    bool crossHatch = false;                                    // (and across them)
    std::shared_ptr<Picture> picture;   // an image shape's picture, in ix0, iy0 - ix1, iy1 (aspect: kept, centred)
    std::string uri;                    // (its data: URI until it is decoded)
    float ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
    bool aspect = true;
    float pictureAlpha = 1;   // (the image shape's opacity)
    bool turned = false;      // (the picture turned / mirrored with its shape by pm, after its placing in ix0 .. iy1)
    plutovg_matrix_t pm{1, 0, 0, 1, 0, 0};
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
};
struct Page {
    double x = 0, y = 0, w = 0, h = 0;   // the shown area
    uint32_t background = 0;   // the diagram's own (0: none)
    std::vector<Item> items;
};

// --- the file ------------------------------------------------------------------

std::string base64(const std::string &s) {
    std::string out;
    out.reserve(s.size() * 3 / 4);
    unsigned v = 0;
    int bits = 0;
    for (unsigned char c : s) {
        int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (d < 0) continue;
        v = v << 6 | (unsigned)d;
        if ((bits += 6) >= 8) out += (char)(v >> (bits -= 8));
    }
    return out;
}
// The XML of a page, 64 MB at most and at most a hundred times the compressed size (a real
// diagram's XML deflates 5-20x; a few hundred KB inflating to hundreds of MB of elements had
// libxml2 build a tree of gigabytes).
struct Inflater {   // (a z_stream ended however the inflating stops)
    z_stream z{};
    bool ok = inflateInit2(&z, -15) == Z_OK;
    ~Inflater() {
        if (ok) inflateEnd(&z);
    }
};
bool inflateRaw(const std::string &in, std::string &out) {
    Inflater inf;
    z_stream &z = inf.z;
    if (!inf.ok) return false;
    z.next_in = (Bytef *)in.data();
    z.avail_in = (uInt)in.size();
    const size_t limit = std::min<size_t>(64u << 20, in.size() * 100 + (1u << 20));
    char buf[65536];
    int rc;
    do {
        z.next_out = (Bytef *)buf;
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        out.append(buf, sizeof buf - z.avail_out);
    } while (rc == Z_OK && out.size() <= limit);
    if (out.size() > limit) out.clear();
    return rc == Z_STREAM_END && !out.empty();
}
std::string urlDecode(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtoul(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else
            o += s[i];
    }
    return o;
}

std::string attr(xmlNode *n, const char *name) {
    xmlChar *v = xmlGetProp(n, (const xmlChar *)name);
    if (!v) return std::string();
    std::string s((const char *)v);
    xmlFree(v);
    return s;
}
double attrNum(xmlNode *n, const char *name, double def = 0) {
    std::string v = attr(n, name);
    if (v.empty()) return def;
    double d = nn_strtod(v.c_str(), nullptr);
    return std::isfinite(d) && std::fabs(d) < 1e8 ? d : def;
}
bool is(xmlNode *n, const char *name) { return n->type == XML_ELEMENT_NODE && !strcmp((const char *)n->name, name); }

struct Cell {
    std::string id, parent, source, target, value;
    Style style;
    bool vertex = false, edge = false, visible = true, collapsed = false;
    bool relative = false;
    double x = 0, y = 0, w = 0, h = 0;   // geometry (relative to the parent vertex)
    bool hasSource = false, hasTarget = false, hasOffset = false;
    Pt sourcePoint, targetPoint, offset;
    std::vector<Pt> points;
    // worked out
    int state = 0;   // (absolute box: 0 not yet, 1 being worked out, 2 done)
    int shownState = 0;   // (0 not yet, 1 shown, 2 hidden, 3 being worked out)
    Box box;
    Pt org;   // (where its children start: its box's corner, but a transparentBounds container's geometry)
    bool transparent = false;   // (transparentBounds=1: drawn round its children, local: that box from its geometry's corner)
    int localState = 0;
    Box local;
    std::vector<Pt> route;   // an edge's points, absolute
    std::unordered_map<std::string, std::string> data;   // a UserObject / object's attributes (the placeholders' values)
    bool placeholders = false;                           // (its label's %name% replaced by them)
};

void readGeometry(Cell &c, xmlNode *g) {
    c.x = attrNum(g, "x");
    c.y = attrNum(g, "y");
    c.w = attrNum(g, "width");
    c.h = attrNum(g, "height");
    c.relative = attr(g, "relative") == "1";
    for (xmlNode *k = g->children; k; k = k->next) {
        if (is(k, "mxPoint")) {
            Pt p{attrNum(k, "x"), attrNum(k, "y")};
            std::string as = attr(k, "as");
            if (as == "sourcePoint") c.sourcePoint = p, c.hasSource = true;
            else if (as == "targetPoint") c.targetPoint = p, c.hasTarget = true;
            else if (as == "offset") c.offset = p, c.hasOffset = true;
        } else if (is(k, "Array") && attr(k, "as") == "points") {
            for (xmlNode *q = k->children; q; q = q->next)
                if (is(q, "mxPoint") && c.points.size() < 10000) c.points.push_back({attrNum(q, "x"), attrNum(q, "y")});
        }
    }
}

// --- labels --------------------------------------------------------------------

struct Seg {
    std::string text;
    bool bold = false, italic = false;
    uint8_t underline = 0;   // (1: underlined, 2: struck through, 3: both)
    uint32_t rgba = 0;   // (an HTML label's own colour; 0: the label's)
    float size = 0;      // (an HTML label's own font size; 0: the label's)
    uint32_t bg = 0;     // (an HTML label's highlight: a span's background colour)
};

// a block's margins (top, bottom) in px: a browser's defaults (em of the block's font size), style margin / margin-top /
// margin-bottom over them
void tagMargins(const std::string &tag, float &top, float &bottom) {
    auto value = [&](size_t at, float &out) {
        const char *p = tag.c_str() + at;
        while (*p == ' ') p++;
        char *end = nullptr;
        double d = nn_strtod(p, &end);
        if (end == p) return (const char *)nullptr;
        out = (float)(end[0] == 'e' && end[1] == 'm' ? d * 16 : d);
        while (*end && *end != ' ' && *end != ';' && *end != '"' && *end != '\'') end++;
        return (const char *)end;
    };
    size_t at = tag.find("margin:");
    if (at != std::string::npos) {   // (1 value: all; 2: vertical horizontal; 3 / 4: top, .., bottom)
        float v[4];
        int n = 0;
        const char *p = tag.c_str() + at + 7;
        while (n < 4) {
            const char *e = value((size_t)(p - tag.c_str()), v[n]);
            if (!e) break;
            n++, p = e;
        }
        if (n) top = v[0], bottom = n >= 3 ? v[2] : v[0];
    }
    if ((at = tag.find("margin-top:")) != std::string::npos) value(at + 11, top);
    if ((at = tag.find("margin-bottom:")) != std::string::npos) value(at + 14, bottom);
}

// the font size an HTML tag sets: style="font-size: Npx / Npt / N% / Nem" (the last two of base, the size around
// it) or <font size="1..7">; 0: none
float tagSize(const std::string &tag, float base) {
    size_t at = tag.find("font-size");
    if (at != std::string::npos) {
        size_t v = tag.find(':', at);
        if (v != std::string::npos) {
            char *end = nullptr;
            const char *p = tag.c_str() + v + 1;
            while (*p == ' ') p++;
            double d = nn_strtod(p, &end);
            if (end != p && d == 0) return 0.01f;   // (0: no text to see, its lines nothing high)
            if (end != p && d > 0 && d < 1000) {
                if (end[0] == '%') return (float)(base * d / 100);
                if (end[0] == 'e' && end[1] == 'm') return (float)(base * d);
                return (float)(end[0] == 'p' && end[1] == 't' ? d * 4 / 3 : d);
            }
        }
    }
    std::string low = tag;
    for (char &ch : low) ch = (char)tolower((unsigned char)ch);
    if (low.compare(0, 5, "font ") == 0 && (at = low.find("size=")) != std::string::npos) {
        static const float sizes[8] = {0, 10, 13, 16, 18, 24, 32, 48};
        size_t v = at + 5;
        while (v < low.size() && (low[v] == '"' || low[v] == '\'')) v++;
        if (v < low.size() && low[v] >= '1' && low[v] <= '7') return sizes[low[v] - '0'];
    }
    return 0;
}

// the text colour an HTML tag sets: <font color="..."> or style="color: ..." (not background-color); 0: none
uint32_t tagColour(const std::string &tag) {
    for (size_t at = 0; (at = tag.find("color", at)) != std::string::npos; at += 5) {
        if (at > 0 && (isalnum((unsigned char)tag[at - 1]) || tag[at - 1] == '-' || tag[at - 1] == '_')) continue;
        size_t v = at + 5;
        while (v < tag.size() && tag[v] == ' ') v++;
        if (v >= tag.size() || (tag[v] != '=' && tag[v] != ':')) continue;
        v++;
        while (v < tag.size() && (tag[v] == ' ' || tag[v] == '"' || tag[v] == '\'')) v++;
        size_t e = v;
        int depth = 0;
        while (e < tag.size() && (depth || (tag[e] != ';' && tag[e] != '"' && tag[e] != '\'' && tag[e] != ' '))) {
            if (tag[e] == '(') depth++;
            else if (tag[e] == ')') depth--;
            e++;
        }
        uint32_t c = parseColour(tag.substr(v, e - v), 0);
        if (c) return c;
    }
    return 0;
}
struct Para : std::vector<Seg> {   // (a line of the label, and the lines it wraps into)
    float indent = 0;     // (a list item's: its lists' padding, 40 px each, as a browser's)
    std::string marker;   // (a list item's bullet or number, in the margin before it)
    std::string align;    // (its block's text-align: left / center / right; empty: the label's)
    float strut = 0;      // (its block's font size: a line with text at least 1.2 of it high, as a browser's strut; 0: the label's)
};

void putEntity(std::string &o, const std::string &e) {
    if (e == "nbsp") o += "\xc2\xa0";
    else if (e == "amp") o += '&';
    else if (e == "lt") o += '<';
    else if (e == "gt") o += '>';
    else if (e == "quot") o += '"';
    else if (e == "apos") o += '\'';
    else if (!e.empty() && e[0] == '#') {
        unsigned long cp = e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? strtoul(e.c_str() + 2, nullptr, 16) : strtoul(e.c_str() + 1, nullptr, 10);
        if (cp && cp < 0x110000) nf_utf8_put(o, (uint32_t)cp);
    } else {   // (HTML 4's named characters: Latin-1, punctuation, arrows, maths, Greek)
        static const std::unordered_map<std::string, uint32_t> named = {
        {"AElig", 0xc6}, {"Aacute", 0xc1}, {"Acirc", 0xc2}, {"Agrave", 0xc0}, {"Alpha", 0x391}, {"Aring", 0xc5}, {"Atilde", 0xc3}, {"Auml", 0xc4},
        {"Beta", 0x392}, {"Ccedil", 0xc7}, {"Chi", 0x3a7}, {"Dagger", 0x2021}, {"Delta", 0x394}, {"ETH", 0xd0}, {"Eacute", 0xc9}, {"Ecirc", 0xca},
        {"Egrave", 0xc8}, {"Epsilon", 0x395}, {"Eta", 0x397}, {"Euml", 0xcb}, {"Gamma", 0x393}, {"Iacute", 0xcd}, {"Icirc", 0xce}, {"Igrave", 0xcc},
        {"Iota", 0x399}, {"Iuml", 0xcf}, {"Kappa", 0x39a}, {"Lambda", 0x39b}, {"Mu", 0x39c}, {"Ntilde", 0xd1}, {"Nu", 0x39d}, {"OElig", 0x152},
        {"Oacute", 0xd3}, {"Ocirc", 0xd4}, {"Ograve", 0xd2}, {"Omega", 0x3a9}, {"Omicron", 0x39f}, {"Oslash", 0xd8}, {"Otilde", 0xd5}, {"Ouml", 0xd6},
        {"Phi", 0x3a6}, {"Pi", 0x3a0}, {"Prime", 0x2033}, {"Psi", 0x3a8}, {"Rho", 0x3a1}, {"Scaron", 0x160}, {"Sigma", 0x3a3}, {"THORN", 0xde},
        {"Tau", 0x3a4}, {"Theta", 0x398}, {"Uacute", 0xda}, {"Ucirc", 0xdb}, {"Ugrave", 0xd9}, {"Upsilon", 0x3a5}, {"Uuml", 0xdc}, {"Xi", 0x39e},
        {"Yacute", 0xdd}, {"Yuml", 0x178}, {"Zeta", 0x396}, {"aacute", 0xe1}, {"acirc", 0xe2}, {"acute", 0xb4}, {"aelig", 0xe6}, {"agrave", 0xe0},
        {"alefsym", 0x2135}, {"alpha", 0x3b1}, {"and", 0x2227}, {"ang", 0x2220}, {"aring", 0xe5}, {"asymp", 0x2248}, {"atilde", 0xe3}, {"auml", 0xe4},
        {"bdquo", 0x201e}, {"beta", 0x3b2}, {"brvbar", 0xa6}, {"bull", 0x2022}, {"cap", 0x2229}, {"ccedil", 0xe7}, {"cedil", 0xb8}, {"cent", 0xa2},
        {"chi", 0x3c7}, {"circ", 0x2c6}, {"clubs", 0x2663}, {"cong", 0x2245}, {"copy", 0xa9}, {"crarr", 0x21b5}, {"cup", 0x222a}, {"curren", 0xa4},
        {"dArr", 0x21d3}, {"dagger", 0x2020}, {"darr", 0x2193}, {"deg", 0xb0}, {"delta", 0x3b4}, {"diams", 0x2666}, {"divide", 0xf7}, {"eacute", 0xe9},
        {"ecirc", 0xea}, {"egrave", 0xe8}, {"empty", 0x2205}, {"emsp", 0x2003}, {"ensp", 0x2002}, {"epsilon", 0x3b5}, {"equiv", 0x2261}, {"eta", 0x3b7},
        {"eth", 0xf0}, {"euml", 0xeb}, {"euro", 0x20ac}, {"exist", 0x2203}, {"fnof", 0x192}, {"forall", 0x2200}, {"frac12", 0xbd}, {"frac14", 0xbc},
        {"frac34", 0xbe}, {"frasl", 0x2044}, {"gamma", 0x3b3}, {"ge", 0x2265}, {"hArr", 0x21d4}, {"harr", 0x2194}, {"hearts", 0x2665}, {"hellip", 0x2026},
        {"iacute", 0xed}, {"icirc", 0xee}, {"iexcl", 0xa1}, {"igrave", 0xec}, {"image", 0x2111}, {"infin", 0x221e}, {"int", 0x222b}, {"iota", 0x3b9},
        {"iquest", 0xbf}, {"isin", 0x2208}, {"iuml", 0xef}, {"kappa", 0x3ba}, {"lArr", 0x21d0}, {"lambda", 0x3bb}, {"lang", 0x2329}, {"laquo", 0xab},
        {"larr", 0x2190}, {"lceil", 0x2308}, {"ldquo", 0x201c}, {"le", 0x2264}, {"lfloor", 0x230a}, {"lowast", 0x2217}, {"loz", 0x25ca}, {"lrm", 0x200e},
        {"lsaquo", 0x2039}, {"lsquo", 0x2018}, {"macr", 0xaf}, {"mdash", 0x2014}, {"micro", 0xb5}, {"middot", 0xb7}, {"minus", 0x2212}, {"mu", 0x3bc},
        {"nabla", 0x2207}, {"ndash", 0x2013}, {"ne", 0x2260}, {"ni", 0x220b}, {"not", 0xac}, {"notin", 0x2209}, {"nsub", 0x2284}, {"ntilde", 0xf1},
        {"nu", 0x3bd}, {"oacute", 0xf3}, {"ocirc", 0xf4}, {"oelig", 0x153}, {"ograve", 0xf2}, {"oline", 0x203e}, {"omega", 0x3c9}, {"omicron", 0x3bf},
        {"oplus", 0x2295}, {"or", 0x2228}, {"ordf", 0xaa}, {"ordm", 0xba}, {"oslash", 0xf8}, {"otilde", 0xf5}, {"otimes", 0x2297}, {"ouml", 0xf6},
        {"para", 0xb6}, {"part", 0x2202}, {"permil", 0x2030}, {"perp", 0x22a5}, {"phi", 0x3c6}, {"pi", 0x3c0}, {"piv", 0x3d6}, {"plusmn", 0xb1},
        {"pound", 0xa3}, {"prime", 0x2032}, {"prod", 0x220f}, {"prop", 0x221d}, {"psi", 0x3c8}, {"rArr", 0x21d2}, {"radic", 0x221a}, {"rang", 0x232a},
        {"raquo", 0xbb}, {"rarr", 0x2192}, {"rceil", 0x2309}, {"rdquo", 0x201d}, {"real", 0x211c}, {"reg", 0xae}, {"rfloor", 0x230b}, {"rho", 0x3c1},
        {"rlm", 0x200f}, {"rsaquo", 0x203a}, {"rsquo", 0x2019}, {"sbquo", 0x201a}, {"scaron", 0x161}, {"sdot", 0x22c5}, {"sect", 0xa7}, {"shy", 0xad},
        {"sigma", 0x3c3}, {"sigmaf", 0x3c2}, {"sim", 0x223c}, {"spades", 0x2660}, {"sub", 0x2282}, {"sube", 0x2286}, {"sum", 0x2211}, {"sup", 0x2283},
        {"sup1", 0xb9}, {"sup2", 0xb2}, {"sup3", 0xb3}, {"supe", 0x2287}, {"szlig", 0xdf}, {"tau", 0x3c4}, {"there4", 0x2234}, {"theta", 0x3b8},
        {"thetasym", 0x3d1}, {"thinsp", 0x2009}, {"thorn", 0xfe}, {"tilde", 0x2dc}, {"times", 0xd7}, {"trade", 0x2122}, {"uArr", 0x21d1}, {"uacute", 0xfa},
        {"uarr", 0x2191}, {"ucirc", 0xfb}, {"ugrave", 0xf9}, {"uml", 0xa8}, {"upsih", 0x3d2}, {"upsilon", 0x3c5}, {"uuml", 0xfc}, {"weierp", 0x2118},
        {"xi", 0x3be}, {"yacute", 0xfd}, {"yen", 0xa5}, {"yuml", 0xff}, {"zeta", 0x3b6}, {"zwj", 0x200d}, {"zwnj", 0x200c},
        };
        auto f = named.find(e);
        if (f != named.end()) nf_utf8_put(o, f->second);
        else o += "&" + e + ";";
    }
}

uint32_t tagBackground(const std::string &tag);
std::string cssValue(const std::string &tag, const char *prop);
std::string htmlAttr(const std::string &tag, const char *name);
double cssLength(const std::string &v, double whole, double em);

// A label's text as paragraphs: plain (its line breaks), or HTML: <br> and the block elements
// break lines, <b> <strong> <i> <em> (and the bold / italic of fontStyle) set the font, the
// other tags are dropped, white space collapses
std::vector<Para> parseLabel(const std::string &v, bool html, bool bold, bool italic, int underline, float baseSize) {
    std::vector<Para> paras(1);
    std::vector<std::pair<std::string, uint32_t>> colours;   // (the open tags that set a colour: the last one's is the text's)
    std::vector<int> lists;   // (the open <ul> (0) / <ol> (its next number))
    std::vector<std::pair<std::string, float>> sizes;   // (the open tags that set a font size, as colours)
    std::vector<std::pair<std::string, float>> margins;   // (the open blocks' bottom margins)
    int unordered = 0;        // (an <li> outside a list: a bullet)
    std::vector<std::pair<std::string, uint32_t>> highlights;   // (the open inline tags with a background colour)
    std::vector<std::pair<std::string, std::string>> aligns;    // (the open blocks and their text-align, "" if none)
    std::vector<std::pair<std::string, float>> lefts;           // (the open blocks' margin-left: their text indented)
    std::vector<std::pair<std::string, float>> struts;          // (the open blocks' font sizes)
    struct Deco {
        std::string name;
        int u, s;
    };
    std::vector<Deco> decos;   // (the open blocks' text-decoration: underline / line-through)
    auto curSize = [&]() { return sizes.empty() || sizes.back().second <= 0 ? baseSize : sizes.back().second; };
    float gap = 0;   // (the block margins due before the next text: collapsed, the largest)
    auto add = [&](const std::string &t, bool b, bool i, int u) {
        if (t.empty()) return;
        if (gap > 0 && paras.back().empty()) {   // (a line of its own, gap high)
            Seg sp;
            sp.size = gap / 1.2f;
            Para gp;
            gp.push_back(sp);
            paras.insert(paras.end() - 1, gp);
        }
        gap = 0;
        Para &p = paras.back();
        if (p.empty() && p.marker.empty())
            for (const auto &lm : lefts) p.indent += lm.second;
        if (p.empty() && !struts.empty()) p.strut = struts.back().second;
        if (p.empty())
            for (size_t k = aligns.size(); k-- > 0;)
                if (!aligns[k].second.empty()) {
                    p.align = aligns[k].second;
                    break;
                }
        uint32_t rgba = colours.empty() ? 0 : colours.back().second;
        float sz = sizes.empty() ? 0 : sizes.back().second;
        uint32_t bg = highlights.empty() ? 0 : highlights.back().second;
        if (!p.empty() && p.back().bold == b && p.back().italic == i && p.back().underline == u && p.back().rgba == rgba && p.back().size == sz &&
            p.back().bg == bg)
            p.back().text += t;
        else p.push_back({t, b, i, (uint8_t)u, rgba, sz, bg});
    };
    if (!html) {
        std::string cur;
        for (char c : v) {
            if (c == '\n') {
                add(cur, bold, italic, underline);
                cur.clear();
                paras.emplace_back();
            } else if (c != '\r')
                cur += c;
        }
        add(cur, bold, italic, underline);
        return paras;
    }
    int b = bold, it = italic, un = underline & 1, st = underline >> 1 & 1;
    struct Added {
        int b, i, u, s;
    };
    std::vector<Added> spans;   // (the bold, italic, underline a <span> / <font> added, taken back at its end)
    bool space = true;   // (white space collapsed: at a line's start, or after one)
    std::string cur;
    auto flush = [&] {
        add(cur, b > 0, it > 0, (un > 0 ? 1 : 0) | (st > 0 ? 2 : 0));
        cur.clear();
    };
    auto newLine = [&] {
        flush();
        paras.emplace_back();
        space = true;
    };
    for (size_t i = 0; i < v.size();) {
        char c = v[i];
        if (c == '<') {
            size_t e = v.find('>', i);
            if (e == std::string::npos) break;
            std::string tag = v.substr(i + 1, e - i - 1);
            i = e + 1;
            bool close = !tag.empty() && tag[0] == '/';
            if (close) tag.erase(0, 1);
            size_t sp = tag.find_first_of(" \t\n/");
            std::string name = tag.substr(0, sp);
            for (char &ch : name) ch = (char)tolower((unsigned char)ch);
            bool isVoid = name == "br" || name == "img" || name == "hr" || name == "wbr" || name == "input" || name == "meta" ||
                          name == "link" || name == "col" || name == "area" || name == "source" || (!name.empty() && name[0] == '!');
            // (every tag that opens is on the colour, size and highlight stacks, with what it sets or else what it is in:
            // its end takes back its own, however its insides set theirs)
            if (!close && !isVoid) {
                uint32_t col = tagColour(tag);
                if (col) flush();
                colours.push_back({name, col ? col : colours.empty() ? 0 : colours.back().second});
                bool block = name == "div" || name == "p" || name == "li" || name == "ul" || name == "ol" || name == "td" || name == "th" ||
                             name == "tr" || name == "table" || (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6');
                if (block) {   // (its text-align: its lines', inside the label's block)
                    std::string ta = cssValue(tag, "text-align");
                    if (ta.empty()) ta = htmlAttr(tag, "align");
                    for (char &ch : ta) ch = (char)tolower((unsigned char)ch);
                    aligns.push_back({name, ta == "left" || ta == "center" || ta == "right" ? ta : ta == "start" ? "left" : ta == "end" ? "right" : ""});
                    if (name != "li" && name != "ul" && name != "ol") {
                        double ml = cssLength(cssValue(tag, "margin-left"), 0, curSize());
                        lefts.push_back({name, ml > 0 && ml < 1e4 ? (float)ml : 0.f});
                    }
                    int du = 0, ds = 0;
                    if (tag.find("text-decoration") != std::string::npos)
                        du = tag.find("underline") != std::string::npos, ds = tag.find("line-through") != std::string::npos;
                    if (du || ds) flush(), un += du, st += ds;
                    decos.push_back({name, du, ds});
                } else {
                    uint32_t hb = tagBackground(tag);
                    if (hb) flush();
                    highlights.push_back({name, hb ? hb : highlights.empty() ? 0 : highlights.back().second});
                }
                float around = curSize();
                static const float heading[6] = {2, 1.5f, 1.17f, 1, 0.83f, 0.67f};   // (a browser's h1 .. h6, bold)
                static const float hmargin[6] = {0.67f, 0.83f, 1, 1.33f, 1.67f, 2.33f};
                bool h = name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
                float own = tagSize(tag, around);   // (its own font size: its margins are of it)
                if (!own && name == "small") own = around / 1.2f;   // (a browser's smaller / larger)
                if (!own && name == "big") own = around * 1.2f;
                if (h) flush(), sizes.push_back({"#" + name, around * heading[name[1] - '1']}), b++;   // (#: a heading's own size)
                if (h || name == "p" || name == "ul" || name == "ol") {   // (its margins: before it now, after it at its end)
                    float fs = own ? own : h ? around * heading[name[1] - '1'] : around, mt = h ? fs * hmargin[name[1] - '1'] : fs, mb = mt;
                    tagMargins(tag, mt, mb);
                    margins.push_back({name, mb});
                    gap = std::max(gap, mt);
                }
                if (own) flush();
                sizes.push_back({name, own ? own : sizes.empty() ? 0.f : sizes.back().second});
                bool blk = h || name == "div" || name == "p" || name == "li" || name == "td" || name == "th";
                if (blk) struts.push_back({name, own ? own : h ? around * heading[name[1] - '1'] : around});
            } else if (close) {
                flush();   // (the text before it still in the tags it closes)
                for (size_t k = struts.size(); k-- > 0;)
                    if (struts[k].first == name) {
                        struts.erase(struts.begin() + (long)k);
                        break;
                    }
                for (size_t k = lefts.size(); k-- > 0;)
                    if (lefts[k].first == name) {
                        lefts.erase(lefts.begin() + (long)k);
                        break;
                    }
                for (size_t k = decos.size(); k-- > 0;)
                    if (decos[k].name == name) {
                        flush();
                        un -= decos[k].u, st -= decos[k].s;
                        decos.erase(decos.begin() + (long)k);
                        break;
                    }
                for (size_t k = aligns.size(); k-- > 0;)
                    if (aligns[k].first == name) {
                        aligns.erase(aligns.begin() + (long)k);
                        break;
                    }
                for (size_t k = highlights.size(); k-- > 0;)
                    if (highlights[k].first == name) {
                        flush();
                        highlights.erase(highlights.begin() + (long)k);
                        break;
                    }
                for (size_t k = margins.size(); k-- > 0;)
                    if (margins[k].first == name) {
                        gap = std::max(gap, margins[k].second);
                        margins.erase(margins.begin() + (long)k);
                        break;
                    }
                for (size_t k = colours.size(); k-- > 0;)
                    if (colours[k].first == name) {
                        flush();
                        colours.erase(colours.begin() + (long)k);
                        break;
                    }
                for (size_t k = sizes.size(); k-- > 0;)   // (its style's size, then a heading's own one)
                    if (sizes[k].first == name) {
                        sizes.erase(sizes.begin() + (long)k);
                        break;
                    }
                for (size_t k = sizes.size(); k-- > 0;)
                    if (sizes[k].first == "#" + name) {
                        sizes.erase(sizes.begin() + (long)k);
                        if (b > 0) b--;
                        break;
                    }
            }
            if (name == "br") newLine();
            else if (name == "hr" && !close) {   // (a rule across the block: 1 px inset border, half an em above and below)
                if (!paras.back().empty() || !cur.empty()) newLine();
                float em = curSize(), half = em / 2;
                Seg top;
                top.size = std::max(gap, half) / 1.2f;
                paras.back().push_back(top);
                Para rule;
                Seg line;
                line.size = 2 / 1.2f;
                rule.push_back(line);
                rule.marker = "\x01";
                paras.push_back(rule);
                paras.emplace_back();
                gap = half;
                space = true;
            }
            else if (name == "ul" || name == "ol") {   // (a list: its items marked, numbered in an <ol>)
                if (!paras.back().empty() || !cur.empty()) newLine();
                if (!close) lists.push_back(name == "ol" ? 1 : 0);
                else if (!lists.empty()) lists.pop_back();
            } else if (name == "li" && !close) {
                if (!paras.back().empty() || !cur.empty()) newLine();
                int &n = lists.empty() ? unordered : lists.back();
                paras.back().indent = 40.0f * (float)std::max<size_t>(lists.size(), 1);
                paras.back().marker = n ? std::to_string(n++) + "." : std::string("\xe2\x80\xa2");
                space = true;
            } else if (name == "div" || name == "p" || name == "li" || name == "tr" || (name.size() == 2 && name[0] == 'h' && isdigit((unsigned char)name[1]))) {
                if (!paras.back().empty() || !cur.empty()) newLine();
            } else if (name == "b" || name == "strong") {
                flush();
                b += close ? -1 : 1;
            } else if (name == "i" || name == "em") {
                flush();
                it += close ? -1 : 1;
            } else if (name == "u" || name == "ins") {
                flush();
                un += close ? -1 : 1;
            } else if (name == "s" || name == "strike" || name == "del") {
                flush();
                st += close ? -1 : 1;
            } else if (name == "span" || name == "font") {
                if (close) {
                    if (!spans.empty()) {
                        flush();
                        b -= spans.back().b, it -= spans.back().i, un -= spans.back().u, st -= spans.back().s;
                        spans.pop_back();
                    }
                } else {
                    int sb = tag.find("font-weight") != std::string::npos && tag.find("bold") != std::string::npos;
                    int si = tag.find("font-style") != std::string::npos && tag.find("italic") != std::string::npos;
                    int su = tag.find("text-decoration") != std::string::npos && tag.find("underline") != std::string::npos;
                    int ss = tag.find("text-decoration") != std::string::npos && tag.find("line-through") != std::string::npos;
                    if (sb || si || su || ss) flush();
                    b += sb, it += si, un += su, st += ss;
                    spans.push_back({sb, si, su, ss});   // (every one, however deep: its end takes back its own; at most a tag per 6 bytes)
                }
            }
            continue;
        }
        if (c == '\n') {   // (a line feed in an HTML label breaks the line: mxText's replaceLinefeeds makes it a <br/>)
            newLine();
            i++;
            continue;
        }
        std::string piece;
        if (c == '&') {   // (its ';' looked for within the 12 bytes an entity can have: not through the rest of the text)
            size_t e = i + 1, stop = std::min(v.size(), i + 12);
            while (e < stop && v[e] != ';') e++;
            if (e < stop) {
                putEntity(piece, v.substr(i + 1, e - i - 1));
                i = e + 1;
            } else
                piece = "&", i++;
        } else
            piece = c, i++;
        if (piece == " " || piece == "\n" || piece == "\t" || piece == "\r") {
            if (!space) cur += ' ';
            space = true;
        } else {
            cur += piece;
            space = false;
        }
    }
    flush();
    for (Para &p : paras)   // (a line's trailing space)
        if (!p.empty() && !p.back().text.empty() && p.back().text.back() == ' ') p.back().text.pop_back();
    if (paras.size() > 1 && paras.back().empty()) paras.pop_back();   // (a last <br> or block end makes no line of its own)
    return paras;
}

float textWidth(plutovg_font_face_t *face, float size, const std::string &t) {
    return face ? plutovg_font_face_text_extents(face, size, t.data(), (int)t.size(), PLUTOVG_TEXT_ENCODING_UTF8, nullptr) : 0;
}

bool hasTable(const std::string &v);
std::shared_ptr<Label> layoutHtmlBlocks(const std::string &value, const Style &s, Box box, double wrapWidth);

// The label laid out in a box (its alignments; wrapWidth > 0: words wrapped to it), the lines
// 1.2 font sizes apart, as draw.io sets them (caller holds nf_lock); blocks: HTML tables laid out as tables
std::shared_ptr<Label> layoutLabel(const std::string &value, const Style &s, Box box, double wrapWidth, bool blocks = true) {
    bool html = str(s, "html") == "1";
    if (html && blocks && hasTable(value)) return layoutHtmlBlocks(value, s, box, wrapWidth);
    int fontStyle = (int)num(s, "fontStyle", 0);
    float size = (float)std::clamp(num(s, "fontSize", 12), 1.0, 1000.0);
    std::vector<Para> paras = parseLabel(value, html, fontStyle & 1, fontStyle & 2, (fontStyle & 4 ? 1 : 0) | (fontStyle & 8 ? 2 : 0), size);
    std::string family = str(s, "fontFamily");
    size_t comma = family.find(',');
    if (comma != std::string::npos) family.resize(comma);
    uint32_t rgba = withOpacity(colour(s, "fontColor", 0xff000000), num(s, "textOpacity", 100));   // (mxText: textOpacity, not opacity)
    if (!(rgba >> 24) && (str(s, "html") != "1" || value.find("olor") == std::string::npos)) return nullptr;   // (no colour: none of its own either)
    plutovg_font_face_t *faces[4] = {nullptr, nullptr, nullptr, nullptr};
    auto face = [&](bool b, bool i) {
        plutovg_font_face_t *&f = faces[b * 2 + i];
        if (!f) f = nf_css_face(family.c_str(), b, i);
        if (!f) f = nf_css_face("sans-serif", b, i);
        return f;
    };
    struct LineRun {
        plutovg_font_face_t *face;
        std::string text;
        float width;
        uint32_t rgba;
        float size;
        uint8_t underline;
        uint32_t bg;
    };
    double alpha = num(s, "textOpacity", 100);
    std::vector<std::vector<LineRun>> lines;
    std::vector<float> widths, indents;   // (a line's width: its indent and its text)
    std::vector<std::string> markers, lineAligns;
    std::vector<float> struts;
    for (const Para &p : paras) {
        lines.emplace_back();
        struts.push_back(p.strut);
        widths.push_back(p.indent);
        indents.push_back(p.indent);
        markers.push_back(p.marker);
        lineAligns.push_back(p.align);
        for (const Seg &sg : p) {
            plutovg_font_face_t *f = face(sg.bold, sg.italic);
            float sz = sg.size > 0 ? sg.size : size;
            if (wrapWidth <= 0) {
                float w = textWidth(f, sz, sg.text);
                lines.back().push_back({f, sg.text, w, sg.rgba ? withOpacity(sg.rgba, alpha) : rgba, sz, sg.underline, sg.bg});
                widths.back() += w;
                continue;
            }
            if (sg.text.empty()) {   // (a spacer: its height only (a margin, an <hr>'s))
                lines.back().push_back({f, std::string(), 0, rgba, sz, sg.underline, sg.bg});
                continue;
            }
            size_t i = 0;   // (word by word: a word with its following spaces)
            while (i < sg.text.size()) {
                size_t e = sg.text.find(' ', i);
                e = e == std::string::npos ? sg.text.size() : e + 1;
                std::string word = sg.text.substr(i, e - i);
                bool glued = i == 0 && !lines.back().empty() && !lines.back().back().text.empty() && lines.back().back().text.back() != ' ';
                i = e;
                float w = textWidth(f, sz, word);
                std::string bare = word;
                while (!bare.empty() && bare.back() == ' ') bare.pop_back();
                std::vector<LineRun> carry;   // (a word begun in the runs before (elő<b>fizetés</b>): wrapped whole, from its start)
                bool wrapIt = widths.back() > indents.back() && widths.back() + textWidth(f, sz, bare) > wrapWidth + 0.5;
                if (wrapIt && glued) {
                    std::vector<LineRun> &ln = lines.back();
                    bool found = false;
                    while (!ln.empty()) {
                        LineRun &r = ln.back();
                        size_t sp = r.text.find_last_of(' ');
                        if (sp != std::string::npos) {
                            if (sp + 1 < r.text.size()) {
                                LineRun t = r;
                                t.text = r.text.substr(sp + 1);
                                t.width = textWidth(t.face, t.size, t.text);
                                carry.insert(carry.begin(), t);
                                r.text.resize(sp + 1);
                                r.width = textWidth(r.face, r.size, r.text);
                            }
                            found = true;
                            break;
                        }
                        carry.insert(carry.begin(), r);
                        ln.pop_back();
                    }
                    if (!found) {   // (no break in the line before it: the word stays, past the width, as a browser's)
                        ln.insert(ln.end(), carry.begin(), carry.end());
                        carry.clear();
                        wrapIt = false;
                    }
                }
                if (wrapIt) {   // (wrapped)
                    if (!lines.back().empty()) {
                        LineRun &last = lines.back().back();
                        while (!last.text.empty() && last.text.back() == ' ') last.text.pop_back();
                        last.width = textWidth(last.face, last.size, last.text);
                        widths.back() = indents.back();
                        for (LineRun &r : lines.back()) widths.back() += r.width;
                    }
                    lines.emplace_back();
                    widths.push_back(p.indent);
                    indents.push_back(p.indent);
                    markers.emplace_back();
                    lineAligns.push_back(p.align);
                    struts.push_back(p.strut);
                    for (const LineRun &r : carry) lines.back().push_back(r), widths.back() += r.width;
                }
                uint32_t col = sg.rgba ? withOpacity(sg.rgba, alpha) : rgba;
                if (!lines.back().empty() && lines.back().back().face == f && lines.back().back().rgba == col && lines.back().back().size == sz &&
                    lines.back().back().underline == sg.underline && lines.back().back().bg == sg.bg)
                    lines.back().back().text += word, lines.back().back().width += w;
                else lines.back().push_back({f, word, w, col, sz, sg.underline, sg.bg});
                widths.back() += w;
            }
        }
    }
    if (str(s, "textDirection") == "vertical-lr") {   // (its lines left to right once turned: in the upright box, bottom to top)
        std::reverse(lines.begin(), lines.end());
        std::reverse(widths.begin(), widths.end());
        std::reverse(indents.begin(), indents.end());
        std::reverse(markers.begin(), markers.end());
        std::reverse(lineAligns.begin(), lineAligns.end());
        std::reverse(struts.begin(), struts.end());
    }
    auto label = std::make_shared<Label>();
    std::vector<float> lineSize(lines.size(), size);   // (a line as high as its largest text: 1.2 font sizes)
    for (size_t l = 0; l < lines.size(); l++)
        if (!lines[l].empty()) {
            lineSize[l] = 0;
            bool text = false;
            for (const LineRun &r : lines[l]) lineSize[l] = std::max(lineSize[l], r.size), text = text || !r.text.empty();
            if (text) lineSize[l] = std::max(lineSize[l], struts[l] > 0 ? struts[l] : size);   // (the block's strut)
        }
    float textH = 0, textW = 0;
    for (float z : lineSize) textH += z * 1.2f;
    for (float w : widths) textW = std::max(textW, w);
    const std::string &va = html && str(s, "overflow") == "fill" ? std::string("top") : str(s, "verticalAlign"), &al = str(s, "align");   // (fill: from the top)
    // (draw.io's text, middle or bottom: lower by about a twelfth of the font size than its lines' height puts it, as
    // measured in its viewer (Chrome); its baseSpacingBottom included)
    double top = va == "top" ? box.y : va == "bottom" ? box.y + box.h - textH + 0.08 * size : box.y + (box.h - textH) / 2 + 0.08 * size;
    plutovg_font_face_t *f0 = face(false, false);
    float asc = size * 0.8f, desc = size * 0.2f;
    if (f0) {
        float a = 0, d = 0, g = 0;
        plutovg_font_face_get_metrics(f0, size, &a, &d, &g, nullptr);
        asc = a, desc = -d;
    }
    float x0 = 1e30f, x1 = -1e30f;
    double y = top;
    // (the lines' block: as wide as its widest line (a wrapped one: the box), placed by align; a line in it by its own
    // block's text-align, else by align)
    double blockW = wrapWidth > 0 || (html && str(s, "overflow") == "fill") ? box.w : textW;   // (overflow=fill: the box's width)
    double blockX = al == "left" ? box.x : al == "right" ? box.x + box.w - blockW : box.x + (box.w - blockW) / 2;
    for (size_t l = 0; l < lines.size(); l++) {
        const std::string &la = lineAligns[l].empty() ? al : lineAligns[l];
        double left = la == "left" ? blockX : la == "right" ? blockX + blockW - widths[l] : blockX + (blockW - widths[l]) / 2;
        float k = lineSize[l] / size, lineH = lineSize[l] * 1.2f;
        float base = (float)(y + (lineH + (asc - desc) * k) / 2);
        y += lineH;
        float pen = (float)left;
        x0 = std::min(x0, pen), x1 = std::max(x1, pen + widths[l]);
        pen += indents[l];
        if (markers[l] == "\x01") {   // (an <hr>: across the block, its top darker)
            label->rects.push_back({(float)blockX, (float)(y - lineH), (float)blockW, 1, 0xff9a9a9au});
            label->rects.push_back({(float)blockX, (float)(y - lineH + 1), (float)blockW, 1, 0xffeeeeeeu});
            x0 = std::min(x0, (float)blockX), x1 = std::max(x1, (float)(blockX + blockW));
            continue;
        }
        if (!markers[l].empty() && !lines[l].empty()) {   // (the marker outside the item's text, a space before it)
            const LineRun &r = lines[l][0];
            bool disc = markers[l][0] == '\xe2';   // (a browser's disc: larger than the font's bullet, its centre 1 em before the text)
            float msz = disc ? r.size * 1.4f : r.size;
            float mx = disc ? pen - r.size - textWidth(r.face, msz, markers[l]) / 2 : pen - textWidth(r.face, r.size, markers[l] + " ");
            if (r.face && (r.rgba >> 24)) label->runs.push_back({r.face, msz, r.rgba, mx, base + (disc ? r.size * 0.1f : 0), markers[l], 0});
            x0 = std::min(x0, mx);
        }
        for (LineRun &r : lines[l]) {
            if ((r.bg >> 24) && r.width > 0) label->rects.push_back({pen, (float)(y - lineH), r.width, lineH, r.bg});
            if (!r.text.empty() && r.face && (r.rgba >> 24))
                label->runs.push_back({r.face, r.size, r.rgba, pen, base, r.text, r.underline & 1 ? textWidth(r.face, r.size, r.text) : 0,
                                       r.underline & 2 ? textWidth(r.face, r.size, r.text) : 0});
            pen += r.width;
        }
    }
    if (label->runs.empty() && label->rects.empty()) return nullptr;   // (an <hr> alone: its rule)
    label->x0 = x0 - 2, label->x1 = x1 + 2;
    label->y0 = (float)top, label->y1 = (float)(top + textH);
    label->background = labelBackground(s);
    if (textW <= 0) label->background = 0;
    return label;
}

// --- HTML tables in labels: a browser's table layout, simplified --------------------------------

// a CSS property's value in a tag's style ("" if none; prop not the end of a longer name)
std::string cssValue(const std::string &tag, const char *prop) {
    size_t n = strlen(prop);
    for (size_t at = 0; (at = tag.find(prop, at)) != std::string::npos; at += n) {
        if (at > 0 && (isalnum((unsigned char)tag[at - 1]) || tag[at - 1] == '-')) continue;
        size_t v = at + n;
        while (v < tag.size() && tag[v] == ' ') v++;
        if (v >= tag.size() || tag[v] != ':') continue;
        v++;
        while (v < tag.size() && tag[v] == ' ') v++;
        size_t e = v;
        int depth = 0;
        while (e < tag.size() && (depth || (tag[e] != ';' && tag[e] != '"' && tag[e] != '\''))) {
            if (tag[e] == '(') depth++;
            else if (tag[e] == ')') depth--;
            e++;
        }
        std::string r = tag.substr(v, e - v);
        while (!r.empty() && r.back() == ' ') r.pop_back();
        return r;
    }
    return "";
}
// an HTML attribute's value (name="v", name='v' or name=v; "" if none)
std::string htmlAttr(const std::string &tag, const char *name) {
    size_t n = strlen(name);
    for (size_t at = 0; (at = tag.find(name, at)) != std::string::npos; at += n) {
        if (at == 0 || !isspace((unsigned char)tag[at - 1])) continue;
        size_t v = at + n;
        while (v < tag.size() && tag[v] == ' ') v++;
        if (v >= tag.size() || tag[v] != '=') continue;
        v++;
        while (v < tag.size() && tag[v] == ' ') v++;
        if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
            size_t e = tag.find(tag[v], v + 1);
            return tag.substr(v + 1, e == std::string::npos ? std::string::npos : e - v - 1);
        }
        size_t e = v;
        while (e < tag.size() && !isspace((unsigned char)tag[e]) && tag[e] != '/') e++;
        return tag.substr(v, e - v);
    }
    return "";
}
// a CSS / HTML length in px (N, Npx, Npt, Nem; N%: of whole, none if whole <= 0); -1: none
double cssLength(const std::string &v, double whole, double em) {
    const char *p = v.c_str();
    while (*p == ' ') p++;
    char *end = nullptr;
    double d = nn_strtod(p, &end);
    if (end == p || !std::isfinite(d) || d < 0 || d > 1e6) return -1;
    if (*end == '%') return whole > 0 ? whole * d / 100 : -1;
    if (end[0] == 'e' && end[1] == 'm') return d * em;
    if (end[0] == 'p' && end[1] == 't') return d * 4 / 3;
    return d;
}
// the colour among a CSS shorthand's words (background: ..., border: 1px solid ...); 0: none
uint32_t cssColour(const std::string &v) {
    size_t i = 0;
    while (i < v.size()) {
        while (i < v.size() && v[i] == ' ') i++;
        size_t e = i;
        int depth = 0;
        while (e < v.size() && (depth || v[e] != ' ')) {
            if (v[e] == '(') depth++;
            else if (v[e] == ')') depth--;
            e++;
        }
        if (uint32_t c = parseColour(v.substr(i, e - i), 0)) return c;
        i = e;
    }
    return 0;
}
// a tag's background colour: style background-color / background, or bgcolor
uint32_t tagBackground(const std::string &tag) {
    std::string v = cssValue(tag, "background-color");
    if (v.empty()) v = cssValue(tag, "background");
    if (uint32_t c = cssColour(v)) return c;
    return parseColour(htmlAttr(tag, "bgcolor"), 0);
}
// a tag's border: its width (px, -1: none said) and colour
double tagBorder(const std::string &tag, uint32_t &rgba) {
    std::string v = cssValue(tag, "border");
    if (v.empty()) return -1;
    if (v == "none" || v == "0") return 0;
    double w = cssLength(v, 0, 16);
    rgba = cssColour(v);
    if (!rgba) rgba = 0xff000000;
    return w < 0 ? 3 : w;   // (a style without a width: medium)
}
std::string lowerName(const std::string &tag) {
    size_t e = 0;
    while (e < tag.size() && !isspace((unsigned char)tag[e]) && tag[e] != '/' && tag[e] != '>') e++;
    std::string n = tag.substr(0, e);
    for (char &c : n) c = (char)tolower((unsigned char)c);
    return n;
}
// whether HTML has blocks of its own: a table, or a <div> with a background
bool hasTable(const std::string &v) {
    for (size_t at = 0; (at = v.find('<', at)) != std::string::npos; at++) {
        if (at + 6 <= v.size() && lowerName(v.substr(at + 1, 6)) == "table") return true;
        if (at + 4 <= v.size() && lowerName(v.substr(at + 1, 4)) == "div") {
            size_t e = v.find('>', at);
            if (e != std::string::npos && v.find("background", at) < e && tagBackground(v.substr(at + 1, e - at - 1))) return true;
        }
    }
    return false;
}


struct Flow {   // HTML laid out as blocks: its text and boxes, its size
    std::vector<Run> runs;
    std::vector<Label::Rect> rects;
    float w = 0, h = 0;
};
Flow flowHtml(const std::string &v, const Style &s, float x, float y, float avail, float availH, bool wrap, int depth);

// a <table> (from its tag to its end) at x, y: avail, availH the room its percentages are of (0: none)
Flow flowTable(const std::string &t, const Style &s, float x, float y, float avail, float availH, int depth) {
    Flow out;
    struct Cell {
        std::string html, tag;
        size_t row;
        int col = 0, span = 1, rspan = 1;   // (its columns and rows)
        bool th = false;
    };
    std::string tableTag;
    std::vector<std::string> rowTags;
    std::vector<Cell> cells;
    size_t open = std::string::npos;   // (the open cell's content start)
    int level = 0;
    auto closeCell = [&](size_t at) {
        if (open != std::string::npos && !cells.empty()) cells.back().html = t.substr(open, at - open);
        open = std::string::npos;
    };
    for (size_t i = 0; (i = t.find('<', i)) != std::string::npos && cells.size() < 4096;) {
        size_t e = t.find('>', i);
        if (e == std::string::npos) break;
        std::string tag = t.substr(i + 1, e - i - 1);
        bool close = !tag.empty() && tag[0] == '/';
        if (close) tag.erase(0, 1);
        std::string name = lowerName(tag);
        size_t at = i;
        i = e + 1;
        if (name == "table") {
            if (!close && ++level == 1) tableTag = tag;
            if (close && --level == 0) {
                closeCell(at);
                break;
            }
            continue;
        }
        if (level != 1) continue;
        if (name == "tr" || name == "td" || name == "th") closeCell(at);
        if (close) continue;
        if (name == "tr") rowTags.push_back(tag);
        else if (name == "td" || name == "th") {
            if (rowTags.empty()) rowTags.push_back("");
            Cell c;
            c.tag = tag;
            c.row = rowTags.size() - 1;
            c.th = name == "th";
            c.span = std::clamp(atoi(htmlAttr(tag, "colspan").c_str()), 1, 64);
            c.rspan = std::clamp(atoi(htmlAttr(tag, "rowspan").c_str()), 1, 64);
            cells.push_back(c);
            open = i;
        }
    }
    if (cells.empty()) return out;
    size_t rows = rowTags.size();
    float em = (float)num(s, "fontSize", 12);
    if (float fs = tagSize(tableTag, em)) em = fs;
    // the table's own measures
    int border = std::clamp(atoi(htmlAttr(tableTag, "border").c_str()), 0, 100);
    double pad = cssLength(htmlAttr(tableTag, "cellpadding"), 0, em);
    if (pad < 0) pad = 1;
    bool collapse = cssValue(tableTag, "border-collapse") == "collapse";
    double spacing = cssLength(htmlAttr(tableTag, "cellspacing"), 0, em);
    if (spacing < 0) spacing = 2;
    if (collapse) spacing = 0;
    std::string wv = cssValue(tableTag, "width"), hv = cssValue(tableTag, "height");
    if (wv.empty()) wv = htmlAttr(tableTag, "width");
    if (hv.empty()) hv = htmlAttr(tableTag, "height");
    double tw = cssLength(wv, avail, em), th = cssLength(hv, availH, em);
    // the cells' columns, styles and natural widths
    // (a row's cells in the columns its rows above leave free: a rowspan holds its columns in the rows below)
    std::vector<std::vector<bool>> taken(rows);
    auto freeCol = [&](size_t r, int from) {
        while ((size_t)from < taken[r].size() && taken[r][(size_t)from]) from++;
        return from;
    };
    std::vector<int> next(rows, 0);
    int cols = 0;
    struct Look {
        Style style;
        double pad, bw;
        uint32_t bg, bc;
        std::string valign;
        double fixed;   // (a width the cell asks for; -1: none)
    };
    std::vector<Look> looks(cells.size());
    for (size_t k = 0; k < cells.size(); k++) {
        Cell &c = cells[k];
        c.col = freeCol(c.row, next[c.row]);
        c.rspan = (int)std::min<size_t>((size_t)c.rspan, rows - c.row);
        for (size_t r = c.row; r < c.row + (size_t)c.rspan; r++) {
            if (taken[r].size() < (size_t)(c.col + c.span)) taken[r].resize((size_t)(c.col + c.span), false);
            for (int q = c.col; q < c.col + c.span; q++) taken[r][(size_t)q] = true;
        }
        next[c.row] = c.col + c.span;
        cols = std::max(cols, next[c.row]);
        const std::string &tr = rowTags[c.row];
        Look &lk = looks[k];
        lk.style = s;
        Style &cs = lk.style;
        uint32_t col = tagColour(c.tag);
        if (!col) col = tagColour(tr);
        if (!col) col = tagColour(tableTag);
        if (col) {
            char hex[16];
            snprintf(hex, sizeof hex, "#%06x", col & 0xffffff);
            cs["fontColor"] = hex;
        }
        std::string al = cssValue(c.tag, "text-align");
        if (al.empty()) al = htmlAttr(c.tag, "align");
        if (al.empty()) al = cssValue(tr, "text-align");
        if (al.empty()) al = htmlAttr(tr, "align");
        if (al == "left" || al == "center" || al == "right") cs["align"] = al;
        cs["verticalAlign"] = "top";
        if (c.th) cs["fontStyle"] = std::to_string((int)num(s, "fontStyle", 0) | 1);
        float fs = tagSize(c.tag, em);
        if (!fs) fs = tagSize(tr, em);
        cs["fontSize"] = std::to_string(fs ? fs : em);
        lk.valign = cssValue(c.tag, "vertical-align");
        if (lk.valign.empty()) lk.valign = htmlAttr(c.tag, "valign");
        if (lk.valign.empty()) lk.valign = cssValue(tr, "vertical-align");
        if (lk.valign.empty()) lk.valign = htmlAttr(tr, "valign");
        double p = cssLength(cssValue(c.tag, "padding"), 0, em);
        lk.pad = p >= 0 ? p : pad;
        lk.bc = 0xff808080;
        lk.bw = tagBorder(c.tag, lk.bc);
        if (lk.bw < 0 && collapse) lk.bw = tagBorder(tr, lk.bc);
        if (lk.bw < 0) lk.bw = border > 0 ? 1 : 0;
        lk.bg = tagBackground(c.tag);
        if (!lk.bg) lk.bg = tagBackground(tr);
        std::string cw = cssValue(c.tag, "width");
        if (cw.empty()) cw = htmlAttr(c.tag, "width");
        lk.fixed = cssLength(cw, tw > 0 ? tw : 0, em);
    }
    if (cols <= 0) return out;
    std::vector<double> colW(cols, 0);
    std::vector<bool> fixedCol(cols, false);
    std::vector<double> nat(cells.size());
    for (size_t k = 0; k < cells.size(); k++) {   // (single columns first, then the spans: what they lack shared out)
        const Look &lk = looks[k];
        nat[k] = flowHtml(cells[k].html, lk.style, 0, 0, 0, 0, false, depth + 1).w + 2 * lk.pad + lk.bw;
        if (lk.fixed >= 0) nat[k] = std::max(nat[k], lk.fixed);
    }
    for (size_t k = 0; k < cells.size(); k++)
        if (cells[k].span == 1) {
            colW[cells[k].col] = std::max(colW[cells[k].col], nat[k]);
            if (looks[k].fixed >= 0) fixedCol[cells[k].col] = true;
        }
    for (size_t k = 0; k < cells.size(); k++)
        if (cells[k].span > 1) {
            int c0 = cells[k].col, c1 = std::min(cols, c0 + cells[k].span);
            double have = spacing * (c1 - c0 - 1);
            for (int c = c0; c < c1; c++) have += colW[c];
            if (have < nat[k])
                for (int c = c0; c < c1; c++) colW[c] += (nat[k] - have) / (c1 - c0);
        }
    double sum = 0;
    for (double w : colW) sum += w;
    double room = spacing * (cols + 1);
    if (tw < 0) tw = avail > 0 ? std::min(sum + room, (double)avail) : sum + room;
    double content = std::max(0.0, tw - room);
    if (content > sum) {   // (the room left: to the columns of no width of their own, by their widths)
        double autoSum = 0;
        int autos = 0;
        for (int c = 0; c < cols; c++)
            if (!fixedCol[c]) autoSum += colW[c], autos++;
        double extra = content - sum;
        for (int c = 0; c < cols; c++) {
            if (autos == 0) colW[c] += sum > 0 ? extra * colW[c] / sum : extra / cols;
            else if (!fixedCol[c]) colW[c] += autoSum > 0 ? extra * colW[c] / autoSum : extra / autos;
        }
    } else if (sum > 0)
        for (double &w : colW) w *= content / sum;
    std::vector<double> colX(cols + 1, spacing);
    for (int c = 0; c < cols; c++) colX[c + 1] = colX[c] + colW[c] + spacing;
    // the rows' heights: the cells' text wrapped to their widths
    std::vector<double> rowH(rows, 0), cellW(cells.size()), textH(cells.size());
    for (size_t k = 0; k < cells.size(); k++) {
        const Look &lk = looks[k];
        int c1 = std::min(cols, cells[k].col + cells[k].span);
        cellW[k] = colX[c1] - spacing - colX[cells[k].col];
        double inner = std::max(0.0, cellW[k] - 2 * lk.pad - lk.bw);
        textH[k] = flowHtml(cells[k].html, lk.style, 0, 0, (float)inner, 0, true, depth + 1).h;
        if (textH[k] <= 0) {   // (a <br> or a space: a line of its own)
            bool line = false;
            for (char ch : cells[k].html) line = line || (ch != ' ' && ch != '\n' && ch != '\t' && ch != '\r');
            if (line) textH[k] = num(lk.style, "fontSize", 12) * 1.2;
        }
        if (cells[k].rspan == 1) rowH[cells[k].row] = std::max(rowH[cells[k].row], textH[k] + 2 * lk.pad + lk.bw);
    }
    for (size_t k = 0; k < cells.size(); k++)   // (a cell over several rows: what they lack to the last of them)
        if (cells[k].rspan > 1) {
            size_t r0 = cells[k].row, r1 = r0 + (size_t)cells[k].rspan - 1;
            double have = spacing * (double)(r1 - r0), need = textH[k] + 2 * looks[k].pad + looks[k].bw;
            for (size_t r = r0; r <= r1; r++) have += rowH[r];
            if (have < need) rowH[r1] += need - have;
        }
    double hsum = 0;
    for (double h : rowH) hsum += h;
    double vroom = spacing * ((double)rows + 1);
    if (th > hsum + vroom) {   // (a taller table: its rows by their heights)
        double extra = th - hsum - vroom;
        for (double &h : rowH) h += hsum > 0 ? extra * h / hsum : extra / (double)rows;
    }
    std::vector<double> rowY(rows + 1, spacing);
    for (size_t r = 0; r < rows; r++) rowY[r + 1] = rowY[r] + rowH[r] + spacing;
    out.w = (float)colX[cols], out.h = (float)rowY[rows];
    // drawn: the table's background, then a cell's background, text and border
    if (uint32_t bg = tagBackground(tableTag)) out.rects.push_back({x, y, out.w, out.h, bg});
    for (size_t k = 0; k < cells.size(); k++) {
        const Look &lk = looks[k];
        float cx = x + (float)colX[cells[k].col], cy = y + (float)rowY[cells[k].row], cw = (float)cellW[k], ch = (float)(rowY[cells[k].row + (size_t)cells[k].rspan] - spacing - rowY[cells[k].row]);
        if (lk.bg) out.rects.push_back({cx, cy, cw, ch, lk.bg});
        double inner = std::max(0.0, cellW[k] - 2 * lk.pad - lk.bw);
        double free = ch - 2 * lk.pad - lk.bw - textH[k];
        double dy = lk.valign == "top" ? 0 : lk.valign == "bottom" ? free : free / 2;
        Flow f = flowHtml(cells[k].html, lk.style, cx + (float)(lk.pad + lk.bw / 2), cy + (float)(lk.pad + lk.bw / 2 + dy), (float)inner, 0,
                          true, depth + 1);
        out.rects.insert(out.rects.end(), f.rects.begin(), f.rects.end());
        out.runs.insert(out.runs.end(), f.runs.begin(), f.runs.end());
        if (lk.bw > 0) {
            float b = (float)lk.bw;
            out.rects.push_back({cx, cy, cw, b, lk.bc});
            out.rects.push_back({cx, cy + ch - b, cw, b, lk.bc});
            out.rects.push_back({cx, cy, b, ch, lk.bc});
            out.rects.push_back({cx + cw - b, cy, b, ch, lk.bc});
        }
    }
    if (border > 0 && !collapse) {   // (the table's own frame)
        float b = (float)border;
        uint32_t bc = 0xff808080;
        out.rects.push_back({x, y, out.w, b, bc});
        out.rects.push_back({x, y + out.h - b, out.w, b, bc});
        out.rects.push_back({x, y, b, out.h, bc});
        out.rects.push_back({x + out.w - b, y, b, out.h, bc});
    }
    return out;
}

// HTML as a browser stacks it: its tables and its <div>s with a background as blocks of their own, the text between
// them as the label's lines; x, y its top left, avail the width it has (0: as wide as it needs)
// (the blocks a label's HTML may still lay out: a table's cells are each laid out three times, in each of the measuring and
// the placing pass, so nested tables multiply; past this the rest of the label is left out)
static thread_local size_t flowBudget = 0;
Flow flowHtml(const std::string &v, const Style &s, float x, float y, float avail, float availH, bool wrap, int depth) {
    Flow out;
    if (depth > 8 || flowBudget == 0) return out;
    flowBudget--;
    struct Block {
        int kind;   // (0 text, 1 table, 2 a <div> with a background)
        std::string html, tag;
    };
    std::vector<Block> blocks;
    size_t textFrom = 0;
    auto text = [&](size_t to) {
        std::string t = v.substr(textFrom, to - textFrom);
        bool blank = true;
        for (char ch : t) blank = blank && (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r');
        if (!blank) blocks.push_back({0, t, ""});
    };
    int divs = 0;   // (the <div>s open in the text: a block is one at the top only)
    for (size_t i = 0; (i = v.find('<', i)) != std::string::npos;) {
        size_t e = v.find('>', i);
        if (e == std::string::npos) break;
        std::string tag = v.substr(i + 1, e - i - 1);
        bool close = !tag.empty() && tag[0] == '/';
        std::string name = lowerName(close ? tag.substr(1) : tag);
        if (name == "div") divs += close ? -1 : 1;
        bool table = !close && name == "table", box = !close && name == "div" && divs == 1 && tagBackground(tag);
        if (!table && !box) {
            i = e + 1;
            continue;
        }
        size_t end = v.size(), inner = v.size(), j = e + 1;   // (its end: the matching close tag; inner: where that starts)
        int level = 1;
        while ((j = v.find('<', j)) != std::string::npos) {
            size_t k = v.find('>', j);
            if (k == std::string::npos) break;
            bool c2 = j + 1 < v.size() && v[j + 1] == '/';
            std::string n2 = lowerName(v.substr(j + 1 + c2, k - j - 1 - c2));
            if (n2 == name) level += c2 ? -1 : 1;
            if (level == 0) {
                inner = j, end = k + 1;
                break;
            }
            j = k + 1;
        }
        text(i);
        if (table) blocks.push_back({1, v.substr(i, end - i), ""});
        else blocks.push_back({2, v.substr(e + 1, inner - e - 1), tag}), divs--;
        textFrom = i = end;
    }
    text(v.size());
    // (a <div>'s own colour, font size and text-align for what is in it)
    auto divStyle = [&](const std::string &tag) {
        Style d = s;
        if (uint32_t c = tagColour(tag)) {
            char hex[16];
            snprintf(hex, sizeof hex, "#%06x", c & 0xffffff);
            d["fontColor"] = hex;
        }
        if (float fs = tagSize(tag, (float)num(s, "fontSize", 12))) d["fontSize"] = std::to_string(fs);
        std::string ta = cssValue(tag, "text-align");
        if (ta == "left" || ta == "center" || ta == "right") d["align"] = ta;
        return d;
    };
    // measured, then placed: the text's lines aligned in the widest block
    std::vector<Flow> laid(blocks.size());
    float wide = 0;
    for (size_t b = 0; b < blocks.size(); b++) {
        const Block &bk = blocks[b];
        if (bk.kind == 1) laid[b] = flowTable(bk.html, s, 0, 0, avail, availH, depth);
        else if (bk.kind == 2) {
            double p = std::max(0.0, cssLength(cssValue(bk.tag, "padding"), 0, num(s, "fontSize", 12)));
            double cw = cssLength(cssValue(bk.tag, "width"), avail, 16);   // (a width of its own: px, or % of the room)
            float in = cw > 0 ? std::max(0.f, (float)cw - 2 * (float)p) : avail > 0 ? std::max(0.f, avail - 2 * (float)p) : 0;
            laid[b] = flowHtml(bk.html, divStyle(bk.tag), 0, 0, in, 0, wrap || cw > 0, depth + 1);
            laid[b].w += 2 * (float)p, laid[b].h += 2 * (float)p;
            if (cw > 0) laid[b].w = (float)cw;
        } else {
            Style ts = s;
            ts["verticalAlign"] = "top";
            std::shared_ptr<Label> l = layoutLabel(bk.html, ts, Box{0, 0, 0, 0}, wrap && avail > 0 ? avail : 0, false);
            if (l) laid[b].w = l->x1 - l->x0 - 4, laid[b].h = l->y1 - l->y0;
        }
        wide = std::max(wide, laid[b].w);
    }
    float cy = y;
    for (size_t b = 0; b < blocks.size(); b++) {
        const Block &bk = blocks[b];
        Flow f;
        if (bk.kind == 1) f = flowTable(bk.html, s, x, cy, avail, availH, depth);
        else if (bk.kind == 2) {
            double p = std::max(0.0, cssLength(cssValue(bk.tag, "padding"), 0, num(s, "fontSize", 12)));
            float bw = cssLength(cssValue(bk.tag, "width"), avail, 16) > 0 ? laid[b].w : wide;   // (its own width, else the block's)
            out.rects.push_back({x, cy, bw, laid[b].h, tagBackground(bk.tag)});
            bool own = cssLength(cssValue(bk.tag, "width"), avail, 16) > 0;
            f = flowHtml(bk.html, divStyle(bk.tag), x + (float)p, cy + (float)p, std::max(0.f, bw - 2 * (float)p), 0, wrap || own, depth + 1);
            f.h = laid[b].h;
        } else {
            Style ts = s;
            ts["verticalAlign"] = "top";
            std::shared_ptr<Label> l = layoutLabel(bk.html, ts, Box{x, cy, wide, 0}, wrap && avail > 0 ? avail : 0, false);
            if (l) f.runs = l->runs, f.rects = l->rects;   // (its text and its highlights / rules)
            f.h = laid[b].h;
        }
        out.rects.insert(out.rects.end(), f.rects.begin(), f.rects.end());
        out.runs.insert(out.runs.end(), f.runs.begin(), f.runs.end());
        cy += f.h;
    }
    out.w = wide, out.h = cy - y;
    return out;
}

// a label with HTML tables: its blocks placed in the box by its align / verticalAlign (overflow=fill: the box's room)
std::shared_ptr<Label> layoutHtmlBlocks(const std::string &value, const Style &s, Box box, double wrapWidth) {
    bool fill = str(s, "overflow") == "fill";
    float avail = (float)(wrapWidth > 0 ? wrapWidth : fill ? box.w : 0), availH = (float)(fill ? box.h : 0);
    flowBudget = 100000;   // (each pass its own: the measuring one cannot use up the placing one's)
    Flow m = flowHtml(value, s, 0, 0, avail, availH, wrapWidth > 0 || fill, 0);
    const std::string &va = str(s, "verticalAlign"), &al = str(s, "align");
    float left = (float)(al == "left" ? box.x : al == "right" ? box.x + box.w - m.w : box.x + (box.w - m.w) / 2);
    float top = (float)(va == "top" ? box.y : va == "bottom" ? box.y + box.h - m.h : box.y + (box.h - m.h) / 2);
    if (fill) left = (float)box.x, top = (float)box.y;
    flowBudget = 100000;
    Flow f = flowHtml(value, s, left, top, avail, availH, wrapWidth > 0 || fill, 0);
    if (f.runs.empty() && f.rects.empty()) return nullptr;
    auto label = std::make_shared<Label>();
    label->runs = std::move(f.runs);
    label->rects = std::move(f.rects);
    label->x0 = left, label->x1 = left + f.w, label->y0 = top, label->y1 = top + f.h;
    label->background = labelBackground(s);
    return label;
}

// --- the hand-drawn look (sketch=1, draw.io's rough.js; its own randomness, so alike, not the same) ----------

// a path's outline drawn twice, each line a little off and bowed (the seed: the path's own numbers, the same each time)
PathP roughPath(const plutovg_path_t *path) {
    PathP out = newPath();
    std::unique_ptr<plutovg_path_t, void (*)(plutovg_path_t *)> flat(plutovg_path_clone_flatten(path), plutovg_path_destroy);
    if (!flat) return out;
    std::vector<std::vector<Pt>> subs;
    plutovg_path_iterator_t it;
    plutovg_path_iterator_init(&it, flat.get());
    plutovg_point_t pts[3];
    uint32_t seed = 2463534242u;
    while (plutovg_path_iterator_has_next(&it)) {
        switch (plutovg_path_iterator_next(&it, pts)) {
        case PLUTOVG_PATH_COMMAND_MOVE_TO: subs.push_back({{pts[0].x, pts[0].y}}); break;
        case PLUTOVG_PATH_COMMAND_LINE_TO:
        case PLUTOVG_PATH_COMMAND_CLOSE:
            if (subs.empty()) subs.push_back({});
            subs.back().push_back({pts[0].x, pts[0].y});
            break;
        default: break;
        }
        uint32_t bits;
        memcpy(&bits, &pts[0].x, 4), seed = seed * 31 + bits;
    }
    auto rnd = [&seed]() {   // (-1 .. 1)
        seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
        return (double)(seed % 20001) / 10000 - 1;
    };
    size_t segments = 0;
    for (const auto &sp : subs) {
        if (sp.size() < 2) continue;
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 1; i < sp.size() && segments < 200000; i++, segments++) {
                Pt a = sp[i - 1], b = sp[i];
                double len = std::hypot(b.x - a.x, b.y - a.y);
                if (len < 0.01) continue;
                double r = std::min(1.5, len * 0.08), nx = -(b.y - a.y) / len, ny = (b.x - a.x) / len, bow = rnd() * r;
                Pt a2{a.x + rnd() * r, a.y + rnd() * r}, b2{b.x + rnd() * r, b.y + rnd() * r};
                Pt m{(a2.x + b2.x) / 2 + nx * bow, (a2.y + b2.y) / 2 + ny * bow};
                plutovg_path_move_to(out.get(), (float)a2.x, (float)a2.y);
                plutovg_path_quad_to(out.get(), (float)m.x, (float)m.y, (float)b2.x, (float)b2.y);
            }
        }
    }
    return out;
}

// --- shapes --------------------------------------------------------------------

// a closed polygon in b (its points relative); arc > 0: its corners rounded as mxShape.addPoints rounds them (a quadratic
// through each corner, from arc before it to arc after it, at most half a side)
void polygon(plutovg_path_t *p, std::initializer_list<Pt> list, const Box &b, double arc = 0) {
    std::vector<Pt> pts(list);
    if (pts.empty()) return;
    auto at = [&](Pt q) { return plutovg_point_t{(float)(b.x + q.x), (float)(b.y + q.y)}; };
    if (!(arc > 0) || pts.size() < 3) {
        bool first = true;
        for (Pt q : pts) {
            plutovg_point_t a = at(q);
            if (first) plutovg_path_move_to(p, a.x, a.y), first = false;
            else plutovg_path_line_to(p, a.x, a.y);
        }
        plutovg_path_close(p);
        return;
    }
    Pt last = pts.back();
    pts.insert(pts.begin(), Pt{last.x + (pts[0].x - last.x) / 2, last.y + (pts[0].y - last.y) / 2});   // (from mid-side)
    size_t n = pts.size();
    Pt cur = pts[0];
    plutovg_point_t a0 = at(cur);
    plutovg_path_move_to(p, a0.x, a0.y);
    for (size_t k = 1; k < n; k++) {
        Pt c = pts[k % n];
        double dx = cur.x - c.x, dy = cur.y - c.y;
        if (dx == 0 && dy == 0) continue;
        double d = std::sqrt(dx * dx + dy * dy);
        plutovg_point_t l = at({c.x + dx * std::min(arc, d / 2) / d, c.y + dy * std::min(arc, d / 2) / d});
        plutovg_path_line_to(p, l.x, l.y);
        Pt nx = pts[(k + 1) % n];
        while (k < n - 2 && std::round(nx.x - c.x) == 0 && std::round(nx.y - c.y) == 0) nx = pts[(k + 2) % n], k++;
        double ex = nx.x - c.x, ey = nx.y - c.y, e = std::max(1.0, std::sqrt(ex * ex + ey * ey));
        Pt to{c.x + ex * std::min(arc, e / 2) / e, c.y + ey * std::min(arc, e / 2) / e};
        plutovg_point_t cc = at(c), tt = at(to);
        plutovg_path_quad_to(p, cc.x, cc.y, tt.x, tt.y);
        cur = to;
    }
    plutovg_path_close(p);
}

// The outline of a vertex shape in its box (draw.io's built-in ones; the others: their frame).
// Second path: the lines drawn over it (a process' bars, a note's fold, a cylinder's rim)
// the shapes drawn turned by their direction as mxShape turns them: in the box turned upright (north / south: width
// and height exchanged), then turned about its centre (south 90, west 180, north 270 degrees)
bool turnsByDirection(const std::string &shape) {
    static const char *const turned[] = {"cube", "folder", "component", "message", "umlActor", "hexagon", "parallelogram", "trapezoid",
                                         "step", "card", "note", "process", "document", "cylinder", "cylinder3", "datastore", "cloud", "actor",
                                         "offPageConnector", "note2", "curlyBracket", "dimension", "crossbar", "or", "xor", "delay",
                                         "dataStorage", "tape", "singleArrow", "corner", "module", "lollipop", "mxgraph.arrows2.arrow",
                                         "mxgraph.infographic.ribbonSimple", "mxgraph.infographic.cylinder", "mxgraph.infographic.shadedCube"};
    for (const char *t : turned)
        if (shape == t) return true;
    return shape.compare(0, 18, "mxgraph.floorplan.") == 0 || shape.compare(0, 14, "mxgraph.basic.") == 0;
}

PathP shapePath(const std::string &shape, const Style &s, const Box &b, PathP &lines);
PathP turnedShapePath(const std::string &shape, const Style &s, const Box &b, PathP &lines) {
    const std::string &dir = str(s, "direction");
    double deg = dir == "south" ? 90 : dir == "west" ? 180 : dir == "north" ? 270 : 0;
    Style upright = s;
    upright.erase("direction");
    Pt c = b.centre();
    Box ub = deg == 90 || deg == 270 ? Box{c.x - b.h / 2, c.y - b.w / 2, b.h, b.w} : b;
    PathP l, p = shapePath(shape, upright, ub, l);
    plutovg_matrix_t m;
    plutovg_matrix_init_translate(&m, (float)c.x, (float)c.y);
    plutovg_matrix_rotate(&m, (float)(deg * M_PI / 180));
    plutovg_matrix_translate(&m, (float)-c.x, (float)-c.y);
    PathP tp = newPath();
    plutovg_path_add_path(tp.get(), p.get(), &m);
    if (l) {
        lines = newPath();
        plutovg_path_add_path(lines.get(), l.get(), &m);
    }
    return tp;
}

PathP shapePath(const std::string &shape, const Style &s, const Box &b, PathP &lines) {
    {
        const std::string &dir = str(s, "direction");
        if ((dir == "south" || dir == "west" || dir == "north") && turnsByDirection(shape)) return turnedShapePath(shape, s, b, lines);
    }
    PathP p = newPath();
    plutovg_path_t *q = p.get();
    double w = b.w, h = b.h;
    float x = (float)b.x, y = (float)b.y;
    auto line = [&](double ax, double ay, double bx, double by) {
        if (!lines) lines = newPath();
        plutovg_path_move_to(lines.get(), (float)(b.x + ax), (float)(b.y + ay));
        plutovg_path_line_to(lines.get(), (float)(b.x + bx), (float)(b.y + by));
    };
    const std::string &dir = str(s, "direction");
    const double arc = str(s, "rounded") == "1" ? std::max(0.0, num(s, "arcSize", 20)) / 2 : 0;   // (a roundable polygon's corners)
    if (shape == "ellipse" || shape == "doubleEllipse") {
        plutovg_path_add_ellipse(q, (float)(b.x + w / 2), (float)(b.y + h / 2), (float)(w / 2), (float)(h / 2));
        if (shape == "doubleEllipse") {
            if (!lines) lines = newPath();
            double in = std::min(w, h) * 0.1;
            plutovg_path_add_ellipse(lines.get(), (float)(b.x + w / 2), (float)(b.y + h / 2), (float)(w / 2 - in), (float)(h / 2 - in));
        }
    } else if (shape == "rhombus") {
        polygon(q, {{w / 2, 0}, {w, h / 2}, {w / 2, h}, {0, h / 2}}, b, arc);
    } else if (shape == "triangle") {
        if (dir == "north") polygon(q, {{0, h}, {w / 2, 0}, {w, h}}, b, arc);
        else if (dir == "south") polygon(q, {{0, 0}, {w, 0}, {w / 2, h}}, b, arc);
        else if (dir == "west") polygon(q, {{w, 0}, {w, h}, {0, h / 2}}, b, arc);
        else polygon(q, {{0, 0}, {w, h / 2}, {0, h}}, b, arc);
    } else if (shape == "hexagon") {
        double d = std::min(w * num(s, "size", 0.25), w / 2);
        if (str(s, "fixedSize") == "1") d = std::min(num(s, "size", 20), w / 2);
        polygon(q, {{d, 0}, {w - d, 0}, {w, h / 2}, {w - d, h}, {d, h}, {0, h / 2}}, b, arc);
    } else if (shape == "parallelogram" || shape == "trapezoid" || shape == "step") {
        double d = str(s, "fixedSize") == "1" ? num(s, "size", 20) : w * num(s, "size", 0.2);
        d = std::clamp(d, 0.0, w / 2);
        if (shape == "parallelogram") polygon(q, {{d, 0}, {w, 0}, {w - d, h}, {0, h}}, b, arc);
        else if (shape == "trapezoid") polygon(q, {{0, h}, {d, 0}, {w - d, 0}, {w, h}}, b, arc);
        else polygon(q, {{0, 0}, {w - d, 0}, {w, h / 2}, {w - d, h}, {0, h}, {d, h / 2}}, b, arc);
    } else if (shape == "offPageConnector") {   // (Shapes.js OffPageConnectorShape: a box with a point below, size of its height)
        double d = h * std::clamp(num(s, "size", 0.375), 0.0, 1.0);
        polygon(q, {{0, 0}, {w, 0}, {w, h - d}, {w / 2, h}, {0, h - d}}, b);
    } else if (shape == "card" || shape == "note" || shape == "note2") {
        double d = std::min({num(s, "size", shape == "card" ? 30 : 30), w, h});
        if (shape == "card") polygon(q, {{d, 0}, {w, 0}, {w, h}, {0, h}, {0, d}}, b);
        else {
            polygon(q, {{0, 0}, {w - d, 0}, {w, d}, {w, h}, {0, h}}, b);
            line(w - d, 0, w - d, d);
            line(w - d, d, w, d);
        }
    } else if (shape == "process") {
        plutovg_path_add_rect(q, x, y, (float)w, (float)h);
        double d = w * num(s, "size", 0.1);
        line(d, 0, d, h);
        line(w - d, 0, w - d, h);
    } else if (shape == "document") {
        double dy = h * num(s, "size", 0.3), fy = 1.4;
        plutovg_path_move_to(q, x, y);
        plutovg_path_line_to(q, (float)(x + w), y);
        plutovg_path_line_to(q, (float)(x + w), (float)(y + h - dy / 2));
        plutovg_path_quad_to(q, (float)(x + w * 3 / 4), (float)(y + h - dy * fy), (float)(x + w / 2), (float)(y + h - dy / 2));
        plutovg_path_quad_to(q, (float)(x + w / 4), (float)(y + h - dy * (1 - fy)), x, (float)(y + h - dy / 2));
        plutovg_path_close(q);
    } else if (shape == "cylinder" || shape == "cylinder3" || shape == "datastore") {
        double dy = shape == "cylinder3" ? std::min(num(s, "size", 15), h / 2) : std::min(40.0, std::floor(h / 5));
        float rx = (float)(w / 2), ry = (float)dy;
        plutovg_path_move_to(q, x, (float)(y + dy));
        plutovg_path_arc_to(q, rx, ry, 0, false, true, (float)(x + w), (float)(y + dy));
        plutovg_path_line_to(q, (float)(x + w), (float)(y + h - dy));
        plutovg_path_arc_to(q, rx, ry, 0, false, true, x, (float)(y + h - dy));
        plutovg_path_close(q);
        if (!lines) lines = newPath();
        plutovg_path_move_to(lines.get(), x, (float)(y + dy));
        plutovg_path_arc_to(lines.get(), rx, ry, 0, false, false, (float)(x + w), (float)(y + dy));
    } else if (shape == "cloud") {
        auto P = [&](double fx, double fy) { return Pt{b.x + fx * w, b.y + fy * h}; };
        auto C = [&](Pt a, Pt c, Pt d) { plutovg_path_cubic_to(q, (float)a.x, (float)a.y, (float)c.x, (float)c.y, (float)d.x, (float)d.y); };
        Pt s0 = P(0.25, 0.25);
        plutovg_path_move_to(q, (float)s0.x, (float)s0.y);
        C(P(0.05, 0.25), P(0, 0.5), P(0.16, 0.55));
        C(P(0, 0.66), P(0.18, 0.9), P(0.31, 0.8));
        C(P(0.4, 1), P(0.7, 1), P(0.8, 0.8));
        C(P(1, 0.8), P(1, 0.6), P(0.875, 0.5));
        C(P(1, 0.3), P(0.8, 0.1), P(0.625, 0.2));
        C(P(0.5, 0.05), P(0.3, 0.05), P(0.25, 0.25));
        plutovg_path_close(q);
    } else if (shape == "actor") {
        auto C = [&](double a, double c, double d, double e, double f, double g) {
            plutovg_path_cubic_to(q, (float)(b.x + a), (float)(b.y + c), (float)(b.x + d), (float)(b.y + e), (float)(b.x + f), (float)(b.y + g));
        };
        plutovg_path_move_to(q, x, (float)(y + h));
        C(0, 3 * h / 5, 0, 2 * h / 5, w / 2, 2 * h / 5);
        C(w / 2 - w / 4, 2 * h / 5, w / 2 - w / 4, 0, w / 2, 0);
        C(w / 2 + w / 4, 0, w / 2 + w / 4, 2 * h / 5, w / 2, 2 * h / 5);
        C(w, 2 * h / 5, w, 3 * h / 5, w, h);
        plutovg_path_close(q);
    } else if (shape == "cube") {   // (Shapes.js CubeShape: the front, its top and side folded back by size)
        double d = std::clamp(num(s, "size", 20), 0.0, std::min(w, h));
        polygon(q, {{0, 0}, {w - d, 0}, {w, d}, {w, h}, {d, h}, {0, h - d}}, b);
        line(d, h, d, d), line(d, d, 0, 0), line(d, d, w, d);
    } else if (shape == "folder") {   // (FolderShape: a tab over the box, on the right by default)
        double dx = std::clamp(num(s, "tabWidth", 60), 0.0, w), dy = std::clamp(num(s, "tabHeight", 20), 0.0, h);
        if (str(s, "tabPosition") == "left") polygon(q, {{0, dy}, {0, 0}, {dx, 0}, {dx, dy}, {w, dy}, {w, h}, {0, h}}, b);
        else polygon(q, {{0, dy}, {w - dx, dy}, {w - dx, 0}, {w, 0}, {w, h}, {0, h}}, b);
    } else if (shape == "component") {   // (ComponentShape: a box with two jetties on its left)
        double jw = num(s, "jettyWidth", 32), jh = num(s, "jettyHeight", 12), x0 = jw / 2, y0 = 0.3 * h - jh / 2, y1 = 0.7 * h - jh / 2;
        polygon(q, {{x0, 0}, {w, 0}, {w, h}, {x0, h}, {x0, y1 + jh}, {0, y1 + jh}, {0, y1}, {x0, y1}, {x0, y0 + jh}, {0, y0 + jh}, {0, y0}, {x0, y0}}, b);
        line(x0, y0, x0 + jw / 2, y0), line(x0 + jw / 2, y0, x0 + jw / 2, y0 + jh), line(x0 + jw / 2, y0 + jh, x0, y0 + jh);
        line(x0, y1, x0 + jw / 2, y1), line(x0 + jw / 2, y1, x0 + jw / 2, y1 + jh), line(x0 + jw / 2, y1 + jh, x0, y1 + jh);
    } else if (shape == "message") {   // (MessageShape: an envelope)
        plutovg_path_add_rect(q, x, y, (float)w, (float)h);
        line(0, 0, w / 2, h / 2), line(w / 2, h / 2, w, 0);
    } else if (shape == "seqActorStick") {   // (Shapes.js: Mermaid's stick figure, fixed size, at the top middle)
        double cx = w / 2, r = 15, t0 = 2 * r, t1 = t0 + 20, ay = t0 + 10;
        plutovg_path_add_circle(q, (float)(b.x + cx), (float)(b.y + r), (float)r);
        line(cx, t0, cx, t1), line(cx - 18, ay, cx + 18, ay), line(cx, t1, cx - 16, t1 + 15), line(cx, t1, cx + 16, t1 + 15);
    } else if (shape == "umlActor") {   // (UmlActorShape: the head filled, the body lines)
        plutovg_path_add_ellipse(q, (float)(b.x + w / 2), (float)(b.y + h / 8), (float)(w / 4), (float)(h / 8));
        line(w / 2, h / 4, w / 2, 2 * h / 3), line(w / 2, h / 3, 0, h / 3), line(w / 2, h / 3, w, h / 3);
        line(w / 2, 2 * h / 3, 0, h), line(w / 2, 2 * h / 3, w, h);
    } else if (shape == "mxgraph.basic.pie" || shape == "mxgraph.basic.partConcEllipse" || shape == "mxgraph.basic.arc") {
        // (mxBasic.js: angles 0..1 clockwise from the top; a pie's slice, a ring's part arcWidth wide, an open arc)
        double a0 = 2 * M_PI * std::clamp(num(s, "startAngle", 0.25), 0.0, 1.0), a1 = 2 * M_PI * std::clamp(num(s, "endAngle", 0.75), 0.0, 1.0);
        double rx = w / 2, ry = h / 2, diff = a1 - a0;
        if (diff < 0) diff += 2 * M_PI;
        if (diff == M_PI) a1 += 0.00001;
        bool big = diff >= M_PI;
        auto P = [&](double a, double fx, double fy) { return Pt{b.x + rx + std::sin(a) * fx, b.y + ry - std::cos(a) * fy}; };
        Pt st = P(a0, rx, ry), en = P(a1, rx, ry);
        if (shape == "mxgraph.basic.arc") {
            if (!lines) lines = newPath();
            plutovg_path_move_to(lines.get(), (float)st.x, (float)st.y);
            plutovg_path_arc_to(lines.get(), (float)rx, (float)ry, 0, big, true, (float)en.x, (float)en.y);
        } else if (shape == "mxgraph.basic.pie") {
            plutovg_path_move_to(q, (float)(b.x + rx), (float)(b.y + ry));
            plutovg_path_line_to(q, (float)st.x, (float)st.y);
            plutovg_path_arc_to(q, (float)rx, (float)ry, 0, big, true, (float)en.x, (float)en.y);
            plutovg_path_close(q);
        } else {
            double k = 1 - std::clamp(num(s, "arcWidth", 0.5), 0.0, 1.0);
            Pt ie = P(a1, rx * k, ry * k), is = P(a0, rx * k, ry * k);
            plutovg_path_move_to(q, (float)st.x, (float)st.y);
            plutovg_path_arc_to(q, (float)rx, (float)ry, 0, big, true, (float)en.x, (float)en.y);
            plutovg_path_line_to(q, (float)ie.x, (float)ie.y);
            plutovg_path_arc_to(q, (float)(rx * k), (float)(ry * k), 0, big, false, (float)is.x, (float)is.y);
            plutovg_path_close(q);
        }
    } else if (shape == "mxgraph.floorplan.wall") {   // (mxFloorplan.js: a wall of wallThickness across the middle)
        double t = num(s, "wallThickness", 10);
        plutovg_path_add_rect(q, x, (float)(b.y + h / 2 - t / 2), (float)w, (float)t);
    } else if (shape == "mxgraph.floorplan.wallCorner") {
        double t = num(s, "wallThickness", 10);
        polygon(q, {{0, h}, {0, 0}, {w, 0}, {w, t}, {t, t}, {t, h}}, b);
    } else if (shape == "mxgraph.floorplan.wallU") {
        double t = num(s, "wallThickness", 10);
        polygon(q, {{0, h}, {0, 0}, {w, 0}, {w, h}, {w - t, h}, {w - t, t}, {t, t}, {t, h}}, b);
    } else if (shape == "isoCube2") {   // (Shapes.js IsoCubeShape2: an upright isometric box, its top's edges and front edge)
        double ih = std::min(w * std::tan(std::clamp(num(s, "isoAngle", 15), 0.01, 94.0) * M_PI / 200), h * 0.5);
        polygon(q, {{w / 2, 0}, {w, ih}, {w, h - ih}, {w / 2, h}, {0, h - ih}, {0, ih}}, b);
        line(0, ih, w / 2, 2 * ih), line(w / 2, 2 * ih, w, ih), line(w / 2, 2 * ih, w / 2, h);
    } else if (shape == "isoRectangle" || shape == "isoCube") {   // (an isometric tile; a cube: its sides below it)
        const double t30 = std::tan(M_PI / 6), dx30 = (0.5 - t30) / 2;
        bool cube = shape == "isoCube";
        double m = cube ? std::min(w, h / (0.5 + t30)) : std::min(w, h / t30);
        Box o{b.x + (w - m) / 2, b.y + (h - m) / 2 + (cube ? 0 : m / 4), m, m};
        if (cube) {
            polygon(q, {{0, 0.25 * m}, {0.5 * m, m * dx30}, {m, 0.25 * m}, {m, 0.75 * m}, {0.5 * m, (1 - dx30) * m}, {0, 0.75 * m}}, o);
            auto L = [&](double ax, double ay, double bx, double by) { line(o.x - b.x + ax, o.y - b.y + ay, o.x - b.x + bx, o.y - b.y + by); };
            L(0, 0.25 * m, 0.5 * m, (0.5 - dx30) * m), L(0.5 * m, (0.5 - dx30) * m, m, 0.25 * m), L(0.5 * m, (0.5 - dx30) * m, 0.5 * m, (1 - dx30) * m);
        } else
            polygon(q, {{0, 0.25 * m}, {0.5 * m, m * dx30}, {m, 0.25 * m}, {0.5 * m, (0.5 - dx30) * m}}, o);
    } else if (shape == "mxgraph.gcp2.doubleRect") {   // (the front box; the one behind it: in vertex())
        plutovg_path_add_round_rect(q, x, y, (float)std::max(0.0, w - 8), (float)std::max(0.0, h - 8), 1, 1);
    } else if (shape == "mxgraph.infographic.ribbonSimple") {   // (a band notched at its tail, pointed at its head)
        double n1 = std::clamp(num(s, "notch1", 0.5), 0.0, w), n2 = std::clamp(num(s, "notch2", 0.5), 0.0, w);
        polygon(q, {{0, h}, {n1, h / 2}, {0, 0}, {w - n2, 0}, {w, h / 2}, {w - n2, h}}, b);
    } else if (shape == "mxgraph.infographic.shadedCube") {   // (an upright isometric cube; its faces' shades in vertex())
        double d = std::min(w * std::tan(std::clamp(num(s, "isoAngle", 15), 0.01, 94.0) * M_PI / 200), h / 2);
        polygon(q, {{w / 2, 0}, {w, d}, {w, h - d}, {w / 2, h}, {0, h - d}, {0, d}}, b);
    } else if (shape == "mxgraph.infographic.cylinder") {   // (a column with ellipses 10 high at its ends; its sheen in vertex())
        plutovg_path_move_to(q, x, (float)(b.y + 10));
        plutovg_path_arc_to(q, (float)(w / 2), 10, 0, false, true, (float)(b.x + w), (float)(b.y + 10));
        plutovg_path_line_to(q, (float)(b.x + w), (float)(b.y + h - 10));
        plutovg_path_arc_to(q, (float)(w / 2), 10, 0, false, true, x, (float)(b.y + h - 10));
        plutovg_path_close(q);
    } else if (shape == "mxgraph.arrows2.arrow") {   // (mxArrows2: a shaft dy of the height, a head dx long, a notch at its tail)
        double d = 0.5 * h * std::clamp(num(s, "dy", 0.5), 0.0, 1.0), e = std::clamp(num(s, "dx", 0.5), 0.0, w);
        double f = std::clamp(num(s, "notch", 0), 0.0, w);
        polygon(q, {{0, d}, {w - e, d}, {w - e, 0}, {w, h / 2}, {w - e, h}, {w - e, h - d}, {0, h - d}, {f, h / 2}}, b);
        if (str(s, "headCrossline") == "1") line(w - e, d, w - e, h - d);
        if (str(s, "tailCrossline") == "1") line(f, d, f, h - d);
    } else if (shape == "mxgraph.floorplan.window") {   // (mxFloorplan.js: a wall's band with a line along its middle)
        double th = num(s, "wallThickness", 10);
        polygon(q, {{0, h / 2 - th / 2}, {w, h / 2 - th / 2}, {w, h / 2 + th / 2}, {0, h / 2 + th / 2}}, b);
        line(0, h / 2, w, h / 2);
    } else if (shape == "mxgraph.floorplan.stairs") {   // (steps 25 apart, an arrow along the middle; at least 50 long)
        double mw = std::max(w, 50.0);
        polygon(q, {{0, 0}, {mw, 0}, {mw, h}, {0, h}}, b);
        for (double i = 25; i < mw && i < 25 * 10000; i += 25) line(i, 0, i, h);
        line(0, h / 2, mw, h / 2), line(mw - 25, 0, mw, h / 2), line(mw, h / 2, mw - 25, h);
    } else if (shape == "mxgraph.floorplan.doorLeft" || shape == "mxgraph.floorplan.doorRight" || shape == "mxgraph.floorplan.doorDouble") {
        polygon(q, {{0, 0}, {w, 0}, {w, 5}, {0, 5}}, b);   // (the frame 5 deep, the leaves' swing below it)
        if (!lines) lines = newPath();
        plutovg_path_t *l = lines.get();
        auto at = [&](double px, double py) { return plutovg_point_t{(float)(b.x + px), (float)(b.y + py)}; };
        auto swing = [&](double fx, double r, bool sweep, double tx) {   // (from the hinge's far side round to the wall, back up)
            plutovg_point_t p0 = at(fx, 5), p1 = at(tx, 5 + r), p2 = at(tx, 5);
            plutovg_path_move_to(l, p0.x, p0.y);
            plutovg_path_arc_to(l, (float)r, (float)r, 0, false, sweep, p1.x, p1.y);
            plutovg_path_line_to(l, p2.x, p2.y);
        };
        if (shape == "mxgraph.floorplan.doorLeft") swing(w, w, true, 0);
        else if (shape == "mxgraph.floorplan.doorRight") swing(0, w, false, w);
        else {
            line(w / 2, 0, w / 2, 5);
            swing(w / 2, w / 2, true, 0), swing(w / 2, w / 2, false, w);
        }
    } else if (shape == "mxgraph.floorplan.room") {   // (its walls: the frame less the inside)
        double t = num(s, "wallThickness", 10);
        polygon(q, {{0, h}, {0, 0}, {w, 0}, {w, h}}, b);
        polygon(q, {{t, t}, {t, h - t}, {w - t, h - t}, {w - t, t}}, b);
    } else if (shape == "endState" || shape == "startState") {   // (Shapes.js StateShape: a disc inset by up to 4; an end
        double in = std::min({4.0, w / 5, h / 5});                 // state's ring round it)
        plutovg_path_add_ellipse(q, (float)(b.x + w / 2), (float)(b.y + h / 2), (float)(w / 2 - in), (float)(h / 2 - in));
        if (shape == "endState") {
            if (!lines) lines = newPath();
            plutovg_path_add_ellipse(lines.get(), (float)(b.x + w / 2), (float)(b.y + h / 2), (float)(w / 2), (float)(h / 2));
        }
    } else if (shape == "curlyBracket") {   // (an open bracket, its point at size of the width)
        double d = w * std::clamp(num(s, "size", 0.5), 0.0, 1.0);
        line(w, 0, d, 0), line(d, 0, d, h), line(d, h / 2, 0, h / 2), line(d, h, w, h);
    } else if (shape == "dimension") {   // (DimensionShape: a measure line with arrowheads between two bars)
        double sw = num(s, "strokeWidth", 1) / 2, al = 10 + 2 * sw, cy = h - al / 2;
        line(0, 0, 0, h), line(w, 0, w, h), line(sw, cy, w - sw, cy);
        line(sw, cy, sw + al, cy - al / 2), line(sw, cy, sw + al, cy + al / 2);
        line(w - sw, cy, w - al - sw, cy - al / 2), line(w - sw, cy, w - al - sw, cy + al / 2);
    } else if (shape == "plus") {   // (PlusShape: a box with a cross in it)
        plutovg_path_add_rect(q, x, y, (float)w, (float)h);
        double bo = std::min(w / 5, h / 5) + 1;
        line(w / 2, bo, w / 2, h - bo), line(bo, h / 2, w - bo, h / 2);
    } else if (shape == "crossbar") {
        line(0, 0, 0, h), line(w, 0, w, h), line(0, h / 2, w, h / 2);
    } else if (shape == "umlDestroy") {
        line(w, 0, 0, h), line(0, 0, w, h);
    } else if (shape == "lollipop") {   // (a circle of size on a stem down the middle)
        double sz = num(s, "size", 10);
        plutovg_path_add_ellipse(q, (float)(b.x + w / 2), (float)(b.y + sz / 2), (float)(sz / 2), (float)(sz / 2));
        line(w / 2, sz, w / 2, h);
    } else if (shape == "smileyFace") {   // (its features in the stroke colour)
        double r = std::min(w, h) / 2, sc = std::min(w, h) / 30, cx = b.x + w / 2, cy = b.y + h / 2;
        plutovg_path_add_ellipse(q, (float)cx, (float)cy, (float)r, (float)r);
        if (!lines) lines = newPath();
        plutovg_path_t *l = lines.get();
        plutovg_path_add_ellipse(l, (float)(cx - 5 * sc), (float)(cy - 5 * sc), (float)(1.5 * sc), (float)(1.5 * sc));
        plutovg_path_add_ellipse(l, (float)(cx + 5 * sc), (float)(cy - 5 * sc), (float)(1.5 * sc), (float)(1.5 * sc));
        const std::string &type = str(s, "smileyType");
        if (type.empty() || type == "happy") {
            plutovg_path_move_to(l, (float)(cx + 7.5 * sc), (float)(cy + 2 * sc));
            plutovg_path_arc_to(l, (float)(7.5 * sc), (float)(7.5 * sc), 0, true, true, (float)(cx - 7.5 * sc), (float)(cy + 2 * sc));
        } else if (type == "sad") {
            plutovg_path_move_to(l, (float)(cx - 7.5 * sc), (float)(cy + 7 * sc));
            plutovg_path_arc_to(l, (float)(7.5 * sc), (float)(7.5 * sc), 0, true, true, (float)(cx + 7.5 * sc), (float)(cy + 7 * sc));
        } else {
            plutovg_path_move_to(l, (float)(cx - 5 * sc), (float)(cy + 7 * sc));
            plutovg_path_line_to(l, (float)(cx + 5 * sc), (float)(cy + 7 * sc));
        }
    } else if (shape == "or" || shape == "xor") {   // (OrShape / XorShape: a D of two curves, xor's back bowed in)
        auto Qd = [&](double cx, double cy, double ex, double ey) {
            plutovg_path_quad_to(q, (float)(b.x + cx), (float)(b.y + cy), (float)(b.x + ex), (float)(b.y + ey));
        };
        plutovg_path_move_to(q, x, y);
        Qd(w, 0, w, h / 2), Qd(w, h, 0, h);
        if (shape == "xor") Qd(w / 2, h / 2, 0, 0);
        plutovg_path_close(q);
    } else if (shape == "delay") {
        double dx = std::min(w, h / 2);
        plutovg_path_move_to(q, x, y);
        plutovg_path_line_to(q, (float)(b.x + w - dx), y);
        plutovg_path_quad_to(q, (float)(b.x + w), y, (float)(b.x + w), (float)(b.y + h / 2));
        plutovg_path_quad_to(q, (float)(b.x + w), (float)(b.y + h), (float)(b.x + w - dx), (float)(b.y + h));
        plutovg_path_line_to(q, x, (float)(b.y + h));
        plutovg_path_close(q);
    } else if (shape == "switch") {
        plutovg_path_move_to(q, x, y);
        plutovg_path_quad_to(q, (float)(b.x + w / 2), (float)(b.y + h / 2), (float)(b.x + w), y);
        plutovg_path_quad_to(q, (float)(b.x + w / 2), (float)(b.y + h / 2), (float)(b.x + w), (float)(b.y + h));
        plutovg_path_quad_to(q, (float)(b.x + w / 2), (float)(b.y + h / 2), x, (float)(b.y + h));
        plutovg_path_quad_to(q, (float)(b.x + w / 2), (float)(b.y + h / 2), x, y);
        plutovg_path_close(q);
    } else if (shape == "dataStorage") {   // (a box with a bowed left side and a hollow right one)
        bool fixed = str(s, "fixedSize") != "" && str(s, "fixedSize") != "0";
        double d = fixed ? std::clamp(num(s, "size", 20), 0.0, w) : w * std::clamp(num(s, "size", 0.1), 0.0, 1.0);
        plutovg_path_move_to(q, (float)(b.x + d), y);
        plutovg_path_line_to(q, (float)(b.x + w), y);
        plutovg_path_quad_to(q, (float)(b.x + w - 2 * d), (float)(b.y + h / 2), (float)(b.x + w), (float)(b.y + h));
        plutovg_path_line_to(q, (float)(b.x + d), (float)(b.y + h));
        plutovg_path_quad_to(q, (float)(b.x - d), (float)(b.y + h / 2), (float)(b.x + d), y);
        plutovg_path_close(q);
    } else if (shape == "tape") {   // (TapeShape: a band with wavy top and bottom)
        double dy = h * std::clamp(num(s, "size", 0.4), 0.0, 1.0), fy = 1.4;
        auto P = [&](double px, double py) { return plutovg_point_t{(float)(b.x + px), (float)(b.y + py)}; };
        plutovg_point_t p0 = P(0, dy / 2);
        plutovg_path_move_to(q, p0.x, p0.y);
        auto Qd = [&](double cx, double cy, double ex, double ey) {
            plutovg_point_t c = P(cx, cy), e = P(ex, ey);
            plutovg_path_quad_to(q, c.x, c.y, e.x, e.y);
        };
        Qd(w / 4, dy * fy, w / 2, dy / 2), Qd(w * 3 / 4, dy * (1 - fy), w, dy / 2);
        plutovg_path_line_to(q, (float)(b.x + w), (float)(b.y + h - dy / 2));
        Qd(w * 3 / 4, h - dy * fy, w / 2, h - dy / 2), Qd(w / 4, h - dy * (1 - fy), 0, h - dy / 2);
        plutovg_path_close(q);
    } else if (shape == "singleArrow") {   // (a shaft arrowWidth of the height, a head arrowSize of the width)
        double aw = h * std::clamp(num(s, "arrowWidth", 0.3), 0.0, 1.0), as = w * std::clamp(num(s, "arrowSize", 0.2), 0.0, 1.0);
        double at = (h - aw) / 2, ab = at + aw;
        polygon(q, {{0, at}, {w - as, at}, {w - as, 0}, {w, h / 2}, {w - as, h}, {w - as, ab}, {0, ab}}, b);
    } else if (shape == "corner") {   // (an L: dx wide down the left, dy high along the top)
        double dx = std::clamp(num(s, "dx", 20), 0.0, w), dy = std::clamp(num(s, "dy", 20), 0.0, h);
        polygon(q, {{0, 0}, {w, 0}, {w, dy}, {dx, dy}, {dx, h}, {0, h}}, b);
    } else if (shape == "module") {   // (ModuleShape: a box with two jetties on its left)
        double dx = num(s, "jettyWidth", 20), dy = num(s, "jettyHeight", 10), x0 = dx / 2, x1 = x0 + dx / 2;
        double y0 = std::min(dy, h - dy), y1 = std::min(y0 + 2 * dy, h - dy);
        polygon(q, {{x0, 0}, {w, 0}, {w, h}, {x0, h}, {x0, y1 + dy}, {0, y1 + dy}, {0, y1}, {x0, y1}, {x0, y0 + dy}, {0, y0 + dy}, {0, y0}, {x0, y0}}, b);
        line(x0, y0, x1, y0), line(x1, y0, x1, y0 + dy), line(x1, y0 + dy, x0, y0 + dy);
        line(x0, y1, x1, y1), line(x1, y1, x1, y1 + dy), line(x1, y1 + dy, x0, y1 + dy);
    } else if (shape == "line") {
        bool vertical = dir == "south" || dir == "north";
        if (vertical) line(w / 2, 0, w / 2, h);
        else line(0, h / 2, w, h / 2);
    } else {   // rectangle, label, text, image, the stencils' shapes: a rectangle (rounded)
        double r = 0;
        if (str(s, "rounded") == "1") {
            r = str(s, "absoluteArcSize") == "1" ? num(s, "arcSize", 10) / 2 : std::min(w, h) * num(s, "arcSize", 15) / 100;
            r = std::clamp(r, 0.0, std::min(w, h) / 2);
        }
        if (r > 0) plutovg_path_add_round_rect(q, x, y, (float)w, (float)h, (float)r, (float)r);
        else plutovg_path_add_rect(q, x, y, (float)w, (float)h);
    }
    return p;
}

// --- the diagram ---------------------------------------------------------------

struct Builder {
    Page &page;
    size_t &budget;   // (the items the file's pages may still have, all together)
    std::vector<Cell> cells;
    std::unordered_map<std::string, size_t> byId;

    Builder(Page &p, size_t &items) : page(p), budget(items) {}

    Cell *find(const std::string &id) {
        auto f = byId.find(id);
        return f == byId.end() ? nullptr : &cells[f->second];
    }
    bool layer(const Cell &c) { return !c.vertex && !c.edge; }
    // whether a cell is shown: it and its ancestors visible, none of them collapsed above it. The chain
    // up to a cell already known, then down it (no recursion: any depth; a cycle of parents ends at no parent)
    bool shown(Cell &c) {
        std::vector<Cell *> chain;
        Cell *p = &c;
        while (p && !p->shownState) {
            p->shownState = 3;
            chain.push_back(p);
            p = find(p->parent);
        }
        for (size_t i = chain.size(); i-- > 0;) {
            Cell *x = chain[i], *up = i + 1 < chain.size() ? chain[i + 1] : p && p->shownState != 3 ? p : nullptr;
            bool v = x->visible && (!up || (up->shownState == 1 && !(up->collapsed && up->vertex)));
            x->shownState = v ? 1 : 2;
        }
        return c.shownState == 1;
    }
    // a vertex's absolute box (its parents' origins added): the chain of parent vertices up to one already
    // worked out, then down it (as shown(): any depth, a cycle of parents ends at no parent)
    Box absBox(Cell &c) {
        if (c.state == 2) return c.box;
        std::vector<Cell *> chain;
        for (Cell *p = &c; p && !p->state;) {
            p->state = 1;
            chain.push_back(p);
            Cell *up = find(p->parent);
            p = up && up->vertex ? up : nullptr;
        }
        for (size_t i = chain.size(); i-- > 0;) {
            Cell *x = chain[i], *up = find(x->parent);
            bool have = up && up->vertex && up->state == 2;
            Box pb = have ? up->box : Box{};
            Pt po = have ? up->org : Pt{0, 0};
            if (x->relative && have) x->box = {pb.x + x->x * pb.w, pb.y + x->y * pb.h, x->w, x->h};
            else x->box = {po.x + x->x, po.y + x->y, x->w, x->h};
            if (x->hasOffset && x->relative) x->box.x += x->offset.x, x->box.y += x->offset.y;
            x->org = {x->box.x, x->box.y};
            if (x->transparent) x->box = {x->org.x + x->local.x, x->org.y + x->local.y, x->local.w, x->local.h};
            x->state = 2;
        }
        return c.state == 2 ? c.box : Box{c.x, c.y, c.w, c.h};
    }
    Pt origin(const std::string &parent) {   // where the children of a cell start
        Cell *p = find(parent);
        if (!p || !p->vertex) return {0, 0};
        absBox(*p);
        return p->state == 2 ? p->org : Pt{p->x, p->y};
    }
    // draw.io's transparentBounds containers (Graph.getTransparentBounds): the union of the children (vertices; an edge's
    // waypoints and loose ends), padded by groupPadding (CSS-like: 1 to 4 numbers) and a swimlane's title
    void transparentBounds() {
        std::unordered_map<std::string, std::vector<size_t>> kids;
        bool any = false;
        for (size_t i = 0; i < cells.size(); i++) {
            if (!cells[i].parent.empty()) kids[cells[i].parent].push_back(i);
            any = any || (cells[i].vertex && str(cells[i].style, "transparentBounds") == "1");
        }
        if (!any) return;
        std::function<void(Cell &, int)> bounds = [&](Cell &c, int depth) {
            if (c.localState) return;
            c.localState = 1;
            bool have = false;
            double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            auto take = [&](double ax, double ay, double bx, double by) {
                if (!have) x0 = ax, y0 = ay, x1 = bx, y1 = by, have = true;
                else x0 = std::min(x0, ax), y0 = std::min(y0, ay), x1 = std::max(x1, bx), y1 = std::max(y1, by);
            };
            auto f = kids.find(c.id);
            if (f != kids.end())
                for (size_t i : f->second) {
                    Cell &k = cells[i];
                    if (&k == &c) continue;
                    if (k.vertex && !k.relative) {
                        if (str(k.style, "transparentBounds") == "1" && depth < 64) bounds(k, depth + 1);
                        if (k.transparent) take(k.x + k.local.x, k.y + k.local.y, k.x + k.local.x + k.local.w, k.y + k.local.y + k.local.h);
                        else take(k.x, k.y, k.x + k.w, k.y + k.h);
                    } else if (k.edge) {
                        if (k.source.empty() && k.hasSource) take(k.sourcePoint.x, k.sourcePoint.y, k.sourcePoint.x, k.sourcePoint.y);
                        if (k.target.empty() && k.hasTarget) take(k.targetPoint.x, k.targetPoint.y, k.targetPoint.x, k.targetPoint.y);
                        for (Pt q : k.points) take(q.x, q.y, q.x, q.y);
                    }
                }
            if (!have) return;   // (no children: its own geometry)
            double pad[4] = {0, 0, 0, 0};   // (n e s w)
            {
                const std::string &v = str(c.style, "groupPadding");
                double n[4];
                int cnt = 0;
                const char *q = v.c_str();
                while (cnt < 4 && *q) {
                    char *e = nullptr;
                    double d = nn_strtod(q, &e);
                    if (e == q) break;
                    n[cnt++] = std::isfinite(d) && std::fabs(d) < 1e6 ? d : 0;
                    q = e;
                    while (*q == ' ' || *q == ',') q++;
                }
                if (cnt == 1) pad[0] = pad[1] = pad[2] = pad[3] = n[0];
                else if (cnt == 2) pad[0] = pad[2] = n[0], pad[1] = pad[3] = n[1];
                else if (cnt == 3) pad[0] = n[0], pad[1] = pad[3] = n[1], pad[2] = n[2];
                else if (cnt == 4) pad[0] = n[0], pad[1] = n[1], pad[2] = n[2], pad[3] = n[3];
            }
            double sx = 0, sy = 0;
            if (str(c.style, "shape") == "swimlane") {
                double ss = std::max(0.0, num(c.style, "startSize", 23));
                if (str(c.style, "horizontal") == "0") sx = ss;
                else sy = ss;
            }
            c.local = {x0 - pad[3] - sx, y0 - pad[0] - sy, x1 - x0 + pad[1] + pad[3] + sx, y1 - y0 + pad[0] + pad[2] + sy};
            c.transparent = true;
        };
        for (Cell &c : cells)
            if (c.vertex && str(c.style, "transparentBounds") == "1") bounds(c, 0);
    }

    void add(Item &&it, float margin) {
        const plutovg_path_element_t *elements;
        if (!it.label && (!it.path || !plutovg_path_get_elements(it.path.get(), &elements))) return;   // (an empty path: nothing drawn,
                                                                                                        // its box 0, 0 no part of the page)
        if (!it.label) {
            nd::Box b = nd::pathBox(it.path.get(), margin);
            it.bx0 = b.x0, it.by0 = b.y0, it.bx1 = b.x1, it.by1 = b.y1;
        }
        for (float v : {it.bx0, it.by0, it.bx1, it.by1})   // (NaN or far off: plutovg converts its coordinates to integers)
            if (!(std::fabs(v) < 1e8f)) return;
        if (!it.dash.empty()) {   // dashes too fine for the path's size: drawn solid (plutovg's dasher adds them up in a
                                  // float, which a dash under its precision no longer moves on: a loop without end)
            float smallest = 0, extent = std::max(it.bx1 - it.bx0, it.by1 - it.by0);
            for (float d : it.dash)
                if (d > 0 && (!smallest || d < smallest)) smallest = d;
            if (!(smallest >= extent * 1e-4f)) it.dash.clear();
        }
        if (it.path) {
            plutovg_path_iterator_t iter;
            plutovg_path_iterator_init(&iter, it.path.get());
            plutovg_point_t pts[3];
            while (plutovg_path_iterator_has_next(&iter)) {
                int n = 0;
                switch (plutovg_path_iterator_next(&iter, pts)) {
                case PLUTOVG_PATH_COMMAND_MOVE_TO:
                case PLUTOVG_PATH_COMMAND_LINE_TO: n = 1; break;
                case PLUTOVG_PATH_COMMAND_CUBIC_TO: n = 3; break;
                default: break;
                }
                for (int k = 0; k < n; k++)
                    if (!(std::fabs(pts[k].x) < 1e8f && std::fabs(pts[k].y) < 1e8f)) return;
            }
        }
        if (page.items.size() < (4u << 20) && budget) page.items.push_back(std::move(it)), budget--;
    }
    void addLabel(std::shared_ptr<Label> l) {
        if (!l) return;
        Item it;
        it.label = l;
        float x0 = l->x0, y0 = l->y0, x1 = l->x1, y1 = l->y1;
        if (l->clip) {   // (what shows of it)
            x0 = std::max(x0, l->clipX0), y0 = std::max(y0, l->clipY0), x1 = std::min(x1, l->clipX1), y1 = std::min(y1, l->clipY1);
            if (x1 <= x0 || y1 <= y0) return;
        }
        if (l->turned) {   // (the box of the turned one: its corners turned)
            nd::Box b = nd::turnedBox(l->cx, l->cy, x0, y0, x1, y1, l->angle);
            x0 = b.x0, y0 = b.y0, x1 = b.x1, y1 = b.y1;
        }
        it.bx0 = x0 - 2, it.by0 = y0 - 2, it.bx1 = x1 + 2, it.by1 = y1 + 2;
        add(std::move(it), 0);
    }

    // fill / stroke of a shape's path: its style's colours, opacity, gradient, dashes, shadow
    void paint(PathP path, PathP lines, const Style &s, const Box &b, bool edge) {
        double opacity = num(s, "opacity", 100);
        uint32_t fill = edge ? 0 : withOpacity(colour(s, "fillColor", 0xffffffff), opacity * num(s, "fillOpacity", 100) / 100);
        uint32_t stroke = withOpacity(colour(s, "strokeColor", 0xff000000), opacity * num(s, "strokeOpacity", 100) / 100);
        float width = (float)std::clamp(num(s, "strokeWidth", 1), 0.0, 1000.0);
        std::vector<float> dash;
        if (str(s, "dashed") == "1") {
            std::string pat = str(s, "dashPattern");
            if (pat.empty()) pat = "3 3";
            const char *c = pat.c_str();
            char *end;
            for (double d; (d = nn_strtod(c, &end)), end != c && dash.size() < 16; c = end)
                if (std::isfinite(d) && d >= 0) dash.push_back((float)(d * std::max(width, 1.f)));
            if (dash.size() % 2) {   // (an odd pattern twice: its own elements copied first, as insert takes no range of itself)
                std::vector<float> once = dash;
                dash.insert(dash.end(), once.begin(), once.end());
            }
            float total = 0;
            for (float d : dash) total += d;
            if (total <= 0) dash.clear();
        }
        if (str(s, "shadow") == "1") {
            Item sh;
            plutovg_matrix_t m;
            plutovg_matrix_init_translate(&m, 3, 3);
            sh.path = newPath();
            plutovg_path_add_path(sh.path.get(), path.get(), &m);
            uint32_t sc = withOpacity(0x40000000, opacity);   // (faded with the shape)
            if (fill >> 24) sh.fill = sc;
            else sh.stroke = sc, sh.width = width;
            if (sc >> 24) add(std::move(sh), width);
        }
        Item it;
        it.path = path;
        it.fill = fill;
        uint32_t g = edge ? 0 : colour(s, "gradientColor", 0);
        if (!edge && colour(s, "fillColor", 0xffffffff) && g) {   // (a fill faded to nothing: its gradient still has its other end)
            it.fill2 = withOpacity(g, opacity * num(s, "fillOpacity", 100) / 100);   // (both ends faded alike)
            const std::string &d = str(s, "gradientDirection");
            float x0 = (float)b.x, y0 = (float)b.y, x1 = (float)(b.x + b.w), y1 = (float)(b.y + b.h);
            if (d == "north") it.gx0 = x0, it.gy0 = y1, it.gx1 = x0, it.gy1 = y0;
            else if (d == "east") it.gx0 = x0, it.gy0 = y0, it.gx1 = x1, it.gy1 = y0;
            else if (d == "west") it.gx0 = x1, it.gy0 = y0, it.gx1 = x0, it.gy1 = y0;
            else it.gx0 = x0, it.gy0 = y0, it.gx1 = x0, it.gy1 = y1;
        }
        it.stroke = width > 0 ? stroke : 0;
        it.width = width;
        it.dash = dash;
        bool sketch = str(s, "sketch") == "1";
        const std::string &fs = str(s, "fillStyle");   // (auto, the default: solid when white or a gradient, else hachure)
        bool pattern = fs.empty() || fs == "auto" ? !(it.fill2 >> 24) && (it.fill & 0xffffff) != 0xffffff : fs != "solid";
        if (str(s, "sketchStyle") == "comic") pattern = false;   // (the comic style: wobbly lines, solid fills)
        // (a fill style of its own (hachure, cross-hatch ...) patterns the fill without sketch too: mxSvgCanvas2D.getFillPattern)
        if ((sketch || (!fs.empty() && fs != "auto" && fs != "solid")) && it.fill && pattern) {   // (rough.js: hachure lines,
                                                                                                 // 4 stroke widths apart, half as wide)
            it.crossHatch = fs == "cross-hatch";
            double gap = num(s, "hachureGap", -1), weight = num(s, "fillWeight", -1);
            it.hachure = (float)(gap > 0 ? gap : 4 * std::max(width, 1.f));
            it.hachureWeight = (float)(weight > 0 ? weight : std::max(width, 1.f) / 2);
            it.hachureAngle = (float)num(s, "hachureAngle", -41);
            it.fill2 = 0;
        }
        if (sketch && it.stroke) {   // (the outline its own item, drawn rough over the fill)
            Item o;
            o.path = roughPath(path.get());
            o.stroke = it.stroke, o.width = width, o.dash = dash;
            it.stroke = 0;
            add(std::move(it), width);
            add(std::move(o), width + 2);
        } else
            add(std::move(it), width);
        if (lines && stroke && width > 0) {
            Item l;
            l.path = sketch ? roughPath(lines.get()) : lines;
            l.stroke = stroke;
            l.width = width;
            l.dash = dash;
            add(std::move(l), width);
        }
    }

    // the box a vertex's label is laid out in: beside the shape by labelPosition /
    // verticalLabelPosition, less the spacing
    Box labelBox(const Style &s, Box b) {
        const std::string &lp = str(s, "labelPosition"), &vp = str(s, "verticalLabelPosition");
        if (lp == "left") b.x -= b.w;
        else if (lp == "right") b.x += b.w;
        if (vp == "top") b.y -= b.h;
        else if (vp == "bottom") b.y += b.h;
        double sp = num(s, "spacing", 2);
        double l = sp + num(s, "spacingLeft", 0), r = sp + num(s, "spacingRight", 0), t = sp + num(s, "spacingTop", 0), bt = sp + num(s, "spacingBottom", 0);
        const std::string &va = str(s, "verticalAlign");   // (Graph.js: mxText's baseSpacingTop 5; its baseSpacingBottom: in layoutLabel)
        if (va == "top") t += 5;
        return {b.x + l, b.y + t, std::max(0.0, b.w - l - r), std::max(0.0, b.h - t - bt)};
    }

    // an inline stencil (shape=stencil(base64 of the raw deflated, URL encoded <shape>), mxStencil): its background
    // and foreground drawn into b (scaled to its w x h, aspect=fixed: kept and centred); text and images left out
    void stencil(const std::string &shape, const Style &s, const Box &b) {
        std::string xml, bin = base64(shape.substr(8, shape.size() - (shape.back() == ')' ? 9 : 8)));
        if (!inflateRaw(bin, xml)) xml = bin;
        if (xml.compare(0, 1, "<") != 0) xml = urlDecode(xml);
        if (xml.size() > (4u << 20)) return;
        std::unique_ptr<xmlDoc, void (*)(xmlDoc *)> doc(xmlReadMemory(xml.data(), (int)xml.size(), nullptr, nullptr,
                                                                     XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
                                                        xmlFreeDoc);
        xmlNode *root = doc ? xmlDocGetRootElement(doc.get()) : nullptr;
        if (!root || !is(root, "shape")) return;
        double w0 = attrNum(root, "w", 100), h0 = attrNum(root, "h", 100);
        if (!(w0 > 0 && h0 > 0)) return;
        double sx = b.w / w0, sy = b.h / h0, x0 = b.x, y0 = b.y;
        if (attr(root, "aspect") == "fixed") {
            sx = sy = std::min(sx, sy);
            x0 += (b.w - w0 * sx) / 2, y0 += (b.h - h0 * sy) / 2;
        }
        double minScale = std::min(sx, sy);
        std::string sw = attr(root, "strokewidth");
        double opacity = num(s, "opacity", 100);
        // (as mxSvgCanvas2D: the colours as they are, faded on painting by alpha (the shape's opacity, or an <alpha>) and
        // fillAlpha / strokeAlpha (its fillOpacity / strokeOpacity, or a <fillalpha> / <strokealpha>))
        struct State {
            uint32_t fill, stroke;
            float width, alpha;
            bool dashed;
            float fillAlpha, strokeAlpha;
        };
        State st{colour(s, "fillColor", 0xffffffff), colour(s, "strokeColor", 0xff000000),
                 (float)(sw == "inherit" ? num(s, "strokeWidth", 1) : (sw.empty() ? 1 : attrNum(root, "strokewidth", 1)) * minScale),
                 (float)std::clamp(opacity / 100, 0.0, 1.0), false, (float)std::clamp(num(s, "fillOpacity", 100) / 100, 0.0, 1.0),
                 (float)std::clamp(num(s, "strokeOpacity", 100) / 100, 0.0, 1.0)};
        uint32_t shapeFill = st.fill, shapeStroke = st.stroke;
        std::vector<State> stack;
        PathP cur;
        auto X = [&](xmlNode *n, const char *k) { return (float)(x0 + attrNum(n, k) * sx); };
        auto Y = [&](xmlNode *n, const char *k) { return (float)(y0 + attrNum(n, k) * sy); };
        auto alphaOf = [](uint32_t c, float a) { return a >= 1 ? c : (uint32_t)std::lround((c >> 24) * a) << 24 | (c & 0xffffff); };
        auto colourAttr = [&](xmlNode *n, uint32_t def) {
            std::string v = attr(n, "color");
            return v == "stroke" ? shapeStroke : v == "fill" ? shapeFill : parseColour(v, def);
        };
        auto paintCur = [&](bool fill, bool stroke) {
            if (!cur) return;
            Item it;
            it.path = cur;
            if (fill) it.fill = alphaOf(st.fill, st.alpha * st.fillAlpha);
            if (stroke && st.width > 0) it.stroke = alphaOf(st.stroke, st.alpha * st.strokeAlpha), it.width = st.width;
            if (stroke && st.dashed) it.dash = {3 * std::max(st.width, 1.f), 3 * std::max(st.width, 1.f)};
            add(std::move(it), st.width);
            cur = nullptr;
        };
        size_t budget = 20000;   // (a damaged stencil's commands; at 0 the rest left out)
        std::function<void(xmlNode *)> node = [&](xmlNode *n) {
            if (n->type != XML_ELEMENT_NODE || budget == 0) return;
            budget--;
            std::string name = (const char *)n->name;
            if (name == "path") {
                cur = newPath();
                for (xmlNode *k = n->children; k; k = k->next) {
                    if (k->type != XML_ELEMENT_NODE || budget == 0) continue;
                    budget--;
                    std::string c = (const char *)k->name;
                    plutovg_path_t *p = cur.get();
                    if (c == "move") plutovg_path_move_to(p, X(k, "x"), Y(k, "y"));
                    else if (c == "line") plutovg_path_line_to(p, X(k, "x"), Y(k, "y"));
                    else if (c == "quad") plutovg_path_quad_to(p, X(k, "x1"), Y(k, "y1"), X(k, "x2"), Y(k, "y2"));
                    else if (c == "curve") plutovg_path_cubic_to(p, X(k, "x1"), Y(k, "y1"), X(k, "x2"), Y(k, "y2"), X(k, "x3"), Y(k, "y3"));
                    else if (c == "arc")
                        plutovg_path_arc_to(p, (float)(attrNum(k, "rx") * sx), (float)(attrNum(k, "ry") * sy), (float)(attrNum(k, "x-axis-rotation") * M_PI / 180),
                                            attrNum(k, "large-arc-flag") != 0, attrNum(k, "sweep-flag") != 0, X(k, "x"), Y(k, "y"));
                    else if (c == "close") plutovg_path_close(p);
                }
            } else if (name == "rect" || name == "roundrect" || name == "ellipse") {
                cur = newPath();
                float x = X(n, "x"), y = Y(n, "y"), w = (float)(attrNum(n, "w") * sx), h = (float)(attrNum(n, "h") * sy);
                if (name == "ellipse") plutovg_path_add_ellipse(cur.get(), x + w / 2, y + h / 2, w / 2, h / 2);
                else if (name == "rect") plutovg_path_add_rect(cur.get(), x, y, w, h);
                else {
                    double a = attrNum(n, "arcsize", 0);
                    float r = (float)(std::min(w, h) * (a > 0 ? a : 15) / 100);
                    plutovg_path_add_round_rect(cur.get(), x, y, w, h, r, r);
                }
            } else if (name == "fill") paintCur(true, false);
            else if (name == "stroke") paintCur(false, true);
            else if (name == "fillstroke") paintCur(true, true);
            else if (name == "fillcolor") st.fill = colourAttr(n, 0);
            else if (name == "strokecolor") st.stroke = colourAttr(n, 0);
            else if (name == "strokewidth") st.width = (float)(attrNum(n, "width", 1) * minScale);
            else if (name == "alpha") st.alpha = (float)std::clamp(attrNum(n, "alpha", 1), 0.0, 1.0);
            else if (name == "fillalpha") st.fillAlpha = (float)std::clamp(attrNum(n, "alpha", 1), 0.0, 1.0);
            else if (name == "strokealpha") st.strokeAlpha = (float)std::clamp(attrNum(n, "alpha", 1), 0.0, 1.0);
            else if (name == "dashed") st.dashed = attr(n, "dashed") == "1";
            else if (name == "save") stack.push_back(st);
            else if (name == "restore" && !stack.empty()) st = stack.back(), stack.pop_back();
        };
        for (xmlNode *part = root->children; part; part = part->next)
            if (is(part, "background") || is(part, "foreground"))
                for (xmlNode *n = part->children; n; n = n->next) node(n);
    }

    // a stencil drawn turned by its direction: in the box turned upright, then turned with the shape
    static Box stencilBox(const Style &s, const Box &b, double &turn) {
        const std::string &dir = str(s, "direction");
        double deg = dir == "south" ? 90 : dir == "west" ? 180 : dir == "north" ? 270 : 0;
        turn += deg;
        Pt c = b.centre();
        return deg == 90 || deg == 270 ? Box{c.x - b.h / 2, c.y - b.w / 2, b.h, b.w} : b;
    }

    // a table's lines (Graph.getTableLines): between its rows (rowLines) and between the first row's cells (columnLines),
    // in its stroke
    // the cells of each parent, in the cells' order (built once a page)
    std::unordered_map<std::string, std::vector<size_t>> kidsIndex;
    bool kidsBuilt = false;
    const std::vector<size_t> &kidsOf(const std::string &id) {
        static const std::vector<size_t> none;
        if (!kidsBuilt) {
            for (size_t i = 0; i < cells.size(); i++) kidsIndex[cells[i].parent].push_back(i);
            kidsBuilt = true;
        }
        auto f = kidsIndex.find(id);
        return f == kidsIndex.end() ? none : f->second;
    }
    void tableLines(Cell &t, const Box &b) {
        std::vector<Box> rows, cols;
        for (size_t i : kidsOf(t.id))
            if (cells[i].vertex && cells[i].id != t.id) rows.push_back(absBox(cells[i]));
        if (rows.empty()) return;
        std::sort(rows.begin(), rows.end(), [](const Box &p, const Box &q) { return p.y < q.y; });
        Cell *firstRow = nullptr;
        for (size_t i : kidsOf(t.id))
            if (cells[i].vertex && absBox(cells[i]).y == rows[0].y) { firstRow = &cells[i]; break; }
        if (firstRow)
            for (size_t i : kidsOf(firstRow->id))
                if (cells[i].vertex) cols.push_back(absBox(cells[i]));
        std::sort(cols.begin(), cols.end(), [](const Box &p, const Box &q) { return p.x < q.x; });
        PathP l = newPath();
        if (str(t.style, "rowLines") != "0")
            for (size_t i = 1; i < rows.size(); i++)
                plutovg_path_move_to(l.get(), (float)b.x, (float)rows[i].y), plutovg_path_line_to(l.get(), (float)(b.x + b.w), (float)rows[i].y);
        if (str(t.style, "columnLines") != "0")
            for (size_t i = 1; i < cols.size(); i++)
                plutovg_path_move_to(l.get(), (float)cols[i].x, (float)rows[0].y), plutovg_path_line_to(l.get(), (float)cols[i].x, (float)(b.y + b.h));
        Style ls = t.style;
        ls["fillColor"] = "none";
        ls["shadow"] = "0";
        paint(newPath(), l, ls, b, false);
    }

    void vertex(Cell &c) {
        Box b = absBox(c);
        std::string shape = str(c.style, "shape");
        if (shape.empty()) shape = "rectangle";
        bool table = shape == "table";
        if (shape == "table" || shape == "tableRow") {   // (Shapes.js TableShape: with a title (startSize, 40 by default) a
            if (!c.style.count("startSize")) c.style["startSize"] = "40";   // swimlane, else a partial rectangle; rows: the same)
            bool head = num(c.style, "startSize", 40) > 0;
            shape = head ? "swimlane" : str(c.style, "rounded") == "1" ? "rectangle" : "partialRectangle";
        }
        const Style &s = c.style;
        double rotation = num(s, "rotation", 0), turn = rotation;   // (turn: and a stencil's direction)
        bool text = shape == "text" || (str(s, "strokeColor") == "none" && str(s, "fillColor") == "none");
        size_t first = page.items.size();   // (the shape's items: turned together by its rotation at the end)
        if (shape == "swimlane" && b.w > 0 && b.h > 0) {   // (its header filled, the body by swimlaneFillColor: the body first,
                                                           // the header over it, the frame and the header's line over both)
            bool across = str(s, "horizontal") == "0";
            double ss = std::clamp(num(s, "startSize", 23), 0.0, across ? b.w : b.h);
            const std::string &bodyFill = str(s, "swimlaneFillColor");
            bool bodyShown = !bodyFill.empty() && parseColour(bodyFill, 0);
            PathP lines, outline = newPath();
            plutovg_path_add_rect(outline.get(), (float)b.x, (float)b.y, (float)b.w, (float)b.h);
            if (bodyShown) {
                Style body = s;
                body["fillColor"] = bodyFill;
                body["gradientColor"] = "none";
                body["strokeColor"] = "none";
                paint(outline, nullptr, body, b, false);
            }
            if (ss > 0) {
                Box hb = across ? Box{b.x, b.y, ss, b.h} : Box{b.x, b.y, b.w, ss};
                Style head = s;
                head["strokeColor"] = "none";
                if (bodyShown) head["shadow"] = "0";   // (the body's shadow is the swimlane's)
                PathP hp = newPath();
                plutovg_path_add_rect(hp.get(), (float)hb.x, (float)hb.y, (float)hb.w, (float)hb.h);
                lines = newPath();
                if (across) plutovg_path_move_to(lines.get(), (float)(b.x + ss), (float)b.y), plutovg_path_line_to(lines.get(), (float)(b.x + ss), (float)(b.y + b.h));
                else plutovg_path_move_to(lines.get(), (float)b.x, (float)(b.y + ss)), plutovg_path_line_to(lines.get(), (float)(b.x + b.w), (float)(b.y + ss));
                paint(hp, nullptr, head, hb, false);
            }
            Style frame = s;
            frame["fillColor"] = "none";
            frame["gradientColor"] = "none";
            if (bodyShown) frame["shadow"] = "0";
            paint(outline, lines, frame, b, false);
        } else if (shape.compare(0, 8, "stencil(") == 0 && b.w > 0 && b.h > 0) {
            stencil(shape, s, stencilBox(s, b, turn));
        } else if (shape == "partialRectangle") {   // (Shapes.js: filled, outlined on the sides top / right / bottom / left say)
            PathP r = newPath(), l = newPath();
            plutovg_path_add_rect(r.get(), (float)b.x, (float)b.y, (float)b.w, (float)b.h);
            Style fill = s;
            fill["strokeColor"] = "none";
            paint(r, nullptr, fill, b, false);
            // (its sides in the box turned upright by its direction, then turned with it: south 90, west 180, north 270)
            const std::string &dir = str(s, "direction");
            double deg = dir == "south" ? 90 : dir == "west" ? 180 : dir == "north" ? 270 : 0;
            Pt ce = b.centre();
            Box ub = deg == 90 || deg == 270 ? Box{ce.x - b.h / 2, ce.y - b.w / 2, b.h, b.w} : b;
            Pt c[4] = {{ub.x, ub.y}, {ub.x + ub.w, ub.y}, {ub.x + ub.w, ub.y + ub.h}, {ub.x, ub.y + ub.h}};
            for (Pt &q : c) q = rotated(q, deg, ce);
            const char *side[4] = {"top", "right", "bottom", "left"};
            for (int k = 0; k < 4; k++)
                if (str(s, side[k]) != "0") {
                    plutovg_path_move_to(l.get(), (float)c[k].x, (float)c[k].y);
                    plutovg_path_line_to(l.get(), (float)c[(k + 1) % 4].x, (float)c[(k + 1) % 4].y);
                }
            Style edges = s;
            edges["fillColor"] = "none";
            edges["shadow"] = "0";
            paint(newPath(), l, edges, b, false);
        } else if (shape == "umlLifeline" && b.w > 0 && b.h > 0) {   // (Shapes.js UmlLifeline: the head, a dashed line down;
            bool mirror = str(s, "lifelineMirror") == "1";                // lifelineMirror: the head again at its foot)
            double size = std::clamp(num(s, "size", 40), 0.0, mirror ? b.h / 2 : b.h), end = mirror ? b.h - size : b.h;
            const std::string &who = str(s, "participant");
            for (int k = 0; k <= (int)mirror; k++) {
                Box hb{b.x, k ? b.y + b.h - size : b.y, b.w, size};
                PathP l, head = who.empty() ? shapePath("rectangle", s, hb, l) : shapePath(who, s, hb, l);
                paint(head, l, s, hb, false);
            }
            if (size < end) {
                Style ls = s;
                ls["fillColor"] = "none";
                ls["shadow"] = "0";
                ls["dashed"] = str(s, "lifelineDashed") == "0" ? "0" : "1";
                if (!str(s, "lifelineColor").empty()) ls["strokeColor"] = str(s, "lifelineColor");
                PathP down = newPath();
                plutovg_path_move_to(down.get(), (float)(b.x + b.w / 2), (float)(b.y + size));
                plutovg_path_line_to(down.get(), (float)(b.x + b.w / 2), (float)(b.y + end));
                paint(newPath(), down, ls, b, false);
            }
        } else if (shape == "umlFrame" && b.w > 0 && b.h > 0) {   // (Shapes.js UmlFrame: the tab filled, the frame outlined)
            double w0 = std::min(b.w, std::max(10.0, num(s, "width", 60))), h0 = std::min(b.h, std::max(15.0, num(s, "height", 30)));
            const std::string &bodyFill = str(s, "swimlaneFillColor");
            if (!bodyFill.empty() && parseColour(bodyFill, 0)) {
                Style body = s;
                body["fillColor"] = bodyFill;
                body["gradientColor"] = "none";
                body["strokeColor"] = "none";
                PathP r = newPath();
                plutovg_path_add_rect(r.get(), (float)b.x, (float)b.y, (float)b.w, (float)b.h);
                paint(r, nullptr, body, b, false);
            }
            PathP tab = newPath(), frame = newPath();
            polygon(tab.get(), {{0, 0}, {w0, 0}, {w0, std::max(0.0, h0 - 15)}, {std::max(0.0, w0 - 10), h0}, {0, h0}}, b);
            paint(tab, nullptr, s, Box{b.x, b.y, w0, h0}, false);
            plutovg_path_move_to(frame.get(), (float)(b.x + w0), (float)b.y);
            plutovg_path_line_to(frame.get(), (float)(b.x + b.w), (float)b.y);
            plutovg_path_line_to(frame.get(), (float)(b.x + b.w), (float)(b.y + b.h));
            plutovg_path_line_to(frame.get(), (float)b.x, (float)(b.y + b.h));
            plutovg_path_line_to(frame.get(), (float)b.x, (float)(b.y + h0));
            Style fs = s;
            fs["fillColor"] = "none";
            fs["shadow"] = "0";
            paint(newPath(), frame, fs, b, false);
        } else if (shape == "waypoint") {   // (draw.io's waypoint: a dot of its size in the stroke colour, at the centre)
            uint32_t dot = withOpacity(colour(s, "strokeColor", 0xff000000), num(s, "opacity", 100) * num(s, "strokeOpacity", 100) / 100);
            double r = std::clamp(num(s, "size", 6), 0.0, 1000.0) / 2;
            if (dot && r > 0) {
                Item it;
                it.path = newPath();
                plutovg_path_add_circle(it.path.get(), (float)(b.x + b.w / 2), (float)(b.y + b.h / 2), (float)r);
                it.fill = dot;
                add(std::move(it), 0);
            }
        } else if (!text && shape != "image" && b.w > 0 && b.h > 0) {
            if (shape == "mxgraph.gcp2.doubleRect") {   // (mxGCP2: a box 8 lower right behind the front one)
                PathP back = newPath(), none;
                plutovg_path_add_round_rect(back.get(), (float)(b.x + 8), (float)(b.y + 8), (float)std::max(0.0, b.w - 8), (float)std::max(0.0, b.h - 8), 1, 1);
                paint(back, none, s, b, false);
            }
            PathP lines;
            PathP p = shapePath(shape, s, b, lines);
            if ((shape == "mxgraph.aws4.resourceIcon" || (shape == "mxgraph.aws4.group" && str(s, "grStroke") == "0"))) {
                Style f = s;   // (mxAWS4: a resource icon's box filled only (its icon: a stencil), a group's outlined if grStroke)
                f["strokeColor"] = "none";
                paint(p, lines, f, b, false);
            } else
                paint(p, lines, s, b, false);
            if (shape == "mxgraph.infographic.shadedCube") {   // (its right face 0.2 darker, its left one 0.2 lighter)
                double d = std::min(b.w * std::tan(std::clamp(num(s, "isoAngle", 15), 0.01, 94.0) * M_PI / 200), b.h / 2);
                for (int k = 0; k < 2; k++) {
                    Item f;
                    f.path = newPath();
                    if (!k) polygon(f.path.get(), {{b.w / 2, 2 * d}, {b.w, d}, {b.w, b.h - d}, {b.w / 2, b.h}}, b);
                    else polygon(f.path.get(), {{b.w / 2, 2 * d}, {0, d}, {0, b.h - d}, {b.w / 2, b.h}}, b);
                    f.fill = withOpacity(k ? 0x33ffffffu : 0x33000000u, num(s, "opacity", 100));
                    add(std::move(f), 0);
                }
            }
            if (shape == "mxgraph.infographic.cylinder") {   // (a sheen across it, 0.4 black at the left to 0.4 white at the right)
                Item g;
                g.path = newPath();
                plutovg_path_t *q = g.path.get();
                plutovg_path_move_to(q, (float)b.x, (float)(b.y + 10));
                plutovg_path_arc_to(q, (float)(b.w / 2), 10, 0, false, false, (float)(b.x + b.w), (float)(b.y + 10));
                plutovg_path_line_to(q, (float)(b.x + b.w), (float)(b.y + b.h - 10));
                plutovg_path_arc_to(q, (float)(b.w / 2), 10, 0, false, true, (float)b.x, (float)(b.y + b.h - 10));
                plutovg_path_close(q);
                g.fill = withOpacity(0x66000000, num(s, "opacity", 100)), g.fill2 = withOpacity(0x66ffffff, num(s, "opacity", 100));
                g.gx0 = (float)b.x, g.gx1 = (float)(b.x + b.w), g.gy0 = g.gy1 = (float)b.y;
                add(std::move(g), 0);
            }
            if (str(s, "glass") == "1" && (shape == "rectangle" || shape == "label") && colour(s, "fillColor", 0xffffffff)) {
                // (mxShape.paintGlassEffect: a white sheen over the top, 0.9 to 0.1 opaque, its lower edge bowed)
                double sw = std::ceil(std::clamp(num(s, "strokeWidth", 1), 0.0, 1000.0) / 2), x = b.x - sw, y = b.y - sw, x1 = b.x + b.w + sw;
                double arc = 0;
                if (str(s, "rounded") == "1")
                    arc = std::clamp(str(s, "absoluteArcSize") == "1" ? num(s, "arcSize", 10) / 2 : std::min(b.w, b.h) * num(s, "arcSize", 15) / 100, 0.0,
                                     std::min(b.w, b.h) / 2) + 2 * sw;
                Item g;
                g.path = newPath();
                plutovg_path_t *q = g.path.get();
                plutovg_path_move_to(q, (float)(x + arc), (float)y);
                if (arc > 0) plutovg_path_quad_to(q, (float)x, (float)y, (float)x, (float)(y + arc));
                plutovg_path_line_to(q, (float)x, (float)(b.y + b.h * 0.4));
                plutovg_path_quad_to(q, (float)(b.x + b.w / 2), (float)(b.y + b.h * 0.7), (float)x1, (float)(b.y + b.h * 0.4));
                plutovg_path_line_to(q, (float)x1, (float)(y + arc));
                if (arc > 0) plutovg_path_quad_to(q, (float)x1, (float)y, (float)(x1 - arc), (float)y);
                plutovg_path_close(q);
                g.fill = withOpacity(0xe6ffffff, num(s, "opacity", 100)), g.fill2 = withOpacity(0x1affffff, num(s, "opacity", 100));
                g.gx0 = g.gx1 = (float)b.x, g.gy0 = (float)b.y, g.gy1 = (float)(b.y + b.h * 0.6);
                add(std::move(g), 0);
            }
            if (shape == "cube" && str(s, "direction").empty()) {   // (CubeShape: its top and side darkened (darkOpacity, -2: white))
                double d = std::clamp(num(s, "size", 20), 0.0, std::min(b.w, b.h));
                for (int k = 0; k < 2; k++) {
                    double op = std::clamp(num(s, k ? "darkOpacity2" : "darkOpacity", 0), -1.0, 1.0);
                    if (op == 0) continue;
                    Item f;
                    f.path = newPath();
                    if (!k) polygon(f.path.get(), {{0, 0}, {b.w - d, 0}, {b.w, d}, {d, d}}, b);
                    else polygon(f.path.get(), {{0, 0}, {d, d}, {d, b.h}, {0, b.h - d}}, b);
                    f.fill = withOpacity((uint32_t)std::lround(std::fabs(op) * 255) << 24 | (op < 0 ? 0xffffffu : 0), num(s, "opacity", 100));
                    add(std::move(f), 0);
                }
            }
        } else if (shape == "image" && b.w > 0 && b.h > 0 && str(s, "image").compare(0, 11, "data:image/") == 0) {
            uint32_t back = colour(s, "imageBackground", 0), border = colour(s, "imageBorder", 0);
            if (back || border) {   // (mxImageShape: its background and border, the box behind the picture)
                Item bg;
                bg.path = newPath();
                plutovg_path_add_rect(bg.path.get(), (float)b.x, (float)b.y, (float)b.w, (float)b.h);
                bg.fill = withOpacity(back, num(s, "opacity", 100));
                bg.stroke = withOpacity(border, num(s, "opacity", 100)), bg.width = (float)std::clamp(num(s, "strokeWidth", 1), 0.0, 1000.0);
                add(std::move(bg), bg.width);
            }
            Item it;   // (a picture the file holds: decoded once the page is read, see pictures())
            it.path = newPath();
            plutovg_path_add_rect(it.path.get(), (float)b.x, (float)b.y, (float)b.w, (float)b.h);
            it.uri = str(s, "image");
            it.ix0 = (float)b.x, it.iy0 = (float)b.y, it.ix1 = (float)(b.x + b.w), it.iy1 = (float)(b.y + b.h);
            it.aspect = str(s, "imageAspect") != "0";
            it.pictureAlpha = (float)std::clamp(num(s, "opacity", 100) / 100, 0.0, 1.0);
            add(std::move(it), 0);
        } else if (shape == "image" && b.w > 0 && b.h > 0) {   // (the picture is not in the file: its frame)
            Item it;
            it.path = newPath();
            plutovg_path_add_rect(it.path.get(), (float)b.x, (float)b.y, (float)b.w, (float)b.h);
            it.stroke = 0xffc0c0c0;
            it.width = 1;
            add(std::move(it), 1);
        }
        if (table && b.w > 0 && b.h > 0) tableLines(c, b);
        bool flipH = str(s, "flipH") == "1", flipV = str(s, "flipV") == "1";
        if (shape.compare(0, 8, "stencil(") == 0)   // (a stencil's own flips too, as its connection points take them)
            flipH = flipH || str(s, "stencilFlipH") == "1", flipV = flipV || str(s, "stencilFlipV") == "1";
        if ((turn != 0 || flipH || flipV) && b.w > 0 && b.h > 0) {   // (the shape's items mirrored, then turned about its centre
            plutovg_matrix_t m;                                          // as mxSvgCanvas2D.rotate does; their boxes again)
            plutovg_matrix_init_translate(&m, (float)(b.x + b.w / 2), (float)(b.y + b.h / 2));
            plutovg_matrix_rotate(&m, (float)(turn * M_PI / 180));
            plutovg_matrix_scale(&m, flipH ? -1.f : 1.f, flipV ? -1.f : 1.f);
            plutovg_matrix_translate(&m, (float)-(b.x + b.w / 2), (float)-(b.y + b.h / 2));
            for (size_t k = first; k < page.items.size(); k++) {
                Item &it = page.items[k];
                if (!it.path || it.label) continue;
                PathP t = newPath();
                plutovg_path_add_path(t.get(), it.path.get(), &m);
                it.path = t;
                if (!it.uri.empty() || it.picture) {   // (a picture turned and mirrored with its frame)
                    plutovg_matrix_t pm = it.pm;
                    plutovg_matrix_multiply(&it.pm, &pm, &m);
                    it.turned = true;
                }
                nd::Box bb = nd::pathBox(t.get(), std::max(it.width, 1.f));
                it.bx0 = bb.x0, it.by0 = bb.y0, it.bx1 = bb.x1, it.by1 = bb.y1;
                if (it.fill2) {   // (a gradient's line turned with it)
                    plutovg_point_t g0{it.gx0, it.gy0}, g1{it.gx1, it.gy1};
                    plutovg_matrix_map_point(&m, &g0, &g0), plutovg_matrix_map_point(&m, &g1, &g1);
                    it.gx0 = g0.x, it.gy0 = g0.y, it.gx1 = g1.x, it.gy1 = g1.y;
                }
            }
        }
        if (c.value.empty()) return;
        Box lb = b;
        if (c.hasOffset && !c.relative) lb.x += c.offset.x, lb.y += c.offset.y;   // (an absolute geometry's offset moves the label)
        if (shape == "umlLifeline") lb.h = std::clamp(num(s, "size", 40), 0.0, str(s, "lifelineMirror") == "1" ? b.h / 2 : b.h);   // (in its head)
        else if (shape == "cube" && str(s, "boundedLbl") == "1") {   // (on its front face)
            double d = std::clamp(num(s, "size", 20), 0.0, std::min(b.w, b.h));
            lb.x += d, lb.y += d, lb.w -= d, lb.h -= d;
        }
        else if (shape == "mxgraph.arrows2.arrow" && str(s, "boundedLbl") == "1") {   // (on the shaft)
            std::string d = str(s, "direction");
            if (d.empty()) d = "east";
            if (str(s, "flipH") == "1") d = d == "west" ? "east" : d == "east" ? "west" : d;
            if (str(s, "flipV") == "1") d = d == "north" ? "south" : d == "south" ? "north" : d;
            bool v = d == "north" || d == "south";
            double c = 0.5 * (v ? b.w : b.h) * std::clamp(num(s, "dy", 0.5), 0.0, 1.0), f = std::clamp(num(s, "dx", 0.5), 0.0, v ? b.h : b.w);
            if (d == "east") lb = {b.x, b.y + c, b.w - f, b.h - 2 * c};
            else if (d == "west") lb = {b.x + f, b.y + c, b.w - f, b.h - 2 * c};
            else if (d == "north") lb = {b.x + c, b.y + f, b.w - 2 * c, b.h - f};
            else lb = {b.x + c, b.y, b.w - 2 * c, b.h - f};
        }
        else if (shape == "folder" && str(s, "boundedLbl") == "1") {   // (FolderShape: below its tab, labelInHeader: in it)
            double tw = std::clamp(num(s, "tabWidth", 15), 0.0, b.w), th = std::clamp(num(s, "tabHeight", 15), 0.0, b.h);
            if (str(s, "labelInHeader") == "1") {
                if (str(s, "tabPosition") != "left") lb.x += b.w - tw;
                lb.w = tw, lb.h = th;
            } else
                lb.y += th, lb.h -= th;
        }
        else if (shape == "umlFrame") lb.w = std::min(b.w, std::max(10.0, num(s, "width", 60))), lb.h = std::min(b.h, std::max(15.0, num(s, "height", 30)));
        bool turned = false;
        if (shape == "swimlane") {   // (in its header)
            double ss = num(s, "startSize", 23);
            if (str(s, "horizontal") == "0") lb.w = std::min(ss, b.w), turned = true;
            else lb.h = std::min(ss, b.h);
        } else if (str(s, "horizontal") == "0")
            turned = true;
        double quarter = turned ? -90 : 0;   // (horizontal=0: read upwards; textDirection vertical-lr / -rl: downwards)
        if (str(s, "textDirection").compare(0, 8, "vertical") == 0) turned = true, quarter = 90;
        Box inner;
        if (turned) {   // (laid out in the box turned upright, then turned back by -90 degrees)
            Pt ce = lb.centre();
            const std::string &lp = str(s, "labelPosition"), &vp = str(s, "verticalLabelPosition");
            double ox = lp == "left" ? -lb.w : lp == "right" ? lb.w : 0, oy = vp == "top" ? -lb.h : vp == "bottom" ? lb.h : 0;
            lb = {ce.x - lb.h / 2, ce.y - lb.w / 2, lb.h, lb.w};
            // (horizontal=0: the label's offset beside the shape taken in the turned box, its x and y exchanged, as
            // mxCellRenderer.getLabelBounds does for a text whose paint bounds are inverted)
            if (quarter == -90) {
                Style ns = s;
                ns.erase("labelPosition"), ns.erase("verticalLabelPosition");
                inner = labelBox(ns, {lb.x + oy, lb.y + ox, lb.w, lb.h});
            } else
                inner = labelBox(s, lb);
        } else
            inner = labelBox(s, lb);
        if (str(s, "overflow") == "fill" && str(s, "html") == "1") {   // (HTML that fills the shape: no spacing)
            Style none = s;
            none["spacing"] = "0";
            for (const char *k : {"spacingLeft", "spacingRight", "spacingTop", "spacingBottom", "verticalAlign"}) none.erase(k);
            inner = labelBox(none, lb);
        }
        double wrap = str(s, "whiteSpace") == "wrap" ? inner.w + (str(s, "html") == "1" ? 2 : 0) : 0;   // (an HTML label's div: 2 wider)
        std::shared_ptr<Label> l = layoutLabel(c.value, s, inner, wrap);
        if (l && str(s, "overflow") == "hidden") {   // (mxText clipped: the label's box beside the shape as labelPosition puts it,
            Style ns = s;                              // its spacing inside)
            ns["spacing"] = "0";
            for (const char *k : {"spacingLeft", "spacingRight", "spacingTop", "spacingBottom", "verticalAlign"}) ns.erase(k);
            Box cb = turned ? lb : labelBox(ns, lb);
            if (l->y1 - l->y0 > inner.h && l->y0 != (float)inner.y) {   // (taller than its box: the clipped block no taller, from its top)
                float dy = (float)inner.y - l->y0;
                for (Run &r : l->runs) r.y += dy;
                for (Label::Rect &q : l->rects) q.y += dy;
                l->y0 += dy, l->y1 += dy;
            }
            l->clip = true;
            l->clipX0 = (float)cb.x, l->clipY0 = (float)cb.y, l->clipX1 = (float)(cb.x + cb.w), l->clipY1 = (float)(cb.y + cb.h);
        }
        // (turned: its own quarter turn about its box's centre; the shape's rotation moves that centre round the shape's)
        auto place = [&](std::shared_ptr<Label> &lab, Box box) {
            if (!lab || (!turned && rotation == 0)) return;
            Pt ce = box.centre(), bc = b.centre();
            if (rotation != 0 && (ce.x != bc.x || ce.y != bc.y)) {
                Pt to = rotated(ce, rotation, bc);
                float dx = (float)(to.x - ce.x), dy = (float)(to.y - ce.y);
                for (Run &r : lab->runs) r.x += dx, r.y += dy;
                for (Label::Rect &q : lab->rects) q.x += dx, q.y += dy;
                lab->x0 += dx, lab->x1 += dx, lab->y0 += dy, lab->y1 += dy;
                lab->clipX0 += dx, lab->clipX1 += dx, lab->clipY0 += dy, lab->clipY1 += dy;
                ce = to;
            }
            lab->turned = true;
            lab->cx = (float)ce.x, lab->cy = (float)ce.y;
            lab->angle = (float)(quarter + rotation) * (float)M_PI / 180;
        };
        place(l, lb);
        addLabel(l);
        if (shape == "umlLifeline" && str(s, "lifelineMirror") == "1" && !turned) {   // (a copy in its foot, turned with it)
            Box fb = lb;
            fb.y = b.y + b.h - lb.h;
            Box fi = labelBox(s, fb);
            std::shared_ptr<Label> foot = layoutLabel(c.value, s, fi, str(s, "whiteSpace") == "wrap" ? fi.w + (str(s, "html") == "1" ? 2 : 0) : 0);
            place(foot, fb);
            addLabel(foot);
        }
    }

    // --- edges ---

    // The routing follows mxGraph's (mxGraphView, mxEdgeStyle, mxPerimeter; Apache-2.0): the fixed
    // ends first (a connection constraint, or a dangling end's point), then the edge style's
    // points between them (OrthConnector, SegmentConnector, ElbowConnector ...), then the floating
    // ends on their terminals' perimeters, towards the neighbouring point.

    struct Term {   // a terminal vertex
        Box b;
        std::string perimeter;   // rectanglePerimeter (the default), ellipsePerimeter, rhombusPerimeter, trianglePerimeter
        const Style *style = nullptr;
        double rotation = 0;   // (degrees, about the box's centre)
    };
    static Term term(Box b, const Style &s) { return {b, str(s, "perimeter"), &s, num(s, "rotation", 0)}; }
    static Pt rotated(Pt p, double deg, Pt c) {
        double a = deg * M_PI / 180, co = std::cos(a), si = std::sin(a), dx = p.x - c.x, dy = p.y - c.y;
        return {c.x + dx * co - dy * si, c.y + dx * si + dy * co};
    }

    // mxPerimeter.TrianglePerimeter: the triangle of the shape (its direction: east, the default, points right)
    static Pt trianglePoint(const Term &t, Pt next, bool orthogonal) {
        const Box &bb = t.b;
        const std::string &dir = t.style ? str(*t.style, "direction") : std::string();
        bool vertical = dir == "north" || dir == "south";
        double x = bb.x, y = bb.y, w = bb.w, h = bb.h, cx = x + w / 2, cy = y + h / 2;
        Pt start{x, y}, corner{x + w, cy}, end{x, y + h};
        if (dir == "north") start = end, corner = {cx, y}, end = {x + w, y + h};
        else if (dir == "south") corner = {cx, y + h}, end = {x + w, y};
        else if (dir == "west") start = {x + w, y}, corner = {x, cy}, end = {x + w, y + h};
        double dx = next.x - cx, dy = next.y - cy;
        double alpha = vertical ? std::atan2(dx, dy) : std::atan2(dy, dx), tt = vertical ? std::atan2(w, h) : std::atan2(h, w);
        bool base = dir == "north" || dir == "west" ? alpha > -tt && alpha < tt : alpha < -M_PI + tt || alpha > M_PI - tt;
        auto cross = [](double x0, double y0, double x1, double y1, Pt a, Pt b, Pt &r) {   // (mxUtils.intersection: of the segments)
            double den = (b.y - a.y) * (x1 - x0) - (b.x - a.x) * (y1 - y0);
            if (den == 0) return false;
            double ua = ((b.x - a.x) * (y0 - a.y) - (b.y - a.y) * (x0 - a.x)) / den, ub = ((x1 - x0) * (y0 - a.y) - (y1 - y0) * (x0 - a.x)) / den;
            if (!(ua >= 0 && ua <= 1 && ub >= 0 && ub <= 1)) return false;
            r = {x0 + ua * (x1 - x0), y0 + ua * (y1 - y0)};
            return true;
        };
        if (base) {
            if (orthogonal && ((vertical && next.x >= start.x && next.x <= end.x) || (!vertical && next.y >= start.y && next.y <= end.y)))
                return vertical ? Pt{next.x, start.y} : Pt{start.x, next.y};
            if (dir == "north") return {x + w / 2 + h * std::tan(alpha) / 2, y + h};
            if (dir == "south") return {x + w / 2 - h * std::tan(alpha) / 2, y};
            if (dir == "west") return {x + w, y + h / 2 + w * std::tan(alpha) / 2};
            return {x, y + h / 2 - w * std::tan(alpha) / 2};
        }
        double ox = cx, oy = cy;
        if (orthogonal) {
            if (next.y >= y && next.y <= y + h) ox = vertical ? cx : dir == "west" ? x + w : x, oy = next.y;
            else if (next.x >= x && next.x <= x + w) ox = next.x, oy = !vertical ? cy : dir == "north" ? y + h : y;
        }
        Pt r;
        bool first = vertical ? next.x <= x + w / 2 : next.y <= y + h / 2;
        if (first ? cross(next.x, next.y, ox, oy, start, corner, r) : cross(next.x, next.y, ox, oy, corner, end, r)) return r;
        return {cx, cy};
    }

    // mxPerimeter: the point of the terminal's perimeter towards next (orthogonal: straight across
    // from next where it lies beside the terminal). A turned terminal: next turned back, the point on
    // the upright shape turned with it (as mxGraphView.getPerimeterPoint)
    // (mxGraphView.getPerimeterPoint: next turned back by the shape's rotation and mirrored by its flips (a stencil's own
    // too), the point on the upright perimeter mirrored and turned again)
    static Pt perimeterPoint(const Term &t, Pt next, bool orthogonal) {
        Pt c = t.b.centre();
        bool fh = false, fv = false;
        if (t.style) {
            const Style &s = *t.style;
            fh = str(s, "flipH") == "1", fv = str(s, "flipV") == "1";
            if (str(s, "shape").compare(0, 8, "stencil(") == 0) fh = fh || str(s, "stencilFlipH") == "1", fv = fv || str(s, "stencilFlipV") == "1";
        }
        if (t.rotation != 0) next = rotated(next, -t.rotation, c);
        if (fh) next.x = 2 * c.x - next.x;
        if (fv) next.y = 2 * c.y - next.y;
        Pt p = uprightPoint(t, next, orthogonal);
        if (fh) p.x = 2 * c.x - p.x;
        if (fv) p.y = 2 * c.y - p.y;
        return t.rotation != 0 ? rotated(p, t.rotation, c) : p;
    }
    // mxUtils.getPerimeterPoint: of the outline's segments that cross the segment centre - next, the crossing nearest to next
    static bool outlineCrossing(const std::vector<Pt> &pts, Pt c, Pt n, Pt &out) {
        bool found = false;
        double best = 0;
        for (size_t i = 0; i + 1 < pts.size(); i++) {
            Pt a = pts[i], b = pts[i + 1];
            double den = (n.y - c.y) * (b.x - a.x) - (n.x - c.x) * (b.y - a.y);
            if (den == 0) continue;
            double ua = ((n.x - c.x) * (a.y - c.y) - (n.y - c.y) * (a.x - c.x)) / den;
            double ub = ((b.x - a.x) * (a.y - c.y) - (b.y - a.y) * (a.x - c.x)) / den;
            if (ua < 0 || ua > 1 || ub < 0 || ub > 1) continue;
            Pt p{a.x + ua * (b.x - a.x), a.y + ua * (b.y - a.y)};
            double d = (n.x - p.x) * (n.x - p.x) + (n.y - p.y) * (n.y - p.y);
            if (!found || d < best) found = true, best = d, out = p;
        }
        return found;
    }
    // Shapes.js getOutlinePerimeterPoint: where an edge towards next leaves an outline (orthogonal: on next's
    // vertical or horizontal line where that reaches the outline)
    static Pt outlinePoint(const Box &b, const std::vector<Pt> &pts, Pt next, bool orthogonal) {
        Pt c = b.centre(), r;
        if (orthogonal) {
            Pt p1 = c;
            if (next.x < b.x || next.x > b.x + b.w) {
                p1.y = next.y;
                if (outlineCrossing(pts, p1, next, r)) return r;
                p1.x = next.x > p1.x ? b.x - 1 : b.x + b.w + 1;
                if (outlineCrossing(pts, p1, next, r)) return r;
            } else {
                p1.x = next.x;
                if (outlineCrossing(pts, p1, next, r)) return r;
                if (next.y < b.y || next.y > b.y + b.h) {
                    p1.y = next.y > p1.y ? b.y - 1 : b.y + b.h + 1;
                    if (outlineCrossing(pts, p1, next, r)) return r;
                }
            }
        }
        return outlineCrossing(pts, c, next, r) ? r : c;
    }
    // the outlines of draw.io's parallelogram, trapezoid, step and hexagon perimeters (Shapes.js); false for other perimeters
    static bool perimeterOutline(const Term &t, std::vector<Pt> &pts) {
        const std::string &pm = t.perimeter;
        bool para = pm == "parallelogramPerimeter", trap = pm == "trapezoidPerimeter", step = pm == "stepPerimeter",
             hex = pm == "hexagonPerimeter2";
        if (!(para || trap || step || hex) || !t.style) return false;
        const Style &s = *t.style;
        bool fixed = str(s, "fixedSize") != "" && str(s, "fixedSize") != "0";
        double size = num(s, "size", fixed ? 20 : hex ? 0.25 : 0.2);
        const std::string &d = str(s, "direction");
        std::string dir = d.empty() ? "east" : d;
        double x = t.b.x, y = t.b.y, w = t.b.w, h = t.b.h, cx = x + w / 2, cy = y + h / 2;
        auto along = [&](double len, double half) { return fixed ? std::max(0.0, std::min(half, size)) : len * std::max(0.0, std::min(1.0, size)); };
        bool vertical = dir == "north" || dir == "south";
        if (para) {
            if (vertical) { double dy = along(h, h); pts = {{x, y}, {x + w, y + dy}, {x + w, y + h}, {x, y + h - dy}, {x, y}}; }
            else { double dx = along(w, w * 0.5); pts = {{x + dx, y}, {x + w, y}, {x + w - dx, y + h}, {x, y + h}, {x + dx, y}}; }
        } else if (trap) {
            if (dir == "east") { double dx = along(w, w * 0.5); pts = {{x + dx, y}, {x + w - dx, y}, {x + w, y + h}, {x, y + h}, {x + dx, y}}; }
            else if (dir == "west") { double dx = along(w, w); pts = {{x, y}, {x + w, y}, {x + w - dx, y + h}, {x + dx, y + h}, {x, y}}; }
            else if (dir == "north") { double dy = along(h, h); pts = {{x, y + dy}, {x + w, y}, {x + w, y + h}, {x, y + h - dy}, {x, y + dy}}; }
            else { double dy = along(h, h); pts = {{x, y}, {x + w, y + dy}, {x + w, y + h - dy}, {x, y + h}, {x, y}}; }
        } else if (step) {
            if (dir == "east") { double dx = along(w, w); pts = {{x, y}, {x + w - dx, y}, {x + w, cy}, {x + w - dx, y + h}, {x, y + h}, {x + dx, cy}, {x, y}}; }
            else if (dir == "west") { double dx = along(w, w); pts = {{x + dx, y}, {x + w, y}, {x + w - dx, cy}, {x + w, y + h}, {x + dx, y + h}, {x, cy}, {x + dx, y}}; }
            else if (dir == "north") { double dy = along(h, h); pts = {{x, y + dy}, {cx, y}, {x + w, y + dy}, {x + w, y + h}, {cx, y + h - dy}, {x, y + h}, {x, y + dy}}; }
            else { double dy = along(h, h); pts = {{x, y}, {cx, y + dy}, {x + w, y}, {x + w, y + h - dy}, {cx, y + h}, {x, y + h - dy}, {x, y}}; }
        } else {
            if (vertical) { double dy = along(h, h); pts = {{cx, y}, {x + w, y + dy}, {x + w, y + h - dy}, {cx, y + h}, {x, y + h - dy}, {x, y + dy}, {cx, y}}; }
            else { double dx = along(w, w); pts = {{x + dx, y}, {x + w - dx, y}, {x + w, cy}, {x + w - dx, y + h}, {x + dx, y + h}, {x, cy}, {x + dx, y}}; }
        }
        return true;
    }
    static Pt uprightPoint(const Term &t, Pt next, bool orthogonal) {
        Box b = t.b;
        if (t.perimeter == "centerPerimeter") return b.centre();   // (draw.io's: a waypoint's edges meet at its centre)
        if (t.perimeter == "orthogonalPerimeter") orthogonal = true;   // (Shapes.js: the rectangle, always orthogonal)
        if (t.style && (t.perimeter == "lifelinePerimeter" || t.perimeter == "backbonePerimeter")) {
            // Shapes.js: on the (lifeline's, backbone's) line in the middle, beside it by half the line width
            const Style &s = *t.style;
            double sw = num(s, "strokeWidth", 1) / 2 - 1, cx = b.x + b.w / 2, cy = b.y + b.h / 2;
            if (t.perimeter == "lifelinePerimeter") {
                double size = num(s, "size", 40), max = b.y + b.h;
                if (str(s, "lifelineMirror") == "1") max -= size;
                if (next.x < cx) sw = -(sw + 1);
                return {cx + sw, std::min(max, std::max(b.y + size, next.y))};
            }
            if (!str(s, "backboneSize").empty()) sw += num(s, "backboneSize", 0) / 2 - 1;
            const std::string &d = str(s, "direction");
            if (d == "south" || d == "north") {
                if (next.x < cx) sw = -(sw + 1);
                return {cx + sw, std::min(b.y + b.h, std::max(b.y, next.y))};
            }
            if (next.y < cy) sw = -(sw + 1);
            return {std::min(b.x + b.w, std::max(b.x, next.x)), cy + sw};
        }
        if (std::vector<Pt> pts; perimeterOutline(t, pts)) return outlinePoint(b, pts, next, orthogonal);
        if (t.perimeter == "calloutPerimeter" && t.style) {   // (Shapes.js: the body, without the pointer)
            const std::string &d = str(*t.style, "direction");
            double size = std::max(0.0, num(*t.style, "size", 30));
            if (d == "south") size = std::min(b.w, size), b.x += size, b.w -= size;
            else if (d == "west") size = std::min(b.h, size), b.y += size, b.h -= size;
            else if (d == "north") b.w -= std::min(b.w, size);
            else b.h -= std::min(b.h, size);
        }
        if (t.perimeter == "trianglePerimeter" && b.w > 0 && b.h > 0) return trianglePoint(t, next, orthogonal);
        if (b.w <= 0 && b.h <= 0) return b.centre();
        double cx = b.x + b.w / 2, cy = b.y + b.h / 2, px = next.x, py = next.y;
        if (t.perimeter == "ellipsePerimeter") {
            double a = b.w / 2, e = b.h / 2, dx = std::trunc(px - cx), dy = std::trunc(py - cy);
            if (dx == 0 && dy != 0) return {cx, cy + e * dy / std::fabs(dy)};
            if (dx == 0 && dy == 0) return {px, py};
            if (orthogonal) {
                if (py >= b.y && py <= b.y + b.h && e > 0) {
                    double ty = py - cy, tx = std::sqrt(std::max(0.0, a * a * (1 - ty * ty / (e * e))));
                    return {px <= b.x ? cx - tx : cx + tx, py};
                }
                if (px >= b.x && px <= b.x + b.w && a > 0) {
                    double tx = px - cx, ty = std::sqrt(std::max(0.0, e * e * (1 - tx * tx / (a * a))));
                    return {px, py <= b.y ? cy - ty : cy + ty};
                }
            }
            double d = dy / dx, h = cy - d * cx, E = a * a * d * d + e * e, f = -2 * cx * E;
            double g = a * a * d * d * cx * cx + e * e * cx * cx - a * a * e * e, det = std::sqrt(std::max(0.0, f * f - 4 * E * g));
            if (E == 0) return b.centre();
            double x1 = (-f + det) / (2 * E), x2 = (-f - det) / (2 * E), y1 = d * x1 + h, y2 = d * x2 + h;
            return std::hypot(x1 - px, y1 - py) < std::hypot(x2 - px, y2 - py) ? Pt{x1, y1} : Pt{x2, y2};
        }
        if (t.perimeter == "rhombusPerimeter") {
            if (cx == px) return {cx, cy > py ? b.y : b.y + b.h};
            if (cy == py) return {cx > px ? b.x : b.x + b.w, cy};
            double tx = cx, ty = cy;
            if (orthogonal) {
                if (px >= b.x && px <= b.x + b.w) tx = px;
                else if (py >= b.y && py <= b.y + b.h) ty = py;
            }
            auto cross = [&](double x1, double y1, double x2, double y2) {   // (px, py)-(tx, ty) with a side of the rhombus
                double x3 = px, y3 = py, x4 = tx, y4 = ty;
                double den = (y4 - y3) * (x2 - x1) - (x4 - x3) * (y2 - y1);
                if (den == 0) return Pt{cx, cy};
                double ua = ((x4 - x3) * (y1 - y3) - (y4 - y3) * (x1 - x3)) / den;
                return Pt{x1 + ua * (x2 - x1), y1 + ua * (y2 - y1)};
            };
            if (px < cx) return py < cy ? cross(cx, b.y, b.x, cy) : cross(cx, b.y + b.h, b.x, cy);
            return py < cy ? cross(cx, b.y, b.x + b.w, cy) : cross(cx, b.y + b.h, b.x + b.w, cy);
        }
        double alpha = std::atan2(py - cy, px - cx), beta = M_PI / 2 - alpha, t2 = std::atan2(b.h, b.w);
        Pt p;
        if (alpha < -M_PI + t2 || alpha > M_PI - t2) p = {b.x, cy - b.w * std::tan(alpha) / 2};
        else if (alpha < -t2) p = {cx - b.h * std::tan(beta) / 2, b.y};
        else if (alpha < t2) p = {b.x + b.w, cy + b.w * std::tan(alpha) / 2};
        else p = {cx + b.h * std::tan(beta) / 2, b.y + b.h};
        if (orthogonal) {
            if (px >= b.x && px <= b.x + b.w) p.x = px;
            else if (py >= b.y && py <= b.y + b.h) p.y = py;
            if (px < b.x) p.x = b.x;
            else if (px > b.x + b.w) p.x = b.x + b.w;
            if (py < b.y) p.y = b.y;
            else if (py > b.y + b.h) p.y = b.y + b.h;
        }
        return p;
    }

    // a connection constraint's point (exitX / exitY, entryX ..., projected on the perimeter
    // unless exitPerimeter=0); false if the style sets none
    static bool constraintPoint(const Style &s, bool source, const Term &t, Pt &p) {
        const char *kx = source ? "exitX" : "entryX", *ky = source ? "exitY" : "entryY";
        const std::string &vx = str(s, kx), &vy = str(s, ky);
        if (t.perimeter == "centerPerimeter") {   // (draw.io: a centre-perimeter vertex's ends (a waypoint's) are fixed at its centre,
            p = t.b.centre();                    // with or without a constraint; a loop on it is no length, not drawn)
            return true;
        }
        if (vx.empty() || vy.empty()) return false;
        double fx = nn_strtod(vx.c_str(), nullptr), fy = nn_strtod(vy.c_str(), nullptr);
        if (!std::isfinite(fx) || !std::isfinite(fy) || std::fabs(fx) > 1e6 || std::fabs(fy) > 1e6) return false;
        // (mxGraph.getConnectionPoint: the point in the shape before its direction turns it, flipped, then turned with it)
        Box e = t.b;
        Pt c = e.centre();
        const Style *ts = t.style;
        const std::string &dir = ts ? str(*ts, "direction") : std::string();
        bool quarter = dir == "north" || dir == "south";
        if (quarter) e = {c.x - e.h / 2, c.y - e.w / 2, e.h, e.w};
        p = {e.x + fx * e.w + num(s, source ? "exitDx" : "entryDx", 0), e.y + fy * e.h + num(s, source ? "exitDy" : "entryDy", 0)};
        if (ts) {
            bool fh = str(*ts, "flipH") == "1", fv = str(*ts, "flipV") == "1";
            if (str(*ts, "shape").compare(0, 8, "stencil(") == 0)
                fh = fh || str(*ts, "stencilFlipH") == "1", fv = fv || str(*ts, "stencilFlipV") == "1";
            if (quarter) std::swap(fh, fv);
            if (fh) p.x = 2 * c.x - p.x;
            if (fv) p.y = 2 * c.y - p.y;
            if (str(*ts, "anchorPointDirection") != "0") {
                double turn = dir == "north" ? 270 : dir == "west" ? 180 : dir == "south" ? 90 : 0;
                if (turn != 0) p = rotated(p, turn, c);
            }
        }
        if (str(s, source ? "exitPerimeter" : "entryPerimeter") != "0") p = uprightPoint(t, p, false);
        if (t.rotation != 0) p = rotated(p, t.rotation, t.b.centre());   // (the point turns with its shape)
        return true;
    }

    static bool contains(const Box &b, double x, double y) { return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h; }

    // mxEdgeStyle.SegmentConnector: segments through the hints, alternately horizontal and
    // vertical
    static void segmentConnector(const Term *source, const Term *target, const Pt *p0, const Pt *pe, std::vector<Pt> hints,
                                 std::vector<Pt> &result) {
        const double tol = 1;
        const Pt *lastPushed = nullptr;   // (a point within 1 of the one before is left out; the first: of a fixed source end,
        Pt lastValue;                     // as draw.io's starts with the result's source slot)
        if (p0) lastValue = *p0, lastPushed = &lastValue;
        auto push = [&](Pt q) {
            if (!lastPushed || std::fabs(lastValue.x - q.x) >= tol || std::fabs(lastValue.y - q.y) >= 1) {
                result.push_back(q);
                lastValue = q;
                lastPushed = &lastValue;
            }
        };
        bool horizontal = true;
        bool haveHint = false;
        Pt hint;
        Pt pt;
        bool havePt = true;
        if (p0) pt = *p0;
        else if (source) pt = source->b.centre();
        else havePt = false;
        if (!hints.empty() && havePt) {
            if (std::fabs(hints[0].x - pt.x) < tol) hints[0].x = pt.x;
            if (std::fabs(hints[0].y - pt.y) < tol) hints[0].y = pt.y;
            if (pe) {
                if (std::fabs(hints.back().x - pe->x) < tol) hints.back().x = pe->x;
                if (std::fabs(hints.back().y - pe->y) < tol) hints.back().y = pe->y;
            }
            hint = hints[0];
            haveHint = true;
            const Term *currentTerm = p0 ? nullptr : source;
            const Pt *currentPt = p0;
            Pt currentHint = hint;
            for (int i = 0; i < 2; i++) {
                bool fixedVert = currentPt && currentPt->x == currentHint.x, fixedHoz = currentPt && currentPt->y == currentHint.y;
                bool inHoz = currentTerm && currentHint.y >= currentTerm->b.y && currentHint.y <= currentTerm->b.y + currentTerm->b.h;
                bool inVert = currentTerm && currentHint.x >= currentTerm->b.x && currentHint.x <= currentTerm->b.x + currentTerm->b.w;
                bool hozChan = fixedHoz || (!currentPt && inHoz), vertChan = fixedVert || (!currentPt && inVert);
                if (!(i == 0 && ((hozChan && vertChan) || (fixedVert && fixedHoz)))) {
                    if (currentPt && !fixedHoz && !fixedVert && (inHoz || inVert)) {
                        horizontal = !inHoz;
                        break;
                    }
                    if (vertChan || hozChan) {
                        horizontal = hozChan;
                        if (i == 1) horizontal = hints.size() % 2 == 0 ? hozChan : vertChan;
                        break;
                    }
                }
                currentTerm = pe ? nullptr : target;
                currentPt = pe;
                currentHint = hints.back();
                if (fixedVert && fixedHoz) hints.erase(hints.begin());
                if (hints.empty()) break;
            }
            if (horizontal && ((p0 && p0->y != hint.y) || (!p0 && source && (hint.y < source->b.y || hint.y > source->b.y + source->b.h))))
                push({pt.x, hint.y});
            else if (!horizontal && ((p0 && p0->x != hint.x) || (!p0 && source && (hint.x < source->b.x || hint.x > source->b.x + source->b.w))))
                push({hint.x, pt.y});
            if (horizontal) pt.y = hint.y;
            else pt.x = hint.x;
            for (const Pt &h : hints) {
                horizontal = !horizontal;
                hint = h;
                if (horizontal) pt.y = hint.y;
                else pt.x = hint.x;
                push(pt);
            }
        } else if (havePt) {
            hint = pt;
            haveHint = true;
            horizontal = true;
        }
        bool haveEnd = true;
        if (pe) pt = *pe;
        else if (target) pt = target->b.centre();
        else haveEnd = false;
        if (haveEnd && haveHint) {
            if (horizontal && ((pe && pe->y != hint.y) || (!pe && target && (hint.y < target->b.y || hint.y > target->b.y + target->b.h))))
                push({pt.x, hint.y});
            else if (!horizontal && ((pe && pe->x != hint.x) || (!pe && target && (hint.x < target->b.x || hint.x > target->b.x + target->b.w))))
                push({hint.x, pt.y});
        }
        // (bends inside the source or target of a floating end; mxGraph's result starts with the source's slot, so its
        // result[1] and "length > 1" are this one's result[0] and "not empty")
        if (!p0 && source)
            while (!result.empty() && contains(source->b, result[0].x, result[0].y)) result.erase(result.begin());
        if (!pe && target)
            while (!result.empty() && contains(target->b, result.back().x, result.back().y)) result.pop_back();
        if (!hints.empty() && pe && !result.empty() && std::fabs(pe->x - result.back().x) <= tol && std::fabs(pe->y - result.back().y) <= tol) {
            result.pop_back();
            if (!result.empty()) {
                if (std::fabs(result.back().x - pe->x) < tol) result.back().x = pe->x;
                if (std::fabs(result.back().y - pe->y) < tol) result.back().y = pe->y;
            }
        }
    }

    // mxEdgeStyle.Loop: an edge from a vertex to itself, out and back beside it (by the edge's direction:
    // west, the default, on the right; or through the first hint)
    static void loop(const Term *source, const Pt *p0, const Pt *pe, const std::vector<Pt> &hints, const Style &s, std::vector<Pt> &result) {
        if (p0 && pe) {
            result = hints;
            return;
        }
        if (!source) return;
        const Box &b = source->b;
        const Pt *pt = hints.empty() || contains(b, hints[0].x, hints[0].y) ? nullptr : &hints[0];
        const std::string &dir = str(s, "direction");
        double x = 0, dx = 0, y = 0, dy = 0, seg = num(s, "segment", 10);
        if (dir == "north" || dir == "south") x = b.x + b.w / 2, dx = seg;
        else y = b.y + b.h / 2, dy = seg;
        if (!pt || pt->x < b.x || pt->x > b.x + b.w) {
            if (pt) x = pt->x, dy = std::max(std::fabs(y - pt->y), dy);
            else if (dir == "north") y = b.y - 2 * dx;
            else if (dir == "south") y = b.y + b.h + 2 * dx;
            else if (dir == "east") x = b.x - 2 * dy;
            else x = b.x + b.w + 2 * dy;
        } else {
            x = b.x + b.w / 2, dx = std::max(std::fabs(x - pt->x), dy), y = pt->y, dy = 0;
        }
        result.push_back({x - dx, y - dy});
        result.push_back({x + dx, y + dy});
    }

    // mxEdgeStyle.SideToSide / TopToBottom: one elbow in the middle (or at the hint)
    // Graph.js mxEdgeStyle.IsometricConnector: from the source (its centre) to the hint (else halfway) and on to the target
    // along the isometric axes (-30 and -150 degrees); elbow=vertical: the other axis first
    static void isometricConnector(const Term *source, const Term *target, const Pt *p0, const Pt *pe, const std::vector<Pt> &hints,
                                   const Style &s, std::vector<Pt> &result) {
        if ((!source && !p0) || (!target && !pe)) return;
        Pt a = p0 ? *p0 : source->b.centre(), e = pe ? *pe : target->b.centre();
        Pt pt = hints.empty() ? Pt{a.x + (e.x - a.x) / 2, a.y + (e.y - a.y) / 2} : hints[0];
        const double hx = std::cos(-M_PI / 6), hy = std::sin(-M_PI / 6), vx = std::cos(-5 * M_PI / 6), vy = std::sin(-5 * M_PI / 6);
        bool horizontal = str(s, "elbow") != "vertical";
        Pt cur = a;
        auto step = [&](Pt to, bool both) {
            double dx = to.x - cur.x, dy = to.y - cur.y;
            double ka = (vy * dx - vx * dy) / (hx * vy - hy * vx), ke = (hy * dx - hx * dy) / (hy * vx - hx * vy);
            if (horizontal) {
                if (both) cur = {cur.x + hx * ka, cur.y + hy * ka}, result.push_back(cur);
                cur = {cur.x + vx * ke, cur.y + vy * ke};
            } else {
                if (both) cur = {cur.x + vx * ke, cur.y + vy * ke}, result.push_back(cur);
                cur = {cur.x + hx * ka, cur.y + hy * ka};
            }
            result.push_back(cur);
        };
        step(pt, true);
        step(e, false);   // (its last point: along one axis only; the target's perimeter towards it ends the edge)
    }

    static void sideToSide(const Term *source, const Term *target, const Pt *p0, const Pt *pe, const std::vector<Pt> &hints, std::vector<Pt> &result) {
        Box s = source ? source->b : Box{}, t = target ? target->b : Box{};
        if (p0) s = {p0->x, p0->y, 0, 0};
        if (pe) t = {pe->x, pe->y, 0, 0};
        if ((!source && !p0) || (!target && !pe)) return;
        const Pt *pt = hints.empty() ? nullptr : &hints[0];
        double l = std::max(s.x, t.x), r = std::min(s.x + s.w, t.x + t.w);
        double x = pt ? pt->x : std::round(r + (l - r) / 2);
        double y1 = s.y + s.h / 2, y2 = t.y + t.h / 2;
        if (pt) {
            if (pt->y >= s.y && pt->y <= s.y + s.h) y1 = pt->y;
            if (pt->y >= t.y && pt->y <= t.y + t.h) y2 = pt->y;
        }
        if (!contains(t, x, y1) && !contains(s, x, y1)) result.push_back({x, y1});
        if (!contains(t, x, y2) && !contains(s, x, y2)) result.push_back({x, y2});
        if (result.empty()) {   // (mxGraph's result starts with the source's slot: its "1 == length" means no point yet)
            if (pt) {
                if (!contains(t, x, pt->y) && !contains(s, x, pt->y)) result.push_back({x, pt->y});
            } else {
                double tt = std::max(s.y, t.y), bb = std::min(s.y + s.h, t.y + t.h);
                result.push_back({x, tt + (bb - tt) / 2});
            }
        }
    }
    static void topToBottom(const Term *source, const Term *target, const Pt *p0, const Pt *pe, const std::vector<Pt> &hints, std::vector<Pt> &result) {
        Box s = source ? source->b : Box{}, t = target ? target->b : Box{};
        if (p0) s = {p0->x, p0->y, 0, 0};
        if (pe) t = {pe->x, pe->y, 0, 0};
        if ((!source && !p0) || (!target && !pe)) return;
        const Pt *pt = hints.empty() ? nullptr : &hints[0];
        double tt = std::max(s.y, t.y), bb = std::min(s.y + s.h, t.y + t.h);
        double x = s.x + s.w / 2;
        if (pt && pt->x >= s.x && pt->x <= s.x + s.w) x = pt->x;
        double y = pt ? pt->y : std::round(bb + (tt - bb) / 2);
        if (!contains(t, x, y) && !contains(s, x, y)) result.push_back({x, y});
        x = pt && pt->x >= t.x && pt->x <= t.x + t.w ? pt->x : t.x + t.w / 2;
        if (!contains(t, x, y) && !contains(s, x, y)) result.push_back({x, y});
        if (result.empty()) {   // (mxGraph's result starts with the source's slot: its "1 == length" means no point yet)
            if (pt) {
                if (!contains(t, pt->x, y) && !contains(s, pt->x, y)) result.push_back({pt->x, y});
            } else {
                double l = std::max(s.x, t.x), r = std::min(s.x + s.w, t.x + t.w);
                result.push_back({l + (r - l) / 2, y});
            }
        }
    }
    static void elbowConnector(const Term *source, const Term *target, const Pt *p0, const Pt *pe, const std::vector<Pt> &hints, const Style &s,
                               std::vector<Pt> &result) {
        bool vertical = false, horizontal = false;
        if (source && target) {
            const Box &a = source->b, &b = target->b;
            if (!hints.empty()) {
                const Pt &pt = hints[0];
                double left = std::min(a.x, b.x), right = std::max(a.x + a.w, b.x + b.w), top = std::min(a.y, b.y), bottom = std::max(a.y + a.h, b.y + b.h);
                vertical = pt.y < top || pt.y > bottom;
                horizontal = pt.x < left || pt.x > right;
            } else {
                double left = std::max(a.x, b.x), right = std::min(a.x + a.w, b.x + b.w);
                vertical = left == right;
                if (!vertical) {
                    double top = std::max(a.y, b.y), bottom = std::min(a.y + a.h, b.y + b.h);
                    horizontal = top == bottom;
                }
            }
        }
        if (!horizontal && (vertical || str(s, "elbow") == "vertical")) topToBottom(source, target, p0, pe, hints, result);
        else sideToSide(source, target, p0, pe, hints, result);
    }

    // mxEdgeStyle.OrthConnector: a local orthogonal router between the terminals by the route
    // patterns of the sides the edge leaves and enters; hints, or ends too close for the jetties:
    // the SegmentConnector
    enum { DIR_W = 1, DIR_N = 2, DIR_S = 4, DIR_E = 8, DIR_ALL = 15 };
    static int reversePort(int c) { return (c & DIR_W) << 3 | (c & DIR_N) << 1 | (c & DIR_S) >> 1 | (c & DIR_E) >> 3; }
    // mxEdgeStyle.EntityRelation: out of the sides segment (30) across, at the terminals' middle heights; both on one
    // side: round it; else through the middle when the sides face away from each other
    static void entityRelation(const Term *source, const Term *target, const Pt *p0, const Pt *pe, const Style &s, std::vector<Pt> &result) {
        if ((!source && !p0) || (!target && !pe)) return;
        double seg = num(s, "segment", 30);
        Box sb = p0 ? Box{p0->x, p0->y, 0, 0} : source->b, tb = pe ? Box{pe->x, pe->y, 0, 0} : target->b;
        bool sl = false, tl = true;
        if (source && target) sl = (pe ? pe->x : target->b.x + target->b.w) < (p0 ? p0->x : source->b.x);
        if (!p0 && source) {
            int pc = portConstraint(s, source, true);
            if (pc == DIR_W || pc == DIR_E) sl = pc == DIR_W;
        }
        if (target && source) tl = (p0 ? p0->x : source->b.x + source->b.w) < (pe ? pe->x : target->b.x);
        if (!pe && target) {
            int pc = portConstraint(s, target, false);
            if (pc == DIR_W || pc == DIR_E) tl = pc == DIR_W;
        }
        auto midY = [&](const Term *t, const Box &b) { return b.y + b.h / 2 + (t && t->style ? num(*t->style, "routingCenterY", 0) * b.h : 0); };
        double x0 = sl ? sb.x : sb.x + sb.w, y0 = midY(p0 ? nullptr : source, sb);
        double xe = tl ? tb.x : tb.x + tb.w, ye = midY(pe ? nullptr : target, tb);
        Pt dep{x0 + (sl ? -seg : seg), y0}, arr{xe + (tl ? -seg : seg), ye};
        if (sl == tl) {
            double x = sl ? std::min(x0, xe) - seg : std::max(x0, xe) + seg;
            result.push_back({x, y0}), result.push_back({x, ye});
            return;
        }
        result.push_back(dep);
        if ((dep.x < arr.x) == sl) {
            double my = y0 + (ye - y0) / 2;
            result.push_back({dep.x, my}), result.push_back({arr.x, my});
        }
        result.push_back(arr);
    }

    static int portConstraint(const Style &edge, const Term *t, bool source) {
        std::string v = str(edge, source ? "sourcePortConstraint" : "targetPortConstraint");
        if (v.empty() && t && t->style) v = str(*t->style, "portConstraint");
        if (v.empty()) return DIR_ALL;
        int m = 0;
        if (v.find("north") != std::string::npos) m |= DIR_N;
        if (v.find("south") != std::string::npos) m |= DIR_S;
        if (v.find("east") != std::string::npos) m |= DIR_E;
        if (v.find("west") != std::string::npos) m |= DIR_W;
        return m ? m : DIR_ALL;
    }
    static double jetty(const Style &s, bool source) {
        std::string v = str(s, source ? "sourceJettySize" : "targetJettySize");
        if (v.empty()) v = str(s, "jettySize");
        if (v.empty()) return 10;
        if (v == "auto") {
            const std::string &type = str(s, source ? "startArrow" : "endArrow");
            if (!type.empty() && type != "none") {
                double size = num(s, source ? "startSize" : "endSize", 6);
                return std::max(2.0, std::ceil((size + 10) / 10)) * 10;
            }
            return 20;
        }
        double d = nn_strtod(v.c_str(), nullptr);
        return std::isfinite(d) && std::fabs(d) < 1e6 ? d : 10;
    }
    static void orthConnector(const Term *source, const Term *target, const Pt *p0, const Pt *pe, const std::vector<Pt> &hints, const Style &s,
                              std::vector<Pt> &result) {
        static const std::vector<int> routePatterns[4][4] = {
            {{513, 2308, 2081, 2562}, {513, 1090, 514, 2184, 2114, 2561}, {513, 1090, 514, 2564, 2184, 2562}, {513, 2308, 2561, 1090, 514, 2568, 2308}},
            {{514, 1057, 513, 2308, 2081, 2562}, {514, 2184, 2114, 2561}, {514, 2184, 2562, 1057, 513, 2564, 2184}, {514, 1057, 513, 2568, 2308, 2561}},
            {{1090, 514, 1057, 513, 2308, 2081, 2562}, {2114, 2561}, {1090, 2562, 1057, 513, 2564, 2184}, {1090, 514, 1057, 513, 2308, 2561, 2568}},
            {{2081, 2562}, {1057, 513, 1090, 514, 2184, 2114, 2561}, {1057, 513, 1090, 514, 2184, 2562, 2564}, {1057, 2561, 1090, 514, 2568, 2308}}};
        static const int dirVectors[7][2] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, 0}};
        double sourceBuffer = jetty(s, true), targetBuffer = jetty(s, false);
        if (source && target == source) sourceBuffer = targetBuffer = std::max(sourceBuffer, targetBuffer);
        double totalBuffer = sourceBuffer + targetBuffer;
        bool tooShort = p0 && pe && (pe->x - p0->x) * (pe->x - p0->x) + (pe->y - p0->y) * (pe->y - p0->y) < totalBuffer * totalBuffer;
        if (tooShort || !hints.empty()) {
            segmentConnector(source, target, p0, pe, hints, result);
            return;
        }
        if ((!source && !p0) || (!target && !pe)) return;
        int pc[2] = {source ? portConstraint(s, source, true) : DIR_ALL, target ? portConstraint(s, target, false) : DIR_ALL};
        double geo[2][4] = {{source ? source->b.x : p0->x, source ? source->b.y : p0->y, source ? source->b.w : 0, source ? source->b.h : 0},
                            {target ? target->b.x : pe->x, target ? target->b.y : pe->y, target ? target->b.w : 0, target ? target->b.h : 0}};
        double buffer[2] = {sourceBuffer, targetBuffer};
        double limits[2][9] = {};
        for (int i = 0; i < 2; i++) {
            limits[i][1] = geo[i][0] - buffer[i];
            limits[i][2] = geo[i][1] - buffer[i];
            limits[i][4] = geo[i][0] + geo[i][2] + buffer[i];
            limits[i][8] = geo[i][1] + geo[i][3] + buffer[i];
        }
        double dx = geo[0][0] + geo[0][2] / 2 - (geo[1][0] + geo[1][2] / 2), dy = geo[0][1] + geo[0][3] / 2 - (geo[1][1] + geo[1][3] / 2);
        int quad = 0;
        if (dx < 0) quad = dy < 0 ? 2 : 1;
        else if (dy <= 0) quad = dx == 0 ? 2 : 3;
        int dir[2] = {0, 0};
        double constraint[2][2] = {{0.5, 0.5}, {0.5, 0.5}};
        for (int i = 0; i < 2; i++) {
            const Pt *term = i == 0 ? (source ? p0 : nullptr) : (target ? pe : nullptr);
            if (!term) continue;
            constraint[i][0] = geo[i][2] != 0 ? (term->x - geo[i][0]) / geo[i][2] : 0.5;
            if (std::fabs(term->x - geo[i][0]) <= 1) dir[i] = DIR_W;
            else if (std::fabs(term->x - geo[i][0] - geo[i][2]) <= 1) dir[i] = DIR_E;
            constraint[i][1] = geo[i][3] != 0 ? (term->y - geo[i][1]) / geo[i][3] : 0.5;
            if (std::fabs(term->y - geo[i][1]) <= 1) dir[i] = DIR_N;
            else if (std::fabs(term->y - geo[i][1] - geo[i][3]) <= 1) dir[i] = DIR_S;
        }
        double sourceTopDist = geo[0][1] - (geo[1][1] + geo[1][3]), sourceLeftDist = geo[0][0] - (geo[1][0] + geo[1][2]);
        double sourceBottomDist = geo[1][1] - (geo[0][1] + geo[0][3]), sourceRightDist = geo[1][0] - (geo[0][0] + geo[0][2]);
        double seps[5] = {0, std::max(sourceLeftDist - totalBuffer, 0.0), std::max(sourceTopDist - totalBuffer, 0.0),
                          std::max(sourceRightDist - totalBuffer, 0.0), std::max(sourceBottomDist - totalBuffer, 0.0)};
        int horPref[2], vertPref[2];
        horPref[0] = sourceLeftDist >= sourceRightDist ? DIR_W : DIR_E;
        vertPref[0] = sourceTopDist >= sourceBottomDist ? DIR_N : DIR_S;
        horPref[1] = reversePort(horPref[0]);
        vertPref[1] = reversePort(vertPref[0]);
        double prefH = std::max(sourceLeftDist, sourceRightDist), prefV = std::max(sourceTopDist, sourceBottomDist);
        int pref[2][2] = {{0, 0}, {0, 0}};
        bool prefSet = false;
        for (int i = 0; i < 2; i++) {
            if (dir[i]) continue;
            if (!(horPref[i] & pc[i])) horPref[i] = reversePort(horPref[i]);
            if (!(vertPref[i] & pc[i])) vertPref[i] = reversePort(vertPref[i]);
            pref[i][0] = vertPref[i];
            pref[i][1] = horPref[i];
        }
        if (prefV > 0 && prefH > 0) {
            if ((horPref[0] & pc[0]) && (vertPref[1] & pc[1])) {
                pref[0][0] = horPref[0], pref[0][1] = vertPref[0], pref[1][0] = vertPref[1], pref[1][1] = horPref[1];
                prefSet = true;
            } else if ((vertPref[0] & pc[0]) && (horPref[1] & pc[1])) {
                pref[0][0] = vertPref[0], pref[0][1] = horPref[0], pref[1][0] = horPref[1], pref[1][1] = vertPref[1];
                prefSet = true;
            }
        }
        if (prefV > 0 && !prefSet) {
            pref[0][0] = vertPref[0], pref[0][1] = horPref[0], pref[1][0] = vertPref[1], pref[1][1] = horPref[1];
            prefSet = true;
        }
        if (prefH > 0 && !prefSet) {
            pref[0][0] = horPref[0], pref[0][1] = vertPref[0], pref[1][0] = horPref[1], pref[1][1] = vertPref[1];
            prefSet = true;
        }
        for (int i = 0; i < 2; i++) {
            if (dir[i]) continue;
            if (!(pref[i][0] & pc[i])) pref[i][0] = pref[i][1];
            int d = pref[i][0] & pc[i];
            d |= (pref[i][1] & pc[i]) << 8;
            d |= (pref[1 - i][i] & pc[i]) << 16;
            d |= (pref[1 - i][1 - i] & pc[i]) << 24;
            if (!(d & 0xF)) d = d << 8;
            if (!(d & 0xF00)) d = (d & 0xF) | d >> 8;
            if (!(d & 0xF0000)) d = (d & 0xFFFF) | ((d & 0xF000000) >> 8);
            dir[i] = d & 0xF;
            if (pc[i] == DIR_W || pc[i] == DIR_N || pc[i] == DIR_E || pc[i] == DIR_S) dir[i] = pc[i];
        }
        int sourceIndex = (dir[0] == DIR_E ? 3 : dir[0]) - quad, targetIndex = (dir[1] == DIR_E ? 3 : dir[1]) - quad;
        if (sourceIndex < 1) sourceIndex += 4;
        if (targetIndex < 1) targetIndex += 4;
        if (sourceIndex < 1 || sourceIndex > 4 || targetIndex < 1 || targetIndex > 4) return;
        const std::vector<int> &route = routePatterns[sourceIndex - 1][targetIndex - 1];
        double wp[12][2] = {};
        wp[0][0] = geo[0][0], wp[0][1] = geo[0][1];
        switch (dir[0]) {
        case DIR_W: wp[0][0] -= sourceBuffer, wp[0][1] += constraint[0][1] * geo[0][3]; break;
        case DIR_S: wp[0][0] += constraint[0][0] * geo[0][2], wp[0][1] += geo[0][3] + sourceBuffer; break;
        case DIR_E: wp[0][0] += geo[0][2] + sourceBuffer, wp[0][1] += constraint[0][1] * geo[0][3]; break;
        case DIR_N: wp[0][0] += constraint[0][0] * geo[0][2], wp[0][1] -= sourceBuffer; break;
        }
        int currentIndex = 0;
        int lastOrientation = (dir[0] & (DIR_E | DIR_W)) ? 0 : 1, initialOrientation = lastOrientation;
        for (int code : route) {
            int nextDirection = code & 0xF;
            int directionIndex = (nextDirection == DIR_E ? 3 : nextDirection) + quad;
            if (directionIndex > 4) directionIndex -= 4;
            if (directionIndex < 1 || directionIndex > 4) return;
            const int *direction = dirVectors[directionIndex - 1];
            int currentOrientation = directionIndex % 2 > 0 ? 0 : 1;
            if (currentOrientation != lastOrientation) {
                if (++currentIndex >= 12) return;
                wp[currentIndex][0] = wp[currentIndex - 1][0];
                wp[currentIndex][1] = wp[currentIndex - 1][1];
            }
            bool tar = code & 2048, sou = code & 1024;
            int side = ((code & 480) >> 5) << quad;
            if (side > 0xF) side >>= 4;
            bool center = code & 512;
            if ((sou || tar) && side < 9) {
                int st = sou ? 0 : 1;
                double limit = center && currentOrientation == 0 ? geo[st][0] + constraint[st][0] * geo[st][2]
                               : center                          ? geo[st][1] + constraint[st][1] * geo[st][3]
                                                                 : limits[st][side];
                if (currentOrientation == 0) {
                    double delta = (limit - wp[currentIndex][0]) * direction[0];
                    if (delta > 0) wp[currentIndex][0] += direction[0] * delta;
                } else {
                    double delta = (limit - wp[currentIndex][1]) * direction[1];
                    if (delta > 0) wp[currentIndex][1] += direction[1] * delta;
                }
            } else if (center) {
                wp[currentIndex][0] += direction[0] * std::fabs(seps[directionIndex] / 2);
                wp[currentIndex][1] += direction[1] * std::fabs(seps[directionIndex] / 2);
            }
            if (currentIndex > 0 && wp[currentIndex][currentOrientation] == wp[currentIndex - 1][currentOrientation]) currentIndex--;
            else lastOrientation = currentOrientation;
        }
        for (int i = 0; i <= currentIndex; i++) {
            if (i == currentIndex) {   // (the last point: only if the turns fit the ends' orientations)
                int targetOrientation = (dir[1] & (DIR_E | DIR_W)) ? 0 : 1;
                int sameOrient = targetOrientation == initialOrientation ? 0 : 1;
                if (sameOrient != (currentIndex + 1) % 2) break;
            }
            Pt q{wp[i][0], wp[i][1]};
            if (result.empty() || result.back().x != q.x || result.back().y != q.y) result.push_back(q);
        }
    }

    // mxGraphView.getVisibleTerminal: the terminal, or the ancestor that is shown in its place (the topmost
    // collapsed one; an invisible terminal's first ancestor; a layer there: no vertex, the end left loose)
    Cell *visibleTerminal(Cell *t) {
        Cell *best = t;
        size_t n = 0;
        for (Cell *r = t; r && n++ <= cells.size(); r = find(r->parent))
            if (!best->visible || r->collapsed) best = r;
        return best;
    }

    void edge(Cell &c) {
        const Style &s = c.style;
        Pt o = origin(c.parent);
        std::vector<Pt> hints;
        for (Pt p : c.points) hints.push_back({o.x + p.x, o.y + p.y});
        Cell *src = visibleTerminal(find(c.source)), *dst = visibleTerminal(find(c.target));
        if (src && !src->vertex) src = nullptr;   // (an edge ending on an edge: at its point, if any)
        if (src) {   // (mxGraphView.getTerminalPort: sourcePort / targetPort name a port cell that takes the end)
            Cell *port = find(str(s, "sourcePort"));
            if (port && port->vertex && shown(*port)) src = port;
        }
        if (dst) {
            Cell *port = find(str(s, "targetPort"));
            if (port && port->vertex && shown(*port)) dst = port;
        }
        if (dst && !dst->vertex) dst = nullptr;
        Term st, tt;
        if (src) st = term(absBox(*src), src->style);
        if (dst) tt = term(absBox(*dst), dst->style);
        const Term *source = src ? &st : nullptr, *target = dst ? (dst == src ? &st : &tt) : nullptr;   // (a loop: one terminal)
        if (!source && !c.hasSource) return;
        if (!target && !c.hasTarget) return;
        // the fixed ends
        Pt sp, tp;
        bool haveS = false, haveT = false;
        if (source) haveS = constraintPoint(s, true, st, sp);
        else sp = {o.x + c.sourcePoint.x, o.y + c.sourcePoint.y}, haveS = true;
        if (target) haveT = constraintPoint(s, false, tt, tp);
        else tp = {o.x + c.targetPoint.x, o.y + c.targetPoint.y}, haveT = true;
        // the route between them
        static const std::string noStyle;
        const std::string &es = str(s, "noEdgeStyle") == "1" ? noStyle : str(s, "edgeStyle");   // (noEdgeStyle: through its points)
        std::vector<Pt> mid;
        bool orth = true;
        const Pt *p0 = haveS ? &sp : nullptr, *pe = haveT ? &tp : nullptr;
        // (a loop's own style, as mxGraphView takes it: fewer than two points, and no fixed end that orthogonalLoop routes)
        bool isLoop = source && target == source && c.points.size() < 2 && !(str(s, "orthogonalLoop") == "1" && (haveS || haveT));
        if (isLoop) loop(source, p0, pe, hints, s, mid), orth = str(s, "orthogonal") == "1";
        else if (es == "orthogonalEdgeStyle") orthConnector(source, target, p0, pe, hints, s, mid);
        else if (es == "elbowEdgeStyle") elbowConnector(source, target, p0, pe, hints, s, mid);
        else if (es == "sideToSideEdgeStyle") sideToSide(source, target, p0, pe, hints, mid);
        else if (es == "entityRelationEdgeStyle") entityRelation(source, target, p0, pe, s, mid);
        else if (es == "topToBottomEdgeStyle") topToBottom(source, target, p0, pe, hints, mid);
        else if (es == "segmentEdgeStyle") segmentConnector(source, target, p0, pe, hints, mid);
        else if (es == "isometricEdgeStyle") isometricConnector(source, target, p0, pe, hints, s, mid), orth = false;
        else mid = hints, orth = str(s, "orthogonal") == "1";
        // the floating ends: the target first, towards its neighbour (the other end, or its centre); on the perimeter
        // grown by perimeterSpacing and the end's own source / targetPerimeterSpacing
        bool fixedS = haveS, fixedT = haveT;
        auto grown = [&](Term t, bool source) {
            double g = num(s, "perimeterSpacing", 0) + num(s, source ? "sourcePerimeterSpacing" : "targetPerimeterSpacing", 0);
            if (g != 0 && std::isfinite(g)) t.b = {t.b.x - g, t.b.y - g, std::max(0.0, t.b.w + 2 * g), std::max(0.0, t.b.h + 2 * g)};
            return t;
        };
        if (!haveT && target) {
            Pt next = !mid.empty() ? mid.back() : haveS ? sp : source ? source->b.centre() : tt.b.centre();
            tp = perimeterPoint(grown(tt, false), next, orth);
            haveT = true;
        }
        if (!haveS && source) {
            Pt next = !mid.empty() ? mid.front() : tp;
            sp = perimeterPoint(grown(st, true), next, orth);
            haveS = true;
        }
        if (str(s, "fixedPointSpacing") == "1") {   // (mxGraphView.updateFixedPointSpacing: a fixed end moved its spacing towards
            auto shift = [&](Pt &e, Pt next, double g) {   // its neighbour; negative: back into the shape)
                double d = std::hypot(next.x - e.x, next.y - e.y);
                g = std::min(g, std::max(0.0, d - 1));
                if (d > 0 && g != 0) e = {e.x + (next.x - e.x) * g / d, e.y + (next.y - e.y) * g / d};
            };
            if (fixedS && source) shift(sp, !mid.empty() ? mid.front() : tp, num(s, "sourcePerimeterSpacing", 0));
            if (fixedT && target) shift(tp, !mid.empty() ? mid.back() : sp, num(s, "targetPerimeterSpacing", 0));
        }
        std::vector<Pt> r{sp};
        for (Pt p : mid)
            if (std::fabs(p.x - r.back().x) > 1e-6 || std::fabs(p.y - r.back().y) > 1e-6) r.push_back(p);
        if (std::fabs(tp.x - r.back().x) > 1e-6 || std::fabs(tp.y - r.back().y) > 1e-6) r.push_back(tp);
        if (r.size() < 2) return;   // (no length: nothing drawn)
        c.route = r;
        drawEdge(c, r);
    }

    // a marker at an end of an edge (its tip at p, pointing along u away from the line); how far
    // the line stops short of the tip
    double marker(const std::string &type, bool filled, double size, double sw, Pt p, Pt u, uint32_t colour) {
        if (type.empty() || type == "none") return 0;
        double len = size + sw, half = len / 2;
        bool thin = type.size() > 4 && type.compare(type.size() - 4, 4, "Thin") == 0;
        std::string t = thin ? type.substr(0, type.size() - 4) : type;
        if (thin) half = len / 3;
        Pt n{-u.y, u.x};
        // (the tip set back by the stroke's miter, as mxMarker does: the outlined tip just touches the terminal)
        double set = t == "classic" || t == "block" || t == "open" || t == "async" || t == "doubleBlock" ? sw * 1.118 : t == "diamond" ? sw * 0.7071 : 0;
        Pt tip = p;
        p = {p.x - u.x * set, p.y - u.y * set};
        Pt back{p.x - u.x * len, p.y - u.y * len};
        PathP path = newPath();
        plutovg_path_t *q = path.get();
        double retreat = 0;
        bool fill = filled;
        auto M = [&](Pt a) { plutovg_path_move_to(q, (float)a.x, (float)a.y); };
        auto L = [&](Pt a) { plutovg_path_line_to(q, (float)a.x, (float)a.y); };
        if (t == "open") {
            M({back.x + n.x * half, back.y + n.y * half});
            L(p);
            L({back.x - n.x * half, back.y - n.y * half});
            fill = false;
            retreat = sw / 2;
        } else if (t == "oval") {
            plutovg_path_add_circle(q, (float)(p.x - u.x * len / 2), (float)(p.y - u.y * len / 2), (float)(len / 2));
            retreat = len / 2;
        } else if (t == "diamond") {
            Pt mid{p.x - u.x * len / 2, p.y - u.y * len / 2};
            M(p);
            L({mid.x + n.x * half, mid.y + n.y * half});
            L(back);
            L({mid.x - n.x * half, mid.y - n.y * half});
            plutovg_path_close(q);
            retreat = len;
        } else if (t == "ERone" || t == "ERmandOne" || t == "ERmany" || t == "ERoneToMany" || t == "ERzeroToMany" || t == "ERzeroToOne") {
            // (draw.io's ER markers: ticks across, a crow's foot, a circle; m along the edge, size + stroke + 1 long)
            double k = size + sw + 1;
            Pt m{u.x * k, u.y * k};
            auto tick = [&](double back) {   // (a line across, back * L before the tip)
                M({tip.x - m.x * back - m.y / 2, tip.y - m.y * back + m.x / 2});
                L({tip.x - m.x * back + m.y / 2, tip.y - m.y * back - m.x / 2});
            };
            auto foot = [&]() {
                M({tip.x + m.y / 2, tip.y - m.x / 2});
                L({tip.x - m.x, tip.y - m.y});
                L({tip.x - m.y / 2, tip.y + m.x / 2});
            };
            fill = false;
            bool zero = t == "ERzeroToMany" || t == "ERzeroToOne";
            if (t == "ERone") tick(0.5);
            else if (t == "ERmandOne") tick(0.5), tick(1);
            else if (t == "ERmany") foot();
            else if (t == "ERoneToMany") tick(1), foot();
            else if (zero) {
                double r = size / 2;
                Pt c{tip.x - m.x * 1.5, tip.y - m.y * 1.5};
                Item ring;
                ring.path = newPath();
                plutovg_path_add_circle(ring.path.get(), (float)c.x, (float)c.y, (float)r);
                ring.stroke = colour, ring.width = (float)sw;
                if (filled) ring.fill = 0xffffffff;
                add(std::move(ring), (float)sw);
                if (t == "ERzeroToMany") foot();
                else tick(0.5);
                if (!filled) {   // (the line stops before the circle, the marker draws the rest to the tip)
                    M({tip.x - m.x - u.x * sw / 2, tip.y - m.y - u.y * sw / 2});
                    L(tip);
                    retreat = 2 * k - sw / 2;
                }
            }
        } else if (t == "circle" || t == "circlePlus") {   // (Graph.js: a circle size + stroke in radius behind the tip; plus: a cross in it)
            double r = len;
            Pt c{tip.x - u.x * (r + sw), tip.y - u.y * (r + sw)};
            plutovg_path_add_circle(q, (float)c.x, (float)c.y, (float)r);
            if (t == "circlePlus") {
                Item plus;
                plus.path = newPath();
                plutovg_path_t *pq = plus.path.get();
                plutovg_path_move_to(pq, (float)(c.x - u.x * r), (float)(c.y - u.y * r));
                plutovg_path_line_to(pq, (float)(c.x + u.x * r), (float)(c.y + u.y * r));
                plutovg_path_move_to(pq, (float)(c.x - n.x * r), (float)(c.y - n.y * r));
                plutovg_path_line_to(pq, (float)(c.x + n.x * r), (float)(c.y + n.y * r));
                plus.stroke = filled ? 0xffffffff : colour, plus.width = (float)sw;
                Item ring;
                ring.path = path;
                ring.stroke = colour, ring.width = (float)sw;
                if (filled) ring.fill = colour;
                add(std::move(ring), (float)sw);
                add(std::move(plus), (float)sw);
                return 2 * r + sw;
            }
            retreat = 2 * r + sw;
            if (!fill) {
                Item it;
                it.path = path, it.stroke = colour, it.width = (float)sw;
                add(std::move(it), (float)sw);
                return retreat;
            }
        } else if (t == "halfCircle") {   // (an arc open towards the tip)
            double d = size + sw + 1;
            Pt c{tip.x - u.x * d, tip.y - u.y * d};
            M({tip.x + n.x * d, tip.y + n.y * d});
            plutovg_path_quad_to(q, (float)(c.x + n.x * d), (float)(c.y + n.y * d), (float)c.x, (float)c.y);
            plutovg_path_quad_to(q, (float)(c.x - n.x * d), (float)(c.y - n.y * d), (float)(tip.x - n.x * d), (float)(tip.y - n.y * d));
            fill = false;
            retreat = d;
            Item it;
            it.path = path, it.stroke = colour, it.width = (float)sw;
            add(std::move(it), (float)sw);
            return retreat;
        } else if (t == "box") {   // (a square size + stroke + 1 behind the tip)
            double d = size + sw + 1;
            M({tip.x + n.x * d / 2, tip.y + n.y * d / 2});
            L({tip.x - n.x * d / 2, tip.y - n.y * d / 2});
            L({tip.x - u.x * d - n.x * d / 2, tip.y - u.y * d - n.y * d / 2});
            L({tip.x - u.x * d + n.x * d / 2, tip.y - u.y * d + n.y * d / 2});
            plutovg_path_close(q);
            retreat = d;
            if (!fill) {
                Item it;
                it.path = path, it.stroke = colour, it.width = (float)sw;
                add(std::move(it), (float)sw);
                return retreat;
            }
        } else if (t == "baseDash") {   // (a line across the tip)
            double d = size + sw + 1;
            M({tip.x + n.x * d / 2, tip.y + n.y * d / 2});
            L({tip.x - n.x * d / 2, tip.y - n.y * d / 2});
            fill = false;
        } else if (t == "async" || t == "openAsync") {   // (half an arrowhead, on the left of the line)
            Pt a0 = t == "async" ? p : tip;
            M(a0);
            L({a0.x - u.x * len - n.x * len / 2, a0.y - u.y * len - n.y * len / 2});
            if (t == "async") {
                L({a0.x - u.x * len, a0.y - u.y * len});
                plutovg_path_close(q);
                retreat = len;
            } else
                fill = false;
        } else if (t == "doubleBlock") {   // (two blocks one behind the other)
            for (int k = 0; k < 2; k++) {
                Pt a0{p.x - u.x * len * k, p.y - u.y * len * k};
                M(a0);
                L({a0.x - u.x * len + n.x * len / 2, a0.y - u.y * len + n.y * len / 2});
                L({a0.x - u.x * len - n.x * len / 2, a0.y - u.y * len - n.y * len / 2});
                plutovg_path_close(q);
            }
            retreat = 2 * len;
        } else if (t == "dash" || t == "cross") {   // (Graph.js: a slanted stroke size + stroke + 1 long across the line, 1 of it
            double d = size + sw + 1;                  // from the tip; a cross: and the other way)
            M({tip.x - u.x * d / 2 + n.x * d / 2, tip.y - u.y * d / 2 + n.y * d / 2});
            L({tip.x - u.x * d * 1.5 - n.x * d / 2, tip.y - u.y * d * 1.5 - n.y * d / 2});
            if (t == "cross") {
                M({tip.x - u.x * d / 2 - n.x * d / 2, tip.y - u.y * d / 2 - n.y * d / 2});
                L({tip.x - u.x * d * 1.5 + n.x * d / 2, tip.y - u.y * d * 1.5 + n.y * d / 2});
            }
            fill = false;
        } else {   // classic, block and the others
            M(p);
            L({back.x + n.x * half, back.y + n.y * half});
            if (t == "classic") L({p.x - u.x * len * 3 / 4, p.y - u.y * len * 3 / 4}), retreat = len * 3 / 4;
            else retreat = len;
            L({back.x - n.x * half, back.y - n.y * half});
            plutovg_path_close(q);
        }
        Item it;
        it.path = path;
        if (fill) it.fill = colour, retreat = std::max(retreat - sw / 2, 0.0);
        else retreat = t == "open" || t.compare(0, 2, "ER") == 0 ? retreat : 0;   // (an ER circle not filled: the line stops before it)
        retreat += std::hypot(p.x - tip.x, p.y - tip.y);
        it.stroke = colour;
        it.width = (float)sw;
        add(std::move(it), (float)sw);
        return retreat;
    }

    // mxArrowConnector (draw.io's flexArrow and link): the edge as a band width wide round its points, its corners mitred
    // (rounded: curved), arrowheads of their own at its ends (flexArrow), filled and outlined; a link: its two sides only
    void arrowConnector(const Style &s, std::vector<Pt> b) {
        bool link = str(s, "shape") == "link";
        double d = std::clamp(num(s, "strokeWidth", 1), 0.0, 1000.0);
        double g = std::max(0.0, num(s, "width", link ? 4 : 10)) + std::max(0.0, d - 1);   // (the band's width)
        double e = g + num(s, "startWidth", 20) + d, f = g + num(s, "endWidth", 20) + d;   // (the heads' widths)
        bool h = link, k = str(s, "startArrow") != "none" && !str(s, "startArrow").empty(), l = str(s, "endArrow") != "none";
        double m = h ? 0 : d / 2, n = 3 * num(s, "startSize", 6) + d, dd = 3 * num(s, "endSize", 6) + d;
        bool curved = str(s, "curved") == "1" && b.size() > 2, p = !curved && str(s, "rounded") == "1";
        if (curved) {   // (getCurvePoints: quadratics through the points' midpoints)
            std::vector<Pt> cv{b[0]};
            Pt from = b[0];
            for (size_t i = 1; i + 1 < b.size(); i++) {
                Pt q = b[i], to = i + 2 < b.size() ? Pt{(b[i].x + b[i + 1].x) / 2, (b[i].y + b[i + 1].y) / 2} : b[i + 1];
                int steps = (int)std::clamp(std::ceil((std::hypot(q.x - from.x, q.y - from.y) + std::hypot(to.x - q.x, to.y - q.y)) / 8), 4.0, 64.0);
                for (int j = 1; j <= steps; j++) {
                    double t = (double)j / steps, u = 1 - t;
                    cv.push_back({u * u * from.x + 2 * u * t * q.x + t * t * to.x, u * u * from.y + 2 * u * t * q.y + t * t * to.y});
                }
                from = to;
            }
            b = cv;
        }
        if (b.size() < 2) return;
        Pt c = b.back();
        double q = b[1].x - b[0].x, r = b[1].y - b[0].y, t = std::hypot(q, r);
        if (t == 0) return;
        PathP path = newPath();
        plutovg_path_t *a = path.get();
        auto M = [&](double x, double y) { plutovg_path_move_to(a, (float)x, (float)y); };
        auto L = [&](double x, double y) { plutovg_path_line_to(a, (float)x, (float)y); };
        auto Q = [&](double cx, double cy, double x, double y) { plutovg_path_quad_to(a, (float)cx, (float)cy, (float)x, (float)y); };
        auto marker = [&](double x0, double y0, double dx, double dy, double size, double aw, bool first) {   // (paintMarker)
            double ratio = g / aw, mx = g * dy / 2, my = -g * dx / 2, nx = (m + size) * dx, ny = (m + size) * dy;
            if (first) M(x0 - mx + nx, y0 - my + ny);
            else L(x0 - mx + nx, y0 - my + ny);
            L(x0 - mx / ratio + nx, y0 - my / ratio + ny);
            L(x0 + m * dx, y0 + m * dy);
            L(x0 + mx / ratio + nx, y0 + my / ratio + ny);
            L(x0 + mx + nx, y0 + my + ny);
        };
        double v = q / t, x = r / t, u = v, y = x;
        std::vector<std::function<void()>> back;   // (the other side, drawn backwards at the end)
        if (k && !h) marker(b[0].x, b[0].y, v, x, n, e, true);
        else {
            double tx = g * x, ty = -g * v;
            double C = b[0].x + tx / 2 + m * v, B = b[0].y + ty / 2 + m * x, z = b[0].x - tx / 2 + m * v, H = b[0].y - ty / 2 + m * x;
            if (h) M(C, B), back.push_back([=, &L] { L(z, H); });
            else M(z, H), L(C, B);
        }
        for (size_t i = 0; i + 2 < b.size(); i++) {
            Pt p0 = b[i], p1 = b[i + 1], p2 = b[i + 2];
            int w;   // (mxUtils.relativeCcw)
            {
                double x2 = p1.x - p0.x, y2 = p1.y - p0.y, px = p2.x - p0.x, py = p2.y - p0.y, ccw = px * y2 - py * x2;
                if (ccw == 0) {
                    ccw = px * x2 + py * y2;
                    if (ccw > 0) {
                        px -= x2, py -= y2;
                        ccw = px * x2 + py * y2;
                        if (ccw < 0) ccw = 0;
                    }
                }
                w = ccw < 0 ? -1 : ccw > 0 ? 1 : 0;
            }
            double C = p2.x - p1.x, B = p2.y - p1.y, E = std::hypot(C, B);
            if (E == 0) continue;
            u = C / E, y = B / E;
            E = std::max(std::sqrt(std::max(0.0, (v * u + x * y + 1) / 2)), .04);
            C = v + u, B = x + y;
            double F = std::hypot(C, B);
            if (F == 0) continue;
            C /= F, B /= F;
            F = std::max(E, std::min(d / 200 + .04, .35));
            E = w != 0 && p ? std::max(.1, F) : std::max(E, .06);
            double O = p1.x + B * g / 2 / E, G = p1.y - C * g / 2 / E, B2 = p1.x - B * g / 2 / E, C2 = p1.y + C * g / 2 / E;
            if (w != 0 && p) {
                if (w == -1) {
                    L(B2 + x * g, C2 - v * g);
                    Q(O, G, B2 + y * g, C2 - u * g);
                    back.push_back([=, &L] { L(B2, C2); });
                } else {
                    L(O, G);
                    double M1 = O - x * g, L1 = G + v * g, N1 = O - y * g, P1 = G + u * g;
                    back.push_back([=, &Q] { Q(B2, C2, M1, L1); });
                    back.push_back([=, &L] { L(N1, P1); });
                }
            } else {
                L(O, G);
                back.push_back([=, &L] { L(B2, C2); });
            }
            v = u, x = y;
        }
        if (l && !h) marker(c.x, c.y, -v, -x, dd, f, false);
        else {
            double tx = g * y, ty = -g * u;
            L(c.x - m * u + tx / 2, c.y - m * y + ty / 2);
            double D = c.x - m * u - tx / 2, J = c.y - m * y - ty / 2;
            if (h) M(D, J);
            else L(D, J);
        }
        for (size_t i = back.size(); i-- > 0;) back[i]();
        if (!h) plutovg_path_close(a);
        Style v2 = s;
        if (h) v2["fillColor"] = "none";
        else if (!v2.count("fillColor") || str(v2, "fillColor").empty()) v2["fillColor"] = "#ffffff";
        double x0 = b[0].x, y0 = b[0].y, x1 = x0, y1 = y0;
        for (Pt q2 : b) x0 = std::min(x0, q2.x), y0 = std::min(y0, q2.y), x1 = std::max(x1, q2.x), y1 = std::max(y1, q2.y);
        paint(path, nullptr, v2, Box{x0, y0, x1 - x0, y1 - y0}, false);
    }

    // Shapes.js TaperedArrowShape: a band from startWidth wide at the source to endWidth at the target, its filled classic /
    // block heads part of its outline (scaled with the band's width there), filled and outlined; other markers left out
    void taperedArrow(const Style &s, std::vector<Pt> pts) {
        std::vector<Pt> p;
        for (Pt q : pts)
            if (p.empty() || q.x != p.back().x || q.y != p.back().y) p.push_back(q);
        if (p.size() < 2) return;
        double sw0 = std::max(0.0, num(s, "startWidth", 12)), ew = std::max(0.0, num(s, "endWidth", 2));
        double stroke = std::clamp(num(s, "strokeWidth", 1), 0.0, 1000.0);
        struct Head {
            bool on = false;
            Pt cl, tip, cr;
        } heads[2];
        for (int k = 0; k < 2; k++) {   // (0: the source's head, 1: the target's)
            bool source = k == 0;
            const std::string &type = str(s, source ? "startArrow" : "endArrow");
            double wf = type == "classic" || type == "block" ? 2 : type == "classicThin" || type == "blockThin" ? 3 : 0;
            if (!wf || str(s, source ? "startFill" : "endFill") == "0") continue;
            size_t n = p.size();
            Pt p0 = source ? p[1] : p[n - 2];
            Pt &pe = source ? p[0] : p[n - 1];
            double dx = pe.x - p0.x, dy = pe.y - p0.y, dist = std::hypot(dx, dy);
            if (dist <= 0) continue;
            double ux = dx / dist, uy = dy / dist, w = source ? sw0 : ew;
            double size = num(s, source ? "startSize" : "endSize", 6) + w * wf / 2;
            double ox = ux * stroke * 1.118, oy = uy * stroke * 1.118, unx = ux * (size + stroke), uny = uy * (size + stroke);
            Pt tip{pe.x - ox, pe.y - oy};
            double f = type == "classic" || type == "classicThin" ? 0.75 : 1;
            pe.x += -unx * f - ox, pe.y += -uny * f - oy;
            Pt c1{tip.x - unx - uny / wf, tip.y - uny + unx / wf}, c2{tip.x + uny / wf - unx, tip.y - uny - unx / wf};
            heads[k].on = true, heads[k].tip = tip;
            heads[k].cl = source ? c2 : c1, heads[k].cr = source ? c1 : c2;
        }
        std::vector<Pt> nrm;
        std::vector<double> len{0};
        for (size_t i = 1; i < p.size(); i++) {
            double dx = p[i].x - p[i - 1].x, dy = p[i].y - p[i - 1].y, d = std::hypot(dx, dy);
            if (d <= 0) d = 1e-9;
            nrm.push_back({-dy / d, dx / d});
            len.push_back(len.back() + d);
        }
        double total = std::max(len.back(), 1e-9);
        std::vector<Pt> left, right;
        for (size_t i = 0; i < p.size(); i++) {   // (each point mitred, at most twice the width out)
            double w = (sw0 + (ew - sw0) * len[i] / total) / 2;
            Pt n0 = nrm[i > 0 ? i - 1 : 0], n1 = nrm[std::min(nrm.size() - 1, i)];
            double nx = n0.x + n1.x, ny = n0.y + n1.y, nl = std::hypot(nx, ny);
            double fct = w / std::max(0.5, nl / 2);
            nx = nl == 0 ? n1.x : nx / nl, ny = nl == 0 ? n1.y : ny / nl;
            left.push_back({p[i].x + nx * fct, p[i].y + ny * fct});
            right.push_back({p[i].x - nx * fct, p[i].y - ny * fct});
        }
        PathP band = newPath();
        plutovg_path_t *a = band.get();
        auto L = [&](Pt q) { plutovg_path_line_to(a, (float)q.x, (float)q.y); };
        plutovg_path_move_to(a, (float)left[0].x, (float)left[0].y);
        for (size_t i = 1; i < left.size(); i++) L(left[i]);
        if (heads[1].on) L(heads[1].cl), L(heads[1].tip), L(heads[1].cr);
        for (size_t i = right.size(); i-- > 0;) L(right[i]);
        if (heads[0].on) L(heads[0].cr), L(heads[0].tip), L(heads[0].cl);
        plutovg_path_close(a);
        double x0 = p[0].x, y0 = p[0].y, x1 = x0, y1 = y0;
        for (Pt q : p) x0 = std::min(x0, q.x), y0 = std::min(y0, q.y), x1 = std::max(x1, q.x), y1 = std::max(y1, q.y);
        paint(band, nullptr, s, Box{x0, y0, x1 - x0, y1 - y0}, false);
    }

    void drawEdge(Cell &c, std::vector<Pt> r) {
        const Style &s = c.style;
        if (str(s, "shape") == "mermaidSankeyLink") {   // (Shapes.js: a band width wide along a cubic from its first point to its
            Pt a = r.front(), e = r.back();                // last, level at both ends, filled like a vertex; markers none)
            double mx = (a.x + e.x) / 2, hw = std::max(1.0, num(s, "width", 10)) / 2;
            Pt side[2][33];
            for (int i = 0; i <= 32; i++) {
                double t = i / 32.0, u = 1 - t;
                double x = u * u * u * a.x + 3 * u * u * t * mx + 3 * u * t * t * mx + t * t * t * e.x;
                double y = u * u * u * a.y + 3 * u * u * t * a.y + 3 * u * t * t * e.y + t * t * t * e.y;
                double dx = 3 * u * u * (mx - a.x) + 3 * t * t * (e.x - mx), dy = 6 * u * t * (e.y - a.y), len = std::hypot(dx, dy);
                if (len == 0) dx = 1, dy = 0, len = 1;
                side[0][i] = {x - dy / len * hw, y + dx / len * hw}, side[1][i] = {x + dy / len * hw, y - dx / len * hw};
            }
            PathP band = newPath();
            plutovg_path_move_to(band.get(), (float)side[0][0].x, (float)side[0][0].y);
            for (int i = 1; i <= 32; i++) plutovg_path_line_to(band.get(), (float)side[0][i].x, (float)side[0][i].y);
            for (int i = 32; i >= 0; i--) plutovg_path_line_to(band.get(), (float)side[1][i].x, (float)side[1][i].y);
            plutovg_path_close(band.get());
            Box bb{std::min(a.x, e.x), std::min(a.y, e.y) - hw, std::fabs(e.x - a.x), std::fabs(e.y - a.y) + 2 * hw};
            Style v = s;
            if (!v.count("fillColor")) v["fillColor"] = "#ffffff";
            paint(band, nullptr, v, bb, false);
            c.route = r;
            if (!c.value.empty()) edgeLabel(c, c, r);
            return;
        }
        if (str(s, "shape") == "arrow" && r.size() >= 2) {   // (mxArrow: a straight arrow from the first point to the last, 10 wide,
            Pt e = r.front(), t = r.back();                    // its head 30 long and wide)
            double fx = t.x - e.x, gy = t.y - e.y, len = std::hypot(fx, gy);
            if (len > 0) {
                double k = len - 30;
                fx /= len, gy /= len;
                double h = 10 * gy, d = -10 * fx, l = e.x - h / 2, ey = e.y - d / 2, m = l + h, n = ey + d, p = m + k * fx, k2 = n + k * gy;
                double q = p + h, rr = k2 + d, tx = q - 3 * h, vy = rr - 3 * d;
                PathP band = newPath();
                plutovg_path_t *a = band.get();
                plutovg_path_move_to(a, (float)l, (float)ey);
                for (Pt v : {Pt{m, n}, Pt{p, k2}, Pt{q, rr}, t, Pt{tx, vy}, Pt{tx + h, vy + d}}) plutovg_path_line_to(a, (float)v.x, (float)v.y);
                plutovg_path_close(a);
                Style v2 = s;
                if (!v2.count("fillColor") || str(v2, "fillColor").empty()) v2["fillColor"] = "#ffffff";
                paint(band, nullptr, v2, Box{std::min(e.x, t.x), std::min(e.y, t.y), std::fabs(t.x - e.x), std::fabs(t.y - e.y)}, false);
            }
            c.route = r;
            if (!c.value.empty()) edgeLabel(c, c, r);
            return;
        }
        if (str(s, "shape") == "taperedArrow") {
            taperedArrow(s, r);
            c.route = r;
            if (!c.value.empty()) edgeLabel(c, c, r);
            return;
        }
        if (str(s, "shape") == "flexArrow" || str(s, "shape") == "link") {
            arrowConnector(s, r);
            c.route = r;
            if (!c.value.empty()) edgeLabel(c, c, r);
            return;
        }
        double sw = std::clamp(num(s, "strokeWidth", 1), 0.0, 1000.0);
        uint32_t stroke = withOpacity(colour(s, "strokeColor", 0xff000000), num(s, "opacity", 100) * num(s, "strokeOpacity", 100) / 100);
        std::vector<Pt> full = r;   // (the labels along it, before the markers shorten it)
        if (stroke) {
            auto unit = [](Pt a, Pt b) {
                double dx = b.x - a.x, dy = b.y - a.y, l = std::hypot(dx, dy);
                return l > 0 ? Pt{dx / l, dy / l} : Pt{1, 0};
            };
            // (mxConnector.createMarker: its direction from the first point that does not round to the end itself, in screen
            // pixels; a stub of less than a unit at an end (a rounding left over) left out, the marker along the line before it)
            auto same = [](Pt a, Pt b) { return std::fabs(a.x - b.x) < 1 && std::fabs(a.y - b.y) < 1; };
            while (r.size() > 2 && same(r[r.size() - 2], r.back())) r.erase(r.end() - 2);
            while (r.size() > 2 && same(r[1], r[0])) r.erase(r.begin() + 1);
            Pt ue = unit(r[r.size() - 2], r.back()), us = unit(r[1], r[0]);
            double re = marker(str(s, "endArrow"), str(s, "endFill") != "0", num(s, "endSize", 6), sw, r.back(), ue, stroke);
            double rs = marker(str(s, "startArrow"), str(s, "startFill") != "0", num(s, "startSize", 6), sw, r.front(), us, stroke);
            double le = std::hypot(r.back().x - r[r.size() - 2].x, r.back().y - r[r.size() - 2].y);
            re = std::min(re, le * 0.9);
            r.back().x -= ue.x * re, r.back().y -= ue.y * re;
            double ls = std::hypot(r[1].x - r[0].x, r[1].y - r[0].y);
            rs = std::min(rs, ls * 0.9);
            r[0].x -= us.x * rs, r[0].y -= us.y * rs;
            PathP p = newPath();
            plutovg_path_move_to(p.get(), (float)r[0].x, (float)r[0].y);
            bool curved = str(s, "curved") == "1", rounded = str(s, "rounded") == "1";
            for (size_t i = 1; i < r.size(); i++) {
                if (i + 1 < r.size() && (curved || rounded)) {
                    Pt a = r[i - 1], k = r[i], b = r[i + 1];
                    if (curved) {
                        Pt m{(k.x + b.x) / 2, (k.y + b.y) / 2};
                        if (i + 2 == r.size()) m = b;
                        plutovg_path_quad_to(p.get(), (float)k.x, (float)k.y, (float)m.x, (float)m.y);
                        if (i + 2 == r.size()) break;
                        continue;
                    }
                    double rad = num(s, "arcSize", 20) / 2, la = std::hypot(k.x - a.x, k.y - a.y), lb = std::hypot(b.x - k.x, b.y - k.y);
                    double d = std::min({rad, la / 2, lb / 2});
                    if (d > 0) {
                        Pt p1{k.x - (k.x - a.x) / la * d, k.y - (k.y - a.y) / la * d}, p2{k.x + (b.x - k.x) / lb * d, k.y + (b.y - k.y) / lb * d};
                        plutovg_path_line_to(p.get(), (float)p1.x, (float)p1.y);
                        plutovg_path_quad_to(p.get(), (float)k.x, (float)k.y, (float)p2.x, (float)p2.y);
                        continue;
                    }
                }
                plutovg_path_line_to(p.get(), (float)r[i].x, (float)r[i].y);
            }
            const std::string &shape = str(s, "shape");
            uint32_t inner = colour(s, "fillColor", 0);
            if (shape == "wire" || shape == "pipe") {   // (Shapes.js: a line in its stroke colour, again in its fill colour over it:
                Style base = s, top = s;               // a wire's dashed, a pipe's width wide in a casing of the stroke's width)
                base["dashed"] = "0";
                double width = std::max(0.0, num(s, "width", 4));
                if (shape == "pipe") base["strokeWidth"] = std::to_string(width + 2 * sw), top["strokeWidth"] = std::to_string(width);
                paint(p, nullptr, base, Box{}, true);
                if (inner) {
                    top["strokeColor"] = str(s, "fillColor");
                    top["shadow"] = "0";
                    paint(p, nullptr, top, Box{}, true);
                }
            } else
                paint(p, nullptr, s, Box{}, true);
            if (shape == "filledEdge" && sw >= 3 && inner) {   // (Shapes.js FilledEdge: the line and its markers again inside,
                Style in = s;                                   // 2 narrower, in its fill colour: an outlined band)
                in["strokeColor"] = str(s, "fillColor");
                in["strokeWidth"] = std::to_string(sw - 2);
                in["shadow"] = "0";
                paint(p, nullptr, in, Box{}, true);
                uint32_t ic = withOpacity(inner, num(s, "opacity", 100) * num(s, "strokeOpacity", 100) / 100);   // (as the inner line)
                marker(str(s, "endArrow"), str(s, "endFill") != "0", num(s, "endSize", 6), sw - 2, full.back(), ue, ic);
                marker(str(s, "startArrow"), str(s, "startFill") != "0", num(s, "startSize", 6), sw - 2, full.front(), us, ic);
            }
        }
        c.route = full;
        if (!c.value.empty()) edgeLabel(c, c, full);
    }

    // where a label sits along an edge: x -1..1 from its start to its end, y across it
    static Pt along(const std::vector<Pt> &r, double gx, double gy) {
        double total = 0;
        for (size_t i = 1; i < r.size(); i++) total += std::hypot(r[i].x - r[i - 1].x, r[i].y - r[i - 1].y);
        double dist = (std::clamp(gx, -1.0, 1.0) + 1) / 2 * total;
        for (size_t i = 1; i < r.size(); i++) {
            double dx = r[i].x - r[i - 1].x, dy = r[i].y - r[i - 1].y, l = std::hypot(dx, dy);
            if (dist <= l || i + 1 == r.size()) {
                double f = l > 0 ? std::min(dist / l, 1.0) : 0;
                Pt p{r[i - 1].x + dx * f, r[i - 1].y + dy * f};
                if (l > 0) p.x += dy / l * gy, p.y -= dx / l * gy;
                return p;
            }
            dist -= l;
        }
        return r.front();
    }
    void edgeLabel(Cell &lc, Cell &edgeCell, const std::vector<Pt> &r) {
        Pt p;
        if (&lc == &edgeCell) p = along(r, lc.relative ? lc.x : 0, lc.relative ? lc.y : 0);
        else if (lc.relative) p = along(r, lc.x, lc.y);
        else {   // (a label cell with an absolute geometry: from the edge's parent's origin, as a vertex)
            Pt o = origin(edgeCell.parent);
            p = {o.x + lc.x + lc.w / 2, o.y + lc.y + lc.h / 2};
        }
        if (lc.hasOffset && (lc.relative || &lc == &edgeCell)) p.x += lc.offset.x, p.y += lc.offset.y;
        Style s = lc.style;
        Box b{p.x - lc.w / 2, p.y - lc.h / 2, lc.w, lc.h};
        if (&lc == &edgeCell) b = {p.x, p.y, 0, 0};   // (its align / verticalAlign from the point: bottom, above it)
        else if (lc.w <= 0) {   // (no box: centred on the point)
            s["align"] = "center";
            s["verticalAlign"] = "middle";
            b = {p.x, p.y, 0, 0};
        }
        Box inner = &lc == &edgeCell || lc.w <= 0 ? b : labelBox(s, b);
        // (an edge's own label: centred on its point, wrapped to the width its geometry gives, as mxCellRenderer)
        double width = &lc == &edgeCell ? lc.w : inner.w;
        double wrap = str(s, "whiteSpace") == "wrap" && width > 0 ? width : 0;
        addLabel(layoutLabel(lc.value, s, inner, wrap));
    }

    // mxCellRenderer.resolveColor: a colour naming another (inherit / parentFillColor / parentStrokeColor: the
    // parent's; swimlane: the fill of the swimlane, table or table row it is in (an edge: its target's); fillColor /
    // strokeColor: its own other one), resolved in its style
    const std::string &colourOf(Cell &c, const char *key, int depth) {
        static const std::string black = "#000000", white = "#ffffff", none;
        std::string &v = c.style[key];
        if (depth > 64) return v;
        bool fillish = !strcmp(key, "fillColor") || !strcmp(key, "gradientColor");
        Cell *ref = nullptr;
        const char *rkey = key;
        if (v == "inherit") ref = find(c.parent);
        else if (v == "parentFillColor" || v == "parentStrokeColor") ref = find(c.parent), rkey = v == "parentFillColor" ? "fillColor" : "strokeColor";
        else if (v == "swimlane") {
            Cell *t = c.edge && find(c.target) ? find(c.target) : &c;
            size_t n = 0;
            for (; t && n++ <= cells.size(); t = find(t->parent)) {
                const std::string &sh = str(t->style, "shape");
                Cell *up = find(t->parent);
                if (!t->edge && up && find(up->parent) && (sh == "swimlane" || sh == "table" || sh == "tableRow")) break;
            }
            if (!t) return v = fillish ? white : black;
            ref = t, rkey = "fillColor";
        } else if (strcmp(key, "fillColor") && v == "fillColor") return v = colourOf(c, "fillColor", depth + 1);
        else if (strcmp(key, "strokeColor") && v == "strokeColor") return v = colourOf(c, "strokeColor", depth + 1);
        else return v;
        if (!ref || ref == &c) return v = none;
        return v = colourOf(*ref, rkey, depth + 1);
    }
    static std::string measure(double v) {   // (a size as JavaScript writes it, '.' in any locale)
        char t[40];
        snprintf(t, sizeof t, "%.10g", v);
        nn_cpoint(t);
        return t;
    }
    // Graph.replacePlaceholders: %name% of a label with placeholders=1, from the data of the cell or its ancestors
    // (%id%, %width%, %height% its own); one without a value is left as it is
    std::string placeholders(Cell &c) {
        std::string out;
        const std::string &v = c.value;
        size_t i = 0;
        for (size_t a; (a = v.find('%', i)) != std::string::npos;) {
            size_t e = v.find('%', a + 1);
            if (e == std::string::npos) break;
            std::string name = v.substr(a + 1, e - a - 1), val;
            bool found = false;
            if (name.empty() || name.find_first_of("{}\"'=;\n") != std::string::npos || name == "label" || name == "tooltip") {
                out.append(v, i, e - i);   // (not a placeholder: its closing % may open the next)
                i = e;
                continue;
            }
            if (name == "id") val = c.id, found = true;
            else if (name == "width" && c.vertex) val = measure(c.w), found = true;
            else if (name == "height" && c.vertex) val = measure(c.h), found = true;
            else {
                size_t n = 0;
                for (Cell *t = &c; t && n++ <= cells.size(); t = find(t->parent)) {
                    auto f = t->data.find(name);
                    if (f != t->data.end()) {
                        val = f->second, found = true;
                        break;
                    }
                }
            }
            out.append(v, i, a - i);
            if (found && out.size() + val.size() <= (4u << 20)) out += val;   // (a label of at most 4 MB: past that, left as it is)
            else out.append(v, a, e + 1 - a);
            i = e + 1;
        }
        out.append(v, i, std::string::npos);
        return out;
    }

    void build() {
        transparentBounds();
        for (Cell &c : cells) {   // (the colours naming others, the placeholders)
            for (const char *k : {"fillColor", "strokeColor", "fontColor", "gradientColor"}) {
                auto f = c.style.find(k);
                if (f != c.style.end()) colourOf(c, k, 0);
            }
            if (c.placeholders && c.value.find('%') != std::string::npos) c.value = placeholders(c);
        }
        std::unordered_map<std::string, std::vector<size_t>> children;   // (of every edge, in the cells' order)
        for (size_t i = 0; i < cells.size(); i++) {
            Cell *p = cells[i].vertex ? find(cells[i].parent) : nullptr;
            if (p && p->edge) children[p->id].push_back(i);
        }
        for (Cell &c : cells) {
            if (!shown(c) || layer(c)) continue;
            if (c.vertex) {
                Cell *p = find(c.parent);
                if (p && p->edge) continue;   // (an edge's label: with the edge)
                vertex(c);
            } else if (c.edge) {
                edge(c);
                auto f = children.find(c.id);   // (its label cells, after it)
                if (f != children.end() && c.route.size() >= 2)
                    for (size_t i : f->second)
                        if (!cells[i].value.empty() && shown(cells[i])) edgeLabel(cells[i], c, c.route);
            }
        }
    }
};

void readCells(xmlNode *root, Builder &b) {
    for (xmlNode *n = root->children; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE || b.cells.size() >= 200000) continue;
        xmlNode *cell = n;
        std::string id, value;
        bool wrapper = !is(n, "mxCell");
        std::unordered_map<std::string, std::string> data;
        if (wrapper) {   // UserObject / object: its id and label, the cell inside
            id = attr(n, "id");
            value = attr(n, "label");
            for (xmlAttr *a = n->properties; a && data.size() < 256; a = a->next) {
                xmlChar *v = xmlNodeListGetString(n->doc, a->children, 1);
                data[(const char *)a->name] = v ? (const char *)v : "";
                xmlFree(v);
            }
            cell = nullptr;
            for (xmlNode *k = n->children; k; k = k->next)
                if (is(k, "mxCell")) cell = k;
            if (!cell) continue;
        } else {
            id = attr(n, "id");
            value = attr(n, "value");
        }
        Cell c;
        c.id = id;
        c.value = value;
        c.placeholders = data.count("placeholders") && data["placeholders"] == "1";
        c.data = std::move(data);
        c.parent = attr(cell, "parent");
        c.source = attr(cell, "source");
        c.target = attr(cell, "target");
        c.edge = attr(cell, "edge") == "1";
        c.vertex = attr(cell, "vertex") == "1" && !c.edge;   // (both: an edge, as mxGraphView takes it)
        c.visible = attr(cell, "visible") != "0" && attr(n, "visible") != "0";
        c.collapsed = attr(cell, "collapsed") == "1";
        c.style = parseStyle(attr(cell, "style"), c.edge);
        for (xmlNode *g = cell->children; g; g = g->next)
            if (is(g, "mxGeometry")) readGeometry(c, g);
        if (!c.id.empty()) b.byId[c.id] = b.cells.size();
        b.cells.push_back(std::move(c));
    }
}

// An empty page (no content, or none that could be read): the size of the model's page, else draw.io's A4
void emptyPage(Page &page, xmlNode *model) {
    page.items.clear();
    page.x = page.y = 0;
    page.w = model ? attrNum(model, "pageWidth", 827) : 827;
    page.h = model ? attrNum(model, "pageHeight", 1169) : 1169;
    if (!(page.w > 0 && page.w < 1e6)) page.w = 827;
    if (!(page.h > 0 && page.h < 1e6)) page.h = 1169;
}

// false: the page has nothing drawn (left as an empty page, keeping the pages after it in their places)
bool readModel(xmlNode *model, Page &page, size_t &budget) {
    page.background = parseColour(attr(model, "background"), 0);
    emptyPage(page, model);
    xmlNode *root = nullptr;
    for (xmlNode *k = model->children; k; k = k->next)
        if (is(k, "root")) root = k;
    if (!root) return false;
    Builder b(page, budget);
    readCells(root, b);
    b.build();
    if (page.items.empty()) return false;
    nd::Union u;
    for (const Item &it : page.items) u.add(it.bx0, it.by0, it.bx1, it.by1);
    u.page(10, page.x, page.y, page.w, page.h);
    if (page.w > 0 && page.h > 0 && page.w < 1e8 && page.h < 1e8) return true;
    emptyPage(page, model);
    return false;
}

// --- the pictures (data: URIs: PNG, JPEG, SVG) -----------------------------------------

// a picture our own decoders leave (an interlaced PNG, a progressive JPEG ...): stb_image, as plutovg has it built in;
// premultiplied BGRA as ours (at most 16 megapixels: its size read first)
std::shared_ptr<Picture> stbPicture(const std::string &d, int64_t pixels) {
    if (pixels <= 0 || pixels > (16 << 20) || d.size() > (size_t)INT32_MAX) return nullptr;
    plutovg_surface_t *sf = plutovg_surface_load_from_image_data(d.data(), (int)d.size());
    if (!sf) return nullptr;
    auto pic = std::make_shared<Picture>();
    pic->w = plutovg_surface_get_width(sf), pic->h = plutovg_surface_get_height(sf);
    int stride = plutovg_surface_get_stride(sf);
    const uint8_t *px = plutovg_surface_get_data(sf);
    if (pic->w <= 0 || pic->h <= 0 || (int64_t)pic->w * pic->h > (16 << 20)) {
        plutovg_surface_destroy(sf);
        return nullptr;
    }
    pic->px.resize((size_t)pic->w * pic->h * 4);
    for (int y = 0; y < pic->h; y++) memcpy(pic->px.data() + (size_t)y * pic->w * 4, px + (size_t)y * stride, (size_t)pic->w * 4);
    plutovg_surface_destroy(sf);
    return pic;
}
// (stb_image too: an embedded picture is small, an icon mostly, where nitropng's parallel inflate does not pay; it reads every
// form: palette, grey, 16 bits, tRNS, interlaced. Its size from the header first)
std::shared_ptr<Picture> pngPicture(const std::string &d) {
    const uint8_t *p = (const uint8_t *)d.data();
    if (d.size() < 24 || memcmp(p + 12, "IHDR", 4) != 0) return nullptr;
    uint32_t w = (uint32_t)p[16] << 24 | p[17] << 16 | p[18] << 8 | p[19], h = (uint32_t)p[20] << 24 | p[21] << 16 | p[22] << 8 | p[23];
    return stbPicture(d, (int64_t)w * h);
}
std::shared_ptr<Picture> jpegPicture(const std::string &d) {
    const uint8_t *p = (const uint8_t *)d.data();
    nj_info in;
    if (nj_read_info(p, d.size(), &in)) return nullptr;
    if ((int64_t)in.width * in.height > (16 << 20)) return nullptr;
    if (in.ncomp != 1 && in.ncomp != 3) return stbPicture(d, (int64_t)in.width * in.height);   // (CMYK ...)
    std::vector<uint8_t> pl[3];
    uint8_t *planes[4] = {nullptr, nullptr, nullptr, nullptr};
    size_t pitch[4] = {0, 0, 0, 0};
    for (int c = 0; c < in.ncomp; c++) {
        pl[c].resize((size_t)in.plane_w[c] * in.plane_h[c]);
        planes[c] = pl[c].data(), pitch[c] = (size_t)in.plane_w[c];
    }
    if (nj_decode_planes(p, d.size(), &in, planes, pitch, 1, nullptr)) return stbPicture(d, (int64_t)in.width * in.height);   // (progressive ...)
    auto pic = std::make_shared<Picture>();
    pic->w = in.width, pic->h = in.height;
    pic->px.resize((size_t)in.width * in.height * 4);
    for (int y = 0; y < in.height; y++)
        for (int x = 0; x < in.width; x++) {
            uint8_t *o = pic->px.data() + ((size_t)y * in.width + x) * 4;
            auto at = [&](int c) { return (int)pl[c][(size_t)(y * in.v[c] / in.vmax) * pitch[c] + x * in.h[c] / in.hmax]; };
            int R, G, B;
            if (in.ncomp == 1) R = G = B = at(0);
            else if (in.adobe == 0 || in.rgb) R = at(0), G = at(1), B = at(2);   // (stored as R G B: Adobe's transform 0, or ids 'R' 'G' 'B')
            else {   // (BT.601 YCbCr, full range)
                double Y = at(0), cb = at(1) - 128.0, cr = at(2) - 128.0;
                R = (int)std::lround(Y + 1.402 * cr), G = (int)std::lround(Y - 0.344136 * cb - 0.714136 * cr), B = (int)std::lround(Y + 1.772 * cb);
            }
            o[0] = (uint8_t)std::clamp(B, 0, 255), o[1] = (uint8_t)std::clamp(G, 0, 255), o[2] = (uint8_t)std::clamp(R, 0, 255), o[3] = 255;
        }
    return pic;
}
std::shared_ptr<Picture> svgPicture(const std::string &d, double side) {   // (drawn at side pixels on its longer side, transparent)
    nitrosvg *svg = nitrosvg_load(d.data(), d.size());
    if (!svg) return nullptr;
    double w, h;
    nitrosvg_size(svg, &w, &h);
    std::shared_ptr<Picture> pic;
    if (w > 0 && h > 0) {
        double scale = side / std::max(w, h);
        int pw = std::max(1, (int)std::lround(w * scale)), ph = std::max(1, (int)std::lround(h * scale));
        pic = std::make_shared<Picture>();
        pic->w = pw, pic->h = ph;
        pic->px.resize((size_t)pw * ph * 4);
        nitrosvg_set_background(svg, 3);
        if (nitrosvg_render(svg, pic->px.data(), pw, ph, (size_t)pw * 4, scale, 0, 0)) pic = nullptr;
    }
    nitrosvg_free(svg);
    return pic;
}
// The page's pictures decoded (each data: URI once a file, 64 megapixels in all); one that cannot be is its frame
void pictures(Page &page, std::map<std::string, std::shared_ptr<Picture>> &cache, size_t &pixels) {
    for (Item &it : page.items) {
        if (it.uri.empty()) continue;
        std::string uri;
        uri.swap(it.uri);
        // (an SVG drawn at four times its box's longer side, 256 to 2048, to the next power of two: cached by that size too,
        // so a small use first leaves a large one sharp)
        bool svgUri = uri.size() > 5 && uri.compare(5, 13, "image/svg+xml") == 0;
        double side = 256;
        while (side < 2048 && side < 4.0 * std::max(it.ix1 - it.ix0, it.iy1 - it.iy0)) side *= 2;
        std::string key = svgUri ? uri + '\x01' + std::to_string((int)side) : uri;
        auto f = cache.find(key);
        if (f == cache.end()) {
            std::shared_ptr<Picture> pic;
            size_t comma = uri.find(',');
            std::string mime = uri.substr(5, comma == std::string::npos ? 0 : comma - 5), body = comma == std::string::npos ? "" : uri.substr(comma + 1);
            bool b64 = mime.find(";base64") != std::string::npos;
            if (svgUri) {
                if (!b64 && body.compare(0, 1, "<") != 0 && body.compare(0, 3, "%3C") != 0 && body.compare(0, 3, "%3c") != 0) b64 = true;   // (draw.io leaves out ;base64)
                std::string svg = b64 ? base64(body) : urlDecode(body);
                if (pixels < (64u << 20)) pic = svgPicture(svg, side);
            } else if (pixels < (64u << 20)) {
                std::string data = base64(body);
                pic = mime.compare(0, 9, "image/png") == 0 ? pngPicture(data) : mime.compare(0, 10, "image/jpeg") == 0 || mime.compare(0, 9, "image/jpg") == 0 ? jpegPicture(data) : nullptr;
            }
            if (pic) pixels += (size_t)pic->w * pic->h;
            f = cache.emplace(std::move(key), pic).first;
        }
        it.picture = f->second;
        if (!it.picture) it.stroke = withOpacity(0xffc0c0c0, it.pictureAlpha * 100), it.width = 1;   // (its frame, faded with the shape)
    }
}

struct XmlDocFree {
    void operator()(xmlDoc *d) const { xmlFreeDoc(d); }
};
using XmlDoc = std::unique_ptr<xmlDoc, XmlDocFree>;
// (256 MB of XML: libxml2's tree of it takes several times that)
// (one that does not parse whole, a cut-off file say, read again in recovery: what it has up to the damage)
XmlDoc parseXml(const char *data, size_t len) {
    if (len > (256u << 20)) return nullptr;
    const int opts = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_HUGE;
    XmlDoc d(xmlReadMemory(data, (int)len, nullptr, nullptr, opts));
    if (!d || !xmlDocGetRootElement(d.get())) d.reset(xmlReadMemory(data, (int)len, nullptr, nullptr, opts | XML_PARSE_RECOVER));
    return d;
}

}   // namespace

struct nitrodio {
    std::vector<Page> pages;
    int background = 0;
};

static nitrodio *load(const void *data, size_t len) {
    XmlDoc doc = parseXml((const char *)data, len);
    if (!doc) return nullptr;
    xmlNode *top = xmlDocGetRootElement(doc.get());
    std::unique_ptr<nitrodio> dio(new nitrodio);
    size_t budget = 8u << 20;   // (the items of all the pages: their display lists stay in memory together)
    bool drawn = false;   // (some page has something: else the file is no drawing)
    std::map<std::string, std::shared_ptr<Picture>> cache;   // (a file's pictures, each decoded once)
    size_t pixels = 0;
    auto model = [&](xmlNode *m) {
        Page p;
        {
            std::lock_guard<std::mutex> g(nf_lock());   // (the labels' faces; not while a page's XML is inflated and parsed)
            drawn |= readModel(m, p, budget);
        }
        pictures(p, cache, pixels);   // (an SVG picture takes nf_lock itself)
        dio->pages.push_back(std::move(p));
    };
    auto empty = [&] {   // (a page that cannot be read: kept empty, the pages after it in their places)
        Page p;
        emptyPage(p, nullptr);
        dio->pages.push_back(std::move(p));
    };
    if (top && is(top, "mxGraphModel")) model(top);
    else if (top && is(top, "mxfile")) {
        for (xmlNode *d = top->children; d && dio->pages.size() < 1000; d = d->next) {
            if (!is(d, "diagram")) continue;
            xmlNode *m = nullptr;
            for (xmlNode *k = d->children; k; k = k->next)
                if (is(k, "mxGraphModel")) m = k;
            if (m) {
                model(m);
                continue;
            }
            xmlChar *t = xmlNodeGetContent(d);   // (compressed: base64 of the raw deflated, URL encoded XML)
            if (!t) {
                empty();
                continue;
            }
            std::string text((const char *)t), xml;
            xmlFree(t);
            std::string bin = base64(text);
            // (a stream cut off or damaged: what it inflated to so far, if that is XML; else taken as not deflated)
            if (!inflateRaw(bin, xml) && !(xml.compare(0, 1, "<") == 0 || xml.compare(0, 3, "%3C") == 0 || xml.compare(0, 3, "%3c") == 0)) xml = bin;
            if (xml.compare(0, 1, "<") != 0) xml = urlDecode(xml);
            else if (xml.compare(0, 3, "%3C") == 0) xml = urlDecode(xml);
            if (xml.compare(0, 3, "%3C") == 0 || xml.compare(0, 3, "%3c") == 0) xml = urlDecode(xml);
            XmlDoc inner = parseXml(xml.data(), xml.size());
            xmlNode *ir = inner ? xmlDocGetRootElement(inner.get()) : nullptr;
            if (ir && is(ir, "mxGraphModel")) model(ir);
            else empty();
        }
    }
    // (a draw.io file is one even with nothing drawn: its pages empty, of their models' size)
    if (!drawn && (dio->pages.empty() || !top || !(is(top, "mxGraphModel") || is(top, "mxfile")))) return nullptr;
    return dio.release();
}

nitrodio *nitrodio_load(const void *data, size_t len) {
    try {   // (a C entry point: an allocation that fails on a huge diagram is "not a drawing", not an abort)
        return load(data, len);
    } catch (...) {
        return nullptr;
    }
}

void nitrodio_free(nitrodio *dio) { delete dio; }

int nitrodio_pages(const nitrodio *dio) { return dio ? (int)dio->pages.size() : 0; }

void nitrodio_set_background(nitrodio *dio, int background) {
    if (dio && background >= 0 && background <= 2) dio->background = background;
}

void nitrodio_size(const nitrodio *dio, int page, double *w, double *h) {
    *w = *h = 0;
    if (!dio || page < 0 || page >= (int)dio->pages.size()) return;
    *w = dio->pages[(size_t)page].w;
    *h = dio->pages[(size_t)page].h;
}

namespace {

// dashOk: a dashed stroke that does not reach too far out of the view (nd::within)
void drawItem(plutovg_canvas_t *c, const Item &it, float scale, bool dashOk) {
    if (it.label) {
        const Label &l = *it.label;
        plutovg_canvas_save(c);
        if (l.turned) {
            plutovg_canvas_translate(c, l.cx, l.cy);
            plutovg_canvas_rotate(c, l.angle);
            plutovg_canvas_translate(c, -l.cx, -l.cy);
        }
        if (l.clip) plutovg_canvas_clip_rect(c, l.clipX0, l.clipY0, l.clipX1 - l.clipX0, l.clipY1 - l.clipY0);
        if (l.background >> 24) {
            nd::setRgba(c, l.background);
            plutovg_canvas_fill_rect(c, l.x0, l.y0, l.x1 - l.x0, l.y1 - l.y0);
        }
        for (const Label::Rect &q : l.rects) {
            nd::setRgba(c, q.rgba);
            plutovg_canvas_fill_rect(c, q.x, q.y, q.w, q.h);
        }
        for (const Run &r : l.runs) {
            nd::setRgba(c, r.rgba);
            plutovg_canvas_set_font(c, r.face, r.size);
            plutovg_canvas_fill_text(c, r.text.data(), (int)r.text.size(), PLUTOVG_TEXT_ENCODING_UTF8, r.x, r.y);
            if (r.underline > 0)   // (a line a tenth of the size under the baseline, a fifteenth thick)
                plutovg_canvas_fill_rect(c, r.x, r.y + r.size * 0.1f, r.underline, std::max(r.size / 15, 0.5f));
            if (r.strike > 0)   // (through the middle of the small letters)
                plutovg_canvas_fill_rect(c, r.x, r.y - r.size * 0.3f, r.strike, std::max(r.size / 15, 0.5f));
        }
        plutovg_canvas_restore(c);
        return;
    }
    if (it.picture) {   // (into its box: aspect kept and centred, or stretched)
        const Picture &pic = *it.picture;
        float dx = it.ix0, dy = it.iy0, dw = it.ix1 - it.ix0, dh = it.iy1 - it.iy0;
        if (it.aspect) {
            float k = std::min(dw / pic.w, dh / pic.h);
            dx += (dw - pic.w * k) / 2, dy += (dh - pic.h * k) / 2, dw = pic.w * k, dh = pic.h * k;
        }
        plutovg_surface_t *sf = plutovg_surface_create_for_data(const_cast<uint8_t *>(pic.px.data()), pic.w, pic.h, pic.w * 4);
        plutovg_matrix_t m;
        plutovg_matrix_init_translate(&m, dx, dy);
        plutovg_matrix_scale(&m, dw / pic.w, dh / pic.h);
        if (it.turned) {   // (placed, then turned / mirrored with its shape)
            plutovg_matrix_t placed = m;
            plutovg_matrix_multiply(&m, &placed, &it.pm);
        }
        plutovg_canvas_set_texture(c, sf, PLUTOVG_TEXTURE_TYPE_PLAIN, it.pictureAlpha, &m);
        if (it.turned) {
            plutovg_path_t *r = plutovg_path_create();
            plutovg_path_add_rect(r, dx, dy, dw, dh);
            plutovg_path_t *tr = plutovg_path_create();
            plutovg_path_add_path(tr, r, &it.pm);
            plutovg_canvas_fill_path(c, tr);
            plutovg_path_destroy(tr), plutovg_path_destroy(r);
        } else
            plutovg_canvas_fill_rect(c, dx, dy, dw, dh);
        plutovg_surface_destroy(sf);
    }
    if ((it.fill >> 24) && it.hachure > 0) {   // (hand-drawn: the fill as parallel lines inside the path)
        plutovg_canvas_save(c);
        plutovg_canvas_clip_path(c, it.path.get());
        nd::setRgba(c, it.fill);
        plutovg_canvas_set_line_width(c, std::max(it.hachureWeight, 0.5f / scale));
        plutovg_canvas_set_dash(c, 0, nullptr, 0);
        float cx = (it.bx0 + it.bx1) / 2, cy = (it.by0 + it.by1) / 2, r = std::hypot(it.bx1 - it.bx0, it.by1 - it.by0) / 2 + 2;
        float a = it.hachureAngle * (float)M_PI / 180, dx = cosf(a), dy = sinf(a);
        // (at most 20000 lines: past that the gap widened, so the lines still cover the whole shape)
        double lines = it.hachure > 0 ? 2.0 * r / it.hachure : 1e9;
        int n = (int)std::min(lines + 1, 20000.0);
        float gapStep = lines + 1 > 20000 ? 2 * r / 20000 : it.hachure;
        plutovg_path_t *h = plutovg_path_create();
        for (int pass = 0; pass < (it.crossHatch ? 2 : 1); pass++) {
            if (pass) std::swap(dx, dy), dx = -dx;   // (cross-hatch: the lines again, turned a quarter)
            for (int k = 0; k <= n; k++) {
                float o = -r + k * gapStep;
                plutovg_path_move_to(h, cx - dy * o - dx * r, cy + dx * o - dy * r);
                plutovg_path_line_to(h, cx - dy * o + dx * r, cy + dx * o + dy * r);
            }
        }
        plutovg_canvas_stroke_path(c, h);
        plutovg_path_destroy(h);
        plutovg_canvas_restore(c);
    } else if ((it.fill >> 24) || (it.fill2 >> 24)) {   // (a gradient from transparent: its other end still there)
        if (it.fill2 >> 24) {
            plutovg_gradient_stop_t stops[2] = {{0, {(it.fill >> 16 & 255) / 255.f, (it.fill >> 8 & 255) / 255.f, (it.fill & 255) / 255.f, (it.fill >> 24) / 255.f}},
                                                {1, {(it.fill2 >> 16 & 255) / 255.f, (it.fill2 >> 8 & 255) / 255.f, (it.fill2 & 255) / 255.f, (it.fill2 >> 24) / 255.f}}};
            plutovg_canvas_set_linear_gradient(c, it.gx0, it.gy0, it.gx1, it.gy1, PLUTOVG_SPREAD_METHOD_PAD, stops, 2, nullptr);
        } else
            nd::setRgba(c, it.fill);
        plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
        plutovg_canvas_fill_path(c, it.path.get());
    }
    if ((it.stroke >> 24) && dashOk) {
        nd::setRgba(c, it.stroke);
        plutovg_canvas_set_line_width(c, std::max(it.width, 1 / scale));   // (a pixel at least)
        plutovg_canvas_set_line_join(c, PLUTOVG_LINE_JOIN_MITER);
        plutovg_canvas_set_line_cap(c, PLUTOVG_LINE_CAP_BUTT);
        if (!it.dash.empty()) plutovg_canvas_set_dash(c, 0, it.dash.data(), (int)it.dash.size());
        else plutovg_canvas_set_dash(c, 0, nullptr, 0);
        plutovg_canvas_stroke_path(c, it.path.get());
    }
}
}   // namespace

int nitrodio_render(nitrodio *dio, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                    int (*cancelled)(void *ctx), void *ctx) {
    if (!dio || page < 0 || page >= (int)dio->pages.size() || !nd::renderArgs(w, h, pitch, scale)) return -1;
    const Page &pg = dio->pages[(size_t)page];
    nd::Target t(bgra, w, h, pitch, pg.background >> 24 ? pg.background : nd::background(dio->background));   // (the diagram's own first)
    plutovg_canvas_t *c = t.canvas;
    plutovg_canvas_scale(c, (float)scale, (float)scale);
    x0 += pg.x, y0 += pg.y;
    plutovg_canvas_translate(c, (float)-x0, (float)-y0);
    double view[4] = {x0, y0, x0 + w / scale, y0 + h / scale};
    size_t n = 0;
    int rc = 0;
    for (const Item &it : pg.items) {
        if ((++n & 255) == 0 && cancelled && cancelled(ctx)) {
            rc = 1;
            break;
        }
        if (nd::touches(view, it.bx0, it.by0, it.bx1, it.by1))
            drawItem(c, it, (float)scale, it.dash.empty() || nd::within(view, it.bx0, it.by0, it.bx1, it.by1, scale));
    }
    return rc;
}
