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

#include FT_FONT_FORMATS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include <zlib.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
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

// The sample lines: the usual English one, then pangrams holding every accented letter of their
// language (hu: all 9; cs: all 15; sk: all but the rare ó; pl: all 9; ro: all 5; hr / sl: all 5; de:
// ä ö ü ß; ru: all 33 letters; el: all 24), the Central European ones first: Latin-2 is what most
// fonts lack. In lower case first, then in capitals where that is not too long.
const struct {
    const char *tag, *text;
} SAMPLES[] = {
    {"en", "the quick brown fox jumps over the lazy dog – THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG"},
    {"hu", "árvíztűrő tükörfúrógép – ÁRVÍZTŰRŐ TÜKÖRFÚRÓGÉP – 0123456789 € $ £ @ & % ! ? ( ) [ ] { }"},   // (the digits and signs here: the line above is long)
    {"cs", "příliš žluťoučký kůň úpěl ďábelské ódy – PŘÍLIŠ ŽLUŤOUČKÝ KŮŇ ÚPĚL ĎÁBELSKÉ ÓDY"},
    {"sk", "kŕdeľ šťastných ďatľov učí pri ústí Váhu mĺkveho koňa obhrýzať kôru a žrať čerstvé mäso – Ä Ĺ Ľ Ô Ŕ"},
    {"pl ro", "zażółć gęślą jaźń – ZAŻÓŁĆ GĘŚLĄ JAŹŃ – muzicologă în bej vând whisky și tequila, preț fix – Ă Â Î Ș Ț"},
    {"hr sl", "gojazni đačić s biciklom drži hmelj i finu vatu u džepu nošnje – Č Ć Đ Š Ž"},
    {"de fr es", "falsches Üben von Xylophonmusik quält jeden größeren Zwerg – voix ambiguë d’un cœur – ¿dónde está el pingüino? – ñ ç Ä Ö"},
    {"ru uk", "съешь же ещё этих мягких французских булок, да выпей чаю – Ё Й Щ Ъ Э Ю Я – ґ є і ї Ґ Є І Ї"},   // (the four letters Ukrainian has and Russian has not)
    {"el", "ξεσκεπάζω την ψυχοφθόρα βδελυγμία – Α Β Γ Δ Θ Λ Ξ Π Σ Φ Ψ Ω"},
};
const int NSAMPLES = sizeof SAMPLES / sizeof SAMPLES[0];
// the one line of a font with the English alphabet alone (no accented, Cyrillic or Greek letter): the digits and signs with it
const char SAMPLE_ASCII[] = "the quick brown fox jumps over the lazy dog – THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG – 0123456789 € $ £ @ & % ! ? ( ) [ ] { }";
// the rows of glyphs on a face's first page, under its sample lines (0: its glyphs under its name), and on each page after it
inline int rowsUnder(int lines) { return (int)((PAGE_H - MARGIN - (lines ? SAMPLE_TOP + lines * SAMPLE_PITCH + 6 : SAMPLE_TOP)) / CELL_H); }
const int ROWS_MORE = (int)((PAGE_H - MARGIN - GRID_TOP) / CELL_H);

enum Role : uint8_t { TEXT, LABEL, FRAME, MARK, MISSING };

struct Item {
    plutovg_path_t *path = nullptr;
    Role role = TEXT;
    bool stroke = false;
    nd::Box box;
};

struct PageDL {
    std::atomic<bool> built{false};
    std::vector<Item> items;
    ~PageDL() {
        for (Item &it : items) plutovg_path_destroy(it.path);
    }
};

struct Face {
    FT_Face ft = nullptr;
    int index = 0;                // (in its collection)
    std::vector<uint32_t> uni;    // a glyph's character (the first one mapped to it), 0: none
    size_t chars = 0;             // the characters the font maps
    // The sample lines it shows: all of them (NSAMPLES) if it has letters beyond the English alphabet
    // (what it lacks is red); the English one alone (1) with that alphabet only; none (0) without it
    // either: an icon or symbol font, or another script's.
    int samples = NSAMPLES;
    int headRows() const { return rowsUnder(samples); }   // the rows of glyphs on its first page
};

struct Page {
    int face;
    uint32_t first, count;   // its glyphs
    bool head;               // the face's first page: its name and the samples
};

// a letter: what a sample line's coverage is counted by (not the digits, signs and dashes every font has)
inline bool isLetter(uint32_t cp) {
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || (cp >= 0xC0 && !(cp >= 0x2000 && cp < 0x2C00));   // (not U+2013, the euro sign)
}
// Whether the font has at least half of a text's letters, drawn (an icon font may map a to z to empty
// glyphs: its icons are ligatures of their names); special: of those beyond the English alphabet (the
// accented, Cyrillic and Greek ones: a text with none of them is not covered).
bool covers(FT_Face ft, const char *text, bool special) {
    std::string s(text);
    int total = 0, missing = 0;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = nf_utf8_next(s, i);
        if (!isLetter(cp) || (special && cp < 0x80)) continue;
        total++;
        FT_UInt gid = FT_Get_Char_Index(ft, cp);
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

}   // namespace

struct nitrotype {
    std::string data;   // the font file (a WOFF's sfnt): FreeType reads it in place
    const char *wrapper = "";   // "WOFF: ", "WOFF2: ", "EOT: ": what the font came in
    FT_Library lib = nullptr;
    std::vector<Face> faces;
    std::vector<Page> pages;
    std::vector<std::unique_ptr<PageDL>> dl;
    int background = 0;
    std::mutex lock;   // (FreeType's faces: one page laid out at a time)
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
            FT_UInt gid = FT_Get_Char_Index(face.ft, cp);
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
        s += fmt ? fmt : "font";
        if (FT_IS_SFNT(face.ft) && fmt && !strcmp(fmt, "CFF")) s = F.wrapper + std::string("OpenType (CFF)");
        if (FT_HAS_MULTIPLE_MASTERS(face.ft)) s += ", variable (its default instance)";
        char b[200];
        snprintf(b, sizeof b, "   ·   %ld glyphs   ·   %zu characters   ·   %d units per em", (long)face.ft->num_glyphs, face.chars, (int)upm);
        s += b;
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

        bool names = FT_HAS_GLYPH_NAMES(face.ft);
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
                char b[80];
                uint32_t u = gid < face.uni.size() ? face.uni[gid] : 0;
                if (u) snprintf(b, sizeof b, "%u   U+%04X", gid, u);
                else snprintf(b, sizeof b, "%u", gid);
                uiCentered(labels, cx + CELL_W / 2, cy + 46.5f, 5, b, CELL_W - 4);
                char nm[64] = "";
                if (names && !FT_Get_Glyph_Name(face.ft, gid, nm, sizeof nm) && nm[0]) uiCentered(labels, cx + CELL_W / 2, cy + 52.5f, 3.6f, nm, CELL_W - 4);
            }
            add(glyphs, TEXT);
            add(labels, LABEL);
        }
        add(marks, MARK, true);
    }

    void build(const Page &p) {
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
        face.samples = covers(ft, SAMPLES[0].text, false) ? 1 : 0;
        for (int k = 1; k < NSAMPLES && face.samples < NSAMPLES; k++)
            if (covers(ft, SAMPLES[k].text, true)) face.samples = NSAMPLES;
        int headRows = face.headRows();
        int fi = (int)F->faces.size();
        F->faces.push_back(std::move(face));
        uint32_t n = (uint32_t)ft->num_glyphs, at = 0;
        for (bool head = true; (head || at < n) && F->pages.size() < MAX_PAGES; head = false) {
            uint32_t room = (uint32_t)((head ? headRows : ROWS_MORE) * COLS), take = std::min(room, n - at);
            F->pages.push_back({fi, at, take, head});
            at += take;
        }
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
    if (!pg.built.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> g(font->lock);
        if (!pg.built.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> fonts(nf_lock());
            const Page &p = font->pages[page];
            Layout(*font, pg, font->faces[p.face]).build(p);
            pg.built.store(true, std::memory_order_release);
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
    return 0;
}
