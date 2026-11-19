// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrotype: see nitrotype.h. Every page is PAGE_W x PAGE_H units (a landscape screen's shape). A
// page is laid out the first time it is drawn, into a display list in its own coordinates: paths
// to fill (the glyphs of a row of cells or of a line of text as one path) and hairlines to stroke
// (the cells' frame, the baselines and advance marks), each with what it is (its colour is the
// background's choice, at render time). The text of the page itself (title, labels) is set in a
// system sans-serif (nitro_font); the samples and the cells in the font shown.
#include "nitrotype.h"
#include "nitro_draw.h"
#include "nitro_font.h"
#include "nitro_ft.h"
#include "nitro_num.h"   // (numbers read and written the same in every locale)
#include "nitropdf.h"   // (nitropdf_glyph_unicode: a glyph name's character)
#include "nitrosvg.h"   // (a colour glyph's SVG document)
#include "nitro_xmltag.h"

#include FT_FONT_FORMATS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H
#include FT_ADVANCES_H
#include <zlib.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

const float PAGE_W = 1600, PAGE_H = 1000, MARGIN = 32;
const int COLS = 32;                                 // cells in a row
const float CELL_W = (PAGE_W - 2 * MARGIN) / COLS;   // 48
const float CELL_H = 56;                             // the glyph over its labels
const float CELL_BASE = 33, CELL_EM = 28;            // a cell's baseline from its top; the em there
const float CELL_DOWN = 8.5f;                        // the room under the baseline, to the labels
const float GRID_TOP = MARGIN + 22;                  // under a page's heading
const float SAMPLE_TOP = MARGIN + 62, SAMPLE_SIZE = 20, SAMPLE_PITCH = 27, TAG_W = 38;
const size_t MAX_FACES = 256, MAX_PAGES = 4096;

// Sample texts and selected additional letters, with Central European languages first. The tags
// document the languages represented; the short lists also show letters missing from the texts.
const struct {
    const char *tag, *text;
} SAMPLES[] = {
    {"eng", "the quick brown fox jumps over the lazy dog – THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG"},
    {"latin-1", "zwölf süße bären – è già lì, però non sa più perché – ÄÖÜ Çç Ææ Œœ Ññ ¡¿ Ëë Ïï – Åå Øø Ðð Þþ Ýý"},
    {"hun", "árvíztűrő tükörfúrógép – ÁRVÍZTŰRŐ TÜKÖRFÚRÓGÉP – 0123456789 @ & % ! ? ( ) [ ] { } € $ £"},
    {"pl ro", "zażółć gęślą jaźń – ZAŻÓŁĆ GĘŚLĄ JAŹŃ – șase țărănci cântă în cor – Ăă Ââ Îî Șș Țț"},
    {"cs sk", "příliš žluťoučký kůň úpěl ďábelské ódy – kŕdeľ šťastných ďatľov – Ďď Ěě Ňň Řř Ťť Ůů Ää Ĺĺ Ľľ Ôô Ŕŕ"},
    {"tr lv lt", "çığ düştü, şoför şaşırdı – Āā Čč Ēē Ģģ Īī Ķķ Ļļ Ņņ Šš Ūū Žž – Ąą Ęę Ėė Įį Ųų – Ç Ğ I İ Ö Ş Ü"},
    {"hr sl sr mk", "čađavi šešir na žućkastoj kući – črn maček čaka žabo v šašu – Č Ć Đ Š Ž – Ђђ Јј Љљ Њњ Ћћ Џџ – Ѓѓ Ѕѕ Ќќ"},
    {"cyrill", "съешь же ещё этих мягких французских булок, да выпей чаю – Ё Й Щ Ъ Э Ю Я – Ґґ Єє Іі Її – гласът Ѝѝ"},
    {"greek", "ξεσκεπάζω την ψυχοφθόρα βδελυγμία – Α Β Γ Δ Θ Λ Ξ Π Σ Φ Ψ Ω – ς Ά Έ Ή Ί Ό Ύ Ώ ΐ ΰ"},
};
const int NSAMPLES = sizeof SAMPLES / sizeof SAMPLES[0];
// the one line of a font with the English alphabet alone (no accented, Cyrillic or Greek letter): the digits and signs with it
const char SAMPLE_ASCII[] = "the quick brown fox jumps over the lazy dog – THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG – 0123456789 € $ £ @ & % ! ? ( ) [ ] { }";
// the rows of glyphs on a face's first page, under its sample lines (0: its glyphs under its name), and on each page after it
inline int rowsUnder(int lines) { return (int)((PAGE_H - MARGIN - (lines ? SAMPLE_TOP + lines * SAMPLE_PITCH + 6 : SAMPLE_TOP)) / CELL_H); }
const int ROWS_MORE = (int)((PAGE_H - MARGIN - GRID_TOP) / CELL_H);
// The pages of colour glyphs, after a face's outlines: the pictures of its SVG table, each in a cell
// twice as large; the pictures of SVG_KEEP such pages stay loaded (a font has thousands).
const int SVG_COLS = COLS / 2, SVG_ROWS = ROWS_MORE / 2;
const float SVG_W = 2 * CELL_W, SVG_H = 2 * CELL_H;
const size_t SVG_KEEP = 4;

enum Role : uint8_t { TEXT, LABEL, FRAME, MARK, MISSING };

struct Item {
    plutovg_path_t *path = nullptr;
    Role role = TEXT;
    bool stroke = false;
    nd::Box box;
};

struct Picture {   // a colour glyph on its page: which of the face's SVG glyphs, and the box its picture is fitted into
    uint32_t index;
    float x, y, w, h;
};
struct Pictures {   // a page's SVG documents, loaded (NULL: one lunasvg does not read)
    std::vector<nitrosvg *> docs;
    ~Pictures() {
        for (nitrosvg *d : docs)
            if (d) nitrosvg_free(d);
    }
};

struct PageDL {
    std::atomic<bool> built{false};
    std::vector<Item> items;
    std::vector<Picture> pictures;
    std::shared_ptr<Pictures> loaded;   // (under the font's lock; a render holds its own reference)
    ~PageDL() {
        for (Item &it : items) plutovg_path_destroy(it.path);
    }
};

struct Face {
    FT_Face ft = nullptr;
    int index = 0;                // (in its collection)
    std::vector<uint32_t> uni;    // a glyph's character (the first one mapped to it), 0: none
    size_t chars = 0;             // the characters the font maps
    // The characters it maps no code to but has a glyph named for (by the glyph list: A, aacute,
    // uni0151): that glyph. Such a font cannot be typed with, but its letters are there to show.
    std::unordered_map<uint32_t, uint32_t> named;
    FT_UInt glyph(uint32_t cp) const {   // a character's glyph, 0: none
        FT_UInt g = FT_Get_Char_Index(ft, cp);
        if (g || named.empty()) return g;
        auto it = named.find(cp);
        return it != named.end() ? it->second : 0;
    }
    // The sample lines it shows: all of them (NSAMPLES) if it has letters beyond the English alphabet
    // (what it lacks is red); the English one alone (1) with that alphabet only; none (0) without it
    // either: an icon or symbol font, or another script's.
    int samples = NSAMPLES;
    int headRows() const { return rowsUnder(samples); }   // the rows of glyphs on its first page
    // Its colour glyphs: the SVG table (OpenType's glyphs as SVG pictures, beside their plain outlines)
    // and the glyphs with a document of their own in it, where in the table it is.
    struct SvgGlyph {
        uint32_t glyph, off, len;
    };
    std::string svgTable;
    std::vector<SvgGlyph> svg;
};

struct Page {
    int face;
    uint32_t first, count;   // its glyphs (a page of colour glyphs: of the face's SVG glyphs)
    bool head;               // the face's first page: its name and the samples
    bool svg;                // a page of colour glyphs
};

// a letter: what a sample line's coverage is counted by (not the digits, signs and dashes every font has)
inline bool isLetter(uint32_t cp) {
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || (cp >= 0xC0 && !(cp >= 0x2000 && cp < 0x2C00));   // (not U+2013, the euro sign)
}
// Whether the font has at least half of a text's letters, drawn (an icon font may map a to z to empty
// glyphs: its icons are ligatures of their names); special: of those beyond the English alphabet (the
// accented, Cyrillic and Greek ones: a text with none of them is not covered).
bool covers(const Face &face, const char *text, bool special) {
    FT_Face ft = face.ft;
    std::string s(text);
    int total = 0, missing = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = nf_utf8_next(s, i);
        if (!isLetter(cp) || (special && cp < 0x80)) continue;
        total++;
        FT_UInt gid = face.glyph(cp);
        FT_BBox box = {0, 0, 0, 0};
        if (gid && !FT_Load_Glyph(ft, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) && ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE && ft->glyph->outline.n_points > 0)
            FT_Outline_Get_CBox(&ft->glyph->outline, &box);
        if (box.xMax <= box.xMin || box.yMax <= box.yMin) missing++;   // (no glyph, or one with nothing in it: points on one spot)
    }
    return total > 0 && missing * 2 <= total;
}

inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline void put32(std::string &o, size_t at, uint32_t v) {
    o[at] = (char)(v >> 24), o[at + 1] = (char)(v >> 16), o[at + 2] = (char)(v >> 8), o[at + 3] = (char)v;
}

// A WOFF file (1.0) as the sfnt it wraps: its tables inflated, each at a 4-byte boundary, under a
// directory of their new places. false: not one, or damaged.
bool unwoff(const uint8_t *d, size_t n, std::string &out) {
    const size_t LIMIT = 256u << 20;
    if (n < 44 || memcmp(d, "wOFF", 4)) return false;
    uint32_t tables = d[12] << 8 | d[13];
    if (!tables || 44 + 20ull * tables > n) return false;
    size_t dir = 12 + 16 * (size_t)tables;
    out.assign(dir, '\0');
    memcpy(&out[0], d + 4, 4);   // (the flavor: the sfnt's version)
    uint32_t pow2 = 1, log2 = 0;
    while (pow2 * 2 <= tables) pow2 *= 2, log2++;
    out[4] = (char)(tables >> 8), out[5] = (char)tables;
    out[6] = (char)(pow2 * 16 >> 8), out[7] = (char)(pow2 * 16);
    out[8] = (char)(log2 >> 8), out[9] = (char)log2;
    out[10] = (char)((tables - pow2) * 16 >> 8), out[11] = (char)((tables - pow2) * 16);
    for (uint32_t k = 0; k < tables; k++) {
        const uint8_t *e = d + 44 + 20 * (size_t)k;
        uint32_t off = be32(e + 4), comp = be32(e + 8), orig = be32(e + 12);
        if (off > n || comp > n - off || orig > LIMIT || out.size() + orig > LIMIT || comp > orig) return false;
        size_t at = out.size();
        out.resize(at + (((size_t)orig + 3) & ~(size_t)3), '\0');
        if (comp == orig) memcpy(&out[at], d + off, orig);
        else {
            uLongf got = orig;
            if (uncompress((Bytef *)&out[at], &got, d + off, comp) != Z_OK || got != orig) return false;
        }
        size_t rec = 12 + 16 * (size_t)k;
        memcpy(&out[rec], e, 4);              // the tag
        memcpy(&out[rec + 4], e + 16, 4);     // its checksum
        put32(out, rec + 8, (uint32_t)at);
        put32(out, rec + 12, orig);
    }
    return true;
}

// An Embedded OpenType file's font: the last FontDataSize bytes, after a header of little-endian
// fields and names (its magic 0x504C at 34), XORed with 0x50 if the flags say so. false: not one,
// or its font is compressed (MicroType Express: not read).
bool uneot(const uint8_t *d, size_t n, std::string &out) {
    if (n < 82 || d[34] != 0x4c || d[35] != 0x50) return false;
    auto le32 = [&](size_t at) { return (uint32_t)d[at] | d[at + 1] << 8 | d[at + 2] << 16 | (uint32_t)d[at + 3] << 24; };
    uint32_t size = le32(0), font = le32(4), flags = le32(12);
    if (size != n || font < 12 || font > n - 82 || (flags & 0x4)) return false;
    out.assign((const char *)d + (n - font), font);
    if (flags & 0x10000000)
        for (char &c : out) c ^= 0x50;
    return true;
}

// --- an SVG font, as the Type 1 font FreeType reads ---------------------------------------------

// a number of a Type 1 charstring
void t1num(std::string &o, int v) {
    if (v >= -107 && v <= 107) o += (char)(v + 139);
    else if (v >= 108 && v <= 1131) v -= 108, o += (char)(247 + (v >> 8)), o += (char)v;
    else if (v >= -1131 && v <= -108) v = -v - 108, o += (char)(251 + (v >> 8)), o += (char)v;
    else o += (char)255, o += (char)(v >> 24), o += (char)(v >> 16), o += (char)(v >> 8), o += (char)v;
}

// Type 1's encryption (the charstrings' with r = 4330, the private part's with 55665): four bytes go before the text
std::string t1crypt(const std::string &plain, unsigned r) {
    std::string o;
    o.reserve(plain.size() + 4);
    for (size_t i = 0; i < plain.size() + 4; i++) {
        uint8_t c = (uint8_t)((i < 4 ? 0 : (uint8_t)plain[i - 4]) ^ (r >> 8));
        r = ((c + r) * 52845u + 22719u) & 0xffff;
        o += (char)c;
    }
    return o;
}

// a glyph's outline (an SVG path's data, in the font's units, y up) as a charstring after its width
struct T1Path {
    std::string *o;
    int x = 0, y = 0;      // the current point (closing a contour does not move it: Type 1's rule)
    float sx = 0, sy = 0;  // where the open contour began
    bool open = false;
    static int unit(float v) { return !(v > -30000) ? -30000 : v > 30000 ? 30000 : (int)lroundf(v); }
    void to(float fx, float fy, int op) {
        int nx = unit(fx), ny = unit(fy);
        t1num(*o, nx - x), t1num(*o, ny - y), *o += (char)op;
        x = nx, y = ny;
    }
    void begin(float fx, float fy) {
        if (open) *o += (char)9;   // closepath
        to(fx, fy, 21);            // rmoveto
        sx = fx, sy = fy, open = true;
    }
    static void step(void *closure, plutovg_path_command_t cmd, const plutovg_point_t *p, int) {
        T1Path &t = *(T1Path *)closure;
        switch (cmd) {
        case PLUTOVG_PATH_COMMAND_MOVE_TO:
            t.begin(p[0].x, p[0].y);
            break;
        case PLUTOVG_PATH_COMMAND_LINE_TO:
            if (!t.open) t.begin(t.sx, t.sy);
            if (unit(p[0].x) != t.x || unit(p[0].y) != t.y) t.to(p[0].x, p[0].y, 5);   // rlineto
            break;
        case PLUTOVG_PATH_COMMAND_CUBIC_TO: {
            if (!t.open) t.begin(t.sx, t.sy);
            int c[6] = {unit(p[0].x), unit(p[0].y), unit(p[1].x), unit(p[1].y), unit(p[2].x), unit(p[2].y)};
            t1num(*t.o, c[0] - t.x), t1num(*t.o, c[1] - t.y), t1num(*t.o, c[2] - c[0]), t1num(*t.o, c[3] - c[1]);
            t1num(*t.o, c[4] - c[2]), t1num(*t.o, c[5] - c[3]), *t.o += (char)8;   // rrcurveto
            t.x = c[4], t.y = c[5];
            break;
        }
        case PLUTOVG_PATH_COMMAND_CLOSE:
            if (t.open) *t.o += (char)9;
            t.open = false;
            break;
        }
    }
};

// An SVG font (SVG 1.1's <font>: a webfont kit's fourth file, an icon set) as a Type 1 font: its
// glyphs' paths as charstrings in the font's own units, named by their character (uniXXXX; the
// glyph's own name where that names the same character), so that FreeType maps them. false: not
// an SVG document that is a font and nothing else (a drawing with a font in it stays a drawing).
bool unsvg(const uint8_t *d, size_t n, std::string &out) {
    const char *s = (const char *)d;
    auto has = [&](const char *what) {
        size_t m = strlen(what);
        for (size_t i = 0; i + m <= n; i++)
            if (s[i] == what[0] && !memcmp(s + i, what, m)) return true;
        return false;
    };
    size_t at = n >= 3 && !memcmp(s, "\xef\xbb\xbf", 3) ? 3 : 0;
    while (at < n && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n')) at++;
    if (at >= n || s[at] != '<' || !has("<font") || !has("<glyph")) return false;
    struct Glyph {
        std::string name, d;
        float adv;
    };
    std::vector<Glyph> glyphs(1);   // (0: the missing glyph's)
    std::unordered_set<std::string> names{".notdef"};
    std::string family, weight = "Regular";
    float upm = 1000, ascent = 0, descent = 0, adv = -1;
    bool haveAscent = false, haveDescent = false, inFont = false, done = false, root = false;
    auto number = [](const std::string *v, float otherwise) {
        if (!v || v->empty()) return otherwise;
        char *e = nullptr;
        float f = (float)nn_strtod(v->c_str(), &e);
        return e == v->c_str() || !(f > -1e6f && f < 1e6f) ? otherwise : f;
    };
    nxt::Tag t;
    while (nxt::next(s, n, at, t)) {
        const std::string &nm = t.name;
        if (!root) {
            if (t.closing || nm != "svg") return false;
            root = true;
            continue;
        }
        if (nm == "metadata" && !t.closing && !t.empty) {   // (whatever it holds: not ours)
            for (nxt::Tag m; nxt::next(s, n, at, m) && !(m.closing && m.name == "metadata");) {}
            continue;
        }
        if (nm == "font") {
            if (t.closing) inFont = false, done = true;
            else if (!done) {
                inFont = !t.empty, adv = number(t.attr("horiz-adv-x"), -1);
                if (const std::string *v = t.attr("id")) family = *v;   // (its name, unless its font-face has one)
            }
            continue;
        }
        if (nm == "svg" || nm == "defs" || nm == "title" || nm == "desc" || nm == "hkern" || nm == "vkern" || nm == "font-face-src" ||
            nm == "font-face-name" || nm == "font-face-uri" || nm == "font-face-format")
            continue;
        if (t.closing && (nm == "glyph" || nm == "missing-glyph" || nm == "font-face")) continue;
        if (nm == "font-face") {
            if (!inFont) continue;
            if (const std::string *v = t.attr("font-family"))
                if (!v->empty()) family = *v;
            upm = number(t.attr("units-per-em"), 1000);
            if (!(upm >= 16 && upm <= 16384)) upm = 1000;
            if (t.attr("ascent")) ascent = number(t.attr("ascent"), 0), haveAscent = true;
            if (t.attr("descent")) descent = number(t.attr("descent"), 0), haveDescent = true;
            if (const std::string *v = t.attr("font-weight"))
                if (*v == "bold" || *v == "bolder" || atoi(v->c_str()) >= 600) weight = "Bold";
            continue;
        }
        if (nm == "glyph" || nm == "missing-glyph") {
            if (!inFont) continue;
            Glyph g;
            g.adv = number(t.attr("horiz-adv-x"), -1);
            if (const std::string *v = t.attr("d")) g.d = *v;
            if (nm == "missing-glyph") {
                glyphs[0].d = g.d, glyphs[0].adv = g.adv;
                continue;
            }
            if (glyphs.size() >= 65000) continue;
            // its name: by its one character; a ligature's or an unmapped one's: its own, or its number
            const std::string *uni = t.attr("unicode"), *own = t.attr("glyph-name");
            uint32_t cp = 0;
            if (uni && !uni->empty()) {
                size_t i = 0;
                uint32_t c = nf_utf8_next(*uni, i);
                if (i >= uni->size() && c >= 0x20 && c != 0xfffd) cp = c;
            }
            std::string clean;
            if (own)
                for (char c : *own)
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') clean += c;
            if (clean.size() > 60 || (!clean.empty() && clean[0] >= '0' && clean[0] <= '9')) clean.clear();
            char b[24];
            if (cp) {
                snprintf(b, sizeof b, cp < 0x10000 ? "uni%04X" : "u%05X", (unsigned)cp);
                g.name = !clean.empty() && nitropdf_glyph_unicode(clean.c_str()) == cp ? clean : b;
            } else if (!clean.empty() && !nitropdf_glyph_unicode(clean.c_str()))
                g.name = clean;
            if (g.name.empty() || !names.insert(g.name).second) {
                snprintf(b, sizeof b, "glyph%zu", glyphs.size());
                g.name = b;
                names.insert(g.name);
            }
            glyphs.push_back(std::move(g));
            continue;
        }
        return false;   // (something that is drawn, or that we do not know: not a font file)
    }
    if (glyphs.size() < 2) return false;
    if (adv < 0) adv = upm;
    if (!haveAscent) ascent = upm * 0.8f;
    if (!haveDescent) descent = ascent - upm;
    if (descent > 0) descent = -descent;   // (some write it as a distance)
    if (family.empty()) family = "SVG font";
    std::string psname;
    for (char c : family)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') psname += c;
    if (psname.empty()) psname = "SVGFont";
    std::string info;
    for (char c : family) {
        if (c == '(' || c == ')' || c == '\\') info += '\\';
        if ((unsigned char)c >= 0x20) info += c;
    }
    // the private part: the charstrings (each: its width, its contours, the end)
    std::string priv = "dup /Private 8 dict dup begin\n/RD {string currentfile exch readstring pop} executeonly def\n"
                       "/ND {noaccess def} executeonly def\n/NP {noaccess put} executeonly def\n/BlueValues [] def\n"
                       "/MinFeature {16 16} def\n/password 5839 def\n2 index /CharStrings " +
                       std::to_string(glyphs.size()) + " dict dup begin\n";
    float widest = adv;
    for (size_t k = 0; k < glyphs.size(); k++) {
        const Glyph &g = glyphs[k];
        float w = g.adv >= 0 ? g.adv : adv;
        if (w > widest) widest = w;
        std::string cs;
        t1num(cs, 0), t1num(cs, T1Path::unit(w)), cs += (char)13;   // hsbw
        if (!g.d.empty()) {
            plutovg_path_t *path = plutovg_path_create();
            if (g.d.size() < (8u << 20) && plutovg_path_parse(path, g.d.data(), (int)g.d.size())) {
                T1Path tp{&cs};
                plutovg_path_traverse(path, T1Path::step, &tp);
                if (tp.open) cs += (char)9;
            }
            plutovg_path_destroy(path);
        }
        cs += (char)14;   // endchar
        std::string coded = t1crypt(cs, 4330);
        priv += "/" + (k ? g.name : std::string(".notdef")) + " " + std::to_string(coded.size()) + " RD " + coded + " ND\n";
    }
    priv += "end\nend\nreadonly put\nnoaccess put\ndup /FontName get exch definefont pop\nmark currentfile closefile\n";
    char b[256];
    out = "%!PS-AdobeFont-1.0: " + psname + " 001.000\n12 dict begin\n/FontInfo 4 dict dup begin\n/FamilyName (" + info +
          ") readonly def\n/FullName (" + info + ") readonly def\n/Weight (" + weight + ") readonly def\n/ItalicAngle 0 def\nend readonly def\n/FontName /" +
          psname + " def\n/PaintType 0 def\n/FontType 1 def\n";
    snprintf(b, sizeof b, "/FontMatrix [%.9g 0 0 %.9g 0 0] readonly def\n/Encoding StandardEncoding def\n/FontBBox {0 %d %d %d} readonly def\n",
             1.0 / upm, 1.0 / upm, T1Path::unit(descent), T1Path::unit(widest), T1Path::unit(ascent));
    nn_cpoint(b);
    out += b;
    out += "currentdict end\ncurrentfile eexec\n";
    std::string coded = t1crypt(priv, 55665);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < coded.size(); i++) {
        out += hex[(uint8_t)coded[i] >> 4], out += hex[coded[i] & 15];
        if (i % 32 == 31) out += '\n';
    }
    out += '\n';
    return true;
}

}   // namespace

struct nitrotype {
    std::string data;   // the font file (a WOFF's sfnt): FreeType reads it in place
    const char *wrapper = "";   // "WOFF: ", "WOFF2: ", "EOT: ": what the font came in
    bool svgFont = false;       // an SVG font, read as the Type 1 font made of it
    FT_Library lib = nullptr;
    std::vector<Face> faces;
    std::vector<Page> pages;
    std::vector<std::unique_ptr<PageDL>> dl;
    int background = 0;
    std::mutex lock;   // (FreeType's faces: one page laid out at a time)
    std::vector<int> svgPages;   // the pages of colour glyphs whose pictures are loaded, the latest used last
    ~nitrotype() {
        for (Face &f : faces)
            if (f.ft) FT_Done_Face(f.ft);
        if (lib) FT_Done_FreeType(lib);
    }
};

namespace {

// Lays a page out. Held throughout: the font's lock, and nitro_font's (the system fonts' index).
struct Layout {
    nitrotype &F;
    PageDL &pg;
    Face &face;
    plutovg_font_face_t *ui, *uiBold;
    float upm;

    Layout(nitrotype &f, PageDL &p, Face &fc) : F(f), pg(p), face(fc) {
        ui = nf_css_face("sans-serif", false, false);
        uiBold = nf_css_face("sans-serif", true, false);
        if (!uiBold) uiBold = ui;
        upm = face.ft->units_per_EM > 0 ? (float)face.ft->units_per_EM : 1000.f;
    }

    void add(plutovg_path_t *path, Role role, bool stroke = false) {
        if (plutovg_path_get_elements(path, nullptr) == 0) {
            plutovg_path_destroy(path);
            return;
        }
        Item it;
        it.path = path, it.role = role, it.stroke = stroke;
        it.box = nd::pathBox(path, 0);
        pg.items.push_back(it);
    }

    // A line in the page's own font (a system one), from x on the baseline y; a character it lacks
    // from whichever system font has it. path NULL: only measured. Returns its width.
    float uiText(plutovg_path_t *path, plutovg_font_face_t *f, float x, float y, float size, const std::string &s) {
        float x0 = x;
        for (size_t i = 0; i < s.size();) {
            uint32_t cp = nf_utf8_next(s, i);
            plutovg_font_face_t *g = f && nf_has_glyph(f, cp) ? f : nf_glyph_face(nullptr, cp, 400, false);
            if (!g) g = f;
            if (!g) {
                x += size / 2;
                continue;
            }
            if (path) x += plutovg_font_face_get_glyph_path(g, size, x, y, cp, path);
            else {
                float adv = 0;
                plutovg_font_face_get_glyph_metrics(g, size, cp, &adv, nullptr, nullptr);
                x += adv;
            }
        }
        return x - x0;
    }
    void uiCentered(plutovg_path_t *path, float cx, float y, float size, const std::string &s, float maxw) {
        float w = uiText(nullptr, ui, 0, 0, size, s);
        if (w > maxw && w > 0) size *= maxw / w, w = maxw;   // (a long glyph name: smaller)
        uiText(path, ui, cx - w / 2, y, size, s);
    }

    // A glyph of the font shown: its outline appended to path under m (font units, y up, to the
    // page), if path is given; its advance and its outline's box in font units. false: no such glyph.
    bool glyph(uint32_t gid, plutovg_path_t *path, const plutovg_matrix_t *m, float *advance, FT_BBox *box) {
        if (FT_Load_Glyph(face.ft, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP)) return false;
        FT_GlyphSlot g = face.ft->glyph;
        if (advance) *advance = (float)g->advance.x;
        if (box) box->xMin = box->yMin = box->xMax = box->yMax = 0;
        if (g->format != FT_GLYPH_FORMAT_OUTLINE || g->outline.n_points <= 0) return true;   // (a space; a bitmap-only glyph)
        if (box) FT_Outline_Get_CBox(&g->outline, box);   // (the control points' box: never smaller than the outline's)
        if (path) {
            plutovg_path_t *one = plutovg_path_create();
            nft::outlinePath(&g->outline, one, 1);
            plutovg_path_add_path(path, one, m);
            plutovg_path_destroy(one);
        }
        return true;
    }

    // A line of text in the font shown, size units to the em, from x on the baseline y: its glyphs
    // into ok; the characters it has no glyph for into miss, from a system font. Paths NULL: only
    // measured. Returns the width.
    float fontText(plutovg_path_t *ok, plutovg_path_t *miss, float x, float y, float size, const std::string &s) {
        float x0 = x, k = size / upm;
        FT_UInt prev = 0;
        for (size_t i = 0; i < s.size();) {
            uint32_t cp = nf_utf8_next(s, i);
            FT_UInt gid = face.glyph(cp);
            if (gid) {
                if (prev && FT_HAS_KERNING(face.ft)) {
                    FT_Vector kern = {0, 0};
                    if (!FT_Get_Kerning(face.ft, prev, gid, FT_KERNING_UNSCALED, &kern)) x += kern.x * k;
                }
                plutovg_matrix_t m;
                plutovg_matrix_init(&m, k, 0, 0, -k, x, y);
                float adv = 0;
                if (glyph(gid, ok, &m, &adv, nullptr)) x += adv * k;
                prev = gid;
                continue;
            }
            prev = 0;
            if (cp == ' ') {   // (a font with no space: half an em)
                x += size / 2;
                continue;
            }
            plutovg_font_face_t *g = ui && nf_has_glyph(ui, cp) ? ui : nf_glyph_face(nullptr, cp, 400, false);
            if (!g) {   // (no system font has it either: the font's own "missing glyph", glyph 0)
                plutovg_matrix_t m;
                plutovg_matrix_init(&m, k, 0, 0, -k, x, y);
                float adv = 0;
                x += glyph(0, miss, &m, &adv, nullptr) && adv > 0 ? adv * k : size / 2;
            } else if (miss) x += plutovg_font_face_get_glyph_path(g, size, x, y, cp, miss);
            else {
                float adv = 0;
                plutovg_font_face_get_glyph_metrics(g, size, cp, &adv, nullptr, nullptr);
                x += adv;
            }
        }
        return x - x0;
    }

    // a string of the font's name table (UTF-16BE on the Microsoft platform, else as Latin-1): the English one first
    std::string name(unsigned id) {
        std::string best;
        int bestRank = 0;
        FT_UInt n = FT_Get_Sfnt_Name_Count(face.ft);
        for (FT_UInt k = 0; k < n && k < 4096; k++) {
            FT_SfntName nm;
            if (FT_Get_Sfnt_Name(face.ft, k, &nm) || nm.name_id != id || !nm.string_len) continue;
            bool utf16 = nm.platform_id == TT_PLATFORM_MICROSOFT || nm.platform_id == TT_PLATFORM_APPLE_UNICODE;
            int rank = utf16 ? (nm.language_id == 0x409 ? 3 : 2) : 1;
            if (rank <= bestRank) continue;
            std::string s;
            for (FT_UInt i = 0; i + (utf16 ? 1 : 0) < nm.string_len && s.size() < 200; i += utf16 ? 2 : 1) {
                uint32_t c = utf16 ? (uint32_t)(nm.string[i] << 8 | nm.string[i + 1]) : nm.string[i];
                if (c >= 32) nf_utf8_put(s, c);
            }
            best = s, bestRank = rank;
        }
        return best;
    }

    std::string faceName() {
        std::string s = face.ft->family_name ? face.ft->family_name : "";
        if (face.ft->style_name && *face.ft->style_name) s += (s.empty() ? "" : " ") + std::string(face.ft->style_name);
        if (s.empty()) s = name(TT_NAME_ID_FULL_NAME);
        return s.empty() ? "(unnamed font)" : s;
    }

    // the heading of every page: the face, and which of its glyphs the page shows
    void heading(const Page &p) {
        char b[160];
        std::string s = faceName();
        snprintf(b, sizeof b, "   glyphs %u – %u of %ld", p.first, p.first + p.count - 1, (long)face.ft->num_glyphs);
        s += b;
        if (F.faces.size() > 1) {
            snprintf(b, sizeof b, "   face %d of the collection", face.index + 1);
            s += b;
        }
        plutovg_path_t *path = plutovg_path_create();
        uiText(path, ui, MARGIN, MARGIN + 9, 10, s);
        add(path, LABEL);
    }

    // a face's first page: its name, what it is, and the sample lines
    void head() {
        plutovg_path_t *title = plutovg_path_create(), *info = plutovg_path_create();
        uiText(title, uiBold, MARGIN, MARGIN + 30, 30, faceName());
        add(title, TEXT);
        const char *fmt = FT_Get_Font_Format(face.ft);
        std::string s = F.wrapper;
        s += F.svgFont ? "SVG font" : fmt ? fmt : "font";
        if (FT_IS_SFNT(face.ft) && fmt && !strcmp(fmt, "CFF")) s = F.wrapper + std::string("OpenType (CFF)");
        if (FT_HAS_MULTIPLE_MASTERS(face.ft)) s += ", variable (its default instance)";
        char b[200];
        snprintf(b, sizeof b, "   ·   %ld glyphs   ·   %zu characters   ·   %d units per em", (long)face.ft->num_glyphs, face.chars, (int)upm);
        s += b;
        if (!face.svg.empty()) {
            snprintf(b, sizeof b, "   ·   %zu colour glyphs (SVG, on the last pages)", face.svg.size());
            s += b;
        }
        if (F.faces.size() > 1) {
            snprintf(b, sizeof b, "   ·   face %d of %zu in the collection", face.index + 1, F.faces.size());
            s += b;
        }
        std::string ver = name(TT_NAME_ID_VERSION_STRING);
        if (!ver.empty()) s += "   ·   " + ver.substr(0, 80);
        float w = uiText(nullptr, ui, 0, 0, 11, s), size = w > PAGE_W - 2 * MARGIN ? 11 * (PAGE_W - 2 * MARGIN) / w : 11;
        uiText(info, ui, MARGIN, MARGIN + 48, size, s);
        add(info, LABEL);
        if (!face.samples) return;   // (an icon font: lines all red say nothing)

        plutovg_path_t *tags = plutovg_path_create();
        const float maxw = PAGE_W - 2 * MARGIN - TAG_W;
        for (int k = 0; k < face.samples; k++) {
            const char *text = face.samples == 1 ? SAMPLE_ASCII : SAMPLES[k].text;
            float y = SAMPLE_TOP + k * SAMPLE_PITCH + SAMPLE_SIZE * 0.78f, x = MARGIN + TAG_W;
            uiText(tags, ui, MARGIN, y, 8, SAMPLES[k].tag);
            // (every character the font lacks in red, a whole line too: the languages it does not cover show at a glance)
            float tw = fontText(nullptr, nullptr, 0, 0, SAMPLE_SIZE, text);
            float size = tw > maxw ? SAMPLE_SIZE * maxw / tw : SAMPLE_SIZE;
            plutovg_path_t *ok = plutovg_path_create(), *miss = plutovg_path_create();
            fontText(ok, miss, x, y, size, text);
            add(ok, TEXT);
            add(miss, MISSING);
        }
        add(tags, LABEL);
    }

    // a cell's labels around cx: the glyph's number and its character on the baseline y1, its name on y2
    void label(plutovg_path_t *labels, uint32_t gid, float cx, float y1, float size1, float y2, float size2, float maxw) {
        char b[80];
        uint32_t u = gid < face.uni.size() ? face.uni[gid] : 0;
        if (u) snprintf(b, sizeof b, "%u   U+%04X", gid, u);
        else snprintf(b, sizeof b, "%u", gid);
        uiCentered(labels, cx, y1, size1, b, maxw);
        char nm[64] = "";
        if (FT_HAS_GLYPH_NAMES(face.ft) && gid < (uint32_t)face.ft->num_glyphs && !FT_Get_Glyph_Name(face.ft, gid, nm, sizeof nm) && nm[0])
            uiCentered(labels, cx, y2, size2, nm, maxw);
    }

    // A page of colour glyphs: its heading, the frame of its cells, each glyph's labels, and the boxes
    // the pictures go into. (The render loads and draws them: nitrosvg takes nitro_font's lock itself.)
    void pictures(const Page &p) {
        char b[160];
        std::string s = faceName();
        snprintf(b, sizeof b, "   colour glyphs (SVG) %u – %u of %zu", p.first + 1, p.first + p.count, face.svg.size());
        s += b;
        plutovg_path_t *head = plutovg_path_create(), *frame = plutovg_path_create(), *labels = plutovg_path_create();
        uiText(head, ui, MARGIN, MARGIN + 9, 10, s);
        add(head, LABEL);
        for (uint32_t i = 0; i < p.count; i++) {
            float x = MARGIN + (i % SVG_COLS) * SVG_W, y = GRID_TOP + (i / SVG_COLS) * SVG_H;
            plutovg_path_move_to(frame, x, y + SVG_H);
            plutovg_path_line_to(frame, x, y);
            plutovg_path_line_to(frame, x + SVG_W, y);
            if (i % SVG_COLS == SVG_COLS - 1 || i + 1 == p.count) plutovg_path_line_to(frame, x + SVG_W, y + SVG_H);
            if (i + SVG_COLS >= p.count) {
                plutovg_path_move_to(frame, x, y + SVG_H);
                plutovg_path_line_to(frame, x + SVG_W, y + SVG_H);
            }
            pg.pictures.push_back({p.first + i, x + 8, y + 7, SVG_W - 16, SVG_H - 36});
            label(labels, face.svg[p.first + i].glyph, x + SVG_W / 2, y + SVG_H - 16, 6.5f, y + SVG_H - 7.5f, 4.6f, SVG_W - 6);
        }
        add(frame, FRAME, true);
        add(labels, LABEL);
    }

    // The glyphs p.first .. of the page in cells from top down: the frame, each cell's baseline with
    // the glyph's origin and advance marked, the glyph (made smaller if it would not fit its cell),
    // and under it its number, its character and its name.
    void grid(const Page &p, float top) {
        int rows = (int)((p.count + COLS - 1) / COLS);
        plutovg_path_t *frame = plutovg_path_create(), *marks = plutovg_path_create();
        for (uint32_t i = 0; i < p.count; i++) {   // (each cell's top and left edge; its right and bottom one where no cell follows)
            float x = MARGIN + (i % COLS) * CELL_W, y = top + (i / COLS) * CELL_H;
            plutovg_path_move_to(frame, x, y + CELL_H);
            plutovg_path_line_to(frame, x, y);
            plutovg_path_line_to(frame, x + CELL_W, y);
            if (i % COLS == COLS - 1 || i + 1 == p.count) plutovg_path_line_to(frame, x + CELL_W, y + CELL_H);
            if (i + COLS >= p.count) {
                plutovg_path_move_to(frame, x, y + CELL_H);
                plutovg_path_line_to(frame, x + CELL_W, y + CELL_H);
            }
        }
        add(frame, FRAME, true);

        float k0 = CELL_EM / upm;
        for (int r = 0; r < rows; r++) {
            plutovg_path_t *glyphs = plutovg_path_create(), *labels = plutovg_path_create();
            for (int c = 0; c < COLS; c++) {
                uint32_t i = (uint32_t)r * COLS + c;
                if (i >= p.count) break;
                uint32_t gid = p.first + i;
                float cx = MARGIN + c * CELL_W, cy = top + r * CELL_H, base = cy + CELL_BASE;
                float adv = 0;
                FT_BBox bb;
                if (glyph(gid, nullptr, nullptr, &adv, &bb)) {
                    // (smaller where it would leave its cell: wider, taller or deeper than the room it has)
                    float k = k0, wide = (bb.xMax - bb.xMin) * k0, up = bb.yMax * k0, down = -bb.yMin * k0;
                    if (wide > CELL_W - 4) k = std::min(k, k0 * (CELL_W - 4) / wide);
                    if (up > CELL_BASE - 2) k = std::min(k, k0 * (CELL_BASE - 2) / up);
                    if (down > CELL_DOWN) k = std::min(k, k0 * CELL_DOWN / down);
                    // its advance centred in the cell; an outline reaching out of the cell so: its box centred
                    float ox = cx + (CELL_W - adv * k) / 2;
                    if (bb.xMax > bb.xMin && (ox + bb.xMin * k < cx + 2 || ox + bb.xMax * k > cx + CELL_W - 2 || adv <= 0))
                        ox = cx + CELL_W / 2 - (bb.xMin + bb.xMax) / 2 * k;
                    plutovg_matrix_t m;
                    plutovg_matrix_init(&m, k, 0, 0, -k, ox, base);
                    glyph(gid, glyphs, &m, nullptr, nullptr);
                    // the baseline across the cell; ticks at the origin and at the advance
                    plutovg_path_move_to(marks, cx + 2, base);
                    plutovg_path_line_to(marks, cx + CELL_W - 2, base);
                    for (float tx : {ox, ox + adv * k})
                        if (tx >= cx + 1 && tx <= cx + CELL_W - 1) {
                            plutovg_path_move_to(marks, tx, base - 2.5f);
                            plutovg_path_line_to(marks, tx, base + 2.5f);
                        }
                }
                label(labels, gid, cx + CELL_W / 2, cy + 46.5f, 5, cy + 52.5f, 3.6f, CELL_W - 4);
            }
            add(glyphs, TEXT);
            add(labels, LABEL);
        }
        add(marks, MARK, true);
    }

    void build(const Page &p) {
        if (p.svg) return pictures(p);
        if (p.head) head();
        else heading(p);
        if (p.count) grid(p, p.head ? PAGE_H - MARGIN - face.headRows() * CELL_H : GRID_TOP);
    }
};

// 0xRRGGBB of what an item is, over a light or a dark background
uint32_t colour(Role r, bool dark) {
    static const uint32_t light[] = {0x000000, 0x6a6a6a, 0xd4d4d4, 0x8fb8f0, 0xd42020};
    static const uint32_t night[] = {0xf2f2f2, 0x9c9c9c, 0x454a50, 0x3a6aa8, 0xff5a5a};
    return (dark ? night : light)[r];
}

// The glyphs a face's SVG table has a document of their own for (one of several glyphs holds them as
// elements named by the glyph: those are not shown).
void readSvgTable(Face &face) {
    const FT_ULong tag = FT_MAKE_TAG('S', 'V', 'G', ' ');
    FT_ULong len = 0;
    if (FT_Load_Sfnt_Table(face.ft, tag, 0, nullptr, &len) || len < 12 || len > (256u << 20)) return;
    face.svgTable.resize(len);
    if (FT_Load_Sfnt_Table(face.ft, tag, 0, (FT_Byte *)&face.svgTable[0], &len)) len = 0;
    const uint8_t *t = (const uint8_t *)face.svgTable.data();
    uint64_t list = len >= 12 ? be32(t + 2) : len;
    uint32_t count = list + 2 <= len ? (uint32_t)(t[list] << 8 | t[list + 1]) : 0;
    for (uint32_t k = 0; k < count && list + 2 + 12 * (uint64_t)(k + 1) <= len; k++) {
        const uint8_t *e = t + list + 2 + 12 * (size_t)k;
        uint32_t start = e[0] << 8 | e[1], end = e[2] << 8 | e[3], size = be32(e + 8);
        uint64_t off = list + be32(e + 4);
        if (start != end || !size || off > len || size > len - off) continue;
        face.svg.push_back({start, (uint32_t)off, size});
    }
    if (face.svg.empty()) std::string().swap(face.svgTable);
}

// An SVG glyph's document as a picture of the glyph's box. In a font the root's viewBox is no frame:
// it is mapped onto the em square hanging from the glyph's origin on the baseline, and what is drawn
// above that shows too (EmojiOne: viewBox="0 54.4 64 64" around a picture from 0 to 64, the baseline
// at 54.4). So the viewBox is rewritten to the glyph's box: from the ascender to the descender, its
// advance wide, in the viewBox's own units. A document without one (drawn in font units, as the
// specification has it now) is left as it is: nitrosvg shows the bounds of what it draws.
void glyphBox(std::string &svg, float upm, float asc, float desc, float adv) {
    size_t tag = svg.find("<svg"), end = tag == std::string::npos ? tag : svg.find('>', tag);
    size_t at = end == std::string::npos ? end : svg.find("viewBox", tag);
    if (at == std::string::npos || at > end) return;
    size_t q = svg.find_first_of("\"'", at);
    size_t close = q == std::string::npos || q > end ? std::string::npos : svg.find(svg[q], q + 1);
    if (close == std::string::npos || close > end || close - q > 200) return;
    std::string nums = svg.substr(q + 1, close - q - 1);
    for (char &c : nums)
        if (c == ',') c = ' ';
    double v[4];
    const char *p = nums.c_str();
    for (int k = 0; k < 4; k++) {
        char *next = nullptr;
        v[k] = nn_strtod(p, &next);
        if (next == p) return;
        p = next;
    }
    if (!(v[2] > 0 && v[3] > 0) || !(upm > 0) || !(asc + desc > 0) || !(adv > 0)) return;
    double k = v[2] / upm;   // the viewBox's units to a font unit
    char b[160];
    snprintf(b, sizeof b, "%g %g %g %g", v[0], v[1] - asc * k, adv * k, (asc + desc) * k);
    nn_cpoint(b);
    svg.replace(q + 1, close - q - 1, b);
}

// a glyph's SVG document loaded (a gzip'd one inflated first), to be drawn over what is there; NULL: lunasvg does not read it
nitrosvg *loadSvg(const Face &face, const Face::SvgGlyph &g) {
    const uint8_t *d = (const uint8_t *)face.svgTable.data() + g.off;
    size_t len = g.len;
    std::string plain;
    if (len >= 2 && d[0] == 0x1f && d[1] == 0x8b) {
        z_stream z;
        memset(&z, 0, sizeof z);
        if (inflateInit2(&z, 15 + 16) != Z_OK) return nullptr;
        z.next_in = (Bytef *)d, z.avail_in = (uInt)len;
        int rc = Z_OK;
        while (rc == Z_OK && plain.size() < (8u << 20)) {   // (a glyph's picture: a few KB)
            size_t at = plain.size();
            plain.resize(at + (64u << 10));
            z.next_out = (Bytef *)&plain[at], z.avail_out = 64u << 10;
            rc = inflate(&z, Z_NO_FLUSH);
            plain.resize(at + (64u << 10) - z.avail_out);
        }
        inflateEnd(&z);
        if (rc != Z_STREAM_END) return nullptr;
    } else
        plain.assign((const char *)d, len);
    FT_Face ft = face.ft;
    FT_Fixed adv = 0;
    if (FT_Get_Advance(ft, g.glyph, FT_LOAD_NO_SCALE, &adv) || adv <= 0) adv = ft->units_per_EM;
    glyphBox(plain, ft->units_per_EM, ft->ascender, -ft->descender, (float)adv);
    nitrosvg *svg = nitrosvg_load(plain.data(), plain.size());
    if (svg) nitrosvg_set_background(svg, 3);
    return svg;
}

}   // namespace

nitrotype *nitrotype_load(const void *data, size_t len) {
    if (!data || len < 12 || len > (256u << 20)) return nullptr;
    const uint8_t *d = (const uint8_t *)data;
    auto *F = new nitrotype;
    if (!memcmp(d, "wOFF", 4)) {
        if (!unwoff(d, len, F->data)) {
            delete F;
            return nullptr;
        }
        F->wrapper = "WOFF: ";
    } else if (uneot(d, len, F->data))
        F->wrapper = "EOT: ";
    else if (unsvg(d, len, F->data))
        F->svgFont = true;
    else {
        F->data.assign((const char *)data, len);
        if (!memcmp(d, "wOF2", 4)) F->wrapper = "WOFF2: ";   // (FreeType unwraps it: Brotli, the glyph table rebuilt)
    }
    FT_Face probe = nullptr;
    if (FT_Init_FreeType(&F->lib)) F->lib = nullptr;
    if (!F->lib || FT_New_Memory_Face(F->lib, (const FT_Byte *)F->data.data(), (FT_Long)F->data.size(), -1, &probe)) {
        delete F;
        return nullptr;
    }
    long count = probe->num_faces;
    FT_Done_Face(probe);
    for (long k = 0; k < count && F->faces.size() < MAX_FACES; k++) {
        FT_Face ft = nullptr;
        if (FT_New_Memory_Face(F->lib, (const FT_Byte *)F->data.data(), (FT_Long)F->data.size(), k, &ft)) continue;
        if (!FT_IS_SCALABLE(ft) || ft->num_glyphs <= 0) {   // (a bitmap font: nothing to zoom into)
            FT_Done_Face(ft);
            continue;
        }
        // a symbol font's own character map where there is no Unicode one (FreeType picks that itself)
        if (!ft->charmap && FT_Select_Charmap(ft, FT_ENCODING_MS_SYMBOL) && ft->num_charmaps > 0) FT_Set_Charmap(ft, ft->charmaps[0]);
        Face face;
        face.ft = ft, face.index = (int)k;
        face.uni.assign((size_t)ft->num_glyphs, 0);
        if (ft->charmap) {
            FT_UInt g = 0;
            FT_ULong c = FT_Get_First_Char(ft, &g);
            for (size_t guard = 0; g && guard < (1u << 21); guard++) {
                face.chars++;
                if (g < face.uni.size() && !face.uni[g]) face.uni[g] = (uint32_t)c;
                c = FT_Get_Next_Char(ft, c, &g);
            }
        }
        if (FT_HAS_GLYPH_NAMES(ft))   // (glyphs with a name and no code: "A", "aacute"; not a variant's, "a.sc")
            for (FT_UInt g = 1; g < (FT_UInt)face.uni.size(); g++) {
                char nm[64] = "";
                if (face.uni[g] || FT_Get_Glyph_Name(ft, g, nm, sizeof nm) || !nm[0] || strchr(nm, '.')) continue;
                uint32_t cp = nitropdf_glyph_unicode(nm);
                if (cp && !FT_Get_Char_Index(ft, cp)) face.named.emplace(cp, g);
            }
        face.samples = covers(face, SAMPLES[0].text, false) ? 1 : 0;
        for (int k = 1; k < NSAMPLES && face.samples < NSAMPLES; k++)
            if (covers(face, SAMPLES[k].text, true)) face.samples = NSAMPLES;
        int headRows = face.headRows();
        int fi = (int)F->faces.size();
        F->faces.push_back(std::move(face));
        uint32_t n = (uint32_t)ft->num_glyphs, at = 0;
        for (bool head = true; (head || at < n) && F->pages.size() < MAX_PAGES; head = false) {
            uint32_t room = (uint32_t)((head ? headRows : ROWS_MORE) * COLS), take = std::min(room, n - at);
            F->pages.push_back({fi, at, take, head, false});
            at += take;
        }
        readSvgTable(F->faces[fi]);
        uint32_t pictures = (uint32_t)F->faces[fi].svg.size(), room = (uint32_t)(SVG_COLS * SVG_ROWS);
        for (uint32_t i = 0; i < pictures && F->pages.size() < MAX_PAGES; i += room)
            F->pages.push_back({fi, i, std::min(room, pictures - i), false, true});
    }
    if (F->pages.empty()) {
        delete F;
        return nullptr;
    }
    for (size_t k = 0; k < F->pages.size(); k++) F->dl.push_back(std::make_unique<PageDL>());
    return F;
}

void nitrotype_free(nitrotype *font) { delete font; }

int nitrotype_pages(const nitrotype *font) { return font ? (int)font->pages.size() : 0; }

void nitrotype_set_background(nitrotype *font, int background) {
    if (font) font->background = background;
}

void nitrotype_size(const nitrotype *font, int page, double *w, double *h) {
    *w = *h = 0;
    if (!font || page < 0 || page >= (int)font->pages.size()) return;
    *w = PAGE_W, *h = PAGE_H;
}

int nitrotype_render(nitrotype *font, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx) {
    if (!font || page < 0 || page >= (int)font->pages.size() || !nd::renderArgs(w, h, pitch, scale)) return -1;
    PageDL &pg = *font->dl[page];
    const Page &p = font->pages[page];
    std::shared_ptr<Pictures> pictures;   // (a page of colour glyphs: its SVG documents, held while it is drawn)
    if (!pg.built.load(std::memory_order_acquire) || p.svg) {
        std::lock_guard<std::mutex> g(font->lock);
        if (!pg.built.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> fonts(nf_lock());
                Layout(*font, pg, font->faces[p.face]).build(p);
            }
            pg.built.store(true, std::memory_order_release);
        }
        if (p.svg) {
            // its pictures loaded now, or still there; those of the page used longest ago are let go
            std::vector<int> &recent = font->svgPages;
            recent.erase(std::remove(recent.begin(), recent.end(), page), recent.end());
            recent.push_back(page);
            if (!pg.loaded) {
                const Face &face = font->faces[p.face];
                pg.loaded = std::make_shared<Pictures>();
                for (const Picture &pc : pg.pictures) pg.loaded->docs.push_back(loadSvg(face, face.svg[pc.index]));
            }
            pictures = pg.loaded;
            if (recent.size() > SVG_KEEP) {
                font->dl[recent.front()]->loaded.reset();
                recent.erase(recent.begin());
            }
        }
    }
    int bg = font->background;
    bool dark = bg == 1 || bg == 2;
    nd::Target t(bgra, w, h, pitch, nd::background(bg));
    if (!t.canvas) return -1;
    nd::Cancel cancel{cancelled, ctx};
    plutovg_matrix_t view;
    plutovg_matrix_init(&view, (float)scale, 0, 0, (float)scale, (float)(-x0 * scale), (float)(-y0 * scale));
    plutovg_canvas_set_matrix(t.canvas, &view);
    plutovg_canvas_set_fill_rule(t.canvas, PLUTOVG_FILL_RULE_NON_ZERO);
    plutovg_canvas_set_line_width(t.canvas, (float)(1 / scale));   // (the frame and the marks: a device pixel)
    double vb[4] = {x0, y0, x0 + w / scale, y0 + h / scale};
    size_t k = 0;
    for (const Item &it : pg.items) {
        if ((++k & 15) == 0 && cancel()) return 1;
        if (!nd::touches(vb, it.box.x0, it.box.y0, it.box.x1, it.box.y1, 2 / scale)) continue;
        nd::setRgb(t.canvas, colour(it.role, dark), 1);
        if (it.stroke) plutovg_canvas_stroke_path(t.canvas, it.path);
        else plutovg_canvas_fill_path(t.canvas, it.path);
    }
    if (!pictures) return 0;
    // The colour glyphs: each SVG fitted into its box, the part of it in this buffer rendered by
    // nitrosvg at the buffer's resolution into a tile of its own, the tile drawn over the page.
    plutovg_matrix_t ident;
    plutovg_matrix_init_identity(&ident);
    plutovg_canvas_set_matrix(t.canvas, &ident);
    for (size_t i = 0; i < pg.pictures.size() && i < pictures->docs.size(); i++) {
        const Picture &pc = pg.pictures[i];
        nitrosvg *svg = pictures->docs[i];
        if (!svg || !nd::touches(vb, pc.x, pc.y, pc.x + pc.w, pc.y + pc.h)) continue;
        if (cancel()) return 1;
        double sw = 0, sh = 0;
        nitrosvg_size(svg, &sw, &sh);
        if (!(sw > 0 && sh > 0)) continue;
        double fit = std::min(pc.w / sw, pc.h / sh), px = fit * scale;   // (px: device pixels per unit of the picture)
        double left = (pc.x + (pc.w - sw * fit) / 2 - x0) * scale, top = (pc.y + (pc.h - sh * fit) / 2 - y0) * scale;
        int ix0 = std::max(0, (int)std::floor(left)), iy0 = std::max(0, (int)std::floor(top));
        int ix1 = (int)std::min<double>(w, std::ceil(left + sw * px)), iy1 = (int)std::min<double>(h, std::ceil(top + sh * px));
        if (ix1 <= ix0 || iy1 <= iy0) continue;
        plutovg_surface_t *tile = plutovg_surface_create(ix1 - ix0, iy1 - iy0);
        if (!tile) continue;
        if (!nitrosvg_render(svg, plutovg_surface_get_data(tile), ix1 - ix0, iy1 - iy0, (size_t)plutovg_surface_get_stride(tile), px, (ix0 - left) / px,
                             (iy0 - top) / px)) {
            plutovg_matrix_t at;
            plutovg_matrix_init_translate(&at, (float)ix0, (float)iy0);
            plutovg_canvas_set_texture(t.canvas, tile, PLUTOVG_TEXTURE_TYPE_PLAIN, 1, &at);
            plutovg_canvas_fill_rect(t.canvas, (float)ix0, (float)iy0, (float)(ix1 - ix0), (float)(iy1 - iy0));
        }
        plutovg_surface_destroy(tile);
    }
    return 0;
}
