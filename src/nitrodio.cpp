// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodio: draw.io diagrams (see nitrodio.h). The file: <mxfile> with a <diagram> per page,
// each holding an <mxGraphModel> or its compressed text; or a bare <mxGraphModel>. The model's
// <root> lists the cells in drawing order: layers (children of the root cell), vertices and
// edges, their geometry (<mxGeometry>, relative to the parent vertex; an edge's label cells
// placed along it) and a style string ("name;key=value;..." over draw.io's default stylesheet).
// The connectors are routed here: straight through their waypoints, or orthogonally (a simple
// router, not mxGraph's own).
#include "nitrodio.h"
#include "nitro_font.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <plutovg.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
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
    };
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
        s["labelBackgroundColor"] = "#ffffff";
    } else
        s["fillColor"] = "#ffffff";
    size_t i = 0;
    while (i <= str.size()) {
        size_t e = str.find(';', i);
        if (e == std::string::npos) e = str.size();
        std::string tok = str.substr(i, e - i);
        size_t eq = tok.find('=');
        if (eq == std::string::npos) applyNamed(s, tok);
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
    double d = strtod(v.c_str(), &end);
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
    if (v.compare(0, 4, "rgb(") == 0) {
        int r = 0, g = 0, b = 0;
        if (sscanf(v.c_str() + 4, "%d , %d , %d", &r, &g, &b) == 3)
            return 0xff000000u | (uint32_t)(std::clamp(r, 0, 255) << 16 | std::clamp(g, 0, 255) << 8 | std::clamp(b, 0, 255));
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
};
struct Label {
    std::vector<Run> runs;
    uint32_t background = 0;   // (0: none) behind the text box
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the text box
    bool turned = false;   // rotated about (cx, cy)
    float cx = 0, cy = 0, angle = 0;
};
struct Item {
    PathP path;
    uint32_t fill = 0, fill2 = 0, stroke = 0;   // fill2: a gradient's end (fill at gx0, gy0 to fill2 at gx1, gy1)
    float gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
    float width = 1;
    std::vector<float> dash;
    std::shared_ptr<Label> label;
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
bool inflateRaw(const std::string &in, std::string &out) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return false;
    z.next_in = (Bytef *)in.data();
    z.avail_in = (uInt)in.size();
    char buf[65536];
    int rc;
    do {
        z.next_out = (Bytef *)buf;
        z.avail_out = sizeof buf;
        rc = inflate(&z, Z_NO_FLUSH);
        out.append(buf, sizeof buf - z.avail_out);
    } while (rc == Z_OK && out.size() < (256u << 20));
    inflateEnd(&z);
    return rc == Z_STREAM_END;
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
    double d = strtod(v.c_str(), nullptr);
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
    Box box;
    std::vector<Pt> route;   // an edge's points, absolute
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
};
using Para = std::vector<Seg>;

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
    } else
        o += "&" + e + ";";
}

// A label's text as paragraphs: plain (its line breaks), or HTML: <br> and the block elements
// break lines, <b> <strong> <i> <em> (and the bold / italic of fontStyle) set the font, the
// other tags are dropped, white space collapses
std::vector<Para> parseLabel(const std::string &v, bool html, bool bold, bool italic) {
    std::vector<Para> paras(1);
    auto add = [&](const std::string &t, bool b, bool i) {
        if (t.empty()) return;
        Para &p = paras.back();
        if (!p.empty() && p.back().bold == b && p.back().italic == i) p.back().text += t;
        else p.push_back({t, b, i});
    };
    if (!html) {
        std::string cur;
        for (char c : v) {
            if (c == '\n') {
                add(cur, bold, italic);
                cur.clear();
                paras.emplace_back();
            } else if (c != '\r')
                cur += c;
        }
        add(cur, bold, italic);
        return paras;
    }
    int b = bold, it = italic;
    bool space = true;   // (white space collapsed: at a line's start, or after one)
    std::string cur;
    auto flush = [&] {
        add(cur, b > 0, it > 0);
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
            if (name == "br") newLine();
            else if (name == "div" || name == "p" || name == "li" || name == "tr" || (name.size() == 2 && name[0] == 'h' && isdigit((unsigned char)name[1]))) {
                if (!paras.back().empty() || !cur.empty()) newLine();
            } else if (name == "b" || name == "strong") {
                flush();
                b += close ? -1 : 1;
            } else if (name == "i" || name == "em") {
                flush();
                it += close ? -1 : 1;
            } else if (name == "span" || name == "font") {
                if (!close && tag.find("font-weight") != std::string::npos && tag.find("bold") != std::string::npos) flush(), b++;
            }
            continue;
        }
        std::string piece;
        if (c == '&') {
            size_t e = v.find(';', i);
            if (e != std::string::npos && e - i < 12) {
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

// The label laid out in a box (its alignments; wrapWidth > 0: words wrapped to it), the lines
// 1.2 font sizes apart, as draw.io sets them (caller holds nf_lock)
std::shared_ptr<Label> layoutLabel(const std::string &value, const Style &s, Box box, double wrapWidth) {
    bool html = str(s, "html") == "1";
    int fontStyle = (int)num(s, "fontStyle", 0);
    std::vector<Para> paras = parseLabel(value, html, fontStyle & 1, fontStyle & 2);
    float size = (float)std::clamp(num(s, "fontSize", 12), 1.0, 1000.0);
    std::string family = str(s, "fontFamily");
    size_t comma = family.find(',');
    if (comma != std::string::npos) family.resize(comma);
    uint32_t rgba = withOpacity(colour(s, "fontColor", 0xff000000), num(s, "textOpacity", 100) * num(s, "opacity", 100) / 100);
    if (!(rgba >> 24)) return nullptr;
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
    };
    std::vector<std::vector<LineRun>> lines;
    std::vector<float> widths;
    for (const Para &p : paras) {
        lines.emplace_back();
        widths.push_back(0);
        for (const Seg &sg : p) {
            plutovg_font_face_t *f = face(sg.bold, sg.italic);
            if (wrapWidth <= 0) {
                float w = textWidth(f, size, sg.text);
                lines.back().push_back({f, sg.text, w});
                widths.back() += w;
                continue;
            }
            size_t i = 0;   // (word by word: a word with its following spaces)
            while (i < sg.text.size()) {
                size_t e = sg.text.find(' ', i);
                e = e == std::string::npos ? sg.text.size() : e + 1;
                std::string word = sg.text.substr(i, e - i);
                i = e;
                float w = textWidth(f, size, word);
                std::string bare = word;
                while (!bare.empty() && bare.back() == ' ') bare.pop_back();
                if (widths.back() > 0 && widths.back() + textWidth(f, size, bare) > wrapWidth + 0.5) {   // (wrapped)
                    if (!lines.back().empty()) {
                        LineRun &last = lines.back().back();
                        while (!last.text.empty() && last.text.back() == ' ') last.text.pop_back();
                        last.width = textWidth(last.face, size, last.text);
                        widths.back() = 0;
                        for (LineRun &r : lines.back()) widths.back() += r.width;
                    }
                    lines.emplace_back();
                    widths.push_back(0);
                }
                if (!lines.back().empty() && lines.back().back().face == f) lines.back().back().text += word, lines.back().back().width += w;
                else lines.back().push_back({f, word, w});
                widths.back() += w;
            }
        }
    }
    auto label = std::make_shared<Label>();
    float lineH = size * 1.2f;
    float textH = lineH * (float)lines.size(), textW = 0;
    for (float w : widths) textW = std::max(textW, w);
    const std::string &va = str(s, "verticalAlign"), &al = str(s, "align");
    double top = va == "top" ? box.y : va == "bottom" ? box.y + box.h - textH : box.y + (box.h - textH) / 2;
    plutovg_font_face_t *f0 = face(false, false);
    float asc = size * 0.8f, desc = size * 0.2f;
    if (f0) {
        float a = 0, d = 0, g = 0;
        plutovg_font_face_get_metrics(f0, size, &a, &d, &g, nullptr);
        asc = a, desc = -d;
    }
    float x0 = 1e30f, x1 = -1e30f;
    for (size_t l = 0; l < lines.size(); l++) {
        double left = al == "left" ? box.x : al == "right" ? box.x + box.w - widths[l] : box.x + (box.w - widths[l]) / 2;
        float base = (float)(top + lineH * l + (lineH + asc - desc) / 2);
        float pen = (float)left;
        x0 = std::min(x0, pen), x1 = std::max(x1, pen + widths[l]);
        for (LineRun &r : lines[l]) {
            if (!r.text.empty() && r.face) label->runs.push_back({r.face, size, rgba, pen, base, r.text});
            pen += r.width;
        }
    }
    if (label->runs.empty()) return nullptr;
    label->x0 = x0 - 2, label->x1 = x1 + 2;
    label->y0 = (float)top, label->y1 = (float)(top + textH);
    label->background = colour(s, "labelBackgroundColor", 0);
    if (textW <= 0) label->background = 0;
    return label;
}

// --- shapes --------------------------------------------------------------------

void polygon(plutovg_path_t *p, std::initializer_list<Pt> pts, const Box &b) {
    bool first = true;
    for (Pt q : pts) {
        float x = (float)(b.x + q.x), y = (float)(b.y + q.y);
        if (first) plutovg_path_move_to(p, x, y), first = false;
        else plutovg_path_line_to(p, x, y);
    }
    plutovg_path_close(p);
}

// The outline of a vertex shape in its box (draw.io's built-in ones; the others: their frame).
// Second path: the lines drawn over it (a process' bars, a note's fold, a cylinder's rim)
PathP shapePath(const std::string &shape, const Style &s, const Box &b, PathP &lines) {
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
    if (shape == "ellipse" || shape == "doubleEllipse") {
        plutovg_path_add_ellipse(q, (float)(b.x + w / 2), (float)(b.y + h / 2), (float)(w / 2), (float)(h / 2));
        if (shape == "doubleEllipse") {
            if (!lines) lines = newPath();
            double in = std::min(w, h) * 0.1;
            plutovg_path_add_ellipse(lines.get(), (float)(b.x + w / 2), (float)(b.y + h / 2), (float)(w / 2 - in), (float)(h / 2 - in));
        }
    } else if (shape == "rhombus") {
        polygon(q, {{w / 2, 0}, {w, h / 2}, {w / 2, h}, {0, h / 2}}, b);
    } else if (shape == "triangle") {
        if (dir == "north") polygon(q, {{0, h}, {w / 2, 0}, {w, h}}, b);
        else if (dir == "south") polygon(q, {{0, 0}, {w, 0}, {w / 2, h}}, b);
        else if (dir == "west") polygon(q, {{w, 0}, {w, h}, {0, h / 2}}, b);
        else polygon(q, {{0, 0}, {w, h / 2}, {0, h}}, b);
    } else if (shape == "hexagon") {
        double d = std::min(w * num(s, "size", 0.25), w / 2);
        if (str(s, "fixedSize") == "1") d = std::min(num(s, "size", 20), w / 2);
        polygon(q, {{d, 0}, {w - d, 0}, {w, h / 2}, {w - d, h}, {d, h}, {0, h / 2}}, b);
    } else if (shape == "parallelogram" || shape == "trapezoid" || shape == "step") {
        double d = str(s, "fixedSize") == "1" ? num(s, "size", 20) : w * num(s, "size", 0.2);
        d = std::clamp(d, 0.0, w / 2);
        if (shape == "parallelogram") polygon(q, {{d, 0}, {w, 0}, {w - d, h}, {0, h}}, b);
        else if (shape == "trapezoid") polygon(q, {{0, h}, {d, 0}, {w - d, 0}, {w, h}}, b);
        else polygon(q, {{0, 0}, {w - d, 0}, {w, h / 2}, {w - d, h}, {0, h}, {d, h / 2}}, b);
    } else if (shape == "card" || shape == "note") {
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
    std::vector<Cell> cells;
    std::unordered_map<std::string, size_t> byId;

    explicit Builder(Page &p) : page(p) {}

    Cell *find(const std::string &id) {
        auto f = byId.find(id);
        return f == byId.end() ? nullptr : &cells[f->second];
    }
    bool layer(const Cell &c) { return !c.vertex && !c.edge; }
    // whether a cell is shown: it and its ancestors visible, none of them collapsed above it
    bool shown(const Cell &c) {
        const Cell *p = &c;
        for (int depth = 0; p && depth < 256; depth++) {
            if (!p->visible) return false;
            const Cell *up = find(p->parent);
            if (up && up != p && up->collapsed && up->vertex) return false;
            p = up == p ? nullptr : up;
        }
        return true;
    }
    // a vertex's absolute box (its parents' origins added)
    Box absBox(Cell &c) {
        if (c.state == 2) return c.box;
        if (c.state == 1) return {c.x, c.y, c.w, c.h};   // (a cycle of parents)
        c.state = 1;
        Pt o = origin(c.parent);
        Box pb;
        Cell *p = find(c.parent);
        if (p && p->vertex) pb = absBox(*p);
        if (c.relative && p && p->vertex) c.box = {pb.x + c.x * pb.w, pb.y + c.y * pb.h, c.w, c.h};
        else c.box = {o.x + c.x, o.y + c.y, c.w, c.h};
        if (c.hasOffset && c.relative) c.box.x += c.offset.x, c.box.y += c.offset.y;
        c.state = 2;
        return c.box;
    }
    Pt origin(const std::string &parent) {   // where the children of a cell start
        Cell *p = find(parent);
        if (!p || !p->vertex) return {0, 0};
        Box b = absBox(*p);
        return {b.x, b.y};
    }

    void add(Item &&it, float margin) {
        if (!it.label) {
            plutovg_rect_t r;
            plutovg_path_extents(it.path.get(), &r, false);
            it.bx0 = r.x - margin, it.by0 = r.y - margin, it.bx1 = r.x + r.w + margin, it.by1 = r.y + r.h + margin;
        }
        for (float v : {it.bx0, it.by0, it.bx1, it.by1})   // (NaN or far off: plutovg converts its coordinates to integers)
            if (!(std::fabs(v) < 1e8f)) return;
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
        if (page.items.size() < (4u << 20)) page.items.push_back(std::move(it));
    }
    void addLabel(std::shared_ptr<Label> l) {
        if (!l) return;
        Item it;
        it.label = l;
        float x0 = l->x0, y0 = l->y0, x1 = l->x1, y1 = l->y1;
        if (l->turned) {   // (the box of the turned one: its corners turned)
            float c = cosf(l->angle), s = sinf(l->angle), bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
            for (float px : {x0, x1})
                for (float py : {y0, y1}) {
                    float dx = px - l->cx, dy = py - l->cy, rx = l->cx + dx * c - dy * s, ry = l->cy + dx * s + dy * c;
                    bx0 = std::min(bx0, rx), by0 = std::min(by0, ry), bx1 = std::max(bx1, rx), by1 = std::max(by1, ry);
                }
            x0 = bx0, y0 = by0, x1 = bx1, y1 = by1;
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
            for (double d; (d = strtod(c, &end)), end != c && dash.size() < 16; c = end)
                if (std::isfinite(d) && d >= 0) dash.push_back((float)(d * std::max(width, 1.f)));
            if (dash.size() % 2) dash.insert(dash.end(), dash.begin(), dash.end());
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
            uint32_t sc = 0x40000000;
            if (fill) sh.fill = sc;
            else sh.stroke = sc, sh.width = width;
            add(std::move(sh), width);
        }
        Item it;
        it.path = path;
        it.fill = fill;
        uint32_t g = edge ? 0 : colour(s, "gradientColor", 0);
        if (fill && g) {
            it.fill2 = withOpacity(g, opacity);
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
        add(std::move(it), width);
        if (lines && stroke && width > 0) {
            Item l;
            l.path = lines;
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
        return {b.x + l, b.y + t, std::max(0.0, b.w - l - r), std::max(0.0, b.h - t - bt)};
    }

    void vertex(Cell &c) {
        Box b = absBox(c);
        const Style &s = c.style;
        std::string shape = str(s, "shape");
        if (shape.empty()) shape = "rectangle";
        double rotation = num(s, "rotation", 0);
        bool text = shape == "text" || (str(s, "strokeColor") == "none" && str(s, "fillColor") == "none");
        if (shape == "swimlane" && b.w > 0 && b.h > 0) {   // (only its header filled: the body by swimlaneFillColor)
            bool across = str(s, "horizontal") == "0";
            double ss = std::clamp(num(s, "startSize", 23), 0.0, across ? b.w : b.h);
            Style body = s;
            body["fillColor"] = str(s, "swimlaneFillColor").empty() ? "none" : str(s, "swimlaneFillColor");
            body["gradientColor"] = "none";
            PathP lines, outline = newPath();
            plutovg_path_add_rect(outline.get(), (float)b.x, (float)b.y, (float)b.w, (float)b.h);
            if (ss > 0) {
                Box hb = across ? Box{b.x, b.y, ss, b.h} : Box{b.x, b.y, b.w, ss};
                Style head = s;
                head["strokeColor"] = "none";
                PathP hp = newPath();
                plutovg_path_add_rect(hp.get(), (float)hb.x, (float)hb.y, (float)hb.w, (float)hb.h);
                lines = newPath();
                if (across) plutovg_path_move_to(lines.get(), (float)(b.x + ss), (float)b.y), plutovg_path_line_to(lines.get(), (float)(b.x + ss), (float)(b.y + b.h));
                else plutovg_path_move_to(lines.get(), (float)b.x, (float)(b.y + ss)), plutovg_path_line_to(lines.get(), (float)(b.x + b.w), (float)(b.y + ss));
                paint(hp, nullptr, head, hb, false);
            }
            paint(outline, lines, body, b, false);
        } else if (!text && shape != "image" && b.w > 0 && b.h > 0) {
            PathP lines;
            PathP p = shapePath(shape, s, b, lines);
            if (rotation != 0) {
                plutovg_matrix_t m;
                plutovg_matrix_init_translate(&m, (float)(b.x + b.w / 2), (float)(b.y + b.h / 2));
                plutovg_matrix_rotate(&m, (float)(rotation * M_PI / 180));
                plutovg_matrix_translate(&m, (float)-(b.x + b.w / 2), (float)-(b.y + b.h / 2));
                PathP t = newPath();
                plutovg_path_add_path(t.get(), p.get(), &m);
                p = t;
                if (lines) {
                    PathP tl = newPath();
                    plutovg_path_add_path(tl.get(), lines.get(), &m);
                    lines = tl;
                }
            }
            paint(p, lines, s, b, false);
        } else if (shape == "image" && b.w > 0 && b.h > 0) {   // (the picture is not in the file: its frame)
            Item it;
            it.path = newPath();
            plutovg_path_add_rect(it.path.get(), (float)b.x, (float)b.y, (float)b.w, (float)b.h);
            it.stroke = 0xffc0c0c0;
            it.width = 1;
            add(std::move(it), 1);
        }
        if (c.value.empty()) return;
        Box lb = b;
        bool turned = false;
        if (shape == "swimlane") {   // (in its header)
            double ss = num(s, "startSize", 23);
            if (str(s, "horizontal") == "0") lb.w = std::min(ss, b.w), turned = true;
            else lb.h = std::min(ss, b.h);
        } else if (str(s, "horizontal") == "0")
            turned = true;
        if (turned) {   // (laid out in the box turned upright, then turned back by -90 degrees)
            Pt ce = lb.centre();
            lb = {ce.x - lb.h / 2, ce.y - lb.w / 2, lb.h, lb.w};
        }
        Box inner = labelBox(s, lb);
        double wrap = str(s, "whiteSpace") == "wrap" ? inner.w : 0;
        std::shared_ptr<Label> l = layoutLabel(c.value, s, inner, wrap);
        if (l && (turned || rotation != 0)) {
            Pt ce = lb.centre();
            l->turned = true;
            l->cx = (float)ce.x, l->cy = (float)ce.y;
            l->angle = (float)((turned ? -90 : 0) + rotation) * (float)M_PI / 180;
        }
        addLabel(l);
    }

    // --- edges ---

    enum Dir { NONE, H, V };
    // a point on the perimeter of a terminal's shape, towards 'to' (from its centre)
    static Pt perimeter(const Box &b, const std::string &shape, Pt to) {
        Pt c = b.centre();
        double dx = to.x - c.x, dy = to.y - c.y;
        if ((dx == 0 && dy == 0) || b.w <= 0 || b.h <= 0) return c;
        if (shape == "ellipse" || shape == "doubleEllipse") {
            double a = b.w / 2, e = b.h / 2, t = 1 / std::sqrt(dx * dx / (a * a) + dy * dy / (e * e));
            return {c.x + dx * t, c.y + dy * t};
        }
        if (shape == "rhombus") {
            double t = 1 / (std::fabs(dx) / (b.w / 2) + std::fabs(dy) / (b.h / 2));
            return {c.x + dx * t, c.y + dy * t};
        }
        double t = std::min(dx != 0 ? (b.w / 2) / std::fabs(dx) : 1e30, dy != 0 ? (b.h / 2) / std::fabs(dy) : 1e30);
        return {c.x + dx * t, c.y + dy * t};
    }
    // a terminal's fixed connection point (exitX / entryX ...); false if the style sets none
    static bool constraint(const Style &s, const char *px, const Box &b, Pt &p, Dir &d) {
        std::string kx = std::string(px) + "X", ky = std::string(px) + "Y", kdx = std::string(px) + "Dx", kdy = std::string(px) + "Dy";
        const std::string &vx = str(s, kx.c_str()), &vy = str(s, ky.c_str());
        if (vx.empty() || vy.empty()) return false;
        double fx = strtod(vx.c_str(), nullptr), fy = strtod(vy.c_str(), nullptr);
        if (!std::isfinite(fx) || !std::isfinite(fy)) return false;
        p = {b.x + fx * b.w + num(s, kdx.c_str(), 0), b.y + fy * b.h + num(s, kdy.c_str(), 0)};
        d = fx <= 0 || fx >= 1 ? H : fy <= 0 || fy >= 1 ? V : NONE;
        return true;
    }

    void edge(Cell &c) {
        const Style &s = c.style;
        Pt o = origin(c.parent);
        std::vector<Pt> pts;
        for (Pt p : c.points) pts.push_back({o.x + p.x, o.y + p.y});
        Cell *src = find(c.source), *dst = find(c.target);
        if (src && !src->vertex) src = nullptr;   // (an edge ending on an edge: at its point, if any)
        if (dst && !dst->vertex) dst = nullptr;
        Box sb, tb;
        std::string sshape, tshape;
        if (src) sb = absBox(*src), sshape = str(src->style, "shape");
        if (dst) tb = absBox(*dst), tshape = str(dst->style, "shape");
        Pt sp{o.x + c.sourcePoint.x, o.y + c.sourcePoint.y}, tp{o.x + c.targetPoint.x, o.y + c.targetPoint.y};
        if (!src && !c.hasSource) return;
        if (!dst && !c.hasTarget) return;
        Dir sd = NONE, td = NONE;
        bool sFixed = src && constraint(s, "exit", sb, sp, sd), tFixed = dst && constraint(s, "entry", tb, tp, td);
        const std::string &es = str(s, "edgeStyle");
        bool ortho = es == "orthogonalEdgeStyle" || es == "elbowEdgeStyle" || es == "entityRelationEdgeStyle" || es == "segmentEdgeStyle" ||
                     es == "sideToSideEdgeStyle" || es == "topToBottomEdgeStyle" || es == "isometricEdgeStyle";
        std::vector<Pt> route;
        if (!ortho) {
            Pt toS = !pts.empty() ? pts.front() : dst ? (tFixed ? tp : tb.centre()) : tp;
            Pt toT = !pts.empty() ? pts.back() : src ? (sFixed ? sp : sb.centre()) : sp;
            if (src && !sFixed) sp = perimeter(sb, sshape, toS);
            if (dst && !tFixed) tp = perimeter(tb, tshape, toT);
            route.push_back(sp);
            route.insert(route.end(), pts.begin(), pts.end());
            route.push_back(tp);
        } else
            route = orthogonal(c, src ? &sb : nullptr, dst ? &tb : nullptr, sp, tp, sFixed, tFixed, sd, td, pts, es);
        // (the duplicates out)
        std::vector<Pt> r;
        for (Pt p : route)
            if (r.empty() || std::fabs(p.x - r.back().x) > 1e-6 || std::fabs(p.y - r.back().y) > 1e-6) r.push_back(p);
        if (r.size() < 2) return;
        c.route = r;
        drawEdge(c, r);
    }

    // A simple orthogonal router: from the source's side (its exit, else the one facing the
    // target), through the waypoints, into the target's side, an elbow between consecutive points
    std::vector<Pt> orthogonal(Cell &c, const Box *sb, const Box *tb, Pt sp, Pt tp, bool sFixed, bool tFixed, Dir sd, Dir td,
                               const std::vector<Pt> &pts, const std::string &es) {
        Pt sref = sb ? sb->centre() : sp, tref = tb ? tb->centre() : tp;
        bool vertical = es == "topToBottomEdgeStyle" || (es == "elbowEdgeStyle" && str(c.style, "elbow") == "vertical");
        auto facing = [&](const Box *b, Pt from, Pt to, Dir &d) -> Pt {   // the side of b facing 'to'
            double dx = to.x - from.x, dy = to.y - from.y;
            if (d == NONE) d = es == "elbowEdgeStyle" || es == "sideToSideEdgeStyle" || es == "entityRelationEdgeStyle" ? (vertical ? V : H)
                               : vertical ? V : std::fabs(dx) * (b ? b->h : 1) >= std::fabs(dy) * (b ? b->w : 1) ? H : V;
            if (!b) return from;
            Pt ce = b->centre();
            if (d == H) return {dx >= 0 ? b->x + b->w : b->x, ce.y};
            return {ce.x, dy >= 0 ? b->y + b->h : b->y};
        };
        if (pts.empty() && sb && tb && !sFixed && !tFixed) {   // (side by side or above each other: one straight segment if they overlap)
            double ox0 = std::max(sb->x, tb->x), ox1 = std::min(sb->x + sb->w, tb->x + tb->w);
            double oy0 = std::max(sb->y, tb->y), oy1 = std::min(sb->y + sb->h, tb->y + tb->h);
            bool apartV = tb->y >= sb->y + sb->h || sb->y >= tb->y + tb->h, apartH = tb->x >= sb->x + sb->w || sb->x >= tb->x + tb->w;
            if (ox1 > ox0 && apartV && !(es == "elbowEdgeStyle" && !vertical)) {
                double x = (ox0 + ox1) / 2;
                bool down = tb->y > sb->y;
                return {{x, down ? sb->y + sb->h : sb->y}, {x, down ? tb->y : tb->y + tb->h}};
            }
            if (oy1 > oy0 && apartH && !vertical) {
                double y = (oy0 + oy1) / 2;
                bool right = tb->x > sb->x;
                return {{right ? sb->x + sb->w : sb->x, y}, {right ? tb->x : tb->x + tb->w, y}};
            }
        }
        if (!sFixed) sp = facing(sb, sref, pts.empty() ? tref : pts.front(), sd);
        else if (sd == NONE) facing(nullptr, sp, pts.empty() ? tref : pts.front(), sd);
        if (!tFixed) tp = facing(tb, tref, pts.empty() ? sp : pts.back(), td);
        else if (td == NONE) facing(nullptr, tp, pts.empty() ? sp : pts.back(), td);
        std::vector<Pt> r{sp};
        Dir d = sd;
        for (Pt q : pts) {   // (to each waypoint: along the current direction, then across)
            Pt cur = r.back();
            if (d == H) {
                if (std::fabs(cur.y - q.y) > 1e-6) r.push_back({q.x, cur.y}), d = V;
            } else {
                if (std::fabs(cur.x - q.x) > 1e-6) r.push_back({cur.x, q.y}), d = H;
            }
            r.push_back(q);
        }
        Pt cur = r.back();
        if (d == H && td == H) {   // (a step halfway)
            if (std::fabs(cur.y - tp.y) > 1e-6) {
                double mx = (cur.x + tp.x) / 2;
                r.push_back({mx, cur.y});
                r.push_back({mx, tp.y});
            }
        } else if (d == V && td == V) {
            if (std::fabs(cur.x - tp.x) > 1e-6) {
                double my = (cur.y + tp.y) / 2;
                r.push_back({cur.x, my});
                r.push_back({tp.x, my});
            }
        } else if (d == H)
            r.push_back({tp.x, cur.y});
        else
            r.push_back({cur.x, tp.y});
        r.push_back(tp);
        return r;
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
        } else if (t == "dash" || t == "cross" || t == "ERone" || t == "ERmandOne") {
            Pt m{p.x - u.x * len / 2, p.y - u.y * len / 2};
            M({m.x + n.x * half, m.y + n.y * half});
            L({m.x - n.x * half, m.y - n.y * half});
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
        else retreat = t == "open" ? retreat : 0;
        it.stroke = colour;
        it.width = (float)sw;
        add(std::move(it), (float)sw);
        return retreat;
    }

    void drawEdge(Cell &c, std::vector<Pt> r) {
        const Style &s = c.style;
        double sw = std::clamp(num(s, "strokeWidth", 1), 0.0, 1000.0);
        uint32_t stroke = withOpacity(colour(s, "strokeColor", 0xff000000), num(s, "opacity", 100) * num(s, "strokeOpacity", 100) / 100);
        std::vector<Pt> full = r;   // (the labels along it, before the markers shorten it)
        if (stroke) {
            auto unit = [](Pt a, Pt b) {
                double dx = b.x - a.x, dy = b.y - a.y, l = std::hypot(dx, dy);
                return l > 0 ? Pt{dx / l, dy / l} : Pt{1, 0};
            };
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
            paint(p, nullptr, s, Box{}, true);
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
        Pt p = &lc == &edgeCell ? along(r, lc.relative ? lc.x : 0, lc.relative ? lc.y : 0) : along(r, lc.x, lc.y);
        if (lc.hasOffset) p.x += lc.offset.x, p.y += lc.offset.y;
        Style s = lc.style;
        Box b{p.x - lc.w / 2, p.y - lc.h / 2, lc.w, lc.h};
        if (&lc == &edgeCell || lc.w <= 0) {   // (no box: centred on the point)
            s["align"] = "center";
            s["verticalAlign"] = "middle";
            b = {p.x, p.y, 0, 0};
        }
        Box inner = &lc == &edgeCell || lc.w <= 0 ? b : labelBox(s, b);
        double wrap = str(s, "whiteSpace") == "wrap" && inner.w > 0 ? inner.w : 0;
        addLabel(layoutLabel(lc.value, s, inner, wrap));
    }

    void build() {
        for (Cell &c : cells) {
            if (!shown(c) || layer(c)) continue;
            if (c.vertex) {
                Cell *p = find(c.parent);
                if (p && p->edge) continue;   // (an edge's label: with the edge)
                vertex(c);
            } else if (c.edge) {
                edge(c);
                for (Cell &k : cells)   // (its label cells, after it)
                    if (k.parent == c.id && k.vertex && !k.value.empty() && shown(k) && c.route.size() >= 2) edgeLabel(k, c, c.route);
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
        if (wrapper) {   // UserObject / object: its id and label, the cell inside
            id = attr(n, "id");
            value = attr(n, "label");
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
        c.parent = attr(cell, "parent");
        c.source = attr(cell, "source");
        c.target = attr(cell, "target");
        c.vertex = attr(cell, "vertex") == "1";
        c.edge = attr(cell, "edge") == "1";
        c.visible = attr(cell, "visible") != "0" && attr(n, "visible") != "0";
        c.collapsed = attr(cell, "collapsed") == "1";
        c.style = parseStyle(attr(cell, "style"), c.edge);
        for (xmlNode *g = cell->children; g; g = g->next)
            if (is(g, "mxGeometry")) readGeometry(c, g);
        if (!c.id.empty()) b.byId[c.id] = b.cells.size();
        b.cells.push_back(std::move(c));
    }
}

bool readModel(xmlNode *model, Page &page) {
    page.background = parseColour(attr(model, "background"), 0);
    xmlNode *root = nullptr;
    for (xmlNode *k = model->children; k; k = k->next)
        if (is(k, "root")) root = k;
    if (!root) return false;
    Builder b(page);
    readCells(root, b);
    b.build();
    if (page.items.empty()) return false;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (const Item &it : page.items) {
        x0 = std::min(x0, (double)it.bx0), y0 = std::min(y0, (double)it.by0);
        x1 = std::max(x1, (double)it.bx1), y1 = std::max(y1, (double)it.by1);
    }
    const double m = 10;
    page.x = x0 - m, page.y = y0 - m, page.w = x1 - x0 + 2 * m, page.h = y1 - y0 + 2 * m;
    return page.w > 0 && page.h > 0 && page.w < 1e8 && page.h < 1e8;
}

xmlDocPtr parseXml(const char *data, size_t len) {
    if (len > (1u << 30)) return nullptr;
    return xmlReadMemory(data, (int)len, nullptr, nullptr, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_HUGE);
}

}   // namespace

struct nitrodio {
    std::vector<Page> pages;
    int background = 0;
};

nitrodio *nitrodio_load(const void *data, size_t len) {
    xmlDocPtr doc = parseXml((const char *)data, len);
    if (!doc) return nullptr;
    xmlNode *top = xmlDocGetRootElement(doc);
    std::unique_ptr<nitrodio> dio(new nitrodio);
    std::lock_guard<std::mutex> g(nf_lock());   // (the labels' faces)
    auto model = [&](xmlNode *m) {
        Page p;
        if (readModel(m, p)) dio->pages.push_back(std::move(p));
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
            if (!t) continue;
            std::string text((const char *)t), xml;
            xmlFree(t);
            std::string bin = base64(text);
            if (!inflateRaw(bin, xml)) xml = bin;
            if (xml.compare(0, 1, "<") != 0) xml = urlDecode(xml);
            else if (xml.compare(0, 3, "%3C") == 0) xml = urlDecode(xml);
            if (xml.compare(0, 3, "%3C") == 0 || xml.compare(0, 3, "%3c") == 0) xml = urlDecode(xml);
            xmlDocPtr inner = parseXml(xml.data(), xml.size());
            if (!inner) continue;
            xmlNode *ir = xmlDocGetRootElement(inner);
            if (ir && is(ir, "mxGraphModel")) model(ir);
            xmlFreeDoc(inner);
        }
    }
    xmlFreeDoc(doc);
    if (dio->pages.empty()) return nullptr;
    return dio.release();
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
void setColour(plutovg_canvas_t *c, uint32_t rgba) {
    plutovg_canvas_set_rgba(c, (rgba >> 16 & 255) / 255.f, (rgba >> 8 & 255) / 255.f, (rgba & 255) / 255.f, (rgba >> 24) / 255.f);
}
void drawItem(plutovg_canvas_t *c, const Item &it, float scale) {
    if (it.label) {
        const Label &l = *it.label;
        plutovg_canvas_save(c);
        if (l.turned) {
            plutovg_canvas_translate(c, l.cx, l.cy);
            plutovg_canvas_rotate(c, l.angle);
            plutovg_canvas_translate(c, -l.cx, -l.cy);
        }
        if (l.background >> 24) {
            setColour(c, l.background);
            plutovg_canvas_fill_rect(c, l.x0, l.y0, l.x1 - l.x0, l.y1 - l.y0);
        }
        for (const Run &r : l.runs) {
            setColour(c, r.rgba);
            plutovg_canvas_set_font(c, r.face, r.size);
            plutovg_canvas_fill_text(c, r.text.data(), (int)r.text.size(), PLUTOVG_TEXT_ENCODING_UTF8, r.x, r.y);
        }
        plutovg_canvas_restore(c);
        return;
    }
    if (it.fill >> 24) {
        if (it.fill2 >> 24) {
            plutovg_gradient_stop_t stops[2] = {{0, {(it.fill >> 16 & 255) / 255.f, (it.fill >> 8 & 255) / 255.f, (it.fill & 255) / 255.f, (it.fill >> 24) / 255.f}},
                                                {1, {(it.fill2 >> 16 & 255) / 255.f, (it.fill2 >> 8 & 255) / 255.f, (it.fill2 & 255) / 255.f, (it.fill2 >> 24) / 255.f}}};
            plutovg_canvas_set_linear_gradient(c, it.gx0, it.gy0, it.gx1, it.gy1, PLUTOVG_SPREAD_METHOD_PAD, stops, 2, nullptr);
        } else
            setColour(c, it.fill);
        plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
        plutovg_canvas_fill_path(c, it.path.get());
    }
    if (it.stroke >> 24) {
        setColour(c, it.stroke);
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
    if (!dio || page < 0 || page >= (int)dio->pages.size() || w <= 0 || h <= 0 || pitch < (size_t)w * 4 || !(scale > 0)) return -1;
    const Page &pg = dio->pages[(size_t)page];
    static const uint32_t backgrounds[3] = {0xffffffff, 0xff212830, 0xff000000};
    uint32_t bg = pg.background >> 24 ? pg.background : backgrounds[dio->background];
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(bgra + (size_t)y * pitch);
        for (int x = 0; x < w; x++) row[x] = bg;
    }
    plutovg_surface_t *surface = plutovg_surface_create_for_data(bgra, w, h, (int)pitch);
    plutovg_canvas_t *c = plutovg_canvas_create(surface);
    plutovg_canvas_scale(c, (float)scale, (float)scale);
    x0 += pg.x, y0 += pg.y;
    plutovg_canvas_translate(c, (float)-x0, (float)-y0);
    float vx0 = (float)x0, vy0 = (float)y0, vx1 = (float)(x0 + w / scale), vy1 = (float)(y0 + h / scale);
    size_t n = 0;
    int rc = 0;
    for (const Item &it : pg.items) {
        if ((++n & 255) == 0 && cancelled && cancelled(ctx)) {
            rc = 1;
            break;
        }
        if (it.bx1 >= vx0 && it.bx0 <= vx1 && it.by1 >= vy0 && it.by0 <= vy1) drawItem(c, it, (float)scale);
    }
    plutovg_canvas_destroy(c);
    plutovg_surface_destroy(surface);
    return rc;
}
