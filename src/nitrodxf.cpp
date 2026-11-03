// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrodxf: AutoCAD DXF drawings, drawn with plutovg. See nitrodxf.h.
//
// The file is read in one pass into tables (layers, line types, text styles) and blocks of
// primitives in their own coordinates: paths of lines and cubic Béziers (arcs, ellipses,
// bulges), text as glyph runs, fills, hatch patterns, block references. Then each page (the model
// space, then every layout's sheet: its paper space over the model space seen through each
// viewport) is flattened: every block reference is expanded with its transform, ByLayer /
// ByBlock colours and line types resolved, into a display list of float paths around the page's
// centre.
// render() replays the items that touch the view; hairlines are one device pixel wide.
// What is 3D is shown from the top (x, y of the world coordinates).
#include "nitrodxf.h"
#include "nitrodwg.h"
#include "nitro_font.h"

#include <plutovg.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>   // MultiByteToWideChar: Windows has no iconv
#else
#include <cerrno>
#include <iconv.h>
#endif
#ifndef M_PI   // (not defined by mingw's math.h)
#define M_PI 3.14159265358979323846
#endif

namespace {

using sv = std::string_view;

// --- geometry ----------------------------------------------------------------

struct V2 {
    double x = 0, y = 0;
};
V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
V2 operator*(V2 a, double k) { return {a.x * k, a.y * k}; }
double len(V2 a) { return std::hypot(a.x, a.y); }
bool finite(V2 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::fabs(a.x) < 1e15 && std::fabs(a.y) < 1e15; }

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 unit(V3 a) {
    double l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return l > 1e-300 ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1};
}

// affine 2D transform (plutovg's order): x' = a x + c y + e, y' = b x + d y + f
struct M2 {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    V2 operator()(V2 p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    V2 vec(V2 p) const { return {a * p.x + c * p.y, b * p.x + d * p.y}; }
};
M2 operator*(const M2 &m, const M2 &n) {   // m after n
    return {m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d,
            m.b * n.c + m.d * n.d, m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f};
}
M2 translate(V2 p) { return {1, 0, 0, 1, p.x, p.y}; }
M2 rotate(double rad) {
    double c = std::cos(rad), s = std::sin(rad);
    return {c, s, -s, c, 0, 0};
}
M2 scaling(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
double det(const M2 &m) { return m.a * m.d - m.b * m.c; }

// AutoCAD's arbitrary axis algorithm: the x and y axes of a plane with normal n (unit)
void plane_axes(V3 n, V3 &ax, V3 &ay) {
    ax = unit(std::fabs(n.x) < 1.0 / 64 && std::fabs(n.y) < 1.0 / 64 ? cross({0, 1, 0}, n) : cross({0, 0, 1}, n));
    ay = unit(cross(n, ax));
}

// The object coordinate system of a planar entity with extrusion n, seen from the top:
// OCS (x, y, elevation) -> WCS (x, y). n = (0, 0, -1) mirrors x.
M2 ocs(V3 n, double elevation = 0) {
    if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)) return M2();
    n = unit(n);
    V3 ax, ay;
    plane_axes(n, ax, ay);
    return {ax.x, ax.y, ay.x, ay.y, elevation * n.x, elevation * n.y};
}

enum : uint8_t { C_MOVE, C_LINE, C_CUBIC, C_CLOSE };

// a path in doubles: commands and their points (move / line: 1, cubic: 3, close: 0)
struct Geo {
    std::vector<uint8_t> cmd;
    std::vector<V2> pt;
    V2 cur, start;
    void move(V2 p) {
        cmd.push_back(C_MOVE);
        pt.push_back(p);
        cur = start = p;
    }
    void line(V2 p) {
        cmd.push_back(C_LINE);
        pt.push_back(p);
        cur = p;
    }
    void cubic(V2 p1, V2 p2, V2 p3) {
        cmd.push_back(C_CUBIC);
        pt.push_back(p1);
        pt.push_back(p2);
        pt.push_back(p3);
        cur = p3;
    }
    void close() {
        cmd.push_back(C_CLOSE);
        cur = start;
    }
};

// Elliptical arc c + u cos t + v sin t for t from t0 to t1 (t1 < t0: clockwise) as cubic
// Béziers, transformed by m; starts with a move (or a line from the current point).
void add_arc(Geo &g, const M2 &m, V2 c, V2 u, V2 v, double t0, double t1, bool move) {
    double sweep = std::max(-2 * M_PI, std::min(2 * M_PI, t1 - t0));
    int n = std::max(1, (int)std::ceil(std::fabs(sweep) / (M_PI / 2) - 1e-9));
    double dt = sweep / n, k = 4.0 / 3 * std::tan(dt / 4);
    auto at = [&](double t) { return c + u * std::cos(t) + v * std::sin(t); };
    auto tangent = [&](double t) { return v * std::cos(t) - u * std::sin(t); };
    V2 p0 = at(t0);
    if (move) g.move(m(p0));
    else g.line(m(p0));
    for (int i = 0; i < n; i++) {
        double ta = t0 + i * dt, tb = ta + dt;
        V2 a = at(ta), b = at(tb);
        g.cubic(m(a + tangent(ta) * k), m(b - tangent(tb) * k), m(b));
    }
}

// the angle t1 as the end of an arc from t0: counter-clockwise (or clockwise: cw) less than a
// full turn away, a full turn if they are the same; NaN if either is not finite
double arc_end(double t0, double t1, bool cw = false) {
    if (!std::isfinite(t0) || !std::isfinite(t1)) return NAN;
    double d = std::fmod(cw ? t0 - t1 : t1 - t0, 2 * M_PI);
    if (d <= 0) d += 2 * M_PI;
    return cw ? t0 - d : t0 + d;
}

// A polyline segment from p to q with bulge b (tan of a quarter of its arc's angle, > 0 ccw).
void add_bulge(Geo &g, const M2 &m, V2 p, V2 q, double b) {
    V2 ch = q - p;
    double d = len(ch);
    if (std::fabs(b) < 1e-9 || d < 1e-12 || !std::isfinite(b) || std::fabs(b) > 1e6) {
        g.line(m(q));
        return;
    }
    V2 mid = (p + q) * 0.5, nrm = {-ch.y / d, ch.x / d};
    V2 c = mid + nrm * (d * (1 - b * b) / (4 * b));
    double r = len(p - c), t0 = std::atan2(p.y - c.y, p.x - c.x);
    add_arc(g, m, c, {r, 0}, {0, r}, t0, t0 + 4 * std::atan(b), false);
}

// --- reading -----------------------------------------------------------------

double pow10i(int e) {
    static const double t[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                               1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    return e >= 0 && e <= 22 ? t[e] : std::pow(10.0, e);
}

// a decimal number (the DXF way: no hex, no inf; a decimal comma too); def if there is none.
// (A DWG's real: '\1' and its 8 bytes, nitrodwg.h.)
double num(sv s, double def = 0) {
    if (s.size() == 9 && s[0] == '\1') {
        double v;
        memcpy(&v, s.data() + 1, 8);
        return std::isfinite(v) ? v : def;
    }
    const char *p = s.data(), *e = p + s.size();
    while (p < e && (*p == ' ' || *p == '\t')) p++;
    bool neg = false;
    if (p < e && (*p == '-' || *p == '+')) neg = *p++ == '-';
    uint64_t m = 0;
    int ex = 0;
    bool any = false;
    for (; p < e && *p >= '0' && *p <= '9'; p++, any = true) {
        if (m < 100000000000000000ull) m = m * 10 + (uint64_t)(*p - '0');
        else ex++;
    }
    if (p < e && (*p == '.' || *p == ',')) {   // (a decimal comma: some programs write the locale's)
        for (p++; p < e && *p >= '0' && *p <= '9'; p++, any = true)
            if (m < 100000000000000000ull) {
                m = m * 10 + (uint64_t)(*p - '0');
                ex--;
            }
    }
    if (!any) return def;
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        bool en = false;
        if (p < e && (*p == '-' || *p == '+')) en = *p++ == '-';
        int x = 0;
        for (; p < e && *p >= '0' && *p <= '9'; p++)
            if (x < 10000) x = x * 10 + (*p - '0');
        ex += en ? -x : x;
    }
    double v = (double)m;
    if (m) v = ex >= 0 ? v * pow10i(ex) : v / pow10i(-ex);
    return neg ? -v : v;
}
int inum(sv s, int def = 0) {
    double v = num(s, def);
    return v > 2e9 ? 2000000000 : v < -2e9 ? -2000000000 : (int)v;
}
// a true colour (420, 421): its low 24 bits (LibreDWG's DXFs keep the DWG's flags in the high byte)
uint32_t truecolor(sv s) {
    double v = num(s);
    return v >= 0 && v < 4294967296.0 ? (uint32_t)v & 0xffffff : 0;
}
sv trim(sv s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
std::string upper(sv s) {
    s = trim(s);
    std::string u(s);
    for (char &ch : u)
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    return u;
}

struct Pair {
    int code;
    sv v;
};

// group code / value line pairs of an ASCII DXF (or a DWG's, from nitrodwg)
struct Reader {
    const char *p, *end;
    const nitrodwg_pairs *dwg = nullptr;
    size_t at = 0;
    Pair cur{-1, {}};
    bool ok = false;
    sv line() {
        const char *s = p;
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *e = nl ? nl : end;
        p = nl ? nl + 1 : end;
        if (e > s && e[-1] == '\r') e--;
        return sv(s, (size_t)(e - s));
    }
    static bool group_code(sv c, int &code) {
        size_t i = c[0] == '-' ? 1 : 0;
        if (i >= c.size()) return false;
        code = 0;
        for (; i < c.size(); i++) {
            if (c[i] < '0' || c[i] > '9' || code > 100000) return false;
            code = code * 10 + (c[i] - '0');
        }
        if (c[0] == '-') code = -code;
        return true;
    }
    static bool type_name(sv v) {   // SECTION, LINE, 3DFACE, ACAD_PROXY_ENTITY ...
        if (v.empty() || v.size() > 40) return false;
        for (char ch : v)
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
        return true;
    }
    // The next pair. A line that is no group code (a damaged file): skipped, up to where a record
    // starts again (a 0 group with a type name).
    bool next() {
        ok = false;
        if (dwg) {
            if (at >= dwg->code.size()) return false;
            uint32_t b = at ? dwg->end[at - 1] : 0;
            cur = {dwg->code[at], sv(dwg->text.data() + b, dwg->end[at] - b)};
            at++;
            return ok = true;
        }
        bool lost = false;
        while (p < end) {
            sv c = trim(line());
            if (c.empty()) continue;   // (blank lines between pairs)
            int code;
            if (!group_code(c, code) || (lost && code != 0)) {
                lost = true;
                continue;
            }
            if (p >= end) return false;
            const char *at = p;
            sv v = line();
            if (lost && !type_name(trim(v))) {
                p = at;
                continue;
            }
            cur = {code, v};
            return ok = true;
        }
        return false;
    }
};

// an entity / table entry: its type (the 0 group) and the groups up to the next 0
struct Rec {
    std::string type;
    std::vector<Pair> g;
    const Pair *find(int code) const {
        for (const Pair &p : g)
            if (p.code == code) return &p;
        return nullptr;
    }
    double d(int code, double def = 0) const {
        const Pair *p = find(code);
        return p ? num(p->v, def) : def;
    }
    int i(int code, int def = 0) const {
        const Pair *p = find(code);
        return p ? inum(p->v, def) : def;
    }
    sv s(int code) const {
        const Pair *p = find(code);
        return p ? p->v : sv();
    }
    V2 p2(int code, V2 def = {}) const { return {d(code, def.x), d(code + 10, def.y)}; }
    V3 p3(int code, V3 def = {}) const { return {d(code, def.x), d(code + 10, def.y), d(code + 20, def.z)}; }
};

// --- text --------------------------------------------------------------------

// The code page of $DWGCODEPAGE ("ANSI_1250", "DOS852", "ANSI_932" ...): its number
int codepage(sv name) {
    std::string u = upper(name);
    static const struct { const char *name; int cp; } names[] = {{"BIG5", 950}, {"GB2312", 936}, {"KSC5601", 949}, {"JOHAB", 1361}};
    for (const auto &e : names)
        if (u.find(e.name) != std::string::npos) return e.cp;
    size_t iso = u.find("8859-");   // ("ISO8859-2": 28592, as Windows numbers it)
    if (iso != std::string::npos && iso + 5 < u.size() && u[iso + 5] >= '1' && u[iso + 5] <= '9') return 28590 + (u[iso + 5] - '0');
    for (size_t i = 0; i < u.size(); i++)
        if (u[i] >= '0' && u[i] <= '9') return atoi(u.c_str() + i);
    return 1252;   // (ANSI, "undefined", ASCII)
}

// Text in a code page -> UTF-8: iconv (as in libcdr), on Windows its own converter. Bytes it
// can't convert are skipped; without a converter for the code page: Latin-1.
struct Conv {
    int cp = 1252;   // the drawing's ($DWGCODEPAGE)
#ifndef _WIN32
    std::unordered_map<int, iconv_t> cds;
    ~Conv() {
        for (auto &e : cds)
            if (e.second != (iconv_t)-1) iconv_close(e.second);
    }
#endif
    void append(std::string &o, sv s, int page) {
        auto latin1 = [&]() {
            for (char ch : s) nf_utf8_put(o, (unsigned char)ch);
        };
#ifdef _WIN32
        int n = MultiByteToWideChar((UINT)page, 0, s.data(), (int)s.size(), nullptr, 0);
        if (n <= 0) return latin1();
        std::vector<wchar_t> w((size_t)n);
        MultiByteToWideChar((UINT)page, 0, s.data(), (int)s.size(), w.data(), n);
        for (size_t i = 0; i < w.size(); i++) {
            uint32_t c = (uint16_t)w[i];
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < w.size() && (uint16_t)w[i + 1] >= 0xdc00 && (uint16_t)w[i + 1] < 0xe000)
                c = (c - 0xd800) * 0x400 + ((uint16_t)w[++i] - 0xdc00) + 0x10000;
            if (c && c != 0xfffd) nf_utf8_put(o, c);
        }
#else
        auto it = cds.find(page);
        if (it == cds.end()) {   // (ISO 8859 and Mac Roman by their names, for macOS's iconv)
            std::string name = page > 28590 && page < 28600 ? "ISO-8859-" + std::to_string(page - 28590) : page == 10000 ? "MACINTOSH" : "CP" + std::to_string(page);
            it = cds.emplace(page, iconv_open("UTF-32LE", name.c_str())).first;
        }
        iconv_t cd = it->second;
        if (cd == (iconv_t)-1) return latin1();
        iconv(cd, nullptr, nullptr, nullptr, nullptr);
        char *src = const_cast<char *>(s.data());
        size_t left = s.size();
        while (left) {
            unsigned char out[4 * 64];
            char *dst = reinterpret_cast<char *>(out);
            size_t room = sizeof out;
            size_t r = iconv(cd, &src, &left, &dst, &room);
            for (size_t k = 0; k + 4 <= sizeof out - room; k += 4) {
                uint32_t c = out[k] | out[k + 1] << 8 | out[k + 2] << 16 | (uint32_t)out[k + 3] << 24;
                if (c && c != 0xfffd && c < 0x110000) nf_utf8_put(o, c);
            }
            if (r == (size_t)-1 && errno != E2BIG) {
                if (errno == EINVAL) break;   // (an incomplete character at the end)
                src++;
                left--;
            }
        }
#endif
    }
};

int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// A DXF string as UTF-8: the drawing's code page before R2007 (UTF-8 from R2007 on), the
// \U+xxxx and \M+nxxxx (a double-byte character of an Asian code page) escapes, and in TEXT
// (special) the %% codes: %%c diameter, %%d degree, %%p plus-minus, %%nnn.
std::string decode(sv raw, Conv &conv, bool utf8, bool special) {
    std::string s;
    bool high = false;
    for (char ch : raw) high |= (unsigned char)ch >= 0x80;
    if (!high) s.assign(raw);
    else if (!utf8) conv.append(s, raw, conv.cp);
    else {   // (invalid UTF-8: as Latin-1)
        std::string t(raw);
        for (size_t i = 0; i < t.size();) nf_utf8_put(s, nf_utf8_next(t, i));
    }
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '\\' && i + 2 < s.size() && (s[i + 1] == 'U' || s[i + 1] == 'u') && s[i + 2] == '+') {
            uint32_t v = 0;
            int n = 0;
            for (; n < 4 && i + 3 + n < s.size() && hexval(s[i + 3 + n]) >= 0; n++) v = v * 16 + (uint32_t)hexval(s[i + 3 + n]);
            if (n == 4) {
                nf_utf8_put(o, v);
                i += 7;
                continue;
            }
        }
        if (c == '\\' && i + 7 < s.size() && (s[i + 1] == 'M' || s[i + 1] == 'm') && s[i + 2] == '+' && s[i + 3] >= '1' && s[i + 3] <= '5') {
            int h[4] = {hexval(s[i + 4]), hexval(s[i + 5]), hexval(s[i + 6]), hexval(s[i + 7])};
            if (h[0] >= 0 && h[1] >= 0 && h[2] >= 0 && h[3] >= 0) {
                static const int pages[] = {932, 950, 949, 1361, 936};
                char b[2] = {(char)(h[0] * 16 + h[1]), (char)(h[2] * 16 + h[3])};
                conv.append(o, sv(b, 2), pages[s[i + 3] - '1']);
                i += 8;
                continue;
            }
        }
        if (special && c == '%' && i + 2 < s.size() && s[i + 1] == '%') {
            char k = s[i + 2];
            if (k >= '0' && k <= '9') {
                uint32_t v = 0;
                size_t j = i + 2;
                for (; j < s.size() && j < i + 5 && s[j] >= '0' && s[j] <= '9'; j++) v = v * 10 + (uint32_t)(s[j] - '0');
                nf_utf8_put(o, v ? v : ' ');
                i = j;
                continue;
            }
            uint32_t v = k == 'c' || k == 'C' ? 0xd8 : k == 'd' || k == 'D' ? 0xb0 : k == 'p' || k == 'P' ? 0xb1 : k == '%' ? '%' : 0;
            if (v || k == 'u' || k == 'U' || k == 'o' || k == 'O' || k == 'k' || k == 'K') {   // (under / overline: left out)
                if (v) nf_utf8_put(o, v);
                i += 3;
                continue;
            }
        }
        o += c;
        i++;
    }
    return o;
}

// --- fonts -------------------------------------------------------------------

// the faces come from nitro_font (shared with nitrosvg and nitrocdr), and are used under its
// lock: plutovg's faces have no lock of their own (glyph caches)

// A text style's font: TrueType by family ("Arial", from the style's extended data) or by file
// name ("arialbd.ttf"); AutoCAD's own SHX stroke fonts (txt, simplex, romans, isocp ...) are
// not here: a plain sans serif instead. Locked by the caller.
plutovg_font_face_t *find_font(sv family, sv file, bool bold, bool italic) {
    std::string fam(trim(family));
    std::string f(trim(file));
    size_t slash = f.find_last_of("/\\");
    if (slash != std::string::npos) f = f.substr(slash + 1);
    std::string lower = f;
    for (char &ch : lower) ch = (char)tolower((unsigned char)ch);
    bool ttf = lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".ttf") == 0 || lower.compare(lower.size() - 4, 4, ".otf") == 0 ||
                                    lower.compare(lower.size() - 4, 4, ".ttc") == 0);
    if (fam.empty() && ttf) {
        static const struct { const char *file, *family; bool bold, italic; } files[] = {
            {"arial", "Arial", 0, 0},          {"arialbd", "Arial", 1, 0},      {"ariali", "Arial", 0, 1},
            {"arialbi", "Arial", 1, 1},        {"arialn", "Arial Narrow", 0, 0}, {"arialnb", "Arial Narrow", 1, 0},
            {"times", "Times New Roman", 0, 0}, {"timesbd", "Times New Roman", 1, 0}, {"timesi", "Times New Roman", 0, 1},
            {"cour", "Courier New", 0, 0},     {"courbd", "Courier New", 1, 0}, {"verdana", "Verdana", 0, 0},
            {"tahoma", "Tahoma", 0, 0},        {"calibri", "Calibri", 0, 0},    {"calibrib", "Calibri", 1, 0},
            {"isocpeur", "ISOCPEUR", 0, 0},    {"isocteur", "ISOCTEUR", 0, 0},  {"swiss", "Swis721 BT", 0, 0},
            {"simplex", "Arial", 0, 0},        {"gothic", "MS Gothic", 0, 0},   {"century", "Century", 0, 0}};
        std::string stem = lower.substr(0, lower.size() - 4);
        for (const auto &e : files)
            if (stem == e.file) {
                fam = e.family;
                bold |= e.bold;
                italic |= e.italic;
            }
        if (fam.empty()) fam = f.substr(0, f.size() - 4);
    }
    const char *tries[] = {fam.c_str(), "Arial", "Helvetica", "Liberation Sans", "DejaVu Sans"};
    for (const char *t : tries) {
        if (!*t) continue;
        plutovg_font_face_t *face = nf_face(t, bold ? 700 : 400, italic);
        if (face) return face;
    }
    return nullptr;
}

// a face for a character the font lacks; null if none has it (locked by the caller)
plutovg_font_face_t *fallback_font(uint32_t cp) {
    static const char *const tries[] = {"Arial", "Helvetica", "Lucida Grande", "Arial Unicode MS", "Liberation Sans", "DejaVu Sans",
                                        "Noto Sans", "PingFang SC", "Hiragino Sans", "MS Gothic", "Noto Sans CJK JP"};
    for (const char *t : tries) {
        plutovg_font_face_t *f = nf_face(t, 400, false);
        if (f && nf_has_glyph(f, cp)) return f;
    }
    return nullptr;
}

struct FaceInfo {
    float cap = 0.7f, descent = 0.21f;   // per em
};

// --- the document ------------------------------------------------------------

// colours of entities / layers: an AutoCAD colour index, a true colour, or ByLayer / ByBlock
enum : uint32_t { COL_ACI = 1u << 24, COL_RGB = 2u << 24, COL_BYLAYER = 3u << 24, COL_BYBLOCK = 4u << 24 };
enum { LT_BYLAYER = -1, LT_BYBLOCK = -2, LT_CONTINUOUS = -3 };

// the backgrounds (nitrodxf_set_background): paper white, AutoCAD's dark model space, black
const uint32_t BACKGROUNDS[3] = {0xffffff, 0x212830, 0x000000};
const uint32_t WIPEOUT = 0x01000000;   // (an item's rgba: the background colour, whichever it is)

uint32_t aci_rgb(int i) {   // the AutoCAD Color Index palette
    static uint32_t t[256];
    static bool init;
    if (!init) {
        static const uint32_t low[10] = {0, 0xff0000, 0xffff00, 0x00ff00, 0x00ffff, 0x0000ff, 0xff00ff, 0xffffff, 0x808080, 0xc0c0c0};
        for (int k = 0; k < 10; k++) t[k] = low[k];
        static const int bright[5] = {255, 165, 127, 76, 38};
        for (int k = 10; k < 250; k++) {   // 24 hues in 15 degree steps, 5 brightnesses, full / half saturation
            int hue = (k - 10) / 10, shade = (k - 10) % 10;
            double vmax = bright[shade / 2], vmin = shade & 1 ? std::floor(vmax / 2) : 0;
            double h = hue * 15.0, rgb[3];
            for (int ch = 0; ch < 3; ch++) {
                double hh = std::fmod(h - ch * 120 + 360, 360);   // (distance from the channel's own hue)
                double dist = std::min(hh, 360 - hh), w = dist <= 60 ? 1 : dist >= 120 ? 0 : (120 - dist) / 60;
                rgb[ch] = vmin + (vmax - vmin) * w;
            }
            t[k] = (uint32_t)std::lround(rgb[0]) << 16 | (uint32_t)std::lround(rgb[1]) << 8 | (uint32_t)std::lround(rgb[2]);
        }
        static const uint32_t grays[6] = {0x333333, 0x505050, 0x696969, 0x828282, 0xbebebe, 0xffffff};
        for (int k = 0; k < 6; k++) t[250 + k] = grays[k];
        init = true;
    }
    return t[i & 255];
}

struct Layer {
    uint32_t color = COL_ACI | 7;
    int ltype = LT_CONTINUOUS;
    bool off = false, frozen = false;
};
struct LType {
    std::vector<double> pattern;   // dash > 0, gap < 0, dot 0 (drawing units)
};
struct TStyle {
    plutovg_font_face_t *face = nullptr;
    double height = 0, width = 1, oblique = 0;
    std::string family, file;   // (resolved to face at the end of the tables)
    bool bold = false, italic = false;
};

// Primitives of a block, in its own coordinates
enum : uint8_t { K_STROKE, K_FILL, K_FILL_EO, K_TEXT, K_POINT, K_INSERT, K_XLINE, K_RAY, K_HATCH, K_WIPEOUT };
struct Prim {
    uint8_t kind;
    int layer;
    uint32_t color;
    int ltype;
    float ltscale, width;
    uint32_t c0, c1, p0, p1;   // ranges of the block's path commands / points
    int aux;                   // TEXT: texts[], INSERT: inserts[], HATCH: patterns[]
};

struct Glyph {
    plutovg_path_t *path;   // at size 1 (em), y down, baseline 0
    float advance;
};
struct GRun {
    uint32_t color;   // 0: the entity's
    float size, sx, skew;
    uint32_t g0, g1;
};
struct GPos {
    uint32_t glyph;
    float x, y;
};
struct TextData {
    M2 m;   // text space (baseline at y 0, drawing units) -> block
    std::vector<GRun> runs;
    std::vector<GPos> glyphs;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // extents in text space
};
struct InsData {
    std::string block;
    int index = -2;   // the block (resolved when first drawn; -1: missing)
    M2 m;
    int cols = 1, rows = 1;
    bool dimension = false;   // (its points: definition points)
    V2 colStep, rowStep;
};
struct PatLine {
    V2 base, dir, off;           // dir: unit length
    std::vector<double> dashes;  // along the line
};
using Pattern = std::vector<PatLine>;

// a viewport of the sheet: the model space in a rectangle of the paper space
struct VPort {
    uint64_t handle;
    M2 m;                      // model -> paper
    double r[4];               // the rectangle
    std::vector<int> frozen;   // layers frozen in it
};

struct Block {
    std::vector<Prim> prims;
    Geo geo;
    V2 base;
    bool xref = false;
    std::vector<VPort> vports;   // a layout's paper space: its viewports
    int viewports = 0;           // (VIEWPORTs seen in it)
};

// --- the display list ----------------------------------------------------------

enum : uint8_t { O_STROKE, O_FILL, O_FILL_EO, O_TEXT, O_POINTS, O_XLINE, O_RAY, O_HATCH, O_DOTS, O_CLIP, O_UNCLIP };
struct Item {
    uint8_t kind;
    uint32_t rgba;   // 0xAARRGGBB
    float width;     // 0: hairline
    int dash;        // dashes[] or -1
    plutovg_path_t *path = nullptr;
    float bx0, by0, bx1, by1;   // bounding box (culling; not clipped); O_CLIP: the rectangle (up to O_UNCLIP)
    int aux = -1;               // O_TEXT: otexts[], O_HATCH: opatterns[], O_XLINE / O_RAY: point + direction in xlines[]
};
struct OText {
    int text;   // texts[]
    float m[6];
    uint32_t rgba;
};
struct Dash {
    std::vector<float> d;   // on, off, ... (dots 0: one device pixel when drawn)
    float offset, total;
};
struct OPatLine {
    float bx, by, dx, dy, ox, oy;
    std::vector<float> dashes;
};
// a page: the model space or a layout's sheet
struct Page {
    std::vector<Item> items;
    double ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;   // shown area (around the origin)
    double ox = 0, oy = 0;                       // the origin in drawing units
};

} // namespace

struct nitrodxf {
    std::vector<Page> pages;   // the model space, then the layouts
    std::vector<OText> otexts;
    std::vector<std::vector<OPatLine>> opatterns;
    std::vector<float> xlines;
    std::vector<Dash> dashes;
    std::vector<TextData> texts;
    std::vector<Glyph> glyphs;
    int pdmode = 0;
    int background = 0;
    double pdsize = 0;
    ~nitrodxf() {
        for (Page &pg : pages)
            for (Item &it : pg.items) plutovg_path_destroy(it.path);
        for (Glyph &g : glyphs) plutovg_path_destroy(g.path);
    }
};

namespace {

// --- the parser ----------------------------------------------------------------

struct Pending {   // an item's path while flattening, in drawing units
    std::vector<uint8_t> cmd;
    std::vector<V2> pt;
};

struct Parser {
    nitrodxf *doc;
    Conv conv;
    bool utf8 = false;
    int version = 0;   // 1009 for AC1009 ...
    double ltscale = 1, dimasz = 0.18, dimscale = 1;

    std::vector<Layer> layers{Layer()};
    std::unordered_map<std::string, int> layerIndex{{"0", 0}};
    std::vector<LType> ltypes;
    std::unordered_map<std::string, int> ltypeIndex;
    std::vector<TStyle> styles;
    std::unordered_map<std::string, int> styleIndex;
    struct DimStyle {
        double asz;
        std::string ldrblk;   // the leader's arrowhead block (handle), empty: closed filled
    };
    std::unordered_map<std::string, DimStyle> dimstyles;
    std::unordered_map<std::string, std::string> blockNames;   // BLOCK_RECORD handle -> name
    std::vector<Block> blocks;
    std::unordered_map<std::string, int> blockIndex;
    std::vector<InsData> inserts;
    std::vector<Pattern> patterns;
    std::unordered_map<uint64_t, uint32_t> glyphIndex;
    std::unordered_map<plutovg_font_face_t *, FaceInfo> faceInfo;
    plutovg_font_face_t *defaultFace = nullptr;

    Block model, paper;
    Block *cur = nullptr;   // the block being read
    struct Layout {
        int tab;
        std::string record;   // its paper space's BLOCK_RECORD handle
    };
    std::vector<Layout> layouts;
    std::unordered_map<std::string, int> layerHandles;
    // Inventor's views: in the model space, one block reference (on *IDW_BlockReferenceLayer) per
    // viewport, which shows only that one, all at about the same place. Which one is not in the
    // DXF; the one with the next handle after the viewport's (in every sample: 4 or 5 after it).
    std::vector<std::pair<uint64_t, int>> idwViews;   // handle, model.prims[]
    std::unordered_map<uint64_t, int> dashIndex;      // (line type, scale) -> doc->dashes[] (of all the pages)

    int layer(sv name) {
        std::string u = upper(name);
        if (u.empty()) return 0;
        auto it = layerIndex.find(u);
        if (it != layerIndex.end()) return it->second;
        layers.push_back(Layer());
        return layerIndex[u] = (int)layers.size() - 1;
    }
    int ltype(sv name) {
        std::string u = upper(name);
        if (u.empty() || u == "BYLAYER") return LT_BYLAYER;
        if (u == "BYBLOCK") return LT_BYBLOCK;
        if (u == "CONTINUOUS") return LT_CONTINUOUS;
        auto it = ltypeIndex.find(u);
        if (it != ltypeIndex.end()) return it->second;
        ltypes.push_back(LType());
        return ltypeIndex[u] = (int)ltypes.size() - 1;
    }
    TStyle &style(sv name) {
        std::string u = upper(name);
        if (u.empty()) u = "STANDARD";
        auto it = styleIndex.find(u);
        if (it != styleIndex.end()) return styles[it->second];
        styles.push_back(TStyle());
        styleIndex[u] = (int)styles.size() - 1;
        return styles.back();
    }
    plutovg_font_face_t *face(TStyle &st) {   // (locked by the caller)
        if (!st.face) st.face = find_font(st.family, st.file, st.bold, st.italic);
        if (!st.face) {
            if (!defaultFace) defaultFace = find_font("", "", false, false);
            st.face = defaultFace;
        }
        return st.face;
    }
    const FaceInfo &info(plutovg_font_face_t *f) {   // (locked by the caller)
        auto it = faceInfo.find(f);
        if (it != faceInfo.end()) return it->second;
        FaceInfo fi;
        if (f) {
            float adv, lsb, asc, desc, gap;
            plutovg_rect_t e;
            plutovg_font_face_get_glyph_metrics(f, 1, 'H', &adv, &lsb, &e);
            if (e.y < -0.2f && e.y > -2) fi.cap = -e.y;
            plutovg_font_face_get_metrics(f, 1, &asc, &desc, &gap, &e);
            if (desc < 0 && desc > -1) fi.descent = -desc;
        }
        return faceInfo[f] = fi;
    }
    // the glyph of a character in face f (or a fallback face): its index in doc->glyphs (locked)
    uint32_t glyph(plutovg_font_face_t *f, uint32_t cp) {
        uint64_t key = (uint64_t)(uintptr_t)f * 1000003u ^ cp;
        auto it = glyphIndex.find(key);
        if (it != glyphIndex.end()) return it->second;
        plutovg_font_face_t *g = f;
        if (g && cp > ' ' && !nf_has_glyph(g, cp)) {
            plutovg_font_face_t *fb = fallback_font(cp);
            if (fb) g = fb;
        }
        Glyph gl{plutovg_path_create(), 0};
        if (g) gl.advance = plutovg_font_face_get_glyph_path(g, 1, 0, 0, cp, gl.path);
        doc->glyphs.push_back(gl);
        return glyphIndex[key] = (uint32_t)doc->glyphs.size() - 1;
    }

    // --- header and tables

    void header(const Rec &r) {
        std::string var;
        for (size_t i = 0; i < r.g.size(); i++) {
            const Pair &p = r.g[i];
            if (p.code == 9) {
                var = upper(p.v);
                continue;
            }
            if (var == "$ACADVER" && p.code == 1) {
                std::string v = upper(p.v);
                if (v.size() >= 6 && v[0] == 'A' && v[1] == 'C') version = atoi(v.c_str() + 2);
            } else if (var == "$DWGCODEPAGE" && p.code == 3)
                conv.cp = codepage(p.v);
            else if (var == "$LTSCALE" && p.code == 40) {
                double v = num(p.v, 1);
                if (v > 0 && v < 1e9) ltscale = v;
            } else if (var == "$PDMODE" && p.code == 70)
                doc->pdmode = inum(p.v);
            else if (var == "$PDSIZE" && p.code == 40)
                doc->pdsize = num(p.v);
            else if (var == "$DIMASZ" && p.code == 40)
                dimasz = num(p.v, dimasz);
            else if (var == "$DIMSCALE" && p.code == 40)
                dimscale = num(p.v, 1);
            else if ((var == "$EXTMIN" || var == "$EXTMAX") && (p.code == 10 || p.code == 20))
                ext[var == "$EXTMAX"][p.code == 20] = num(p.v);
        }
        double diag = std::hypot(ext[1][0] - ext[0][0], ext[1][1] - ext[0][1]);
        if (diag > 0 && diag < 1e15) splineTol = diag / 4000;
    }
    double ext[2][2] = {{0, 0}, {0, 0}};

    uint32_t color(const Rec &r) {   // 62 (index) / 420 (true colour) of an entity
        const Pair *t = r.find(420);
        if (t) return COL_RGB | truecolor(t->v);
        int c = r.i(62, 256);
        if (c == 0) return COL_BYBLOCK;
        if (c < 0) c = -c;
        if (c >= 1 && c <= 255) return COL_ACI | (uint32_t)c;
        return COL_BYLAYER;
    }

    void tableEntry(const Rec &r) {
        if (r.type == "LAYER") {
            int i = layer(r.s(2));
            if (r.find(5)) layerHandles[upper(r.s(5))] = i;
            Layer &l = layers[i];
            int c = r.i(62, 7);
            l.off = c < 0;
            const Pair *t = r.find(420);
            l.color = t ? COL_RGB | truecolor(t->v) : COL_ACI | (uint32_t)(c == 0 ? 7 : std::min(255, std::abs(c)));
            l.frozen = r.i(70) & 1;
            int lt = ltype(r.s(6));
            l.ltype = lt < 0 ? LT_CONTINUOUS : lt;
        } else if (r.type == "LTYPE") {
            int i = ltype(r.s(2));
            if (i < 0) return;
            LType &lt = ltypes[i];
            lt.pattern.clear();
            for (const Pair &p : r.g)
                if (p.code == 49) lt.pattern.push_back(num(p.v));
        } else if (r.type == "STYLE") {
            if (r.i(70) & 1) return;   // (a shape file)
            TStyle &st = style(r.s(2));
            st.height = r.d(40);
            st.width = r.d(41, 1);
            st.oblique = r.d(50);
            st.file = std::string(trim(r.s(3)));
            bool acad = false;
            for (const Pair &p : r.g) {   // extended data of AutoCAD: the TrueType family and style
                if (p.code == 1001) acad = upper(p.v) == "ACAD";
                else if (acad && p.code == 1000 && st.family.empty()) st.family = std::string(trim(p.v));
                else if (acad && p.code == 1071) {
                    int f = inum(p.v);
                    st.italic = f & 0x1000000;
                    st.bold = f & 0x2000000;
                }
            }
        } else if (r.type == "DIMSTYLE") {
            double asz = r.d(41, dimasz), sc = r.d(40, 1);
            dimstyles[upper(r.s(2))] = {asz * (sc > 0 ? sc : 1), upper(r.s(341))};
        } else if (r.type == "BLOCK_RECORD") {
            blockNames[upper(r.s(5))] = upper(r.s(2));
        }
    }

    // --- entities

    bool add(Prim p, Block &b) {   // (the geometry is already in b.geo)
        b.prims.push_back(p);
        return true;
    }
    Prim prim(uint8_t kind, const Rec &r, Block &b) {
        Prim p{};
        p.kind = kind;
        p.layer = layer(r.s(8));
        p.color = color(r);
        p.ltype = ltype(r.s(6));
        double s = r.d(48, 1);
        p.ltscale = (float)(s > 0 && s < 1e9 ? s : 1);
        p.c0 = (uint32_t)b.geo.cmd.size();
        p.p0 = (uint32_t)b.geo.pt.size();
        p.aux = -1;
        return p;
    }
    void finish(Prim &p, Block &b) {   // closes the primitive's geometry range and keeps it (if any)
        p.c1 = (uint32_t)b.geo.cmd.size();
        p.p1 = (uint32_t)b.geo.pt.size();
        bool ok = p.p1 > p.p0 || p.kind == K_INSERT || p.kind == K_TEXT;
        for (uint32_t i = p.p0; i < p.p1 && ok; i++) ok = finite(b.geo.pt[i]);
        if (!ok) {   // (NaN / huge coordinates: dropped)
            b.geo.cmd.resize(p.c0);
            b.geo.pt.resize(p.p0);
            return;
        }
        b.prims.push_back(p);
    }
    V3 extrusion(const Rec &r) { return r.p3(210, {0, 0, 1}); }

    void line(const Rec &r, Block &b) {
        Prim p = prim(K_STROKE, r, b);
        b.geo.move(r.p2(10));
        b.geo.line(r.p2(11));
        finish(p, b);
    }
    void point(const Rec &r, Block &b) {
        Prim p = prim(K_POINT, r, b);
        b.geo.move(r.p2(10));
        finish(p, b);
    }
    void circle(const Rec &r, Block &b, bool arc) {
        Prim p = prim(K_STROKE, r, b);
        double rad = std::fabs(r.d(40));
        V3 c = r.p3(10);
        M2 m = ocs(extrusion(r), c.z);
        double t0 = 0, t1 = 2 * M_PI;
        if (arc) {
            t0 = std::fmod(r.d(50), 360) * M_PI / 180;
            t1 = arc_end(t0, std::fmod(r.d(51), 360) * M_PI / 180);
        }
        if (!(rad > 0) || !std::isfinite(t0 + t1)) return;
        add_arc(b.geo, m, {c.x, c.y}, {rad, 0}, {0, rad}, t0, t1, true);
        if (!arc) b.geo.close();
        finish(p, b);
    }
    void ellipse(const Rec &r, Block &b) {
        Prim p = prim(K_STROKE, r, b);
        V3 c = r.p3(10), mj = r.p3(11), n = unit(extrusion(r));
        double ratio = r.d(40, 1), t0 = std::fmod(r.d(41), 2 * M_PI), t1 = arc_end(t0, r.d(42, 2 * M_PI));
        V3 mn = cross(n, mj);
        mn = {mn.x * ratio, mn.y * ratio, mn.z * ratio};
        if (!std::isfinite(t0 + t1 + ratio)) return;
        add_arc(b.geo, M2(), {c.x, c.y}, {mj.x, mj.y}, {mn.x, mn.y}, t0, t1, true);
        finish(p, b);
    }

    // a 2D polyline with bulges and widths: a stroke, or with widths a fill / a wide stroke
    struct Vtx {
        V2 p;
        double w0 = 0, w1 = 0, bulge = 0;
    };
    void polyline2d(Prim p, std::vector<Vtx> &v, bool closed, const M2 &m, Block &b) {
        if (v.empty()) return;
        bool wide = false, constant = true;
        for (const Vtx &x : v) {
            if (x.w0 > 0 || x.w1 > 0) wide = true;
            if (x.w0 != v[0].w0 || x.w1 != v[0].w0) constant = false;
        }
        size_t nseg = closed ? v.size() : v.size() - 1;
        if (!wide || constant) {
            b.geo.move(m(v[0].p));
            for (size_t i = 0; i < nseg; i++) add_bulge(b.geo, m, v[i].p, v[(i + 1) % v.size()].p, v[i].bulge);
            if (closed) b.geo.close();
            if (v.size() == 1) b.geo.line(m(v[0].p));
            if (wide) p.width = (float)(v[0].w0 * std::sqrt(std::fabs(det(m))));
            finish(p, b);
            return;
        }
        // varying widths (arrows ...): each segment a filled quad (arcs: as short pieces)
        p.kind = K_FILL;
        for (size_t i = 0; i < nseg; i++) {
            const Vtx &a = v[i], &c = v[(i + 1) % v.size()];
            std::vector<V2> mid;   // centre line points
            std::vector<double> w;
            if (std::fabs(a.bulge) < 1e-9) {
                mid = {a.p, c.p};
                w = {a.w0, a.w1};
            } else {
                V2 ch = c.p - a.p;
                double d = len(ch), bb = a.bulge;
                if (d < 1e-12) continue;
                V2 cen = (a.p + c.p) * 0.5 + V2{-ch.y / d, ch.x / d} * (d * (1 - bb * bb) / (4 * bb));
                double rr = len(a.p - cen), t0 = std::atan2(a.p.y - cen.y, a.p.x - cen.x), sw = 4 * std::atan(bb);
                int n = std::max(2, std::min(64, (int)(std::fabs(sw) * 16)));
                for (int k = 0; k <= n; k++) {
                    double t = t0 + sw * k / n;
                    mid.push_back(cen + V2{std::cos(t), std::sin(t)} * rr);
                    w.push_back(a.w0 + (a.w1 - a.w0) * k / n);
                }
            }
            std::vector<V2> left, right;
            for (size_t k = 0; k < mid.size(); k++) {
                V2 dir = k + 1 < mid.size() ? mid[k + 1] - mid[k] : mid[k] - mid[k - 1];
                double l = len(dir);
                if (l < 1e-12) dir = {1, 0}, l = 1;
                V2 nrm = {-dir.y / l, dir.x / l};
                left.push_back(mid[k] + nrm * (w[k] / 2));
                right.push_back(mid[k] - nrm * (w[k] / 2));
            }
            b.geo.move(m(left[0]));
            for (size_t k = 1; k < left.size(); k++) b.geo.line(m(left[k]));
            for (size_t k = right.size(); k-- > 0;) b.geo.line(m(right[k]));
            b.geo.close();
        }
        finish(p, b);
    }

    void lwpolyline(const Rec &r, Block &b) {
        Prim p = prim(K_STROKE, r, b);
        std::vector<Vtx> v;
        double cw = r.d(43);
        for (const Pair &g : r.g) {
            if (g.code == 10) {
                v.push_back(Vtx());
                v.back().p.x = num(g.v);
                v.back().w0 = v.back().w1 = cw;
            } else if (v.empty())
                continue;
            else if (g.code == 20)
                v.back().p.y = num(g.v);
            else if (g.code == 40)
                v.back().w0 = num(g.v);
            else if (g.code == 41)
                v.back().w1 = num(g.v);
            else if (g.code == 42)
                v.back().bulge = num(g.v);
        }
        polyline2d(p, v, r.i(70) & 1, ocs(extrusion(r), r.d(38)), b);
    }

    // POLYLINE with its VERTEX records
    void polyline(const Rec &r, const std::vector<Rec> &verts, Block &b) {
        Prim p = prim(K_STROKE, r, b);
        int flags = r.i(70);
        if (flags & (16 | 64)) {   // polygon mesh / polyface mesh: their edges
            std::vector<V2> pts;
            std::vector<const Rec *> faces;
            for (const Rec &v : verts) {
                int vf = v.i(70);
                if (flags & 64 && (vf & 128) && !(vf & 64)) faces.push_back(&v);
                else pts.push_back(v.p2(10));
            }
            if (flags & 64) {
                for (const Rec *f : faces) {
                    int idx[4] = {f->i(71), f->i(72), f->i(73), f->i(74)};
                    int n = 0;
                    while (n < 4 && idx[n]) n++;
                    for (int k = 0; k < n; k++) {
                        int a = idx[k], c = idx[(k + 1) % n];
                        if (a < 0) continue;   // (an invisible edge)
                        a = std::abs(a) - 1;
                        c = std::abs(c) - 1;
                        if (a >= (int)pts.size() || c >= (int)pts.size()) continue;
                        b.geo.move(pts[a]);
                        b.geo.line(pts[c]);
                    }
                }
            } else {
                int mm = r.i(71), nn = r.i(72);
                if (mm > 0 && nn > 0 && (size_t)mm * nn <= pts.size()) {
                    for (int i = 0; i < mm; i++) {
                        b.geo.move(pts[(size_t)i * nn]);
                        for (int j = 1; j < nn; j++) b.geo.line(pts[(size_t)i * nn + j]);
                        if (flags & 32) b.geo.line(pts[(size_t)i * nn]);
                    }
                    for (int j = 0; j < nn; j++) {
                        b.geo.move(pts[j]);
                        for (int i = 1; i < mm; i++) b.geo.line(pts[(size_t)i * nn + j]);
                        if (flags & 1) b.geo.line(pts[j]);
                    }
                }
            }
            finish(p, b);
            return;
        }
        if (flags & 8) {   // 3D polyline (WCS)
            bool first = true;
            for (const Rec &v : verts) {
                if (v.i(70) & 16) continue;   // (spline frame control point)
                if (first) b.geo.move(v.p2(10));
                else b.geo.line(v.p2(10));
                first = false;
            }
            if (flags & 1 && !first) b.geo.close();
            finish(p, b);
            return;
        }
        std::vector<Vtx> v;
        double w0 = r.d(40), w1 = r.d(41);
        for (const Rec &x : verts) {
            if (x.i(70) & 16) continue;
            Vtx t;
            t.p = x.p2(10);
            t.w0 = x.d(40, w0);
            t.w1 = x.d(41, w1);
            t.bulge = x.d(42);
            v.push_back(t);
        }
        polyline2d(p, v, flags & 1, ocs(extrusion(r), r.d(30)), b);
    }

    void solid(const Rec &r, Block &b) {   // SOLID / TRACE: corners 1 2 4 3
        Prim p = prim(K_FILL, r, b);
        M2 m = ocs(extrusion(r), r.d(30));
        V2 a = r.p2(10), c = r.p2(11), d = r.p2(12), e = r.find(13) ? r.p2(13) : d;
        b.geo.move(m(a));
        b.geo.line(m(c));
        b.geo.line(m(e));
        b.geo.line(m(d));
        b.geo.close();
        finish(p, b);
    }
    void face3d(const Rec &r, Block &b) {
        Prim p = prim(K_STROKE, r, b);
        V2 v[4] = {r.p2(10), r.p2(11), r.p2(12), r.find(13) ? r.p2(13) : r.p2(12)};
        int inv = r.i(70);
        for (int k = 0; k < 4; k++) {
            if (inv & (1 << k)) continue;
            b.geo.move(v[k]);
            b.geo.line(v[(k + 1) % 4]);
        }
        finish(p, b);
    }

    // B-spline (or fit points only: a smooth curve through them) as line segments
    void spline_points(const std::vector<V2> &ctrl, std::vector<double> knots, std::vector<double> w, int deg, const std::vector<V2> &fit,
                       bool closed, std::vector<V2> &out) {
        size_t n = ctrl.size();
        if (n < 2 || deg < 1) {
            if (fit.size() >= 2) {   // Catmull-Rom through the fit points
                for (size_t i = 0; i + 1 < fit.size(); i++) {
                    V2 p0 = fit[i ? i - 1 : 0], p1 = fit[i], p2 = fit[i + 1], p3 = fit[i + 2 < fit.size() ? i + 2 : i + 1];
                    for (int k = i ? 1 : 0; k <= 8; k++) {
                        double t = k / 8.0, t2 = t * t, t3 = t2 * t;
                        out.push_back((p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5);
                    }
                }
            }
            return;
        }
        deg = std::min(deg, (int)n - 1);
        if (knots.size() != n + deg + 1) {   // (missing / broken knots: clamped uniform)
            knots.clear();
            for (size_t i = 0; i < n + deg + 1; i++) knots.push_back(i <= (size_t)deg ? 0 : i >= n ? (double)(n - deg) : (double)(i - deg));
        }
        bool rational = w.size() == n;
        for (double x : w)
            if (!(x > 0)) rational = false;
        double lo = knots[deg], hi = knots[n];
        if (!(hi > lo)) return;
        std::vector<double> px(deg + 1), py(deg + 1), pw(deg + 1);
        for (size_t span = deg; span < n; span++) {
            double k0 = knots[span], k1 = knots[span + 1];
            if (!(k1 > k0)) continue;
            double plen = 0;
            for (int j = 0; j < deg; j++) plen += len(ctrl[span - deg + j + 1] - ctrl[span - deg + j]);
            double q = splineTol > 0 ? plen / splineTol : 64;
            int most = (int)std::max<size_t>(1, std::min<size_t>(64, 2000000 / n));   // (at most ~2M points a spline)
            int steps = q > most || !(q == q) ? most : q < 4 ? std::min(4, most) : (int)q;
            for (int s = out.empty() ? 0 : 1; s <= steps; s++) {
                double u = k0 + (k1 - k0) * s / steps;
                for (int j = 0; j <= deg; j++) {   // de Boor
                    const V2 &c = ctrl[span - deg + j];
                    double ww = rational ? w[span - deg + j] : 1;
                    px[j] = c.x * ww;
                    py[j] = c.y * ww;
                    pw[j] = ww;
                }
                for (int rr = 1; rr <= deg; rr++)
                    for (int j = deg; j >= rr; j--) {
                        size_t i = span - deg + j;
                        double den = knots[i + deg + 1 - rr] - knots[i];
                        double a = den > 0 ? (u - knots[i]) / den : 0;
                        px[j] = (1 - a) * px[j - 1] + a * px[j];
                        py[j] = (1 - a) * py[j - 1] + a * py[j];
                        pw[j] = (1 - a) * pw[j - 1] + a * pw[j];
                    }
                out.push_back({px[deg] / pw[deg], py[deg] / pw[deg]});
            }
        }
        if (closed && !out.empty()) out.push_back(out[0]);
    }
    double splineTol = 0;   // segment length of splines (from the drawing's size, see HEADER $EXTMIN)

    void spline(const Rec &r, Block &b) {
        Prim p = prim(K_STROKE, r, b);
        std::vector<V2> ctrl, fit;
        std::vector<double> knots, w;
        for (size_t i = 0; i < r.g.size(); i++) {
            const Pair &g = r.g[i];
            if (g.code == 10) ctrl.push_back({num(g.v), 0});
            else if (g.code == 20 && !ctrl.empty()) ctrl.back().y = num(g.v);
            else if (g.code == 11) fit.push_back({num(g.v), 0});
            else if (g.code == 21 && !fit.empty()) fit.back().y = num(g.v);
            else if (g.code == 40) knots.push_back(num(g.v));
            else if (g.code == 41) w.push_back(num(g.v));
        }
        if (ctrl.size() > 1000000 || knots.size() > 1000000) return;
        std::vector<V2> out;
        spline_points(ctrl, knots, w, r.i(71, 3), fit, false, out);
        if (out.size() < 2) return;
        b.geo.move(out[0]);
        for (size_t i = 1; i < out.size(); i++) b.geo.line(out[i]);
        finish(p, b);
    }

    void xline(const Rec &r, Block &b, bool ray) {
        Prim p = prim(ray ? K_RAY : K_XLINE, r, b);
        V2 d = r.p2(11, {1, 0});
        double l = len(d);
        if (!(l > 0)) return;
        b.geo.move(r.p2(10));
        b.geo.move(d * (1 / l));
        finish(p, b);
    }

    // --- text

    // lays out a line of text (UTF-8) at x on the baseline y into t; returns its width
    double run(TextData &t, plutovg_font_face_t *f, const std::string &s, double x, double y, double size, double sx, double skew,
               uint32_t color, double tracking = 1) {
        GRun gr{color, (float)size, (float)sx, (float)skew, (uint32_t)t.glyphs.size(), 0};
        double x0 = x;
        for (size_t i = 0; i < s.size();) {
            uint32_t c = nf_utf8_next(s, i);
            if (c == '\t') c = ' ';
            if (c < ' ') continue;
            uint32_t gi = glyph(f, c);
            t.glyphs.push_back({gi, (float)x, (float)y});
            x += doc->glyphs[gi].advance * size * sx * tracking;
        }
        gr.g1 = (uint32_t)t.glyphs.size();
        if (gr.g1 > gr.g0) t.runs.push_back(gr);
        return x - x0;
    }

    void text(const Rec &r, Block &b, bool attrib) {
        if (attrib && (r.i(70) & 1)) return;   // (invisible attribute)
        sv raw = r.type == "ATTDEF" ? r.s(2) : r.s(1);
        if (trim(raw).empty()) return;
        Prim p = prim(K_TEXT, r, b);
        std::string s = decode(raw, conv, utf8, true);
        TStyle &st = style(r.s(7));
        double h = r.d(40, st.height > 0 ? st.height : 2.5), wf = r.d(41, st.width), obl = r.d(51, st.oblique);
        if (!(h > 0) || h > 1e12 || !(wf > 0)) return;
        int hj = r.i(72), vj = r.i(attrib ? 74 : 73), gen = r.i(71);
        V2 p10 = r.p2(10), p11 = r.find(11) ? r.p2(11) : p10;
        double rot = r.d(50) * M_PI / 180;
        TextData t;
        std::lock_guard<std::mutex> lock(nf_lock());
        plutovg_font_face_t *f = face(st);
        const FaceInfo &fi = info(f);
        double size = h / fi.cap, skew = std::tan(obl * M_PI / 180);
        double w = run(t, f, s, 0, 0, size, wf, skew, 0);
        V2 at = hj == 0 && vj == 0 ? p10 : p11;
        M2 local;
        if (hj == 3 || hj == 5) {   // aligned / fit: between 10 and 11 (aligned: the height follows)
            V2 d = p11 - p10;
            double l = len(d);
            if (l > 0 && w > 0) {
                rot = std::atan2(d.y, d.x);
                local = scaling(l / w, hj == 3 ? l / w : 1);
            }
            at = p10;
        } else {
            double dx = hj == 1 || hj == 4 ? -w / 2 : hj == 2 ? -w : 0, dy = 0;
            if (hj == 4 && vj == 0) dy = -h / 2;
            else if (vj == 1) dy = fi.descent / fi.cap * h;
            else if (vj == 2) dy = -h / 2;
            else if (vj == 3) dy = -h;
            local = translate({dx, dy});
        }
        M2 mir = scaling(gen & 2 ? -1 : 1, gen & 4 ? -1 : 1);
        t.m = ocs(extrusion(r), r.d(30)) * translate(at) * rotate(rot) * mir * local;
        t.x0 = 0;
        t.x1 = w;
        t.y0 = -fi.descent / fi.cap * h;
        t.y1 = h;
        p.aux = (int)doc->texts.size();
        doc->texts.push_back(std::move(t));
        p.c1 = p.c0;   // (no path)
        p.p1 = p.p0;
        b.prims.push_back(p);
    }

    void mtext(const Rec &r, Block &b) {
        std::string raw;
        for (const Pair &g : r.g)
            if (g.code == 3) raw.append(g.v.data(), g.v.size());
        for (const Pair &g : r.g)
            if (g.code == 1) raw.append(g.v.data(), g.v.size());
        if (raw.empty()) return;
        std::string s = decode(raw, conv, utf8, true);
        Prim p = prim(K_TEXT, r, b);
        TStyle &st = style(r.s(7));
        double h0 = r.d(40, st.height > 0 ? st.height : 2.5), width = r.d(41), spacing = r.d(44, 1);
        if (!(h0 > 0) || h0 > 1e12) return;
        if (!(spacing > 0) || spacing > 100) spacing = 1;
        int attach = r.i(71, 1);
        if (attach < 1 || attach > 9) attach = 1;

        struct St {
            plutovg_font_face_t *face;
            double h, w, skew, track;
            uint32_t color;
        };
        struct Ch {
            uint32_t cp;
            int st;
        };
        std::lock_guard<std::mutex> lock(nf_lock());
        std::vector<St> states;
        St s0{face(st), h0, st.width > 0 ? st.width : 1, std::tan(st.oblique * M_PI / 180), 1, 0};
        St cs = s0;
        std::vector<St> stack;
        std::vector<std::vector<Ch>> paras(1);
        auto state = [&]() {
            if (states.empty() || memcmp(&states.back(), &cs, sizeof cs)) states.push_back(cs);
            return (int)states.size() - 1;
        };
        auto param = [&](size_t &i) {   // up to ';'
            size_t e = s.find(';', i);
            if (e == std::string::npos) e = s.size();
            std::string v = s.substr(i, e - i);
            i = e < s.size() ? e + 1 : e;
            return v;
        };
        for (size_t i = 0; i < s.size();) {
            char c = s[i];
            if (c == '{') {
                stack.push_back(cs);
                i++;
                continue;
            }
            if (c == '}') {
                if (!stack.empty()) cs = stack.back(), stack.pop_back();
                i++;
                continue;
            }
            if (c == '^' && i + 1 < s.size()) {   // caret codes: ^J ^M line breaks, ^I tab
                char k = s[i + 1];
                if (k == 'J' || k == 'M') {
                    paras.emplace_back();
                    i += 2;
                    continue;
                }
                if (k == 'I') {
                    paras.back().push_back({' ', state()});
                    i += 2;
                    continue;
                }
            }
            if (c == '\\' && i + 1 < s.size()) {
                char k = s[i + 1];
                i += 2;
                switch (k) {
                case 'P':
                case 'N':
                case 'X': paras.emplace_back(); continue;
                case '~': paras.back().push_back({' ', state()}); continue;
                case '\\':
                case '{':
                case '}': paras.back().push_back({(uint32_t)k, state()}); continue;
                case 'L': case 'l': case 'O': case 'o': case 'K': case 'k': continue;
                case 'H': {
                    std::string v = param(i);
                    double x = num(v, 1);
                    if (!v.empty() && (v.back() == 'x' || v.back() == 'X')) x *= cs.h;
                    if (x > 0 && x < 1e12) cs.h = x;
                    continue;
                }
                case 'W': {
                    std::string v = param(i);
                    double x = num(v, 1);
                    if (!v.empty() && (v.back() == 'x' || v.back() == 'X')) x *= cs.w;
                    if (x > 0 && x < 100) cs.w = x;
                    continue;
                }
                case 'Q': cs.skew = std::tan(num(param(i)) * M_PI / 180); continue;
                case 'T': {
                    double x = num(param(i), 1);
                    if (x > 0 && x < 100) cs.track = x;
                    continue;
                }
                case 'C': {
                    int x = inum(param(i), 256);
                    cs.color = x >= 1 && x <= 255 ? COL_ACI | (uint32_t)x : 0;
                    continue;
                }
                case 'c': {   // (blue in the low byte)
                    uint32_t x = (uint32_t)inum(param(i));
                    cs.color = COL_RGB | (x & 0xff) << 16 | (x & 0xff00) | (x >> 16 & 0xff);
                    continue;
                }
                case 'f':
                case 'F': {
                    std::string v = param(i);
                    size_t bar = v.find('|');
                    std::string name = v.substr(0, bar);
                    bool bold = v.find("|b1") != std::string::npos, italic = v.find("|i1") != std::string::npos;
                    bool file = k == 'F' || name.find('.') != std::string::npos;
                    plutovg_font_face_t *fc = file ? find_font("", name, bold, italic) : find_font(name, "", bold, italic);
                    if (fc) cs.face = fc;
                    continue;
                }
                case 'A':
                case 'p': param(i); continue;
                case 'S': {   // stacked text: "1/2", "1^2", "1#2"
                    std::string v = param(i);
                    for (char &x : v)
                        if (x == '^' || x == '#') x = '/';
                    for (size_t j = 0; j < v.size();) paras.back().push_back({nf_utf8_next(v, j), state()});
                    continue;
                }
                default: i -= 1; paras.back().push_back({'\\', state()}); continue;
                }
            }
            paras.back().push_back({nf_utf8_next(s, i), state()});
        }

        // lines: words wrapped at the reference width
        struct LGlyph {
            uint32_t g;
            double x, adv;
            int st;
            bool space;
        };
        struct LLine {
            std::vector<LGlyph> g;
            double w = 0, h = 0;
        };
        std::vector<LLine> lines;
        for (auto &para : paras) {
            lines.emplace_back();
            lines.back().h = cs.h;
            double x = 0;
            for (const Ch &ch : para) {
                const St &q = states[ch.st];
                uint32_t gi = glyph(q.face, ch.cp);
                double adv = doc->glyphs[gi].advance * q.h / info(q.face).cap * q.w * q.track;
                LLine *L = &lines.back();
                if (width > 0 && x + adv > width * 1.0001 && ch.cp != ' ' && !L->g.empty()) {
                    size_t sp = L->g.size();
                    while (sp > 0 && !L->g[sp - 1].space) sp--;
                    if (sp > 0) {   // (a word longer than the width stays on its line)
                        LLine nl;
                        double shift = sp < L->g.size() ? L->g[sp].x : x;
                        for (size_t k = sp; k < L->g.size(); k++) {
                            LGlyph lg = L->g[k];
                            lg.x -= shift;
                            nl.g.push_back(lg);
                        }
                        L->g.resize(sp);
                        lines.push_back(nl);
                        L = &lines.back();
                        x -= shift;
                    }
                }
                L->g.push_back({gi, x, adv, ch.st, ch.cp == ' '});
                x += adv;
            }
        }
        for (LLine &L : lines) {
            while (!L.g.empty() && L.g.back().space) L.g.pop_back();
            L.w = L.g.empty() ? 0 : L.g.back().x + L.g.back().adv;
            for (const LGlyph &g : L.g) L.h = std::max(L.h, states[g.st].h);
            if (L.g.empty()) L.h = std::max(L.h, h0);
        }
        TextData t;
        double y = 0, maxw = 0;
        int col = (attach - 1) % 3, row = (attach - 1) / 3;
        for (size_t li = 0; li < lines.size(); li++) {
            LLine &L = lines[li];
            y -= li ? L.h * spacing * 5 / 3 : L.h;
            double x0 = col == 0 ? 0 : col == 1 ? -L.w / 2 : -L.w;
            maxw = std::max(maxw, L.w);
            int last = -1;
            for (const LGlyph &g : L.g) {
                const St &q = states[g.st];
                if (g.st != last) {
                    t.runs.push_back({q.color, (float)(q.h / info(q.face).cap), (float)q.w, (float)q.skew, (uint32_t)t.glyphs.size(), 0});
                    last = g.st;
                }
                t.glyphs.push_back({g.g, (float)(x0 + g.x), (float)y});
                t.runs.back().g1 = (uint32_t)t.glyphs.size();
            }
        }
        double bottom = y - h0 * 0.3, shift = row == 0 ? 0 : row == 1 ? -y / 2 : -y;
        for (GPos &g : t.glyphs) g.y += (float)shift;
        double x0 = col == 0 ? 0 : col == 1 ? -maxw / 2 : -maxw;
        t.x0 = x0;
        t.x1 = x0 + maxw;
        t.y0 = bottom + shift;
        t.y1 = shift;
        double rot = r.d(50) * M_PI / 180;
        M2 place;
        if (r.find(11)) {   // the direction of the x axis (WCS) wins over the rotation
            V2 d = r.p2(11);
            if (len(d) > 0) rot = std::atan2(d.y, d.x);
            place = translate(r.p2(10)) * rotate(rot);
        } else
            place = ocs(extrusion(r)) * translate(r.p2(10)) * rotate(rot);
        t.m = place;
        p.aux = (int)doc->texts.size();
        doc->texts.push_back(std::move(t));
        p.c1 = p.c0;
        p.p1 = p.p0;
        b.prims.push_back(p);
    }

    // --- blocks, hatches, images, leaders

    void insert(const Rec &r, Block &b, bool dimension) {
        Prim p = prim(K_INSERT, r, b);
        InsData in;
        in.block = upper(r.s(2));
        if (in.block.empty()) return;
        in.dimension = dimension;
        if (dimension) {   // the dimension's own anonymous block, in place
            if (r.find(12)) in.m = translate(r.p2(12));
        } else {
            V2 at = r.p2(10);
            double sx = r.d(41, 1), sy = r.d(42, 1), rot = r.d(50) * M_PI / 180;
            if (!std::isfinite(sx + sy + rot) || sx == 0 || sy == 0) return;
            M2 o = ocs(extrusion(r), r.d(30));
            in.m = o * translate(at) * rotate(rot) * scaling(sx, sy);   // (the base point: when expanded)
            in.cols = std::max(1, std::min(10000, r.i(70, 1)));
            in.rows = std::max(1, std::min(10000, r.i(71, 1)));
            M2 dir = o * rotate(rot);
            in.colStep = dir.vec({r.d(44), 0});
            in.rowStep = dir.vec({0, r.d(45)});
        }
        p.aux = (int)inserts.size();
        inserts.push_back(std::move(in));
        p.c1 = p.c0;
        p.p1 = p.p0;
        b.prims.push_back(p);
    }

    void leader(const Rec &r, Block &b) {
        Prim p = prim(K_STROKE, r, b);
        std::vector<V2> v;
        for (const Pair &g : r.g) {
            if (g.code == 10) v.push_back({num(g.v), 0});
            else if (g.code == 20 && !v.empty()) v.back().y = num(g.v);
        }
        if (v.size() < 2) return;
        b.geo.move(v[0]);
        for (size_t i = 1; i < v.size(); i++) b.geo.line(v[i]);
        finish(p, b);
        if (!r.i(71, 1)) return;
        // the arrowhead at the first vertex: the dimension style's (or its overrides in the
        // extended data) leader arrow block, scaled to the arrow size; else a closed filled one
        auto it = dimstyles.find(upper(r.s(3)));
        double asz = it != dimstyles.end() ? it->second.asz : dimasz * dimscale, scale = 1;
        std::string blk = it != dimstyles.end() ? it->second.ldrblk : std::string();
        bool acad = false;
        for (size_t i = 0; i + 1 < r.g.size(); i++) {
            const Pair &g = r.g[i];
            if (g.code == 1001) acad = upper(g.v) == "ACAD";
            else if (acad && g.code == 1070) {
                int var = inum(g.v);
                const Pair &val = r.g[i + 1];
                if (var == 41 && val.code == 1040) asz = num(val.v, asz);
                else if (var == 40 && val.code == 1040) scale = num(val.v, 1);
                else if (var == 341 && val.code == 1005) blk = upper(val.v);
            }
        }
        if (scale > 0) asz *= scale;
        V2 d = v[0] - v[1];
        double l = len(d);
        if (!(asz > 0) || !(l > 0) || !std::isfinite(asz)) return;
        auto name = blockNames.find(blk);
        if (!blk.empty() && blk != "0" && name != blockNames.end()) {
            Prim q = prim(K_INSERT, r, b);
            InsData in;
            in.block = name->second;
            in.m = translate(v[0]) * rotate(std::atan2(d.y, d.x)) * scaling(asz, asz);
            q.aux = (int)inserts.size();
            inserts.push_back(std::move(in));
            q.c1 = q.c0;
            q.p1 = q.p0;
            b.prims.push_back(q);
            return;
        }
        d = d * (1 / l);
        V2 n = {-d.y, d.x}, back = v[0] - d * asz;
        Prim q = prim(K_FILL, r, b);
        b.geo.move(v[0]);
        b.geo.line(back + n * (asz / 6));
        b.geo.line(back - n * (asz / 6));
        b.geo.close();
        finish(q, b);
    }

    // IMAGE (its frame) / WIPEOUT (an area in the background colour)
    void image(const Rec &r, Block &b, bool wipeout) {
        Prim p = prim(wipeout ? K_WIPEOUT : K_STROKE, r, b);
        V2 at = r.p2(10), u = r.p2(11), v = r.p2(12), size = r.p2(13, {1, 1});
        std::vector<V2> bp;
        for (const Pair &g : r.g) {
            if (g.code == 14) bp.push_back({num(g.v), 0});
            else if (g.code == 24 && !bp.empty()) bp.back().y = num(g.v);
        }
        if (bp.size() < 2) bp = {{-0.5, -0.5}, {size.x - 0.5, size.y - 0.5}};
        if (bp.size() == 2) bp = {bp[0], {bp[1].x, bp[0].y}, bp[1], {bp[0].x, bp[1].y}};
        V2 o = at + u * 0.5 - v * 0.5;
        for (size_t i = 0; i < bp.size(); i++) {
            V2 q = o + u * bp[i].x + v * (size.y - bp[i].y);
            if (i) b.geo.line(q);
            else b.geo.move(q);
        }
        b.geo.close();
        finish(p, b);
    }

    // HATCH: its boundary loops (OCS) filled, or its pattern lines clipped to them
    void hatch(const Rec &r, Block &b) {
        const std::vector<Pair> &g = r.g;
        size_t i = 0, n = g.size();
        auto seek = [&](int code) {   // to the next group code, from i
            while (i < n && g[i].code != code) i++;
            return i < n;
        };
        auto take = [&](int code, double def) {   // the group at i if it has the code
            if (i < n && g[i].code == code) return num(g[i++].v);
            return def;
        };
        V3 elev = r.p3(10);
        int solidFill = r.i(70);
        if (!seek(91)) return;
        int nloops = inum(g[i++].v);
        if (nloops <= 0 || nloops > 1000000) return;
        struct Seg {
            bool cubic;
            V2 p[4];
        };
        std::vector<std::vector<Seg>> loops;
        for (int L = 0; L < nloops; L++) {
            if (!seek(92)) break;
            int flags = inum(g[i++].v);
            loops.emplace_back();
            auto &segs = loops.back();
            if (flags & 2) {   // polyline
                bool bulges = take(72, 0) != 0, closed = take(73, 1) != 0;
                (void)closed;
                if (!seek(93)) break;
                int nv = inum(g[i++].v);
                std::vector<Vtx> v;
                for (int k = 0; k < nv && i < n && g[i].code == 10; k++) {
                    Vtx x;
                    x.p.x = take(10, 0);
                    x.p.y = take(20, 0);
                    if (bulges) x.bulge = take(42, 0);
                    v.push_back(x);
                }
                for (size_t k = 0; k < v.size(); k++) {
                    const Vtx &a = v[k], &c = v[(k + 1) % v.size()];
                    if (std::fabs(a.bulge) < 1e-9) {
                        segs.push_back({false, {a.p, a.p, c.p, c.p}});
                        continue;
                    }
                    Geo tmp;
                    tmp.move(a.p);
                    add_bulge(tmp, M2(), a.p, c.p, a.bulge);
                    V2 last = a.p;
                    size_t pi = 1;
                    for (size_t ci = 1; ci < tmp.cmd.size(); ci++) {
                        if (tmp.cmd[ci] == C_CUBIC) {
                            segs.push_back({true, {last, tmp.pt[pi], tmp.pt[pi + 1], tmp.pt[pi + 2]}});
                            last = tmp.pt[pi + 2];
                            pi += 3;
                        } else {
                            segs.push_back({false, {last, last, tmp.pt[pi], tmp.pt[pi]}});
                            last = tmp.pt[pi];
                            pi++;
                        }
                    }
                }
            } else {   // edges
                if (!seek(93)) break;
                int ne = inum(g[i++].v);
                for (int k = 0; k < ne && i < n; k++) {
                    if (!seek(72)) break;
                    int type = inum(g[i++].v);
                    std::vector<Seg> es;
                    auto arcSegs = [&](V2 c, V2 u, V2 v, double t0, double t1) {
                        Geo tmp;
                        add_arc(tmp, M2(), c, u, v, t0, t1, true);
                        V2 last = tmp.pt[0];
                        for (size_t pi = 1; pi + 2 < tmp.pt.size(); pi += 3) {
                            es.push_back({true, {last, tmp.pt[pi], tmp.pt[pi + 1], tmp.pt[pi + 2]}});
                            last = tmp.pt[pi + 2];
                        }
                    };
                    if (type == 1) {
                        V2 a{take(10, 0), take(20, 0)}, c{take(11, 0), take(21, 0)};
                        es.push_back({false, {a, a, c, c}});
                    } else if (type == 2 || type == 3) {
                        V2 c{take(10, 0), take(20, 0)}, mj{1, 0};
                        double rad = 1, ratio = 1;
                        if (type == 2) rad = take(40, 0);
                        else {
                            mj = {take(11, 0), take(21, 0)};
                            ratio = take(40, 1);
                        }
                        double a0 = take(50, 0), a1 = take(51, 360);
                        bool ccw = take(73, 1) != 0;
                        if (!ccw) {   // (stored as 360 - angle)
                            a0 = 360 - a0;
                            a1 = 360 - a1;
                        }
                        double t0 = a0 * M_PI / 180, t1 = a1 * M_PI / 180;
                        if (type == 3) {   // angles -> ellipse parameters
                            t0 = std::atan2(std::sin(t0) / ratio, std::cos(t0));
                            t1 = std::atan2(std::sin(t1) / ratio, std::cos(t1));
                        }
                        bool full = std::fabs(std::fabs(a1 - a0) - 360) < 1e-6;
                        t1 = full ? t0 + (ccw ? 2 * M_PI : -2 * M_PI) : arc_end(t0, t1, !ccw);
                        V2 u = type == 2 ? V2{rad, 0} : mj, v = type == 2 ? V2{0, rad} : V2{-mj.y, mj.x} * ratio;
                        if (std::isfinite(t0 + t1) && len(u) > 0) arcSegs(c, u, v, t0, t1);
                    } else if (type == 4) {
                        int deg = (int)take(94, 3);
                        bool rational = take(73, 0) != 0;
                        take(74, 0);
                        int nk = (int)take(95, 0), nc = (int)take(96, 0);
                        if (nk < 0 || nc < 0 || nk > 100000 || nc > 100000) return;
                        std::vector<double> knots, w;
                        std::vector<V2> ctrl, fit;
                        for (int q = 0; q < nk && i < n && g[i].code == 40; q++) knots.push_back(num(g[i++].v));
                        for (int q = 0; q < nc && i < n && g[i].code == 10; q++) {
                            V2 c{take(10, 0), take(20, 0)};
                            ctrl.push_back(c);
                            if (rational || (i < n && g[i].code == 42)) w.push_back(take(42, 1));
                        }
                        int nf = (int)take(97, 0);
                        for (int q = 0; q < nf && q < 100000 && i < n && g[i].code == 11; q++) fit.push_back({take(11, 0), take(21, 0)});
                        take(12, 0), take(22, 0), take(13, 0), take(23, 0);
                        std::vector<V2> out;
                        spline_points(ctrl, knots, w, deg, fit, false, out);
                        for (size_t q = 1; q < out.size(); q++) es.push_back({false, {out[q - 1], out[q - 1], out[q], out[q]}});
                    }
                    if (es.empty()) continue;
                    // chain the edges: reversed where its end meets the loop
                    if (!segs.empty()) {
                        V2 e = segs.back().p[3];
                        double tol = 1e-9 + 1e-6 * (std::fabs(e.x) + std::fabs(e.y));
                        if (len(es.back().p[3] - e) < tol && len(es.front().p[0] - e) >= tol) {
                            std::reverse(es.begin(), es.end());
                            for (Seg &sg : es) std::swap(sg.p[0], sg.p[3]), std::swap(sg.p[1], sg.p[2]);
                        }
                    }
                    segs.insert(segs.end(), es.begin(), es.end());
                }
            }
            if (i < n && g[i].code == 97) {   // source boundary objects
                int ns = inum(g[i++].v);
                for (int k = 0; k < ns && i < n && g[i].code == 330; k++) i++;
            }
        }
        // the pattern (already scaled and turned in the file)
        Pattern pat;
        if (!solidFill && seek(78)) {
            int nl = inum(g[i++].v);
            for (int k = 0; k < nl && k < 1000 && i < n; k++) {
                if (!seek(53)) break;
                double a = num(g[i++].v) * M_PI / 180;
                PatLine pl;
                pl.base = {take(43, 0), take(44, 0)};
                pl.off = {take(45, 0), take(46, 0)};
                pl.dir = {std::cos(a), std::sin(a)};
                int nd = (int)take(79, 0);
                for (int q = 0; q < nd && q < 1000 && i < n && g[i].code == 49; q++) pl.dashes.push_back(num(g[i++].v));
                pat.push_back(pl);
            }
        }
        uint32_t gradient = 0;
        if (r.i(450) == 1) {   // gradient fill: its first colour
            const Pair *c = r.find(421);
            gradient = c ? COL_RGB | truecolor(c->v) : 0;
        }
        bool solid = solidFill || gradient || pat.empty();
        Prim p = prim(solid ? K_FILL_EO : K_HATCH, r, b);
        if (gradient) p.color = gradient;
        M2 m = ocs(extrusion(r), elev.z);
        for (auto &segs : loops) {
            if (segs.empty()) continue;
            b.geo.move(m(segs[0].p[0]));
            for (const Seg &sg : segs) {
                if (len(sg.p[0] - b.geo.cur) > 0 && len(m(sg.p[0]) - b.geo.cur) > 1e-9) b.geo.line(m(sg.p[0]));
                if (sg.cubic) b.geo.cubic(m(sg.p[1]), m(sg.p[2]), m(sg.p[3]));
                else b.geo.line(m(sg.p[3]));
            }
            b.geo.close();
        }
        if (!solid) {
            for (PatLine &pl : pat) {
                pl.base = m(pl.base);
                pl.off = m.vec(pl.off);
                V2 d = m.vec(pl.dir);
                double l = len(d);
                pl.dir = l > 0 ? d * (1 / l) : V2{1, 0};
                for (double &x : pl.dashes) x *= l;
            }
            p.aux = (int)patterns.size();
            patterns.push_back(std::move(pat));
        }
        finish(p, b);
    }

    // A viewport of a layout (in its paper space b), as ezdxf draws them: paper = centre + scale *
    // (twist (DCS of (model - target)) - view centre), the display coordinate system from the view
    // direction by the arbitrary axis algorithm (a parallel projection of the plane z = 0: affine;
    // perspective views are left out). The layout's own view is not one on the sheet: id 1 in
    // the entities, the first one in a layout's block (whose ids are all 0). Up to R14 the view
    // is in AutoCAD's extended data ("MVIEW").
    void viewport(const Rec &r, Block &b, bool entities) {
        if (b.viewports++ == 0 && !entities) return;
        double w = r.d(40), h = r.d(41), vh = r.d(45), twist = r.d(51);
        V2 c = r.p2(10), vc = r.p2(12);
        V3 t = r.p3(17), dir = r.p3(16, {0, 0, 1});
        VPort v;
        if (!r.find(45)) {
            bool acad = false, mview = false;
            int pts = 0, reals = 0;
            for (const Pair &p : r.g) {
                double x = num(p.v);
                if (p.code == 1001) acad = upper(p.v) == "ACAD", mview = false;
                else if (acad && p.code == 1000) mview = upper(p.v) == "MVIEW";
                else if (!mview) continue;
                else if (p.code >= 1010 && p.code <= 1030 && (pts < 2 || (pts == 2 && p.code != 1010))) {   // target, direction
                    pts += p.code == 1010;
                    V3 &q = pts == 1 ? t : dir;
                    (p.code == 1010 ? q.x : p.code == 1020 ? q.y : q.z) = x;
                } else if (p.code == 1040 && pts == 2 && reals < 4)   // twist, view height, centre
                    *(reals == 0 ? &twist : reals == 1 ? &vh : reals == 2 ? &vc.x : &vc.y) = x, reals++;
                else if (p.code == 1003) {   // frozen layers, by name
                    auto it = layerIndex.find(upper(p.v));
                    if (it != layerIndex.end()) v.frozen.push_back(it->second);
                }
            }
        }
        double k = vh > 0 ? h / vh : 0;
        V3 n = unit(dir), ax, ay;
        if ((entities && r.i(69) == 1) || (r.i(90) & 0x20001) || !(w > 0 && k > 1e-12 && k < 1e12) || std::fabs(n.z) < 1e-6) return;
        if (!finite(c) || !finite(vc) || !finite(V2{w, h}) || !std::isfinite(t.x + t.y + t.z + twist)) return;
        plane_axes(n, ax, ay);
        M2 dcs{ax.x, ay.x, ax.y, ay.y, -(t.x * ax.x + t.y * ax.y + t.z * ax.z), -(t.x * ay.x + t.y * ay.y + t.z * ay.z)};
        v.handle = strtoull(std::string(r.s(5)).c_str(), nullptr, 16);
        v.m = translate(c - vc * k) * rotate(twist * M_PI / 180) * scaling(k, k) * dcs;
        v.r[0] = c.x - w / 2, v.r[1] = c.y - h / 2, v.r[2] = c.x + w / 2, v.r[3] = c.y + h / 2;
        for (const Pair &p : r.g) {
            auto it = p.code == 331 ? layerHandles.find(upper(p.v)) : layerHandles.end();
            if (it != layerHandles.end()) v.frozen.push_back(it->second);
        }
        if (b.vports.size() < 1000) b.vports.push_back(std::move(v));
    }

    void layout(const Rec &r) {   // (its own groups after the plot settings)
        bool own = false;
        Layout l{0, {}};
        for (const Pair &p : r.g) {
            if (p.code == 100) own = upper(p.v) == "ACDBLAYOUT";
            else if (own && p.code == 71) l.tab = inum(p.v);
            else if (own && p.code == 330) l.record = upper(trim(p.v));
        }
        if (!l.record.empty() && layouts.size() < 1000) layouts.push_back(l);
    }

    // one entity record into block b (POLYLINE: with its vertices)
    void entity(const Rec &r, Block &b, const std::vector<Rec> *verts) {
        const std::string &t = r.type;
        if (r.i(60) == 1) return;   // (invisible)
        if (t == "LINE") line(r, b);
        else if (t == "LWPOLYLINE") lwpolyline(r, b);
        else if (t == "POLYLINE") polyline(r, verts ? *verts : std::vector<Rec>(), b);
        else if (t == "CIRCLE") circle(r, b, false);
        else if (t == "ARC") circle(r, b, true);
        else if (t == "ELLIPSE") ellipse(r, b);
        else if (t == "SPLINE" || t == "HELIX") spline(r, b);
        else if (t == "POINT") point(r, b);
        else if (t == "TEXT") text(r, b, false);
        else if (t == "ATTRIB") text(r, b, true);
        else if (t == "ATTDEF") {
            if (&b == &model || &b == &paper) text(r, b, true);   // (in a block: a definition only)
        } else if (t == "MTEXT")
            mtext(r, b);
        else if (t == "INSERT")
            insert(r, b, false);
        else if (t == "DIMENSION" || t == "ARC_DIMENSION" || t == "LARGE_RADIAL_DIMENSION")
            insert(r, b, true);
        else if (t == "SOLID" || t == "TRACE")
            solid(r, b);
        else if (t == "3DFACE")
            face3d(r, b);
        else if (t == "HATCH")
            hatch(r, b);
        else if (t == "LEADER")
            leader(r, b);
        else if (t == "WIPEOUT")
            image(r, b, true);
        else if (t == "IMAGE")
            image(r, b, false);
        else if (t == "XLINE")
            xline(r, b, false);
        else if (t == "RAY")
            xline(r, b, true);
    }
};

} // namespace

namespace {

// --- flattening ----------------------------------------------------------------

struct Flat {
    Parser &P;
    nitrodxf *doc;
    std::vector<Item> &items;    // a page's
    std::vector<Pending> pend;   // the items' paths
    std::vector<M2> textM;       // the texts' transforms (doc->otexts from otext0)
    size_t otext0;
    std::vector<Pattern> pats;   // hatch patterns (doc->opatterns)
    std::vector<char> active;   // blocks being expanded (no recursion)
    std::vector<char> hidden;   // layers frozen in the viewport being drawn
    std::vector<char> skip;     // the model space's prims not in the viewport being drawn
    size_t points, work;        // (of all the pages)
    // (block references exploding, arrays or nested: drawn in part)
    static constexpr size_t kMaxPoints = 40000000, kMaxWork = 100000000;

    Flat(Parser &p, nitrodxf *d, std::vector<Item> &it, size_t pts, size_t wk)
        : P(p), doc(d), items(it), otext0(d->otexts.size()), active(p.blocks.size()), hidden(p.layers.size()), points(pts), work(wk) {}

    struct Ctx {
        int layer;
        uint32_t rgb;
        int ltype;
    };

    uint32_t rgb(uint32_t col, int layer, const Ctx &parent) {
        switch (col & 0xff000000u) {
        case COL_ACI: return aci_rgb((int)(col & 255));
        case COL_RGB: return col & 0xffffff;
        case COL_BYBLOCK: return parent.rgb;
        default: {
            uint32_t lc = P.layers[layer].color;
            return (lc & 0xff000000u) == COL_RGB ? lc & 0xffffff : aci_rgb((int)(lc & 255));
        }
        }
    }

    int dash(int lt, double scale) {   // a line type at a scale: doc->dashes[], -1 if continuous
        if (lt < 0 || lt >= (int)P.ltypes.size()) return -1;
        const std::vector<double> &pat = P.ltypes[lt].pattern;
        float fs = (float)scale;
        uint32_t bits;
        memcpy(&bits, &fs, 4);
        uint64_t key = (uint64_t)lt << 32 | bits;
        auto it = P.dashIndex.find(key);
        if (it != P.dashIndex.end()) return it->second;
        // alternating on / off from an "on" element: the pattern turned, the turn as the dash offset
        std::vector<std::pair<double, bool>> e;
        for (double x : pat)
            if (std::isfinite(x)) e.push_back({std::fabs(x) * scale, x >= 0});
        int idx = -1;
        size_t first = 0;
        while (first < e.size() && !e[first].second) first++;
        bool gaps = false;
        for (auto &x : e) gaps |= !x.second && x.first > 0;
        if (first < e.size() && gaps) {
            double pre = 0, total = 0;
            for (size_t k = 0; k < first; k++) pre += e[k].first;
            std::vector<std::pair<double, bool>> r;
            for (size_t k = 0; k < e.size(); k++) {
                auto x = e[(first + k) % e.size()];
                if (!r.empty() && r.back().second == x.second) r.back().first += x.first;
                else r.push_back(x);
            }
            if (r.size() > 1 && r.back().second) {   // ends "on": joined to the first
                pre -= r.back().first;
                r[0].first += r.back().first;
                r.pop_back();
            }
            Dash d;
            for (auto &x : r) {
                d.d.push_back((float)x.first);
                total += x.first;
            }
            if (total > 0 && r.size() % 2 == 0) {
                d.total = (float)total;
                d.offset = (float)std::fmod(total - std::fmod(pre, total) + total, total);
                idx = (int)doc->dashes.size();
                doc->dashes.push_back(std::move(d));
            }
        }
        return P.dashIndex[key] = idx;
    }

    // a new item, or the last one if it can take more of the same (up to 32 path commands:
    // plutovg's time grows faster than a path's size)
    size_t item(uint8_t kind, uint32_t rgba, float width, int dsh) {
        if (!items.empty()) {
            Item &it = items.back();
            if (it.kind == kind && it.rgba == rgba && it.width == width && it.dash == dsh &&
                (kind == O_STROKE || kind == O_FILL || kind == O_POINTS || kind == O_DOTS) && pend.back().cmd.size() < 32)
                return items.size() - 1;
        }
        Item it{};
        it.kind = kind;
        it.rgba = rgba;
        it.width = width;
        it.dash = dsh;
        items.push_back(it);
        pend.emplace_back();
        return items.size() - 1;
    }
    void geo(Pending &pd, const Block &b, const Prim &p, const M2 &m) {
        size_t pi = p.p0;
        for (uint32_t ci = p.c0; ci < p.c1; ci++) {
            uint8_t c = b.geo.cmd[ci];
            pd.cmd.push_back(c);
            int n = c == C_CUBIC ? 3 : c == C_CLOSE ? 0 : 1;
            for (int k = 0; k < n; k++) pd.pt.push_back(m(b.geo.pt[pi++]));
        }
        points += p.p1 - p.p0;
    }

    // (dim: in a dimension's block, whose points are its definition points: always dots, as in AutoCAD)
    void expand(const Block &b, const M2 &m, const Ctx &ctx, int depth, bool dim = false) {
        for (const Prim &p : b.prims) {
            if (points > kMaxPoints || ++work > kMaxWork) return;
            if (depth == 0 && !skip.empty() && skip[(size_t)(&p - b.prims.data())]) continue;
            int layer = p.layer == 0 && depth > 0 ? ctx.layer : p.layer;
            const Layer &L = P.layers[layer];
            if (L.frozen || hidden[layer] || (L.off && p.kind != K_INSERT)) continue;
            uint32_t col = rgb(p.color, layer, ctx);
            uint32_t rgba = 0xff000000u | col;
            if (p.kind == K_INSERT) {
                InsData &in = P.inserts[p.aux];
                if (in.index == -2) {
                    auto it = P.blockIndex.find(in.block);
                    in.index = it == P.blockIndex.end() ? -1 : it->second;
                }
                if (in.index < 0 || depth >= 32 || active[in.index]) continue;
                const Block &child = P.blocks[in.index];
                if (child.xref || child.prims.empty()) continue;
                Ctx cc{layer, col, p.ltype == LT_BYLAYER ? L.ltype : p.ltype == LT_BYBLOCK ? ctx.ltype : p.ltype};
                active[in.index] = 1;
                for (int row = 0; row < in.rows; row++)
                    for (int c = 0; c < in.cols && work <= kMaxWork; c++, work++) {
                        M2 cm = m * translate(in.colStep * c + in.rowStep * row) * in.m * translate(V2{} - child.base);
                        expand(child, cm, cc, depth + 1, dim || in.dimension);
                    }
                active[in.index] = 0;
                continue;
            }
            int lt = p.ltype == LT_BYLAYER ? L.ltype : p.ltype == LT_BYBLOCK ? ctx.ltype : p.ltype;
            switch (p.kind) {
            case K_STROKE: {
                float w = p.width > 0 ? (float)(p.width * std::sqrt(std::fabs(det(m)))) : 0;
                geo(pend[item(O_STROKE, rgba, w, dash(lt, P.ltscale * p.ltscale))], b, p, m);
                break;
            }
            case K_FILL: geo(pend[item(O_FILL, rgba, 0, -1)], b, p, m); break;
            case K_FILL_EO: geo(pend[item(O_FILL_EO, rgba, 0, -1)], b, p, m); break;
            case K_WIPEOUT: geo(pend[item(O_FILL, WIPEOUT, 0, -1)], b, p, m); break;
            case K_POINT: geo(pend[item(dim ? O_DOTS : O_POINTS, rgba, 0, -1)], b, p, m); break;
            case K_XLINE:
            case K_RAY: {
                size_t i = item(p.kind == K_RAY ? O_RAY : O_XLINE, rgba, 0, -1);
                V2 at = m(b.geo.pt[p.p0]), d = m.vec(b.geo.pt[p.p0 + 1]);
                double l = len(d);
                if (!(l > 0)) d = {1, 0}, l = 1;
                pend[i].pt = {at, d * (1 / l)};
                break;
            }
            case K_TEXT: {
                size_t i = item(O_TEXT, rgba, 0, -1);
                items[i].aux = (int)doc->otexts.size();
                doc->otexts.push_back({p.aux, {}, rgba});
                M2 tm = m * doc->texts[p.aux].m;
                textM.push_back(tm);
                const TextData &t = doc->texts[p.aux];
                pend[i].pt = {tm({t.x0, t.y0}), tm({t.x1, t.y0}), tm({t.x1, t.y1}), tm({t.x0, t.y1})};   // (its box, for the extents)
                points += t.glyphs.size();
                break;
            }
            case K_HATCH: {
                size_t i = item(O_HATCH, rgba, 0, -1);
                geo(pend[i], b, p, m);
                items[i].aux = (int)pats.size();
                Pattern pt = P.patterns[p.aux];
                for (PatLine &pl : pt) {
                    pl.base = m(pl.base);
                    pl.off = m.vec(pl.off);
                    V2 d = m.vec(pl.dir);
                    double l = len(d);
                    pl.dir = l > 0 ? d * (1 / l) : V2{1, 0};
                    for (double &x : pl.dashes) x *= l;
                }
                pats.push_back(std::move(pt));
                break;
            }
            }
        }
    }

    // the model space through a viewport, clipped to its rectangle
    void viewport(const Block &ms, const VPort &v, const Ctx &ctx) {
        size_t n = items.size();
        item(O_CLIP, 0, 0, -1);
        pend.back().pt = {{v.r[0], v.r[1]}, {v.r[2], v.r[3]}};
        for (int l : v.frozen) hidden[l] = 1;
        if (&ms == &P.model && !P.idwViews.empty()) {   // (Inventor: its own view only)
            skip.assign(ms.prims.size(), 0);
            for (auto &h : P.idwViews) skip[(size_t)h.second] = 1;
            auto it = std::upper_bound(P.idwViews.begin(), P.idwViews.end(), std::make_pair(v.handle, INT_MAX));
            if (it != P.idwViews.end()) skip[(size_t)it->second] = 0;
        }
        expand(ms, v.m, ctx, 0);
        skip.clear();
        for (int l : v.frozen) hidden[l] = 0;
        if (items.size() == n + 1) {   // (nothing in it)
            items.pop_back();
            pend.pop_back();
        } else
            item(O_UNCLIP, 0, 0, -1);
    }
};

// The shown area: the items' extents, without stray far-off objects (a few items far outside
// where almost all the drawing is, as "zoom extents" in AutoCAD would show them tiny).
void extents(const std::vector<Item> &items, const std::vector<std::array<double, 4>> &box, double out[4]) {
    std::vector<double> cx, cy;
    double all[4] = {1e300, 1e300, -1e300, -1e300};
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].kind == O_XLINE || items[i].kind == O_RAY) continue;
        const auto &b = box[i];
        if (!(b[2] >= b[0])) continue;
        cx.push_back((b[0] + b[2]) / 2);
        cy.push_back((b[1] + b[3]) / 2);
        all[0] = std::min(all[0], b[0]);
        all[1] = std::min(all[1], b[1]);
        all[2] = std::max(all[2], b[2]);
        all[3] = std::max(all[3], b[3]);
    }
    memcpy(out, all, sizeof all);
    if (cx.size() < 20) return;
    auto q = [](std::vector<double> &v, double f) {
        size_t k = std::min(v.size() - 1, (size_t)(f * (double)v.size()));
        std::nth_element(v.begin(), v.begin() + (long)k, v.end());
        return v[k];
    };
    double x0 = q(cx, 0.005), x1 = q(cx, 0.995), y0 = q(cy, 0.005), y1 = q(cy, 0.995);
    double w = std::max(x1 - x0, 1e-9), h = std::max(y1 - y0, 1e-9), s = std::max(w, h);
    if (all[2] - all[0] <= 4 * s && all[3] - all[1] <= 4 * s) return;
    double k[4] = {x0 - s / 2, y0 - s / 2, x1 + s / 2, y1 + s / 2};
    double r[4] = {1e300, 1e300, -1e300, -1e300};
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].kind == O_XLINE || items[i].kind == O_RAY) continue;
        const auto &b = box[i];
        double mx = (b[0] + b[2]) / 2, my = (b[1] + b[3]) / 2;
        if (!(b[2] >= b[0]) || mx < k[0] || mx > k[2] || my < k[1] || my > k[3]) continue;
        r[0] = std::min(r[0], b[0]);
        r[1] = std::min(r[1], b[1]);
        r[2] = std::max(r[2], b[2]);
        r[3] = std::max(r[3], b[3]);
    }
    if (r[2] >= r[0]) memcpy(out, r, sizeof r);
}

// --- drawing -----------------------------------------------------------------

void set_rgba(plutovg_canvas_t *c, uint32_t rgba, float alpha = 1) {
    plutovg_canvas_set_rgba(c, (rgba >> 16 & 255) / 255.0f, (rgba >> 8 & 255) / 255.0f, (rgba & 255) / 255.0f, (rgba >> 24) / 255.0f * alpha);
}

// An item's colour on the background: on white, AutoCAD's white (colour 7) is black and light
// colours are darkened enough to be seen (yellow, light grey ...).
uint32_t on_background(uint32_t rgba, int background) {
    if (rgba == WIPEOUT) return 0xff000000u | BACKGROUNDS[background];
    if (background != 0) return rgba;
    uint32_t r = rgba >> 16 & 255, g = rgba >> 8 & 255, b = rgba & 255, a = rgba & 0xff000000u;
    if ((rgba & 0xffffff) == 0xffffff) return a;
    double lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
    if (lum <= 0.55) return rgba;
    double k = 0.55 / lum;
    return a | (uint32_t)(r * k) << 16 | (uint32_t)(g * k) << 8 | (uint32_t)(b * k);
}

// A hairline's segments as quads one device pixel wide (filled: much cheaper than plutovg's
// stroker, the same with butt caps), its curves cut into lines a fraction of a pixel close.
void hairline_quads(plutovg_path_t *out, const plutovg_path_t *path, float hair, float scale) {
    struct Q {
        plutovg_path_t *out;
        float h, scale, x, y, sx, sy;
        void seg(float x1, float y1) {
            float dx = x1 - x, dy = y1 - y, l = std::sqrt(dx * dx + dy * dy);
            if (l > 0) {
                float nx = -dy / l * h, ny = dx / l * h;
                plutovg_path_move_to(out, x + nx, y + ny);
                plutovg_path_line_to(out, x1 + nx, y1 + ny);
                plutovg_path_line_to(out, x1 - nx, y1 - ny);
                plutovg_path_line_to(out, x - nx, y - ny);
                plutovg_path_close(out);
            }
            x = x1, y = y1;
        }
    } q{out, hair / 2, scale, 0, 0, 0, 0};
    plutovg_path_reset(out);
    plutovg_path_traverse(path, [](void *cl, plutovg_path_command_t cmd, const plutovg_point_t *p, int n) {
        Q &q = *static_cast<Q *>(cl);
        switch (cmd) {
        case PLUTOVG_PATH_COMMAND_MOVE_TO: q.x = q.sx = p[0].x, q.y = q.sy = p[0].y; break;
        case PLUTOVG_PATH_COMMAND_LINE_TO: q.seg(p[0].x, p[0].y); break;
        case PLUTOVG_PATH_COMMAND_CUBIC_TO: {
            float x0 = q.x, y0 = q.y;
            float len = (std::hypot(p[0].x - x0, p[0].y - y0) + std::hypot(p[1].x - p[0].x, p[1].y - p[0].y) + std::hypot(p[2].x - p[1].x, p[2].y - p[1].y)) * q.scale;
            int k = (int)std::min(256.0f, std::max(1.0f, std::ceil(std::sqrt(len))));
            for (int i = 1; i <= k; i++) {
                float t = (float)i / k, u = 1 - t;
                float a = u * u * u, b = 3 * u * u * t, cc = 3 * u * t * t, dd = t * t * t;
                q.seg(a * x0 + b * p[0].x + cc * p[1].x + dd * p[2].x, a * y0 + b * p[0].y + cc * p[1].y + dd * p[2].y);
            }
            break;
        }
        default: q.seg(q.sx, q.sy); break;   // (close)
        }
        (void)n;
    }, &q);
}

// the part [t0, t1] of the line p + t d inside the rectangle; false if none
bool clip_line(double px, double py, double dx, double dy, const double r[4], double &t0, double &t1) {
    double p[4] = {-dx, dx, -dy, dy}, q[4] = {px - r[0], r[2] - px, py - r[1], r[3] - py};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        double t = q[i] / p[i];
        if (p[i] < 0) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
    }
    return t0 <= t1;
}

// a render's "stop now" (nitrodxf_render's cancelled)
struct Cancel {
    int (*fn)(void *);
    void *ctx;
    bool operator()() const { return fn && fn(ctx); }
};

void draw_hatch(plutovg_canvas_t *c, const Item &it, uint32_t rgba, const std::vector<OPatLine> &pat, const double view[4], float scale,
                const Cancel &cancel) {
    double r[4] = {std::max(view[0], (double)it.bx0), std::max(view[1], (double)it.by0), std::min(view[2], (double)it.bx1),
                   std::min(view[3], (double)it.by1)};
    if (r[0] > r[2] || r[1] > r[3]) return;
    float hair = 1 / scale;
    plutovg_path_t *lines = plutovg_path_create();
    size_t segs = 0;
    bool tone = false;
    for (const OPatLine &pl : pat) {
        double nx = -pl.dy, ny = pl.dx, s = pl.ox * nx + pl.oy * ny;
        if (std::fabs(s) * scale < 2.5) {   // (denser than the pixels: a tone instead)
            tone = true;
            continue;
        }
        double nmin = 1e300, nmax = -1e300;
        for (int k = 0; k < 4; k++) {
            double x = r[k & 1 ? 2 : 0] - pl.bx, y = r[k & 2 ? 3 : 1] - pl.by, d = x * nx + y * ny;
            nmin = std::min(nmin, d);
            nmax = std::max(nmax, d);
        }
        double k0 = s > 0 ? std::ceil(nmin / s) : std::ceil(nmax / s), k1 = s > 0 ? std::floor(nmax / s) : std::floor(nmin / s);
        if (k1 - k0 > 20000) {
            tone = true;
            continue;
        }
        double period = 0;
        for (float x : pl.dashes) period += std::fabs(x);
        for (double k = k0; k <= k1 && segs < 500000; k++) {
            double px = pl.bx + pl.ox * k, py = pl.by + pl.oy * k, t0 = -1e300, t1 = 1e300;
            if (!clip_line(px, py, pl.dx, pl.dy, r, t0, t1)) continue;
            if (pl.dashes.empty() || period * scale < 3) {
                plutovg_path_move_to(lines, (float)(px + pl.dx * t0), (float)(py + pl.dy * t0));
                plutovg_path_line_to(lines, (float)(px + pl.dx * t1), (float)(py + pl.dy * t1));
                segs++;
                continue;
            }
            double t = std::floor(t0 / period) * period;
            for (size_t e = 0; t < t1 && segs < 500000; e = (e + 1) % pl.dashes.size()) {
                double l = pl.dashes[e];
                if (l >= 0) {
                    double a = std::max(t, t0), b = std::min(t + std::max(l, (double)hair), t1);
                    if (b > a) {
                        plutovg_path_move_to(lines, (float)(px + pl.dx * a), (float)(py + pl.dy * a));
                        plutovg_path_line_to(lines, (float)(px + pl.dx * b), (float)(py + pl.dy * b));
                        segs++;
                    }
                }
                t += std::fabs(l);
            }
        }
    }
    plutovg_canvas_save(c);
    plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_EVEN_ODD);
    if (tone) {
        set_rgba(c, rgba, 0.4f);
        plutovg_canvas_fill_path(c, it.path);
    }
    if (segs) {
        plutovg_canvas_clip_path(c, it.path);
        set_rgba(c, rgba);
        plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
        // (each line filled alone: plutovg's rasterizer keeps a scanline's cells in a list, so
        // the time grows with the square of the lines crossing the same scanlines in one path)
        const plutovg_path_element_t *el;
        int ne = plutovg_path_get_elements(lines, &el);
        plutovg_path_t *part = plutovg_path_create(), *quads = plutovg_path_create();
        for (int k = 0; k + 3 < ne && !((k & 63) == 60 && cancel()); k += 4) {   // (move_to, point, line_to, point)
            plutovg_path_reset(part);
            plutovg_path_move_to(part, el[k + 1].point.x, el[k + 1].point.y);
            plutovg_path_line_to(part, el[k + 3].point.x, el[k + 3].point.y);
            hairline_quads(quads, part, hair, scale);
            plutovg_canvas_fill_path(c, quads);
        }
        plutovg_path_destroy(part);
        plutovg_path_destroy(quads);
    }
    plutovg_canvas_restore(c);
    plutovg_path_destroy(lines);
}

// A page's display list: boxes, the shown area, then float paths around its centre. False if
// it shows nothing.
bool finish_page(Flat &F, Page &pg) {
    nitrodxf *doc = F.doc;
    if (pg.items.empty()) return false;
    std::vector<std::array<double, 4>> box(pg.items.size()), raw(pg.items.size());   // (raw: not clipped)
    double clip[4] = {};
    bool clipped = false;
    for (size_t i = 0; i < pg.items.size(); i++) {
        double b[4] = {1e300, 1e300, -1e300, -1e300};
        Item &it = pg.items[i];
        auto &pts = F.pend[i].pt;
        size_t n = it.kind == O_XLINE || it.kind == O_RAY ? std::min<size_t>(1, pts.size()) : pts.size();
        bool bad = !(it.width < 1e15f);
        for (size_t k = 0; k < n && !bad; k++) bad = !finite(pts[k]);
        if (bad) {   // (out of any drawing, a damaged file's block references or viewports: left out)
            it.kind = O_FILL;
            it.width = 0;
            F.pend[i].cmd.clear();
            pts.clear();
            box[i] = raw[i] = {1e300, 1e300, -1e300, -1e300};
            continue;
        }
        for (size_t k = 0; k < n; k++) {
            b[0] = std::min(b[0], pts[k].x);
            b[1] = std::min(b[1], pts[k].y);
            b[2] = std::max(b[2], pts[k].x);
            b[3] = std::max(b[3], pts[k].y);
        }
        double h = it.width / 2;
        box[i] = raw[i] = {b[0] - h, b[1] - h, b[2] + h, b[3] + h};
        if (it.kind == O_CLIP) {   // (its rectangle: not in the extents; what is drawn in it, clipped)
            memcpy(clip, b, sizeof clip);
            box[i] = {1e300, 1e300, -1e300, -1e300};
            clipped = true;
        } else if (it.kind == O_UNCLIP)
            clipped = false;
        else if (clipped)
            box[i] = {std::max(box[i][0], clip[0]), std::max(box[i][1], clip[1]), std::min(box[i][2], clip[2]), std::min(box[i][3], clip[3])};
    }
    double ext[4];
    extents(pg.items, box, ext);
    if (!(ext[2] >= ext[0] && ext[3] >= ext[1])) return false;
    double w = ext[2] - ext[0], h = ext[3] - ext[1], s = std::max(w, h);
    if (!(s > 0)) s = 1;
    double margin = 0.015 * s;
    w = std::max(w, s / 1000);   // (a single line: not a zero-width image)
    h = std::max(h, s / 1000);
    pg.ox = (ext[0] + ext[2]) / 2;
    pg.oy = (ext[1] + ext[3]) / 2;
    pg.ax0 = -w / 2 - margin;
    pg.ax1 = w / 2 + margin;
    pg.ay0 = -h / 2 - margin;
    pg.ay1 = h / 2 + margin;
    double ox = pg.ox, oy = pg.oy;
    for (size_t i = 0; i < pg.items.size(); i++) {
        Item &it = pg.items[i];
        Pending &pd = F.pend[i];
        it.bx0 = (float)(raw[i][0] - ox);
        it.by0 = (float)(raw[i][1] - oy);
        it.bx1 = (float)(raw[i][2] - ox);
        it.by1 = (float)(raw[i][3] - oy);
        if (it.kind == O_CLIP) {
            it.bx0 = (float)(pd.pt[0].x - ox), it.by0 = (float)(pd.pt[0].y - oy);
            it.bx1 = (float)(pd.pt[1].x - ox), it.by1 = (float)(pd.pt[1].y - oy);
        } else if (it.kind == O_UNCLIP) {
        } else if (it.kind == O_XLINE || it.kind == O_RAY) {
            it.aux = (int)doc->xlines.size() / 4;
            for (float v : {(float)(pd.pt[0].x - ox), (float)(pd.pt[0].y - oy), (float)pd.pt[1].x, (float)pd.pt[1].y}) doc->xlines.push_back(v);
        } else if (it.kind == O_TEXT) {
            const M2 &m = F.textM[(size_t)it.aux - F.otext0];
            float *fm = doc->otexts[it.aux].m;
            fm[0] = (float)m.a, fm[1] = (float)m.b, fm[2] = (float)m.c, fm[3] = (float)m.d, fm[4] = (float)(m.e - ox), fm[5] = (float)(m.f - oy);
        } else {
            it.path = plutovg_path_create();
            if (it.kind == O_POINTS || it.kind == O_DOTS) {
                for (const V2 &q : pd.pt) plutovg_path_move_to(it.path, (float)(q.x - ox), (float)(q.y - oy));
            } else {
                plutovg_path_reserve(it.path, (int)pd.pt.size() + (int)pd.cmd.size());
                size_t pi = 0;
                for (uint8_t c : pd.cmd) {
                    const V2 *q = &pd.pt[pi];
                    switch (c) {
                    case C_MOVE: plutovg_path_move_to(it.path, (float)(q[0].x - ox), (float)(q[0].y - oy)), pi++; break;
                    case C_LINE: plutovg_path_line_to(it.path, (float)(q[0].x - ox), (float)(q[0].y - oy)), pi++; break;
                    case C_CUBIC:
                        plutovg_path_cubic_to(it.path, (float)(q[0].x - ox), (float)(q[0].y - oy), (float)(q[1].x - ox), (float)(q[1].y - oy),
                                              (float)(q[2].x - ox), (float)(q[2].y - oy));
                        pi += 3;
                        break;
                    default: plutovg_path_close(it.path);
                    }
                }
            }
            if (it.kind == O_HATCH) {
                std::vector<OPatLine> op;
                for (const PatLine &pl : F.pats[it.aux]) {
                    OPatLine o{(float)(pl.base.x - ox), (float)(pl.base.y - oy), (float)pl.dir.x, (float)pl.dir.y, (float)pl.off.x, (float)pl.off.y, {}};
                    for (double d : pl.dashes) o.dashes.push_back((float)d);
                    op.push_back(std::move(o));
                }
                it.aux = (int)doc->opatterns.size();
                doc->opatterns.push_back(std::move(op));
            }
        }
        Pending().cmd.swap(pd.cmd);
        Pending().pt.swap(pd.pt);
    }
    return true;
}

// A page's display list as a hash (FNV-1a): the same pages (layouts showing the same) are kept once.
uint64_t page_hash(const nitrodxf *doc, const Page &pg) {
    uint64_t h = 14695981039346656037ull;
    auto add = [&](const void *p, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t *>(p)[i]) * 1099511628211ull;
    };
    double area[6] = {pg.ax0, pg.ay0, pg.ax1, pg.ay1, pg.ox, pg.oy};
    add(area, sizeof area);
    for (const Item &it : pg.items) {
        add(&it.kind, 1), add(&it.rgba, 4), add(&it.width, 4), add(&it.dash, 4);
        if (it.kind == O_CLIP) add(&it.bx0, 4), add(&it.by0, 4), add(&it.bx1, 4), add(&it.by1, 4);
        else if (it.kind == O_TEXT) {
            const OText &t = doc->otexts[(size_t)it.aux];
            add(&t.text, sizeof t.text), add(t.m, sizeof t.m);
        } else if (it.kind == O_XLINE || it.kind == O_RAY)
            add(&doc->xlines[(size_t)it.aux * 4], 4 * sizeof(float));
        if (it.path) {
            const plutovg_path_element_t *el;
            int n = plutovg_path_get_elements(it.path, &el);
            add(el, (size_t)n * sizeof *el);
        }
        if (it.kind == O_HATCH)
            for (const OPatLine &pl : doc->opatterns[(size_t)it.aux]) {
                add(&pl, 6 * sizeof(float));
                add(pl.dashes.data(), pl.dashes.size() * sizeof(float));
            }
    }
    return h;
}

} // namespace

nitrodxf *nitrodxf_load(const void *data, size_t len) {
    const char *p = static_cast<const char *>(data);
    if (!p || len < 8 || (len >= 18 && !memcmp(p, "AutoCAD Binary DXF", 18))) return nullptr;
    Reader rd{p, p + len};
    nitrodwg_pairs dwg;   // (a DWG: its pairs, read from here)
    if (!memcmp(p, "AC10", 4)) {
        if (!nitrodwg_read(data, len, dwg)) return nullptr;
        rd.dwg = &dwg;
    }
    if (!rd.next() || (rd.cur.code != 0 && rd.cur.code != 999)) return nullptr;
    std::unique_ptr<nitrodxf> doc(new nitrodxf);
    Parser P;
    P.doc = doc.get();
    std::string section;
    Rec r, poly;
    std::vector<Rec> verts;
    bool inPoly = false, sections = false, polyPaper = false;
    int curBlock = -1;
    auto target = [&](bool paper) -> Block * {
        if (section == "BLOCKS") return curBlock >= 0 ? &P.blocks[curBlock] : nullptr;
        if (section == "ENTITIES" || !sections) return paper ? &P.paper : &P.model;
        return nullptr;
    };
    auto flushPoly = [&]() {
        if (!inPoly) return;
        inPoly = false;
        if (Block *b = target(polyPaper)) P.entity(poly, *b, &verts);
    };
    while (rd.ok) {
        if (rd.cur.code != 0) {
            rd.next();
            continue;
        }
        r.type = upper(rd.cur.v);
        r.g.clear();
        bool embedded = false;   // (an embedded object of R2018 MTEXT / ATTRIB: its groups repeat the entity's)
        while (rd.next() && rd.cur.code != 0)
            if (rd.cur.code == 101) embedded = true;
            else if (!embedded) r.g.push_back(rd.cur);
        if (r.type == "VERTEX" && inPoly) {
            if (verts.size() < 10000000) verts.push_back(r);
            continue;
        }
        flushPoly();
        if (r.type == "SEQEND") continue;
        if (r.type == "SECTION") {
            section = upper(r.s(2));
            sections = true;
            if (section == "HEADER") {
                P.header(r);
                P.utf8 = P.version >= 1021;
            }
            continue;
        }
        if (r.type == "ENDSEC") {
            section.clear();
            continue;
        }
        if (r.type == "EOF") break;
        if (section == "TABLES") {
            if (r.type != "TABLE" && r.type != "ENDTAB") P.tableEntry(r);
            continue;
        }
        if (section == "BLOCKS") {
            if (r.type == "BLOCK") {
                std::string name = upper(r.s(2));
                Block nb;
                nb.base = r.p2(10);
                nb.xref = r.i(70) & 4;
                curBlock = (int)P.blocks.size();
                P.blocks.push_back(std::move(nb));
                P.blockIndex[name] = curBlock;
                continue;
            }
            if (r.type == "ENDBLK") {
                curBlock = -1;
                continue;
            }
        }
        if (section == "OBJECTS") {
            if (r.type == "LAYOUT") P.layout(r);
            continue;
        }
        bool paper = r.i(67) == 1;
        if (r.type == "VIEWPORT") {
            if (Block *b = target(paper); b && b != &P.model) P.viewport(r, *b, b == &P.paper);
            continue;
        }
        if (r.type == "POLYLINE") {
            poly = r;
            verts.clear();
            inPoly = true;
            polyPaper = paper;
            continue;
        }
        Block *b = target(paper);
        if (!b) continue;
        size_t n = b->prims.size();
        P.entity(r, *b, nullptr);
        if (b == &P.model && r.type == "INSERT" && b->prims.size() == n + 1 && upper(r.s(8)) == "*IDW_BLOCKREFERENCELAYER")
            P.idwViews.push_back({strtoull(std::string(r.s(5)).c_str(), nullptr, 16), (int)n});
    }
    flushPoly();   // (a file of entities alone, without sections: they were read into the model space)

    // The pages: the model space, then the layouts in their tabs' order (a layout's sheet: its
    // paper space, over the model space in its viewports). Empty ones and repeated ones are left
    // out, and so is an Inventor drawing's model space (its views piled up, for the viewports)
    // when a sheet shows.
    auto named = [&](const std::string &n) -> const Block * {
        auto it = P.blockIndex.find(n);
        return it == P.blockIndex.end() ? nullptr : &P.blocks[it->second];
    };
    const Block *ms = named("*MODEL_SPACE"), *ps = named("*PAPER_SPACE");
    if (!P.model.prims.empty() || !ms) ms = &P.model;
    const Block *paper = P.paper.prims.empty() && P.paper.vports.empty() && ps ? ps : &P.paper;
    bool inventor = ms == &P.model && !P.idwViews.empty() && P.idwViews.size() == P.model.prims.size();
    std::vector<const Block *> srcs;   // (nullptr: the model space)
    if (!inventor) srcs.push_back(nullptr);
    std::stable_sort(P.layouts.begin(), P.layouts.end(), [](const Parser::Layout &a, const Parser::Layout &b) { return a.tab < b.tab; });
    for (const Parser::Layout &l : P.layouts) {
        auto it = P.blockNames.find(l.record);
        if (it == P.blockNames.end() || it->second == "*MODEL_SPACE") continue;
        const Block *b = it->second == "*PAPER_SPACE" ? paper : named(it->second);
        if (b && std::find(srcs.begin(), srcs.end(), b) == srcs.end()) srcs.push_back(b);
    }
    if (std::find(srcs.begin(), srcs.end(), paper) == srcs.end()) srcs.push_back(paper);   // (up to R14 no layouts: one)
    if (inventor) srcs.push_back(nullptr);   // (only if no sheet shows)
    std::sort(P.idwViews.begin(), P.idwViews.end());
    size_t points = 0, work = 0;
    std::vector<uint64_t> hashes;   // (the pages': 0 until one could be the same as another)
    auto same = [&](const Page &a, const Page &b) {
        return a.items.size() == b.items.size() && a.ax0 == b.ax0 && a.ay0 == b.ay0 && a.ax1 == b.ax1 && a.ay1 == b.ay1 && a.ox == b.ox && a.oy == b.oy;
    };
    for (const Block *src : srcs) {
        if (doc->pages.size() >= 64 || (!src && inventor && !doc->pages.empty())) break;
        Page pg;
        Flat F(P, doc.get(), pg.items, points, work);
        Flat::Ctx ctx{0, aci_rgb(7), LT_CONTINUOUS};
        if (src) {
            for (const VPort &v : src->vports) F.viewport(*ms, v, ctx);
            F.expand(*src, M2(), ctx, 0);
        } else
            F.expand(*ms, M2(), ctx, 0);
        points = F.points, work = F.work;
        bool keep = finish_page(F, pg);
        uint64_t h = 0;
        for (size_t i = 0; keep && i < doc->pages.size(); i++)   // (hashed only to tell pages alike in size apart)
            if (same(doc->pages[i], pg)) {
                if (!hashes[i]) hashes[i] = page_hash(doc.get(), doc->pages[i]) | 1;
                if (!h) h = page_hash(doc.get(), pg) | 1;
                keep = h != hashes[i];
            }
        if (keep) {
            hashes.push_back(h);
            doc->pages.push_back(std::move(pg));
        } else
            for (Item &it : pg.items) plutovg_path_destroy(it.path);
    }
    if (doc->pages.empty()) return nullptr;
    return doc.release();
}

void nitrodxf_free(nitrodxf *dxf) { delete dxf; }

int nitrodxf_pages(const nitrodxf *dxf) { return dxf ? (int)dxf->pages.size() : 0; }

void nitrodxf_set_background(nitrodxf *dxf, int background) {
    if (dxf && background >= 0 && background <= 2) dxf->background = background;
}

void nitrodxf_size(const nitrodxf *dxf, int page, double *w, double *h) {
    *w = *h = 0;
    if (!dxf || page < 0 || page >= (int)dxf->pages.size()) return;
    const Page &pg = dxf->pages[(size_t)page];
    *w = pg.ax1 - pg.ax0;
    *h = pg.ay1 - pg.ay0;
}

static constexpr double kFar = 1 << 24;   // device pixels out of the view

int nitrodxf_render(nitrodxf *dxf, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                    int (*cancelled)(void *ctx), void *ctx) {
    if (!dxf || page < 0 || page >= (int)dxf->pages.size() || w <= 0 || h <= 0 || pitch < (size_t)w * 4 || !(scale > 0)) return -1;
    const Page &pg = dxf->pages[(size_t)page];
    uint32_t bg = 0xff000000u | BACKGROUNDS[dxf->background];
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(bgra + (size_t)y * pitch);
        for (int x = 0; x < w; x++) row[x] = bg;
    }
    plutovg_surface_t *surface = plutovg_surface_create_for_data(bgra, w, h, (int)pitch);
    plutovg_canvas_t *c = plutovg_canvas_create(surface);
    double vx0 = pg.ax0 + x0, vy1 = pg.ay1 - y0;   // the view in drawing units (around the origin)
    plutovg_matrix_t mt = {(float)scale, 0, 0, (float)-scale, (float)(-scale * vx0), (float)(scale * vy1)};
    plutovg_canvas_set_matrix(c, &mt);
    float hair = (float)(1 / scale), fs = (float)scale;
    double view[4] = {vx0 - 2 * hair, vy1 - h / scale - 2 * hair, vx0 + w / scale + 2 * hair, vy1 + 2 * hair};
    plutovg_path_t *tmp = plutovg_path_create();
    std::vector<float> dashes;
    plutovg_canvas_set_line_cap(c, PLUTOVG_LINE_CAP_BUTT);
    float clip[4] = {-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX};   // (the viewport drawn in)
    Cancel cancel{cancelled, ctx};
    size_t n = 0;
    int rc = 0;
    for (const Item &it : pg.items) {
        if ((++n & 63) == 0 && cancel()) {
            rc = 1;
            break;
        }
        if (it.kind == O_CLIP) clip[0] = it.bx0, clip[1] = it.by0, clip[2] = it.bx1, clip[3] = it.by1;
        else if (it.kind == O_UNCLIP) clip[0] = clip[1] = -FLT_MAX, clip[2] = clip[3] = FLT_MAX;
        else if (it.kind != O_XLINE && it.kind != O_RAY &&
                 (std::min(it.bx1, clip[2]) < view[0] || std::max(it.bx0, clip[0]) > view[2] || std::min(it.by1, clip[3]) < view[1] ||
                  std::max(it.by0, clip[1]) > view[3] ||
                  std::max(view[0] - it.bx0, std::max(it.bx1 - view[2], std::max(view[1] - it.by0, it.by1 - view[3]))) * scale > kFar))
            continue;   // (kFar: plutovg's fixed point would overflow; far-off strays of a damaged file)
        uint32_t rgba = on_background(it.rgba, dxf->background);
        switch (it.kind) {
        case O_STROKE: {
            set_rgba(c, rgba);
            const Dash *d = it.dash >= 0 ? &dxf->dashes[it.dash] : nullptr;
            bool dashed = d && d->total * fs >= 4;   // (finer than that: drawn solid)
            if (it.width <= hair && !dashed) {
                hairline_quads(tmp, it.path, hair, fs);
                plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
                plutovg_canvas_fill_path(c, tmp);
                break;
            }
            plutovg_canvas_set_line_width(c, std::max(it.width, hair));
            plutovg_canvas_set_line_join(c, it.width > hair ? PLUTOVG_LINE_JOIN_MITER : PLUTOVG_LINE_JOIN_BEVEL);
            if (dashed) {
                dashes.assign(d->d.begin(), d->d.end());
                for (size_t k = 0; k < dashes.size(); k += 2) dashes[k] = std::max(dashes[k], hair);
                plutovg_canvas_set_dash(c, d->offset, dashes.data(), (int)dashes.size());
            } else
                plutovg_canvas_set_dash(c, 0, nullptr, 0);
            plutovg_canvas_stroke_path(c, it.path);
            break;
        }
        case O_FILL:
        case O_FILL_EO:
            set_rgba(c, rgba);
            plutovg_canvas_set_fill_rule(c, it.kind == O_FILL_EO ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
            plutovg_canvas_fill_path(c, it.path);
            break;
        case O_HATCH: {   // (its lines generated in the viewport's part of the view only)
            double cv[4] = {std::max(view[0], (double)clip[0]), std::max(view[1], (double)clip[1]), std::min(view[2], (double)clip[2]),
                            std::min(view[3], (double)clip[3])};
            draw_hatch(c, it, rgba, dxf->opatterns[it.aux], cv, fs, cancel);
            break;
        }
        case O_POINTS:
        case O_DOTS: {   // as $PDMODE: 0 a dot, 1 nothing, 2 +, 3 x, 4 a tick; +32 a circle, +64 a square
            int mode = it.kind == O_DOTS ? 0 : dxf->pdmode;
            if (mode == 1) break;
            struct Pts {
                plutovg_path_t *p;
                int mode;
                float r, hair;
            } pc{tmp, mode, 0, hair};
            double size = dxf->pdsize > 0 ? dxf->pdsize : (dxf->pdsize < 0 ? -dxf->pdsize / 100 : 0.05) * h / scale;
            pc.r = (float)(size / 2);
            plutovg_path_reset(tmp);
            plutovg_path_traverse(it.path, [](void *cl, plutovg_path_command_t cmd, const plutovg_point_t *pts, int n) {
                const Pts *a = static_cast<const Pts *>(cl);
                if (cmd != PLUTOVG_PATH_COMMAND_MOVE_TO || n < 1) return;
                float x = pts[0].x, y = pts[0].y, r = a->r;
                int m = a->mode & 7;
                if (m == 0) {   // (a one pixel long hairline: a dot)
                    plutovg_path_move_to(a->p, x - a->hair / 2, y);
                    plutovg_path_line_to(a->p, x + a->hair / 2, y);
                } else if (m == 2 || m == 3) {
                    float d = m == 3 ? r * 0.7071f : r;
                    plutovg_path_move_to(a->p, x - d, y - (m == 3 ? d : 0));
                    plutovg_path_line_to(a->p, x + d, y + (m == 3 ? d : 0));
                    plutovg_path_move_to(a->p, x - (m == 3 ? d : 0), y + d);
                    plutovg_path_line_to(a->p, x + (m == 3 ? d : 0), y - d);
                } else if (m == 4) {
                    plutovg_path_move_to(a->p, x, y);
                    plutovg_path_line_to(a->p, x, y + r);
                }
                if (a->mode & 32) plutovg_path_add_circle(a->p, x, y, r);
                if (a->mode & 64) plutovg_path_add_rect(a->p, x - r, y - r, 2 * r, 2 * r);
            }, &pc);
            set_rgba(c, rgba);
            plutovg_canvas_set_line_width(c, hair);
            plutovg_canvas_set_dash(c, 0, nullptr, 0);
            plutovg_canvas_stroke_path(c, tmp);
            break;
        }
        case O_TEXT: {
            const OText &ot = dxf->otexts[it.aux];
            const TextData &t = dxf->texts[ot.text];
            const float *m = ot.m;
            float k = std::sqrt(std::fabs(m[0] * m[3] - m[1] * m[2]));
            plutovg_canvas_set_fill_rule(c, PLUTOVG_FILL_RULE_NON_ZERO);
            for (const GRun &run : t.runs) {
                if (run.size * k * fs < 1.5f) continue;   // (smaller than a pixel or two: not drawn)
                plutovg_path_reset(tmp);
                for (uint32_t g = run.g0; g < run.g1; g++) {
                    const GPos &gp = t.glyphs[g];
                    // glyph (em, y down) -> text space -> drawing
                    float a = run.size * run.sx, cc = -run.size * run.skew, d = -run.size;
                    plutovg_matrix_t gm = {m[0] * a, m[1] * a, m[0] * cc + m[2] * d, m[1] * cc + m[3] * d,
                                           m[0] * gp.x + m[2] * gp.y + m[4], m[1] * gp.x + m[3] * gp.y + m[5]};
                    plutovg_path_add_path(tmp, dxf->glyphs[gp.glyph].path, &gm);
                }
                uint32_t col = run.color == 0 ? ot.rgba : 0xff000000u | ((run.color & 0xff000000u) == COL_RGB ? run.color & 0xffffff : aci_rgb((int)(run.color & 255)));
                set_rgba(c, on_background(col, dxf->background));
                plutovg_canvas_fill_path(c, tmp);
            }
            break;
        }
        case O_CLIP:
            plutovg_canvas_save(c);
            plutovg_canvas_clip_rect(c, it.bx0, it.by0, it.bx1 - it.bx0, it.by1 - it.by0);
            break;
        case O_UNCLIP: plutovg_canvas_restore(c); break;
        case O_XLINE:
        case O_RAY: {
            const float *x = &dxf->xlines[(size_t)it.aux * 4];
            double t0 = it.kind == O_RAY ? 0 : -1e300, t1 = 1e300;
            if (!clip_line(x[0], x[1], x[2], x[3], view, t0, t1)) break;
            plutovg_path_reset(tmp);
            plutovg_path_move_to(tmp, (float)(x[0] + x[2] * t0), (float)(x[1] + x[3] * t0));
            plutovg_path_line_to(tmp, (float)(x[0] + x[2] * t1), (float)(x[1] + x[3] * t1));
            set_rgba(c, rgba);
            plutovg_canvas_set_line_width(c, hair);
            plutovg_canvas_set_dash(c, 0, nullptr, 0);
            plutovg_canvas_stroke_path(c, tmp);
            break;
        }
        }
    }
    plutovg_path_destroy(tmp);
    plutovg_canvas_destroy(c);
    plutovg_surface_destroy(surface);
    return rc || cancel();   // (a hatch may have stopped)
}
