// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitro_font: the fonts of the vector renderers. See nitro_font.h.
//
// The index reads the names and style tables from each font file. A face itself is loaded
// with plutovg when first used, and kept.
#include "nitro_font.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwrite_3.h>
#include <functional>
#else
#include <cerrno>
#include <dirent.h>
#include <iconv.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#if !defined(__APPLE__)
#include <dlfcn.h>
#endif
#endif

namespace {

std::mutex g_lock;

struct Face {
    std::string file;
    std::vector<std::string> postscript;
    int ttc = 0;           // its index in the collection
    bool namesRead = false;   // (all its name table's names indexed: read from the file, not a system list's)
    int weight = 400, width = 5;
    bool italic = false;
    plutovg_font_face_t *face = nullptr;
    bool failed = false;          // (loading it failed: not tried again)
};

using Names = std::unordered_map<std::string, std::vector<int>>;

struct Index {
    std::vector<Face> faces;
    Names families, faceNames, files;   // lower-case name -> faces
    std::unordered_map<plutovg_font_face_t *, int> loaded;
    std::unordered_map<std::string, std::vector<uint8_t>> fontData;   // shared by the faces of a collection
};

uint32_t u16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

std::string lower(const char *s) {
    std::string o(s);
    for (char &c : o)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return o;
}

const char *without_subset_tag(const char *name) {
    if (strlen(name) > 7 && name[6] == '+' && std::all_of(name, name + 6, [](char c) { return c >= 'A' && c <= 'Z'; }))
        return name + 7;
    return name;
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

bool ends_with_ci(const std::string &s, const char *t) {
    size_t n = strlen(t);
    return s.size() >= n && !strcasecmp(s.c_str() + s.size() - n, t);
}

// OS/2 width classes, with the more specific spellings before their suffixes.
const struct { const char *name; const char *compact; int width; } widths[] = {
    {" Ultra Condensed", "ultracondensed", 1}, {" Extra Condensed", "extracondensed", 2},
    {" Semi Condensed", "semicondensed", 4}, {" Condensed", "condensed", 3},
    {" Compressed", "compressed", 2}, {" Narrow", "narrow", 3},
    {" Ultra Expanded", "ultraexpanded", 9}, {" Extra Expanded", "extraexpanded", 8},
    {" Semi Expanded", "semiexpanded", 6}, {" Expanded", "expanded", 7},
    {" Extended", "extended", 7}, {" Wide", "wide", 7}};

// Name IDs 1 and 16 identify families; IDs 4 and 6 identify individual faces.
void bind_name(Index &ix, uint32_t id, std::string value, int face, Face &font) {
    if (value.empty() || value.find(char(0)) != std::string::npos) return;
    bool family = id == 1 || id == 16;
    Names &names = family ? ix.families : ix.faceNames;
    auto bind = [&](const std::string &key) {
        auto &faces = names[lower(key.c_str())];
        if (faces.empty() || faces.back() != face) faces.push_back(face);
    };
    bind(value);
    // Older fonts can give a width variant its own family, e.g. Arial Narrow.
    // Also index the base family when the name agrees with its OS/2 width.
    if (family && font.width != 5)
        for (const auto &w : widths)
            if (ends_with_ci(value, w.name)) {
                if ((font.width < 5) == (w.width < 5) && value.size() > strlen(w.name))
                    bind(value.substr(0, value.size() - strlen(w.name)));
                break;
            }
    if (id == 6 && std::find(font.postscript.begin(), font.postscript.end(), value) == font.postscript.end())
        font.postscript.push_back(std::move(value));
}

// the name table's names, each in bind_name
void index_names(Index &ix, const std::vector<uint8_t> &name, int face, Face &font) {
    if (name.size() < 6) return;
    uint32_t count = u16(name.data() + 2), strings = u16(name.data() + 4);
    for (uint32_t i = 0; i < count; i++) {
        size_t loc = 6 + 12 * i;
        if (loc + 12 > name.size()) break;
        const uint8_t *r = name.data() + loc;
        uint32_t id = u16(r + 6);
        if (id != 1 && id != 4 && id != 6 && id != 16) continue;
        uint32_t platform = u16(r), encoding = u16(r + 2), len = u16(r + 8), off = u16(r + 10);
        if ((size_t)strings + off + len > name.size()) continue;
        const uint8_t *s = name.data() + strings + off;
        std::string value;
        if (platform == 1 && encoding == 0) {
            for (size_t k = 0; k < len; k++) nf_utf8_put(value, s[k] < 0x80 ? s[k] : MAC_ROMAN[s[k] - 0x80]);
        } else if (platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10))) {
            for (size_t k = 0; k + 1 < len; k += 2) {
                uint32_t c = u16(s + k);
                if (c >= 0xd800 && c < 0xdc00 && k + 3 < len) {
                    uint32_t trail = u16(s + k + 2);
                    if (trail >= 0xdc00 && trail < 0xe000) {
                        c = ((c - 0xd800) << 10) + (trail - 0xdc00) + 0x10000;
                        k += 2;
                    }
                }
                if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
                nf_utf8_put(value, c);
            }
        }
        bind_name(ix, id, std::move(value), face, font);
    }
}

const char *basename(const std::string &path) {
    size_t slash = path.find_last_of("/\\");
    return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

void index_file(Index &ix, const std::string &path) {
    std::string filename = lower(basename(path));
    if (auto known = ix.files.find(filename); known != ix.files.end())   // (indexed already, by the system's font list)
        for (int k : known->second)
#ifdef _WIN32
            if (lower(ix.faces[k].file.c_str()) == lower(path.c_str())) return;   // (Windows' paths: any case)
#else
            if (ix.faces[k].file == path) return;
#endif
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
        face.file = path;
        face.ttc = (int)i;
        bool bold = false;
        if (t[1].off && read_at(f, t[1].off, 46, hd)) {   // head: macStyle
            uint32_t style = u16(hd.data() + 44);
            bold = style & 1;
            face.italic = style & 2;
        }
        if (t[2].off && read_at(f, t[2].off, 8, os2)) {   // OS/2: usWeightClass, usWidthClass
            uint32_t weight = u16(os2.data() + 4), width = u16(os2.data() + 6);
            face.weight = weight && weight <= 1000 ? (int)weight : bold ? 700 : 400;
            if (width >= 1 && width <= 9) face.width = (int)width;
        } else
            face.weight = bold ? 700 : 400;
        int k = (int)ix.faces.size();
        index_names(ix, name, k, face);
        face.namesRead = true;
        ix.files[filename].push_back(k);
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

#ifdef _WIN32
// Windows 10+: the installed fonts as DirectWrite's system font set lists them, from the Windows font cache
// service's data: their names, weight, width, style, file and index in a collection, without opening a font
// file. false where it is not there (an older Windows): the font directories are read instead.
template <class T> struct Com {   // (a COM pointer, released at its end)
    T *p = nullptr;
    ~Com() { if (p) p->Release(); }
    T *operator->() const { return p; }
    T **operator&() { return &p; }
};

std::string narrow(const WCHAR *w, UINT cp) {   // (UTF-16 to UTF-8 or the ANSI code page; "" if it does not fit)
    std::string out;
    BOOL lossy = FALSE;
    // (WC_NO_BEST_FIT_CHARS: a character the code page lacks is no look-alike but the default one, so it is noticed)
    DWORD flags = cp == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS;
    int n = WideCharToMultiByte(cp, flags, w, -1, nullptr, 0, nullptr, cp == CP_UTF8 ? nullptr : &lossy);
    if (n <= 1 || lossy) return out;
    out.resize((size_t)n);
    WideCharToMultiByte(cp, flags, w, -1, out.data(), n, nullptr, nullptr);
    out.resize((size_t)n - 1);
    return out;
}

bool index_dwrite(Index &ix) {
    Com<IDWriteFactory3> factory;
    Com<IDWriteFontSet> set;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), (IUnknown **)&factory)) ||
        FAILED(factory->GetSystemFontSet(&set)))
        return false;
    UINT32 count = set->GetFontCount();
    std::vector<WCHAR> buf;
    // (a property's strings, every language's; a weight, width or style: its first, a number)
    auto strings = [&](UINT32 i, DWRITE_FONT_PROPERTY_ID id, const std::function<void(const WCHAR *)> &use) {
        Com<IDWriteLocalizedStrings> values;
        BOOL exists = FALSE;
        if (FAILED(set->GetPropertyValues(i, id, &exists, &values)) || !exists || !values.p) return;
        for (UINT32 k = 0, n = values->GetCount(); k < n; k++) {
            UINT32 len = 0;
            if (FAILED(values->GetStringLength(k, &len))) continue;
            buf.resize(len + 1);
            if (SUCCEEDED(values->GetString(k, buf.data(), len + 1))) use(buf.data());
        }
    };
    auto number = [&](UINT32 i, DWRITE_FONT_PROPERTY_ID id, int def) {
        int v = def;
        bool got = false;
        strings(i, id, [&](const WCHAR *s) { if (!got) v = (int)wcstol(s, nullptr, 10), got = true; });
        return v;
    };
    // (a variable font's named instances (Bahnschrift Light, Segoe UI Variable Semibold ...) are listed one by one, all
    // with its file and index: plutovg draws only its default instance, so such a file is indexed from its own tables
    // instead, as one face, as reading the directory does)
    struct Entry { UINT32 i; std::string file; int ttc; };
    std::vector<Entry> entries;
    std::unordered_map<std::string, int> seen;   // (lower-case path # index -> how often listed)
    for (UINT32 i = 0; i < count; i++) {
        Com<IDWriteFontFaceReference> ref;
        Com<IDWriteFontFile> file;
        Com<IDWriteFontFileLoader> loader;
        Com<IDWriteLocalFontFileLoader> local;
        const void *key = nullptr;
        UINT32 keySize = 0, pathLen = 0;
        // (only a font file on this machine, not a simulated bold or oblique of another face)
        if (FAILED(set->GetFontFaceReference(i, &ref)) || ref->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE ||
            ref->GetLocality() != DWRITE_LOCALITY_LOCAL || FAILED(ref->GetFontFile(&file)) ||
            FAILED(file->GetReferenceKey(&key, &keySize)) || FAILED(file->GetLoader(&loader)) ||
            FAILED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), (void **)&local)) ||
            FAILED(local->GetFilePathLengthFromKey(key, keySize, &pathLen)))
            continue;
        std::vector<WCHAR> wpath(pathLen + 1);
        if (FAILED(local->GetFilePathFromKey(key, keySize, wpath.data(), pathLen + 1))) continue;
        std::string path = narrow(wpath.data(), CP_ACP);   // (opened with fopen, as a listed directory's files are)
        if (path.empty() || !font_file(basename(path))) continue;
        int ttc = (int)ref->GetFontFaceIndex();
        seen[lower(path.c_str()) + "#" + std::to_string(ttc)]++;
        entries.push_back({i, std::move(path), ttc});
    }
    // (the file of a face listed more than once: every face of it read from its own tables, so a static face
    // beside a variable one in a collection is not left out)
    std::vector<std::string> variable;
    for (Entry &e : entries)
        if (seen[lower(e.file.c_str()) + "#" + std::to_string(e.ttc)] > 1 &&
            std::find(variable.begin(), variable.end(), e.file) == variable.end())
            variable.push_back(e.file);
    for (Entry &e : entries) {
        UINT32 i = e.i;
        if (std::find(variable.begin(), variable.end(), e.file) != variable.end()) continue;
        Face face;
        face.file = std::move(e.file);
        face.ttc = e.ttc;
        face.weight = std::clamp(number(i, DWRITE_FONT_PROPERTY_ID_WEIGHT, 400), 1, 1000);
        int width = number(i, DWRITE_FONT_PROPERTY_ID_STRETCH, 5);
        face.width = width >= 1 && width <= 9 ? width : 5;
        face.italic = number(i, DWRITE_FONT_PROPERTY_ID_STYLE, 0) != DWRITE_FONT_STYLE_NORMAL;
        int k = (int)ix.faces.size();
        // (the name table's IDs: 1 the Win32 family, 16 the typographic one, 4 the full name, 6 the PostScript name)
        static const struct { DWRITE_FONT_PROPERTY_ID property; uint32_t id; } names[] = {
            {DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME, 1}, {DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME, 16},
            {DWRITE_FONT_PROPERTY_ID_FULL_NAME, 4}, {DWRITE_FONT_PROPERTY_ID_POSTSCRIPT_NAME, 6}};
        for (const auto &n : names) strings(i, n.property, [&](const WCHAR *s) { bind_name(ix, n.id, narrow(s, CP_UTF8), k, face); });
        ix.files[lower(basename(face.file))].push_back(k);
        ix.faces.push_back(std::move(face));
    }
    for (const std::string &file : variable) index_file(ix, file);
    return !ix.faces.empty();
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
// Linux and the BSDs: the installed fonts as fontconfig lists them, from its own cache: every family and full name
// (all languages), the PostScript name, OpenType weight, width, slant, file and index in a collection, without
// opening a font file. libfontconfig is loaded when there (a desktop's own), so the portable build runs without it;
// false where it is not: the font directories are read instead.
bool index_fontconfig(Index &ix) {
    void *lib = dlopen("libfontconfig.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return false;
    struct FcFontSet { int nfont, sfont; void **fonts; };
    auto sym = [&](const char *name) { return dlsym(lib, name); };
    auto initLoad = (void *(*)())sym("FcInitLoadConfigAndFonts");
    auto patternCreate = (void *(*)())sym("FcPatternCreate");
    auto patternDestroy = (void (*)(void *))sym("FcPatternDestroy");
    auto objectSetCreate = (void *(*)())sym("FcObjectSetCreate");
    auto objectSetAdd = (int (*)(void *, const char *))sym("FcObjectSetAdd");
    auto objectSetDestroy = (void (*)(void *))sym("FcObjectSetDestroy");
    auto fontList = (FcFontSet *(*)(void *, void *, void *))sym("FcFontList");
    auto fontSetDestroy = (void (*)(FcFontSet *))sym("FcFontSetDestroy");
    auto getString = (int (*)(void *, const char *, int, const unsigned char **))sym("FcPatternGetString");
    auto getInteger = (int (*)(void *, const char *, int, int *))sym("FcPatternGetInteger");
    auto getBool = (int (*)(void *, const char *, int, int *))sym("FcPatternGetBool");
    auto weightToOpenType = (int (*)(int))sym("FcWeightToOpenType");
    auto configDestroy = (void (*)(void *))sym("FcConfigDestroy");
    if (!initLoad || !patternCreate || !patternDestroy || !objectSetCreate || !objectSetAdd || !objectSetDestroy || !fontList ||
        !fontSetDestroy || !getString || !getInteger || !getBool || !weightToOpenType || !configDestroy) {
        dlclose(lib);
        return false;
    }
    void *config = initLoad();   // (its own configuration: the cache read, not built, when it is up to date)
    void *pattern = patternCreate(), *objects = objectSetCreate();
    bool fields = objects != nullptr;
    for (const char *o : {"file", "index", "family", "fullname", "postscriptname", "weight", "width", "slant", "variable"})
        fields = fields && objectSetAdd(objects, o);
    FcFontSet *set = config && pattern && fields ? fontList(config, pattern, objects) : nullptr;
    // (a file with a variable font in it (one listed as variable, its weight a range, or a named instance of it): read
    // from its own tables, every face of it, as one face a font; plutovg draws only a variable font's default instance)
    std::vector<std::string> variable;
    for (int i = 0; set && i < set->nfont; i++) {
        void *p = set->fonts[i];
        const unsigned char *file = nullptr;
        int index = 0, isVariable = 0, weight = 0;
        if (getString(p, "file", 0, &file) || !file || !font_file(basename(std::string((const char *)file)))) continue;
        getInteger(p, "index", 0, &index);
        if (((index >> 16) || (!getBool(p, "variable", 0, &isVariable) && isVariable) || getInteger(p, "weight", 0, &weight)) &&
            std::find(variable.begin(), variable.end(), (const char *)file) == variable.end())
            variable.push_back((const char *)file);
    }
    for (int i = 0; set && i < set->nfont; i++) {
        void *p = set->fonts[i];
        const unsigned char *file = nullptr;
        int index = 0, weight = 0, width = 100, slant = 0;
        if (getString(p, "file", 0, &file) || !file || !font_file(basename(std::string((const char *)file))) ||
            std::find(variable.begin(), variable.end(), (const char *)file) != variable.end())
            continue;
        getInteger(p, "index", 0, &index);
        getInteger(p, "weight", 0, &weight);
        getInteger(p, "width", 0, &width);
        getInteger(p, "slant", 0, &slant);
        Face face;
        face.file = (const char *)file;
        face.ttc = index;
        // (its weight scale and back: 250 comes back as 251; OpenType weights are multiples of 5)
        face.weight = std::clamp((weightToOpenType(weight) + 2) / 5 * 5, 1, 1000);
        // (fontconfig's width, % of normal (112.5 as 113): OS/2's usWidthClass 1-9, the nearest one)
        static const double widths[] = {50, 62.5, 75, 87.5, 100, 112.5, 125, 150, 200};
        face.width = 1;
        for (int k = 1; k < 9; k++)
            if (std::fabs(width - widths[k]) < std::fabs(width - widths[face.width - 1])) face.width = k + 1;
        face.italic = slant != 0;   // (italic 100, oblique 110)
        int k = (int)ix.faces.size();
        // (the name table's IDs: 1 a family (fontconfig lists the typographic and the legacy ones), 4 the full name,
        // 6 the PostScript name)
        for (const auto &n : {std::make_pair("family", 1u), std::make_pair("fullname", 4u), std::make_pair("postscriptname", 6u)}) {
            const unsigned char *v = nullptr;
            for (int j = 0; !getString(p, n.first, j, &v) && v; j++) bind_name(ix, n.second, (const char *)v, k, face);
        }
        ix.files[lower(basename(face.file))].push_back(k);
        ix.faces.push_back(std::move(face));
    }
    if (set) fontSetDestroy(set);
    if (objects) objectSetDestroy(objects);
    if (pattern) patternDestroy(pattern);
    if (config) configDestroy(config);
    for (const std::string &file : variable) index_file(ix, file);
    return !ix.faces.empty();   // (the library stays loaded: dlclose of fontconfig is not safe everywhere)
}
#endif

// the system's font directories and the user's (where Font Book, Windows 10+ and fontconfig
// install a user's own fonts)
void index_system(Index &ix) {
#ifdef _WIN32
    const char *windir = getenv("WINDIR");
    if (!index_dwrite(ix)) index_dir(ix, std::string(windir ? windir : "C:\\Windows") + "\\Fonts", 0);
    // (a user's own fonts: in the system font set since Windows 10 1809, read here for the ones before; listed ones skipped)
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
    if (index_fontconfig(ix)) return;
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
        auto [it, fresh] = ix.fontData.try_emplace(f.file);
        auto &data = it->second;
        if (fresh) {
            if (FILE *file = fopen(f.file.c_str(), "rb")) {
                long size = fseek(file, 0, SEEK_END) ? -1 : ftell(file);
                if (size >= 12 && (unsigned long)size <= UINT_MAX && !read_at(file, 0, (size_t)size, data)) data.clear();
                fclose(file);
            }
        }
        // The index and its immutable font data live as long as every cached face.
        if (!data.empty())
            f.face = plutovg_font_face_load_from_data(data.data(), (unsigned int)data.size(), f.ttc, nullptr, nullptr);
        if (f.face) ix.loaded[f.face] = k;
        else {
            if (fresh) ix.fontData.erase(it);
            f.failed = true;
        }
    }
    return f.face;
}

// --- the lookups -------------------------------------------------------------

const std::vector<int> *named_faces(const Names &names, const char *name) {
    auto it = names.find(lower(name));
    return it == names.end() ? nullptr : &it->second;
}

} // namespace

std::mutex &nf_lock() { return g_lock; }

static plutovg_font_face_t *match_face(Index &ix, const std::vector<int> *faces, int weight, bool italic, bool uprightOnly = false, int width = 5, uint32_t codepoint = 0) {
    if (!faces) return nullptr;
    weight = std::clamp(weight, 1, 1000);   // (a damaged file's)
    for (;;) {
        int best = -1;
        long bestScore = 0;
        for (int k : *faces) {
            const Face &f = ix.faces[k];
            if (f.failed || (uprightOnly && f.italic)) continue;
            if (codepoint && f.face && !nf_has_glyph(f.face, codepoint)) continue;
            int diff = std::abs(f.weight - weight);
            // The slant first, then the requested width, then the weight. Of two equally close weights
            // the one on the side of the request (a light weight: the lighter)
            bool side = weight < 400 ? f.weight < weight : f.weight > weight;
            long score = (f.italic != italic) * 100000L + std::abs(f.width - width) * 10000L + diff * 2 + !side;
            if (best < 0 || score < bestScore) {
                best = k;
                bestScore = score;
            }
        }
        if (best < 0) return nullptr;
        if (plutovg_font_face_t *f = load(ix, best))
            if (!codepoint || nf_has_glyph(f, codepoint)) return f;
    }
}

static plutovg_font_face_t *match_name(const char *name, int weight, bool italic, bool uprightOnly = false, int width = 5, uint32_t codepoint = 0) {
    Index &ix = index();
    if (const auto *family = named_faces(ix.families, name))
        return match_face(ix, family, weight, italic, uprightOnly, width, codepoint);
    return match_face(ix, named_faces(ix.faceNames, name), weight, italic, false, width, codepoint);
}

plutovg_font_face_t *nf_face(const char *family, int weight, bool italic) {
    return match_name(family, weight, italic);
}

// a face from a system list (DirectWrite, fontconfig) has the PostScript name it gives; its file may name it another way
// too (a Mac platform record: "Corbel-Light" beside "CorbelLight"): its name table read once, when a name must match
void read_names(Index &ix, int k) {
    Face &f = ix.faces[k];
    f.namesRead = true;
    FILE *file = fopen(f.file.c_str(), "rb");
    if (!file) return;
    std::vector<uint8_t> head, name;
    uint32_t off = 0;
    if (read_at(file, 0, 12, head) && u32(head.data()) == 0x74746366) {   // (a collection: its face's offset)
        std::vector<uint8_t> o;
        off = (uint32_t)f.ttc < std::min(u32(head.data() + 8), 64u) && read_at(file, 12 + 4 * (uint32_t)f.ttc, 4, o) ? u32(o.data()) : 0;
    }
    static const char *const tags[] = {"name"};
    Table t[1];
    if ((off || f.ttc == 0) && tables(file, off, tags, t, 1) && t[0].off && t[0].len <= 65536 && read_at(file, t[0].off, t[0].len, name))
        index_names(ix, name, k, f);
    fclose(file);
}

bool nf_face_file(plutovg_font_face_t *face, const char *postscript, std::string &file, int &ttc) {
    Index &ix = index();
    auto it = ix.loaded.find(face);
    if (it == ix.loaded.end()) return false;
    Face &f = ix.faces[it->second];
    const char *name = without_subset_tag(postscript);
    auto named = [&] { return std::find(f.postscript.begin(), f.postscript.end(), name) != f.postscript.end(); };
    if (!named() && !f.namesRead) read_names(ix, it->second);
    if (!named()) return false;
    file = f.file;
    ttc = f.ttc;
    return true;
}

bool nf_has_style(const char *family, bool italic) {
    Index &ix = index();
    const std::vector<int> *faces = named_faces(ix.families, family);
    if (!faces) return false;
    for (int k : *faces)
        if (!ix.faces[k].failed && ix.faces[k].italic == italic) return true;
    return false;
}

bool nf_has_glyph(plutovg_font_face_t *face, uint32_t codepoint) {
    return face && plutovg_font_face_get_glyph_index(face, codepoint) != 0;
}

static plutovg_font_face_t *first_face(const char *const *families, int weight, bool italic, int width) {
    for (; *families; families++)
        if (**families)
            if (plutovg_font_face_t *f = match_name(*families, weight, italic, false, width)) return f;
    return nullptr;
}

plutovg_font_face_t *nf_first_face(const char *const *families, int weight, bool italic) {
    return first_face(families, weight, italic, 5);
}

static plutovg_font_face_t *glyph_face(const char *const *first, uint32_t codepoint, int weight, bool italic, int width) {
    if (!codepoint) return nullptr;
    static const char *const common[] = {"Arial", "Helvetica", "Lucida Grande", "Arial Unicode MS", "Liberation Sans", "DejaVu Sans",
                                         "Noto Sans", "PingFang SC", "Hiragino Sans", "MS Gothic", "Noto Sans CJK JP",
                                         "Noto Sans CJK SC", "Noto Sans CJK TC", "Noto Sans CJK KR",
                                         "Segoe UI Symbol", "Apple Symbols", "Noto Sans Symbols 2", nullptr};
    for (; first && *first; first++)
        if (**first)
            if (plutovg_font_face_t *f = match_name(*first, weight, italic, false, width, codepoint)) return f;
    // A last-resort probe should not load every style of each large system collection.
    for (const char *const *name = common; *name; name++)
        if (plutovg_font_face_t *f = match_name(*name, weight, italic, false, width))
            if (nf_has_glyph(f, codepoint)) return f;
    return nullptr;
}

plutovg_font_face_t *nf_glyph_face(const char *const *first, uint32_t codepoint, int weight, bool italic) {
    return glyph_face(first, codepoint, weight, italic, 5);
}

// (Adobe Caslon Pro: Times New Roman's widths are closest, as a CD label's lettering moved one by one
// along its circle shows; with Arial it ran off the disc.)
const char *nf_substitute(const char *family) {
    static const char *const subst[][2] = {{"AvantGarde", "Avenir"}, {"Century Gothic", "Avenir"},
                                           {"Adobe Caslon Pro", "Times New Roman"},
                                           // Bitstream scripts: like ones macOS has, then Windows'
                                           {"Lydian Csv", "Apple Chancery"}, {"CommercialScript", "Snell Roundhand"},
                                           {"Apple Chancery", "Lucida Handwriting"}, {"Snell Roundhand", "Lucida Handwriting"},
                                           // Microsoft's old name of Segoe UI (Visio 2007's posters)
                                           {"Segoe", "Segoe UI"},
                                           // Bitstream's clones of the core fonts (CorelDRAW's, AutoCAD's)
                                           {"Dutch801", "Times New Roman"}, {"Swiss721", "Arial"}, {"Swis721", "Arial"},
                                           {"Arial", "Liberation Sans"}, {"Helvetica", "Liberation Sans"},
                                           {"Times New Roman", "Liberation Serif"}, {"Times", "Liberation Serif"},
                                           {"Courier New", "Liberation Mono"}, {"Courier", "Liberation Mono"}};
    if (!family) return nullptr;
    for (const auto &e : subst)
        if (!strcasecmp(family, e[0])) return e[1];
    return nullptr;
}

const char *nf_generic_of(const char *family) {
    if (!family || !*family) return nullptr;
    std::string l = lower(family);
    auto has = [&](const char *w) { return l.find(w) != std::string::npos; };
    static const char *const mono[] = {"mono", "courier", "consol", "menlo", "monaco", " code", "typewriter", "fixedsys",
                                       "terminal", "inconsolata", "ocr-", "ocr "};
    for (const char *w : mono)
        if (has(w)) return "monospace";
    if (has("sans")) return nullptr;
    // (not "roman": AutoCAD's romans / romanc stroke fonts, Linotype's "Helvetica Neue 55 Roman" are no serif ones)
    static const char *const serif[] = {"serif", "times", "georgia", "garamond", "caslon", "palatino", "cambria",
                                        "baskerville", "bookman", "book antiqua", "century schoolbook", "didot", "bodoni",
                                        "minion", "charter", "constantia"};
    for (const char *w : serif)
        if (has(w)) return "serif";
    return nullptr;
}

namespace {

// Bitstream's names of old CorelDRAW fonts: "Futura Lt BT", "AvantGarde Bk BT", "Futura XBlkCnIt BT":
// The last word encodes the face's weight, slant and width.
bool bitstream_name(std::string &family, int &weight, bool &italic, int &width) {
    if (!ends_with_ci(family, " BT")) return false;
    family.resize(family.size() - 3);
    size_t sp = family.rfind(' ');
    if (sp == std::string::npos) return true;
    static const struct { const char *abbr; int weight; int width = 0; } abbrs[] = {   // weight -1: italic, 0: width
        {"XBlk", 900}, {"Blk", 900}, {"XBd", 800}, {"Bd", 700}, {"Hv", 800}, {"It", -1}, {"XLt", 200}, {"Lt", 300},
        {"Bk", 400}, {"Md", 500}, {"XCn", 0, 2}, {"Cn", 0, 3}, {"Ex", 0, 7}};
    int w = weight, wd = width;
    bool it = italic;
    for (size_t i = sp + 1; i < family.size();) {
        bool found = false;
        for (const auto &a : abbrs) {
            size_t n = strlen(a.abbr);
            if (!family.compare(i, n, a.abbr)) {
                if (a.weight > 0) w = std::max(a.weight, weight > 400 ? weight : 0);   // (bold stays bold)
                if (a.weight < 0) it = true;
                if (a.width) wd = a.width;
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
    width = wd;
    return true;
}

struct FontName {
    std::string original;   // (trimmed and without the PDF subset tag)
    std::string filename;   // (a TrueType or OpenType file's basename)
    std::string raw;        // (the name less its subset tag and PostScript style)
    std::string family;     // (its family, its style words off)
    int width = 5;
    bool bitstream = false;
    bool file = false;      // (a font file's name: its family from the file, nothing else to try)
};

FontName parse_name(const char *name, int &weight, bool &italic) {
    FontName p;
    std::string s = name ? name : "";
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t lead = s.find_first_not_of(" \t");
    s = lead == std::string::npos ? std::string() : s.substr(lead);
    // a PDF subset tag: six capitals and a +
    s.erase(0, without_subset_tag(s.c_str()) - s.c_str());
    // a font file (DXF's text styles): Windows' core fonts by their file names, else the file's own name; an AutoCAD SHX
    // stroke font: none (a plain sans serif)
    std::string low = lower(s.c_str());
    p.original = s;
    if (font_file(low) || ends_with_ci(low, ".shx")) {
        std::string f = basename(s), stem = lower(f.c_str());
        stem.resize(stem.size() - 4);
        p.file = true;
        if (ends_with_ci(low, ".shx")) return p;
        p.filename = f;
        static const struct { const char *file, *family; bool bold, italic; } files[] = {
            {"arial", "Arial", 0, 0},          {"arialbd", "Arial", 1, 0},      {"ariali", "Arial", 0, 1},
            {"arialbi", "Arial", 1, 1},        {"arialn", "Arial Narrow", 0, 0}, {"arialnb", "Arial Narrow", 1, 0},
            {"times", "Times New Roman", 0, 0}, {"timesbd", "Times New Roman", 1, 0}, {"timesi", "Times New Roman", 0, 1},
            {"cour", "Courier New", 0, 0},     {"courbd", "Courier New", 1, 0}, {"verdana", "Verdana", 0, 0},
            {"tahoma", "Tahoma", 0, 0},        {"calibri", "Calibri", 0, 0},    {"calibrib", "Calibri", 1, 0},
            {"isocpeur", "ISOCPEUR", 0, 0},    {"isocteur", "ISOCTEUR", 0, 0},  {"swiss", "Swis721 BT", 0, 0},
            {"simplex", "Arial", 0, 0},        {"gothic", "MS Gothic", 0, 0},   {"century", "Century", 0, 0}};
        p.family = f.substr(0, f.size() - 4);
        for (const auto &e : files)
            if (stem == e.file) {
                p.family = e.family;
                if (e.bold) weight = std::max(weight, 700);
                if (e.italic) italic = true;
            }
        p.raw = p.family;
        return p;
    }
    // a PostScript name's style: after a comma ("Arial,Bold"), or a dash in a name without spaces ("Helvetica-BoldOblique")
    std::string style;
    size_t cut = s.find(',');
    if (cut == std::string::npos && s.find(' ') == std::string::npos) cut = s.find('-');
    if (cut != std::string::npos) style = lower(s.substr(cut + 1).c_str()), s.resize(cut);
    style.erase(std::remove_if(style.begin(), style.end(), [](char c) { return c == ' ' || c == '-'; }), style.end());
    auto in = [&](const char *w) { return style.find(w) != std::string::npos; };
    if (in("black") || in("heavy")) weight = std::max(weight, 900);
    else if (in("extrabold") || in("ultrabold")) weight = std::max(weight, 800);
    else if (in("semibold") || in("demi")) weight = std::max(weight, 600);
    else if (in("bold")) weight = std::max(weight, 700);
    else if (in("medium")) weight = std::max(weight, 500);
    else if (in("thin")) weight = std::min(weight, 100);
    else if (in("extralight") || in("ultralight")) weight = std::min(weight, 200);
    else if (in("semilight")) weight = std::min(weight, 350);
    else if (in("light")) weight = std::min(weight, 300);
    if (in("italic") || in("oblique")) italic = true;
    for (const auto &styleWidth : widths)
        if (in(styleWidth.compact)) { p.width = styleWidth.width; break; }
    p.raw = s;
    // a PostScript family in one word: its PS / MT marks off, its words spaced ("TimesNewRomanPSMT": "Times New Roman")
    std::string family = s;
    if (family.find(' ') == std::string::npos) {
        for (const char *mark : {"PSMT", "PS", "MT"})
            if (family.size() > strlen(mark) + 2 && ends_with_ci(family, mark) && !strcmp(family.c_str() + family.size() - strlen(mark), mark)) {
                family.resize(family.size() - strlen(mark));
                break;
            }
        std::string spaced;
        for (size_t k = 0; k < family.size(); k++) {
            if (k && isupper((unsigned char)family[k]) && islower((unsigned char)family[k - 1])) spaced.push_back(' ');
            spaced.push_back(family[k]);
        }
        family = spaced;
    }
    p.bitstream = bitstream_name(family, weight, italic, p.width);
    // style words at its end (CorelDRAW's, Visio's): "Arial CE", "Times New Roman Bold", "Segoe UI Light"
    for (bool changed = true; changed;) {
        changed = false;
        static const char *const scripts[] = {" CE", " Cyr", " Baltic", " Greek", " Tur"};
        for (const char *sfx : scripts)
            if (ends_with_ci(family, sfx)) { family.resize(family.size() - strlen(sfx)); changed = true; }
        // (a localized Windows' name of a CE font: "Tahoma Közép-európai", "Courier New Mitteleuropäisch", in whatever
        // code page it came: its last word by its ASCII part)
        size_t sp = family.rfind(' ');
        if (sp != std::string::npos && sp > 0) {
            std::string last = lower(family.c_str() + sp + 1);
            if (last.find("-eur") != std::string::npos || last.find("mitteleurop") != std::string::npos) { family.resize(sp); changed = true; }
        }
        static const char *const italics[] = {" Italic", " Oblique"};
        for (const char *sfx : italics)
            if (ends_with_ci(family, sfx)) { family.resize(family.size() - strlen(sfx)); italic = changed = true; }
        static const struct { const char *suffix; int weight; } weights[] = {
            {" Semi Bold", 600}, {" Demi Bold", 600}, {" Extra Bold", 800}, {" Ultra Bold", 800},
            {" Semi Light", 350}, {" Extra Light", 200}, {" Ultra Light", 200},
            {" Semibold", 600}, {" Demibold", 600}, {" Extrabold", 800}, {" Ultrabold", 800},
            {" Semilight", 350}, {" Extralight", 200}, {" Ultralight", 200},
            {" Bold", 700}, {" Black", 900}, {" Heavy", 900}, {" Medium", 500}, {" Light", 300}, {" Thin", 100}};
        for (const auto &w : weights)
            if (ends_with_ci(family, w.suffix)) {
                family.resize(family.size() - strlen(w.suffix));
                weight = w.weight < 400 ? std::min(weight, w.weight) : std::max(weight, w.weight);
                changed = true;
                break;
            }
        for (const auto &w : widths)
            if (ends_with_ci(family, w.name)) {
                family.resize(family.size() - strlen(w.name));
                p.width = w.width;
                changed = true;
                break;
            }
    }
    p.family = family;
    return p;
}

} // namespace

std::string nf_font_family(const char *name, int &weight, bool &italic) { return parse_name(name, weight, italic).family; }

static plutovg_font_face_t *find_own_face(const FontName &p, int weight, bool italic, uint32_t codepoint = 0) {
    Index &ix = index();
    if (!p.filename.empty())
        if (plutovg_font_face_t *f = match_face(ix, named_faces(ix.files, p.filename.c_str()), weight, italic, false, p.width, codepoint)) return f;
    std::string linotype = p.bitstream ? p.family + " LT" : std::string();
    const char *own[] = {p.file ? "" : p.original.c_str(), p.raw.c_str(), linotype.c_str(), p.family.c_str()};
    for (const char *t : own)
        if (*t)
            if (plutovg_font_face_t *f = match_name(t, weight, italic, !italic && !codepoint, p.width, codepoint)) return f;
    return nullptr;
}

plutovg_font_face_t *nf_find_face(const char *name, int weight, bool italic, NfKind kind, const char *file) {
    int fileWeight = weight;
    bool fileItalic = italic;
    FontName p = parse_name(name, weight, italic);
    if (plutovg_font_face_t *f = find_own_face(p, weight, italic)) return f;
    FontName alt = parse_name(file, fileWeight, fileItalic);
    if (alt.file)
        if (plutovg_font_face_t *f = find_own_face(alt, fileWeight, fileItalic)) return f;
    if (p.family.empty() && alt.file && !alt.family.empty()) {
        p = std::move(alt);
        weight = fileWeight;
        italic = fileItalic;
    }
    const char *sub = p.family.empty() ? nullptr : nf_substitute(p.family.c_str());
    const char *sub2 = sub ? nf_substitute(sub) : nullptr;
    for (const char *t : {sub, sub2})
        if (t)
            if (plutovg_font_face_t *f = match_name(t, weight, italic, !italic, p.width)) return f;
    // then a core font of its kind: by what the format says, else by its name (an SHX stroke font: sans serif)
    const char *g = kind == NF_SERIF ? "serif" : kind == NF_MONO ? "monospace" : kind == NF_SANS || p.file ? nullptr : nf_generic_of(name);
    if (!g && kind == NF_ANY && !p.file) g = nf_generic_of(p.family.c_str());
    static const char *const serif[] = {"Times New Roman", "Times", "Liberation Serif", "DejaVu Serif", nullptr};
    static const char *const mono[] = {"Courier New", "Courier", "Liberation Mono", "DejaVu Sans Mono", nullptr};
    if (g)
        if (plutovg_font_face_t *f = first_face(!strcmp(g, "serif") ? serif : mono, weight, italic, p.width)) return f;
    // the last resorts: a light, medium or semibold weight from a family that has such faces (Arial has regular and bold
    // only); a missing semibold as a medium, the nearer in width (Visio's Segoe UI Semibold labels fit their boxes in
    // Arial Bold no more)
    bool between = weight != 400 && weight != 700;
    if (between)
        if (plutovg_font_face_t *f = match_name("Helvetica Neue", weight == 600 ? 500 : weight, italic, false, p.width)) return f;
    static const char *const sans[] = {"Arial", "Helvetica", "Liberation Sans", "DejaVu Sans", nullptr};
    return first_face(sans, weight == 600 ? 500 : weight, italic, p.width);
}

plutovg_font_face_t *nf_find_glyph_face(const char *name, int weight, bool italic, uint32_t codepoint, const char *file) {
    if (!codepoint) return nullptr;
    int fileWeight = weight;
    bool fileItalic = italic;
    FontName p = parse_name(name, weight, italic);
    if (plutovg_font_face_t *f = find_own_face(p, weight, italic, codepoint)) return f;
    if (file && *file) {
        FontName alt = parse_name(file, fileWeight, fileItalic);
        if (alt.file)
            if (plutovg_font_face_t *f = find_own_face(alt, fileWeight, fileItalic, codepoint)) return f;
        if (p.family.empty() && alt.file) {
            p = std::move(alt);
            weight = fileWeight;
            italic = fileItalic;
        }
    }
    const char *sub = nf_substitute(p.family.c_str());
    const char *sub2 = sub ? nf_substitute(sub) : nullptr;
    const char *first[] = {sub ? sub : "", sub2 ? sub2 : "", nullptr};
    return glyph_face(first, codepoint, weight, italic, p.width);
}

plutovg_font_face_t *nf_css_face_weight(const char *family, int weight, bool italic) {
    if (family && *family) {
        if (plutovg_font_face_t *f = nf_face(family, weight, italic)) return f;
        // (not installed: its substitute, that one's own (Linux's Liberation one), then by its name a fixed-width or serif
        // one, so a monospaced label (Lucida Console, Consolas) stays monospaced)
        const char *sub = nf_substitute(family), *sub2 = sub ? nf_substitute(sub) : nullptr;
        for (const char *t : {sub, sub2})
            if (t)
                if (plutovg_font_face_t *f = nf_face(t, weight, italic)) return f;
        if (const char *g = nf_generic_of(family))
            if (strcmp(g, family) != 0) return nf_css_face_weight(g, weight, italic);
    }
    // Keep the platform's usual choices first, then try the other core families.
#if defined(__linux__)
    static const char *const sans[] = {"DejaVu Sans", "Liberation Sans", "Noto Sans", "Arial", "Helvetica", nullptr};
    static const char *const serif[] = {"DejaVu Serif", "Liberation Serif", "Noto Serif", "Times New Roman", "Times", nullptr};
    static const char *const mono[] = {"DejaVu Sans Mono", "Liberation Mono", "Noto Sans Mono", "Courier New", "Courier", nullptr};
#else
    static const char *const sans[] = {"Arial", "Helvetica", "Liberation Sans", "DejaVu Sans", "Noto Sans", nullptr};
    static const char *const serif[] = {"Times New Roman", "Times", "Liberation Serif", "DejaVu Serif", "Noto Serif", nullptr};
    static const char *const mono[] = {"Courier New", "Courier", "Liberation Mono", "DejaVu Sans Mono", "Noto Sans Mono", nullptr};
#endif
    std::string generic = family ? lower(family) : "";
    if (generic == "serif") return nf_first_face(serif, weight, italic);
    if (generic == "monospace") return nf_first_face(mono, weight, italic);
    if (generic == "cursive") return nf_face("Comic Sans MS", weight, italic);
    if (generic == "fantasy") return nf_face("Impact", weight, italic);
    if (generic.empty() || generic == "sans-serif") {
        if (plutovg_font_face_t *f = nf_first_face(sans, weight, italic)) return f;
        Index &ix = index();
        for (const auto &entry : ix.families)
            if (plutovg_font_face_t *f = match_face(ix, &entry.second, weight, italic)) return f;
        for (size_t i = 0; i < ix.faces.size(); i++)
            if (plutovg_font_face_t *f = load(ix, (int)i)) return f;
    }
    return nullptr;
}

plutovg_font_face_t *nf_css_face(const char *family, bool bold, bool italic) {
    return nf_css_face_weight(family, bold ? 700 : 400, italic);
}

plutovg_font_face_t *nf_css_face_list(const char *families, int weight, bool italic, uint32_t codepoint) {
    const char *p = families;
    while (*p) {
        std::string family;
        char quote = 0;
        for (; *p; ++p) {
            if (*p == '\\' && p[1]) family.push_back(*++p);
            else if (quote && *p == quote) quote = 0;
            else if (!quote && (*p == '\'' || *p == '"')) quote = *p;
            else if (!quote && *p == ',') { ++p; break; }
            else family.push_back(*p);
        }
        size_t first = family.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        family = family.substr(first, family.find_last_not_of(" \t\r\n") - first + 1);
        const char *name = family.c_str();
        if (codepoint) {
            if (plutovg_font_face_t *f = match_name(name, weight, italic, false, 5, codepoint)) return f;
            const char *sub = nf_substitute(name);
            for (const char *t : {sub, sub ? nf_substitute(sub) : nullptr})
                if (t)
                    if (plutovg_font_face_t *f = match_name(t, weight, italic, false, 5, codepoint)) return f;
            if (plutovg_font_face_t *f = nf_css_face_weight(name, weight, italic))
                if (nf_has_glyph(f, codepoint)) return f;
        } else if (plutovg_font_face_t *f = nf_css_face_weight(name, weight, italic)) return f;
    }
    return codepoint ? nf_glyph_face(nullptr, codepoint, weight, italic) : nf_css_face_weight("", weight, italic);
}

uint32_t nf_utf8_next(const std::string &s, size_t &i) {
    unsigned char c = (unsigned char)s[i++];
    int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    uint32_t cp = n ? c & (0x3f >> n) : c;
    for (; n && i < s.size() && ((unsigned char)s[i] & 0xc0) == 0x80; n--) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3f);
    return cp;
}

void nf_codepage(const uint8_t *s, size_t n, unsigned cp, std::vector<uint32_t> &out) {
    auto latin1 = [cp](uint8_t b) -> uint32_t { return cp == 1252 && b >= 0x80 && b < 0xa0 ? '?' : b; };
#ifdef _WIN32
    int m = MultiByteToWideChar(cp, 0, (const char *)s, (int)n, nullptr, 0);
    if (m > 0) {
        std::vector<wchar_t> wide((size_t)m);
        MultiByteToWideChar(cp, 0, (const char *)s, (int)n, wide.data(), m);
        for (int i = 0; i < m; i++) {   // (UTF-16: its surrogate pairs joined)
            uint32_t c = (uint16_t)wide[(size_t)i];
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < m && (uint16_t)wide[(size_t)i + 1] >= 0xdc00 && (uint16_t)wide[(size_t)i + 1] < 0xe000)
                c = 0x10000 + ((c - 0xd800) << 10) + ((uint16_t)wide[(size_t)++i] - 0xdc00);
            out.push_back(c);
        }
        return;
    }
#else
    // the converters stay open (a drawing of thousands of texts), shared by the loads under their
    // own lock; a code page without one is remembered too (ISO 8859 and Mac Roman by their names,
    // for macOS's iconv)
    static std::mutex lock;
    static std::unordered_map<unsigned, iconv_t> cds;
    std::lock_guard<std::mutex> g(lock);
    auto it = cds.find(cp);
    if (it == cds.end()) {
        std::string name = cp > 28590 && cp < 28600 ? "ISO-8859-" + std::to_string(cp - 28590) : cp == 10000 ? "MACINTOSH" : "CP" + std::to_string(cp);
        it = cds.emplace(cp, iconv_open("UTF-32LE", name.c_str())).first;
    }
    iconv_t cd = it->second;
    if (cd != (iconv_t)-1) {
        iconv(cd, nullptr, nullptr, nullptr, nullptr);
        char *src = (char *)s;
        size_t left = n;
        while (left) {
            unsigned char buf[4 * 64];
            char *dst = (char *)buf;
            size_t room = sizeof buf;
            size_t r = iconv(cd, &src, &left, &dst, &room);
            for (size_t k = 0; k + 4 <= sizeof buf - room; k += 4)
                out.push_back(buf[k] | buf[k + 1] << 8 | buf[k + 2] << 16 | (uint32_t)buf[k + 3] << 24);
            if (r == (size_t)-1 && errno != E2BIG) {
                if (errno != EILSEQ) break;   // (an incomplete character at the end)
                out.push_back(latin1((uint8_t)*src));   // a byte the code page does not define: as Latin-1, then on with the rest
                src++, left--;
            }
        }
        return;
    }
#endif
    for (size_t i = 0; i < n; i++) out.push_back(latin1(s[i]));
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
