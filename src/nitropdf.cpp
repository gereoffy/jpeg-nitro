// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitropdf: see nitropdf.h. A page's content is interpreted once, the first time it is drawn,
// into a display list in the page's coordinates (points, y down, its crop box's top left at 0, 0,
// turned by /Rotate): paths with the matrix they were painted under (strokes keep their width in
// their own space), the clipping paths as a chain each item points into, images decoded into
// premultiplied surfaces. Consecutive opaque strokes of the same state (CAD plots write a line per
// "m l S") are one path. Lines are never thinner than a device pixel, as PDF viewers draw thin
// lines by default (Acrobat's "enhance thin lines"), or 0.1 pt plan lines would fade away.
#include "nitropdf.h"
#include "nitro_draw.h"
#include "nitro_font.h"   // (stand-ins for the fonts not embedded)
#include "nitropdf_glyphs.h"
#include "nitro_convert.h"   // (nc_jpeg_to_bgra)
#include "nitro_cmyk.h"
#include "nitrojpeg.h"
extern "C" {
#include "nitrotiff.h"   // (nt_ccitt)
}

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <pdfparse3.h>
#include <plutovg.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using pdfparse3::Val;
namespace pp = pdfparse3;

namespace {

const int MAX_DEPTH = 12;                  // forms within forms, patterns, functions within functions
const size_t MAX_OPS = 20u << 20;          // a page's operators (forms included)
const uint64_t MAX_IMAGE_PIXELS = 16u << 20;   // an image beyond this is decoded smaller
const size_t MAX_MERGED = 4096;            // path elements merged into one stroke

inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

// --- functions (Type 0 sampled, 2 exponential, 3 stitching, 4 PostScript calculator) -----------

struct Func {
    int type = -1, nin = 1, nout = 1;
    std::vector<float> domain, range, c0, c1, bounds, encode, decode, samples;
    std::vector<int> size;
    float N = 1;
    std::vector<std::shared_ptr<Func>> subs;   // type 3's, or an array of one-output functions (type 100)
    struct Ins {
        uint8_t op;
        float v;      // a number to push
        int jump;     // JZ / JMP target
    };
    std::vector<Ins> code;   // type 4

    void eval(const float *in, float *out) const;
    void evalPs(const float *in, float *out) const;
};

enum PsOp : uint8_t {
    PS_NUM, PS_JZ, PS_JMP, PS_ABS, PS_ADD, PS_ATAN, PS_CEIL, PS_COS, PS_CVI, PS_CVR, PS_DIV, PS_EXP, PS_FLOOR, PS_IDIV, PS_LN,
    PS_LOG, PS_MOD, PS_MUL, PS_NEG, PS_ROUND, PS_SIN, PS_SQRT, PS_SUB, PS_TRUNC, PS_AND, PS_BITSHIFT, PS_EQ, PS_FALSE, PS_GE,
    PS_GT, PS_LE, PS_LT, PS_NE, PS_NOT, PS_OR, PS_TRUE, PS_XOR, PS_COPY, PS_DUP, PS_EXCH, PS_INDEX, PS_POP, PS_ROLL
};
const struct {
    const char *name;
    PsOp op;
} PS_NAMES[] = {{"abs", PS_ABS},     {"add", PS_ADD},     {"atan", PS_ATAN},       {"ceiling", PS_CEIL}, {"cos", PS_COS},   {"cvi", PS_CVI},
                {"cvr", PS_CVR},     {"div", PS_DIV},     {"exp", PS_EXP},         {"floor", PS_FLOOR},  {"idiv", PS_IDIV}, {"ln", PS_LN},
                {"log", PS_LOG},     {"mod", PS_MOD},     {"mul", PS_MUL},         {"neg", PS_NEG},      {"round", PS_ROUND}, {"sin", PS_SIN},
                {"sqrt", PS_SQRT},   {"sub", PS_SUB},     {"truncate", PS_TRUNC},  {"and", PS_AND},      {"bitshift", PS_BITSHIFT},
                {"eq", PS_EQ},       {"false", PS_FALSE}, {"ge", PS_GE},           {"gt", PS_GT},        {"le", PS_LE},    {"lt", PS_LT},
                {"ne", PS_NE},       {"not", PS_NOT},     {"or", PS_OR},           {"true", PS_TRUE},    {"xor", PS_XOR},  {"copy", PS_COPY},
                {"dup", PS_DUP},     {"exch", PS_EXCH},   {"index", PS_INDEX},     {"pop", PS_POP},      {"roll", PS_ROLL}};

// a block appended to code, its jumps (relative to its own start) moved with it
void appendBlock(std::vector<Func::Ins> &code, const std::vector<Func::Ins> &blk) {
    int at = (int)code.size();
    for (Func::Ins i : blk) {
        if (i.op == PS_JZ || i.op == PS_JMP) i.jump += at;
        code.push_back(i);
    }
}

// a Type 4 program: its braces as conditional jumps ("{a} if": JZ end, a; "{a} {b} ifelse": JZ else, a, JMP end, b)
bool compilePs(const char *s, size_t n, std::vector<Func::Ins> &code) {
    size_t p = 0;
    std::vector<std::vector<Func::Ins>> stack(1);
    std::vector<std::vector<std::vector<Func::Ins>>> pending(1);   // the blocks waiting for if / ifelse at each level
    while (p < n) {
        char c = s[p];
        if (c == '{') {
            stack.emplace_back();
            pending.emplace_back();
            p++;
            continue;
        }
        if (c == '}') {
            if (stack.size() < 2) return false;
            std::vector<Func::Ins> blk = std::move(stack.back());
            stack.pop_back();
            pending.pop_back();
            pending.back().push_back(std::move(blk));
            p++;
            continue;
        }
        if (c <= ' ' || c == '%') {
            if (c == '%')
                while (p < n && s[p] != '\n' && s[p] != '\r') p++;
            p++;
            continue;
        }
        size_t q = p;
        while (q < n && s[q] > ' ' && s[q] != '{' && s[q] != '}') q++;
        std::string tok(s + p, q - p);
        p = q;
        std::vector<Func::Ins> &cur = stack.back();
        if (tok == "if" || tok == "ifelse") {
            auto &pend = pending[pending.size() - 1];
            bool ifelse = tok == "ifelse";
            if (pend.size() < (ifelse ? 2u : 1u)) return false;
            std::vector<Func::Ins> b = std::move(pend.back());
            pend.pop_back();
            if (ifelse) {
                std::vector<Func::Ins> a = std::move(pend.back());
                pend.pop_back();
                size_t jz = cur.size();
                cur.push_back({PS_JZ, 0, 0});
                appendBlock(cur, a);
                size_t jmp = cur.size();
                cur.push_back({PS_JMP, 0, 0});
                cur[jz].jump = (int)cur.size();
                appendBlock(cur, b);
                cur[jmp].jump = (int)cur.size();
            } else {
                size_t jz = cur.size();
                cur.push_back({PS_JZ, 0, 0});
                appendBlock(cur, b);
                cur[jz].jump = (int)cur.size();
            }
            continue;
        }
        if (!pending.back().empty()) return false;   // (a block not followed by if / ifelse)
        char *end = nullptr;
        double v = strtod(tok.c_str(), &end);
        if (end && *end == 0 && !tok.empty()) {
            cur.push_back({PS_NUM, (float)v, 0});
            continue;
        }
        bool found = false;
        for (auto &pn : PS_NAMES)
            if (tok == pn.name) {
                cur.push_back({(uint8_t)pn.op, 0, 0});
                found = true;
                break;
            }
        if (!found) return false;
    }
    // the program is the one outermost block
    if (stack.size() != 1 || pending[0].size() != 1 || !stack[0].empty()) return false;
    code = std::move(pending[0][0]);
    return true;
}

void Func::evalPs(const float *in, float *out) const {
    float st[100];
    int sp = 0;
    for (int k = 0; k < nin && sp < 100; k++) st[sp++] = clampf(in[k], domain[2 * k], domain[2 * k + 1]);
    size_t pc = 0;
    int steps = 0;
    auto pop = [&]() { return sp > 0 ? st[--sp] : 0.f; };
    auto push = [&](float v) {
        if (sp < 100) st[sp++] = v;
    };
    while (pc < code.size() && steps++ < 100000) {
        const Ins &i = code[pc++];
        float a, b;
        switch (i.op) {
        case PS_NUM: push(i.v); break;
        case PS_JZ:
            if (pop() == 0) pc = i.jump;
            break;
        case PS_JMP: pc = i.jump; break;
        case PS_ABS: push(fabsf(pop())); break;
        case PS_ADD: b = pop(), a = pop(), push(a + b); break;
        case PS_ATAN: {
            b = pop(), a = pop();
            float d = atan2f(a, b) * 180 / (float)M_PI;
            push(d < 0 ? d + 360 : d);
            break;
        }
        case PS_CEIL: push(ceilf(pop())); break;
        case PS_COS: push(cosf(pop() * (float)M_PI / 180)); break;
        case PS_CVI: push(truncf(pop())); break;
        case PS_CVR: break;
        case PS_DIV: b = pop(), a = pop(), push(b != 0 ? a / b : 0); break;
        case PS_EXP: b = pop(), a = pop(), push(powf(a, b)); break;
        case PS_FLOOR: push(floorf(pop())); break;
        case PS_IDIV: b = pop(), a = pop(), push((int)b ? (float)((int)a / (int)b) : 0); break;
        case PS_LN: a = pop(), push(a > 0 ? logf(a) : 0); break;
        case PS_LOG: a = pop(), push(a > 0 ? log10f(a) : 0); break;
        case PS_MOD: b = pop(), a = pop(), push((int)b ? (float)((int)a % (int)b) : 0); break;
        case PS_MUL: b = pop(), a = pop(), push(a * b); break;
        case PS_NEG: push(-pop()); break;
        case PS_ROUND: push(floorf(pop() + 0.5f)); break;
        case PS_SIN: push(sinf(pop() * (float)M_PI / 180)); break;
        case PS_SQRT: a = pop(), push(a > 0 ? sqrtf(a) : 0); break;
        case PS_SUB: b = pop(), a = pop(), push(a - b); break;
        case PS_TRUNC: push(truncf(pop())); break;
        case PS_AND: b = pop(), a = pop(), push((float)((int)a & (int)b)); break;
        case PS_OR: b = pop(), a = pop(), push((float)((int)a | (int)b)); break;
        case PS_XOR: b = pop(), a = pop(), push((float)((int)a ^ (int)b)); break;
        case PS_NOT: a = pop(), push(a == 0 ? 1.f : a == 1 ? 0.f : (float)~(int)a); break;
        case PS_BITSHIFT: {
            b = pop(), a = pop();
            int s = (int)b, v = (int)a;
            push((float)(s >= 0 ? (s < 31 ? v << s : 0) : (-s < 31 ? v >> -s : 0)));
            break;
        }
        case PS_EQ: b = pop(), a = pop(), push(a == b); break;
        case PS_NE: b = pop(), a = pop(), push(a != b); break;
        case PS_GE: b = pop(), a = pop(), push(a >= b); break;
        case PS_GT: b = pop(), a = pop(), push(a > b); break;
        case PS_LE: b = pop(), a = pop(), push(a <= b); break;
        case PS_LT: b = pop(), a = pop(), push(a < b); break;
        case PS_TRUE: push(1); break;
        case PS_FALSE: push(0); break;
        case PS_COPY: {
            int k = (int)pop();
            if (k < 0 || k > sp || sp + k > 100) break;
            for (int j = 0; j < k; j++) st[sp + j] = st[sp - k + j];
            sp += k;
            break;
        }
        case PS_DUP:
            if (sp > 0) push(st[sp - 1]);
            break;
        case PS_EXCH:
            if (sp > 1) std::swap(st[sp - 1], st[sp - 2]);
            break;
        case PS_INDEX: {
            int k = (int)pop();
            if (k >= 0 && k < sp) push(st[sp - 1 - k]);
            break;
        }
        case PS_POP: pop(); break;
        case PS_ROLL: {
            int j = (int)pop(), k = (int)pop();
            if (k <= 0 || k > sp) break;
            j = ((j % k) + k) % k;
            std::rotate(st + sp - k, st + sp - j, st + sp);
            break;
        }
        }
    }
    for (int k = nout - 1; k >= 0; k--) {
        float v = pop();
        out[k] = range.size() >= 2u * nout ? clampf(v, range[2 * k], range[2 * k + 1]) : v;
    }
}

void Func::eval(const float *in, float *out) const {
    switch (type) {
    case 100:   // an array of one-output functions
        for (size_t k = 0; k < subs.size(); k++) subs[k]->eval(in, out + k);
        return;
    case 2: {
        float x = clampf(in[0], domain[0], domain[1]);
        float t = N == 1 ? x : x > 0 ? powf(x, N) : (N == (int)N ? powf(x, N) : 0);
        for (int k = 0; k < nout; k++) {
            float v = c0[k] + t * (c1[k] - c0[k]);
            out[k] = range.size() >= 2u * nout ? clampf(v, range[2 * k], range[2 * k + 1]) : v;
        }
        return;
    }
    case 3: {
        float x = clampf(in[0], domain[0], domain[1]);
        size_t k = 0;
        while (k < bounds.size() && x >= bounds[k]) k++;
        float lo = k == 0 ? domain[0] : bounds[k - 1], hi = k == bounds.size() ? domain[1] : bounds[k];
        float e0 = encode[2 * k], e1 = encode[2 * k + 1];
        float t = hi > lo ? e0 + (x - lo) / (hi - lo) * (e1 - e0) : e0;
        subs[k]->eval(&t, out);
        return;
    }
    case 4: evalPs(in, out); return;
    case 0: {
        // multilinear interpolation between the samples (up to 4 inputs; more: the nearest)
        float e[8];
        int i0[8];
        float f[8];
        for (int k = 0; k < nin; k++) {
            float x = clampf(in[k], domain[2 * k], domain[2 * k + 1]);
            float d = domain[2 * k + 1] - domain[2 * k];
            e[k] = encode[2 * k] + (d != 0 ? (x - domain[2 * k]) / d : 0) * (encode[2 * k + 1] - encode[2 * k]);
            e[k] = clampf(e[k], 0, (float)(size[k] - 1));
            i0[k] = std::min((int)e[k], size[k] - 1);
            f[k] = e[k] - i0[k];
        }
        auto at = [&](const int *idx, int o) {
            size_t pos = 0, mul = 1;
            for (int k = 0; k < nin; k++) pos += idx[k] * mul, mul *= size[k];
            return samples[pos * nout + o];
        };
        int corners = nin <= 4 ? 1 << nin : 1;
        for (int o = 0; o < nout; o++) {
            float v = 0;
            for (int c = 0; c < corners; c++) {
                int idx[8];
                float w = 1;
                for (int k = 0; k < nin; k++) {
                    bool hi = nin <= 4 && (c >> k & 1);
                    idx[k] = std::min(i0[k] + (hi ? 1 : 0), size[k] - 1);
                    w *= nin <= 4 ? (hi ? f[k] : 1 - f[k]) : 1;
                }
                v += w * at(idx, o);
            }
            float d0 = decode[2 * o], d1 = decode[2 * o + 1];
            v = d0 + v * (d1 - d0);
            out[o] = range.size() >= 2u * nout ? clampf(v, range[2 * o], range[2 * o + 1]) : v;
        }
        return;
    }
    default:
        for (int k = 0; k < nout; k++) out[k] = 0;
    }
}

// --- the document ------------------------------------------------------------------

struct CS;

struct Img {
    plutovg_surface_t *s = nullptr;
    std::unique_ptr<nd::Mips> mips;   // (made once decoded: finish())
    void finish() { mips = std::make_unique<nd::Mips>(s); }
    ~Img() {
        mips.reset();
        plutovg_surface_destroy(s);
    }
};

struct PathRef {   // (a plutovg path shared by items: a fill and its stroke)
    plutovg_path_t *p;
    explicit PathRef(plutovg_path_t *path) : p(path) {}
    ~PathRef() { plutovg_path_destroy(p); }
};

struct Clip {
    int parent;
    std::shared_ptr<PathRef> path;
    plutovg_matrix_t m;
    bool evenOdd;
    bool rect;     // an upright rectangle: its box (what is wholly inside it, it does not clip)
    nd::Box box;   // in the page's coordinates, the parents' too
};

struct Item {
    enum : uint8_t { FILL, STROKE, IMAGE } kind;
    bool evenOdd = false;
    uint8_t cap = 0, join = 0;
    int clip = -1;
    int clipOwn = -1;   // (a stroke: its clip before the rectangles around it were left out (clipBox);
    float clipGap = 0;  // how far inside them it is: a line drawn wider than it is (1 pixel) may reach out)
    uint32_t rgb = 0;
    float alpha = 1, width = 1, miter = 10, dashOffset = 0;
    plutovg_matrix_t m;
    std::shared_ptr<PathRef> path;
    std::shared_ptr<std::vector<float>> dash;
    std::shared_ptr<Img> img;
    int segments = 0;   // (merged strokes: the path's elements so far)
    bool text = false;  // (a glyph run's fill: merged with the next)
    nd::Box box;
};

struct Font;

struct PageDL {
    std::atomic<bool> built{false};
    std::vector<Item> items;
    std::vector<Clip> clips;
};

}  // namespace

struct nitropdf {
    std::string data;
    pp::Doc doc;
    std::vector<pp::Page> pages;
    std::vector<std::unique_ptr<PageDL>> dl;
    int background = 0;
    std::mutex lock;   // (pdfparse3's document and the caches: one page read at a time)
    // the caches, by object number
    std::unordered_map<uint32_t, std::shared_ptr<CS>> spaces;
    std::unordered_map<uint32_t, std::shared_ptr<Func>> funcs;
    // large resource dictionaries indexed by name (resource()): by where their pairs are
    std::unordered_map<const pp::Val *, std::unordered_map<std::string_view, pp::Val>> names;
    std::unordered_map<uint32_t, std::weak_ptr<Img>> images;
    // optional content: the groups hidden by the default configuration
    std::unordered_set<uint32_t> ocOff;
    bool ocBaseOff = false;
    std::unordered_set<uint32_t> ocOn;
    // the fonts, by object number, with the FreeType library their embedded programs are read by
    std::unordered_map<uint32_t, std::shared_ptr<Font>> fonts;
    FT_Library ft = nullptr;
    ~nitropdf() {
        fonts.clear();   // (their faces before the library)
        if (ft) FT_Done_FreeType(ft);
    }
};

namespace {

// --- colour spaces -----------------------------------------------------------------

struct CS {
    enum K { GRAY, RGB, CMYK, LAB, INDEXED, TINT, PATTERN, NONE } k = GRAY;
    int n = 1;
    std::shared_ptr<CS> base;
    std::shared_ptr<Func> tint;
    std::vector<uint8_t> lookup;
    int hival = 0;
    float wp[3] = {0.9505f, 1, 1.089f}, range[4] = {-100, 100, -100, 100};

    void initial(float *c) const {
        for (int i = 0; i < n; i++) c[i] = k == CMYK && i == 3 ? 1 : k == TINT ? 1 : 0;
    }
    void toRGB(const float *c, float *rgb) const {
        switch (k) {
        case GRAY:
        case PATTERN: rgb[0] = rgb[1] = rgb[2] = clampf(c[0], 0, 1); break;
        case RGB:
            for (int i = 0; i < 3; i++) rgb[i] = clampf(c[i], 0, 1);
            break;
        case CMYK: ncm_color(c, rgb); break;   // (through a printing profile, as Acrobat shows it)
        case LAB: {
            float L = clampf(c[0], 0, 100), a = clampf(c[1], range[0], range[1]), b = clampf(c[2], range[2], range[3]);
            float fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
            auto g = [](float t) { return t > 6.f / 29 ? t * t * t : 3 * (6.f / 29) * (6.f / 29) * (t - 4.f / 29); };
            // XYZ relative to its white point, scaled to D50's, then sRGB (its matrix adapted to D50 by Bradford's)
            float X = 0.9642f * g(fx), Y = g(fy), Z = 0.8249f * g(fz);
            float r = 3.1338561f * X - 1.6168667f * Y - 0.4906146f * Z, gg = -0.9787684f * X + 1.9161415f * Y + 0.0334540f * Z,
                  bb = 0.0719453f * X - 0.2289914f * Y + 1.4052427f * Z;
            auto gam = [](float v) { return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(std::max(v, 0.f), 1 / 2.4f) - 0.055f; };
            rgb[0] = clampf(gam(r), 0, 1), rgb[1] = clampf(gam(gg), 0, 1), rgb[2] = clampf(gam(bb), 0, 1);
            break;
        }
        case INDEXED: {
            int i = std::max(0, std::min(hival, (int)lroundf(c[0])));
            float bc[32];
            int bn = base ? base->n : 1;
            for (int j = 0; j < bn && j < 32; j++) {
                size_t o = (size_t)i * bn + j;
                bc[j] = o < lookup.size() ? lookup[o] / 255.f : 0;
            }
            if (base && base->k == LAB)   // (a Lab lookup holds its components scaled to 0..255 over their range)
                for (int j = 0; j < 3; j++) bc[j] = j == 0 ? bc[0] * 100 : base->range[2 * (j - 1)] + bc[j] * (base->range[2 * j - 1] - base->range[2 * (j - 1)]);
            if (base) base->toRGB(bc, rgb);
            else rgb[0] = rgb[1] = rgb[2] = 0;
            break;
        }
        case TINT: {
            float out[32] = {0};
            if (tint) tint->eval(c, out);
            if (base) base->toRGB(out, rgb);
            else rgb[0] = rgb[1] = rgb[2] = 1 - clampf(c[0], 0, 1);
            break;
        }
        case NONE: rgb[0] = rgb[1] = rgb[2] = 1; break;
        }
    }
};

inline uint32_t packRgb(const float *rgb) {
    return (uint32_t)lroundf(rgb[0] * 255) << 16 | (uint32_t)lroundf(rgb[1] * 255) << 8 | (uint32_t)lroundf(rgb[2] * 255);
}

std::vector<float> nums(pp::Doc &doc, Val v) {
    std::vector<float> out;
    v = doc.resolve(v);
    for (size_t k = 0; k < v.size(); k++) out.push_back((float)doc.resolve(v.at(k)).num());
    return out;
}

std::shared_ptr<Func> loadFunc(nitropdf &P, Val ref, int depth);

std::shared_ptr<Func> loadFuncUncached(nitropdf &P, Val ref, int depth) {
    pp::Doc &doc = P.doc;
    Val v = doc.resolve(ref);
    if (depth > MAX_DEPTH) return nullptr;
    auto f = std::make_shared<Func>();
    if (v.t == pp::ARRAY) {   // (n one-output functions)
        f->type = 100;
        for (size_t k = 0; k < v.size(); k++) {
            auto s = loadFunc(P, v.at(k), depth + 1);
            if (!s) return nullptr;
            f->subs.push_back(s);
        }
        f->nout = (int)f->subs.size();
        return f;
    }
    Val d = v.t == pp::DICT ? v : Val();
    const pp::Stream *st = ref.t == pp::REF ? doc.stream(ref.oid()) : nullptr;
    if (d.t != pp::DICT) return nullptr;
    f->type = (int)doc.get(d, "FunctionType").num(-1);
    f->domain = nums(doc, d.get("Domain"));
    f->range = nums(doc, d.get("Range"));
    if (f->domain.size() < 2) f->domain = {0, 1};
    f->nin = (int)f->domain.size() / 2;
    if (f->nin > 8) return nullptr;
    if (f->range.size() >= 2) f->nout = (int)f->range.size() / 2;
    switch (f->type) {
    case 2: {
        f->c0 = nums(doc, d.get("C0"));
        f->c1 = nums(doc, d.get("C1"));
        if (f->c0.empty()) f->c0 = {0};
        if (f->c1.empty()) f->c1 = {1};
        f->nout = (int)std::min(f->c0.size(), f->c1.size());
        f->N = (float)doc.get(d, "N").num(1);
        return f->nout > 0 && f->nout <= 32 ? f : nullptr;
    }
    case 3: {
        Val fs = doc.get(d, "Functions");
        for (size_t k = 0; k < fs.size(); k++) {
            auto s = loadFunc(P, fs.at(k), depth + 1);
            if (!s) return nullptr;
            f->subs.push_back(s);
        }
        f->bounds = nums(doc, d.get("Bounds"));
        f->encode = nums(doc, d.get("Encode"));
        if (f->subs.empty() || f->bounds.size() + 1 != f->subs.size()) return nullptr;
        while (f->encode.size() < 2 * f->subs.size()) f->encode.push_back(f->encode.size() & 1);
        f->nout = f->subs[0]->nout;
        return f;
    }
    case 0: {
        if (!st || f->range.size() < 2) return nullptr;
        for (float s : nums(doc, d.get("Size"))) f->size.push_back(std::max(1, (int)s));
        if ((int)f->size.size() != f->nin) return nullptr;
        int bps = (int)doc.get(d, "BitsPerSample").num(8);
        if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 12 && bps != 16 && bps != 24 && bps != 32) return nullptr;
        f->encode = nums(doc, d.get("Encode"));
        if (f->encode.size() < 2u * f->nin) {
            f->encode.clear();
            for (int s : f->size) f->encode.push_back(0), f->encode.push_back((float)(s - 1));
        }
        f->decode = nums(doc, d.get("Decode"));
        if (f->decode.size() < 2u * f->nout) f->decode = f->range;
        uint64_t count = (uint64_t)f->nout;
        for (int s : f->size) count *= s;
        if (count > (16u << 20)) return nullptr;
        pp::Decoded dec;
        doc.decode(*st, dec, 64u << 20);
        const uint8_t *b = (const uint8_t *)dec.data.data();
        uint64_t bits = (uint64_t)dec.data.size() * 8;
        f->samples.resize((size_t)count);
        double maxv = std::pow(2.0, bps) - 1;
        for (uint64_t i = 0; i < count; i++) {
            uint64_t at = i * bps;
            uint32_t v = 0;
            if (at + bps <= bits)
                for (int k = 0; k < bps; k++) v = v << 1 | (b[(at + k) >> 3] >> (7 - ((at + k) & 7)) & 1);
            f->samples[i] = (float)(v / maxv);
        }
        return f;
    }
    case 4: {
        if (!st) return nullptr;
        pp::Decoded dec;
        doc.decode(*st, dec, 1u << 20);
        if (!compilePs(dec.data.data(), dec.data.size(), f->code)) return nullptr;
        return f->nout > 0 && f->nout <= 32 ? f : nullptr;
    }
    }
    return nullptr;
}

std::shared_ptr<Func> loadFunc(nitropdf &P, Val ref, int depth) {
    if (ref.t == pp::REF) {
        auto it = P.funcs.find(ref.oid());
        if (it != P.funcs.end()) return it->second;
        P.funcs[ref.oid()] = nullptr;   // (a cycle)
        auto f = loadFuncUncached(P, ref, depth);
        P.funcs[ref.oid()] = f;
        return f;
    }
    return loadFuncUncached(P, ref, depth);
}

std::shared_ptr<CS> deviceCS(CS::K k) {
    static std::shared_ptr<CS> g = std::make_shared<CS>(), rgb, cmyk;
    if (k == CS::GRAY) return g;
    if (k == CS::RGB) {
        static std::shared_ptr<CS> s = [] {
            auto c = std::make_shared<CS>();
            c->k = CS::RGB, c->n = 3;
            return c;
        }();
        return s;
    }
    static std::shared_ptr<CS> s = [] {
        auto c = std::make_shared<CS>();
        c->k = CS::CMYK, c->n = 4;
        return c;
    }();
    return s;
}

std::shared_ptr<CS> loadCS(nitropdf &P, Val v, Val res, int depth);
Val resource(nitropdf &P, Val res, const char *category, Val name);

std::shared_ptr<CS> loadCSUncached(nitropdf &P, Val v, Val res, int depth) {
    pp::Doc &doc = P.doc;
    if (depth > MAX_DEPTH) return nullptr;
    v = doc.resolve(v);
    if (v.t == pp::NAME) {
        if (v.is("DeviceGray") || v.is("G") || v.is("CalGray")) return deviceCS(CS::GRAY);
        if (v.is("DeviceRGB") || v.is("RGB") || v.is("CalRGB")) return deviceCS(CS::RGB);
        if (v.is("DeviceCMYK") || v.is("CMYK")) return deviceCS(CS::CMYK);
        if (v.is("Pattern")) {
            auto c = std::make_shared<CS>();
            c->k = CS::PATTERN;
            return c;
        }
        Val named = resource(P, res, "ColorSpace", v);
        if (doc.resolve(named).t == pp::NUL) return nullptr;
        return loadCS(P, named, Val(), depth + 1);
    }
    if (v.t != pp::ARRAY || !v.size()) return nullptr;
    Val kind = doc.resolve(v.at(0));
    auto c = std::make_shared<CS>();
    if (kind.is("DeviceGray") || kind.is("CalGray") || kind.is("G")) return deviceCS(CS::GRAY);
    if (kind.is("DeviceRGB") || kind.is("CalRGB") || kind.is("RGB")) return deviceCS(CS::RGB);
    if (kind.is("DeviceCMYK") || kind.is("CMYK")) return deviceCS(CS::CMYK);
    if (kind.is("ICCBased")) {   // (its alternate, or the device space of its number of components)
        Val d = doc.resolve(v.at(1));
        Val alt = d.get("Alternate");
        if (alt.t != pp::NUL) {
            auto a = loadCS(P, alt, res, depth + 1);
            if (a) return a;
        }
        int n = (int)doc.get(d, "N").num(3);
        return deviceCS(n == 1 ? CS::GRAY : n == 4 ? CS::CMYK : CS::RGB);
    }
    if (kind.is("Lab")) {
        Val d = doc.resolve(v.at(1));
        c->k = CS::LAB, c->n = 3;
        auto w = nums(doc, d.get("WhitePoint"));
        auto r = nums(doc, d.get("Range"));
        if (w.size() == 3) memcpy(c->wp, w.data(), sizeof c->wp);
        if (r.size() == 4) memcpy(c->range, r.data(), sizeof c->range);
        return c;
    }
    if (kind.is("Indexed") || kind.is("I")) {
        c->k = CS::INDEXED, c->n = 1;
        c->base = loadCS(P, v.at(1), res, depth + 1);
        if (!c->base) return nullptr;
        c->hival = std::max(0, std::min(255, (int)doc.resolve(v.at(2)).num(0)));
        Val lk = v.at(3);
        Val lr = doc.resolve(lk);
        if (lr.t == pp::STR) c->lookup.assign((const uint8_t *)lr.s, (const uint8_t *)lr.s + lr.n);
        else if (lk.t == pp::REF) {
            if (const pp::Stream *st = doc.stream(lk.oid())) {
                pp::Decoded dec;
                doc.decode(*st, dec, 1u << 20);
                c->lookup.assign(dec.data.begin(), dec.data.end());
            }
        }
        return c;
    }
    if (kind.is("Separation") || kind.is("DeviceN")) {
        bool sep = kind.is("Separation");
        Val names = doc.resolve(v.at(1));
        c->n = sep ? 1 : std::max(1, std::min(32, (int)names.size()));
        if (sep && names.is("None")) {
            c->k = CS::NONE;
            return c;
        }
        c->k = CS::TINT;
        c->base = loadCS(P, v.at(2), res, depth + 1);
        c->tint = loadFunc(P, v.at(3), depth + 1);
        if (!c->base || !c->tint) return nullptr;
        return c;
    }
    if (kind.is("Pattern")) {
        c->k = CS::PATTERN;
        if (v.size() > 1) c->base = loadCS(P, v.at(1), res, depth + 1);
        return c;
    }
    return nullptr;
}

std::shared_ptr<CS> loadCS(nitropdf &P, Val v, Val res, int depth) {
    if (v.t == pp::REF) {
        auto it = P.spaces.find(v.oid());
        if (it != P.spaces.end()) return it->second;
        P.spaces[v.oid()] = nullptr;
        auto c = loadCSUncached(P, v, res, depth);
        P.spaces[v.oid()] = c;
        return c;
    }
    return loadCSUncached(P, v, res, depth);
}

// --- images ------------------------------------------------------------------------

// The samples of an image, rows of w * n components of bpc bits, to 8 bits per component (16-bit
// ones rounded; fewer bits scaled up), w x h of them, every step-th source pixel (a huge image smaller)
struct Samples {
    int w = 0, h = 0, n = 1;
    std::vector<uint8_t> v;   // h rows of w * n
    std::vector<uint16_t> raw;   // (the samples as they were, for /Mask colour keys; only when asked)
};

bool unpack(const uint8_t *d, size_t len, int W, int H, int n, int bpc, int step, Samples &out, bool keepRaw) {
    if (W <= 0 || H <= 0 || n <= 0 || n > 32 || step <= 0) return false;
    size_t rowBytes = ((size_t)W * n * bpc + 7) / 8;
    out.w = (W + step - 1) / step, out.h = (H + step - 1) / step, out.n = n;
    out.v.assign((size_t)out.w * out.h * n, 0);
    if (keepRaw) out.raw.assign(out.v.size(), 0);
    int maxv = (1 << std::min(bpc, 16)) - 1;
    for (int y = 0; y < out.h; y++) {
        size_t sy = (size_t)y * step;
        if ((sy + 1) * rowBytes > len) break;   // (missing rows: left 0)
        const uint8_t *row = d + sy * rowBytes;
        uint8_t *o = &out.v[(size_t)y * out.w * n];
        for (int x = 0; x < out.w; x++) {
            size_t sx = (size_t)x * step;
            for (int c = 0; c < n; c++) {
                size_t i = sx * n + c;
                unsigned s;
                if (bpc == 8) s = row[i];
                else if (bpc == 16) s = (unsigned)row[2 * i] << 8 | row[2 * i + 1];
                else s = (row[i * bpc / 8] >> (8 - bpc - (i * bpc) % 8)) & maxv;
                if (keepRaw) out.raw[((size_t)y * out.w + x) * n + c] = (uint16_t)s;
                o[x * n + c] = (uint8_t)(bpc == 8 ? s : bpc == 16 ? s >> 8 : s * 255 / maxv);
            }
        }
    }
    return true;
}

plutovg_surface_t *newSurface(int w, int h) { return w > 0 && h > 0 ? plutovg_surface_create(w, h) : nullptr; }

// A JPEG (DCTDecode) as an opaque surface: by nitrojpeg, in parallel, where it can (1, 3 or 4
// components: CMYK / YCCK through nitro_cmyk's profile, the values as stored, inverted by
// an inverting /Decode: Adobe's inverted ones come with one, Adobe's marker alone says nothing),
// else by stb_image; nullptr if neither reads it. *ncomp: its components (0 if not known).
plutovg_surface_t *jpegSurface(const std::string &data, bool decodeInverted, int *ncomp) {
    const uint8_t *d = (const uint8_t *)data.data();
    nj_info ji;
    *ncomp = 0;
    if (!nj_read_info(d, data.size(), &ji)) {
        *ncomp = ji.ncomp;
        if (ji.supported && (uint64_t)ji.width * ji.height <= (128u << 20)) {
            size_t pitch[4] = {0}, total = 0, at[4] = {0};
            for (int c = 0; c < ji.ncomp; c++) {
                pitch[c] = (size_t)ji.plane_w[c];
                at[c] = total;
                total += pitch[c] * ji.plane_h[c];
            }
            std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[total ? total : 1]);
            plutovg_surface_t *s = buf ? newSurface(ji.width, ji.height) : nullptr;
            uint8_t *planes[4] = {buf.get() + at[0], buf.get() + at[1], buf.get() + at[2], buf.get() + at[3]};
            if (s && !nj_decode_planes(d, data.size(), &ji, planes, pitch, 0, nullptr)) {
                nc_jpeg_to_bgra(&ji, planes, pitch, decodeInverted, ncm_row_bgra, plutovg_surface_get_data(s), (size_t)plutovg_surface_get_stride(s));
                return s;
            }
            plutovg_surface_destroy(s);
        }
        if (ji.ncomp == 4) return nullptr;   // (stb_image draws CMYK black)
    }
    return plutovg_surface_load_from_image_data(data.data(), (int)std::min<size_t>(data.size(), INT32_MAX));
}

// a surface's pixels scaled to w x h, nearest (masks of another size than their image)
std::vector<uint8_t> resampleGray(const Samples &m, int w, int h) {
    std::vector<uint8_t> out((size_t)w * h);
    for (int y = 0; y < h; y++) {
        int sy = std::min(m.h - 1, (int)((int64_t)y * m.h / h));
        for (int x = 0; x < w; x++) {
            int sx = std::min(m.w - 1, (int)((int64_t)x * m.w / w));
            out[(size_t)y * w + x] = m.v[((size_t)sy * m.w + sx) * m.n];
        }
    }
    return out;
}

struct ImageSource {   // an image's dictionary and its data, decoded but for an image filter
    Val dict;
    std::string data;
    std::string filter;   // DCTDecode, CCITTFaxDecode, JBIG2Decode, JPXDecode or empty
    Val parms;
};

// A resource by its name: the entry of res's category dictionary (/XObject, /Font ...), unresolved.
// A large one (a CAD export's thousands of forms on a page) is indexed the first time: looked up
// through for every operator using it, a page's build took seconds.
Val resource(nitropdf &P, Val res, const char *category, Val name) {
    pp::Doc &doc = P.doc;
    if (name.t != pp::NAME || doc.resolve(res).t != pp::DICT) return Val();
    Val d = doc.get(res, category);
    if (d.t != pp::DICT) return Val();
    if (d.size() < 64) return d.get(std::string(name.str()).c_str());
    auto &index = P.names[d.v];   // (the pairs never move: the document holds them)
    if (index.empty())
        for (size_t k = 0; k < d.size(); k++) index[d.v[2 * k].str()] = d.v[2 * k + 1];   // (the last of the same keys counts)
    auto it = index.find(name.str());
    return it != index.end() ? it->second : Val();
}

// the dictionary's value by its name or (an inline image's) abbreviation
Val key(pp::Doc &doc, Val d, const char *name, const char *abbr) {
    Val v = d.get(name);
    if (v.t == pp::NUL && abbr) v = d.get(abbr);
    return doc.resolve(v);
}

// 1-bit / gray samples of a mask (a stencil /Mask, an /SMask), w x h, its /Decode applied: 255 painted / opaque
bool maskSamples(nitropdf &P, ImageSource &src, bool stencil, Samples &out) {
    pp::Doc &doc = P.doc;
    int W = (int)key(doc, src.dict, "Width", "W").num(0), H = (int)key(doc, src.dict, "Height", "H").num(0);
    int bpc = stencil ? 1 : (int)key(doc, src.dict, "BitsPerComponent", "BPC").num(8);
    if (W <= 0 || H <= 0 || (uint64_t)W * H > (256u << 20)) return false;
    int step = 1;
    while ((uint64_t)W * H / ((uint64_t)step * step) > MAX_IMAGE_PIXELS) step++;
    std::string ccitt;
    const uint8_t *d = (const uint8_t *)src.data.data();
    size_t len = src.data.size();
    if (src.filter == "CCITTFaxDecode" || src.filter == "CCF") {
        Val p = src.parms;
        int k = (int)key(doc, p, "K", nullptr).num(0), cols = (int)key(doc, p, "Columns", nullptr).num(1728);
        bool b1 = key(doc, p, "BlackIs1", nullptr).t == pp::BOOL && key(doc, p, "BlackIs1", nullptr).i;
        bool align = key(doc, p, "EncodedByteAlign", nullptr).t == pp::BOOL && key(doc, p, "EncodedByteAlign", nullptr).i;
        size_t prow = ((size_t)cols + 7) / 8;
        ccitt.assign(prow * H, '\0');
        if (nt_ccitt(d, len, k, cols, H, b1, align, (uint8_t *)&ccitt[0], prow)) return false;
        d = (const uint8_t *)ccitt.data(), len = ccitt.size();
        bpc = 1;
        W = cols;
    } else if (src.filter == "DCTDecode" || src.filter == "DCT") {
        int nc;
        plutovg_surface_t *s = jpegSurface(src.data, false, &nc);
        if (!s) return false;
        out.w = plutovg_surface_get_width(s), out.h = plutovg_surface_get_height(s), out.n = 1;
        out.v.resize((size_t)out.w * out.h);
        const uint8_t *px = plutovg_surface_get_data(s);
        int stride = plutovg_surface_get_stride(s);
        for (int y = 0; y < out.h; y++)
            for (int x = 0; x < out.w; x++) out.v[(size_t)y * out.w + x] = px[(size_t)y * stride + 4 * x + 1];   // (green: gray)
        plutovg_surface_destroy(s);
    } else if (!src.filter.empty())
        return false;
    if (out.v.empty() && !unpack(d, len, W, H, 1, bpc, step, out, false)) return false;
    auto dec = nums(doc, key(doc, src.dict, "Decode", "D"));
    bool invert = dec.size() >= 2 && dec[0] > dec[1];
    if (stencil) invert = !invert;   // (a stencil: a 0 sample is painted)
    if (invert)
        for (uint8_t &b : out.v) b = 255 - b;
    return true;
}

bool imageSource(nitropdf &P, Val ref, ImageSource &src) {
    const pp::Stream *st = ref.t == pp::REF ? P.doc.stream(ref.oid()) : nullptr;
    if (!st) return false;
    pp::Decoded dec;
    P.doc.decode(*st, dec, 512u << 20);
    src.dict = st->dict;
    src.data.swap(dec.data);
    src.filter = dec.image;
    src.parms = dec.imageParms;
    return true;
}

// An image (XObject or inline) as a premultiplied surface; a stencil mask painted with fill (rgb, alpha).
std::shared_ptr<Img> decodeImage(nitropdf &P, ImageSource &src, Val res, uint32_t fill, float fillAlpha) {
    pp::Doc &doc = P.doc;
    Val d = src.dict;
    int W = (int)key(doc, d, "Width", "W").num(0), H = (int)key(doc, d, "Height", "H").num(0);
    if (W <= 0 || H <= 0 || (uint64_t)W * H > (1ull << 32)) return nullptr;
    Val im = key(doc, d, "ImageMask", "IM");
    bool stencil = im.t == pp::BOOL && im.i;
    auto img = std::make_shared<Img>();
    if (stencil) {
        Samples m;
        if (!maskSamples(P, src, true, m)) return nullptr;
        img->s = newSurface(m.w, m.h);
        if (!img->s) return nullptr;
        uint8_t *px = plutovg_surface_get_data(img->s);
        int stride = plutovg_surface_get_stride(img->s);
        float r = (fill >> 16 & 255) * fillAlpha, g = (fill >> 8 & 255) * fillAlpha, b = (fill & 255) * fillAlpha, a = 255 * fillAlpha;
        for (int y = 0; y < m.h; y++) {
            uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
            for (int x = 0; x < m.w; x++) {
                float k = m.v[(size_t)y * m.w + x] / 255.f;
                row[x] = (uint32_t)lroundf(a * k) << 24 | (uint32_t)lroundf(r * k) << 16 | (uint32_t)lroundf(g * k) << 8 | (uint32_t)lroundf(b * k);
            }
        }
        img->finish();
        return img;
    }
    int bpc = (int)key(doc, d, "BitsPerComponent", "BPC").num(8);
    Val csv = d.get("ColorSpace");
    if (csv.t == pp::NUL) csv = d.get("CS");
    std::shared_ptr<CS> cs = loadCS(P, csv, res, 0);
    if (!cs && src.filter != "JPXDecode") cs = deviceCS(CS::GRAY);
    int step = 1;
    while ((uint64_t)W * H / ((uint64_t)step * step) > MAX_IMAGE_PIXELS) step++;
    // the colour of each pixel: from the decoded JPEG, the CCITT bits or the samples
    Samples smp;
    Val maskv = doc.resolve(d.get("Mask"));
    bool colorKey = maskv.t == pp::ARRAY;
    if (src.filter == "DCTDecode" || src.filter == "DCT") {
        auto dec = nums(doc, key(doc, d, "Decode", "D"));
        int nc;
        img->s = jpegSurface(src.data, dec.size() >= 2 && dec[0] > dec[1], &nc);
        if (!img->s) {   // (one neither decoder reads: a grey area where the picture is)
            img->s = newSurface(1, 1);
            if (img->s) *(uint32_t *)plutovg_surface_get_data(img->s) = 0xffc0c0c0;
        }
        if (img->s && cs && cs->n == 3 && cs->k != CS::RGB) {   // (a JPEG of other components: Lab ones, through the space)
            uint8_t *px = plutovg_surface_get_data(img->s);
            int stride = plutovg_surface_get_stride(img->s), w = plutovg_surface_get_width(img->s);
            for (int y = 0; y < plutovg_surface_get_height(img->s); y++)
                for (int x = 0; x < w; x++) {
                    uint8_t *q = px + (size_t)y * stride + 4 * x;
                    float v[3] = {q[2] / 255.f, q[1] / 255.f, q[0] / 255.f}, c[3], rgb[3];
                    for (int j = 0; j < 3; j++) {
                        float lo = cs->k == CS::LAB ? (j == 0 ? 0 : cs->range[2 * (j - 1)]) : 0;
                        float hi = cs->k == CS::LAB ? (j == 0 ? 100 : cs->range[2 * j - 1]) : 1;
                        if (dec.size() >= 6) lo = dec[2 * j], hi = dec[2 * j + 1];
                        c[j] = lo + v[j] * (hi - lo);
                    }
                    cs->toRGB(c, rgb);
                    q[2] = (uint8_t)lroundf(rgb[0] * 255), q[1] = (uint8_t)lroundf(rgb[1] * 255), q[0] = (uint8_t)lroundf(rgb[2] * 255);
                }
        } else if (img->s && nc != 4 && dec.size() >= 2 && dec[0] > dec[1]) {   // (inverted: a negative gray / RGB; CMYK: above)
            uint8_t *px = plutovg_surface_get_data(img->s);
            int stride = plutovg_surface_get_stride(img->s), w = plutovg_surface_get_width(img->s);
            for (int y = 0; y < plutovg_surface_get_height(img->s); y++)
                for (int x = 0; x < w; x++) {
                    uint8_t *p = px + (size_t)y * stride + 4 * x;
                    p[0] = 255 - p[0], p[1] = 255 - p[1], p[2] = 255 - p[2];
                }
        }
    } else if (src.filter == "JPXDecode" || src.filter == "JBIG2Decode" || (!src.filter.empty() && src.filter != "CCITTFaxDecode" &&
                                                                            src.filter != "CCF")) {
        img->s = newSurface(1, 1);   // (not decoded: a light grey area)
        if (img->s) *(uint32_t *)plutovg_surface_get_data(img->s) = 0xffe0e0e0;
    } else {
        const uint8_t *dd = (const uint8_t *)src.data.data();
        size_t len = src.data.size();
        std::string ccitt;
        if (src.filter == "CCITTFaxDecode" || src.filter == "CCF") {
            Val p = src.parms;
            int k = (int)key(doc, p, "K", nullptr).num(0), cols = (int)key(doc, p, "Columns", nullptr).num(1728);
            bool b1 = key(doc, p, "BlackIs1", nullptr).t == pp::BOOL && key(doc, p, "BlackIs1", nullptr).i;
            bool align = key(doc, p, "EncodedByteAlign", nullptr).t == pp::BOOL && key(doc, p, "EncodedByteAlign", nullptr).i;
            if (cols <= 0 || (uint64_t)cols * H > (1ull << 32)) return nullptr;
            size_t prow = ((size_t)cols + 7) / 8;
            ccitt.assign(prow * H, '\0');
            if (nt_ccitt(dd, len, k, cols, H, b1, align, (uint8_t *)&ccitt[0], prow)) return nullptr;
            dd = (const uint8_t *)ccitt.data(), len = ccitt.size();
            bpc = 1, W = cols;
            cs = deviceCS(CS::GRAY);
        }
        if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) return nullptr;
        if (!unpack(dd, len, W, H, cs->n, bpc, step, smp, colorKey)) return nullptr;
        img->s = newSurface(smp.w, smp.h);
        if (!img->s) return nullptr;
        auto dec = nums(doc, key(doc, d, "Decode", "D"));
        int n = cs->n;
        int maxv = (1 << std::min(bpc, 16)) - 1;
        // each component's 0..255 value to its value in the colour space: by /Decode, or the default
        std::vector<float> lo(n), hi(n);
        for (int c = 0; c < n; c++) {
            float d0 = 0, d1 = cs->k == CS::INDEXED ? (float)maxv : cs->k == CS::LAB ? (c == 0 ? 100.f : cs->range[2 * c - 1]) : 1;
            if (cs->k == CS::LAB && c > 0) d0 = cs->range[2 * (c - 1)];
            if (dec.size() >= 2u * n) d0 = dec[2 * c], d1 = dec[2 * c + 1];
            lo[c] = d0, hi[c] = d1;
        }
        uint8_t *px = plutovg_surface_get_data(img->s);
        int stride = plutovg_surface_get_stride(img->s);
        // one component: a table of its 256 values' colours; else colour by colour (an RGB or CMYK one directly)
        uint32_t lut[256];
        if (n == 1) {
            for (int v = 0; v < 256; v++) {
                float c = lo[0] + (cs->k == CS::INDEXED ? (float)lroundf(v * maxv / 255.f) / maxv : v / 255.f) * (hi[0] - lo[0]), rgb[3];
                cs->toRGB(&c, rgb);
                lut[v] = 0xff000000u | packRgb(rgb);
            }
        }
        bool plain = dec.empty() || dec.size() < 2u * n;
        // CMYK: rows through nitro_cmyk, each component plain or inverted by its /Decode
        bool cmykRows = cs->k == CS::CMYK && n == 4, inv[4] = {false, false, false, false};
        for (int c = 0; c < 4 && cmykRows && !plain; c++) {
            if (lo[c] == 1 && hi[c] == 0) inv[c] = true;
            else if (lo[c] != 0 || hi[c] != 1) cmykRows = false;
        }
        for (int y = 0; y < smp.h; y++) {
            uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
            const uint8_t *s = &smp.v[(size_t)y * smp.w * n];
            if (cmykRows) {
                uint8_t *o = (uint8_t *)row;
                for (int x = 0; x < 4 * smp.w; x++) o[x] = inv[x & 3] ? 255 - s[x] : s[x];
                ncm_row_bgra(o, o, (size_t)smp.w);
                continue;
            }
            for (int x = 0; x < smp.w; x++, s += n) {
                if (n == 1) row[x] = lut[s[0]];
                else if (cs->k == CS::RGB && plain) row[x] = 0xff000000u | s[0] << 16 | s[1] << 8 | s[2];
                else {
                    float c[32], rgb[3];
                    for (int j = 0; j < n && j < 32; j++) c[j] = lo[j] + s[j] / 255.f * (hi[j] - lo[j]);
                    cs->toRGB(c, rgb);
                    row[x] = 0xff000000u | packRgb(rgb);
                }
            }
        }
        if (colorKey && !smp.raw.empty()) {   // (/Mask [min max ...]: those samples transparent)
            auto r = nums(doc, maskv);
            if (r.size() >= 2u * n)
                for (int y = 0; y < smp.h; y++) {
                    uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
                    for (int x = 0; x < smp.w; x++) {
                        const uint16_t *s = &smp.raw[((size_t)y * smp.w + x) * n];
                        bool in = true;
                        for (int c = 0; c < n && in; c++) in = s[c] >= r[2 * c] && s[c] <= r[2 * c + 1];
                        if (in) row[x] = 0;
                    }
                }
        }
    }
    if (!img->s) return nullptr;
    // an alpha mask: /SMask (gray: its alpha), or a stencil /Mask (its 0 samples shown)
    Val smask = d.get("SMask");
    Samples alpha;
    bool haveAlpha = false;
    if (smask.t == pp::REF) {
        ImageSource ms;
        haveAlpha = imageSource(P, smask, ms) && maskSamples(P, ms, false, alpha);
    } else if (maskv.t == pp::DICT || d.get("Mask").t == pp::REF) {
        ImageSource ms;
        haveAlpha = d.get("Mask").t == pp::REF && imageSource(P, d.get("Mask"), ms) && maskSamples(P, ms, true, alpha);
    }
    if (haveAlpha) {
        int w = plutovg_surface_get_width(img->s), h = plutovg_surface_get_height(img->s);
        if ((alpha.w > w || alpha.h > h) && (uint64_t)alpha.w * alpha.h <= MAX_IMAGE_PIXELS) {
            // a mask finer than its image (a driver's dithered transparency): the image enlarged to it
            plutovg_surface_t *big = plutovg_surface_create(alpha.w, alpha.h);
            if (big) {
                plutovg_canvas_t *c = plutovg_canvas_create(big);
                plutovg_matrix_t sm;
                plutovg_matrix_init_scale(&sm, (float)alpha.w / w, (float)alpha.h / h);
                plutovg_canvas_set_texture(c, img->s, PLUTOVG_TEXTURE_TYPE_PLAIN, 1, &sm);
                plutovg_canvas_set_operator(c, PLUTOVG_OPERATOR_SRC);
                plutovg_canvas_fill_rect(c, 0, 0, alpha.w, alpha.h);
                plutovg_canvas_destroy(c);
                plutovg_surface_destroy(img->s);
                img->s = big, w = alpha.w, h = alpha.h;
            }
        }
        std::vector<uint8_t> a = resampleGray(alpha, w, h);
        uint8_t *px = plutovg_surface_get_data(img->s);
        int stride = plutovg_surface_get_stride(img->s);
        for (int y = 0; y < h; y++) {
            uint8_t *row = px + (size_t)y * stride;
            for (int x = 0; x < w; x++) {
                unsigned k = a[(size_t)y * w + x];
                for (int c = 0; c < 4; c++) row[4 * x + c] = (uint8_t)(row[4 * x + c] * k / 255);
            }
        }
    }
    img->finish();
    return img;
}

// --- fonts -------------------------------------------------------------------------

// A glyph name's character: the glyph list's names, uniXXXX, uXXXX[XX] (0: none)
uint32_t glyphUnicode(const char *name) {
    if (!name || !*name) return 0;
    size_t lo = 0, hi = sizeof kGlyphNames / sizeof kGlyphNames[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(kGlyphNames[mid].name, name);
        if (!c) return kGlyphNames[mid].cp;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    const char *dot = strchr(name, '.');   // (a variant's name: "a.sc", "one.oldstyle")
    if (dot && dot > name) {
        std::string base(name, dot - name);
        return glyphUnicode(base.c_str());
    }
    auto hex = [](const char *s, size_t n, uint32_t &v) {
        v = 0;
        for (size_t k = 0; k < n; k++) {
            char c = s[k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return false;
            v = v << 4 | d;
        }
        return n > 0;
    };
    uint32_t v;
    size_t n = strlen(name);
    if (n == 7 && !strncmp(name, "uni", 3) && hex(name + 3, 4, v)) return v;
    if ((n == 5 || n == 6 || n == 7) && name[0] == 'u' && hex(name + 1, n - 1, v) && v <= 0x10FFFF) return v;
    return 0;
}

struct Font {
    enum Kind { SIMPLE, CID, TYPE3 } kind = SIMPLE;
    std::string program;                  // the embedded font (FreeType reads it in place)
    FT_Face face = nullptr;
    plutovg_font_face_t *sub = nullptr;   // a system font standing in for one not embedded
    int weight = 400;
    bool italic = false;
    // simple fonts and Type 3: a code's glyph, character and width
    int gid[256];
    uint32_t uni[256];
    const char *names[256];
    std::vector<std::string> nameStore;   // (the /Differences' names)
    float widths[256];
    bool haveWidth[256];
    // CID fonts: the CMap (codespace ranges; code ranges -> CIDs), CIDs -> glyphs, widths
    struct Space {
        uint32_t lo, hi;
        int bytes;
    };
    struct Range {
        uint32_t lo, hi, cid;
        int bytes;
    };
    std::vector<Space> spaces;
    std::vector<Range> ranges;
    bool identity = true, vertical = false;
    std::vector<uint16_t> cidToGid;
    bool cidIdentity = true;
    std::unordered_map<uint32_t, float> cidWidths;
    float dw = 1000;
    std::unordered_map<uint32_t, uint32_t> toUnicode;   // (codes, from /ToUnicode)
    // Type 3
    float m[6] = {0.001f, 0, 0, 0.001f, 0, 0};
    Val charProcs, resources;
    // the glyphs (glyph space: 1000 units to the em), by glyph (or a stand-in's code)
    struct Glyph {
        std::shared_ptr<PathRef> path;
        float advance = 0;   // (where the PDF gives no width)
    };
    std::unordered_map<uint32_t, Glyph> glyphs;

    Font() {
        for (int k = 0; k < 256; k++) gid[k] = -1, uni[k] = 0, names[k] = nullptr, widths[k] = 0, haveWidth[k] = false;
    }
    ~Font() {
        if (face) FT_Done_Face(face);
    }
};

// FreeType outline -> plutovg path, in glyph units (26.6 at 1000 pixels to the em)
struct Decompose {
    plutovg_path_t *p;
    bool open = false;
};
int ftMove(const FT_Vector *to, void *u) {
    Decompose *d = (Decompose *)u;
    if (d->open) plutovg_path_close(d->p);
    plutovg_path_move_to(d->p, to->x / 64.f, to->y / 64.f);
    d->open = true;
    return 0;
}
int ftLine(const FT_Vector *to, void *u) {
    plutovg_path_line_to(((Decompose *)u)->p, to->x / 64.f, to->y / 64.f);
    return 0;
}
int ftConic(const FT_Vector *c, const FT_Vector *to, void *u) {
    plutovg_path_quad_to(((Decompose *)u)->p, c->x / 64.f, c->y / 64.f, to->x / 64.f, to->y / 64.f);
    return 0;
}
int ftCubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *u) {
    plutovg_path_cubic_to(((Decompose *)u)->p, c1->x / 64.f, c1->y / 64.f, c2->x / 64.f, c2->y / 64.f, to->x / 64.f, to->y / 64.f);
    return 0;
}

// A CMap (an embedded /Encoding, or a /ToUnicode): its code space, its CID ranges and characters, or
// its characters (bfchar, bfrange); read with the content lexer, its operators after their operands.
void parseCMap(const std::string &data, Font &f, bool toUni) {
    pp::ContentLexer lx(data.data(), data.size());
    pp::Op op;
    auto code = [](Val v, int &bytes) {
        uint32_t c = 0;
        bytes = (int)std::min<uint32_t>(v.n, 4);
        for (int k = 0; k < bytes; k++) c = c << 8 | (uint8_t)v.s[k];
        return c;
    };
    auto unicode = [](Val v) {   // (UTF-16BE: the first character)
        if (v.t != pp::STR || v.n < 2) return v.t == pp::STR && v.n == 1 ? (uint32_t)(uint8_t)v.s[0] : 0u;
        uint32_t u = (uint8_t)v.s[0] << 8 | (uint8_t)v.s[1];
        if (u >= 0xD800 && u < 0xDC00 && v.n >= 4) u = 0x10000 + ((u - 0xD800) << 10) + (((uint8_t)v.s[2] << 8 | (uint8_t)v.s[3]) - 0xDC00);
        return u;
    };
    size_t entries = 0;
    while (lx.next(op) && entries < (1u << 20)) {
        std::string_view o = op.name;
        const Val *a = op.args;
        size_t n = op.nargs;
        if (o == "endcodespacerange" && !toUni) {
            for (size_t k = 0; k + 1 < n; k += 2)
                if (a[k].t == pp::STR && a[k + 1].t == pp::STR) {
                    int b0, b1;
                    uint32_t lo = code(a[k], b0), hi = code(a[k + 1], b1);
                    if (b0 >= 1 && b0 == b1) f.spaces.push_back({lo, hi, b0});
                }
        } else if (o == "endcidrange" && !toUni) {
            for (size_t k = 0; k + 2 < n; k += 3, entries++)
                if (a[k].t == pp::STR && a[k + 1].t == pp::STR && a[k + 2].isNum()) {
                    int b0, b1;
                    uint32_t lo = code(a[k], b0), hi = code(a[k + 1], b1);
                    f.ranges.push_back({lo, hi, (uint32_t)a[k + 2].num(), b0});
                }
            f.identity = false;
        } else if (o == "endcidchar" && !toUni) {
            for (size_t k = 0; k + 1 < n; k += 2, entries++)
                if (a[k].t == pp::STR && a[k + 1].isNum()) {
                    int b;
                    uint32_t c = code(a[k], b);
                    f.ranges.push_back({c, c, (uint32_t)a[k + 1].num(), b});
                }
            f.identity = false;
        } else if (o == "endbfchar" && toUni) {
            for (size_t k = 0; k + 1 < n; k += 2, entries++) {
                int b;
                if (a[k].t == pp::STR) f.toUnicode[code(a[k], b)] = unicode(a[k + 1]);
            }
        } else if (o == "endbfrange" && toUni) {
            for (size_t k = 0; k + 2 < n; k += 3) {
                if (a[k].t != pp::STR || a[k + 1].t != pp::STR) continue;
                int b0, b1;
                uint32_t lo = code(a[k], b0), hi = code(a[k + 1], b1);
                for (uint32_t c = lo; c <= hi && c - lo < 65536 && entries < (1u << 20); c++, entries++) {
                    if (a[k + 2].t == pp::ARRAY) f.toUnicode[c] = unicode(a[k + 2].at(c - lo));
                    else f.toUnicode[c] = unicode(a[k + 2]) + (c - lo);
                }
            }
        }
    }
}

// the stand-in for a font not embedded: its family by its name (the subset tag and the style cut
// off), else by its descriptor's flags; its weight and slant by its name or descriptor
plutovg_font_face_t *standIn(pp::Doc &doc, Val desc, std::string name, Font &f) {
    size_t plus = name.find('+');
    if (plus == 6) name = name.substr(7);
    std::string lower;
    for (char c : name) lower.push_back((char)tolower((unsigned char)c));
    auto has = [&](const char *w) { return lower.find(w) != std::string::npos; };
    f.weight = has("black") || has("heavy") ? 900 : has("bold") ? 700 : has("semibold") || has("demi") ? 600 : has("light") ? 300 : 400;
    Val fw = doc.get(desc, "FontWeight");
    if (fw.isNum() && fw.num() >= 100) f.weight = (int)fw.num();
    f.italic = has("italic") || has("oblique") || doc.get(desc, "ItalicAngle").num(0) != 0;
    int flags = (int)doc.get(desc, "Flags").num(0);
    std::string family = name.substr(0, name.find_first_of(",-"));
    std::string spaced;   // ("TimesNewRoman" -> "Times New Roman")
    for (size_t k = 0; k < family.size(); k++) {
        if (k && isupper((unsigned char)family[k]) && islower((unsigned char)family[k - 1])) spaced.push_back(' ');
        spaced.push_back(family[k]);
    }
    const char *serif[] = {"Times New Roman", "Times", "Liberation Serif", "DejaVu Serif", nullptr};
    const char *sans[] = {"Arial", "Helvetica", "Liberation Sans", "DejaVu Sans", nullptr};
    const char *mono[] = {"Courier New", "Courier", "Liberation Mono", "DejaVu Sans Mono", nullptr};
    const char *own[] = {family.c_str(), spaced.c_str(), nullptr};
    std::lock_guard<std::mutex> g(nf_lock());
    plutovg_font_face_t *face = nf_first_face(own, f.weight, f.italic);
    if (!face) {
        bool isMono = has("courier") || has("mono") || (flags & 1);
        bool isSerif = !isMono && (has("times") || has("serif") || has("roman") || has("garamond") || has("georgia") || (flags & 2));
        if (has("arial") || has("helvetica") || has("sans")) isSerif = false;
        face = nf_first_face(isMono ? mono : isSerif ? serif : sans, f.weight, f.italic);
    }
    return face;
}

std::shared_ptr<Font> loadFontUncached(nitropdf &P, Val ref, int depth);

std::shared_ptr<Font> loadFont(nitropdf &P, Val ref, int depth) {
    if (ref.t == pp::REF) {
        auto it = P.fonts.find(ref.oid());
        if (it != P.fonts.end()) return it->second;
        auto f = loadFontUncached(P, ref, depth);
        P.fonts[ref.oid()] = f;
        return f;
    }
    return loadFontUncached(P, ref, depth);
}

// the embedded program of a font descriptor, opened by FreeType
void openProgram(nitropdf &P, Val desc, Font &f) {
    pp::Doc &doc = P.doc;
    for (const char *k : {"FontFile", "FontFile2", "FontFile3"}) {
        Val r = doc.resolve(desc).get(k);
        if (r.t != pp::REF) continue;
        const pp::Stream *st = doc.stream(r.oid());
        if (!st) continue;
        pp::Decoded dec;
        doc.decode(*st, dec, 64u << 20);
        if (dec.data.empty()) continue;
        f.program.swap(dec.data);
        if (!P.ft && FT_Init_FreeType(&P.ft)) P.ft = nullptr;
        if (P.ft && !FT_New_Memory_Face(P.ft, (const FT_Byte *)f.program.data(), (FT_Long)f.program.size(), 0, &f.face)) {
            FT_Set_Char_Size(f.face, 0, 1000 * 64, 72, 72);   // (1000 pixels to the em: glyph space's units)
            return;
        }
        f.face = nullptr;
        f.program.clear();
    }
}

// a simple font's encoding: its base table and its /Differences
void simpleEncoding(nitropdf &P, Val enc, Font &f, const char *const *base) {
    pp::Doc &doc = P.doc;
    Val e = doc.resolve(enc);
    Val baseName = e.t == pp::NAME ? e : doc.get(e, "BaseEncoding");
    if (baseName.is("WinAnsiEncoding")) base = kWinAnsiEncoding;
    else if (baseName.is("MacRomanEncoding")) base = kMacRomanEncoding;
    else if (baseName.is("StandardEncoding") || baseName.is("MacExpertEncoding")) base = kStandardEncoding;
    if (base)
        for (int k = 0; k < 256; k++) f.names[k] = base[k];
    Val diff = doc.get(e, "Differences");
    int code = 0;
    f.nameStore.reserve(diff.size());
    for (size_t k = 0; k < diff.size(); k++) {
        Val v = doc.resolve(diff.at(k));
        if (v.isNum()) code = (int)v.num();
        else if (v.t == pp::NAME && code >= 0 && code < 256) {
            f.nameStore.emplace_back(v.str());
            f.names[code++] = nullptr;   // (filled below: nameStore may still move)
        }
    }
    code = 0;
    size_t s = 0;
    for (size_t k = 0; k < diff.size(); k++) {
        Val v = doc.resolve(diff.at(k));
        if (v.isNum()) code = (int)v.num();
        else if (v.t == pp::NAME && code >= 0 && code < 256) f.names[code++] = f.nameStore[s++].c_str();
    }
}

std::shared_ptr<Font> loadFontUncached(nitropdf &P, Val ref, int depth) {
    pp::Doc &doc = P.doc;
    Val d = doc.resolve(ref);
    if (d.t != pp::DICT || depth > MAX_DEPTH) return nullptr;
    auto f = std::make_shared<Font>();
    Val sub = doc.get(d, "Subtype");
    Val tu = d.get("ToUnicode");
    if (tu.t == pp::REF)
        if (const pp::Stream *st = doc.stream(tu.oid())) {
            pp::Decoded dec;
            doc.decode(*st, dec, 16u << 20);
            parseCMap(dec.data, *f, true);
        }
    if (sub.is("Type0")) {
        f->kind = Font::CID;
        Val desc = doc.resolve(doc.get(d, "DescendantFonts").at(0));
        Val enc = d.get("Encoding");
        Val er = doc.resolve(enc);
        if (er.t == pp::NAME) {
            f->vertical = er.str().size() > 2 && er.str().substr(er.str().size() - 2) == "-V";
            // (Identity-H / -V; the predefined CJK CMaps are not built in: their codes taken as two-byte CIDs)
        } else if (enc.t == pp::REF) {
            if (const pp::Stream *st = doc.stream(enc.oid())) {
                pp::Decoded dec;
                doc.decode(*st, dec, 16u << 20);
                parseCMap(dec.data, *f, false);
                f->vertical = doc.get(st->dict, "WMode").num(0) == 1;
            }
        }
        if (f->spaces.empty()) f->spaces.push_back({0, 0xFFFF, 2});
        Val fd = doc.get(desc, "FontDescriptor");
        openProgram(P, fd, *f);
        Val dw = doc.get(desc, "DW");
        if (dw.isNum()) f->dw = (float)dw.num();
        Val w = doc.get(desc, "W");
        for (size_t k = 0; k + 1 < w.size() && f->cidWidths.size() < (1u << 20);) {
            uint32_t c0 = (uint32_t)doc.resolve(w.at(k)).num();
            Val nx = doc.resolve(w.at(k + 1));
            if (nx.t == pp::ARRAY) {
                for (size_t j = 0; j < nx.size(); j++) f->cidWidths[c0 + (uint32_t)j] = (float)doc.resolve(nx.at(j)).num();
                k += 2;
            } else if (k + 2 < w.size()) {
                uint32_t c1 = (uint32_t)nx.num();
                float wv = (float)doc.resolve(w.at(k + 2)).num();
                for (uint32_t c = c0; c <= c1 && c - c0 < 65536; c++) f->cidWidths[c] = wv;
                k += 3;
            } else break;
        }
        Val c2g = desc.get("CIDToGIDMap");
        if (c2g.t == pp::REF)
            if (const pp::Stream *st = doc.stream(c2g.oid())) {
                pp::Decoded dec;
                doc.decode(*st, dec, 16u << 20);
                f->cidIdentity = false;
                for (size_t k = 0; k + 1 < dec.data.size(); k += 2) f->cidToGid.push_back((uint16_t)((uint8_t)dec.data[k] << 8 | (uint8_t)dec.data[k + 1]));
            }
        if (!f->face) f->sub = standIn(doc, fd, std::string(doc.get(desc, "BaseFont").str()), *f);
        // not embedded, its text by the TrueType font's glyph numbers alone (no /ToUnicode): the
        // system font's own glyphs by those numbers (right where it is the same font, as readers do)
        std::string file;
        int ttc = 0;
        bool known = false;
        if (!f->face && f->sub && f->toUnicode.empty() && doc.get(desc, "Subtype").is("CIDFontType2")) {
            std::lock_guard<std::mutex> g(nf_lock());
            known = nf_face_file(f->sub, file, ttc);
        }
        if (known && (P.ft || !FT_Init_FreeType(&P.ft)) && !FT_New_Face(P.ft, file.c_str(), ttc, &f->face))
            FT_Set_Char_Size(f->face, 0, 1000 * 64, 72, 72);
        else if (known)
            f->face = nullptr;
        return f;
    }
    // simple fonts: widths
    int first = (int)doc.get(d, "FirstChar").num(0);
    Val ws = doc.get(d, "Widths");
    for (size_t k = 0; k < ws.size(); k++) {
        int c = first + (int)k;
        if (c >= 0 && c < 256) f->widths[c] = (float)doc.resolve(ws.at(k)).num(), f->haveWidth[c] = true;
    }
    if (sub.is("Type3")) {
        f->kind = Font::TYPE3;
        auto fm = nums(doc, d.get("FontMatrix"));
        if (fm.size() == 6) memcpy(f->m, fm.data(), sizeof f->m);
        f->charProcs = doc.get(d, "CharProcs");
        f->resources = doc.get(d, "Resources");
        simpleEncoding(P, d.get("Encoding"), *f, nullptr);
        for (int k = 0; k < 256; k++)   // (its widths in glyph space, as for other fonts: 1000 to the unit)
            f->widths[k] *= f->m[0] * 1000;
        return f;
    }
    Val desc = doc.get(d, "FontDescriptor");
    openProgram(P, desc, *f);
    int flags = (int)doc.get(desc, "Flags").num(0);
    bool symbolic = (flags & 4) != 0;
    std::string baseFont(doc.get(d, "BaseFont").str());
    bool symbolFont = baseFont.find("Symbol") != std::string::npos || baseFont.find("Dingbats") != std::string::npos;
    bool tt = f->face && FT_IS_SFNT(f->face);
    // the base table: a TrueType's or an outside font's StandardEncoding (unless symbolic), a Type 1's own
    simpleEncoding(P, d.get("Encoding"), *f, (tt || !f->face) && !symbolFont && (!symbolic || !f->face) ? kStandardEncoding : nullptr);
    if (f->face && !tt) {   // Type 1 / CFF: by name, else by its own encoding
        FT_CharMap own = nullptr;
        for (int k = 0; k < f->face->num_charmaps; k++)
            if (f->face->charmaps[k]->encoding == FT_ENCODING_ADOBE_CUSTOM || f->face->charmaps[k]->encoding == FT_ENCODING_ADOBE_STANDARD ||
                f->face->charmaps[k]->encoding == FT_ENCODING_ADOBE_EXPERT)
                own = f->face->charmaps[k];
        for (int c = 0; c < 256; c++) {
            FT_UInt g = f->names[c] && FT_HAS_GLYPH_NAMES(f->face) ? FT_Get_Name_Index(f->face, (FT_String *)f->names[c]) : 0;
            if (!g && own && !FT_Set_Charmap(f->face, own)) g = FT_Get_Char_Index(f->face, c);
            if (!g && f->names[c] && !FT_Select_Charmap(f->face, FT_ENCODING_UNICODE)) g = FT_Get_Char_Index(f->face, glyphUnicode(f->names[c]));
            f->gid[c] = g ? (int)g : -1;
            f->uni[c] = glyphUnicode(f->names[c]);
        }
    } else if (f->face) {   // TrueType: (3,0) or (1,0) by the code for a symbolic one, (3,1) by the name's character
        FT_CharMap m30 = nullptr, m31 = nullptr, m10 = nullptr;
        for (int k = 0; k < f->face->num_charmaps; k++) {
            FT_CharMap cm = f->face->charmaps[k];
            if (cm->platform_id == 3 && cm->encoding_id == 0) m30 = cm;
            if (cm->platform_id == 3 && cm->encoding_id == 1) m31 = cm;
            if (cm->platform_id == 1 && cm->encoding_id == 0) m10 = cm;
        }
        for (int c = 0; c < 256; c++) {
            FT_UInt g = 0;
            uint32_t u = glyphUnicode(f->names[c]);
            if (m31 && u && (!symbolic || f->names[c]) && !FT_Set_Charmap(f->face, m31)) g = FT_Get_Char_Index(f->face, u);
            if (!g && m30 && !FT_Set_Charmap(f->face, m30)) {
                g = FT_Get_Char_Index(f->face, 0xF000 + c);
                if (!g) g = FT_Get_Char_Index(f->face, c);
            }
            if (!g && m10 && !FT_Set_Charmap(f->face, m10)) g = FT_Get_Char_Index(f->face, c);
            if (!g && f->names[c] && FT_HAS_GLYPH_NAMES(f->face)) g = FT_Get_Name_Index(f->face, (FT_String *)f->names[c]);
            if (!g && !m31 && !m30 && !m10 && f->face->num_charmaps > 0 && !FT_Set_Charmap(f->face, f->face->charmaps[0]))
                g = FT_Get_Char_Index(f->face, c);
            f->gid[c] = g ? (int)g : -1;
            f->uni[c] = u;
        }
    } else {   // not embedded: a system font by the characters (Symbol's own codes in the private area)
        f->sub = standIn(doc, desc, baseFont, *f);
        for (int c = 0; c < 256; c++) f->uni[c] = symbolFont && !f->names[c] ? 0xF000 + c : glyphUnicode(f->names[c]);
    }
    for (int c = 0; c < 256; c++) {   // (/ToUnicode where the encoding said nothing)
        auto it = f->toUnicode.find(c);
        if (!f->uni[c] && it != f->toUnicode.end()) f->uni[c] = it->second;
    }
    return f;
}

// a glyph's outline in glyph space (1000 units to the em, y up) and its advance; a stand-in's
// stretched to width (its advance made the PDF's, 0: as it is). Its path null: nothing to draw.
const Font::Glyph &glyphOf(Font &f, uint32_t key, int gid, uint32_t unicode, float width) {
    auto it = f.glyphs.find(key);
    if (it != f.glyphs.end()) return it->second;
    std::shared_ptr<PathRef> out;
    float advance = 0;
    if (f.face && gid > 0 && !FT_Load_Glyph(f.face, (FT_UInt)gid, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) &&
        f.face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        Decompose d{plutovg_path_create()};
        FT_Outline_Funcs fn = {ftMove, ftLine, ftConic, ftCubic, 0, 0};
        FT_Outline_Decompose(&f.face->glyph->outline, &fn, &d);
        if (d.open) plutovg_path_close(d.p);
        out = std::make_shared<PathRef>(d.p);
        advance = f.face->glyph->advance.x / 64.f;
    } else if (!f.face && unicode) {   // (a space too: its advance, where the PDF gives no width)
        std::lock_guard<std::mutex> g(nf_lock());
        plutovg_font_face_t *face = f.sub && nf_has_glyph(f.sub, unicode) ? f.sub : nf_glyph_face(nullptr, unicode, f.weight, f.italic);
        if (face) {
            plutovg_path_t *p = plutovg_path_create();
            float adv = plutovg_font_face_get_glyph_path(face, 1000, 0, 0, unicode, p);
            float sx = width > 0 && adv > 0 ? clampf(width / adv, 0.5f, 2.f) : 1;
            plutovg_matrix_t flip;   // (plutovg's y is down)
            plutovg_matrix_init(&flip, sx, 0, 0, -1, 0, 0);
            plutovg_path_transform(p, &flip);
            out = std::make_shared<PathRef>(p);
            advance = adv * sx;
        }
    }
    if (out && plutovg_path_get_elements(out->p, nullptr) == 0) out.reset();
    if (f.glyphs.size() >= 65536) f.glyphs.clear();   // (a font of more glyphs than that: some read again)
    Font::Glyph &g = f.glyphs[key];
    g.path = out, g.advance = advance;
    return g;
}

// --- the content interpreter --------------------------------------------------------

struct GState {
    plutovg_matrix_t ctm;
    int clip = -1;
    std::shared_ptr<CS> fillCS, strokeCS;
    float fillC[32], strokeC[32];
    uint32_t fillRgb = 0, strokeRgb = 0;
    float fillA = 1, strokeA = 1;
    bool fillPattern = false, strokePattern = false, fillNone = false, strokeNone = false;
    float width = 1, miter = 10, dashOffset = 0;
    uint8_t cap = 0, join = 0;
    std::shared_ptr<std::vector<float>> dash;
    // the text state
    std::shared_ptr<Font> font;
    float fontSize = 0, charSpace = 0, wordSpace = 0, hScale = 100, leading = 0, rise = 0;
    int render = 0;
};

struct Interp {
    nitropdf &P;
    pp::Doc &doc;
    PageDL &pg;
    plutovg_matrix_t base;   // the page's user space -> its display coordinates
    float pageW = 0, pageH = 0;   // (its size there)
    std::unordered_map<std::string, int> clipIds;   // (the clips made: their index by parent, rule, matrix, path)
    GState gs;
    std::vector<GState> stack;
    plutovg_path_t *path = nullptr;
    int pendingClip = 0;   // 1 nonzero, 2 even-odd
    float cx = 0, cy = 0;  // (the current point, for v and y)
    size_t ops = 0;
    std::vector<uint32_t> forms;   // (the form XObjects being drawn: no cycles)
    std::vector<bool> hiddenStack;
    int hidden = 0;
    plutovg_matrix_t tm, tlm;           // the text matrix and the text line matrix
    plutovg_path_t *textClip = nullptr;  // (render modes 4-7: the glyphs clip at ET)

    Interp(nitropdf &p, PageDL &dl) : P(p), doc(p.doc), pg(dl) {}
    ~Interp() {
        if (path) plutovg_path_destroy(path);
        if (textClip) plutovg_path_destroy(textClip);
    }

    void resetColors() {
        gs.fillCS = gs.strokeCS = deviceCS(CS::GRAY);
        gs.fillC[0] = gs.strokeC[0] = 0;
        gs.fillRgb = gs.strokeRgb = 0;
    }

    static void mul(plutovg_matrix_t &out, const plutovg_matrix_t &a, const plutovg_matrix_t &b) { plutovg_matrix_multiply(&out, &a, &b); }

    float matrixScale(const plutovg_matrix_t &m) { return sqrtf(fabsf(m.a * m.d - m.b * m.c)); }

    // a path's box in the page's coordinates under m, margin (in its own units) around
    nd::Box box(const plutovg_path_t *p, const plutovg_matrix_t &m, float margin) {
        plutovg_rect_t r;
        plutovg_path_extents(p, &r, false);
        r.x -= margin, r.y -= margin, r.w += 2 * margin, r.h += 2 * margin;
        return boxOf(r.x, r.y, r.x + r.w, r.y + r.h, m);
    }
    static nd::Box boxOf(float x0, float y0, float x1, float y1, const plutovg_matrix_t &m) {
        nd::Box b{1e30f, 1e30f, -1e30f, -1e30f};
        for (int k = 0; k < 4; k++) {
            float x, y;
            plutovg_matrix_map(&m, k & 1 ? x1 : x0, k & 2 ? y1 : y0, &x, &y);
            b.x0 = std::min(b.x0, x), b.y0 = std::min(b.y0, y), b.x1 = std::max(b.x1, x), b.y1 = std::max(b.y1, y);
        }
        return b;
    }
    // an item's box within its clip's; false if nothing is left of it (a box of no width or height
    // is something: a hairline exactly across or up, as a CAD plot's letters are drawn)
    // An item's extents b under the clip: false if nothing of it is left. An item wholly inside a
    // rectangle clip is not clipped by it (clip: its parent then): a CAD export's objects clipped to
    // the drawing's frame need no clip, and the canvas's clip is not set again and again for them.
    bool clipBox(nd::Box &b, int &clip, float *gap = nullptr) {
        while (clip >= 0 && pg.clips[clip].rect) {
            const nd::Box &c = pg.clips[clip].box;
            if (b.x0 < c.x0 || b.y0 < c.y0 || b.x1 > c.x1 || b.y1 > c.y1) break;
            if (gap) *gap = std::min({*gap, b.x0 - c.x0, b.y0 - c.y0, c.x1 - b.x1, c.y1 - b.y1});
            clip = pg.clips[clip].parent;
        }
        if (clip < 0) return true;
        const nd::Box &c = pg.clips[clip].box;
        b.x0 = std::max(b.x0, c.x0), b.y0 = std::max(b.y0, c.y0), b.x1 = std::min(b.x1, c.x1), b.y1 = std::min(b.y1, c.y1);
        return b.x1 >= b.x0 && b.y1 >= b.y0;
    }

    void newPath() {
        if (path) plutovg_path_destroy(path);
        path = plutovg_path_create();
    }

    // The current path is an upright rectangle under m, b its extents.
    bool uprightRect(const plutovg_matrix_t &m, const nd::Box &b) {
        bool upright = fabsf(m.b) < 1e-6f && fabsf(m.c) < 1e-6f, turned = fabsf(m.a) < 1e-6f && fabsf(m.d) < 1e-6f;
        if (!upright && !turned) return false;
        const plutovg_path_element_t *e;
        int n = plutovg_path_get_elements(path, &e), moves = 0, lines = 0;
        float px = 0, py = 0, eps = 0.01f;
        for (int i = 0; i < n; i += e[i].header.length) {
            plutovg_path_command_t cmd = e[i].header.command;
            if (cmd == PLUTOVG_PATH_COMMAND_CUBIC_TO || (cmd == PLUTOVG_PATH_COMMAND_MOVE_TO && ++moves > 1)) return false;
            if (cmd == PLUTOVG_PATH_COMMAND_CLOSE) continue;
            const plutovg_point_t &q = e[i + 1].point;
            float x = m.a * q.x + m.c * q.y + m.e, y = m.b * q.x + m.d * q.y + m.f;
            bool cx = fabsf(x - b.x0) < eps || fabsf(x - b.x1) < eps, cy = fabsf(y - b.y0) < eps || fabsf(y - b.y1) < eps;
            if (!cx || !cy) return false;   // (every point a corner, every edge along one axis)
            if (cmd == PLUTOVG_PATH_COMMAND_LINE_TO && ++lines && fabsf(x - px) >= eps && fabsf(y - py) >= eps) return false;
            px = x, py = y;
        }
        return lines >= 3 && lines <= 4;
    }

    // The clip of the current path: none if it cuts nothing, the same one if it is there already (a
    // CAD export clips every object to the same rectangle: one clip, not one per object, so that the
    // items drawn under it share it and the canvas's clip is set once, not before each of them)
    void addClip(bool evenOdd) {
        plutovg_matrix_t m;
        mul(m, gs.ctm, base);
        nd::Box b = box(path, m, 0);
        bool rect = uprightRect(m, b);
        if (rect) {   // (around all of the page, or of the clip it is in: it cuts nothing away)
            nd::Box area = gs.clip >= 0 ? pg.clips[gs.clip].box : nd::Box{0, 0, pageW, pageH};
            const float eps = 0.01f;
            if (b.x0 <= area.x0 + eps && b.y0 <= area.y0 + eps && b.x1 >= area.x1 - eps && b.y1 >= area.y1 - eps) return;
        }
        const plutovg_path_element_t *e;
        int n = plutovg_path_get_elements(path, &e);
        std::string key((const char *)&gs.clip, sizeof gs.clip);
        key += (char)evenOdd;
        key.append((const char *)&m, sizeof m);
        key.append((const char *)e, (size_t)n * sizeof *e);
        auto known = clipIds.find(key);
        if (known != clipIds.end()) {
            gs.clip = known->second;
            return;
        }
        Clip c;
        c.parent = gs.clip;
        c.path = std::make_shared<PathRef>(plutovg_path_reference(path));
        c.m = m;
        c.evenOdd = evenOdd;
        c.rect = rect;
        c.box = b;
        if (gs.clip >= 0) {
            const nd::Box &p = pg.clips[gs.clip].box;
            c.box.x0 = std::max(c.box.x0, p.x0), c.box.y0 = std::max(c.box.y0, p.y0);
            c.box.x1 = std::min(c.box.x1, p.x1), c.box.y1 = std::min(c.box.y1, p.y1);
        }
        gs.clip = (int)pg.clips.size();
        pg.clips.push_back(std::move(c));
        clipIds.emplace(std::move(key), gs.clip);
    }

    // painting the current path: fill (0 none, 1 nonzero, 2 even-odd) and / or stroke
    void paint(int fill, bool stroke) {
        if (!path) return newPath();
        if (plutovg_path_get_elements(path, nullptr) > 0 && !hidden) {
            plutovg_matrix_t m;
            mul(m, gs.ctm, base);
            std::shared_ptr<PathRef> shared;
            if (fill && !gs.fillNone && (!gs.fillPattern || true)) {
                Item it;
                it.kind = Item::FILL;
                it.evenOdd = fill == 2;
                it.clip = gs.clip;
                it.rgb = gs.fillRgb, it.alpha = gs.fillA;
                it.m = m;
                it.box = box(path, m, 0);
                if (clipBox(it.box, it.clip)) {
                    shared = std::make_shared<PathRef>(plutovg_path_reference(path));
                    it.path = shared;
                    pg.items.push_back(std::move(it));
                }
            }
            if (stroke && !gs.strokeNone) strokeItem(m, shared);
        }
        if (pendingClip) addClip(pendingClip == 2);
        pendingClip = 0;
        newPath();
    }

    // a glyph run's fill (in user space), merged into the last one of the same state
    void textFill(plutovg_path_t *run, const plutovg_matrix_t &m) {
        nd::Box b = box(run, m, 0);
        int clip = gs.clip;
        if (!clipBox(b, clip)) return;
        int elements = plutovg_path_get_elements(run, nullptr);
        if (!pg.items.empty() && gs.fillA >= 1) {
            Item &l = pg.items.back();
            if (l.kind == Item::FILL && l.text && l.clip == clip && l.rgb == gs.fillRgb && l.alpha >= 1 && !memcmp(&l.m, &m, sizeof m) &&
                l.segments + elements <= 4 * (int)MAX_MERGED && l.path.use_count() == 1) {
                plutovg_path_add_path(l.path->p, run, nullptr);
                l.segments += elements;
                l.box.x0 = std::min(l.box.x0, b.x0), l.box.y0 = std::min(l.box.y0, b.y0);
                l.box.x1 = std::max(l.box.x1, b.x1), l.box.y1 = std::max(l.box.y1, b.y1);
                return;
            }
        }
        Item it;
        it.kind = Item::FILL;
        it.text = true;
        it.clip = clip;
        it.rgb = gs.fillRgb, it.alpha = gs.fillA;
        it.m = m;
        it.box = b;
        plutovg_path_t *own = plutovg_path_create();
        plutovg_path_add_path(own, run, nullptr);
        it.path = std::make_shared<PathRef>(own);
        it.segments = elements;
        pg.items.push_back(std::move(it));
    }

    // a string shown by Tj / TJ / ' / ": its glyphs drawn by the render mode, the text matrix advanced
    void showText(Val str, Val res, int depth) {
        Font *f = gs.font.get();
        if (!f || str.t != pp::STR) return;
        const uint8_t *s = (const uint8_t *)str.s;
        size_t n = str.n;
        float fs = gs.fontSize, th = gs.hScale / 100;
        int mode = gs.render;
        bool fill = mode == 0 || mode == 2 || mode == 4 || mode == 6, stroke = mode == 1 || mode == 2 || mode == 5 || mode == 6;
        bool clip = mode >= 4;
        plutovg_path_t *run = plutovg_path_create();
        for (size_t i = 0; i < n;) {
            uint32_t code = s[i], key, uni = 0;
            int bytes = 1, gid = -1;
            float w;
            bool haveW;
            if (f->kind == Font::CID) {
                bytes = 0;
                for (int len = 1; len <= 4 && !bytes && i + len <= n; len++) {
                    uint32_t c = 0;
                    for (int k = 0; k < len; k++) c = c << 8 | s[i + k];
                    for (const Font::Space &sp : f->spaces)
                        if (sp.bytes == len && c >= sp.lo && c <= sp.hi) {
                            bytes = len, code = c;
                            break;
                        }
                }
                if (!bytes) {   // (not in the code space: as many bytes as its first range has)
                    bytes = std::min<int>(f->spaces[0].bytes, (int)(n - i));
                    code = 0;
                    for (int k = 0; k < bytes; k++) code = code << 8 | s[i + k];
                }
                uint32_t cid = f->identity ? code : 0;
                for (const Font::Range &r : f->ranges)
                    if (r.bytes == bytes && code >= r.lo && code <= r.hi) {
                        cid = r.cid + (code - r.lo);
                        break;
                    }
                gid = f->face ? (int)(f->cidIdentity ? cid : cid < f->cidToGid.size() ? f->cidToGid[cid] : 0) : -1;
                auto u = f->toUnicode.find(code);
                uni = u != f->toUnicode.end() ? u->second : 0;
                auto cw = f->cidWidths.find(cid);
                haveW = true;
                w = cw != f->cidWidths.end() ? cw->second : f->dw;
                key = cid;
            } else {
                gid = f->gid[code], uni = f->uni[code];
                haveW = f->haveWidth[code];
                w = f->widths[code];
                key = code;
            }
            i += bytes;
            // the glyph: drawn at the text rendering matrix's origin
            plutovg_matrix_t g, gm;
            plutovg_matrix_init(&g, fs * th / 1000, 0, 0, fs / 1000, 0, gs.rise);
            if (f->vertical) {   // (its origin at the middle of its top: the default metrics, W2 not read)
                plutovg_matrix_t v;
                plutovg_matrix_init(&v, 1, 0, 0, 1, -w / 2, -880);
                mul(gm, v, g);
                g = gm;
            }
            mul(gm, g, tm);
            if (f->kind == Font::TYPE3) {
                if (!hidden) type3Glyph(*f, code, gm, res, depth);
            } else {
                const Font::Glyph &gl = glyphOf(*f, key, gid, uni, haveW ? w : 0);
                if (!haveW) w = gl.advance;
                if (gl.path) plutovg_path_add_path(run, gl.path->p, &gm);
            }
            // the advance
            plutovg_matrix_t t, out;
            if (!f->vertical) {
                float tx = (w / 1000 * fs + gs.charSpace + (bytes == 1 && code == 32 ? gs.wordSpace : 0)) * th;
                plutovg_matrix_init_translate(&t, tx, 0);
            } else {
                float ty = -fs + gs.charSpace + (bytes == 1 && code == 32 ? gs.wordSpace : 0);
                plutovg_matrix_init_translate(&t, 0, ty);
            }
            mul(out, t, tm);
            tm = out;
        }
        if (plutovg_path_get_elements(run, nullptr) > 0) {
            plutovg_matrix_t m;
            mul(m, gs.ctm, base);
            if (!hidden && fill) textFill(run, m);
            if (!hidden && stroke && !gs.strokeNone) {
                plutovg_path_t *saved = path;
                path = run;
                strokeItem(m, nullptr);
                path = saved;
            }
            if (clip) {
                if (!textClip) textClip = plutovg_path_create();
                plutovg_path_add_path(textClip, run, nullptr);
            }
        } else if (clip && !textClip)
            textClip = plutovg_path_create();
        plutovg_path_destroy(run);
    }

    // a Type 3 glyph: its procedure drawn in its font matrix at the glyph's place
    void type3Glyph(Font &f, uint32_t code, const plutovg_matrix_t &glyph, Val res, int depth) {
        if (code > 255 || !f.names[code] || depth >= MAX_DEPTH) return;
        Val proc = doc.resolve(f.charProcs).get(f.names[code]);
        const pp::Stream *st = proc.t == pp::REF ? doc.stream(proc.oid()) : nullptr;
        if (!st) return;
        pp::Decoded dec;
        doc.decode(*st, dec, 16u << 20);
        stack.push_back(gs);
        plutovg_matrix_t fm, g1000, a, b2;   // (glyph is in 1000ths: the font matrix's units are its own)
        plutovg_matrix_init(&fm, f.m[0], f.m[1], f.m[2], f.m[3], f.m[4], f.m[5]);
        plutovg_matrix_init_scale(&g1000, 1000, 1000);
        mul(a, fm, g1000);
        mul(b2, a, glyph);
        mul(gs.ctm, b2, stack.back().ctm);
        gs.font = nullptr;
        Val fres = doc.resolve(f.resources).t == pp::DICT ? f.resources : res;
        run(dec.data.data(), dec.data.size(), fres, depth + 1);
        gs = stack.back();
        stack.pop_back();
    }

    void textClipEnd() {   // ET: the glyphs shown with a clipping mode clip
        if (!textClip) return;
        plutovg_path_t *saved = path;
        path = textClip;
        addClip(false);
        path = saved;
        plutovg_path_destroy(textClip);
        textClip = nullptr;
    }

    void strokeItem(const plutovg_matrix_t &m, std::shared_ptr<PathRef> shared) {
        nd::Box b = box(path, m, gs.width / 2 * std::max(1.f, gs.join == 0 ? std::min(gs.miter, 4.f) : 1.f));
        int clip = gs.clip;
        float gap = 1e30f;
        if (!clipBox(b, clip, &gap)) return;
        int elements = plutovg_path_get_elements(path, nullptr);
        // the same stroke as the last item, opaque: one path (a CAD plot's "m l S" lines)
        if (!pg.items.empty() && gs.strokeA >= 1 && !shared) {
            Item &l = pg.items.back();
            if (l.kind == Item::STROKE && l.clip == clip && l.clipOwn == gs.clip && l.rgb == gs.strokeRgb && l.alpha >= 1 && l.width == gs.width && l.cap == gs.cap &&
                l.join == gs.join && l.miter == gs.miter && l.dash == gs.dash && l.dashOffset == gs.dashOffset && !memcmp(&l.m, &m, sizeof m) &&
                l.segments + elements <= (int)MAX_MERGED && l.path.use_count() == 1) {
                plutovg_path_add_path(l.path->p, path, nullptr);
                l.segments += elements;
                l.clipGap = std::min(l.clipGap, gap);
                l.box.x0 = std::min(l.box.x0, b.x0), l.box.y0 = std::min(l.box.y0, b.y0);
                l.box.x1 = std::max(l.box.x1, b.x1), l.box.y1 = std::max(l.box.y1, b.y1);
                return;
            }
        }
        Item it;
        it.kind = Item::STROKE;
        it.clip = clip;
        it.clipOwn = gs.clip, it.clipGap = gap;
        it.rgb = gs.strokeRgb, it.alpha = gs.strokeA;
        it.width = gs.width, it.cap = gs.cap, it.join = gs.join, it.miter = gs.miter;
        it.dash = gs.dash, it.dashOffset = gs.dashOffset;
        it.m = m;
        it.box = b;
        if (shared) it.path = shared;
        else {   // (its own copy: later strokes may be merged into it)
            plutovg_path_t *own = plutovg_path_create();
            plutovg_path_add_path(own, path, nullptr);
            it.path = std::make_shared<PathRef>(own);
        }
        it.segments = elements;
        pg.items.push_back(std::move(it));
    }

    void setColor(bool fill, const Val *args, size_t n, Val res) {
        std::shared_ptr<CS> &cs = fill ? gs.fillCS : gs.strokeCS;
        float *c = fill ? gs.fillC : gs.strokeC;
        bool &pat = fill ? gs.fillPattern : gs.strokePattern;
        uint32_t &rgb = fill ? gs.fillRgb : gs.strokeRgb;
        bool &none = fill ? gs.fillNone : gs.strokeNone;
        if (cs->k == CS::PATTERN) {
            // a pattern: a shading pattern fills with its middle colour, a tiling one with grey (not yet drawn)
            pat = true;
            none = false;
            rgb = 0x808080;
            if (n && args[n - 1].t == pp::NAME) {
                Val p = doc.resolve(resource(P, res, "Pattern", args[n - 1]));
                if ((int)doc.get(p, "PatternType").num(0) == 2) {
                    Val sh = doc.get(p, "Shading");
                    auto scs = loadCS(P, doc.resolve(sh).get("ColorSpace"), res, 0);
                    auto fn = loadFunc(P, doc.resolve(sh).get("Function"), 0);
                    auto dom = nums(doc, doc.resolve(sh).get("Domain"));
                    if (scs && fn) {
                        float t = dom.size() >= 2 ? (dom[0] + dom[1]) / 2 : 0.5f, out[32] = {0}, col[3];
                        fn->eval(&t, out);
                        scs->toRGB(out, col);
                        rgb = packRgb(col);
                    }
                } else if (cs->base && n > 1) {   // (an uncoloured tiling pattern: its colour)
                    float cc[32];
                    for (size_t k = 0; k + 1 < n && k < 32; k++) cc[k] = (float)args[k].num();
                    float col[3];
                    cs->base->toRGB(cc, col);
                    rgb = packRgb(col);
                }
            }
            return;
        }
        pat = false;
        none = cs->k == CS::NONE;
        for (size_t k = 0; k < n && k < 32; k++) c[k] = (float)args[k].num();
        float col[3];
        cs->toRGB(c, col);
        rgb = packRgb(col);
    }

    void setSpace(bool fill, Val name, Val res) {
        auto cs = loadCS(P, name, res, 0);
        if (!cs) cs = deviceCS(CS::GRAY);
        (fill ? gs.fillCS : gs.strokeCS) = cs;
        float *c = fill ? gs.fillC : gs.strokeC;
        cs->initial(c);
        float col[3];
        cs->toRGB(c, col);
        (fill ? gs.fillRgb : gs.strokeRgb) = packRgb(col);
        (fill ? gs.fillPattern : gs.strokePattern) = cs->k == CS::PATTERN;
        (fill ? gs.fillNone : gs.strokeNone) = cs->k == CS::NONE;
    }
    void deviceColor(bool fill, CS::K k, const Val *args, size_t n) {
        (fill ? gs.fillCS : gs.strokeCS) = deviceCS(k);
        (fill ? gs.fillPattern : gs.strokePattern) = false;
        (fill ? gs.fillNone : gs.strokeNone) = false;
        setColor(fill, args, n, Val());
    }

    void extGState(Val res, Val name) {
        Val g = doc.resolve(resource(P, res, "ExtGState", name));
        if (g.t != pp::DICT) return;
        Val v;
        if ((v = doc.get(g, "LW")).isNum()) gs.width = (float)v.num();
        if ((v = doc.get(g, "LC")).isNum()) gs.cap = (uint8_t)std::max(0, std::min(2, (int)v.num()));
        if ((v = doc.get(g, "LJ")).isNum()) gs.join = (uint8_t)std::max(0, std::min(2, (int)v.num()));
        if ((v = doc.get(g, "ML")).isNum()) gs.miter = (float)v.num();
        if ((v = doc.get(g, "CA")).isNum()) gs.strokeA = clampf((float)v.num(), 0, 1);
        if ((v = doc.get(g, "ca")).isNum()) gs.fillA = clampf((float)v.num(), 0, 1);
        if ((v = doc.get(g, "D")).t == pp::ARRAY && v.size() >= 2) setDash(doc.resolve(v.at(0)), doc.resolve(v.at(1)));
    }
    void setDash(Val arr, Val phase) {
        auto d = nums(doc, arr);
        bool any = false;
        for (float x : d) any |= x > 0;
        for (float &x : d) x = std::max(x, 0.f);
        gs.dash = any ? std::make_shared<std::vector<float>>(d) : nullptr;
        gs.dashOffset = (float)phase.num();
    }

    // optional content: whether a group / membership dictionary is shown
    bool visible(Val oc, int depth = 0) {
        Val v = doc.resolve(oc);
        if (v.t != pp::DICT || depth > 8) return true;
        if (doc.get(v, "Type").is("OCMD")) {
            Val ocgs = doc.resolve(v.get("OCGs"));
            std::vector<Val> list;
            if (ocgs.t == pp::ARRAY)
                for (size_t k = 0; k < ocgs.size(); k++) list.push_back(ocgs.at(k));
            else if (v.get("OCGs").t != pp::NUL) list.push_back(v.get("OCGs"));
            if (list.empty()) return true;
            Val pol = doc.get(v, "P");
            int on = 0;
            for (Val &g : list) on += visible(g, depth + 1);
            if (pol.is("AllOn")) return on == (int)list.size();
            if (pol.is("AnyOff")) return on < (int)list.size();
            if (pol.is("AllOff")) return on == 0;
            return on > 0;   // (AnyOn, the default)
        }
        uint32_t id = oc.t == pp::REF ? oc.oid() : 0;
        if (!id) return true;
        if (P.ocOff.count(id)) return false;
        if (P.ocBaseOff) return P.ocOn.count(id) > 0;
        return true;
    }

    void image(ImageSource &src, Val res) {
        if (hidden) return;
        Val oc = src.dict.get("OC");
        if (oc.t != pp::NUL && !visible(oc)) return;
        Val im = key(doc, src.dict, "ImageMask", "IM");
        bool stencil = im.t == pp::BOOL && im.i;
        std::shared_ptr<Img> img = decodeImage(P, src, res, gs.fillRgb, 1);
        if (!img || !img->s) return;
        addImage(img, stencil ? gs.fillA : gs.fillA);
    }
    void addImage(std::shared_ptr<Img> img, float alpha) {
        int iw = plutovg_surface_get_width(img->s), ih = plutovg_surface_get_height(img->s);
        // the unit square (y up) is the image: pixel (0, 0) at its top left
        plutovg_matrix_t unit, m, m2;
        plutovg_matrix_init(&unit, 1.f / iw, 0, 0, -1.f / ih, 0, 1);
        mul(m, gs.ctm, base);
        mul(m2, unit, m);
        Item it;
        it.kind = Item::IMAGE;
        it.clip = gs.clip;
        it.alpha = alpha;
        it.m = m2;
        it.img = img;
        it.box = boxOf(0, 0, 1, 1, m);
        if (!clipBox(it.box, it.clip)) return;
        pg.items.push_back(std::move(it));
    }

    void doXObject(Val res, Val name, int depth) {
        Val ref = resource(P, res, "XObject", name);
        if (ref.t != pp::REF) return;
        const pp::Stream *st = doc.stream(ref.oid());
        if (!st) return;
        Val d = st->dict;
        Val sub = doc.get(d, "Subtype");
        if (sub.is("Image")) {
            if (hidden) return;
            Val oc = d.get("OC");
            if (oc.t != pp::NUL && !visible(oc)) return;
            Val im = doc.get(d, "ImageMask");
            bool stencil = im.t == pp::BOOL && im.i;
            std::shared_ptr<Img> img;
            if (!stencil) {   // (the same picture on several pages: decoded once while any of them holds it)
                auto it = P.images.find(ref.oid());
                if (it != P.images.end()) img = it->second.lock();
            }
            if (!img) {
                ImageSource src;
                if (!imageSource(P, ref, src)) return;
                img = decodeImage(P, src, res, gs.fillRgb, 1);
                if (!img || !img->s) return;
                if (!stencil) P.images[ref.oid()] = img;
            }
            addImage(img, gs.fillA);
            return;
        }
        if (sub.is("Form")) doForm(ref.oid(), *st, res, depth);
    }
    // a form XObject drawn in the current state: its /Matrix, clipped by its /BBox
    void doForm(uint32_t oid, const pp::Stream &st, Val res, int depth) {
        if (depth >= MAX_DEPTH || std::find(forms.begin(), forms.end(), oid) != forms.end()) return;
        Val d = st.dict;
        Val oc = d.get("OC");
        if (oc.t != pp::NUL && !visible(oc)) return;
        pp::Decoded dec;
        doc.decode(st, dec, 256u << 20);
        Val fres = doc.get(d, "Resources");
        if (fres.t != pp::DICT) fres = doc.resolve(res);
        stack.push_back(gs);
        auto mtx = nums(doc, d.get("Matrix"));
        if (mtx.size() == 6) {
            plutovg_matrix_t fm, out;
            plutovg_matrix_init(&fm, mtx[0], mtx[1], mtx[2], mtx[3], mtx[4], mtx[5]);
            mul(out, fm, gs.ctm);
            gs.ctm = out;
        }
        auto bb = nums(doc, d.get("BBox"));
        if (bb.size() == 4) {   // (the form's box clips it)
            plutovg_path_t *saved = path;
            path = plutovg_path_create();
            plutovg_path_add_rect(path, std::min(bb[0], bb[2]), std::min(bb[1], bb[3]), fabsf(bb[2] - bb[0]), fabsf(bb[3] - bb[1]));
            addClip(false);
            plutovg_path_destroy(path);
            path = saved;
        }
        forms.push_back(oid);
        run(dec.data.data(), dec.data.size(), fres, depth + 1);
        forms.pop_back();
        gs = stack.back();
        stack.pop_back();
    }

    void run(const char *data, size_t len, Val res, int depth) {
        if (!path) newPath();
        size_t qBase = stack.size();
        pp::ContentLexer lx(data, len);
        pp::Op op;
        while (lx.next(op)) {
            if (++ops > MAX_OPS) break;
            const Val *a = op.args;
            size_t n = op.nargs;
            std::string_view o = op.name;
            auto num = [&](size_t k) { return k < n ? (float)a[k].num() : 0.f; };
            if (o.size() > 3) continue;
            switch (o[0]) {
            case 'q':
                if (o == "q" && stack.size() < 256 + qBase) stack.push_back(gs);
                break;
            case 'Q':
                if (o == "Q" && stack.size() > qBase) gs = stack.back(), stack.pop_back();
                break;
            case 'c':
                if (o == "cm" && n >= 6) {
                    plutovg_matrix_t m, out;
                    plutovg_matrix_init(&m, num(0), num(1), num(2), num(3), num(4), num(5));
                    mul(out, m, gs.ctm);
                    gs.ctm = out;
                } else if (o == "c" && n >= 6) {
                    plutovg_path_cubic_to(path, num(0), num(1), num(2), num(3), num(4), num(5));
                    cx = num(4), cy = num(5);
                } else if (o == "cs" && n >= 1) setSpace(true, a[0], res);
                break;
            case 'C':
                if (o == "CS" && n >= 1) setSpace(false, a[0], res);
                break;
            case 'm':
                if (o == "m" && n >= 2) plutovg_path_move_to(path, num(0), num(1)), cx = num(0), cy = num(1);
                break;
            case 'l':
                if (o == "l" && n >= 2) plutovg_path_line_to(path, num(0), num(1)), cx = num(0), cy = num(1);
                break;
            case 'v':
                if (o == "v" && n >= 4) plutovg_path_cubic_to(path, cx, cy, num(0), num(1), num(2), num(3)), cx = num(2), cy = num(3);
                break;
            case 'y':
                if (o == "y" && n >= 4) plutovg_path_cubic_to(path, num(0), num(1), num(2), num(3), num(2), num(3)), cx = num(2), cy = num(3);
                break;
            case 'h':
                if (o == "h") plutovg_path_close(path);
                break;
            case 'r':
                if (o == "re" && n >= 4) {
                    float x = num(0), y = num(1), w = num(2), h = num(3);
                    plutovg_path_move_to(path, x, y);
                    plutovg_path_line_to(path, x + w, y);
                    plutovg_path_line_to(path, x + w, y + h);
                    plutovg_path_line_to(path, x, y + h);
                    plutovg_path_close(path);
                    cx = x, cy = y;
                } else if (o == "rg" && n >= 3) deviceColor(true, CS::RGB, a, n);
                else if (o == "ri") {}
                break;
            case 'R':
                if (o == "RG" && n >= 3) deviceColor(false, CS::RGB, a, n);
                break;
            case 'g':
                if (o == "g" && n >= 1) deviceColor(true, CS::GRAY, a, n);
                else if (o == "gs" && n >= 1) extGState(res, a[0]);
                break;
            case 'G':
                if (o == "G" && n >= 1) deviceColor(false, CS::GRAY, a, n);
                break;
            case 'k':
                if (o == "k" && n >= 4) deviceColor(true, CS::CMYK, a, n);
                break;
            case 'K':
                if (o == "K" && n >= 4) deviceColor(false, CS::CMYK, a, n);
                break;
            case 's':
                if (o == "sc" || o == "scn") setColor(true, a, n, res);
                else if (o == "s") plutovg_path_close(path), paint(0, true);
                else if (o == "sh") {}   // (shadings: not yet)
                break;
            case 'S':
                if (o == "SC" || o == "SCN") setColor(false, a, n, res);
                else if (o == "S") paint(0, true);
                break;
            case 'f':
                if (o == "f") paint(1, false);
                else if (o == "f*") paint(2, false);
                break;
            case 'F':
                if (o == "F") paint(1, false);
                break;
            case 'B':
                if (o == "BT") {
                    plutovg_matrix_init_identity(&tm);
                    tlm = tm;
                } else if (o == "B") paint(1, true);
                else if (o == "B*") paint(2, true);
                else if (o == "BI") {
                    ImageSource src;
                    if (!inlineImage(op, res, src)) break;
                    image(src, res);
                } else if (o == "BMC" || o == "BDC") {
                    bool hide = false;
                    if (o == "BDC" && n >= 2 && a[0].is("OC")) {
                        Val prop = a[1].t == pp::NAME ? resource(P, res, "Properties", a[1]) : a[1];
                        hide = !visible(prop);
                    }
                    hiddenStack.push_back(hide);
                    hidden += hide;
                }
                break;
            case 'b':
                if (o == "b") plutovg_path_close(path), paint(1, true);
                else if (o == "b*") plutovg_path_close(path), paint(2, true);
                break;
            case 'E':
                if (o == "ET") textClipEnd();
                else if (o == "EMC" && !hiddenStack.empty()) {
                    hidden -= hiddenStack.back();
                    hiddenStack.pop_back();
                }
                break;
            case 'n':
                if (o == "n") paint(0, false);
                break;
            case 'W':
                if (o == "W") pendingClip = 1;
                else if (o == "W*") pendingClip = 2;
                break;
            case 'w':
                if (o == "w" && n >= 1) gs.width = num(0);
                break;
            case 'J':
                if (o == "J" && n >= 1) gs.cap = (uint8_t)std::max(0, std::min(2, (int)num(0)));
                break;
            case 'j':
                if (o == "j" && n >= 1) gs.join = (uint8_t)std::max(0, std::min(2, (int)num(0)));
                break;
            case 'M':
                if (o == "M" && n >= 1) gs.miter = num(0);
                break;
            case 'd':
                if (o == "d" && n >= 2) setDash(a[0], a[1]);
                break;
            case 'D':
                if (o == "Do" && n >= 1 && a[0].t == pp::NAME) doXObject(res, a[0], depth);
                break;
            case 'T':
                if (o == "Tf" && n >= 2) {
                    gs.font = a[0].t == pp::NAME ? loadFont(P, resource(P, res, "Font", a[0]), 0) : nullptr;
                    gs.fontSize = num(1);
                } else if (o == "Tc" && n >= 1) gs.charSpace = num(0);
                else if (o == "Tw" && n >= 1) gs.wordSpace = num(0);
                else if (o == "Tz" && n >= 1) gs.hScale = num(0);
                else if (o == "TL" && n >= 1) gs.leading = num(0);
                else if (o == "Ts" && n >= 1) gs.rise = num(0);
                else if (o == "Tr" && n >= 1) gs.render = (int)num(0) & 7;
                else if ((o == "Td" || o == "TD") && n >= 2) {
                    if (o == "TD") gs.leading = -num(1);
                    plutovg_matrix_t t, out;
                    plutovg_matrix_init_translate(&t, num(0), num(1));
                    mul(out, t, tlm);
                    tm = tlm = out;
                } else if (o == "Tm" && n >= 6) {
                    plutovg_matrix_init(&tlm, num(0), num(1), num(2), num(3), num(4), num(5));
                    tm = tlm;
                } else if (o == "T*") nextLine();
                else if (o == "Tj" && n >= 1) showText(a[0], res, depth);
                else if (o == "TJ" && n >= 1 && a[0].t == pp::ARRAY) {
                    for (size_t k = 0; k < a[0].size(); k++) {
                        Val e = a[0].at(k);
                        if (e.t == pp::STR) showText(e, res, depth);
                        else if (e.isNum() && gs.font) {   // (thousandths of the font size, against the direction)
                            float d = -(float)e.num() / 1000 * gs.fontSize;
                            plutovg_matrix_t t, out;
                            if (gs.font->vertical) plutovg_matrix_init_translate(&t, 0, d);
                            else plutovg_matrix_init_translate(&t, d * gs.hScale / 100, 0);
                            mul(out, t, tm);
                            tm = out;
                        }
                    }
                }
                break;
            case '\'':
                if (n >= 1) nextLine(), showText(a[0], res, depth);
                break;
            case '"':
                if (n >= 3) gs.wordSpace = num(0), gs.charSpace = num(1), nextLine(), showText(a[2], res, depth);
                break;
            }
        }
        while (stack.size() > qBase) gs = stack.back(), stack.pop_back();   // (unbalanced q)
    }

    void nextLine() {
        plutovg_matrix_t t, out;
        plutovg_matrix_init_translate(&t, 0, -gs.leading);
        mul(out, t, tlm);
        tm = tlm = out;
    }

    // an inline image's dictionary (abbreviated keys and names), its data decoded but for an image filter
    bool inlineImage(const pp::Op &op, Val res, ImageSource &src) {
        src.dict = op.image;
        Val f = doc.resolve(op.image.get("F"));
        if (f.t == pp::NUL) f = doc.resolve(op.image.get("Filter"));
        Val dp = doc.resolve(op.image.get("DP"));
        if (dp.t == pp::NUL) dp = doc.resolve(op.image.get("DecodeParms"));
        // a dictionary pdfparse3's decode reads: /Filter [/Crypt ...] (its data is the content's: already decrypted)
        static const char kFilter[] = "Filter", kParms[] = "DecodeParms", kCrypt[] = "Crypt";
        std::vector<Val> filters;
        Val crypt;
        crypt.t = pp::NAME, crypt.s = kCrypt, crypt.n = 5;
        filters.push_back(crypt);
        if (f.t == pp::ARRAY)
            for (size_t k = 0; k < f.size(); k++) filters.push_back(doc.resolve(f.at(k)));
        else if (f.t == pp::NAME) filters.push_back(f);
        std::vector<Val> parms;
        parms.push_back(Val());
        if (dp.t == pp::ARRAY)
            for (size_t k = 0; k < dp.size(); k++) parms.push_back(doc.resolve(dp.at(k)));
        else parms.push_back(dp);
        Val fa, pa, pairs[4];
        fa.t = pp::ARRAY, fa.v = filters.data(), fa.n = (uint32_t)filters.size();
        pa.t = pp::ARRAY, pa.v = parms.data(), pa.n = (uint32_t)parms.size();
        pairs[0].t = pp::NAME, pairs[0].s = kFilter, pairs[0].n = 6, pairs[1] = fa;
        pairs[2].t = pp::NAME, pairs[2].s = kParms, pairs[2].n = 11, pairs[3] = pa;
        Val dict;
        dict.t = pp::DICT, dict.v = pairs, dict.n = 2;
        pp::Stream st;
        st.raw = (const uint8_t *)op.imageData.data(), st.len = op.imageData.size(), st.dict = dict, st.exact = true;
        pp::Decoded dec;
        doc.decode(st, dec, 64u << 20);
        src.data.swap(dec.data);
        src.filter = dec.image == "CCF" ? "CCITTFaxDecode" : dec.image == "DCT" ? "DCTDecode" : dec.image;
        src.parms = dec.imageParms;
        (void)res;
        return true;
    }
};

// --- pages -------------------------------------------------------------------------

// the page's display size and its user space -> display matrix (y down, turned, /UserUnit)
void pageGeometry(const pp::Page &p, double *w, double *h, plutovg_matrix_t *m) {
    double cw = p.crop[2] - p.crop[0], ch = p.crop[3] - p.crop[1], u = p.userUnit > 0 ? p.userUnit : 1;
    bool turned = p.rotate == 90 || p.rotate == 270;
    if (w) *w = (turned ? ch : cw) * u;
    if (h) *h = (turned ? cw : ch) * u;
    if (!m) return;
    // (x, y) -> (x - x0, y1 - y): unturned, y down; then turned clockwise, then scaled
    plutovg_matrix_t flip, rot, out, sc;
    plutovg_matrix_init(&flip, 1, 0, 0, -1, (float)-p.crop[0], (float)p.crop[3]);
    switch (p.rotate) {
    case 90: plutovg_matrix_init(&rot, 0, 1, -1, 0, (float)ch, 0); break;
    case 180: plutovg_matrix_init(&rot, -1, 0, 0, -1, (float)cw, (float)ch); break;
    case 270: plutovg_matrix_init(&rot, 0, -1, 1, 0, 0, (float)cw); break;
    default: plutovg_matrix_init_identity(&rot);
    }
    plutovg_matrix_multiply(&out, &flip, &rot);
    plutovg_matrix_init_scale(&sc, (float)u, (float)u);
    plutovg_matrix_multiply(m, &out, &sc);
}

// The annotations' normal appearances (12.5.5): each form's /BBox, through its /Matrix, fitted to
// the annotation's /Rect. Not the hidden ones, nor those without an appearance (popups).
void drawAnnotations(Interp &in, const pp::Page &page, const GState &initial) {
    pp::Doc &doc = in.doc;
    Val annots = doc.get(page.dict, "Annots");
    for (size_t k = 0; k < annots.size() && k < 100000; k++) {
        Val a = doc.resolve(annots.at(k));
        if (a.t != pp::DICT || ((int)doc.get(a, "F").num(0) & (2 | 32))) continue;   // (Hidden, NoView)
        Val oc = a.get("OC");
        if (oc.t != pp::NUL && !in.visible(oc)) continue;
        Val n = doc.get(a, "AP").get("N"), ap = n;   // (unresolved: a stream's reference)
        if (doc.resolve(n).t == pp::DICT && n.t != pp::REF) {   // (appearance states: the one /AS names)
            Val as = doc.get(a, "AS");
            ap = as.t == pp::NAME ? doc.resolve(n).get(std::string(as.str()).c_str()) : Val();
        }
        const pp::Stream *st = ap.t == pp::REF ? doc.stream(ap.oid()) : nullptr;
        auto rect = nums(doc, a.get("Rect")), bb = nums(doc, st ? st->dict.get("BBox") : Val());
        if (!st || rect.size() != 4 || bb.size() != 4) continue;
        auto mv = nums(doc, st->dict.get("Matrix"));
        plutovg_matrix_t fm;
        if (mv.size() == 6) plutovg_matrix_init(&fm, mv[0], mv[1], mv[2], mv[3], mv[4], mv[5]);
        else plutovg_matrix_init_identity(&fm);
        nd::Box b = Interp::boxOf(bb[0], bb[1], bb[2], bb[3], fm);   // (the box the form's matrix makes of its BBox)
        float bw = b.x1 - b.x0, bh = b.y1 - b.y0;
        if (!(bw > 1e-6f) || !(bh > 1e-6f)) continue;
        float rx0 = std::min(rect[0], rect[2]), ry0 = std::min(rect[1], rect[3]);
        float sx = fabsf(rect[2] - rect[0]) / bw, sy = fabsf(rect[3] - rect[1]) / bh;
        in.gs = initial;
        in.stack.clear();
        in.hidden = 0, in.hiddenStack.clear();
        plutovg_matrix_init(&in.gs.ctm, sx, 0, 0, sy, rx0 - b.x0 * sx, ry0 - b.y0 * sy);
        in.doForm(ap.oid(), *st, Val(), 0);
    }
}

void buildPage(nitropdf &P, int k) {
    PageDL &pg = *P.dl[k];
    const pp::Page &page = P.pages[k];
    Interp in(P, pg);
    double pw, ph;
    pageGeometry(page, &pw, &ph, &in.base);
    in.pageW = (float)pw, in.pageH = (float)ph;
    plutovg_matrix_init_identity(&in.gs.ctm);
    plutovg_matrix_init_identity(&in.tm);
    in.tlm = in.tm;
    in.resetColors();
    std::string content;
    P.doc.contents(page, content);
    GState initial = in.gs;
    in.run(content.data(), content.size(), page.resources, 0);
    drawAnnotations(in, page, initial);
}

void readOptionalContent(nitropdf &P) {
    pp::Doc &doc = P.doc;
    Val d = doc.get(doc.get(doc.root(), "OCProperties"), "D");
    if (d.t != pp::DICT) return;
    P.ocBaseOff = doc.get(d, "BaseState").is("OFF");
    Val off = doc.get(d, "OFF"), on = doc.get(d, "ON");
    for (size_t k = 0; k < off.size(); k++)
        if (off.at(k).t == pp::REF) P.ocOff.insert(off.at(k).oid());
    for (size_t k = 0; k < on.size(); k++)
        if (on.at(k).t == pp::REF) P.ocOn.insert(on.at(k).oid());
}

void drawItems(nitropdf &P, const PageDL &pg, plutovg_canvas_t *c, const plutovg_matrix_t &view, const double vb[4], double scale,
               const nd::Cancel &cancel, int &rc) {
    (void)P;
    plutovg_canvas_save(c);
    int curClip = -1;
    size_t k = 0;
    for (const Item &it : pg.items) {
        if ((++k & 255) == 0 && cancel()) {
            rc = 1;
            break;
        }
        if (!nd::inView(vb, it.box.x0, it.box.y0, it.box.x1, it.box.y1, scale, 2 / scale)) continue;
        // (a stroke within a pixel of a rectangle clip left out: drawn at least a pixel wide, it may reach out)
        int clip = it.kind == Item::STROKE && it.clipOwn != it.clip && it.clipGap * scale < 1 ? it.clipOwn : it.clip;
        if (clip != curClip) {   // (the clip chain, from its outermost)
            plutovg_canvas_restore(c);
            plutovg_canvas_save(c);
            std::vector<int> chain;
            for (int x = clip; x >= 0 && chain.size() < 1024; x = pg.clips[x].parent) chain.push_back(x);
            for (size_t i = chain.size(); i-- > 0;) {
                const Clip &cl = pg.clips[chain[i]];
                plutovg_matrix_t m;
                plutovg_matrix_multiply(&m, &cl.m, &view);
                plutovg_canvas_set_matrix(c, &m);
                plutovg_canvas_set_fill_rule(c, cl.evenOdd ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
                plutovg_canvas_clip_path(c, cl.path->p);
            }
            curClip = clip;
        }
        plutovg_matrix_t m;
        plutovg_matrix_multiply(&m, &it.m, &view);
        plutovg_canvas_set_matrix(c, &m);
        if (it.kind == Item::FILL) {
            nd::setRgb(c, it.rgb, it.alpha);
            plutovg_canvas_set_fill_rule(c, it.evenOdd ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
            plutovg_canvas_fill_path(c, it.path->p);
        } else if (it.kind == Item::STROKE) {
            nd::setRgb(c, it.rgb, it.alpha);
            float k2 = sqrtf(fabsf(m.a * m.d - m.b * m.c));   // (device pixels per unit)
            plutovg_canvas_set_line_width(c, k2 > 0 ? std::max(it.width, 1 / k2) : it.width);
            plutovg_canvas_set_line_cap(c, (plutovg_line_cap_t)it.cap);
            plutovg_canvas_set_line_join(c, (plutovg_line_join_t)it.join);
            plutovg_canvas_set_miter_limit(c, it.miter);
            if (it.dash) plutovg_canvas_set_dash(c, it.dashOffset, it.dash->data(), (int)it.dash->size());
            else plutovg_canvas_set_dash(c, 0, nullptr, 0);
            plutovg_canvas_stroke_path(c, it.path->p);
        } else {   // (in the picture's pixels; much smaller: one of its halved copies)
            int iw = plutovg_surface_get_width(it.img->s), ih = plutovg_surface_get_height(it.img->s), f = 1;
            if (fabsf(m.b) < 1e-6f && fabsf(m.c) < 1e-6f) {
                // upright: its edges on whole pixels, so that the strips a picture is cut into (a printer
                // driver's rows of one pixel) meet without the background showing between their edges
                float x0 = roundf(m.e), y0 = roundf(m.f), x1 = roundf(m.e + m.a * iw), y1 = roundf(m.f + m.d * ih);
                plutovg_matrix_init(&m, (x1 - x0) / iw, 0, 0, (y1 - y0) / ih, x0, y0);
                plutovg_canvas_set_matrix(c, &m);
            }
            plutovg_surface_t *s = it.img->mips->forScale(sqrtf(fabsf(m.a * m.d - m.b * m.c)), &f);
            plutovg_matrix_t tm;
            plutovg_matrix_init_scale(&tm, (float)f, (float)f);
            plutovg_canvas_set_texture(c, s, PLUTOVG_TEXTURE_TYPE_PLAIN, it.alpha, &tm);
            plutovg_canvas_fill_rect(c, 0, 0, (float)iw, (float)ih);
            plutovg_surface_destroy(s);
        }
    }
    plutovg_canvas_restore(c);
}

}  // namespace

// --- the interface -----------------------------------------------------------------

nitropdf *nitropdf_load(const void *data, size_t len) {
    if (!data || len < 8) return nullptr;
    auto *p = new nitropdf;
    p->data.assign((const char *)data, len);
    if (!p->doc.open(p->data.data(), p->data.size())) {
        delete p;
        return nullptr;
    }
    size_t n = p->doc.pageCount();
    for (size_t k = 0; k < n; k++) {
        pp::Page pg;
        if (!p->doc.page(k, pg)) continue;
        double w, h;
        pageGeometry(pg, &w, &h, nullptr);
        if (!(w > 0 && h > 0 && w < 1e6 && h < 1e6)) continue;
        p->pages.push_back(pg);
        p->dl.push_back(std::make_unique<PageDL>());
    }
    if (p->pages.empty()) {
        delete p;
        return nullptr;
    }
    readOptionalContent(*p);
    return p;
}

void nitropdf_free(nitropdf *pdf) { delete pdf; }

int nitropdf_pages(const nitropdf *pdf) { return pdf ? (int)pdf->pages.size() : 0; }

void nitropdf_set_background(nitropdf *pdf, int background) {
    if (pdf) pdf->background = background;
}

void nitropdf_size(const nitropdf *pdf, int page, double *w, double *h) {
    *w = *h = 0;
    if (!pdf || page < 0 || page >= (int)pdf->pages.size()) return;
    pageGeometry(pdf->pages[page], w, h, nullptr);
}

int nitropdf_render(nitropdf *pdf, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                    int (*cancelled)(void *ctx), void *ctx) {
    if (!pdf || page < 0 || page >= (int)pdf->pages.size() || !nd::renderArgs(w, h, pitch, scale)) return -1;
    PageDL &pg = *pdf->dl[page];
    if (!pg.built.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> g(pdf->lock);
        if (!pg.built.load(std::memory_order_relaxed)) {
            buildPage(*pdf, page);
            pg.built.store(true, std::memory_order_release);
        }
    }
    nd::Target t(bgra, w, h, pitch, pdf->background == 3 ? nd::background(3) : nd::background(0));
    if (!t.canvas) return -1;
    plutovg_matrix_t view;
    plutovg_matrix_init(&view, (float)scale, 0, 0, (float)scale, (float)(-x0 * scale), (float)(-y0 * scale));
    double vb[4] = {x0, y0, x0 + w / scale, y0 + h / scale};
    int rc = 0;
    drawItems(*pdf, pg, t.canvas, view, vb, scale, nd::Cancel{cancelled, ctx}, rc);
    return rc;
}
