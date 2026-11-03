// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitro_font: the fonts of the vector renderers. See nitro_font.h.
//
// The index reads only the tables it needs from each font file (the names, the OS/2 and head
// tables; the cmap when a glyph is first asked for), not the glyphs: a system's 800 faces take
// about 10 ms. A face itself is loaded with plutovg when first used, and kept.
#include "nitro_font.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

std::mutex g_lock;

struct Face {
    std::string family, file;
    uint32_t offset = 0;   // of the font in its file (a collection: one of several)
    int ttc = 0;           // its index in the collection
    int weight = 400;
    bool italic = false, narrow = false;
    plutovg_font_face_t *face = nullptr;
    bool failed = false;          // (loading it failed: not tried again)
    std::vector<uint8_t> cmap;    // its cmap table, read when first asked for a glyph
    uint32_t cmapTable = 0;       // the subtable used, offset in cmap (0: none)
    bool cmapRead = false;
};

struct Index {
    std::vector<Face> faces;
    std::unordered_map<std::string, std::vector<int>> families;   // lower-case name -> faces
    std::unordered_map<plutovg_font_face_t *, int> loaded;
};

uint32_t u16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

std::string lower(const char *s) {
    std::string o(s);
    for (char &c : o)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return o;
}

bool read_at(FILE *f, uint32_t off, size_t n, std::vector<uint8_t> &out) {
    out.clear();
    if (fseek(f, (long)off, SEEK_SET)) return false;
    out.resize(n);
    size_t got = fread(out.data(), 1, n, f);
    out.resize(got);
    return got == n;
}

// --- the font files -----------------------------------------------------------

struct Table {
    uint32_t off = 0, len = 0;
};

// the offsets of the tables named, from the font's table directory
bool tables(FILE *f, uint32_t fontOff, const char *const *tags, Table *out, int n) {
    std::vector<uint8_t> dir;
    if (!read_at(f, fontOff, 12, dir)) return false;
    uint32_t tag = u32(dir.data());
    if (tag != 0x00010000 && tag != 0x74727565 /* true */ && tag != 0x4f54544f /* OTTO */ && tag != 0x31000000 /* 1 */) return false;
    uint32_t count = u16(dir.data() + 4);
    if (!count || count > 512 || !read_at(f, fontOff + 12, count * 16, dir)) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = dir.data() + i * 16;
        for (int k = 0; k < n; k++)
            if (!memcmp(r, tags[k], 4)) {
                out[k].off = u32(r + 8);
                out[k].len = u32(r + 12);
            }
    }
    return true;
}

// The high half of Mac Roman (the name table's platform 1 strings), as Unicode.
const uint16_t MAC_ROMAN[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7};

// The font's family name (name id 1): a Unicode name (platform 0, or Windows' Unicode) if it has
// one, else the Mac Roman one; as plutovg's own cache reads it.
std::string family_name(const std::vector<uint8_t> &name) {
    if (name.size() < 6) return "";
    uint32_t count = u16(name.data() + 2), strings = u16(name.data() + 4);
    const uint8_t *unicode = nullptr, *roman = nullptr;
    size_t ulen = 0, rlen = 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t loc = 6 + 12 * i;
        if (loc + 12 > name.size()) break;
        const uint8_t *r = name.data() + loc;
        if (u16(r + 6) != 1) continue;
        uint32_t platform = u16(r), encoding = u16(r + 2), len = u16(r + 8), off = u16(r + 10);
        if ((size_t)strings + off + len > name.size()) continue;
        const uint8_t *s = name.data() + strings + off;
        if (platform == 1 && encoding == 0) {
            roman = s;
            rlen = len;
        } else if (platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10))) {
            unicode = s;
            ulen = len;
            break;
        }
    }
    std::string o;
    if (unicode) {
        for (size_t i = 0; i + 1 < ulen; i += 2) {
            uint32_t c = u16(unicode + i);
            if (c >= 0xd800 && c < 0xdc00 && i + 3 < ulen) {
                c = ((c - 0xd800) << 10) + (u16(unicode + i + 2) - 0xdc00) + 0x10000;
                i += 2;
            }
            nf_utf8_put(o, c);
        }
    } else if (roman) {
        for (size_t i = 0; i < rlen; i++) nf_utf8_put(o, roman[i] < 0x80 ? roman[i] : MAC_ROMAN[roman[i] - 0x80]);
    }
    return o;
}

void index_file(Index &ix, const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return;
    std::vector<uint8_t> head;
    std::vector<uint32_t> offsets;
    if (read_at(f, 0, 12, head)) {
        if (u32(head.data()) == 0x74746366) {   // a collection (ttcf): its fonts
            uint32_t n = std::min(u32(head.data() + 8), 64u);
            std::vector<uint8_t> offs;
            if (read_at(f, 12, n * 4, offs))
                for (uint32_t i = 0; i < n; i++) offsets.push_back(u32(offs.data() + i * 4));
        } else
            offsets.push_back(0);
    }
    for (size_t i = 0; i < offsets.size(); i++) {
        static const char *const tags[] = {"name", "head", "OS/2"};
        Table t[3];
        if (!tables(f, offsets[i], tags, t, 3) || !t[0].off || t[0].len > 65536) continue;
        std::vector<uint8_t> name, os2, hd;
        if (!read_at(f, t[0].off, t[0].len, name)) continue;
        Face face;
        face.family = family_name(name);
        if (face.family.empty()) continue;
        face.file = path;
        face.offset = offsets[i];
        face.ttc = (int)i;
        bool bold = false;
        if (t[1].off && read_at(f, t[1].off, 46, hd)) {   // head: macStyle
            uint32_t style = u16(hd.data() + 44);
            bold = style & 1;
            face.italic = style & 2;
        }
        if (t[2].off && read_at(f, t[2].off, 8, os2)) {   // OS/2: usWeightClass, usWidthClass
            uint32_t weight = u16(os2.data() + 4), width = u16(os2.data() + 6);
            face.weight = weight ? (int)weight : bold ? 700 : 400;
            face.narrow = width != 5 && width != 0;
        } else
            face.weight = bold ? 700 : 400;
        ix.families[lower(face.family.c_str())].push_back((int)ix.faces.size());
        ix.faces.push_back(std::move(face));
    }
    fclose(f);
}

bool font_file(const std::string &name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || name.size() - dot != 4) return false;
    std::string ext = lower(name.c_str() + dot);
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

void index_dir(Index &ix, const std::string &dir, int depth) {
    if (depth > 8) return;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        std::string path = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) index_dir(ix, path, depth + 1);
        else if (font_file(fd.cFileName)) index_file(ix, path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string path = dir + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) index_dir(ix, path, depth + 1);
        else if (S_ISREG(st.st_mode) && font_file(e->d_name)) index_file(ix, path);
    }
    closedir(d);
#endif
}

// the system's font directories and the user's (where Font Book, Windows 10+ and fontconfig
// install a user's own fonts)
void index_system(Index &ix) {
#ifdef _WIN32
    const char *windir = getenv("WINDIR");
    index_dir(ix, std::string(windir ? windir : "C:\\Windows") + "\\Fonts", 0);
    if (const char *local = getenv("LOCALAPPDATA")) index_dir(ix, std::string(local) + "\\Microsoft\\Windows\\Fonts", 0);
#elif defined(__APPLE__)
    index_dir(ix, "/Library/Fonts", 0);
    index_dir(ix, "/System/Library/Fonts", 0);
    if (const char *home = getenv("HOME")) index_dir(ix, std::string(home) + "/Library/Fonts", 0);
    // Microsoft Office's own fonts (Calibri, Cambria, Candara, Tahoma ...: what Office and Visio
    // documents are set in) come with each Office app, the same set in each: one of them
    static const char *const office[] = {"/Applications/Microsoft Word.app/Contents/Resources/DFonts",
                                         "/Applications/Microsoft Excel.app/Contents/Resources/DFonts",
                                         "/Applications/Microsoft PowerPoint.app/Contents/Resources/DFonts",
                                         "/Applications/Microsoft Outlook.app/Contents/Resources/DFonts",
                                         "/Applications/Microsoft OneNote.app/Contents/Resources/DFonts"};
    for (const char *dir : office)
        if (access(dir, R_OK) == 0) {
            index_dir(ix, dir, 0);
            break;
        }
#else
    index_dir(ix, "/usr/share/fonts", 0);
    index_dir(ix, "/usr/local/share/fonts", 0);
    const char *home = getenv("HOME"), *data = getenv("XDG_DATA_HOME");
    if (home) index_dir(ix, std::string(home) + "/.fonts", 0);
    if (data && *data) index_dir(ix, std::string(data) + "/fonts", 0);
    else if (home) index_dir(ix, std::string(home) + "/.local/share/fonts", 0);
#endif
}

Index &index() {   // (built on first use; locked by the caller)
    static Index *ix;
    if (!ix) {
        ix = new Index;
        index_system(*ix);
    }
    return *ix;
}

plutovg_font_face_t *load(Index &ix, int k) {
    Face &f = ix.faces[k];
    if (!f.face && !f.failed) {
        f.face = plutovg_font_face_load_from_file(f.file.c_str(), f.ttc);
        if (f.face) ix.loaded[f.face] = k;
        else f.failed = true;
    }
    return f.face;
}

// --- glyphs: the cmap table ----------------------------------------------------

// the subtable to look characters up in: Windows' full Unicode one, else its BMP one, else a
// Unicode platform one (as stb_truetype chooses)
uint32_t choose_cmap(const std::vector<uint8_t> &cmap) {
    if (cmap.size() < 4) return 0;
    uint32_t n = u16(cmap.data() + 2), best = 0;
    int bestRank = 0;
    for (uint32_t i = 0; i < n && 4 + 8 * i + 8 <= cmap.size(); i++) {
        const uint8_t *r = cmap.data() + 4 + 8 * i;
        uint32_t platform = u16(r), encoding = u16(r + 2), off = u32(r + 4);
        int rank = platform == 3 && encoding == 10 ? 3 : platform == 3 && encoding == 1 ? 2 : platform == 0 ? 1 : 0;
        if (rank > bestRank && off + 4 <= cmap.size()) {
            best = off;
            bestRank = rank;
        }
    }
    return best;
}

bool has_glyph(const std::vector<uint8_t> &cmap, uint32_t table, uint32_t cp) {
    const uint8_t *t = cmap.data() + table;
    size_t avail = cmap.size() - table;
    auto in = [&](size_t off, size_t n) { return off + n <= avail; };
    switch (u16(t)) {
    case 0:   // a byte a character
        return cp < 256 && in(6 + cp, 1) && t[6 + cp] != 0;
    case 4: {   // segments of the BMP
        if (cp > 0xffff || !in(0, 14)) return false;
        uint32_t segs = u16(t + 6) / 2;
        if (!segs || !in(14, segs * 8 + 2)) return false;
        const uint8_t *ends = t + 14, *starts = ends + segs * 2 + 2, *deltas = starts + segs * 2, *ranges = deltas + segs * 2;
        uint32_t lo = 0, hi = segs;   // the first segment whose end is >= cp
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (u16(ends + mid * 2) < cp) lo = mid + 1;
            else hi = mid;
        }
        if (lo == segs || u16(starts + lo * 2) > cp) return false;
        uint32_t range = u16(ranges + lo * 2);
        if (!range) return ((cp + u16(deltas + lo * 2)) & 0xffff) != 0;
        size_t at = (ranges + lo * 2 - t) + range + (cp - u16(starts + lo * 2)) * 2;
        if (!in(at, 2)) return false;
        uint32_t g = u16(t + at);
        return g && ((g + u16(deltas + lo * 2)) & 0xffff) != 0;
    }
    case 6: {   // one range
        if (!in(0, 10)) return false;
        uint32_t first = u16(t + 6), count = u16(t + 8);
        return cp >= first && cp < first + count && in(10 + (cp - first) * 2, 2) && u16(t + 10 + (cp - first) * 2) != 0;
    }
    case 12: {   // groups of the whole range
        if (!in(0, 16)) return false;
        uint32_t groups = u32(t + 12);
        if (!in(16, (size_t)groups * 12)) return false;
        uint32_t lo = 0, hi = groups;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            const uint8_t *g = t + 16 + mid * 12;
            if (u32(g + 4) < cp) lo = mid + 1;
            else hi = mid;
        }
        if (lo == groups) return false;
        const uint8_t *g = t + 16 + lo * 12;
        return u32(g) <= cp && u32(g + 8) + (cp - u32(g)) != 0;
    }
    default: return false;
    }
}

void read_cmap(Face &f) {
    f.cmapRead = true;
    FILE *file = fopen(f.file.c_str(), "rb");
    if (!file) return;
    static const char *const tags[] = {"cmap"};
    Table t;
    if (tables(file, f.offset, tags, &t, 1) && t.off && t.len >= 4 && t.len <= 8u << 20 && read_at(file, t.off, t.len, f.cmap))
        f.cmapTable = choose_cmap(f.cmap);
    fclose(file);
}

// --- the lookups -------------------------------------------------------------

const std::vector<int> *family_faces(Index &ix, const char *family) {
    auto it = ix.families.find(lower(family));
    return it == ix.families.end() ? nullptr : &it->second;
}

} // namespace

std::mutex &nf_lock() { return g_lock; }

plutovg_font_face_t *nf_face(const char *family, int weight, bool italic) {
    Index &ix = index();
    const std::vector<int> *faces = family_faces(ix, family);
    if (!faces) return nullptr;
    weight = std::clamp(weight, 1, 1000);   // (a damaged file's)
    int best = -1;
    long bestScore = 0;
    for (int k : *faces) {
        const Face &f = ix.faces[k];
        int diff = std::abs(f.weight - weight);
        // the slant first, then a normal width, then the weight; of two equally close weights
        // the one on the side of the request (a light weight: the lighter)
        bool side = weight < 400 ? f.weight < weight : f.weight > weight;
        long score = (f.italic != italic) * 100000L + f.narrow * 10000L + diff * 2 + !side;
        if (best < 0 || score < bestScore) {
            best = k;
            bestScore = score;
        }
    }
    return best < 0 ? nullptr : load(ix, best);
}

bool nf_has_style(const char *family, bool italic) {
    Index &ix = index();
    const std::vector<int> *faces = family_faces(ix, family);
    if (!faces) return false;
    for (int k : *faces)
        if (ix.faces[k].italic == italic) return true;
    return false;
}

bool nf_has_glyph(plutovg_font_face_t *face, uint32_t codepoint) {
    Index &ix = index();
    auto it = ix.loaded.find(face);
    if (it == ix.loaded.end()) return true;   // (not a face of ours: taken as complete)
    Face &f = ix.faces[it->second];
    if (!f.cmapRead) read_cmap(f);
    return f.cmapTable && has_glyph(f.cmap, f.cmapTable, codepoint);
}

plutovg_font_face_t *nf_css_face(const char *family, bool bold, bool italic) {
    int weight = bold ? 700 : 400;
    if (family && *family)
        if (plutovg_font_face_t *f = nf_face(family, weight, italic)) return f;
    // the generic families (and the default font, sans-serif): the fonts lunasvg takes for them
    static const struct { const char *generic, *fallback; } generics[] = {
#if defined(__linux__)
        {"sans-serif", "DejaVu Sans"}, {"serif", "DejaVu Serif"}, {"monospace", "DejaVu Sans Mono"},
#else
        {"sans-serif", "Arial"}, {"serif", "Times New Roman"}, {"monospace", "Courier New"},
#endif
        {"cursive", "Comic Sans MS"}, {"fantasy", "Impact"}};
    for (const auto &g : generics)
        if (!family || !*family || !strcmp(g.generic, family)) return nf_face(g.fallback, weight, italic);
    return nullptr;
}

uint32_t nf_utf8_next(const std::string &s, size_t &i) {
    unsigned char c = (unsigned char)s[i++];
    int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    uint32_t cp = n ? c & (0x3f >> n) : c;
    for (; n && i < s.size() && ((unsigned char)s[i] & 0xc0) == 0x80; n--) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3f);
    return cp;
}

void nf_utf8_put(std::string &o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) {
        o += (char)(0xc0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        o += (char)(0xe0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3f));
        o += (char)(0x80 | (cp & 0x3f));
    } else {
        o += (char)(0xf0 | (cp >> 18));
        o += (char)(0x80 | ((cp >> 12) & 0x3f));
        o += (char)(0x80 | ((cp >> 6) & 0x3f));
        o += (char)(0x80 | (cp & 0x3f));
    }
}
