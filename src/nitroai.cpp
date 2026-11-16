// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitroai: the Adobe Illustrator documents before version 9 (Illustrator 9 and later save a PDF).
// They are PostScript programs, but the drawing is written in a documented subset ("Adobe Illustrator
// File Format Specification", Adobe, 1998): operators of its own, defined by the procsets of the prolog,
// which a reader takes as they are named, without the prolog. This one reads them into the content
// stream of a PDF page (paths, fills and strokes, the colours: grey, CMYK, RGB, custom ones with their
// tints; clipping groups, compound paths, the fill rule; linear and radial gradients as shadings,
// patterns as tiling patterns, raster images, text with its fonts re-encoded as the file says), and
// nitropdf draws that page: it has the rest (system fonts for the ones named, CMYK, rendering).
// The page: the document's bounding box. Not read: placed (linked) art, graphs' data, multi-layer
// masks (Illustrator 5), objects of plug-ins (their art is in the file as plain paths too).
#include "nitroai.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

const double DEG = 3.14159265358979323846 / 180;   // (M_PI is not standard: mingw has none)

// --- PostScript objects, as much as the format uses -------------------------------------------------

struct Obj {
    enum T : uint8_t { NUM, NAME, STR, MARK, ARR, TILEFILL } t = NUM;
    double n = 0;
    std::string s;
    std::vector<Obj> a;
    int nest = 0;   // an array's: how deep the arrays in it go
};
const int MAX_NEST = 32;   // (the format's arrays: a matrix, a dash pattern, a pattern's layers, an encoding)

struct Mtx {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};
Mtx mul(const Mtx &A, const Mtx &B) {   // A, then B
    return {A.a * B.a + A.b * B.c, A.a * B.b + A.b * B.d, A.c * B.a + A.d * B.c, A.c * B.b + A.d * B.d,
            A.e * B.a + A.f * B.c + B.e, A.e * B.b + A.f * B.d + B.f};
}
Mtx rotate(double deg) {
    double r = deg * DEG, c = cos(r), s = sin(r);
    return {c, s, -s, c, 0, 0};
}

std::string fmt(double v) {
    if (!std::isfinite(v)) v = 0;
    v = std::max(-1e9, std::min(1e9, v));
    char b[64];
    snprintf(b, sizeof b, "%.5f", v);
    // (printf writes the locale's decimal point, a ',' in many: the digits before it and the five after it are kept)
    size_t n = strlen(b), i = b[0] == '-';
    while (b[i] >= '0' && b[i] <= '9') i++;
    if (n >= i + 6) b[i] = '.', memmove(b + i + 1, b + n - 5, 6);
    char *e = b + strlen(b);
    while (e > b && e[-1] == '0') e--;
    if (e > b && e[-1] == '.') e--;
    *e = 0;
    if (!*b || !strcmp(b, "-")) return "0";
    if (!strcmp(b, "-0")) return "0";
    return b;
}
std::string fmt(const Mtx &m) {
    return fmt(m.a) + " " + fmt(m.b) + " " + fmt(m.c) + " " + fmt(m.d) + " " + fmt(m.e) + " " + fmt(m.f);
}
std::string hex(const std::string &s) {
    static const char H[] = "0123456789ABCDEF";
    std::string r = "<";
    for (unsigned char c : s) r += H[c >> 4], r += H[c & 15];
    return r + ">";
}
std::string pdfName(const std::string &s) {
    std::string r = "/";
    for (unsigned char c : s) {
        if (c < 0x21 || c > 0x7e || strchr("()<>[]{}/%#", c)) {
            char b[4];
            snprintf(b, sizeof b, "#%02X", c);
            r += b;
        } else
            r += (char)c;
    }
    return r.size() > 1 ? r : "/_";
}
std::string stream(const std::string &dict, const std::string &data) {
    return "<<" + dict + "/Length " + std::to_string(data.size()) + ">>stream\n" + data + "\nendstream";
}
int ival(double v) { return v >= 2147483647.0 ? INT32_MAX : v <= -2147483647.0 ? -INT32_MAX : v == v ? (int)v : 0; }
double clamp01(double v) { return std::isfinite(v) ? std::max(0.0, std::min(1.0, v)) : 0; }

// --- the PDF being made ---------------------------------------------------------------------------

struct Stop {
    double pos, mid;
    int sp;   // 0 grey, 1 CMYK, 2 RGB
    double c[4];
};
struct Grad {
    bool radial = false;
    std::vector<Stop> stops;
    int fn = 0, n = 0;   // its function's object, the components
};
struct Pattern {
    double x0, y0, x1, y1;
    std::string content;
    int form = 0;   // the tile as a form, once it is used: the patterns made of it (one for each matrix) draw that
};
// a colour, as a PDF content stream sets it for fills and for strokes
struct Colour {
    std::string f = "0 g", s = "0 G";
    Colour() {}
    Colour(const std::string &v, const char *fop, const char *sop) : f(v + " " + fop), s(v + " " + sop) {}
};
struct Reenc {   // a font re-encoded (TZ)
    std::string base;
    std::vector<Obj> pairs;
    bool useStd = true;   // the TE encoding first
};

struct Pdf {
    std::vector<std::string> objs;
    int add(std::string s) {
        objs.push_back(std::move(s));
        return (int)objs.size();
    }
    std::string fonts, shadings, patterns, xobjects;   // the resources' entries
    std::map<std::string, std::string> fontRes, patRes;
    std::map<std::string, Grad> grads;
    std::map<std::string, Pattern> pats;
    std::vector<Obj> te;
    std::map<std::string, Reenc> tz;
    int resObj = 4, nSh = 0, nIm = 0;
    size_t padded = 0;   // image bytes made up so far (see PAD_MAX)
};

// An image whose data is cut short is shown with what it has, the rest blank; all the images of a
// document get this much of such filling together (a damaged file names sizes it has no data for:
// megabytes for every few bytes of it, otherwise).
const size_t PAD_MAX = 16u << 20;

// --- the interpreter --------------------------------------------------------------------------------

bool isDelim(char c) { return strchr("()<>[]{}/%", c) != nullptr; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0; }
int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool number(const char *b, const char *e, double &v) {   // (no locale: the files' numbers are plain)
    const char *p = b;
    bool neg = false;
    if (p < e && (*p == '-' || *p == '+')) neg = *p++ == '-';
    double r = 0;
    int digits = 0;
    while (p < e && *p >= '0' && *p <= '9') r = r * 10 + (*p++ - '0'), digits++;
    if (p < e && *p == '.') {
        p++;
        double s = 0.1;
        while (p < e && *p >= '0' && *p <= '9') r += (*p++ - '0') * s, s /= 10, digits++;
    }
    if (!digits) return false;
    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        bool en = false;
        if (p < e && (*p == '-' || *p == '+')) en = *p++ == '-';
        int x = 0, xd = 0;
        while (p < e && *p >= '0' && *p <= '9') x = std::min(x * 10 + (*p++ - '0'), 400), xd++;
        if (!xd) return false;
        r *= pow(10.0, en ? -x : x);
    }
    if (p != e) return false;
    v = neg ? -r : r;
    return true;
}

const char *findLine(const char *p, const char *e, const char *marker) {   // a line starting with marker
    size_t n = strlen(marker);
    for (const char *q = p; q + n <= e; q++) {
        q = (const char *)memchr(q, marker[0], e - q);
        if (!q || q + n > e) return e;
        if ((q == p || q[-1] == '\n' || q[-1] == '\r') && !memcmp(q, marker, n)) return q;
    }
    return e;
}

struct Run {
    Pdf &D;
    std::string &out;
    const char *p = nullptr, *e = nullptr;
    const char *prologEnd = nullptr;   // the next "%%EndProlog" line of the text being read (e: none), once looked for
    int depth;
    bool draw;   // in the script body (the setup only defines)
    bool stop = false;
    std::vector<Obj> st;
    std::vector<bool> layers;
    int hidden = 0;
    std::string path;
    bool eo = false, clip = false;
    int pola = 0;   // compound paths open
    char crender = 0;
    // The paint styles. The procsets keep the fill's and the stroke's colour in variables (the colour
    // operators set those), and PostScript's one current colour is loaded from one of them when a
    // fill or a stroke needs it and it is not that one already (their _lp: the last paint). So q / Q
    // (gsave / grestore) do not bring a variable back, but they do bring back the loaded colour: an
    // object painted after a Q without a colour operator has the colour that was current at the q,
    // if the last paint in the group was of its own kind (the variable is thought loaded, and is not
    // loaded again); the group's colour otherwise. Illustrator sets the colour again after a Q.
    Colour fill, stroke;         // the variables
    Colour gcol;                 // the loaded colour
    int lp = 0;                  // which it is: 0 neither (a colour operator came since), 1 the fill's, 2 the stroke's
    std::vector<Colour> saved;   // gcol at each q
    std::string efill, estroke;  // what the PDF's graphics state has
    int qd = 0;
    // a gradient instance (Bb ... BB)
    bool grad = false, hasBh = false;
    std::string gname, gpath;
    double g[10] = {}, bh[4] = {};
    bool hasXm = false, hasBm = false, gradLater = false;
    int gflag = 1;   // Bg's: 0 no clip, 1 clip, 2 not rendered
    double ext[4] = {HUGE_VAL, HUGE_VAL, -HUGE_VAL, -HUGE_VAL};   // the path's points' extent
    bool artboard = false;   // a document's artboard (ab), and what was drawn on it and off it
    double ab[4] = {};
    long onBoard = 0, offBoard = 0;
    int bbFlag = 0;
    Mtx xm, bm;
    Grad *gdef = nullptr;   // a gradient being defined
    // text
    bool bt = false;
    Mtx tlm;
    double vscale = 1, hscale = 100, fsize = 0, leading = 0;
    std::string fres;   // the font's resource
    int tr = 0;

    Run(Pdf &D, std::string &out, int depth, bool draw) : D(D), out(out), depth(depth), draw(draw) {}
    bool emit() const { return draw && hidden == 0; }

    size_t base() const {
        for (size_t k = st.size(); k-- > 0;)
            if (st[k].t == Obj::MARK) return k + 1;
        return 0;
    }
    size_t nargs() const { return st.size() - base(); }
    const Obj *arg(size_t k) const {   // k-th from the top
        return k < nargs() ? &st[st.size() - 1 - k] : nullptr;
    }
    double num(size_t k) const {
        const Obj *o = arg(k);
        return o && o->t == Obj::NUM ? o->n : 0;
    }
    int inum(size_t k) const { return ival(num(k)); }
    bool nums(size_t from, size_t count) const {
        for (size_t k = from; k < from + count; k++)
            if (!arg(k) || arg(k)->t != Obj::NUM) return false;
        return true;
    }

    // ---- the tokens

    void feed(const char *b, const char *end) {
        p = b, e = end, prologEnd = nullptr;
        while (p < e && !stop) {
            while (p < e && isSpace(*p)) p++;
            if (p >= e) break;
            char ch = *p;
            if (ch == '%') comment();
            else if (ch == '(') st.push_back(literal());
            else if (ch == '<') {
                if (p + 1 < e && p[1] == '<') p += 2;
                else st.push_back(hexString());
            } else if (ch == '>' || ch == ')' || ch == '}') p++;
            else if (ch == '[') {
                p++;
                Obj m;
                m.t = Obj::MARK;
                st.push_back(m);
            } else if (ch == ']') {
                p++;
                size_t b0 = base();
                Obj a;
                a.t = Obj::ARR;
                for (size_t k = b0; k < st.size(); k++)
                    if (st[k].t == Obj::ARR) a.nest = std::max(a.nest, st[k].nest + 1);
                // (its items moved, not copied: an array in an array in an array ... took time by the
                // square of the depth; and one nested deeper than any drawing has them is left empty)
                if (a.nest <= MAX_NEST) a.a.assign(std::make_move_iterator(st.begin() + b0), std::make_move_iterator(st.end()));
                else a.nest = 0;
                st.resize(b0 ? b0 - 1 : 0);
                st.push_back(std::move(a));
            } else if (ch == '{') skipProc();
            else {
                bool lit = ch == '/';
                if (lit) p++;
                const char *t = p;
                while (p < e && !isSpace(*p) && !isDelim(*p)) p++;
                if (p == t && !lit) {
                    p++;
                    continue;
                }
                Obj o;
                if (lit) {
                    o.t = Obj::NAME;
                    o.s.assign(t, p);
                    st.push_back(std::move(o));
                } else if (number(t, p, o.n)) st.push_back(std::move(o));
                else op(std::string(t, p));
            }
            if (st.size() > 100000) st.clear();
        }
    }

    void comment() {
        if (p + 1 < e && p[1] == '_') {   // a pseudo comment: operators PostScript does not see
            p += 2;
            return;
        }
        const char *eol = p;
        while (eol < e && *eol != '\n' && *eol != '\r') eol++;
        std::string l(p, std::min(eol, p + 40));
        if (!l.compare(0, 14, "%%BeginBinary:")) {   // (by its byte count: the data may have anything in it)
            double count;
            const char *t = p + 14, *te;
            while (t < eol && (*t == ' ' || *t == '\t')) t++;
            for (te = t; te < eol && !isSpace(*te);) te++;
            const char *d = eol < e && *eol == '\r' && eol + 1 < e && eol[1] == '\n' ? eol + 2 : eol + (eol < e);
            if (number(t, te, count) && count >= 0 && count <= (double)(e - d)) {
                p = findLine(d + (size_t)count, e, "%%EndBinary");
                return;
            }
        }
        static const char *const skips[][2] = {
            {"%%BeginProlog", "%%EndProlog"},       {"%%BeginResource", "%%EndResource"},
            {"%%BeginProcSet", "%%EndProcSet"},     {"%%BeginFont", "%%EndFont"},
            {"%%BeginDocument", "%%EndDocument"},   {"%%BeginFile", "%%EndFile"},
            {"%%BeginBinary", "%%EndBinary"},       {"%AI5_BeginPalette", "%AI5_EndPalette"},
            {"%AI5_Begin_NonPrinting", "%AI5_End_NonPrinting"},
            {"%AI8_BeginPluginObject", "%AI8_EndPluginObject"}};
        for (auto &s : skips)
            if (!l.compare(0, strlen(s[0]), s[0])) {
                p = findLine(eol, e, s[1]);
                if (strcmp(s[0], "%%BeginProlog")) {   // (a procset left open: no further than the prolog's end)
                    // (looked for once, not for every section: after the prolog that was a search to the file's end each time)
                    if (!prologEnd || prologEnd < eol) prologEnd = findLine(eol, e, "%%EndProlog");
                    p = std::min(p, prologEnd);
                }
                return;
            }
        if (!l.compare(0, 10, "%%EndSetup")) draw = true;
        else if (!l.compare(0, 13, "%%PageTrailer") || !l.compare(0, 9, "%%Trailer")) stop = true;
        p = eol;
    }

    Obj literal() {
        Obj o;
        o.t = Obj::STR;
        p++;
        int lev = 1;
        while (p < e) {
            char c = *p++;
            if (c == '\\') {
                if (p >= e) break;
                char d = *p++;
                switch (d) {
                case 'n': o.s += '\n'; break;
                case 'r': o.s += '\r'; break;
                case 't': o.s += '\t'; break;
                case 'b': o.s += '\b'; break;
                case 'f': o.s += '\f'; break;
                case '\r':
                    if (p < e && *p == '\n') p++;
                    break;
                case '\n': break;
                default:
                    if (d >= '0' && d <= '7') {
                        int v = d - '0';
                        for (int k = 0; k < 2 && p < e && *p >= '0' && *p <= '7'; k++) v = v * 8 + (*p++ - '0');
                        o.s += (char)v;
                    } else
                        o.s += d;
                }
            } else if (c == '(') {
                lev++;
                o.s += c;
            } else if (c == ')') {
                if (--lev == 0) break;
                o.s += c;
            } else
                o.s += c;
        }
        return o;
    }
    Obj hexString() {
        Obj o;
        o.t = Obj::STR;
        p++;
        int hi = -1;
        while (p < e && *p != '>') {
            int v = hexVal(*p++);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else o.s += (char)(hi * 16 + v), hi = -1;
        }
        if (hi >= 0) o.s += (char)(hi * 16);
        if (p < e) p++;
        return o;
    }
    void skipProc() {
        int lev = 0;
        while (p < e) {
            char c = *p;
            if (c == '(') {
                literal();
                continue;
            }
            if (c == '%') {
                while (p < e && *p != '\n' && *p != '\r') p++;
                continue;
            }
            p++;
            if (c == '{') lev++;
            else if (c == '}' && --lev == 0) return;
        }
    }

    // ---- drawing

    void endText() {
        if (bt) out += "ET\n", bt = false;
    }
    void beginText() {
        if (!bt) out += "BT\n", bt = true, putTm();
    }
    void putTm() { out += fmt(tlm) + " Tm\n"; }
    // the font, at its size times the vertical scale (Tz's second number), and the horizontal scale
    // relative to that: PDF has a horizontal one only, and the text matrix keeps the line's position
    void putFont() {
        if (!emit()) return;
        if (!fres.empty() && fsize > 0) out += "/" + fres + " " + fmt(fsize * vscale) + " Tf\n";
        out += fmt(hscale / vscale) + " Tz\n";
    }
    void useFill() {
        if (lp != 1) gcol = fill, lp = 1;
        if (efill != gcol.f) out += gcol.f + "\n", efill = gcol.f;
    }
    void useStroke() {
        if (lp != 2) gcol = stroke, lp = 2;
        if (estroke != gcol.s) out += gcol.s + "\n", estroke = gcol.s;
    }
    void setFill(const Colour &c) { fill = c, lp = 0; }
    void setStroke(const Colour &c) { stroke = c, lp = 0; }

    void pt(int n, const char *opname) {   // a path segment of n points
        if (!nums(0, 2 * n)) return;
        for (int k = 2 * n; k-- > 0;) path += fmt(num(k)) + " ";
        for (int i = 0; i < n; i++) {   // (the path's extent, for the page: see placed())
            double x = num(2 * n - 1 - 2 * i), y = num(2 * n - 2 - 2 * i);
            ext[0] = std::min(ext[0], x), ext[1] = std::min(ext[1], y), ext[2] = std::max(ext[2], x), ext[3] = std::max(ext[3], y);
        }
        path += opname;
        path += "\n";
    }

    void paint(char op) {
        if (strchr("fnsb", op)) path += "h\n";
        if (pola > 0) {   // a compound path: its parts are painted together, at the end
            crender = op;
            return;
        }
        doPaint(op);
    }
    // whether what is drawn lies on the artboard (counted for the top level's paths and text)
    void placed(double x0, double y0, double x1, double y1) {
        if (!artboard || depth) return;
        bool in = x0 >= ab[0] && y0 >= ab[1] && x1 <= ab[2] && y1 <= ab[3];
        (in ? onBoard : offBoard)++;
    }
    void doPaint(char op) {
        std::string body;
        body.swap(path);
        double e0 = ext[0], e1 = ext[1], e2 = ext[2], e3 = ext[3];
        ext[0] = ext[1] = HUGE_VAL, ext[2] = ext[3] = -HUGE_VAL;
        bool clipNow = clip;
        clip = false;
        if (!emit() || body.empty()) return;
        if (op != 'N' && op != 'n' && e0 <= e2) placed(e0, e1, e2, e3);
        endText();
        bool fills = strchr("FfBb", op), strokes = strchr("SsBb", op);
        if (grad && fills && gflag == 2) {   // (Bg's flag 2: the gradient is not rendered)
            if (strokes) useStroke(), out += body + "S\n";
            if (clipNow) out += body + (eo ? "W* n\n" : "W n\n");
            return;
        }
        if (grad && fills && gradFill(body)) {
            gpath = body;
            if (strokes) useStroke(), out += body + "S\n";
            if (clipNow) out += body + (eo ? "W* n\n" : "W n\n");
            return;
        }
        if (fills) useFill();
        if (strokes) useStroke();
        const char *o = "n";
        switch (op) {
        case 'F': case 'f': o = eo ? "f*" : "f"; break;
        case 'S': o = "S"; break;
        case 's': o = "s"; break;
        case 'B': o = eo ? "B*" : "B"; break;
        case 'b': o = eo ? "b*" : "b"; break;
        }
        out += body;
        if (clipNow) out += eo ? "W* " : "W ";
        out += o;
        out += "\n";
    }

    // the gradient's function: its ramp sampled (with the mid points), in the space of its colours
    int gradFunction(Grad &G) {
        if (G.fn) return G.fn;
        std::vector<Stop> s = G.stops;
        std::stable_sort(s.begin(), s.end(), [](const Stop &a, const Stop &b) { return a.pos < b.pos; });
        int sp = 0;
        for (auto &x : s) sp = x.sp == 2 ? 2 : std::max(sp, x.sp);
        int n = sp == 0 ? 1 : sp == 1 ? 4 : 3;
        for (auto &x : s) {   // all in one space
            double c[4] = {};   // (an RGB one has three: the fourth is 0, not left as it was)
            if (x.sp == sp) continue;
            if (x.sp == 0 && sp == 1) c[0] = c[1] = c[2] = 0, c[3] = 1 - x.c[0];
            else if (x.sp == 0) c[0] = c[1] = c[2] = x.c[0];
            else
                for (int k = 0; k < 3; k++) c[k] = 1 - std::min(1.0, x.c[k] + x.c[3]);
            memcpy(x.c, c, sizeof c);
            x.sp = sp;
        }
        std::string data;
        for (int i = 0; i < 256; i++) {
            double t = i / 2.55, c[4];
            size_t k = 0;
            while (k + 1 < s.size() && t > s[k + 1].pos) k++;
            if (t <= s[0].pos || k + 1 >= s.size()) memcpy(c, (t <= s[0].pos ? s[0] : s.back()).c, sizeof c);
            else {
                const Stop &a = s[k], &b = s[k + 1];
                double u = b.pos > a.pos ? (t - a.pos) / (b.pos - a.pos) : 1;
                double m = std::max(0.01, std::min(0.99, a.mid / 100));
                u = pow(clamp01(u), log(0.5) / log(m));
                for (int j = 0; j < 4; j++) c[j] = a.c[j] + (b.c[j] - a.c[j]) * u;
            }
            for (int j = 0; j < n; j++) data += (char)(uint8_t)lround(clamp01(c[j]) * 255);
        }
        std::string range;
        for (int j = 0; j < n; j++) range += "0 1 ";
        G.n = n;
        G.fn = D.add(stream("/FunctionType 0/Domain[0 1]/Range[" + range + "]/Size[256]/BitsPerSample 8", data));
        return G.fn;
    }
    bool gradFill(const std::string &body) {
        auto it = D.grads.find(gname);
        if (it == D.grads.end() || it->second.stops.empty()) return false;
        Grad &G = it->second;
        int fn = gradFunction(G);
        double xo = g[0], yo = g[1], ang = g[2] * DEG, len = g[3];
        Mtx m;
        // where it is: by the matrix written for printing (Xm; a radial one's Bm), the unit ramp (or circle)
        // to the page; else by the instance's own (Bg), relative to a canvas whose corner moved between versions
        bool unit = hasXm || (G.radial && hasBm);
        if (unit) {
            m = hasXm ? xm : bm;
            xo = yo = 0, len = 1, ang = 0;
        } else {
            double a = g[4], b = g[5], c = g[6], d = g[7], tx = g[8], ty = g[9], X0 = -4014, Y0 = 4716;
            m = {a, -b, -c, d, tx + X0 - a * X0 + c * Y0, -ty + Y0 + b * X0 - d * Y0};
        }
        double det = m.a * m.d - m.b * m.c;
        if (!std::isfinite(xo + yo + ang + len + det + m.e + m.f) || fabs(det) < 1e-12) return false;
        std::string coords;
        if (G.radial) {
            double fx = xo, fy = yo;
            if (hasBh && std::isfinite(bh[2] + bh[3])) {
                // the highlight: its angle and its share of the radius in the instance's space (Bg's),
                // so its offset goes through Bg's matrix (y down: as above), then into the circle's own
                // space (that of Xm / Bm, which may be mirrored: Bm is, in the specification's example)
                double l = std::max(0.0, std::min(0.99, bh[3])) * g[3], a = bh[2] * DEG, dx = l * cos(a), dy = l * sin(a);
                double px = g[4] * dx - g[6] * dy, py = -g[5] * dx + g[7] * dy;
                if (unit) {
                    double ux = (m.d * px - m.c * py) / det, uy = (-m.b * px + m.a * py) / det, r = hypot(ux, uy);
                    if (r > 0.99) ux *= 0.99 / r, uy *= 0.99 / r;
                    if (std::isfinite(ux + uy)) fx = ux, fy = uy;
                } else
                    fx += dx, fy += dy;
            }
            coords = "/ShadingType 3/Coords[" + fmt(fx) + " " + fmt(fy) + " 0 " + fmt(xo) + " " + fmt(yo) + " " + fmt(fabs(len)) + "]";
        } else
            coords = "/ShadingType 2/Coords[" + fmt(xo) + " " + fmt(yo) + " " + fmt(xo + len * cos(ang)) + " " + fmt(yo + len * sin(ang)) + "]";
        const char *cs = G.n == 1 ? "/DeviceGray" : G.n == 4 ? "/DeviceCMYK" : "/DeviceRGB";
        int sh = D.add("<<" + coords + "/ColorSpace" + cs + "/Function " + std::to_string(fn) + " 0 R/Extend[true true]>>");
        std::string res = "Sh" + std::to_string(++D.nSh);
        D.shadings += "/" + res + " " + std::to_string(sh) + " 0 R";
        out += "q\n" + body + (eo ? "W* n\n" : "W n\n");
        if (m.a != 1 || m.b != 0 || m.c != 0 || m.d != 1 || m.e != 0 || m.f != 0) out += fmt(m) + " cm\n";
        out += "/" + res + " sh\nQ\n";
        return true;
    }

    void endGradient() {
        if (emit() && (bbFlag == 1 || bbFlag == 2) && !gpath.empty()) {
            endText();
            useStroke();
            out += gpath + (bbFlag == 2 ? "h S\n" : "S\n");
        }
        grad = gradLater = false;
        gpath.clear();
    }

    // ---- colours

    static Colour grey(double v) { return Colour(fmt(clamp01(v)), "g", "G"); }
    static Colour cmyk(double c, double m, double y, double k, double tint) {
        double t = 1 - clamp01(tint);
        return Colour(fmt(clamp01(c) * t) + " " + fmt(clamp01(m) * t) + " " + fmt(clamp01(y) * t) + " " + fmt(clamp01(k) * t), "k", "K");
    }
    static Colour rgb(double r, double g, double b, double tint) {
        double t = 1 - clamp01(tint);
        return Colour(fmt(1 - (1 - clamp01(r)) * t) + " " + fmt(1 - (1 - clamp01(g)) * t) + " " + fmt(1 - (1 - clamp01(b)) * t), "rg", "RG");
    }
    // a custom colour of Xx / Xk: components, name, tint, type (0 CMYK, 1 RGB)
    bool custom(Colour &to) {
        if (!nums(0, 2)) return false;
        int type = inum(0);
        double tint = num(1);
        if (!arg(2) || arg(2)->t != Obj::STR) return false;
        if (type == 1 && nums(3, 3)) to = rgb(num(5), num(4), num(3), tint);
        else if (type == 0 && nums(3, 4)) to = cmyk(num(6), num(5), num(4), num(3), tint);
        else return false;
        return true;
    }

    std::string patternFor() {   // (name) px py sx sy angle rf r k ka [matrix] p: the pattern's resource name ("": none)
        size_t k0 = arg(0) && arg(0)->t == Obj::ARR ? 1 : 0;
        if (!nums(k0, 9) || !arg(k0 + 9) || arg(k0 + 9)->t != Obj::STR) return "";
        auto it = D.pats.find(arg(k0 + 9)->s);
        if (it == D.pats.end()) return "";
        Pattern &P = it->second;
        double w = P.x1 - P.x0, h = P.y1 - P.y0;
        if (!(w > 0.01 && h > 0.01 && w < 1e6 && h < 1e6)) return "";
        Mtx m;
        if (k0) {
            const auto &a = arg(0)->a;
            if (a.size() == 6) m = {a[0].n, a[1].n, a[2].n, a[3].n, a[4].n, a[5].n};
        }
        double px = num(k0 + 8), py = num(k0 + 7), sx = num(k0 + 6), sy = num(k0 + 5), ang = num(k0 + 4);
        double rf = num(k0 + 3), r = num(k0 + 2), sk = num(k0 + 1), ska = num(k0);
        m = mul(Mtx{1, 0, 0, 1, px, py}, m);
        m = mul(Mtx{sx, 0, 0, sy, 0, 0}, m);
        m = mul(rotate(ang), m);
        if (rf != 0) m = mul(rotate(-r), mul(Mtx{1, 0, 0, -1, 0, 0}, mul(rotate(r), m)));
        double tn = tan(sk * DEG);
        if (std::isfinite(tn) && fabs(tn) < 1e3) m = mul(rotate(-ska), mul(Mtx{1, 0, tn, 1, 0, 0}, mul(rotate(ska), m)));
        std::string key = it->first + " " + fmt(m);
        auto r2 = D.patRes.find(key);
        if (r2 == D.patRes.end()) {
            // (the tile's drawing once, in a form: a pattern filled with many matrices had a copy of it for each)
            std::string box = "/BBox[0 0 " + fmt(w) + " " + fmt(h) + "]";
            if (!P.form) P.form = D.add(stream("/Type/XObject/Subtype/Form" + box + "/Resources " + std::to_string(D.resObj) + " 0 R", P.content));
            std::string dict = "/Type/Pattern/PatternType 1/PaintType 1/TilingType 1" + box + "/XStep " + fmt(w) + "/YStep " + fmt(h) +
                               "/Matrix[" + fmt(m) + "]/Resources<</XObject<</T " + std::to_string(P.form) + " 0 R>>>>";
            int o = D.add(stream(dict, "/T Do"));
            std::string name = "P" + std::to_string(D.patRes.size() + 1);
            D.patterns += "/" + name + " " + std::to_string(o) + " 0 R";
            r2 = D.patRes.emplace(key, name).first;
        }
        return r2->second;
    }

    // (name) llx lly urx ury [ (colour) @ (tile) & ... ] E
    void definePattern() {
        size_t n = nargs(), b0 = base(), k = 0;
        while (k + 4 < n && !(st[b0 + k].t == Obj::STR && st[b0 + k + 1].t == Obj::NUM && st[b0 + k + 4].t == Obj::NUM)) k++;
        if (k + 4 >= n || depth > 2) return;
        Pattern P{st[b0 + k + 1].n, st[b0 + k + 2].n, st[b0 + k + 3].n, st[b0 + k + 4].n, "", 0};
        std::vector<const Obj *> items;
        for (size_t j = b0 + k + 5; j < st.size(); j++) {
            if (st[j].t == Obj::ARR)
                for (auto &o : st[j].a) items.push_back(&o);
            else
                items.push_back(&st[j]);
        }
        P.content = "4 M\n1 0 0 1 " + fmt(-P.x0) + " " + fmt(-P.y0) + " cm\n";
        Run r(D, P.content, depth + 1, true);
        for (size_t j = 0; j + 1 < items.size(); j += 2) {
            if (items[j]->t == Obj::STR) r.feed(items[j]->s.data(), items[j]->s.data() + items[j]->s.size());
            if (items[j + 1]->t == Obj::STR) r.feed(items[j + 1]->s.data(), items[j + 1]->s.data() + items[j + 1]->s.size());
            else if (items[j + 1]->t == Obj::TILEFILL) {
                r.endText();
                r.useFill();
                P.content += fmt(P.x0) + " " + fmt(P.y0) + " " + fmt(P.x1 - P.x0) + " " + fmt(P.y1 - P.y0) + " re f\n";
            }
            r.stop = false;
        }
        r.finish();
        D.pats[st[b0 + k].s] = std::move(P);
    }

    // ---- images: [matrix] llx lly urx ury w h bits channels alpha reserved encoding mask XI, the data after it
    void image() {
        if (!nums(0, 12) || !arg(12) || arg(12)->t != Obj::ARR || arg(12)->a.size() != 6) return;
        const auto &ma = arg(12)->a;
        Mtx m{ma[0].n, ma[1].n, ma[2].n, ma[3].n, ma[4].n, ma[5].n};
        bool mask = num(0) != 0;
        int enc = inum(1) % 256, comp = inum(1) / 256, alpha = inum(3), ch = inum(4), bits = inum(5);
        double w = num(7), h = num(6);
        // the data: hexadecimal on comment lines, or binary right after the operator
        while (p < e && *p != '\n' && *p != '\r') p++;
        if (p < e && *p == '\r') p++;
        if (p < e && *p == '\n') p++;
        bool ok = w >= 1 && h >= 1 && w * h <= 1e8 && (bits == 1 || bits == 8) && (ch == 1 || ch == 3 || ch == 4) && alpha >= 0 && alpha < 4 && !comp;
        int comps = ok ? (bits == 1 ? 1 : ch + alpha) : 0;   // (ch and alpha only checked when ok)
        size_t W = ok ? (size_t)w : 0, H = ok ? (size_t)h : 0, row = bits == 1 ? (W + 7) / 8 : W * comps, need = row * H;
        if (need > (size_t)(e - p) + (PAD_MAX - D.padded)) ok = false, need = 0;   // (far more than the file has)
        std::string data;
        if (enc == 1) {
            size_t n = std::min(need, (size_t)(e - p));
            data.assign(p, n);
            p += n;
        } else {
            while (p < e && *p == '%' && p + 1 < e && hexVal(p[1]) >= 0) {
                p++;
                int hi = -1;
                while (p < e && *p != '\n' && *p != '\r') {
                    int v = hexVal(*p++);
                    if (v < 0) continue;
                    if (hi < 0) hi = v;
                    else if (data.size() < need) data += (char)(hi * 16 + v), hi = -1;
                    else hi = -1;
                }
                while (p < e && (*p == '\n' || *p == '\r')) p++;
            }
        }
        if (!ok || !emit()) return;
        if (need > data.size()) {
            if (need - data.size() > PAD_MAX - D.padded) return;
            D.padded += need - data.size();
        }
        data.resize(need, bits == 1 ? '\xff' : '\0');
        if (alpha && bits == 8) {   // (the alpha channels left out)
            std::string d2;
            d2.reserve(W * H * ch);
            for (size_t i = 0; i < W * H; i++) d2.append(data, i * comps, ch);
            data.swap(d2);
        }
        std::string dict = "/Type/XObject/Subtype/Image/Width " + std::to_string(W) + "/Height " + std::to_string(H);
        if (mask && bits == 1) dict += "/ImageMask true/Decode[1 0]";
        else
            dict += std::string("/ColorSpace") + (ch == 1 || bits == 1 ? "/DeviceGray" : ch == 3 ? "/DeviceRGB" : "/DeviceCMYK") +
                    "/BitsPerComponent " + std::to_string(bits);
        int o = D.add(stream(dict, data));
        std::string name = "Im" + std::to_string(++D.nIm);
        D.xobjects += "/" + name + " " + std::to_string(o) + " 0 R";
        // image space: x right, y down from the matrix's origin
        Mtx cm{m.a * w, m.b * w, m.c * h, m.d * h, m.e - m.c * h, m.f - m.d * h};
        endText();
        if (mask && bits == 1) useFill();
        out += "q " + fmt(cm) + " cm /" + name + " Do Q\n";
    }

    // ---- text

    std::string font(const std::string &ai) {
        auto it = D.fontRes.find(ai);
        if (it != D.fontRes.end()) return it->second;
        std::string baseName = ai;
        std::vector<const Obj *> diff;
        auto r = D.tz.find(ai);
        if (r != D.tz.end()) {
            baseName = r->second.base;
            if (r->second.useStd)
                for (auto &o : D.te) diff.push_back(&o);
            for (auto &o : r->second.pairs) diff.push_back(&o);
        } else if (!ai.empty() && ai[0] == '_') {
            baseName = ai.substr(1);
            for (auto &o : D.te) diff.push_back(&o);
        }
        std::string d = "<</Type/Font/Subtype/Type1/BaseFont" + pdfName(baseName);
        if (!diff.empty()) {   // (a list of names without a code first: a whole encoding, from 0)
            d += "/Encoding<</Type/Encoding/Differences[";
            if (diff[0]->t != Obj::NUM) d += "0 ";
            for (const Obj *o : diff) d += o->t == Obj::NUM ? std::to_string(ival(o->n)) + " " : o->t == Obj::NAME ? pdfName(o->s) + " " : "";
            d += "]>>";
        }
        int o = D.add(d + ">>");
        std::string name = "F" + std::to_string(D.fontRes.size() + 1);
        D.fonts += "/" + name + " " + std::to_string(o) + " 0 R";
        D.fontRes[ai] = name;
        return name;
    }
    void show(const std::string &s0) {
        if (!emit()) return;
        std::string s;
        for (char c : s0)
            if (c != '\r') s += c;
        if (s.empty()) return;
        beginText();
        placed(tlm.e, tlm.f, tlm.e, tlm.f);
        if (tr == 0 || tr == 2 || tr == 4 || tr == 6) useFill();
        if (tr == 1 || tr == 2 || tr == 5 || tr == 6) useStroke();
        out += hex(s) + " Tj\n";
    }

    // ---- the operators

    void op(const std::string &o) {
        if (o == "@" || o == "&") return;   // (pattern layers: the strings stay for E)
        if (o == "_") {
            Obj t;
            t.t = Obj::TILEFILL;
            st.push_back(t);
            return;
        }
        exec(o);
        if (o == "TE" || o == "TZ" || o == "BD") {   // (these take the mark too)
            size_t b0 = base();
            st.resize(b0 ? b0 - 1 : 0);
            return;
        }
        st.resize(base());
    }

    void exec(const std::string &o) {
        const char *s = o.c_str();
        size_t n = o.size();
        if (n == 1) {
            switch (s[0]) {
            case 'm': pt(1, "m"); return;
            case 'l': case 'L': pt(1, "l"); return;
            case 'c': case 'C': pt(3, "c"); return;
            case 'v': case 'V': pt(2, "v"); return;
            case 'y': case 'Y': pt(2, "y"); return;
            case 'h': path += "h\n"; return;
            case 'H': return;
            case 'W': clip = true; return;
            case 'N': case 'n': case 'F': case 'f': case 'S': case 's': case 'B': case 'b': paint(s[0]); return;
            case 'q':
                if (!pola && emit()) endText(), out += "q\n", qd++, saved.push_back(gcol);
                return;
            case 'Q':
                if (!pola && emit() && qd > 0) {
                    endText(), out += "Q\n", qd--, efill.clear(), estroke.clear();
                    if (!saved.empty()) gcol = saved.back(), saved.pop_back();
                }
                return;
            case 'u': case 'U': return;
            case '*': path.clear(), clip = false, ext[0] = ext[1] = HUGE_VAL, ext[2] = ext[3] = -HUGE_VAL; return;   // (a guide)
            case 'w': if (emit() && nums(0, 1)) out += fmt(num(0)) + " w\n"; return;
            case 'j': if (emit() && nums(0, 1)) out += std::to_string(std::max(0, std::min(2, inum(0)))) + " j\n"; return;
            case 'J': if (emit() && nums(0, 1)) out += std::to_string(std::max(0, std::min(2, inum(0)))) + " J\n"; return;
            case 'M': if (emit() && nums(0, 1)) out += fmt(std::max(1.0, num(0))) + " M\n"; return;
            case 'd':
                if (emit() && nums(0, 1) && arg(1) && arg(1)->t == Obj::ARR) {
                    std::string a = "[";
                    for (auto &x : arg(1)->a) a += fmt(x.n) + " ";
                    out += a + "] " + fmt(num(0)) + " d\n";
                }
                return;
            case 'g': if (nums(0, 1)) setFill(grey(num(0))); return;
            case 'G': if (nums(0, 1)) setStroke(grey(num(0))); return;
            case 'k': if (nums(0, 4)) setFill(cmyk(num(3), num(2), num(1), num(0), 0)); return;
            case 'K': if (nums(0, 4)) setStroke(cmyk(num(3), num(2), num(1), num(0), 0)); return;
            case 'x': case 'X':   // c m y k (name) tint
                if (nums(0, 1) && arg(1) && arg(1)->t == Obj::STR && nums(2, 4))
                    s[0] == 'x' ? setFill(cmyk(num(5), num(4), num(3), num(2), num(0))) : setStroke(cmyk(num(5), num(4), num(3), num(2), num(0)));
                return;
            case 'p': case 'P': {
                std::string name = patternFor();
                if (name.empty()) return;
                Colour c;
                c.f = "/Pattern cs /" + name + " scn", c.s = "/Pattern CS /" + name + " SCN";
                s[0] == 'p' ? setFill(c) : setStroke(c);
                return;
            }
            case 'E': definePattern(); return;
            case 'A': case 'D': case 'O': case 'R': case 'i': return;
            }
        }
        if (o == "*u") pola++;
        else if (o == "*U") {
            if (pola > 0 && --pola == 0 && crender) {
                char c = crender;
                crender = 0;
                doPaint(c);
            }
            if (!pola && gradLater) endGradient();
        } else if (o == "XR") eo = num(0) != 0;
        else if (o == "Xa" || o == "XA") {   // r g b (Illustrator 8 writes c m y k r g b)
            if (nums(0, 3)) o == "Xa" ? setFill(rgb(num(2), num(1), num(0), 0)) : setStroke(rgb(num(2), num(1), num(0), 0));
        } else if (o == "Xx" || o == "Xk") {
            Colour c;
            if (custom(c)) setFill(c);
        } else if (o == "XX" || o == "XK") {
            Colour c;
            if (custom(c)) setStroke(c);
        }
        else if (o == "Lb") {
            bool vis = nargs() == 0 || st[base()].t != Obj::NUM || st[base()].n != 0;
            layers.push_back(vis);
            if (!vis) hidden++;
        } else if (o == "LB") {
            if (!layers.empty()) {
                if (!layers.back()) hidden--;
                layers.pop_back();
            }
        }
        // gradients
        else if (o == "Bd") {   // (name) type ncolours
            if (nums(0, 2) && arg(2) && arg(2)->t == Obj::STR) {
                gdef = &D.grads[arg(2)->s];
                *gdef = Grad();
                gdef->radial = num(1) == 1;
            }
        } else if (o == "Bs") stopDef();
        else if (o == "BD") gdef = nullptr;
        else if (o == "Bb") grad = true, hasBh = hasXm = hasBm = false, gflag = 1, gname.clear(), gpath.clear();
        else if (o == "Xm" || o == "Bm") {   // a b c d tx ty (the first Bm: a radial one has one)
            if (grad && nums(0, 6) && (o == "Xm" || !hasBm)) {
                Mtx t{num(5), num(4), num(3), num(2), num(1), num(0)};
                if (o == "Xm") xm = t, hasXm = true;
                else bm = t, hasBm = true;
            }
        }
        else if (o == "Bh") {
            if (nums(0, 4)) {
                for (int k = 0; k < 4; k++) bh[k] = num(3 - k);
                hasBh = true;
            }
        } else if (o == "Bg") {   // flag (name) x y angle length a b c d tx ty
            if (nums(0, 10) && arg(10) && arg(10)->t == Obj::STR) {
                gname = arg(10)->s;
                gflag = arg(11) && arg(11)->t == Obj::NUM ? ival(arg(11)->n) : 1;
                for (int k = 0; k < 10; k++) g[k] = num(9 - k);
            }
        } else if (o == "BB") {   // the end of the instance: 1 strokes its path, 2 closes it first
            bbFlag = inum(0);
            if (pola > 0 && crender) gradLater = true;   // (in a compound path: when it is painted, at *U)
            else endGradient();
        }
        // images
        else if (o == "XI") image();
        // fonts
        else if (o == "TE") {
            size_t b0 = base();
            D.te.assign(st.begin() + b0, st.end());
        } else if (o == "TZ") reencode();
        // text
        else if (o == "To") endText(), tlm = Mtx();
        else if (o == "TO") endText();
        else if (o == "Tp" || o == "Tm") {
            size_t k = o == "Tp" ? 1 : 0;
            if (nums(k, 6)) {
                tlm = {num(k + 5), num(k + 4), num(k + 3), num(k + 2), num(k + 1), num(k)};
                if (bt) putTm();
            }
        } else if (o == "Td" || o == "T*") {
            double tx = 0, ty = -leading;
            if (o == "Td") {
                if (!nums(0, 2)) return;
                tx = num(1), ty = num(0);
            }
            tlm = mul(Mtx{1, 0, 0, 1, tx, ty}, tlm);
            if (bt) putTm();
        } else if (o == "Tl") {
            if (nums(0, 2)) leading = num(1);
            else if (nums(0, 1)) leading = num(0);
        } else if (o == "Tr") {
            int m = inum(0);
            tr = m == 8 ? 0 : m == 9 ? 1 : m >= 0 && m <= 7 ? m : 0;
            if (emit()) out += std::to_string(tr) + " Tr\n";
        } else if (o == "Tf") {   // /name size [ascent descent]
            size_t k = 0;
            while (k < nargs() && arg(k)->t != Obj::NAME) k++;
            if (k < nargs() && k >= 1 && emit()) {
                fres = font(arg(k)->s), fsize = num(k - 1);
                putFont();
            }
        } else if (o == "Tz") {
            double hz = nums(0, 2) ? num(1) : num(0), vz = nums(0, 2) ? num(0) : 100;
            if (hz > 0 && hz < 10000) hscale = hz;
            vscale = vz > 0 && vz < 10000 ? vz / 100 : 1;
            putFont();   // (not the text matrix: the text goes on where the run before it ended)
        } else if (o == "Ts" || o == "Tc" || o == "Tw") {
            if (emit() && nums(0, 1)) out += fmt(num(0)) + " " + o + "\n";
        } else if (o == "Tx" || o == "Tj") {
            if (arg(0) && arg(0)->t == Obj::STR) show(arg(0)->s);
        } else if (o == "TS") {   // (string) justified TS: a run in the Symbol font at the size set, as the procsets do
            if (arg(1) && arg(1)->t == Obj::STR && emit() && fsize > 0) {
                std::string keep = fres;
                fres = font("Symbol");
                putFont();
                show(arg(1)->s);
                fres = keep;
                putFont();
            }
        } else if (o == "Tk") {   // autoKern kern: after the run, the kern in thousandths of an em
            if (emit() && nums(0, 1) && num(0) != 0) beginText(), out += "[" + fmt(num(0)) + "] TJ\n";
        }
    }

    // colourSpec colourStyle midPoint rampPoint %_Bs
    void stopDef() {
        if (!gdef || !nums(0, 3) || gdef->stops.size() > 1000) return;
        Stop x{num(0), num(1), 0, {0, 0, 0, 0}};
        if (!std::isfinite(x.pos) || !std::isfinite(x.mid)) return;   // ("0e999": the stops are sorted by their place)
        int style = inum(2);
        switch (style) {
        case 0:
            if (!nums(3, 1)) return;
            x.c[0] = clamp01(num(3));
            break;
        case 1:
            if (!nums(3, 4)) return;
            x.sp = 1;
            for (int k = 0; k < 4; k++) x.c[k] = clamp01(num(6 - k));
            break;
        case 2:   // c m y k r g b
            if (!nums(3, 3)) return;
            x.sp = 2;
            for (int k = 0; k < 3; k++) x.c[k] = clamp01(num(5 - k));
            break;
        case 3: {   // c m y k (name) tint
            if (!nums(3, 1) || !nums(5, 4)) return;
            double t = 1 - clamp01(num(3));
            x.sp = 1;
            for (int k = 0; k < 4; k++) x.c[k] = clamp01(num(8 - k)) * t;
            break;
        }
        case 4: {   // c m y k r g b (name) tint type
            if (!nums(3, 2) || !nums(6, 3)) return;
            double t = 1 - clamp01(num(4));
            x.sp = 2;
            for (int k = 0; k < 3; k++) x.c[k] = 1 - (1 - clamp01(num(8 - k))) * t;
            break;
        }
        case 5: {   // (Illustrator 8) components (name) tint type, as Xk: type 0 CMYK, 1 RGB
            if (!nums(3, 2) || !arg(5) || arg(5)->t != Obj::STR) return;
            double t = 1 - clamp01(num(4));
            if (num(3) == 1 && nums(6, 3)) {
                x.sp = 2;
                for (int k = 0; k < 3; k++) x.c[k] = 1 - (1 - clamp01(num(8 - k))) * t;
            } else if (num(3) == 0 && nums(6, 4)) {
                x.sp = 1;
                for (int k = 0; k < 4; k++) x.c[k] = clamp01(num(9 - k)) * t;
            } else
                return;
            break;
        }
        default: return;
        }
        gdef->stops.push_back(x);
    }

    // [ pairs /_new /old [direction] script useDefault [weights] TZ
    void reencode() {
        size_t b0 = base(), n = st.size();
        size_t k = n;
        while (k > b0 && (st[k - 1].t == Obj::NUM || st[k - 1].t == Obj::ARR)) k--;
        if (k < b0 + 2 || st[k - 1].t != Obj::NAME || st[k - 2].t != Obj::NAME) return;
        Reenc r;
        r.base = st[k - 1].s;
        size_t last = n;
        while (last > k && st[last - 1].t != Obj::NUM) last--;
        r.useStd = last > k ? st[last - 1].n != 0 : true;
        r.pairs.assign(st.begin() + b0, st.begin() + (k - 2));
        D.tz[st[k - 2].s] = std::move(r);
    }

    void finish() {
        path.clear();
        endText();
        if (emit())
            while (qd > 0) out += "Q\n", qd--;
    }
};


// The PostScript of the file: all of it, or the part a DOS EPS file's binary header names (a TIFF or
// WMF preview is the rest). false: such a header that points outside the file.
bool postscript(const void *data, size_t len, const char *&ps, size_t &n) {
    const uint8_t *d = (const uint8_t *)data;
    ps = (const char *)data, n = len;
    if (len >= 30 && d[0] == 0xC5 && d[1] == 0xD0 && d[2] == 0xD3 && d[3] == 0xC6) {
        uint32_t off = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24, size = d[8] | d[9] << 8 | d[10] << 16 | (uint32_t)d[11] << 24;
        if (off >= len || size > len - off) return false;
        ps += off, n = size;
    }
    return true;
}

bool header(const char *d, size_t len) {
    if (len < 20 || memcmp(d, "%!PS-Adobe", 10)) return false;
    std::string h(d, std::min(len, (size_t)16384));
    // Illustrator 9 and later (an EPS of theirs: PostScript of another kind, Adobe's AGM procsets; they may
    // write the version 8 format too, then their Creator says so)
    if (h.find("%AI9_PrintingDataBegin") != std::string::npos || h.find("procset Adobe_AGM_") != std::string::npos) return false;
    size_t c = h.find("%%Creator:");
    if (c != std::string::npos) {
        size_t eol = h.find_first_of("\r\n", c);
        std::string cr = h.substr(c, eol == std::string::npos ? std::string::npos : eol - c);
        size_t i = cr.find("Adobe Illustrator");
        if (i != std::string::npos && cr.find("xported") == std::string::npos) {   // (versions 9 and later write no such file)
            i += 17;
            while (i < cr.size() && !(cr[i] >= '0' && cr[i] <= '9')) i++;
            int v = i < cr.size() ? atoi(cr.c_str() + i) : 0;
            if (v >= 9 && v < 88) return false;   // (Illustrator 88 is older)
            return true;
        }
    }
    return h.find("%AI3_") != std::string::npos || h.find("%AI5_") != std::string::npos || h.find("%AI7_") != std::string::npos;
}

bool values(const std::string &ps, const char *key, int n, double *v) {   // a header comment's numbers
    size_t i = ps.find(key);
    if (i == std::string::npos || i > 65536) return false;
    const char *p = ps.c_str() + i + strlen(key), *e = ps.c_str() + std::min(ps.size(), i + 300);
    for (int k = 0; k < n; k++) {
        while (p < e && (*p == ' ' || *p == '\t')) p++;
        const char *t = p;
        while (p < e && !isSpace(*p)) p++;
        if (!number(t, p, v[k])) return false;
    }
    return true;
}

bool box(const std::string &ps, const char *key, double b[4]) {
    size_t i = ps.find(key);
    if (i == std::string::npos || i > 65536) return false;
    i += strlen(key);
    size_t a = i;
    while (a < ps.size() && (ps[a] == ' ' || ps[a] == '\t')) a++;
    if (ps.compare(a, 7, "(atend)") == 0) {   // (after any spaces, as the numbers; the key may end the file)
        i = ps.rfind(key);
        if (i == std::string::npos) return false;
        i += strlen(key);
    }
    const char *p = ps.c_str() + i, *e = ps.c_str() + std::min(ps.size(), i + 200);
    for (int k = 0; k < 4; k++) {
        while (p < e && (*p == ' ' || *p == '\t')) p++;
        const char *t = p;
        while (p < e && !isSpace(*p)) p++;
        if (!number(t, p, b[k]) || !(fabs(b[k]) < 1e6)) return false;   // (also the infinite: "1e999")
    }
    return b[2] > b[0] && b[3] > b[1];
}

}  // namespace

bool nitroai_is(const void *data, size_t len) {
    const char *ps;
    size_t n;
    return data && postscript(data, len, ps, n) && header(ps, n);
}

std::string nitroai_pdf(const void *data, size_t len) {
    const char *text;
    size_t n;
    if (!data || !postscript(data, len, text, n) || !header(text, n)) return std::string();
    std::string ps(text, n);
    Pdf D;
    for (int k = 0; k < 4; k++) D.add("");   // catalog, pages, page, resources
    std::string content = "4 M\n";   // (Illustrator's first miter limit; PDF's is 10)
    bool setup = findLine(ps.data(), ps.data() + ps.size(), "%%BeginSetup") < ps.data() + ps.size() &&
                 findLine(ps.data(), ps.data() + ps.size(), "%%EndSetup") < ps.data() + ps.size();
    Run r(D, content, 0, !setup);
    // a document (not an EPS): its artboard, as Illustrator shows it (and the PDF of a later version
    // has it): the size the header gives, centred on the template box's centre (in the art's coordinates)
    double art[2], tb[4];
    size_t eol = ps.find_first_of("\r\n");
    if (ps.substr(0, eol).find("EPSF") == std::string::npos && values(ps, "%AI5_ArtSize:", 2, art) &&
        values(ps, "%AI3_TemplateBox:", 4, tb) && art[0] > 1 && art[1] > 1 && art[0] < 1e5 && art[1] < 1e5 &&
        std::isfinite(tb[0] + tb[1] + tb[2] + tb[3]) && fabs(tb[0]) + fabs(tb[2]) < 1e6 && fabs(tb[1]) + fabs(tb[3]) < 1e6) {
        double cx = (tb[0] + tb[2]) / 2, cy = (tb[1] + tb[3]) / 2;
        r.artboard = true;
        r.ab[0] = cx - art[0] / 2, r.ab[1] = cy - art[1] / 2, r.ab[2] = cx + art[0] / 2, r.ab[3] = cy + art[1] / 2;
    }
    // F by the even-odd rule: the procsets of some programs that wrote the format (Illustrator's own: nonzero)
    const char *pe = findLine(ps.data(), ps.data() + ps.size(), "%%EndProlog");
    for (const char *q = ps.data(); (q = findLine(q, pe, "/F")) < pe; q += 2) {
        const char *t = q + 2;
        while (t < pe && (*t == ' ' || *t == '{')) t++;
        std::string def(q, std::min(pe, t + 60));
        if (q[2] != ' ' && q[2] != '{') continue;
        size_t close = def.find('}');
        r.eo = def.find("eofill") < close;
        break;
    }
    r.feed(ps.data(), ps.data() + ps.size());
    r.finish();
    double b[4];
    bool bbox = box(ps, "%%HiResBoundingBox:", b) || box(ps, "%%BoundingBox:", b);
    if (!bbox) b[0] = b[1] = 0, b[2] = 612, b[3] = 792;
    // a document (not an EPS): its artboard, as Illustrator shows it (and the PDF of a later version
    // has it): the size the header gives, centred on the template box's centre (in the art's coordinates)
    // the artboard, unless most of what is drawn lies (in part) off it: a drawing beside its artboard,
    // as the start of a piece of work often is, gets its bounding box (a stray object far off does not)
    if (r.artboard && (!bbox || r.onBoard >= r.offBoard)) memcpy(b, r.ab, sizeof b);
    int c = D.add(stream("", content));
    D.objs[0] = "<</Type/Catalog/Pages 2 0 R>>";
    D.objs[1] = "<</Type/Pages/Kids[3 0 R]/Count 1>>";
    D.objs[2] = "<</Type/Page/Parent 2 0 R/MediaBox[" + fmt(b[0]) + " " + fmt(b[1]) + " " + fmt(b[2]) + " " + fmt(b[3]) +
                "]/Resources 4 0 R/Contents " + std::to_string(c) + " 0 R>>";
    D.objs[3] = "<</Font<<" + D.fonts + ">>/Shading<<" + D.shadings + ">>/Pattern<<" + D.patterns + ">>/XObject<<" + D.xobjects + ">>>>";
    std::string pdf = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    std::vector<size_t> at;
    for (size_t k = 0; k < D.objs.size(); k++) {
        at.push_back(pdf.size());
        pdf += std::to_string(k + 1) + " 0 obj\n" + D.objs[k] + "\nendobj\n";
    }
    size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(D.objs.size() + 1) + "\n0000000000 65535 f \n";
    char line[32];
    for (size_t o : at) snprintf(line, sizeof line, "%010zu 00000 n \n", o), pdf += line;
    pdf += "trailer\n<</Size " + std::to_string(D.objs.size() + 1) + "/Root 1 0 R>>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return pdf;
}
