// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// The DWG structures as ACadSharp reads them (MIT, Copyright (c) 2021 Albert Domenech,
// https://github.com/DomCR/ACadSharp) and the Open Design Specification for .dwg files 5.4.1.
//
// nitrodwg: AutoCAD DWG drawings R13 - R2000 (AC1012, AC1014, AC1015) as the group code / value
// pairs of their DXF (nitrodwg.h). The file header's section locators lead to the header
// variables (the few nitrodxf uses), the classes and the object map (handle -> file offset).
// The table entries, block headers and layouts are read first, then each block's entities in
// their order (the entities' next links), an insert with its attributes and a polyline with its
// vertices, as in a DXF. What nitrodxf doesn't draw is left out; a damaged object is skipped,
// not the drawing.
#include "nitrodwg.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace {

using sv = std::string_view;

struct P3 {
    double x = 0, y = 0, z = 0;
};

const double DEG = 180 / M_PI;

// --- the bit stream ----------------------------------------------------------------

// The bit codes of the specification (B, BB, BS, BL, BD, DD, RC ... H) on a byte range, MSB
// first. Reading past its end gives zeros and sets bad.
struct Bits {
    const uint8_t *d = nullptr;
    size_t end = 0, pos = 0;   // in bits
    bool bad = false;
    Bits() {}
    Bits(const uint8_t *data, size_t endByte, size_t startByte) : d(data), end(endByte * 8), pos(startByte * 8) {
        if (pos > end) pos = end, bad = true;
    }
    size_t left() const { return end - pos; }
    void seek(size_t bit) {
        if (bit > end) bad = true, bit = end;
        pos = bit;
    }
    void skip(size_t bits) {
        if (bits > left()) bad = true, bits = left();
        pos += bits;
    }
    unsigned B() {
        if (pos >= end) {
            bad = true;
            return 0;
        }
        unsigned b = d[pos >> 3] >> (7 - (pos & 7)) & 1;
        pos++;
        return b;
    }
    unsigned BB() {
        unsigned a = B();
        return a << 1 | B();
    }
    uint8_t RC() {
        if (left() < 8) {
            bad = true;
            pos = end;
            return 0;
        }
        size_t i = pos >> 3;
        unsigned s = pos & 7;
        pos += 8;
        return s ? (uint8_t)(d[i] << s | d[i + 1] >> (8 - s)) : d[i];
    }
    uint16_t RS() {
        uint16_t a = RC();
        return (uint16_t)(a | RC() << 8);
    }
    uint32_t RL() {
        uint32_t a = RS();
        return a | (uint32_t)RS() << 16;
    }
    double RD() {
        uint8_t b[8];
        for (uint8_t &x : b) x = RC();
        double v;
        memcpy(&v, b, 8);
        return v;
    }
    int BS() {
        switch (BB()) {
        case 0: return (int16_t)RS();
        case 1: return RC();
        case 2: return 0;
        default: return 256;
        }
    }
    int32_t BL() {
        switch (BB()) {
        case 0: return (int32_t)RL();
        case 1: return RC();
        case 2: return 0;
        default: bad = true; return 0;
        }
    }
    uint64_t BLL() {
        unsigned n = B() << 2;
        n |= B() << 1;
        n |= B();
        uint64_t v = 0;
        for (unsigned k = 0; k < n; k++) v |= (uint64_t)RC() << (8 * k);
        return v;
    }
    double BD() {
        switch (BB()) {
        case 0: return RD();
        case 1: return 1;
        case 2: return 0;
        default: bad = true; return 0;
        }
    }
    double DD(double def) {   // a double with a default: the default's bytes, some patched
        unsigned c = BB();
        if (c == 0) return def;
        if (c == 3) return RD();
        uint8_t b[8];
        memcpy(b, &def, 8);
        if (c == 2) b[4] = RC(), b[5] = RC();
        for (int k = 0; k < 4; k++) b[k] = RC();
        double v;
        memcpy(&v, b, 8);
        return v;
    }
    P3 BD2() {
        P3 p;
        p.x = BD();
        p.y = BD();
        return p;
    }
    P3 BD3() {
        P3 p = BD2();
        p.z = BD();
        return p;
    }
    P3 RD2() {
        P3 p;
        p.x = RD();
        p.y = RD();
        return p;
    }
    P3 RD3() {
        P3 p = RD2();
        p.z = RD();
        return p;
    }
    P3 DD2(P3 def) {
        P3 p = def;
        p.x = DD(def.x);
        p.y = DD(def.y);
        return p;
    }
    P3 DD3(P3 def) {
        P3 p = DD2(def);
        p.z = DD(def.z);
        return p;
    }
    uint64_t MC() {   // modular char, unsigned
        uint64_t v = 0;
        for (int sh = 0; sh < 64; sh += 7) {
            uint8_t c = RC();
            v |= (uint64_t)(c & 0x7f) << sh;
            if (!(c & 0x80) || bad) return v;
        }
        bad = true;
        return v;
    }
    int64_t MCs() {   // modular char, signed (0x40 of the last byte)
        uint64_t v = 0;
        for (int sh = 0; sh < 63; sh += 7) {
            uint8_t c = RC();
            if (!(c & 0x80) || bad) {
                v |= (uint64_t)(c & 0x3f) << sh;
                return c & 0x40 ? -(int64_t)v : (int64_t)v;
            }
            v |= (uint64_t)(c & 0x7f) << sh;
        }
        bad = true;
        return 0;
    }
    uint32_t MS() {   // modular short
        uint32_t v = 0;
        for (int sh = 0; sh < 32; sh += 15) {
            uint32_t w = RS();
            v |= (w & 0x7fff) << sh;
            if (!(w & 0x8000) || bad) return v;
        }
        bad = true;
        return v;
    }
    uint64_t H(uint64_t ref) {   // a handle reference: absolute, or relative to ref (the object's)
        uint8_t f = RC();
        unsigned code = f >> 4, n = f & 15;
        if (n > 8) {
            bad = true;
            return 0;
        }
        uint64_t v = 0;
        for (unsigned k = 0; k < n; k++) v = v << 8 | RC();
        switch (code) {
        case 6: return ref + 1;
        case 8: return ref - 1;
        case 0xa: return ref + v;
        case 0xc: return ref - v;
        default:
            if (code <= 5) return v;
            bad = true;
            return 0;
        }
    }
    std::string TV() {   // R13 - R2004: in the drawing's code page
        int n = BS();
        std::string s;
        if (n <= 0) return s;
        if ((size_t)n * 8 > left()) {
            bad = true;
            return s;
        }
        s.reserve((size_t)n);
        for (int k = 0; k < n; k++)
            if (char c = (char)RC()) s.push_back(c);
        return s;
    }
};

// --- the drawing -------------------------------------------------------------------

enum : int {
    T_TEXT = 1, T_ATTRIB, T_ATTDEF, T_BLOCK, T_ENDBLK, T_SEQEND, T_INSERT, T_MINSERT,
    T_VERTEX_2D = 0x0a, T_VERTEX_3D, T_VERTEX_MESH, T_VERTEX_PFACE, T_VERTEX_PFACE_FACE,
    T_POLYLINE_2D, T_POLYLINE_3D, T_ARC, T_CIRCLE, T_LINE, T_DIM_ORDINATE, T_DIM_LINEAR,
    T_DIM_ALIGNED, T_DIM_ANG3PT, T_DIM_ANG2LN, T_DIM_RADIUS, T_DIM_DIAMETER, T_POINT, T_3DFACE,
    T_POLYLINE_PFACE, T_POLYLINE_MESH, T_SOLID, T_TRACE, T_SHAPE, T_VIEWPORT, T_ELLIPSE, T_SPLINE,
    T_RAY = 0x28, T_XLINE, T_MTEXT = 0x2c, T_LEADER, T_BLOCK_HEADER = 0x31, T_LAYER = 0x33,
    T_STYLE = 0x35, T_LTYPE = 0x39, T_APPID = 0x43, T_DIMSTYLE = 0x45, T_LWPOLYLINE = 0x4d,
    T_HATCH, T_LAYOUT = 0x52,
    // classes (by their DXF name)
    T_IMAGE = 1000, T_WIPEOUT, T_ARC_DIMENSION, T_LARGE_RADIAL_DIMENSION, T_ACIDBLOCKREFERENCE,
    T_OTHER_ENTITY, T_OTHER_OBJECT
};

struct Eed {   // an extended data item
    uint64_t app;
    int code;   // 1000 ...
    std::string s;
    P3 p;
    double r = 0;
    int64_t i = 0;
};

struct Obj {   // an object being read: its data (with the strings) and handle streams
    Bits b, h;
    uint64_t handle = 0;
    int type = 0;
    size_t start = 0;   // (the bit of its type: the handle stream's offsets are from here)
    std::vector<Eed> eed;
};

struct Ent {   // an entity's common data
    uint64_t owner = 0, layer = 0, ltype = 0, next = 0;
    int ltflags = 0;   // 0 BYLAYER, 1 BYBLOCK, 2 CONTINUOUS, 3 ltype
    int color = 256;
    uint32_t rgb = 0;   // (R2004+: a true colour, 0x1000000 set)
    double ltscale = 1;
    bool invisible = false;
};

struct Dwg {
    const uint8_t *d;
    size_t len;
    int ver = 0;   // 1012 ...
    nitrodwg_pairs &out;
    Dwg(const uint8_t *data, size_t n, nitrodwg_pairs &o) : d(data), len(n), out(o) {}

    bool r13_14() const { return ver <= 1014; }
    bool r2000p() const { return ver >= 1015; }
    bool r2004p() const { return ver >= 1018; }
    bool r2007p() const { return ver >= 1021; }
    bool r2010p() const { return ver >= 1024; }
    bool r2013p() const { return ver >= 1027; }

    // the sections (R13 - R2000: the file header's locators)
    struct Loc {
        size_t seeker = 0, size = 0;
    } loc[6];
    int codepage = 30;
    std::unordered_map<int, int> classes;   // class number -> T_ ...
    std::vector<std::pair<uint64_t, uint64_t>> map;   // (handle, file offset), by handle
    std::vector<uint8_t> written;                     // (map's entities written)

    // header variables
    double ltscale = 1, pdsize = 0, dimscale = 1, dimasz = 0.18;
    int pdmode = 0;
    P3 extmin, extmax;

    // tables
    struct Layer {
        uint64_t handle;
        std::string name;
        int color = 7;
        uint32_t rgb = 0;
        bool off = false, frozen = false;
        uint64_t ltype = 0;
    };
    struct LType {
        uint64_t handle;
        std::string name;
        std::vector<double> dashes;
    };
    struct Style {
        uint64_t handle;
        std::string name, font;
        bool shape = false;
        double height = 0, width = 1, oblique = 0;
        std::vector<Eed> eed;
    };
    struct DimStyle {
        uint64_t handle;
        std::string name;
        double scale = 1, asz = 0.18;
        uint64_t ldrblk = 0;
    };
    struct BlockH {
        uint64_t handle;
        std::string name;
        bool anon = false, xref = false;
        P3 base;
        uint64_t block = 0, first = 0, last = 0;
        std::vector<uint64_t> owned;   // (R2004+)
    };
    struct Layout {
        int tab;
        uint64_t block;
    };
    std::vector<Layer> layers;
    std::vector<LType> ltypes;
    std::vector<Style> styles;
    std::vector<DimStyle> dimstyles;
    std::vector<BlockH> blocks;
    std::vector<Layout> layouts;
    std::unordered_map<uint64_t, std::string> names;   // table entry / block header / appid handle -> name

    // --- the output

    void put(int code, sv v) {
        out.code.push_back(code);
        out.text.append(v.data(), v.size());
        out.end.push_back((uint32_t)out.text.size());
    }
    void str(int code, const std::string &s) { put(code, s); }
    void real(int code, double v) {
        char b[9];
        b[0] = 1;
        memcpy(b + 1, &v, 8);
        put(code, sv(b, 9));
    }
    void inum(int code, long long v) {
        char b[24], *e = b + sizeof b, *p = e;
        unsigned long long u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
        do *--p = (char)('0' + u % 10);
        while (u /= 10);
        if (v < 0) *--p = '-';
        put(code, sv(p, (size_t)(e - p)));
    }
    void hex(int code, uint64_t h) {
        char b[16], *e = b + sizeof b, *p = e;
        do *--p = "0123456789ABCDEF"[h & 15];
        while (h >>= 4);
        put(code, sv(p, (size_t)(e - p)));
    }
    void pt(int code, P3 p, bool z = true) {
        real(code, p.x);
        real(code + 10, p.y);
        if (z) real(code + 20, p.z);
    }
    void rec(const char *type) { put(0, type); }
    size_t mark() const { return out.code.size(); }
    void rollback(size_t m) {
        out.code.resize(m);
        out.end.resize(m);
        out.text.resize(m ? out.end[m - 1] : 0);
    }
    const std::string &name(uint64_t h) {
        static const std::string none;
        auto it = names.find(h);
        return it == names.end() ? none : it->second;
    }

    // --- sections

    uint32_t rl(size_t at) const { return at + 4 <= len ? (uint32_t)(d[at] | d[at + 1] << 8 | d[at + 2] << 16 | (uint32_t)d[at + 3] << 24) : 0; }

    bool fileHeader() {
        if (len < 0x19) return false;
        codepage = d[0x13] | d[0x14] << 8;
        uint32_t n = rl(0x15);
        for (uint32_t i = 0; i < n && i < 16; i++) {
            size_t at = 0x19 + (size_t)i * 9;
            if (at + 9 > len) return false;
            int num = d[at];
            if (num < 6) loc[num] = {rl(at + 1), rl(at + 5)};
        }
        return loc[2].seeker > 0 && loc[2].seeker < len;
    }

    // a section's bit data (after its start sentinel and size)
    bool section(int i, Bits &b) {
        const Loc &l = loc[i];
        if (!l.seeker || l.seeker + 20 > len) return false;
        size_t size = rl(l.seeker + 16), from = l.seeker + 20;
        b = Bits(d, std::min(len, from + size), from);
        return true;
    }

    // Header variables up to DIMASZ (those after it aren't used)
    void header() {
        Bits b;
        if (!section(0, b)) return;
        if (r2013p()) b.BLL();
        for (int k = 0; k < 4; k++) b.BD();
        for (int k = 0; k < 4; k++) b.TV();
        b.BL(), b.BL();
        if (r13_14()) b.BS();
        if (!r2004p()) b.H(0);
        b.B(), b.B();
        if (r13_14()) b.B();
        for (int k = 0; k < 7; k++) b.B();   // PLINEGEN ... LIMCHECK
        if (r13_14()) b.B();
        if (r2004p()) b.B();
        for (int k = 0; k < 4; k++) b.B();   // USRTIMER ... SPLFRAME
        if (r13_14()) b.B(), b.B();
        b.B(), b.B();   // MIRRTEXT, WORLDVIEW
        if (r13_14()) b.B();
        b.B(), b.B(), b.B();   // TILEMODE, PLIMCHECK, VISRETAIN
        if (r13_14()) b.B();
        b.B(), b.B();   // DISPSILH, PELLIPSE
        b.BS();         // PROXYGRAPHICS
        if (r13_14()) b.BS();
        for (int k = 0; k < 5; k++) b.BS();   // TREEDEPTH ... AUPREC
        if (r13_14()) b.BS();
        b.BS();   // ATTMODE
        if (r13_14()) b.BS();
        pdmode = b.BS();
        if (r13_14()) b.BS();
        if (r2004p()) b.BL(), b.BL(), b.BL();
        for (int k = 0; k < 5 + 14; k++) b.BS();   // USERI1-5, SPLINESEGS ... TEXTQLTY
        ltscale = b.BD();
        for (int k = 0; k < 6; k++) b.BD();   // TEXTSIZE ... ANGBASE
        pdsize = b.BD();
        for (int k = 0; k < 1 + 5 + 4 + 3; k++) b.BD();   // PLINEWID, USERR1-5, CHAMFERA-D, FACETRES, CMLSCALE, CELTSCALE
        b.TV();
        for (int k = 0; k < 4; k++) b.BL();   // TDCREATE, TDUPDATE
        if (r2004p()) b.BL(), b.BL(), b.BL();
        for (int k = 0; k < 4; k++) b.BL();   // TDINDWG, TDUSRTIMER
        cmc(b);
        b.H(0);   // HANDSEED
        b.H(0), b.H(0), b.H(0);
        if (r2007p()) b.H(0);
        b.H(0), b.H(0);
        if (r2000p()) b.BD();
        b.BD3(), b.BD3(), b.BD3(), b.RD2(), b.RD2(), b.BD();   // the paper space's
        b.BD3(), b.BD3(), b.BD3(), b.H(0);
        if (r2000p()) {
            b.H(0), b.BS(), b.H(0);
            for (int k = 0; k < 6; k++) b.BD3();
        }
        b.BD3();
        extmin = b.BD3();
        extmax = b.BD3();
        b.RD2(), b.RD2(), b.BD();
        b.BD3(), b.BD3(), b.BD3(), b.H(0);
        if (r2000p()) {
            b.H(0), b.BS(), b.H(0);
            for (int k = 0; k < 6; k++) b.BD3();
            b.TV(), b.TV();
        }
        if (r13_14()) {
            for (int k = 0; k < 11; k++) b.B();
            b.RC(), b.RC(), b.B(), b.B(), b.RC(), b.RC(), b.RC(), b.B();
            for (int k = 0; k < 4; k++) b.RC();
            for (int k = 0; k < 6; k++) b.BS();
            b.H(0);
        }
        double sc = b.BD(), asz = b.BD();
        if (b.bad) return;
        dimscale = sc;
        dimasz = asz;
    }

    // a colour (CMC): R2004+ with its true colour (0x1000000 set) and a book's names
    int cmc(Bits &b, uint32_t *rgb = nullptr) {
        int c = b.BS();
        if (!r2004p()) return c;
        uint32_t v = (uint32_t)b.BL();
        uint8_t f = b.RC();
        if (f & 1) b.TV();
        if (f & 2) b.TV();
        if (v == 0xc0000000u) return 256;
        if ((v >> 24) == 0xc3) return v & 0xff;   // (an index)
        if ((v >> 24) == 0xc2 && rgb) *rgb = 0x1000000 | (v & 0xffffff);
        return c;
    }

    void readClasses() {
        Bits b;
        if (!section(1, b)) return;
        size_t stop = b.end / 8;
        while (!b.bad && (b.pos + 7) / 8 < stop) {
            int num = b.BS();
            b.BS();
            b.TV(), b.TV();
            std::string dxf = b.TV();
            b.B();
            int item = b.BS();
            if (r2004p()) b.BL(), b.BL(), b.BL(), b.BL(), b.BL();
            if (b.bad) break;
            classes[num] = item == 0x1f2 ? T_OTHER_ENTITY : T_OTHER_OBJECT;
            static const struct { const char *name; int type; } known[] = {
                {"IMAGE", T_IMAGE}, {"WIPEOUT", T_WIPEOUT}, {"LAYOUT", T_LAYOUT}, {"ARC_DIMENSION", T_ARC_DIMENSION},
                {"LARGE_RADIAL_DIMENSION", T_LARGE_RADIAL_DIMENSION}, {"ACIDBLOCKREFERENCE", T_ACIDBLOCKREFERENCE},
                {"LWPOLYLINE", T_LWPOLYLINE}, {"HATCH", T_HATCH}};   // (the last two: classes in R14)
            for (const auto &k : known)
                if (dxf == k.name) classes[num] = k.type;
        }
    }

    // the object map: sections of (handle, offset) deltas, the size big-endian (not more than
    // 2032 bytes, as a rule)
    void readMap() {
        size_t at = loc[2].seeker;
        while (at + 2 <= len) {
            size_t size = (size_t)d[at] << 8 | d[at + 1];
            if (size <= 2) break;
            size_t from = at + 2, stop = std::min(len, from + size - 2);
            Bits b(d, len, from);   // (the last pair may end past it)
            uint64_t h = 0;
            int64_t off = 0;
            while (b.pos / 8 < stop && !b.bad) {
                uint64_t dh = b.MC();
                int64_t doff = b.MCs();
                if (b.bad) break;
                h += dh;
                off += doff;
                if (dh && off > 0 && (uint64_t)off < len) map.push_back({h, (uint64_t)off});
            }
            at = b.pos / 8 + 2;
        }
        std::stable_sort(map.begin(), map.end(), [](const auto &x, const auto &y) { return x.first < y.first; });   // (sorted already, as a rule)
        map.erase(std::unique(map.begin(), map.end(), [](const auto &x, const auto &y) { return x.first == y.first; }), map.end());
        written.assign(map.size(), 0);
    }
    size_t hint = 0;   // (the last one found: entities follow each other, as a rule)
    size_t find(uint64_t handle) {   // its index in map, or map.size()
        if (hint < map.size() && map[hint].first == handle) return hint;
        if (hint + 1 < map.size() && map[hint + 1].first == handle) return ++hint;
        auto it = std::lower_bound(map.begin(), map.end(), handle, [](const auto &x, uint64_t h) { return x.first < h; });
        if (it == map.end() || it->first != handle) return map.size();
        return hint = (size_t)(it - map.begin());
    }
    bool fresh(uint64_t handle) {   // (an entity not written yet: now it is)
        size_t i = find(handle);
        if (i == map.size() || written[i]) return false;
        return written[i] = 1;
    }

    // --- objects

    bool open(uint64_t handle, Obj &o) {
        size_t i = find(handle);
        if (i == map.size()) return false;
        Bits s(d, len, (size_t)map[i].second);
        uint32_t size = s.MS();
        size_t from = s.pos / 8;
        if (s.bad || !size || size > len - from) return false;
        o.b = Bits(d, from + size, from);
        o.h = o.b;
        o.start = o.b.pos;
        o.handle = handle;
        o.eed.clear();
        o.type = o.b.BS();
        if (o.type == 0x1f2) o.type = T_OTHER_ENTITY;   // (ACAD_PROXY_ENTITY)
        else if (o.type >= 500) {
            auto c = classes.find(o.type);
            o.type = c == classes.end() ? T_OTHER_OBJECT : c->second;
        }
        return !o.b.bad;
    }
    void handles_at(Obj &o, uint32_t bits) { o.h.seek(o.start + bits); }

    void eed(Obj &o) {
        Bits &b = o.b;
        for (int guard = 0; guard < 100000; guard++) {
            int size = b.BS() & 0xffff;
            if (!size || b.bad) return;
            uint64_t app = b.H(0);
            size_t stop = b.pos + (size_t)size * 8;
            if (stop > b.end) {
                b.bad = true;
                return;
            }
            while (b.pos + 8 <= stop && !b.bad) {
                Eed e{app, 1000 + b.RC(), {}, {}, 0, 0};
                switch (e.code) {
                case 1000:
                case 1001: {
                    int n = b.RC();
                    b.RS();
                    for (int k = 0; k < n && b.pos + 8 <= stop; k++)
                        if (char c = (char)b.RC()) e.s.push_back(c);
                    e.code = 1000;
                    break;
                }
                case 1002: e.i = b.RC(); break;
                case 1003:
                case 1005:
                    for (int k = 0; k < 8; k++) e.i = e.i << 8 | b.RC();
                    break;
                case 1004: b.skip((size_t)b.RC() * 8); continue;
                case 1010:
                case 1011:
                case 1012:
                case 1013: e.p = b.RD3(); break;
                case 1040:
                case 1041:
                case 1042: e.r = b.RD(); break;
                case 1070: e.i = (int16_t)b.RS(); break;
                case 1071: e.i = (int32_t)b.RL(); break;
                default: b.pos = stop; continue;
                }
                if (o.eed.size() < 100000) o.eed.push_back(std::move(e));
            }
            b.pos = stop;
        }
    }
    void put_eed(const std::vector<Eed> &v) {
        uint64_t app = ~0ull;
        for (const Eed &e : v) {
            if (e.app != app) {
                app = e.app;
                str(1001, name(app));
            }
            switch (e.code) {
            case 1000: str(1000, e.s); break;
            case 1002: put(1002, e.i ? "}" : "{"); break;
            case 1003: str(1003, name((uint64_t)e.i)); break;
            case 1005: hex(1005, (uint64_t)e.i); break;
            case 1010:
            case 1011:
            case 1012:
            case 1013: pt(e.code, e.p); break;
            case 1040:
            case 1041:
            case 1042: real(e.code, e.r); break;
            default: inum(e.code, e.i); break;
            }
        }
    }

    // a non-entity's common data, up to its own
    bool objHead(Obj &o) {
        if (r2000p()) handles_at(o, o.b.RL());
        o.b.H(0);
        eed(o);
        if (r13_14()) handles_at(o, o.b.RL());
        o.h.H(o.handle);   // the owner
        int n = o.b.BL();
        if (n < 0 || (size_t)n > o.h.left() / 8) return false;
        for (int k = 0; k < n; k++) o.h.H(o.handle);
        bool noXdic = r2004p() && o.b.B();
        if (!noXdic) o.h.H(o.handle);
        if (r2013p()) o.b.B();
        return !o.b.bad && !o.h.bad;
    }
    void xrefBits(Obj &o) {
        o.b.B();
        o.b.BS();
        o.b.B();
    }

    // an entity's common data, up to its own
    bool entHead(Obj &o, Ent &e) {
        Bits &b = o.b, &h = o.h;
        if (r2000p()) handles_at(o, b.RL());
        b.H(0);
        eed(o);
        if (b.B()) b.skip((size_t)b.RL() * 8);   // (proxy graphics)
        if (r13_14()) handles_at(o, b.RL());
        if (b.BB() == 0) e.owner = h.H(o.handle);
        int n = b.BL();
        if (n < 0 || (size_t)n > h.left() / 8) return false;
        for (int k = 0; k < n; k++) h.H(o.handle);
        bool noXdic = r2004p() && b.B();
        if (!noXdic) h.H(o.handle);
        if (r13_14()) {
            e.layer = h.H(o.handle);
            if (!b.B()) e.ltype = h.H(o.handle), e.ltflags = 3;
        }
        e.next = o.handle + 1;
        if (!r2004p() && !b.B()) {
            h.H(o.handle);
            e.next = h.H(o.handle);
        }
        e.color = b.BS();
        e.ltscale = b.BD();
        if (r13_14()) {
            e.invisible = b.BS() & 1;
            return !b.bad && !h.bad;
        }
        e.layer = h.H(o.handle);
        e.ltflags = (int)b.BB();
        if (e.ltflags == 3) e.ltype = h.H(o.handle);
        if (b.BB() == 3) h.H(o.handle);   // (plot style)
        e.invisible = b.BS() & 1;
        b.RC();   // (line weight)
        return !b.bad && !h.bad;
    }

    // --- tables, block headers, layouts

    void readTables() {
        Obj o;
        for (const auto &e : map) {
            if (!open(e.first, o)) continue;
            switch (o.type) {
            case T_LAYER: layer(o); break;
            case T_LTYPE: ltype(o); break;
            case T_STYLE: style(o); break;
            case T_DIMSTYLE: dimstyle(o); break;
            case T_BLOCK_HEADER: blockHeader(o); break;
            case T_APPID:
                if (objHead(o)) names[o.handle] = o.b.TV();
                break;
            case T_LAYOUT: layout(o); break;
            default: break;
            }
        }
        // block names: anonymous ones (*D, *U ...) made unique, as their BLOCKs have them
        std::unordered_set<std::string> used;
        for (BlockH &bh : blocks) {
            std::string n = bh.name;
            Obj b;
            Ent e;
            if (bh.block && open(bh.block, b) && b.type == T_BLOCK && entHead(b, e)) {
                std::string bn = b.b.TV();
                if (!b.b.bad && bn.size() > n.size() && bn.compare(0, n.size(), n) == 0) n = bn;
            }
            std::string u = n;
            for (char &c : u)
                if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (u.empty() || !used.insert(u).second) {
                char sfx[24];
                snprintf(sfx, sizeof sfx, "|%llX", (unsigned long long)bh.handle);
                n += sfx;
                used.insert(n);
            }
            bh.name = n;
            names[bh.handle] = n;
        }
    }

    void layer(Obj &o) {
        if (!objHead(o)) return;
        Layer l;
        l.handle = o.handle;
        l.name = o.b.TV();
        xrefBits(o);
        if (r13_14()) {   // (off: R13 - R14 by a negative colour alone; their "on" bit disagrees)
            l.frozen = o.b.B();
            o.b.B(), o.b.B(), o.b.B();
        } else {
            int v = o.b.BS();
            l.frozen = v & 1;
            l.off = v & 2;
        }
        l.color = cmc(o.b, &l.rgb);
        if (l.color < 0) l.off = true, l.color = -l.color;
        o.h.H(o.handle);
        if (r2000p()) o.h.H(o.handle);
        if (r2007p()) o.h.H(o.handle);
        l.ltype = o.h.H(o.handle);
        if (o.b.bad) return;
        names[l.handle] = l.name;
        layers.push_back(std::move(l));
    }

    void ltype(Obj &o) {
        if (!objHead(o)) return;
        LType l;
        l.handle = o.handle;
        l.name = o.b.TV();
        xrefBits(o);
        o.b.TV();
        o.b.BD();
        o.b.RC();
        int n = o.b.RC();
        for (int k = 0; k < n && !o.b.bad; k++) {
            l.dashes.push_back(o.b.BD());
            o.b.BS();
            o.b.RD(), o.b.RD(), o.b.BD(), o.b.BD();
            o.b.BS();
        }
        if (o.b.bad) return;
        names[l.handle] = l.name;
        ltypes.push_back(std::move(l));
    }

    void style(Obj &o) {
        if (!objHead(o)) return;
        Style s;
        s.handle = o.handle;
        s.name = o.b.TV();
        xrefBits(o);
        s.shape = o.b.B();
        o.b.B();
        s.height = o.b.BD();
        s.width = o.b.BD();
        s.oblique = o.b.BD() * DEG;
        o.b.RC();
        o.b.BD();
        s.font = o.b.TV();
        if (o.b.bad) return;
        s.eed = std::move(o.eed);
        names[s.handle] = s.name;
        styles.push_back(std::move(s));
    }

    void dimstyle(Obj &o) {
        if (!objHead(o)) return;
        DimStyle s;
        s.handle = o.handle;
        s.name = o.b.TV();
        xrefBits(o);
        Bits &b = o.b;
        if (r13_14()) {
            for (int k = 0; k < 11; k++) b.B();
            b.RC(), b.RC(), b.B(), b.B(), b.RC(), b.RC(), b.RC(), b.B();
            for (int k = 0; k < 4; k++) b.RC();
            for (int k = 0; k < 6; k++) b.BS();
        } else
            b.TV(), b.TV();
        s.scale = b.BD();
        s.asz = b.BD();
        if (r2000p()) {   // (the handles: an xref block, the text style, DIMLDRBLK)
            o.h.H(o.handle), o.h.H(o.handle);
            s.ldrblk = o.h.H(o.handle);
        }
        if (b.bad) return;
        names[s.handle] = s.name;
        dimstyles.push_back(std::move(s));
    }

    void blockHeader(Obj &o) {
        if (!objHead(o)) return;
        Bits &b = o.b;
        BlockH bh;
        bh.handle = o.handle;
        bh.name = b.TV();
        xrefBits(o);
        bh.anon = b.B();
        b.B();
        bh.xref = b.B();
        bool overlay = b.B();
        if (r2000p()) b.B();
        int owned = 0;
        if (r2004p() && !bh.xref && !overlay) owned = b.BL();
        bh.base = b.BD3();
        b.TV();
        if (r2000p()) {
            while (b.RC() && !b.bad) {}   // (the inserts' count)
            b.TV();
            b.skip((size_t)std::max(0, b.BL()) * 8);
        }
        if (r2007p()) b.BS(), b.B(), b.RC();
        o.h.H(o.handle);
        bh.block = o.h.H(o.handle);
        if (!r2004p() && !bh.xref && !overlay) {
            bh.first = o.h.H(o.handle);
            bh.last = o.h.H(o.handle);
        }
        if (owned < 0 || (size_t)owned > o.h.left() / 8) return;
        for (int k = 0; k < owned; k++) bh.owned.push_back(o.h.H(o.handle));
        if (b.bad || o.h.bad) return;
        blocks.push_back(std::move(bh));
    }

    void layout(Obj &o) {
        if (!objHead(o)) return;
        Bits &b = o.b;
        b.TV(), b.TV();   // the plot settings
        b.BS();
        for (int k = 0; k < 6; k++) b.BD();
        b.TV();
        b.BD(), b.BD();
        b.BS(), b.BS(), b.BS();
        for (int k = 0; k < 4; k++) b.BD();
        if (!r2004p()) b.TV();
        b.BD(), b.BD();
        b.TV();
        b.BS();
        b.BD();
        b.BD2();
        if (r2004p()) {
            b.BS(), b.BS(), b.BS();
            o.h.H(o.handle);
        }
        if (r2007p()) o.h.H(o.handle);
        b.TV();   // the layout's own
        Layout l;
        l.tab = b.BL();
        l.block = o.h.H(o.handle);
        if (b.bad || o.h.bad) return;
        layouts.push_back(l);
    }

    // --- entities

    // the entities of a block: R13 - R2000 from the first one along their next links
    void chain(uint64_t first, uint64_t last) {
        uint64_t h = first;
        for (size_t n = 0; h && n <= map.size(); n++) {
            if (!fresh(h)) break;
            uint64_t next = 0;
            entity(h, &next);
            if (h == last) break;
            h = next;
        }
    }
    void blockEntities(const BlockH &bh) {
        if (r2004p()) {
            for (uint64_t h : bh.owned)
                if (fresh(h)) entity(h, nullptr);
        } else if (bh.first)
            chain(bh.first, bh.last);
    }

    void common(const char *type, const Obj &o, const Ent &e) {
        rec(type);
        hex(5, o.handle);
        str(8, name(e.layer));
        if (e.ltflags == 1) put(6, "BYBLOCK");
        else if (e.ltflags == 2) put(6, "CONTINUOUS");
        else if (e.ltflags == 3 && e.ltype) str(6, name(e.ltype));
        if (e.rgb) inum(420, e.rgb & 0xffffff);
        if (e.color != 256) inum(62, e.color);
        if (e.ltscale != 1) real(48, e.ltscale);
        if (e.invisible) inum(60, 1);
    }

    void entity(uint64_t handle, uint64_t *next) {
        Obj o;
        Ent e;
        if (!open(handle, o)) return;
        int t = o.type;
        bool ent = (t >= T_TEXT && t <= T_LEADER + 2 && t != 0x2a) || t == 0x4a || t == T_LWPOLYLINE || t == T_HATCH ||
                   (t >= T_IMAGE && t <= T_OTHER_ENTITY);
        if (!ent || t == T_BLOCK || t == T_ENDBLK) return;
        bool drawn = (t >= T_TEXT && t <= T_SPLINE && t != T_SHAPE) || t == T_RAY || t == T_XLINE || t == T_MTEXT || t == T_LEADER ||
                     t == T_LWPOLYLINE || t == T_HATCH || t == T_IMAGE || t == T_WIPEOUT || t == T_ARC_DIMENSION ||
                     t == T_LARGE_RADIAL_DIMENSION || t == T_ACIDBLOCKREFERENCE;
        if (!entHead(o, e)) return;
        if (next) *next = e.next;
        if (!drawn) return;
        size_t m = mark();
        Bits &b = o.b;
        switch (t) {
        case T_TEXT:
        case T_ATTRIB:
        case T_ATTDEF: text(o, e); break;
        case T_INSERT:
        case T_MINSERT:
        case T_ACIDBLOCKREFERENCE: insert(o, e); return;   // (these two: read in full before written, then their attributes / vertices)
        case T_VERTEX_2D:
        case T_VERTEX_3D:
        case T_VERTEX_MESH:
        case T_VERTEX_PFACE:
        case T_VERTEX_PFACE_FACE: vertex(o, e); break;
        case T_POLYLINE_2D:
        case T_POLYLINE_3D:
        case T_POLYLINE_PFACE:
        case T_POLYLINE_MESH: polyline(o, e); return;
        case T_ARC:
        case T_CIRCLE: {
            P3 c = b.BD3();
            double r = b.BD();
            thickness(b);
            common(t == T_ARC ? "ARC" : "CIRCLE", o, e);
            pt(10, c);
            real(40, r);
            P3 n = extrusion(b);
            pt(210, n);
            if (t == T_ARC) {
                real(50, b.BD() * DEG);
                real(51, b.BD() * DEG);
            }
            break;
        }
        case T_LINE: {
            P3 a, c;
            if (r13_14()) {
                a = b.BD3();
                c = b.BD3();
            } else {
                bool flat = b.B();
                a.x = b.RD();
                c.x = b.DD(a.x);
                a.y = b.RD();
                c.y = b.DD(a.y);
                if (!flat) {
                    a.z = b.RD();
                    c.z = b.DD(a.z);
                }
            }
            thickness(b);
            common("LINE", o, e);
            pt(10, a);
            pt(11, c);
            pt(210, extrusion(b));
            break;
        }
        case T_POINT: {
            P3 p = b.BD3();
            thickness(b);
            P3 n = extrusion(b);
            common("POINT", o, e);
            pt(10, p);
            pt(210, n);
            real(50, b.BD() * DEG);
            break;
        }
        case T_3DFACE: {
            P3 v[4];
            int flags = 0;
            if (r13_14()) {
                for (P3 &p : v) p = b.BD3();
                flags = b.BS();
            } else {
                bool noFlags = b.B(), flat = b.B();
                v[0].x = b.RD();
                v[0].y = b.RD();
                if (!flat) v[0].z = b.RD();
                for (int k = 1; k < 4; k++) v[k] = b.DD3(v[k - 1]);
                if (!noFlags) flags = b.BS();
            }
            common("3DFACE", o, e);
            for (int k = 0; k < 4; k++) pt(10 + k, v[k]);
            inum(70, flags);
            break;
        }
        case T_SOLID:
        case T_TRACE: {
            thickness(b);
            double z = b.BD();
            P3 v[4];
            for (P3 &p : v) p = b.RD2(), p.z = z;
            common(t == T_SOLID ? "SOLID" : "TRACE", o, e);
            for (int k = 0; k < 4; k++) pt(10 + k, v[k]);
            pt(210, extrusion(b));
            break;
        }
        case T_ELLIPSE: {
            P3 c = b.BD3(), mj = b.BD3(), n = b.BD3();
            double ratio = b.BD(), t0 = b.BD(), t1 = b.BD();
            common("ELLIPSE", o, e);
            pt(10, c);
            pt(11, mj);
            pt(210, n);
            real(40, ratio);
            real(41, t0);
            real(42, t1);
            break;
        }
        case T_SPLINE: spline(o, e); break;
        case T_RAY:
        case T_XLINE: {
            P3 p = b.BD3(), v = b.BD3();
            common(t == T_RAY ? "RAY" : "XLINE", o, e);
            pt(10, p);
            pt(11, v);
            break;
        }
        case T_DIM_ORDINATE:
        case T_DIM_LINEAR:
        case T_DIM_ALIGNED:
        case T_DIM_ANG3PT:
        case T_DIM_ANG2LN:
        case T_DIM_RADIUS:
        case T_DIM_DIAMETER:
        case T_ARC_DIMENSION:
        case T_LARGE_RADIAL_DIMENSION: dimension(o, e); break;
        case T_VIEWPORT: viewport(o, e); break;
        case T_MTEXT: mtext(o, e); break;
        case T_LEADER: leader(o, e); break;
        case T_LWPOLYLINE: lwpolyline(o, e); break;
        case T_HATCH: hatch(o, e); break;
        case T_IMAGE:
        case T_WIPEOUT: image(o, e); break;
        default: break;
        }
        if (o.b.bad || o.h.bad) rollback(m);
        else put_eed(o.eed);
    }

    void thickness(Bits &b) {   // (R2000+: a bit, 1 for none)
        if (r13_14() || !b.B()) b.BD();
    }
    P3 extrusion(Bits &b) {   // (R2000+: a bit, 1 for 0, 0, 1)
        if (!r13_14() && b.B()) return {0, 0, 1};
        return b.BD3();
    }

    void text(Obj &o, const Ent &e) {
        Bits &b = o.b;
        int t = o.type;
        P3 at, al;
        bool hasAl = true;
        double obl = 0, rot = 0, h = 0, w = 1;
        int gen = 0, hj = 0, vj = 0;
        std::string value;
        P3 n{0, 0, 1};
        if (r13_14()) {
            double z = b.BD();
            at = b.RD2(), at.z = z;
            al = b.RD2(), al.z = z;
            n = b.BD3();
            b.BD();
            obl = b.BD(), rot = b.BD(), h = b.BD(), w = b.BD();
            value = b.TV();
            gen = b.BS(), hj = b.BS(), vj = b.BS();
        } else {
            uint8_t f = b.RC();
            double z = f & 1 ? 0 : b.RD();
            at = b.RD2(), at.z = z;
            if (!(f & 2)) al = b.DD2(at), al.z = z;
            else hasAl = false;
            n = extrusion(b);
            thickness(b);
            if (!(f & 4)) obl = b.RD();
            if (!(f & 8)) rot = b.RD();
            h = b.RD();
            if (!(f & 16)) w = b.RD();
            value = b.TV();
            if (!(f & 32)) gen = b.BS();
            if (!(f & 64)) hj = b.BS();
            if (!(f & 128)) vj = b.BS();
        }
        uint64_t st = o.h.H(o.handle);
        std::string tag, prompt;
        int flags = 0;
        if (t != T_TEXT) {
            if (r2010p()) b.RC();
            tag = b.TV();
            b.BS();
            flags = b.RC();
            if (r2007p()) b.B();
            if (t == T_ATTDEF) {
                if (r2010p()) b.RC();
                prompt = b.TV();
            }
        }
        common(t == T_TEXT ? "TEXT" : t == T_ATTRIB ? "ATTRIB" : "ATTDEF", o, e);
        pt(10, at);
        if (hasAl) pt(11, al);
        pt(210, n);
        real(40, h);
        real(41, w);
        real(50, rot * DEG);
        real(51, obl * DEG);
        str(7, name(st));
        inum(71, gen);
        inum(72, hj);
        inum(t == T_TEXT ? 73 : 74, vj);
        str(1, value);
        if (t != T_TEXT) {
            str(2, tag);
            inum(70, flags);
            if (t == T_ATTDEF) str(3, prompt);
        }
    }

    void mtext(Obj &o, const Ent &e) {
        Bits &b = o.b;
        P3 at = b.BD3(), n = b.BD3(), x = b.BD3();
        double w = b.BD();
        if (r2007p()) b.BD();
        double h = b.BD();
        int attach = b.BS(), dir = b.BS();
        b.BD(), b.BD();
        std::string value;   // (a raw line feed: a paragraph, as \P in a DXF)
        for (char c : b.TV())
            if (c == '\n') value += "\\P";
            else if (c != '\r') value += c;
        uint64_t st = o.h.H(o.handle);
        double spacing = 1;
        int sstyle = 1;
        if (r2000p()) {
            sstyle = b.BS();
            spacing = b.BD();
            b.B();
        }
        common("MTEXT", o, e);
        pt(10, at);
        pt(210, n);
        pt(11, x);
        real(40, h);
        real(41, w);
        inum(71, attach);
        inum(72, dir);
        str(1, value);
        str(7, name(st));
        inum(73, sstyle);
        real(44, spacing);
    }

    void insert(Obj &o, const Ent &e) {
        Bits &b = o.b;
        P3 at = b.BD3(), sc{1, 1, 1};
        if (r13_14()) sc = b.BD3();
        else
            switch (b.BB()) {
            case 0:
                sc.x = b.RD();
                sc.y = b.DD(sc.x);
                sc.z = b.DD(sc.x);
                break;
            case 1:
                sc.y = b.DD(1);
                sc.z = b.DD(1);
                break;
            case 2: sc.x = sc.y = sc.z = b.RD(); break;
            default: break;
            }
        double rot = b.BD();
        P3 n = b.BD3();
        bool atts = b.B();
        int owned = r2004p() && atts ? b.BL() : 0;
        int cols = 1, rows = 1;
        double cs = 0, rs = 0;
        if (o.type == T_MINSERT) {
            cols = b.BS(), rows = b.BS();
            cs = b.BD(), rs = b.BD();
        }
        uint64_t blk = o.h.H(o.handle), first = 0, last = 0;
        std::vector<uint64_t> list;
        if (atts) {
            if (!r2004p()) {
                first = o.h.H(o.handle);
                last = o.h.H(o.handle);
            } else if (owned > 0 && (size_t)owned <= o.h.left() / 8)
                for (int k = 0; k < owned; k++) list.push_back(o.h.H(o.handle));
        }
        if (b.bad || o.h.bad) return;
        common("INSERT", o, e);
        str(2, name(blk));
        pt(10, at);
        real(41, sc.x);
        real(42, sc.y);
        real(43, sc.z);
        real(50, rot * DEG);
        pt(210, n);
        if (o.type == T_MINSERT) {
            inum(70, cols);
            inum(71, rows);
            real(44, cs);
            real(45, rs);
        }
        if (atts) inum(66, 1);
        put_eed(o.eed);
        if (!atts) return;
        if (r2004p()) {
            for (uint64_t h : list)
                if (fresh(h)) entity(h, nullptr);
        } else if (first)
            chain(first, last);
        rec("SEQEND");
    }

    void polyline(Obj &o, const Ent &e) {
        Bits &b = o.b;
        int t = o.type, flags = 0, mm = 0, nn = 0;
        double w0 = 0, w1 = 0, z = 0;
        P3 n{0, 0, 1};
        if (t == T_POLYLINE_2D) {
            flags = b.BS();
            b.BS();
            w0 = b.BD(), w1 = b.BD();
            thickness(b);
            z = b.BD();
            n = extrusion(b);
        } else if (t == T_POLYLINE_3D) {
            uint8_t sp = b.RC();
            flags = 8 | (b.RC() & 1) | (sp & 3 ? 4 : 0);
        } else if (t == T_POLYLINE_PFACE) {
            mm = b.BS(), nn = b.BS();
            flags = 64;
        } else {
            flags = b.BS() | 16;
            b.BS();
            mm = b.BS(), nn = b.BS();
            b.BS(), b.BS();
        }
        int owned = r2004p() ? b.BL() : 0;
        uint64_t first = 0, last = 0;
        std::vector<uint64_t> list;
        if (r2004p()) {
            if (owned < 0 || (size_t)owned > o.h.left() / 8) return;
            for (int k = 0; k < owned; k++) list.push_back(o.h.H(o.handle));
        } else {
            first = o.h.H(o.handle);
            last = o.h.H(o.handle);
        }
        if (b.bad || o.h.bad) return;
        common("POLYLINE", o, e);
        inum(66, 1);
        pt(10, {0, 0, z});
        inum(70, flags);
        if (t == T_POLYLINE_2D) {
            real(40, w0);
            real(41, w1);
            pt(210, n);
        }
        if (mm || nn) {
            inum(71, mm);
            inum(72, nn);
        }
        put_eed(o.eed);
        if (r2004p()) {
            for (uint64_t h : list)
                if (fresh(h)) entity(h, nullptr);
        } else if (first)
            chain(first, last);
        rec("SEQEND");
    }

    void vertex(Obj &o, const Ent &e) {
        Bits &b = o.b;
        int t = o.type;
        if (t == T_VERTEX_PFACE_FACE) {
            int idx[4];
            for (int &x : idx) x = b.BS();
            common("VERTEX", o, e);
            pt(10, {});
            inum(70, 128);
            for (int k = 0; k < 4; k++) inum(71 + k, idx[k]);
            return;
        }
        int flags = b.RC();
        P3 p = b.BD3();
        if (t != T_VERTEX_2D) {
            common("VERTEX", o, e);
            pt(10, p);
            inum(70, t == T_VERTEX_PFACE ? 192 : t == T_VERTEX_MESH ? 64 : flags | 32);
            return;
        }
        double w0 = b.BD(), w1;
        if (w0 < 0) w1 = w0 = -w0;
        else w1 = b.BD();
        double bulge = b.BD();
        common("VERTEX", o, e);
        pt(10, p);
        real(40, w0);
        real(41, w1);
        real(42, bulge);
        inum(70, flags);
    }

    void lwpolyline(Obj &o, const Ent &e) {
        Bits &b = o.b;
        int flags = b.BS();
        double cw = flags & 4 ? b.BD() : 0, z = flags & 8 ? b.BD() : 0;
        if (flags & 2) b.BD();
        P3 n{0, 0, 1};
        if (flags & 1) n = b.BD3();
        int nv = b.BL(), nb = flags & 16 ? b.BL() : 0, ni = flags & 1024 ? b.BL() : 0, nw = flags & 32 ? b.BL() : 0;
        size_t room = b.left() / 2;
        if (nv < 0 || nb < 0 || ni < 0 || nw < 0 || (size_t)nv > room / 2 || (size_t)nb > room || (size_t)ni > room || (size_t)nw > room) {
            b.bad = true;
            return;
        }
        std::vector<P3> v((size_t)nv);
        std::vector<double> bulge((size_t)nv), wa((size_t)nv, cw), wb((size_t)nv, cw);
        for (int k = 0; k < nv; k++) v[k] = r13_14() || k == 0 ? b.RD2() : b.DD2(v[k - 1]);
        for (int k = 0; k < nb; k++) {
            double x = b.BD();
            if (k < nv) bulge[k] = x;
        }
        for (int k = 0; k < ni; k++) b.BL();
        for (int k = 0; k < nw; k++) {
            double x = b.BD(), y = b.BD();
            if (k < nv) wa[k] = x, wb[k] = y;
        }
        common("LWPOLYLINE", o, e);
        inum(90, nv);
        inum(70, (flags & 512 ? 1 : 0) | (flags & 256 ? 128 : 0));
        real(43, cw);
        real(38, z);
        pt(210, n);
        for (int k = 0; k < nv; k++) {
            pt(10, v[k], false);
            if (nw) {
                real(40, wa[k]);
                real(41, wb[k]);
            }
            if (nb) real(42, bulge[k]);
        }
    }

    void spline(Obj &o, const Ent &e) {
        Bits &b = o.b;
        int scenario = b.BL();
        if (r2013p()) {
            int f1 = b.BL(), knotParam = b.BL();
            scenario = knotParam == 15 || !(f1 & 1) ? 1 : 2;
        }
        int deg = b.BL(), nk = 0, nc = 0, nf = 0, flags = 0;
        bool weights = false;
        if (scenario == 1) {
            flags |= b.B() ? 4 : 0;
            flags |= b.B() ? 1 : 0;
            flags |= b.B() ? 2 : 0;
            b.BD(), b.BD();
            nk = b.BL(), nc = b.BL();
            weights = b.B();
        } else if (scenario == 2) {
            b.BD();
            b.BD3(), b.BD3();
            nf = b.BL();
        } else {
            b.bad = true;
            return;
        }
        size_t room = b.left() / 2;
        if (nk < 0 || nc < 0 || nf < 0 || (size_t)nk > room || (size_t)nc > room || (size_t)nf > room) {
            b.bad = true;
            return;
        }
        std::vector<double> knots((size_t)nk), w;
        std::vector<P3> ctrl((size_t)nc), fit((size_t)nf);
        for (double &k : knots) k = b.BD();
        for (P3 &c : ctrl) {
            c = b.BD3();
            if (weights) w.push_back(b.BD());
        }
        for (P3 &f : fit) f = b.BD3();
        common("SPLINE", o, e);
        inum(70, flags);
        inum(71, deg);
        for (double k : knots) real(40, k);
        for (size_t k = 0; k < ctrl.size(); k++) {
            pt(10, ctrl[k]);
            if (weights) real(41, w[k]);
        }
        for (const P3 &f : fit) pt(11, f);
    }

    void dimension(Obj &o, const Ent &e) {
        Bits &b = o.b;
        int t = o.type;
        if (r2010p()) b.RC();
        b.BD3();
        P3 mid = b.RD2();
        double z = b.BD();
        b.RC();
        b.TV();
        b.BD(), b.BD();
        b.BD3();
        b.BD();
        if (r2000p()) b.BS(), b.BS(), b.BD(), b.BD();
        if (r2007p()) b.B(), b.B(), b.B();
        P3 ins = b.RD2();
        ins.z = z;
        mid.z = z;
        P3 def;
        switch (t) {
        case T_DIM_ORDINATE: def = b.BD3(), b.BD3(), b.BD3(), b.RC(); break;
        case T_DIM_LINEAR:
        case T_DIM_ALIGNED:
            b.BD3(), b.BD3();
            def = b.BD3();
            b.BD();
            if (t == T_DIM_LINEAR) b.BD();
            break;
        case T_DIM_ANG3PT: def = b.BD3(), b.BD3(), b.BD3(), b.BD3(); break;
        case T_DIM_ANG2LN:
            b.RD2();
            b.BD3(), b.BD3(), b.BD3();
            def = b.BD3();
            break;
        case T_DIM_RADIUS:
        case T_DIM_DIAMETER:
        case T_LARGE_RADIAL_DIMENSION: def = b.BD3(), b.BD3(), b.BD(); break;
        case T_ARC_DIMENSION:
            def = b.BD3();
            b.BD3(), b.BD3(), b.BD3(), b.B(), b.BD(), b.BD(), b.B(), b.BD3(), b.BD3();
            break;
        default: break;
        }
        uint64_t st = o.h.H(o.handle), blk = o.h.H(o.handle);
        common(t == T_ARC_DIMENSION ? "ARC_DIMENSION" : t == T_LARGE_RADIAL_DIMENSION ? "LARGE_RADIAL_DIMENSION" : "DIMENSION", o, e);
        str(2, name(blk));
        str(3, name(st));
        pt(10, def);
        pt(11, mid);
        pt(12, ins);
    }

    void viewport(Obj &o, const Ent &e) {
        Bits &b = o.b;
        P3 c = b.BD3();
        double w = b.BD(), h = b.BD();
        common("VIEWPORT", o, e);
        pt(10, c);
        real(40, w);
        real(41, h);
        if (!r2000p()) return;
        P3 target = b.BD3(), dir = b.BD3();
        double twist = b.BD(), vh = b.BD();
        b.BD(), b.BD(), b.BD(), b.BD();
        P3 vc = b.RD2();
        b.RD2(), b.RD2(), b.RD2();
        b.BS();
        if (r2007p()) b.BS();
        int frozen = b.BL(), status = b.BL();
        pt(12, vc, false);
        pt(16, dir);
        pt(17, target);
        real(45, vh);
        real(51, twist * DEG);
        inum(90, status);
        if (frozen < 0 || (size_t)frozen > o.h.left() / 8) return;
        for (int k = 0; k < frozen; k++) hex(331, o.h.H(o.handle));
    }

    void leader(Obj &o, const Ent &e) {
        Bits &b = o.b;
        b.B();
        b.BS();
        int path = b.BS(), n = b.BL();
        if (n < 0 || (size_t)n > b.left() / 6) {
            b.bad = true;
            return;
        }
        std::vector<P3> v((size_t)n);
        for (P3 &p : v) p = b.BD3();
        b.BD3();
        P3 nrm = b.BD3();
        b.BD3(), b.BD3();
        if (ver >= 1014) b.BD3();
        if (r13_14()) b.BD();
        if (!r2010p()) b.BD(), b.BD();
        b.B();
        bool arrow = b.B();
        o.h.H(o.handle);
        uint64_t ds = o.h.H(o.handle);
        common("LEADER", o, e);
        str(3, name(ds));
        inum(71, arrow);
        inum(72, path);
        inum(76, n);
        for (const P3 &p : v) pt(10, p);
        pt(210, nrm);
    }

    void image(Obj &o, const Ent &e) {
        Bits &b = o.b;
        b.BL();
        P3 at = b.BD3(), u = b.BD3(), v = b.BD3(), size = b.RD2();
        b.BS();
        bool clip = b.B();
        b.RC(), b.RC(), b.RC();
        if (r2010p()) b.B();
        int ct = b.BS(), n = ct == 1 ? 2 : ct == 2 ? b.BL() : 0;
        if (n < 0 || (size_t)n > b.left() / 128) {
            b.bad = true;
            return;
        }
        std::vector<P3> bp((size_t)n);
        for (P3 &p : bp) p = b.RD2();
        common(o.type == T_WIPEOUT ? "WIPEOUT" : "IMAGE", o, e);
        pt(10, at);
        pt(11, u);
        pt(12, v);
        pt(13, size, false);
        if (clip || o.type == T_WIPEOUT)
            for (const P3 &p : bp) pt(14, p, false);
    }

    void hatch(Obj &o, const Ent &e) {
        Bits &b = o.b;
        bool gradient = false;
        uint32_t grgb = 0;
        if (r2004p()) {
            gradient = b.BL() != 0;
            b.BL();
            b.BD(), b.BD();
            b.BL();
            b.BD();
            int nc = b.BL();
            for (int k = 0; k < nc && k < 1000 && !b.bad; k++) {
                b.BD();
                uint32_t rgb = 0;
                cmc(b, &rgb);
                if (k == 0) grgb = rgb;
            }
            b.TV();
        }
        double z = b.BD();
        P3 n = b.BD3();
        std::string pattern = b.TV();
        bool solid = b.B(), assoc = b.B();
        int np = b.BL();
        if (np < 0 || (size_t)np > b.left() / 8) {
            b.bad = true;
            return;
        }
        common("HATCH", o, e);
        pt(10, {0, 0, z});
        pt(210, n);
        str(2, pattern);
        inum(70, solid);
        inum(71, assoc);
        inum(91, np);
        for (int p = 0; p < np && !b.bad; p++) {
            int flags = b.BL();
            inum(92, flags);
            if (!(flags & 2)) {
                int ne = b.BL();
                if (ne < 0 || (size_t)ne > b.left() / 8) {
                    b.bad = true;
                    return;
                }
                inum(93, ne);
                for (int k = 0; k < ne && !b.bad; k++) {
                    int type = b.RC();
                    inum(72, type);
                    switch (type) {
                    case 1: pt(10, b.RD2(), false), pt(11, b.RD2(), false); break;
                    case 2: {
                        pt(10, b.RD2(), false);
                        real(40, b.BD());
                        real(50, b.BD() * DEG);
                        real(51, b.BD() * DEG);
                        inum(73, b.B());
                        break;
                    }
                    case 3: {
                        pt(10, b.RD2(), false);
                        pt(11, b.RD2(), false);
                        real(40, b.BD());
                        real(50, b.BD() * DEG);
                        real(51, b.BD() * DEG);
                        inum(73, b.B());
                        break;
                    }
                    case 4: {
                        inum(94, b.BL());
                        bool rational = b.B();
                        inum(73, rational);
                        inum(74, b.B());
                        int nk = b.BL(), nc = b.BL();
                        if (nk < 0 || nc < 0 || (size_t)nk > b.left() / 2 || (size_t)nc > b.left() / 2) {
                            b.bad = true;
                            return;
                        }
                        inum(95, nk);
                        inum(96, nc);
                        for (int q = 0; q < nk; q++) real(40, b.BD());
                        for (int q = 0; q < nc; q++) {
                            pt(10, b.RD2(), false);
                            if (rational) real(42, b.BD());
                        }
                        if (r2010p()) {
                            int nf = b.BL();
                            if (nf < 0 || (size_t)nf > b.left() / 128) {
                                b.bad = true;
                                return;
                            }
                            inum(97, nf);
                            for (int q = 0; q < nf; q++) pt(11, b.RD2(), false);
                            if (nf) pt(12, b.RD2(), false), pt(13, b.RD2(), false);
                        }
                        break;
                    }
                    default: b.bad = true; return;
                    }
                }
            } else {
                bool bulges = b.B(), closed = b.B();
                int nv = b.BL();
                if (nv < 0 || (size_t)nv > b.left() / 128) {
                    b.bad = true;
                    return;
                }
                inum(72, bulges);
                inum(73, closed);
                inum(93, nv);
                for (int k = 0; k < nv; k++) {
                    pt(10, b.RD2(), false);
                    if (bulges) real(42, b.BD());
                }
            }
            int nsrc = b.BL();
            if (nsrc < 0 || (size_t)nsrc > o.h.left() / 8) {
                b.bad = true;
                return;
            }
            inum(97, nsrc);
            for (int k = 0; k < nsrc; k++) hex(330, o.h.H(o.handle));
        }
        inum(75, b.BS());
        inum(76, b.BS());
        if (!solid) {
            real(52, b.BD() * DEG);
            real(41, b.BD());
            inum(77, b.B());
            int nl = b.BS();
            if (nl < 0 || nl > 10000) {
                b.bad = true;
                return;
            }
            inum(78, nl);
            for (int k = 0; k < nl && !b.bad; k++) {
                real(53, b.BD() * DEG);
                P3 base = b.BD2(), off = b.BD2();
                real(43, base.x);
                real(44, base.y);
                real(45, off.x);
                real(46, off.y);
                int nd = b.BS();
                if (nd < 0 || nd > 1000) {
                    b.bad = true;
                    return;
                }
                inum(79, nd);
                for (int q = 0; q < nd; q++) real(49, b.BD());
            }
        }
        if (gradient && grgb) {
            inum(450, 1);
            inum(421, grgb & 0xffffff);
        }
    }

    // --- the DXF

    void emit() {
        static const int cps[] = {1252, 1252, 28591, 28592, 28593, 28594, 28595, 28596, 28597, 28598, 28599, 437, 850, 852, 855,
                                  857, 860, 861, 863, 864, 865, 869, 932, 10000, 950, 949, 1361, 866, 1250, 1251, 1252, 936, 1253,
                                  1254, 1255, 1256, 1257, 874, 932, 936, 949, 950, 1361, 1200, 1258};
        char buf[32];
        rec("SECTION");
        put(2, "HEADER");
        put(9, "$ACADVER");
        snprintf(buf, sizeof buf, "AC%d", ver);
        put(1, buf);
        put(9, "$DWGCODEPAGE");
        snprintf(buf, sizeof buf, "ANSI_%d", codepage >= 0 && codepage < (int)(sizeof cps / sizeof *cps) ? cps[codepage] : 1252);
        put(3, buf);
        put(9, "$LTSCALE");
        real(40, ltscale);
        put(9, "$PDMODE");
        inum(70, pdmode);
        put(9, "$PDSIZE");
        real(40, pdsize);
        put(9, "$DIMASZ");
        real(40, dimasz);
        put(9, "$DIMSCALE");
        real(40, dimscale);
        put(9, "$EXTMIN");
        pt(10, extmin);
        put(9, "$EXTMAX");
        pt(10, extmax);
        rec("ENDSEC");

        rec("SECTION");
        put(2, "TABLES");
        for (const LType &l : ltypes) {
            rec("LTYPE");
            hex(5, l.handle);
            str(2, l.name);
            for (double x : l.dashes) real(49, x);
        }
        for (const Layer &l : layers) {
            rec("LAYER");
            hex(5, l.handle);
            str(2, l.name);
            inum(62, l.off ? -l.color : l.color);
            if (l.rgb) inum(420, l.rgb & 0xffffff);
            inum(70, l.frozen);
            str(6, name(l.ltype));
        }
        for (const Style &s : styles) {
            rec("STYLE");
            hex(5, s.handle);
            str(2, s.name);
            inum(70, s.shape);
            real(40, s.height);
            real(41, s.width);
            real(50, s.oblique);
            str(3, s.font);
            put_eed(s.eed);
        }
        for (const DimStyle &s : dimstyles) {
            rec("DIMSTYLE");
            hex(5, s.handle);
            str(2, s.name);
            real(40, s.scale);
            real(41, s.asz);
            if (s.ldrblk) hex(341, s.ldrblk);
        }
        for (const BlockH &bh : blocks) {
            rec("BLOCK_RECORD");
            hex(5, bh.handle);
            str(2, bh.name);
        }
        rec("ENDSEC");

        // the blocks (the paper spaces too: a layout's first viewport is its own view); the
        // model space as the entities
        auto upper = [](std::string s) {
            for (char &c : s)
                if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            return s;
        };
        rec("SECTION");
        put(2, "BLOCKS");
        const BlockH *model = nullptr;
        for (const BlockH &bh : blocks) {
            if (upper(bh.name) == "*MODEL_SPACE") {
                model = &bh;
                continue;
            }
            rec("BLOCK");
            str(2, bh.name);
            inum(70, (bh.anon ? 1 : 0) | (bh.xref ? 4 : 0));
            pt(10, bh.base);
            if (!bh.xref) blockEntities(bh);
            rec("ENDBLK");
        }
        rec("ENDSEC");
        rec("SECTION");
        put(2, "ENTITIES");
        if (model) blockEntities(*model);
        rec("ENDSEC");
        rec("SECTION");
        put(2, "OBJECTS");
        for (const Layout &l : layouts) {
            rec("LAYOUT");
            put(100, "AcDbLayout");
            inum(71, l.tab);
            hex(330, l.block);
        }
        rec("ENDSEC");
        rec("EOF");
    }
};

} // namespace

bool nitrodwg_read(const void *data, size_t len, nitrodwg_pairs &out) {
    const uint8_t *d = static_cast<const uint8_t *>(data);
    if (!d || len < 0x40 || memcmp(d, "AC10", 4) != 0) return false;
    int ver = atoi(std::string(reinterpret_cast<const char *>(d) + 2, 4).c_str());
    if (ver != 1012 && ver != 1014 && ver != 1015) return false;
    Dwg g(d, len, out);
    g.ver = ver;
    if (!g.fileHeader()) return false;
    g.header();
    g.readClasses();
    g.readMap();
    if (g.map.empty()) return false;
    g.readTables();
    out.code.reserve(len / 4);
    out.end.reserve(len / 4);
    out.text.reserve(len * 2);
    g.emit();
    return out.text.size() < 0xf0000000u;
}
