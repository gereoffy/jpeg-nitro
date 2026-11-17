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
#include "nitroai.h"   // (Illustrator 1-8 documents, read as a PDF)
#include "nitro_draw.h"
#include "nitro_font.h"   // (stand-ins for the fonts not embedded)
#include "nitro_num.h"   // (nn_strtod: numbers read the same in every locale)
#include "nitropdf_glyphs.h"
#include "nitro_convert.h"   // (nc_jpeg_to_bgra)
#include "nitro_cmyk.h"
#include "nitrojpeg.h"
#include "nitrojbig2.h"
#include "nitrojpx.h"
extern "C" {
#include "nitrotiff.h"   // (nt_ccitt)
}

#include "nitro_ft.h"   // (FreeType; a glyph's outline as a path)
#include <pdfparse3.h>
#include <plutovg.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
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
const size_t MAX_OPS = 20u << 20;          // a page's operators (forms included) and glyphs shown
const size_t MAX_BYTES = 1u << 30;         // a page's content decoded to be read: its own (MAX_PAGE_BYTES), its forms'
const size_t MAX_PAGE_BYTES = 256u << 20;  // and glyph procedures' each time they are drawn
const size_t MAX_LIST_BYTES = (size_t)768 << 20;   // the pages' display lists kept up to this ...
const size_t MIN_LISTS = 4;                          // ... but always the last few drawn
const uint64_t MAX_IMAGE_PIXELS = 32u << 20;   // an image beyond this is decoded smaller (a scanned A0 plan: 17 MP)
const size_t MAX_MERGED = 4096;            // path elements merged into one stroke
const float MAX_DASHES = 1 << 20;           // a stroke's dashes: with more, drawn solid
const int MAX_COMPS = 32;                  // a colour's components, a function's outputs (the arrays they are read into)

// (a NaN is a: 0 times an infinite /Encode or /Decode, inf - inf, are never an index or a colour)
inline float clampf(float v, float a, float b) { return !(v > a) ? a : v > b ? b : v; }
// a number as an int: beyond an int the nearest one, a NaN 0 (the cast alone is undefined there)
inline int toInt(double v) { return v >= 2147483647.0 ? INT_MAX : v <= -2147483648.0 ? INT_MIN : v == v ? (int)v : 0; }
inline uint32_t toU32(double v) { return v >= 4294967295.0 ? UINT32_MAX : v > 0 ? (uint32_t)v : 0; }   // (a negative one, a NaN: 0)

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
        double v = nn_strtod(tok.c_str(), &end);
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
        case PS_IDIV: b = pop(), a = pop(), push(toInt(b) ? (float)((int64_t)toInt(a) / toInt(b)) : 0); break;   // (64 bits: INT_MIN / -1)
        case PS_LN: a = pop(), push(a > 0 ? logf(a) : 0); break;
        case PS_LOG: a = pop(), push(a > 0 ? log10f(a) : 0); break;
        case PS_MOD: b = pop(), a = pop(), push(toInt(b) ? (float)((int64_t)toInt(a) % toInt(b)) : 0); break;
        case PS_MUL: b = pop(), a = pop(), push(a * b); break;
        case PS_NEG: push(-pop()); break;
        case PS_ROUND: push(floorf(pop() + 0.5f)); break;
        case PS_SIN: push(sinf(pop() * (float)M_PI / 180)); break;
        case PS_SQRT: a = pop(), push(a > 0 ? sqrtf(a) : 0); break;
        case PS_SUB: b = pop(), a = pop(), push(a - b); break;
        case PS_TRUNC: push(truncf(pop())); break;
        case PS_AND: b = pop(), a = pop(), push((float)(toInt(a) & toInt(b))); break;
        case PS_OR: b = pop(), a = pop(), push((float)(toInt(a) | toInt(b))); break;
        case PS_XOR: b = pop(), a = pop(), push((float)(toInt(a) ^ toInt(b))); break;
        case PS_NOT: a = pop(), push(a == 0 ? 1.f : a == 1 ? 0.f : (float)~toInt(a)); break;
        case PS_BITSHIFT: {
            b = pop(), a = pop();
            int s = toInt(b), v = toInt(a);
            push((float)(s >= 0 ? (s < 31 ? (int)((unsigned)v << s) : 0) : (s > -31 ? v >> -s : 0)));
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
            int k = toInt(pop());
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
            int k = toInt(pop());
            if (k >= 0 && k < sp) push(st[sp - 1 - k]);
            break;
        }
        case PS_POP: pop(); break;
        case PS_ROLL: {
            int j = toInt(pop()), k = toInt(pop());
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
        float t = N == 1 ? x : x > 0 ? powf(x, N) : (N == floorf(N) ? powf(x, N) : 0);
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

// A function-based (1) or mesh shading (4-7) as Gouraud-shaded triangles in the shading's space: a
// function's grid, the triangles, the patches cut into a grid of them (8.7.4.5.2, 8.7.4.5.5-8)
struct Mesh {
    std::vector<float> v;        // per vertex x, y and its colour: r g b (0-1), or t in v[2] (0-1, through lut)
    std::vector<uint32_t> tri;   // the triangles' vertices
    std::vector<uint32_t> lut;   // (a Function: t interpolated, then its colour of 256, 0xffrrggbb)
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the vertices' box
    static const int STRIDE = 5;
};

// An axial (2) or radial (3) shading as plutovg's gradient: its function sampled into stops, in the
// shading's own space (8.7.4.5.3, 8.7.4.5.4); the others as a mesh
struct Shade {
    int type = 2;
    float c[6] = {0};   // x0 y0 x1 y1 (axial); x0 y0 r0 x1 y1 r1 (radial)
    bool extend[2] = {false, false};
    std::vector<plutovg_gradient_stop_t> stops;
    std::shared_ptr<Mesh> mesh;
};

// A tiling pattern (8.7.3.1): its cell's content as a display list of its own, in the pattern's space
// (clipped by its /BBox), repeated every xs and ys; an uncoloured one (PaintType 2) drawn in the colour
// given with it
struct PageDL;
struct SoftMask;
struct Group;
struct Tiling {
    std::shared_ptr<PageDL> cell;
    float bbox[4] = {0};   // x0 y0 x1 y1
    float xs = 0, ys = 0;
    bool uncoloured = false;
};

struct Item {
    enum : uint8_t { FILL, STROKE, IMAGE, GROUP } kind;
    uint8_t blend = 0;   // (its blend mode: BLEND_NAMES' index)
    std::shared_ptr<SoftMask> smask;   // (the soft mask it is drawn through)
    std::shared_ptr<Group> group;      // (a GROUP: its items, composed as one at alpha)
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
    std::shared_ptr<Shade> shade;   // (a fill: by this gradient instead of rgb)
    std::shared_ptr<Tiling> tile;   // (or by this pattern: rgb an uncoloured one's colour)
    plutovg_matrix_t shadeM;        // (their space -> the path's)
    int segments = 0;   // (merged strokes: the path's elements so far)
    float length = 0;   // (a dashed stroke: its path's length, in its own units)
    bool text = false;  // (a glyph run's fill: merged with the next)
    nd::Box box;
};

// A transparency group drawn as one (11.4.7): its items into a layer of their own, the layer composed
// with the backdrop by the group item's alpha, blend mode and soft mask
struct Group {
    std::vector<Item> items;
};

// A soft mask (11.6.5.2): its group's items in the page's display coordinates, drawn over the backdrop
// colour (luminosity) or nothing (alpha), each pixel's luminosity or alpha through the transfer function
// the mask. Outside the items' box the backdrop alone: the same value everywhere there.
struct SoftMask {
    std::vector<Item> items;
    bool luminosity = true;
    uint32_t bc = 0;     // (the backdrop, rgb)
    uint8_t tr[256];     // (the transfer function)
    nd::Box box{0, 0, -1, -1};
};

const char *const BLEND_NAMES[] = {"Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "ColorDodge", "ColorBurn",
                                   "HardLight", "SoftLight", "Difference", "Exclusion", "Hue", "Saturation", "Color", "Luminosity"};

struct Font;

struct PageDL {
    std::atomic<bool> built{false};
    std::vector<Item> items;
    std::vector<Clip> clips;
    uint64_t lastUse = 0;   // (when last drawn: the least recent lists are let go of first)
    size_t bytes = 0;       // (built: about what it holds, its pictures most of it)
    bool blends = false;    // (something in it is drawn in a blend mode: the page drawn on nothing, then on its background)
};

}  // namespace

struct nitropdf {
    std::string data;
    pp::Doc doc;
    std::vector<pp::Page> pages;
    // the PDFs embedded in it (a portfolio's documents, attachments), each a document of its own;
    // the pages the interface shows: its own (not a portfolio's cover) and theirs, after them
    std::vector<std::unique_ptr<nitropdf>> parts;
    std::vector<std::pair<nitropdf *, int>> shown;
    // the pages' display lists, built when first drawn; beyond MAX_LIST_BYTES (a book of colour
    // scans grew by tens of MB a page) the least recently drawn ones let go of (a render holds its
    // own reference: one drawing is freed when it ends), built again when drawn again
    std::vector<std::shared_ptr<PageDL>> dl;
    std::mutex dlLock;   // (the slots of dl and their use counts; a list is built under lock)
    uint64_t useClock = 0;
    int background = 0;
    std::mutex lock;   // (pdfparse3's document and the caches: one page read at a time)
    // the caches, by object number
    std::unordered_map<uint32_t, std::shared_ptr<CS>> spaces;
    std::unordered_map<uint32_t, std::shared_ptr<Func>> funcs;
    // large resource dictionaries indexed by name (resource()): by where their pairs are
    std::unordered_map<const pp::Val *, std::unordered_map<std::string_view, pp::Val>> names;
    // the image XObjects decoded, while a page's list holds them: by object number, a stencil mask
    // by its colour too (imageKey). Like the other caches, used only while a page is built, under
    // lock: the threads drawing a page read its finished list alone.
    std::unordered_map<uint64_t, std::weak_ptr<Img>> images;
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
            int i = std::max(0, std::min(hival, toInt(roundf(c[0]))));
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

// A function as it is in the file. Its callers evaluate it into arrays of MAX_COMPS: loadFuncUncached
// lets none through with more outputs (or none).
std::shared_ptr<Func> readFunc(nitropdf &P, Val ref, int depth) {
    pp::Doc &doc = P.doc;
    Val v = doc.resolve(ref);
    if (depth > MAX_DEPTH) return nullptr;
    auto f = std::make_shared<Func>();
    if (v.t == pp::ARRAY) {   // (n one-output functions)
        f->type = 100;
        if (v.size() > (size_t)MAX_COMPS) return nullptr;
        for (size_t k = 0; k < v.size(); k++) {
            auto s = loadFunc(P, v.at(k), depth + 1);
            if (!s || s->nout != 1) return nullptr;   // (each writes one output: the next one's follows it)
            f->nin = std::max(f->nin, s->nin);   // (they read the same inputs)
            f->subs.push_back(s);
        }
        f->nout = (int)f->subs.size();
        return f;
    }
    Val d = v.t == pp::DICT ? v : Val();
    const pp::Stream *st = ref.t == pp::REF ? doc.stream(ref.oid()) : nullptr;
    if (d.t != pp::DICT) return nullptr;
    f->type = toInt(doc.get(d, "FunctionType").num(-1));
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
            if (!s || s->nin != 1) return nullptr;   // (its part is given the one input)
            f->subs.push_back(s);
        }
        f->bounds = nums(doc, d.get("Bounds"));
        f->encode = nums(doc, d.get("Encode"));
        if (f->subs.empty() || f->bounds.size() + 1 != f->subs.size()) return nullptr;
        while (f->encode.size() < 2 * f->subs.size()) f->encode.push_back(f->encode.size() & 1);
        f->nout = f->subs[0]->nout;
        for (auto &s : f->subs)   // (whichever is picked writes nout outputs)
            if (s->nout != f->nout) return nullptr;
        return f;
    }
    case 0: {
        if (!st || f->range.size() < 2) return nullptr;
        for (float s : nums(doc, d.get("Size"))) f->size.push_back(std::max(1, toInt(s)));
        if ((int)f->size.size() != f->nin) return nullptr;
        int bps = toInt(doc.get(d, "BitsPerSample").num(8));
        if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 12 && bps != 16 && bps != 24 && bps != 32) return nullptr;
        f->encode = nums(doc, d.get("Encode"));
        if (f->encode.size() < 2u * f->nin) {
            f->encode.clear();
            for (int s : f->size) f->encode.push_back(0), f->encode.push_back((float)(s - 1));
        }
        f->decode = nums(doc, d.get("Decode"));
        if (f->decode.size() < 2u * f->nout) f->decode = f->range;
        uint64_t count = (uint64_t)f->nout;
        for (int s : f->size)   // (checked at each step: four of 65536 multiply to 2^64, that is 0)
            if ((count *= s) > (16u << 20)) return nullptr;
        if (!count) return nullptr;
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

std::shared_ptr<Func> loadFuncUncached(nitropdf &P, Val ref, int depth) {
    auto f = readFunc(P, ref, depth);
    return f && f->nout >= 1 && f->nout <= MAX_COMPS ? f : nullptr;
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
        int n = toInt(doc.get(d, "N").num(3));
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
        c->hival = std::max(0, std::min(255, toInt(doc.resolve(v.at(2)).num(0))));
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
        // (a tint transform reading more inputs than the space has components would read past the colour)
        if (!c->base || !c->tint || c->tint->nin > c->n) return nullptr;
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
    if (W <= 0 || H <= 0 || n <= 0 || n > MAX_COMPS || step <= 0) return false;
    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) return false;   // (the bit depths there are: 0 divided by 0 below)
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

struct JpxPixels {
    njpx_image im{};
    ~JpxPixels() { njpx_free(&im); }
};

Val key(pp::Doc &doc, Val d, const char *name, const char *abbr);

// A JPX image decoded: one beyond MAX_IMAGE_PIXELS (32 MP: its /Width x /Height) at half, a quarter ... of its
// size, its finest resolution levels left out (faster than decoding all, and filtered, not decimated);
// in full if it cannot be (fewer levels in a tile)
int jpxDecode(nitropdf &P, ImageSource &src, unsigned flags, njpx_image &im) {
    int64_t W = toInt(key(P.doc, src.dict, "Width", nullptr).num(0)), H = toInt(key(P.doc, src.dict, "Height", nullptr).num(0));
    int r = 0;
    while (r < 7 && W > 0 && H > 0 && (uint64_t)((W + (1 << r) - 1) >> r) * (uint64_t)((H + (1 << r) - 1) >> r) > MAX_IMAGE_PIXELS) r++;
    const uint8_t *d = (const uint8_t *)src.data.data();
    if (r && !njpx_decode(d, src.data.size(), flags | NJPX_REDUCE(r), &im)) return 0;
    return njpx_decode(d, src.data.size(), flags, &im);
}

// JPX carries its own dimensions and per-component precision. /Decode is ignored for colour
// images (ISO 32000-1, 7.4.9); a PDF /ColorSpace overrides JP2 colour specifications/palettes.
plutovg_surface_t *jpxSurface(nitropdf &P, ImageSource &src, std::shared_ptr<CS> &cs) {
    JpxPixels pixels;
    njpx_image &im = pixels.im;
    bool override = cs != nullptr;
    if (jpxDecode(P, src, override ? NJPX_RAW_COLORS : 0, im)) return nullptr;
    int smask = toInt(key(P.doc, src.dict, "SMaskInData", nullptr).num(0));
    if (smask < 0 || smask > 2) return nullptr;
    if (src.dict.get("SMask").t != pp::NUL) smask = 0;   // (an /SMask overrides the opacity in the data: 8.9.5.4)
    int alpha = im.alpha;
    int expected = cs ? cs->n : im.colorspace == NJPX_GRAY ? 1 : im.colorspace == NJPX_CMYK ? 4 :
                   im.colorspace == NJPX_SRGB || im.colorspace == NJPX_SYCC ? 3 : 0;
    if (!expected && smask && (im.ncomp == 2 || im.ncomp == 4)) expected = im.ncomp - 1;
    if (alpha < 0 && smask && expected && im.ncomp == expected + 1) alpha = im.ncomp - 1;
    int n = im.ncomp - (alpha >= 0);
    if (!cs) {
        if (expected && expected != n) return nullptr;
        if (im.colorspace && im.colorspace != NJPX_GRAY && im.colorspace != NJPX_SRGB &&
            im.colorspace != NJPX_CMYK && im.colorspace != NJPX_SYCC) return nullptr;
        if (n != 1 && n != 3 && n != 4) return nullptr;
        cs = deviceCS(n == 1 ? CS::GRAY : n == 4 ? CS::CMYK : CS::RGB);
    }
    if (cs->n != n || n > NJPX_MAX_COMPS || (smask && alpha < 0)) return nullptr;
    int step = 1;
    while ((uint64_t)im.width * im.height / ((uint64_t)step * step) > MAX_IMAGE_PIXELS) step++;
    int w = (im.width + step - 1) / step, h = (im.height + step - 1) / step;
    plutovg_surface_t *surface = newSurface(w, h);
    if (!surface) return nullptr;
    // (a colour key: an array; a stream there is a stencil mask, applied by the caller)
    Val mk = P.doc.resolve(src.dict.get("Mask"));
    auto range = mk.t == pp::ARRAY ? nums(P.doc, mk) : std::vector<float>();
    bool colorKey = range.size() >= 2u * n;
    uint8_t *dst = plutovg_surface_get_data(surface);
    int stride = plutovg_surface_get_stride(surface);
    // Opaque device RGB and single-component spaces are common in scanned PDFs. Avoid
    // per-pixel floating point colour conversion: RGB bytes are already the result;
    // gray / Indexed / Separation values repeat, so map their raw samples once.
    if (!smask && alpha < 0 && cs->k == CS::RGB && im.ncomp == 3 && !colorKey &&
        im.precision[0] == 8 && im.precision[1] == 8 && im.precision[2] == 8 &&
        (override || im.colorspace != NJPX_SYCC)) {
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(dst + (size_t)y * stride);
            const uint16_t *s = im.samples + (size_t)y * step * im.width * 3;
            for (int x = 0; x < w; x++, s += 3 * step) row[x] = 0xff000000u | s[0] << 16 | s[1] << 8 | s[2];
        }
        return surface;
    }
    if (!smask && alpha < 0 && cs->k == CS::CMYK && im.ncomp == 4 && !colorKey &&
        im.precision[0] == 8 && im.precision[1] == 8 && im.precision[2] == 8 && im.precision[3] == 8) {
        // The same byte-row conversion as JPEG/ordinary CMYK images; the surface's row
        // holds the CMYK bytes temporarily, then nitro_cmyk converts them in place.
        for (int y = 0; y < h; y++) {
            uint8_t *row = dst + (size_t)y * stride;
            const uint16_t *s = im.samples + (size_t)y * step * im.width * 4;
            for (int x = 0; x < w; x++, s += 4 * step)
                for (int c = 0; c < 4; c++) row[4 * x + c] = (uint8_t)s[c];
            ncm_row_bgra(row, row, (size_t)w);
        }
        return surface;
    }
    if (!smask && alpha < 0 && n == 1 && (uint64_t)w * h >= 2u * (1u << im.precision[0])) {
        unsigned values = 1u << im.precision[0];
        std::vector<uint32_t> lut(values);
        for (unsigned v = 0; v < values; v++) {
            float c = cs->k == CS::INDEXED ? (float)v : v / (float)(values - 1), rgb[3];
            cs->toRGB(&c, rgb);
            lut[v] = colorKey && !(v < range[0] || v > range[1]) ? 0 : 0xff000000u | packRgb(rgb);
        }
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(dst + (size_t)y * stride);
            const uint16_t *s = im.samples + (size_t)y * step * im.width;
            for (int x = 0; x < w; x++) row[x] = lut[s[x * step]];
        }
        return surface;
    }
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(dst + (size_t)y * stride);
        for (int x = 0; x < w; x++) {
            const uint16_t *s = im.samples + ((size_t)y * step * im.width + x * step) * im.ncomp;
            float a = smask ? s[alpha] / (float)((1u << im.precision[alpha]) - 1) : 1;
            float c[NJPX_MAX_COMPS], rgb[3];
            bool transparent = colorKey;
            int k = 0;
            for (int j = 0; j < im.ncomp; j++) {
                if (j == alpha) continue;
                float v = s[j] / (float)((1u << im.precision[j]) - 1);
                if (smask == 2 || (smask && im.premultiplied)) v = a > 0 ? clampf(v / a, 0, 1) : 0;   // (or the JP2's channel definition says so)
                if (cs->k == CS::INDEXED) v = (float)s[j];
                else if (cs->k == CS::LAB) v = k == 0 ? v * 100 : cs->range[2 * (k - 1)] + v * (cs->range[2 * k - 1] - cs->range[2 * (k - 1)]);
                c[k] = v;
                if (colorKey && (s[j] < range[2 * k] || s[j] > range[2 * k + 1])) transparent = false;
                k++;
            }
            if (!override && im.colorspace == NJPX_SYCC) {
                float cb = c[1] - (1u << (im.precision[1] - 1)) / (float)((1u << im.precision[1]) - 1);
                float cr = c[2] - (1u << (im.precision[2] - 1)) / (float)((1u << im.precision[2]) - 1);
                float yy = c[0];
                c[0] = clampf(yy + 1.402f * cr, 0, 1);
                c[1] = clampf(yy - 0.344136f * cb - 0.714136f * cr, 0, 1);
                c[2] = clampf(yy + 1.772f * cb, 0, 1);
            }
            cs->toRGB(c, rgb);
            unsigned aa = transparent ? 0 : (unsigned)lroundf(a * 255);
            row[x] = aa << 24 | (unsigned)lroundf(rgb[0] * aa) << 16 | (unsigned)lroundf(rgb[1] * aa) << 8 | (unsigned)lroundf(rgb[2] * aa);
        }
    }
    return surface;
}

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

bool imageSource(nitropdf &P, Val ref, ImageSource &src);

bool bilevel(const ImageSource &src) {
    return src.filter == "CCITTFaxDecode" || src.filter == "CCF" || src.filter == "JBIG2Decode";
}

// A CCITT or JBIG2 coded image (bilevel(src)) as rows of packed bits, a black pixel 0 (CCITT's
// /BlackIs1: 1); W x H pixels, W: CCITT's /Columns.
bool bilevelRows(nitropdf &P, ImageSource &src, int &W, int H, std::string &bits) {
    pp::Doc &doc = P.doc;
    Val p = src.parms;
    const uint8_t *d = (const uint8_t *)src.data.data();
    if (src.filter == "JBIG2Decode") {
        if (W <= 0 || (uint64_t)W * H > (1ull << 32)) return false;
        ImageSource g;   // (/JBIG2Globals: the segments the pages share)
        Val gr = p.t == pp::DICT ? p.get("JBIG2Globals") : Val();
        if (gr.t != pp::REF || !imageSource(P, gr, g)) g.data.clear();
        size_t prow = ((size_t)W + 7) / 8;
        bits.assign(prow * H, '\0');
        return !nj2_decode(d, src.data.size(), (const uint8_t *)g.data.data(), g.data.size(), W, H, (uint8_t *)&bits[0], prow);
    }
    int k = toInt(key(doc, p, "K", nullptr).num(0)), cols = toInt(key(doc, p, "Columns", nullptr).num(1728));
    bool b1 = key(doc, p, "BlackIs1", nullptr).t == pp::BOOL && key(doc, p, "BlackIs1", nullptr).i;
    bool align = key(doc, p, "EncodedByteAlign", nullptr).t == pp::BOOL && key(doc, p, "EncodedByteAlign", nullptr).i;
    if (cols <= 0 || (uint64_t)cols * H > (1ull << 32)) return false;
    size_t prow = ((size_t)cols + 7) / 8;
    bits.assign(prow * H, '\0');
    if (nt_ccitt(d, src.data.size(), k, cols, H, b1, align, (uint8_t *)&bits[0], prow)) return false;
    W = cols;
    return true;
}

// 1-bit / gray samples of a mask (a stencil /Mask, an /SMask), w x h, its /Decode applied: 255 painted / opaque
bool maskSamples(nitropdf &P, ImageSource &src, bool stencil, Samples &out) {
    pp::Doc &doc = P.doc;
    int W = toInt(key(doc, src.dict, "Width", "W").num(0)), H = toInt(key(doc, src.dict, "Height", "H").num(0));
    int bpc = stencil ? 1 : toInt(key(doc, src.dict, "BitsPerComponent", "BPC").num(8));
    if (W <= 0 || H <= 0 || (uint64_t)W * H > (256u << 20)) return false;
    int step = 1;
    while ((uint64_t)W * H / ((uint64_t)step * step) > MAX_IMAGE_PIXELS) step++;
    std::string bits;
    const uint8_t *d = (const uint8_t *)src.data.data();
    size_t len = src.data.size();
    if (bilevel(src)) {
        if (!bilevelRows(P, src, W, H, bits)) return false;
        d = (const uint8_t *)bits.data(), len = bits.size();
        bpc = 1;
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
    } else if (src.filter == "JPXDecode") {
        JpxPixels pixels;
        njpx_image &im = pixels.im;
        if (jpxDecode(P, src, NJPX_RAW_COLORS, im) || im.ncomp != 1 || (stencil && im.precision[0] != 1)) return false;
        step = 1;
        while ((uint64_t)im.width * im.height / ((uint64_t)step * step) > MAX_IMAGE_PIXELS) step++;
        out.w = (im.width + step - 1) / step, out.h = (im.height + step - 1) / step, out.n = 1;
        out.v.resize((size_t)out.w * out.h);
        unsigned maxv = (1u << im.precision[0]) - 1;
        for (int y = 0; y < out.h; y++)
            for (int x = 0; x < out.w; x++)
                out.v[(size_t)y * out.w + x] = (uint8_t)((im.samples[(size_t)y * step * im.width + x * step] * 255u + maxv / 2) / maxv);
    } else if (!src.filter.empty())
        return false;
    if (out.v.empty() && !unpack(d, len, W, H, 1, bpc, step, out, false)) return false;
    auto dec = nums(doc, key(doc, src.dict, "Decode", "D"));
    bool invert = (stencil || src.filter != "JPXDecode") && dec.size() >= 2 && dec[0] > dec[1];
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
    int W = toInt(key(doc, d, "Width", "W").num(0)), H = toInt(key(doc, d, "Height", "H").num(0));
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
    int bpc = toInt(key(doc, d, "BitsPerComponent", "BPC").num(8));
    Val csv = d.get("ColorSpace");
    if (csv.t == pp::NUL) csv = d.get("CS");
    std::shared_ptr<CS> cs = loadCS(P, csv, res, 0);
    if (!cs && src.filter != "JPXDecode") cs = deviceCS(CS::GRAY);
    int step = 1;
    while ((uint64_t)W * H / ((uint64_t)step * step) > MAX_IMAGE_PIXELS) step++;
    // the colour of each pixel: from the decoded JPEG, the CCITT / JBIG2 bits or the samples
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
    } else if (src.filter == "JPXDecode") {
        img->s = jpxSurface(P, src, cs);
        if (!img->s) {   // (damaged or unsupported JPEG 2000: a light grey area)
            img->s = newSurface(1, 1);
            if (img->s) *(uint32_t *)plutovg_surface_get_data(img->s) = 0xffe0e0e0;
        }
    } else if (!src.filter.empty() && !bilevel(src)) {
        return nullptr;
    } else {
        const uint8_t *dd = (const uint8_t *)src.data.data();
        size_t len = src.data.size();
        std::string bits;
        if (bilevel(src)) {
            if (!bilevelRows(P, src, W, H, bits)) return nullptr;
            dd = (const uint8_t *)bits.data(), len = bits.size();
            bpc = 1;
            cs = deviceCS(CS::GRAY);
        }
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
    float matte[3] = {-1, 0, 0};   // (the /SMask's /Matte: the colours premultiplied towards it; its RGB, 0..255)
    if (smask.t == pp::REF) {
        ImageSource ms;
        haveAlpha = imageSource(P, smask, ms) && maskSamples(P, ms, false, alpha);
        auto m = nums(doc, ms.dict.t == pp::DICT ? ms.dict.get("Matte") : Val());
        if (haveAlpha && cs && (int)m.size() == cs->n && cs->n <= 4) {
            float rgb[3];
            cs->toRGB(m.data(), rgb);
            for (int c = 0; c < 3; c++) matte[c] = rgb[c] * 255;
        }
    } else if (maskv.t == pp::DICT || d.get("Mask").t == pp::REF) {
        ImageSource ms;
        haveAlpha = d.get("Mask").t == pp::REF && imageSource(P, d.get("Mask"), ms) && maskSamples(P, ms, true, alpha);
        // a stencil we cannot read (JBIG2): the image left out; it is often a scan's colour layer
        // (MRC), only its text shown by the mask, the rest junk
        if (!haveAlpha) return nullptr;
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
                if (matte[0] < 0) {
                    for (int c = 0; c < 4; c++) row[4 * x + c] = (uint8_t)(row[4 * x + c] * k / 255);
                    continue;
                }
                // premultiplied towards the matte (c' = m + k (c - m)): k c = c' - m (1 - k)
                for (int c = 0; c < 3; c++) {   // (B G R: the matte's 2 1 0)
                    float v = row[4 * x + c] - matte[2 - c] * (255 - k) / 255.f;
                    row[4 * x + c] = (uint8_t)lroundf(clampf(v, 0, (float)k));
                }
                row[4 * x + 3] = (uint8_t)k;
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
                    f.ranges.push_back({lo, hi, toU32(a[k + 2].num()), b0});
                }
            f.identity = false;
        } else if (o == "endcidchar" && !toUni) {
            for (size_t k = 0; k + 1 < n; k += 2, entries++)
                if (a[k].t == pp::STR && a[k + 1].isNum()) {
                    int b;
                    uint32_t c = code(a[k], b);
                    f.ranges.push_back({c, c, toU32(a[k + 1].num()), b});
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
    if (fw.isNum() && fw.num() >= 100) f.weight = toInt(fw.num());
    f.italic = has("italic") || has("oblique") || doc.get(desc, "ItalicAngle").num(0) != 0;
    int flags = toInt(doc.get(desc, "Flags").num(0));
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
        if (v.isNum()) code = toInt(v.num());
        else if (v.t == pp::NAME && code >= 0 && code < 256) {
            f.nameStore.emplace_back(v.str());
            f.names[code++] = nullptr;   // (filled below: nameStore may still move)
        }
    }
    code = 0;
    size_t s = 0;
    for (size_t k = 0; k < diff.size(); k++) {
        Val v = doc.resolve(diff.at(k));
        if (v.isNum()) code = toInt(v.num());
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
            uint32_t c0 = toU32(doc.resolve(w.at(k)).num());
            Val nx = doc.resolve(w.at(k + 1));
            if (nx.t == pp::ARRAY) {
                for (size_t j = 0; j < nx.size(); j++) f->cidWidths[c0 + (uint32_t)j] = (float)doc.resolve(nx.at(j)).num();
                k += 2;
            } else if (k + 2 < w.size()) {
                uint32_t c1 = toU32(nx.num());
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
    int first = toInt(doc.get(d, "FirstChar").num(0));
    Val ws = doc.get(d, "Widths");
    for (size_t k = 0; k < ws.size(); k++) {
        int64_t c = (int64_t)first + (int64_t)k;
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
    int flags = toInt(doc.get(desc, "Flags").num(0));
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
        plutovg_path_t *p = plutovg_path_create();
        nft::outlinePath(&f.face->glyph->outline, p, 1 / 64.f);   // (26.6 at 1000 pixels to the em: glyph units)
        out = std::make_shared<PathRef>(p);
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
    float groupA = 1;   // (the alpha the transparency groups around compose with: in fillA and strokeA)
    bool fillPattern = false, strokePattern = false, fillNone = false, strokeNone = false;
    std::shared_ptr<Shade> fillShade;   // (a shading pattern's: fills by it)
    std::shared_ptr<Tiling> fillTile;   // (a tiling pattern's)
    plutovg_matrix_t fillShadeM;        // (their space -> the page's display coordinates)
    std::shared_ptr<SoftMask> smask;   // (the soft mask set: what is painted is drawn through it)
    uint8_t blend = 0;                 // (the blend mode: BLEND_NAMES' index)
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
    // the default space of the content stream being read (the page's, a form's: its matrix in the
    // state it was drawn in) -> the display coordinates: what a pattern's matrix maps to (8.7.3.1)
    plutovg_matrix_t patBase;
    float pageW = 0, pageH = 0;   // (its size there)
    int patDepth = 0;   // (a tiling pattern's cell being read: how deep in patterns)
    std::unordered_map<uint32_t, std::shared_ptr<Tiling>> tilings;   // (read on this page, by object number)
    std::vector<Item> *out;   // (where items go: the page's list, a group's, a soft mask's)
    std::unordered_map<std::string, std::shared_ptr<SoftMask>> masks;   // (read on this page: by dictionary and matrix)
    int maskDepth = 0;
    // the items' boxes together (none: x1 < x0)
    static nd::Box itemsBox(const std::vector<Item> &items) {
        nd::Box b{0, 0, -1, -1};
        for (size_t k = 0; k < items.size(); k++) {
            const nd::Box &x = items[k].box;
            if (!k) b = x;
            else b.x0 = std::min(b.x0, x.x0), b.y0 = std::min(b.y0, x.y0), b.x1 = std::max(b.x1, x.x1), b.y1 = std::max(b.y1, x.y1);
        }
        return b;
    }
    // an item to the list, drawn in the blend mode and through the soft mask set
    void emit(Item &&it) {
        it.blend = gs.blend;
        it.smask = gs.smask;
        if (gs.blend) pg.blends = true;
        out->push_back(std::move(it));
    }
    std::unordered_map<std::string, int> clipIds;   // (the clips made: their index by parent, rule, matrix, path)
    GState gs;
    std::vector<GState> stack;
    plutovg_path_t *path = nullptr;
    int pendingClip = 0;   // 1 nonzero, 2 even-odd
    float cx = 0, cy = 0;  // (the current point, for v and y)
    size_t ops = 0, bytes = 0;   // (counted against MAX_OPS, MAX_BYTES)
    nd::Cancel cancel{nullptr, nullptr};   // asked every 4096 operators and glyphs: stopped is set, every run ends
    bool stopped = false;
    bool over() {   // (the page has no more operators to spend, or its reading was cancelled)
        if (!stopped && (++ops & 4095) == 0 && cancel()) stopped = true;
        return stopped || ops > MAX_OPS;
    }
    std::vector<uint32_t> forms;   // (the form XObjects being drawn: no cycles)
    std::vector<bool> hiddenStack;
    int hidden = 0;
    plutovg_matrix_t tm, tlm;           // the text matrix and the text line matrix
    plutovg_path_t *textClip = nullptr;  // (render modes 4-7: the glyphs clip at ET)
    std::vector<std::shared_ptr<Img>> decoded;   // (the images decoded for this page, held until it is built: one clipped away is not decoded again)

    Interp(nitropdf &p, PageDL &dl) : P(p), doc(p.doc), pg(dl), out(&dl.items) {}
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

    // A content stream decoded to be interpreted, at most limit bytes of it and what is left of the
    // page's MAX_BYTES; false when none is. (The operators alone did not bound the work: a form of
    // 64 MB of spaces has none, and it was decoded at each of its 2000 Do's.)
    bool content(const pp::Stream &st, pp::Decoded &dec, size_t limit) {
        if (bytes >= MAX_BYTES) return false;
        doc.decode(st, dec, std::min(limit, MAX_BYTES - bytes));
        bytes += dec.data.size();
        return true;
    }
    // The page's content streams joined by newlines (as pdfparse3's Doc::contents joins them), up to
    // MAX_PAGE_BYTES in all: a /Contents naming the same stream of 64 MB 1000 times is 64 GB.
    void pageContent(const pp::Page &page, std::string &out) {
        Val c = page.dict.get("Contents"), r = doc.resolve(c);
        size_t n = r.t == pp::ARRAY ? r.size() : 1;
        for (size_t k = 0; k < n && out.size() < MAX_PAGE_BYTES; k++) {
            Val part = r.t == pp::ARRAY ? r.at(k) : c;
            const pp::Stream *st = part.t == pp::REF ? doc.stream(part.oid()) : nullptr;
            pp::Decoded dec;
            if (!st || !content(*st, dec, MAX_PAGE_BYTES - out.size())) continue;
            if (!out.empty()) out.push_back('\n');
            out += dec.data;
        }
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
                plutovg_matrix_t inv;
                if (gs.fillPattern && gs.fillShade && plutovg_matrix_invert(&m, &inv)) {
                    it.shade = gs.fillShade;
                    mul(it.shadeM, gs.fillShadeM, inv);
                } else if (gs.fillPattern && gs.fillTile && plutovg_matrix_invert(&m, &inv)) {
                    it.tile = gs.fillTile;
                    mul(it.shadeM, gs.fillShadeM, inv);
                }
                if (clipBox(it.box, it.clip)) {
                    shared = std::make_shared<PathRef>(plutovg_path_reference(path));
                    it.path = shared;
                    emit(std::move(it));
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
        plutovg_matrix_t inv;
        bool tiled = gs.fillPattern && gs.fillTile && plutovg_matrix_invert(&m, &inv);
        if (!out->empty() && gs.fillA >= 1 && !tiled) {
            Item &l = out->back();
            if (l.kind == Item::FILL && l.text && !l.tile && l.blend == gs.blend && l.smask == gs.smask && l.clip == clip && l.rgb == gs.fillRgb && l.alpha >= 1 && !memcmp(&l.m, &m, sizeof m) &&
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
        if (tiled) {
            it.tile = gs.fillTile;
            mul(it.shadeM, gs.fillShadeM, inv);
        }
        it.box = b;
        plutovg_path_t *own = plutovg_path_create();
        plutovg_path_add_path(own, run, nullptr);
        it.path = std::make_shared<PathRef>(own);
        it.segments = elements;
        emit(std::move(it));
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
            if (over()) break;   // (a glyph counts as an operator: a Type 3 one showing a string of itself, 1000 each)
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
        if (!content(*st, dec, 16u << 20)) return;
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
        plutovg_rect_t unused;
        float length = gs.dash ? plutovg_path_extents(path, &unused, false) : 0;
        // the same stroke as the last item, opaque: one path (a CAD plot's "m l S" lines)
        if (!out->empty() && gs.strokeA >= 1 && !shared) {
            Item &l = out->back();
            if (l.kind == Item::STROKE && l.blend == gs.blend && l.smask == gs.smask && l.clip == clip && l.clipOwn == gs.clip && l.rgb == gs.strokeRgb && l.alpha >= 1 && l.width == gs.width && l.cap == gs.cap &&
                l.join == gs.join && l.miter == gs.miter && l.dash == gs.dash && l.dashOffset == gs.dashOffset && !memcmp(&l.m, &m, sizeof m) &&
                l.segments + elements <= (int)MAX_MERGED && l.path.use_count() == 1) {
                plutovg_path_add_path(l.path->p, path, nullptr);
                l.segments += elements;
                l.length += length;
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
        it.length = length;
        emit(std::move(it));
    }

    // an axial or radial shading (a dictionary or a stream) as gradient stops; null for the others
    // a shading, once a page for each object (a mesh in a soft mask set before each of many objects)
    std::unordered_map<uint32_t, std::shared_ptr<Shade>> shades;
    std::shared_ptr<Shade> loadShade(Val ref, Val res) {
        if (ref.t != pp::REF) return readShade(ref, res);
        auto known = shades.find(ref.oid());
        if (known != shades.end()) return known->second;
        return shades[ref.oid()] = readShade(ref, res);
    }
    std::shared_ptr<Shade> readShade(Val ref, Val res) {
        Val sh = doc.resolve(ref);
        if (sh.t != pp::DICT) {   // (a stream: its dictionary)
            const pp::Stream *st = ref.t == pp::REF ? doc.stream(ref.oid()) : nullptr;
            if (!st) return nullptr;
            sh = st->dict;
        }
        int type = toInt(doc.get(sh, "ShadingType").num(0));
        if (type == 1 || (type >= 4 && type <= 7)) return loadMesh(ref, sh, type, res);
        auto co = nums(doc, doc.get(sh, "Coords"));
        if ((type != 2 || co.size() < 4) && (type != 3 || co.size() < 6)) return nullptr;
        auto cs = loadCS(P, doc.get(sh, "ColorSpace"), res, 0);
        auto fn = loadFunc(P, sh.get("Function"), 0);   // (its reference: a sampled or PostScript one is a stream)
        if (!cs || !fn || cs->k == CS::PATTERN) return nullptr;
        auto s = std::make_shared<Shade>();
        s->type = type;
        for (size_t k = 0; k < 6 && k < co.size(); k++) s->c[k] = co[k];
        if (type == 3 && (s->c[2] < 0 || s->c[5] < 0)) return nullptr;
        auto dom = nums(doc, doc.get(sh, "Domain"));
        float t0 = dom.size() >= 2 ? dom[0] : 0, t1 = dom.size() >= 2 ? dom[1] : 1;
        Val ext = doc.resolve(doc.get(sh, "Extend"));
        for (size_t k = 0; k < 2 && k < ext.size(); k++) {
            Val e = doc.resolve(ext.at(k));
            s->extend[k] = e.t == pp::BOOL && e.i;
        }
        const int N = 64;   // (the function sampled: stitched ones change slope at their bounds)
        for (int k = 0; k < N; k++) {
            float t = t0 + (t1 - t0) * k / (N - 1), out[MAX_COMPS] = {0}, col[3];
            fn->eval(&t, out);
            cs->toRGB(out, col);
            plutovg_gradient_stop_t st;
            st.offset = (float)k / (N - 1);
            st.color = {clampf(col[0], 0, 1), clampf(col[1], 0, 1), clampf(col[2], 0, 1), 1};
            s->stops.push_back(st);
        }
        return s;
    }

    // a function-based (1) or mesh shading (4-7): its triangles; null if it has none or is not read
    std::shared_ptr<Shade> loadMesh(Val ref, Val sh, int type, Val res) {
        auto cs = loadCS(P, doc.get(sh, "ColorSpace"), res, 0);
        if (!cs || cs->k == CS::PATTERN) return nullptr;
        Val fv = sh.get("Function");   // (as above)
        std::shared_ptr<Func> fn = doc.resolve(fv).t == pp::NUL ? nullptr : loadFunc(P, fv, 0);
        if (doc.resolve(fv).t != pp::NUL && !fn) return nullptr;
        auto mesh = std::make_shared<Mesh>();
        Mesh &M = *mesh;
        const int S = Mesh::STRIDE;
        auto rgbOf = [&](const float *comp, float *out) {   // (its colour components: RGB)
            float col[3];
            cs->toRGB(comp, col);
            for (int k = 0; k < 3; k++) out[k] = clampf(col[k], 0, 1);
        };
        if (type == 1) {   // the function over its Domain, mapped by Matrix: a grid of its colours
            if (!fn || fn->nin != 2) return nullptr;
            auto dom = nums(doc, doc.get(sh, "Domain"));
            if (dom.size() < 4) dom = {0, 1, 0, 1};
            auto mx = nums(doc, doc.get(sh, "Matrix"));
            plutovg_matrix_t fm;
            if (mx.size() >= 6) plutovg_matrix_init(&fm, mx[0], mx[1], mx[2], mx[3], mx[4], mx[5]);
            else plutovg_matrix_init_identity(&fm);
            const int G = 48;
            for (int j = 0; j <= G; j++)
                for (int i = 0; i <= G; i++) {
                    float in[2] = {dom[0] + (dom[1] - dom[0]) * i / G, dom[2] + (dom[3] - dom[2]) * j / G}, out[MAX_COMPS] = {0};
                    plutovg_point_t p = {in[0], in[1]}, q;
                    plutovg_matrix_map_point(&fm, &p, &q);
                    fn->eval(in, out);
                    float c[3];
                    rgbOf(out, c);
                    M.v.insert(M.v.end(), {q.x, q.y, c[0], c[1], c[2]});
                }
            for (int j = 0; j < G; j++)
                for (int i = 0; i < G; i++) {
                    uint32_t a = j * (G + 1) + i;
                    M.tri.insert(M.tri.end(), {a, a + 1, a + G + 1, a + 1, a + G + 2, a + G + 1});
                }
        } else {
            const pp::Stream *st = ref.t == pp::REF ? doc.stream(ref.oid()) : nullptr;
            if (!st) return nullptr;
            int bc = toInt(doc.get(sh, "BitsPerCoordinate").num(0)), bk = toInt(doc.get(sh, "BitsPerComponent").num(0));
            int bf = type == 5 ? 0 : toInt(doc.get(sh, "BitsPerFlag").num(0));
            int nc = fn ? 1 : cs->n;
            auto dec = nums(doc, doc.get(sh, "Decode"));
            if (bc < 1 || bc > 32 || bk < 1 || bk > 16 || bf < 0 || bf > 8 || nc > MAX_COMPS || dec.size() < 4 + 2u * nc) return nullptr;
            pp::Decoded data;
            doc.decode(*st, data, 64u << 20);
            const uint8_t *b = (const uint8_t *)data.data.data();
            uint64_t bits = (uint64_t)data.data.size() * 8, at = 0;
            auto get = [&](int n) -> double {   // (n bits, high first)
                uint64_t v = 0;
                for (int k = 0; k < n; k++, at++) v = v << 1 | (b[at >> 3] >> (7 - (at & 7)) & 1);
                return (double)v;
            };
            double cmax = std::pow(2.0, bc) - 1, kmax = std::pow(2.0, bk) - 1;
            auto point = [&](float *xy) {
                xy[0] = (float)(dec[0] + get(bc) * (dec[1] - dec[0]) / cmax);
                xy[1] = (float)(dec[2] + get(bc) * (dec[3] - dec[2]) / cmax);
            };
            auto colour = [&](float *c) {   // (its components, or t made 0-1 over its Decode range)
                for (int k = 0; k < nc; k++) {
                    double v = get(bk) / kmax;
                    c[k] = fn ? (float)v : (float)(dec[4 + 2 * k] + v * (dec[5 + 2 * k] - dec[4 + 2 * k]));
                }
            };
            if (fn) {   // t's colours: the function at 256 points of the Decode range
                M.lut.resize(256);
                for (int k = 0; k < 256; k++) {
                    float t = dec[4] + (dec[5] - dec[4]) * k / 255.f, out[MAX_COMPS] = {0}, c[3];
                    fn->eval(&t, out);
                    rgbOf(out, c);
                    M.lut[k] = 0xff000000u | (uint32_t)lrintf(c[0] * 255) << 16 | (uint32_t)lrintf(c[1] * 255) << 8 | (uint32_t)lrintf(c[2] * 255);
                }
            }
            auto vertex = [&](const float *xy, const float *comp) {   // (appended; its index)
                float c[3] = {comp[0], 0, 0};
                if (!fn) rgbOf(comp, c);
                M.v.insert(M.v.end(), {xy[0], xy[1], c[0], c[1], c[2]});
                return (uint32_t)(M.v.size() / S - 1);
            };
            const size_t MAX_VERTICES = 4u << 20;
            if (type == 4 || type == 5) {
                int perRow = type == 5 ? toInt(doc.get(sh, "VerticesPerRow").num(0)) : 0;
                if (type == 5 && perRow < 2) return nullptr;
                uint64_t need = bf + 2 * bc + nc * bk;
                uint32_t va = 0, vb = 0, vc = 0;
                int have = 0;   // (type 4: the vertices of the triangle being started)
                while (at + need <= bits && M.v.size() / S < MAX_VERTICES) {
                    int flag = bf ? (int)get(bf) & 3 : 0;
                    float xy[2], comp[MAX_COMPS];
                    point(xy), colour(comp);
                    at = (at + 7) & ~7ull;   // (each vertex whole bytes)
                    uint32_t v = vertex(xy, comp);
                    if (type == 5) continue;
                    if (have < 3 && (have > 0 || flag == 0)) {
                        (have == 0 ? va : have == 1 ? vb : vc) = v;
                        if (++have == 3) M.tri.insert(M.tri.end(), {va, vb, vc});
                    } else if (flag == 0) va = v, have = 1;
                    else if (have == 3) {
                        if (flag == 1) va = vb, vb = vc, vc = v;
                        else vb = vc, vc = v;   // (2: side va vc)
                        M.tri.insert(M.tri.end(), {va, vb, vc});
                    }
                }
                if (type == 5) {
                    uint32_t rows = (uint32_t)(M.v.size() / S / perRow);
                    for (uint32_t i = 0; i + 1 < rows; i++)
                        for (uint32_t j = 0; j + 1 < (uint32_t)perRow; j++) {
                            uint32_t a = i * perRow + j, c2 = a + perRow;
                            M.tri.insert(M.tri.end(), {a, a + 1, c2, a + 1, c2, c2 + 1});
                        }
                }
            } else {   // the patches: 16 control points p[i][j] (column i, row j), the corners' colours
                static const uint8_t ORDER[16][2] = {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 3}, {2, 3}, {3, 3}, {3, 2},
                                                     {3, 1}, {3, 0}, {2, 0}, {1, 0}, {1, 1}, {1, 2}, {2, 2}, {2, 1}};
                float p[4][4][2], prev[4][4][2] = {}, cc[4][MAX_COMPS] = {}, pc[4][MAX_COMPS] = {};   // (cc: c00 c03 c33 c30)
                int npts = type == 6 ? 12 : 16;
                bool any = false;
                uint64_t patches = 0;
                while (M.v.size() / S < MAX_VERTICES) {
                    if (at + bf > bits) break;
                    int flag = (int)get(bf) & 3;
                    if (!any) flag = 0;
                    int first = flag ? 4 : 0;
                    if (at + (npts - first) * 2ull * bc + (flag ? 2 : 4) * (uint64_t)nc * bk > bits) break;
                    if (flag) {   // (the shared edge, the previous patch's: Table 85, 86)
                        static const uint8_t EDGE[4][4][2] = {{}, {{0, 3}, {1, 3}, {2, 3}, {3, 3}}, {{3, 3}, {3, 2}, {3, 1}, {3, 0}},
                                                              {{3, 0}, {2, 0}, {1, 0}, {0, 0}}};
                        static const uint8_t CEDGE[4][2] = {{0, 0}, {1, 2}, {2, 3}, {3, 0}};
                        for (int k = 0; k < 4; k++)
                            p[0][k][0] = prev[EDGE[flag][k][0]][EDGE[flag][k][1]][0], p[0][k][1] = prev[EDGE[flag][k][0]][EDGE[flag][k][1]][1];
                        memcpy(cc[0], pc[CEDGE[flag][0]], sizeof cc[0]);
                        memcpy(cc[1], pc[CEDGE[flag][1]], sizeof cc[1]);
                    }
                    for (int k = first; k < npts; k++) point(p[ORDER[k][0]][ORDER[k][1]]);
                    for (int k = flag ? 2 : 0; k < 4; k++) colour(cc[k]);
                    at = (at + 7) & ~7ull;
                    if (type == 6)   // (a Coons patch: its inner points from the edges, 8.7.4.5.8)
                        for (int d = 0; d < 2; d++) {
                            auto P_ = [&](int i, int j) { return p[i][j][d]; };
                            p[1][1][d] = (-4 * P_(0, 0) + 6 * (P_(0, 1) + P_(1, 0)) - 2 * (P_(0, 3) + P_(3, 0)) + 3 * (P_(3, 1) + P_(1, 3)) - P_(3, 3)) / 9;
                            p[1][2][d] = (-4 * P_(0, 3) + 6 * (P_(0, 2) + P_(1, 3)) - 2 * (P_(0, 0) + P_(3, 3)) + 3 * (P_(3, 2) + P_(1, 0)) - P_(3, 0)) / 9;
                            p[2][1][d] = (-4 * P_(3, 0) + 6 * (P_(3, 1) + P_(2, 0)) - 2 * (P_(3, 3) + P_(0, 0)) + 3 * (P_(0, 1) + P_(2, 3)) - P_(0, 3)) / 9;
                            p[2][2][d] = (-4 * P_(3, 3) + 6 * (P_(3, 2) + P_(2, 3)) - 2 * (P_(3, 0) + P_(0, 3)) + 3 * (P_(0, 2) + P_(2, 0)) - P_(0, 0)) / 9;
                        }
                    memcpy(prev, p, sizeof p);
                    memcpy(pc, cc, sizeof cc);
                    any = true;
                    // a grid of N x N cells over (u, v); rows of v last, so that a patch folding over itself shows its larger v
                    int N = patches < 512 ? 16 : patches < 8192 ? 8 : 4;
                    patches++;
                    uint32_t base0 = (uint32_t)(M.v.size() / S);
                    for (int jv = 0; jv <= N; jv++)
                        for (int iu = 0; iu <= N; iu++) {
                            float u = (float)iu / N, v = (float)jv / N, bu[4], bv[4];
                            float mu = 1 - u, mv = 1 - v;
                            bu[0] = mu * mu * mu, bu[1] = 3 * u * mu * mu, bu[2] = 3 * u * u * mu, bu[3] = u * u * u;
                            bv[0] = mv * mv * mv, bv[1] = 3 * v * mv * mv, bv[2] = 3 * v * v * mv, bv[3] = v * v * v;
                            float xy[2] = {0, 0}, comp[MAX_COMPS];
                            for (int i = 0; i < 4; i++)
                                for (int j = 0; j < 4; j++) xy[0] += p[i][j][0] * bu[i] * bv[j], xy[1] += p[i][j][1] * bu[i] * bv[j];
                            for (int k = 0; k < nc; k++)   // (c00 at u 0 v 0, c03 at u 0 v 1, c33 at 1 1, c30 at 1 0)
                                comp[k] = mu * mv * cc[0][k] + mu * v * cc[1][k] + u * v * cc[2][k] + u * mv * cc[3][k];
                            vertex(xy, comp);
                        }
                    for (int jv = 0; jv < N; jv++)
                        for (int iu = 0; iu < N; iu++) {
                            uint32_t a = base0 + jv * (N + 1) + iu, c2 = a + N + 1;
                            M.tri.insert(M.tri.end(), {a, a + 1, c2, a + 1, c2 + 1, c2});
                        }
                }
            }
        }
        if (M.tri.empty()) return nullptr;
        M.x0 = M.y0 = 1e30f, M.x1 = M.y1 = -1e30f;
        for (size_t k = 0; k < M.v.size(); k += S) {
            if (!std::isfinite(M.v[k]) || !std::isfinite(M.v[k + 1])) return nullptr;
            M.x0 = std::min(M.x0, M.v[k]), M.x1 = std::max(M.x1, M.v[k]);
            M.y0 = std::min(M.y0, M.v[k + 1]), M.y1 = std::max(M.y1, M.v[k + 1]);
        }
        auto s = std::make_shared<Shade>();
        s->type = type;
        s->mesh = mesh;
        return s;
    }

    // sh: the shading painted over the clip (its /BBox, if any, limiting it; an axial one not
    // extended: only between the lines its ends are on)
    void shFill(Val name, Val res) {
        if (hidden) return;
        Val ref = resource(P, res, "Shading", name);
        auto s = loadShade(ref, res);
        if (!s) return;
        plutovg_matrix_t m, inv;
        mul(m, gs.ctm, base);
        if (!plutovg_matrix_invert(&m, &inv)) return;
        // the area: the clip's box (or the page) back in user space, a quad
        nd::Box b{0, 0, pageW, pageH};
        if (gs.clip >= 0) b = pg.clips[gs.clip].box;
        std::vector<plutovg_point_t> poly;
        for (int k = 0; k < 4; k++) {
            plutovg_point_t q = {k == 1 || k == 2 ? b.x1 : b.x0, k >= 2 ? b.y1 : b.y0}, u;
            plutovg_matrix_map_point(&inv, &q, &u);
            poly.push_back(u);
        }
        auto cut = [&](float nx, float ny, float d) {   // (keeps nx x + ny y >= d)
            std::vector<plutovg_point_t> out;
            for (size_t k = 0; k < poly.size(); k++) {
                plutovg_point_t p = poly[k], q = poly[(k + 1) % poly.size()];
                float fp = nx * p.x + ny * p.y - d, fq = nx * q.x + ny * q.y - d;
                if (fp >= 0) out.push_back(p);
                if ((fp >= 0) != (fq >= 0)) out.push_back({p.x + (q.x - p.x) * fp / (fp - fq), p.y + (q.y - p.y) * fp / (fp - fq)});
            }
            poly.swap(out);
        };
        auto bb = nums(doc, doc.get(doc.resolve(ref).t == pp::DICT ? doc.resolve(ref) : Val(), "BBox"));
        if (bb.size() == 4) {
            cut(1, 0, std::min(bb[0], bb[2])), cut(-1, 0, -std::max(bb[0], bb[2]));
            cut(0, 1, std::min(bb[1], bb[3])), cut(0, -1, -std::max(bb[1], bb[3]));
        }
        if (s->mesh) {
            // (a mesh: the area not cut at its box, its triangles' own edges show it; cut, the edges smoothed
            // where meshes meet would leave a lighter line between them)
        } else if (s->type == 2) {
            float dx = s->c[2] - s->c[0], dy = s->c[3] - s->c[1];
            if (!s->extend[0]) cut(dx, dy, dx * s->c[0] + dy * s->c[1]);
            if (!s->extend[1]) cut(-dx, -dy, -(dx * s->c[2] + dy * s->c[3]));
        }
        if (poly.size() < 3) return;
        plutovg_path_t *area = plutovg_path_create();
        if (s->type == 3 && !s->extend[1]) {   // (radial, not extended: within its end circle)
            plutovg_path_add_circle(area, s->c[3], s->c[4], s->c[5]);
        } else {
            plutovg_path_move_to(area, poly[0].x, poly[0].y);
            for (size_t k = 1; k < poly.size(); k++) plutovg_path_line_to(area, poly[k].x, poly[k].y);
            plutovg_path_close(area);
        }
        Item it;
        it.kind = Item::FILL;
        it.clip = gs.clip;
        it.alpha = gs.fillA;
        it.m = m;
        it.box = box(area, m, 0);
        if (s->mesh) {   // (what it covers: within its triangles' box)
            const Mesh &M = *s->mesh;
            nd::Box mb = boxOf(M.x0, M.y0, M.x1, M.y1, m);
            it.box.x0 = std::max(it.box.x0, mb.x0), it.box.y0 = std::max(it.box.y0, mb.y0);
            it.box.x1 = std::min(it.box.x1, mb.x1), it.box.y1 = std::min(it.box.y1, mb.y1);
        }
        it.shade = s;
        plutovg_matrix_init_identity(&it.shadeM);
        if (clipBox(it.box, it.clip)) {
            it.path = std::make_shared<PathRef>(area);
            emit(std::move(it));
        } else plutovg_path_destroy(area);
    }

    // A tiling pattern's cell read into a display list of its own (in the pattern's space, clipped by
    // its BBox), once a page; null if it cannot be (no steps, a pattern in itself, too deep). Its
    // operators and bytes count against the page's.
    std::shared_ptr<Tiling> loadTiling(Val ref, Val res) {
        if (ref.t != pp::REF) return nullptr;
        auto known = tilings.find(ref.oid());
        if (known != tilings.end()) return known->second;
        tilings[ref.oid()] = nullptr;   // (while it is read: a pattern drawn in its own cell is not)
        const pp::Stream *st = doc.stream(ref.oid());
        if (!st || patDepth >= 4) return nullptr;
        Val d = st->dict;
        auto bb = nums(doc, d.get("BBox"));
        float xs = fabsf((float)doc.get(d, "XStep").num(0)), ys = fabsf((float)doc.get(d, "YStep").num(0));
        if (bb.size() != 4 || !(xs > 1e-6f && xs < 1e9f) || !(ys > 1e-6f && ys < 1e9f)) return nullptr;
        for (float v : bb)
            if (!std::isfinite(v) || fabsf(v) > 1e9f) return nullptr;
        pp::Decoded dec;
        if (!content(*st, dec, 64u << 20)) return nullptr;
        auto t = std::make_shared<Tiling>();
        t->cell = std::make_shared<PageDL>();
        t->bbox[0] = std::min(bb[0], bb[2]), t->bbox[1] = std::min(bb[1], bb[3]);
        t->bbox[2] = std::max(bb[0], bb[2]), t->bbox[3] = std::max(bb[1], bb[3]);
        t->xs = xs, t->ys = ys;
        t->uncoloured = toInt(doc.get(d, "PaintType").num(1)) == 2;
        Interp in(P, *t->cell);
        in.cancel = cancel, in.ops = ops, in.bytes = bytes;
        in.patDepth = patDepth + 1;
        plutovg_matrix_init_identity(&in.base);
        in.patBase = in.base;
        in.pageW = in.pageH = 1e30f;   // (no "page" a clip could cover)
        plutovg_matrix_init_identity(&in.gs.ctm);
        plutovg_matrix_init_identity(&in.tm);
        in.tlm = in.tm;
        in.resetColors();
        in.newPath();
        plutovg_path_add_rect(in.path, t->bbox[0], t->bbox[1], t->bbox[2] - t->bbox[0], t->bbox[3] - t->bbox[1]);
        in.addClip(false);
        in.newPath();
        Val pres = doc.get(d, "Resources");
        in.run(dec.data.data(), dec.data.size(), pres.t == pp::DICT ? pres : res, 0);
        ops = in.ops, bytes = in.bytes;
        if (in.stopped) stopped = true;
        decoded.insert(decoded.end(), in.decoded.begin(), in.decoded.end());
        return tilings[ref.oid()] = t;
    }

    void setColor(bool fill, const Val *args, size_t n, Val res) {
        std::shared_ptr<CS> &cs = fill ? gs.fillCS : gs.strokeCS;
        float *c = fill ? gs.fillC : gs.strokeC;
        bool &pat = fill ? gs.fillPattern : gs.strokePattern;
        uint32_t &rgb = fill ? gs.fillRgb : gs.strokeRgb;
        bool &none = fill ? gs.fillNone : gs.strokeNone;
        if (fill) gs.fillShade.reset(), gs.fillTile.reset();
        if (cs->k == CS::PATTERN) {
            // a pattern: a shading pattern fills by its shading, a tiling one by its cell; what they
            // cannot (a stroke) with the shading's middle colour, grey, an uncoloured pattern's colour
            pat = true;
            none = false;
            rgb = 0x808080;
            if (n && args[n - 1].t == pp::NAME) {
                Val pref = resource(P, res, "Pattern", args[n - 1]);
                Val p = doc.resolve(pref);
                auto pm = nums(doc, doc.get(p, "Matrix"));   // (the pattern's space -> the content stream's default one)
                plutovg_matrix_t pmat;
                if (pm.size() == 6) plutovg_matrix_init(&pmat, pm[0], pm[1], pm[2], pm[3], pm[4], pm[5]);
                else plutovg_matrix_init_identity(&pmat);
                if (toInt(doc.get(p, "PatternType").num(0)) == 2) {
                    Val sh = doc.get(p, "Shading");
                    auto scs = loadCS(P, doc.resolve(sh).get("ColorSpace"), res, 0);
                    auto fn = loadFunc(P, doc.resolve(sh).get("Function"), 0);
                    auto dom = nums(doc, doc.resolve(sh).get("Domain"));
                    if (scs && fn) {
                        float in[8], out[MAX_COMPS] = {0}, col[3];   // (the middle of each input's domain: a function-based shading has two)
                        for (size_t k = 0; k < 8; k++) in[k] = dom.size() >= 2 * k + 2 ? (dom[2 * k] + dom[2 * k + 1]) / 2 : 0.5f;
                        fn->eval(in, out);
                        scs->toRGB(out, col);
                        rgb = packRgb(col);
                    }
                    if (fill && (gs.fillShade = loadShade(sh, res))) mul(gs.fillShadeM, pmat, patBase);
                } else {
                    if (cs->base && n > 1) {   // (an uncoloured tiling pattern: its colour)
                        float cc[32];
                        for (size_t k = 0; k + 1 < n && k < 32; k++) cc[k] = (float)args[k].num();
                        float col[3];
                        cs->base->toRGB(cc, col);
                        rgb = packRgb(col);
                    }
                    if (fill && (gs.fillTile = loadTiling(pref, res))) mul(gs.fillShadeM, pmat, patBase);
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
        if (fill) gs.fillShade.reset(), gs.fillTile.reset();
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
        v = doc.get(g, "SMask");
        if (v.t == pp::NAME) gs.smask.reset();
        else if (v.t == pp::DICT) gs.smask = loadSoftMask(g.get("SMask"), res);
        v = doc.get(g, "BM");   // (an array: the first of its modes known)
        for (size_t k = 0; k < (v.t == pp::ARRAY ? v.size() : 1); k++) {
            Val b = v.t == pp::ARRAY ? doc.resolve(v.at(k)) : v;
            if (b.t != pp::NAME) continue;
            int found = b.is("Compatible") ? 0 : -1;
            for (int i = 0; i < 16 && found < 0; i++)
                if (b.is(BLEND_NAMES[i])) found = i;
            if (found >= 0) {
                gs.blend = (uint8_t)found;
                break;
            }
        }
        if ((v = doc.get(g, "LC")).isNum()) gs.cap = (uint8_t)std::max(0, std::min(2, toInt(v.num())));
        if ((v = doc.get(g, "LJ")).isNum()) gs.join = (uint8_t)std::max(0, std::min(2, toInt(v.num())));
        if ((v = doc.get(g, "ML")).isNum()) gs.miter = (float)v.num();
        if ((v = doc.get(g, "CA")).isNum()) gs.strokeA = clampf((float)v.num(), 0, 1) * gs.groupA;
        if ((v = doc.get(g, "ca")).isNum()) gs.fillA = clampf((float)v.num(), 0, 1) * gs.groupA;
        if ((v = doc.get(g, "D")).t == pp::ARRAY && v.size() >= 2) setDash(doc.resolve(v.at(0)), doc.resolve(v.at(1)));
    }
    // A soft mask dictionary's group read into a list of its own, in the current matrix (11.6.5.2), its
    // painting in the initial state (no alpha, mask, blend mode or clip); once a page for the same
    // dictionary and matrix (Illustrator sets the same one before each object). Null: none (no group).
    std::shared_ptr<SoftMask> loadSoftMask(Val ref, Val res) {
        Val d = doc.resolve(ref);
        Val g = d.get("G");
        const pp::Stream *st = g.t == pp::REF ? doc.stream(g.oid()) : nullptr;
        if (!st || maskDepth >= 4) return nullptr;
        std::string key((const char *)&d.v, sizeof d.v);
        key.append((const char *)&gs.ctm, sizeof gs.ctm);
        auto known = masks.find(key);
        if (known != masks.end()) return known->second;
        auto sm = std::make_shared<SoftMask>();
        sm->luminosity = !doc.get(d, "S").is("Alpha");
        Val grp = doc.get(st->dict, "Group");
        auto bc = nums(doc, d.get("BC"));
        Val gres = doc.get(st->dict, "Resources");
        auto cs = loadCS(P, doc.get(grp, "CS"), gres.t == pp::DICT ? gres : res, 0);
        if (cs && cs->k != CS::PATTERN && !bc.empty()) {
            float c[32] = {0}, col[3];
            for (size_t k = 0; k < bc.size() && k < 32; k++) c[k] = bc[k];
            cs->toRGB(c, col);
            sm->bc = packRgb(col);
        }
        Val tr = doc.get(d, "TR");
        auto fn = tr.t == pp::NAME ? nullptr : loadFunc(P, d.get("TR"), 0);
        for (int k = 0; k < 256; k++) {
            float in = k / 255.f, o[MAX_COMPS] = {in};
            if (fn) fn->eval(&in, o);
            sm->tr[k] = (uint8_t)lrintf(clampf(o[0], 0, 1) * 255);
        }
        std::vector<Item> *savedOut = out;
        out = &sm->items;
        stack.push_back(gs);
        GState fresh;
        fresh.ctm = gs.ctm;
        gs = fresh;
        resetColors();
        int savedHidden = hidden;
        hidden = 0;
        maskDepth++;
        doForm(g.oid(), *st, res, 0);
        maskDepth--;
        hidden = savedHidden;
        gs = stack.back();
        stack.pop_back();
        out = savedOut;
        sm->box = itemsBox(sm->items);
        return masks[key] = sm;
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
        emit(std::move(it));
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
            // The same picture again, on this page or another: decoded once while any of them holds it.
            // A stencil mask is painted with the fill colour: one for each colour it is drawn in (a
            // 1024 x 1024 one drawn 3000 times was decoded 3000 times, 4 MB each).
            uint64_t imageKey = (uint64_t)(stencil ? 0x1000000u | gs.fillRgb : 0u) << 32 | ref.oid();
            std::shared_ptr<Img> img;
            auto known = P.images.find(imageKey);
            if (known != P.images.end()) img = known->second.lock();
            if (!img) {
                ImageSource src;
                if (!imageSource(P, ref, src)) return;
                img = decodeImage(P, src, res, gs.fillRgb, 1);
                if (!img || !img->s) return;
                P.images[imageKey] = img;
                decoded.push_back(img);
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
        if (!content(st, dec, 256u << 20)) return;
        Val fres = doc.get(d, "Resources");
        if (fres.t != pp::DICT) fres = doc.resolve(res);
        stack.push_back(gs);
        // A transparency group painted with an alpha, a blend mode or a soft mask: its items in a
        // group of their own, painted from the initial state of those, the group composed by them
        // (11.4.7, 11.6.6). Without them it is drawn as its items, as a non-isolated group is.
        // (Isolated or not, the group is composed alone: what its blend modes do with the
        // backdrop beneath it is not seen.)
        Val grp = doc.get(d, "Group");
        std::shared_ptr<Group> group;
        Item gi;
        std::vector<Item> *savedOut = this->out;
        if (grp.t == pp::DICT && doc.get(grp, "S").is("Transparency")) {
            if (gs.blend || gs.smask || gs.fillA < 1) {
                group = std::make_shared<Group>();
                gi.alpha = gs.fillA, gi.blend = gs.blend, gi.smask = gs.smask;
                this->out = &group->items;
                gs.fillA = gs.strokeA = gs.groupA = 1;
                gs.blend = 0;
                gs.smask.reset();
            } else gs.groupA = gs.strokeA = gs.fillA;
        }
        auto mtx = nums(doc, d.get("Matrix"));
        if (mtx.size() == 6) {
            plutovg_matrix_t fm, out;
            plutovg_matrix_init(&fm, mtx[0], mtx[1], mtx[2], mtx[3], mtx[4], mtx[5]);
            mul(out, fm, gs.ctm);
            gs.ctm = out;
        }
        plutovg_matrix_t savedPatBase = patBase;
        mul(patBase, gs.ctm, base);
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
        patBase = savedPatBase;
        gs = stack.back();
        stack.pop_back();
        this->out = savedOut;
        if (group && !group->items.empty()) {
            gi.kind = Item::GROUP;
            gi.group = group;
            gi.box = itemsBox(group->items);
            plutovg_matrix_init_identity(&gi.m);
            if (gi.blend) pg.blends = true;
            savedOut->push_back(std::move(gi));
        }
    }

    void run(const char *data, size_t len, Val res, int depth) {
        if (!path) newPath();
        size_t qBase = stack.size();
        pp::ContentLexer lx(data, len);
        pp::Op op;
        while (lx.next(op)) {
            if (over()) break;
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
                else if (o == "sh" && n >= 1) shFill(a[0], res);
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
                if (o == "J" && n >= 1) gs.cap = (uint8_t)std::max(0, std::min(2, toInt(num(0))));
                break;
            case 'j':
                if (o == "j" && n >= 1) gs.join = (uint8_t)std::max(0, std::min(2, toInt(num(0))));
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
                else if (o == "Tr" && n >= 1) gs.render = toInt(num(0)) & 7;
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
    for (size_t k = 0; k < annots.size() && k < 100000 && !in.stopped; k++) {
        Val a = doc.resolve(annots.at(k));
        if (a.t != pp::DICT || (toInt(doc.get(a, "F").num(0)) & (2 | 32))) continue;   // (Hidden, NoView)
        Val oc = a.get("OC");
        if (oc.t != pp::NUL && !in.visible(oc)) continue;
        Val n = doc.get(a, "AP").get("N"), ap = n;   // (unresolved: a stream's reference)
        // not a stream but a dictionary, direct or indirect: the appearance states, the one /AS names
        if (!(n.t == pp::REF && doc.stream(n.oid())) && doc.resolve(n).t == pp::DICT) {
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

// The page's display list; false if cancel said to stop: then it is left empty, to be built again.
bool buildPage(nitropdf &P, int k, PageDL &pg, const nd::Cancel &cancel) {
    const pp::Page &page = P.pages[k];
    if (cancel()) return false;
    Interp in(P, pg);
    in.cancel = cancel;
    double pw, ph;
    pageGeometry(page, &pw, &ph, &in.base);
    in.patBase = in.base;
    in.pageW = (float)pw, in.pageH = (float)ph;
    plutovg_matrix_init_identity(&in.gs.ctm);
    plutovg_matrix_init_identity(&in.tm);
    in.tlm = in.tm;
    in.resetColors();
    std::string content;
    in.pageContent(page, content);
    GState initial = in.gs;
    in.run(content.data(), content.size(), page.resources, 0);
    drawAnnotations(in, page, initial);
    if (in.stopped) pg.items.clear(), pg.clips.clear();
    return !in.stopped;
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

// A mesh's triangles, Gouraud-shaded, into a surface of w x h pixels: d maps the shading's space to its
// pixels. Opaque where the triangles are, transparent elsewhere: a pixel is a triangle's if its centre is
// in it (not smoothed, so that neighbours meet without a seam), the later triangle's where they overlap.
void rasterMesh(const Mesh &M, const plutovg_matrix_t &d, plutovg_surface_t *s) {
    int w = plutovg_surface_get_width(s), h = plutovg_surface_get_height(s), stride = plutovg_surface_get_stride(s);
    uint8_t *px = plutovg_surface_get_data(s);
    const int S = Mesh::STRIDE;
    bool lut = !M.lut.empty();
    for (size_t t = 0; t + 2 < M.tri.size(); t += 3) {
        const float *v[3];
        float x[3], y[3];
        for (int k = 0; k < 3; k++) {
            v[k] = &M.v[(size_t)M.tri[t + k] * S];
            x[k] = d.a * v[k][0] + d.c * v[k][1] + d.e, y[k] = d.b * v[k][0] + d.d * v[k][1] + d.f;
        }
        float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
        if (!(fabsf(area) > 1e-12f)) continue;
        if (area < 0) std::swap(x[1], x[2]), std::swap(y[1], y[2]), std::swap(v[1], v[2]), area = -area;
        float fx0 = std::min({x[0], x[1], x[2]}), fx1 = std::max({x[0], x[1], x[2]});
        float fy0 = std::min({y[0], y[1], y[2]}), fy1 = std::max({y[0], y[1], y[2]});
        if (!(fx1 >= 0 && fy1 >= 0 && fx0 < w && fy0 < h)) continue;
        int ix0 = std::max(0, (int)floorf(fx0)), ix1 = std::min(w - 1, (int)ceilf(fx1));
        int iy0 = std::max(0, (int)floorf(fy0)), iy1 = std::min(h - 1, (int)ceilf(fy1));
        // the edge functions (e_k: of the edge opposite vertex k, positive inside), each edge moved out by a
        // quarter pixel: two meshes meant to meet but a hair apart (their coordinates rounded each its own
        // way) leave no row of pixels between them
        float ea[3], eb[3], ec[3], out[3];
        for (int k = 0; k < 3; k++) {
            int i = (k + 1) % 3, j = (k + 2) % 3;
            ea[k] = y[i] - y[j], eb[k] = x[j] - x[i], ec[k] = x[i] * y[j] - x[j] * y[i];
            out[k] = -0.25f * sqrtf(ea[k] * ea[k] + eb[k] * eb[k]);
        }
        float inv = 1 / area;
        for (int py = iy0; py <= iy1; py++) {
            uint32_t *row = (uint32_t *)(px + (size_t)py * stride);
            float cy = py + 0.5f;
            for (int pxi = ix0; pxi <= ix1; pxi++) {
                float cx = pxi + 0.5f, e[3];
                bool in = true;
                for (int k = 0; k < 3 && in; k++) {
                    e[k] = ea[k] * cx + eb[k] * cy + ec[k];
                    in = e[k] >= out[k];
                }
                if (!in) continue;
                float b0 = e[0] * inv, b1 = e[1] * inv, b2 = 1 - b0 - b1;
                uint32_t col;
                if (lut) {
                    float tt = b0 * v[0][2] + b1 * v[1][2] + b2 * v[2][2];
                    col = M.lut[(int)lrintf(clampf(tt, 0, 1) * 255)];
                } else {
                    float r = b0 * v[0][2] + b1 * v[1][2] + b2 * v[2][2];
                    float g = b0 * v[0][3] + b1 * v[1][3] + b2 * v[2][3];
                    float bl = b0 * v[0][4] + b1 * v[1][4] + b2 * v[2][4];
                    col = 0xff000000u | (uint32_t)lrintf(clampf(r, 0, 1) * 255) << 16 | (uint32_t)lrintf(clampf(g, 0, 1) * 255) << 8 |
                          (uint32_t)lrintf(clampf(bl, 0, 1) * 255);
                }
                row[pxi] = col;
            }
        }
    }
}

// A fill by a mesh: its triangles drawn into a surface over the part of the canvas the item's box covers
// (coarser if that is very large), filling the path as a texture
void meshFill(plutovg_canvas_t *c, const Item &it, const plutovg_matrix_t &m, const plutovg_matrix_t &view) {
    plutovg_surface_t *cs = plutovg_canvas_get_surface(c);
    int W = plutovg_surface_get_width(cs), H = plutovg_surface_get_height(cs);
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int k = 0; k < 4; k++) {   // (the item's box on the canvas)
        plutovg_point_t p = {k & 1 ? it.box.x1 : it.box.x0, k & 2 ? it.box.y1 : it.box.y0}, q;
        plutovg_matrix_map_point(&view, &p, &q);
        x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), y0 = std::min(y0, q.y), y1 = std::max(y1, q.y);
    }
    int ox = std::max(0, (int)floorf(x0) - 1), oy = std::max(0, (int)floorf(y0) - 1);
    int ex = std::min(W, (int)ceilf(x1) + 1), ey = std::min(H, (int)ceilf(y1) + 1);
    if (ex <= ox || ey <= oy) return;
    int f = 1;
    while ((double)(ex - ox) / f * ((ey - oy) / f) > (16 << 20)) f *= 2;
    int sw = (ex - ox + f - 1) / f, sh = (ey - oy + f - 1) / f;
    plutovg_surface_t *s = plutovg_surface_create(sw, sh);
    if (!s) return;
    plutovg_matrix_t toDev, d, sc;   // (the shading's space -> the canvas -> the surface)
    plutovg_matrix_multiply(&toDev, &it.shadeM, &m);
    plutovg_matrix_init(&sc, 1.f / f, 0, 0, 1.f / f, -(float)ox / f, -(float)oy / f);
    plutovg_matrix_multiply(&d, &toDev, &sc);
    rasterMesh(*it.shade->mesh, d, s);
    plutovg_matrix_t inv, back, tm;   // (the texture: the surface's pixels -> the path's space)
    if (plutovg_matrix_invert(&m, &inv)) {
        plutovg_matrix_init(&back, (float)f, 0, 0, (float)f, (float)ox, (float)oy);
        plutovg_matrix_multiply(&tm, &back, &inv);
        plutovg_canvas_set_texture(c, s, PLUTOVG_TEXTURE_TYPE_PLAIN, it.alpha, &tm);
        plutovg_canvas_set_fill_rule(c, it.evenOdd ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
        plutovg_canvas_fill_path(c, it.path->p);
    }
    plutovg_surface_destroy(s);
}

// The tiles a render has drawn, by pattern, size and colour: a pattern filling many paths is drawn once
struct TileCache {
    struct Entry {
        const Tiling *t;
        int w, h, nx, ny;
        uint32_t rgb;
        float a, b, c, d;   // (the linear part of the pattern's space -> the canvas)
        plutovg_surface_t *s;
    };
    std::vector<Entry> e;
    // the soft masks drawn: over a canvas of w x h under view, the part their items cover
    struct Mask {
        const SoftMask *m;
        int cw, ch;
        float view[6];
        int x0, y0, w, h;
        std::vector<uint8_t> v;
        uint8_t outside;   // (beyond it)
        uint8_t at(int x, int y) const {
            x -= x0, y -= y0;
            return x >= 0 && y >= 0 && x < w && y < h ? v[(size_t)y * w + x] : outside;
        }
    };
    std::vector<std::unique_ptr<Mask>> masks;
    ~TileCache() {
        for (auto &x : e) plutovg_surface_destroy(x.s);
    }
};

void drawItems(nitropdf &P, const Item *first, const Item *last, const std::vector<Clip> &clips, plutovg_canvas_t *c,
               const plutovg_matrix_t &view, const double vb[4], double scale, const nd::Cancel &cancel, int &rc, TileCache *tc,
               bool plain = false);
void drawItems(nitropdf &P, const PageDL &pg, plutovg_canvas_t *c, const plutovg_matrix_t &view, const double vb[4], double scale,
               const nd::Cancel &cancel, int &rc, TileCache *tc = nullptr) {
    drawItems(P, pg.items.data(), pg.items.data() + pg.items.size(), pg.clips, c, view, vb, scale, cancel, rc, tc);
}

const int MAX_TILE_PIXELS = 4 << 20, MIN_TILE = 16, MAX_TILE_COPIES = 4096;

// A fill by a tiling pattern: its cell drawn into a tile at the canvas's resolution (nx x ny cells
// of a small pattern in one tile, at least MIN_TILE pixels across; coarser if more than
// MAX_TILE_PIXELS), with the neighbouring cells' parts reaching into it (a BBox larger than the
// steps), filling the path as a repeated texture. An uncoloured one: the tile in its colour.
void tileFill(nitropdf &P, plutovg_canvas_t *c, const Item &it, const plutovg_matrix_t &m, TileCache &tc, const nd::Cancel &cancel,
              int &rc) {
    const Tiling &T = *it.tile;
    plutovg_matrix_t pd;   // (the pattern's space -> the canvas)
    plutovg_matrix_multiply(&pd, &it.shadeM, &m);
    double ux = hypot(pd.a, pd.b), uy = hypot(pd.c, pd.d), w = T.xs * ux, h = T.ys * uy;
    if (!(w > 1e-4 && h > 1e-4 && w < 1e9 && h < 1e9)) return;
    int nx = std::min(64, std::max(1, (int)ceil(MIN_TILE / w))), ny = std::min(64, std::max(1, (int)ceil(MIN_TILE / h)));
    double W = w * nx, H = h * ny;
    if (W * H > MAX_TILE_PIXELS) {
        double k = sqrt(MAX_TILE_PIXELS / (W * H));
        W *= k, H *= k;
    }
    int iw = std::max(1, (int)lround(W)), ih = std::max(1, (int)lround(H));
    uint32_t rgb = T.uncoloured ? it.rgb : 0;
    plutovg_surface_t *s = nullptr;
    for (auto &x : tc.e)
        if (x.t == &T && x.w == iw && x.h == ih && x.nx == nx && x.ny == ny && x.rgb == rgb && x.a == pd.a && x.b == pd.b && x.c == pd.c &&
            x.d == pd.d)
            s = x.s;
    float kx = (float)(iw / (T.xs * nx)), ky = (float)(ih / (T.ys * ny));   // (tile pixels per pattern unit)
    if (!s) {
        if (!(s = plutovg_surface_create(iw, ih))) return;
        plutovg_canvas_t *tcv = plutovg_canvas_create(s);
        if (!tcv) {
            plutovg_surface_destroy(s);
            return;
        }
        // the cells whose box reaches into the tile's nx x ny of them
        float X = T.xs * nx, Y = T.ys * ny;
        int i0 = (int)std::max(-1e6, floor(-T.bbox[2] / (double)T.xs)), i1 = (int)std::min(1e6, ceil((X - T.bbox[0]) / (double)T.xs));
        int j0 = (int)std::max(-1e6, floor(-T.bbox[3] / (double)T.ys)), j1 = (int)std::min(1e6, ceil((Y - T.bbox[1]) / (double)T.ys));
        long copies = 0;
        for (int j = j0; j <= j1 && !rc; j++)
            for (int i = i0; i <= i1 && !rc && copies < MAX_TILE_COPIES; i++) {
                float dx = i * T.xs, dy = j * T.ys;
                if (T.bbox[2] + dx <= 0 || T.bbox[0] + dx >= X || T.bbox[3] + dy <= 0 || T.bbox[1] + dy >= Y) continue;
                copies++;
                plutovg_matrix_t cv;
                plutovg_matrix_init(&cv, kx, 0, 0, ky, dx * kx, dy * ky);
                double vb[4] = {-dx, -dy, X - dx, Y - dy};
                drawItems(P, *T.cell, tcv, cv, vb, std::min(kx, ky), cancel, rc, &tc);
            }
        plutovg_canvas_destroy(tcv);
        if (T.uncoloured) {   // (premultiplied: the colour by each pixel's alpha)
            uint8_t *px = plutovg_surface_get_data(s);
            int stride = plutovg_surface_get_stride(s);
            uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
            for (int y = 0; y < ih; y++) {
                uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
                for (int x = 0; x < iw; x++) {
                    uint32_t a = row[x] >> 24;
                    row[x] = a << 24 | (r * a + 127) / 255 << 16 | (g * a + 127) / 255 << 8 | (b * a + 127) / 255;
                }
            }
        }
        size_t total = (size_t)iw * ih;
        for (auto &x : tc.e) total += (size_t)x.w * x.h;
        while (!tc.e.empty() && (tc.e.size() >= 16 || total > 4 * (size_t)MAX_TILE_PIXELS)) {
            total -= (size_t)tc.e[0].w * tc.e[0].h;
            plutovg_surface_destroy(tc.e[0].s);
            tc.e.erase(tc.e.begin());
        }
        tc.e.push_back({&T, iw, ih, nx, ny, rgb, pd.a, pd.b, pd.c, pd.d, s});
    }
    plutovg_matrix_t sc, tm;   // (the tile's pixels -> the pattern's space -> the path's)
    plutovg_matrix_init_scale(&sc, 1 / kx, 1 / ky);
    plutovg_matrix_multiply(&tm, &sc, &it.shadeM);
    plutovg_canvas_set_texture(c, s, PLUTOVG_TEXTURE_TYPE_TILED, it.alpha, &tm);
    plutovg_canvas_set_fill_rule(c, it.evenOdd ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
    plutovg_canvas_fill_path(c, it.path->p);
}

// --- transparency: blend modes (11.3.5), soft masks (11.6.5), groups (11.4.7) --------------------

// a separable blend mode's B(cb, cs), colours 0-1
inline float blendSep(int bm, float cb, float cs) {
    switch (bm) {
    case 1: return cb * cs;                                                   // Multiply
    case 2: return cb + cs - cb * cs;                                         // Screen
    case 3: return cb <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb);    // Overlay: HardLight(cs, cb)
    case 4: return std::min(cb, cs);                                          // Darken
    case 5: return std::max(cb, cs);                                          // Lighten
    case 6: return cb <= 0 ? 0 : cs >= 1 ? 1 : std::min(1.f, cb / (1 - cs)); // ColorDodge
    case 7: return cb >= 1 ? 1 : cs <= 0 ? 0 : 1 - std::min(1.f, (1 - cb) / cs);   // ColorBurn
    case 8: return cs <= 0.5f ? 2 * cs * cb : 1 - 2 * (1 - cs) * (1 - cb);    // HardLight
    case 9: {                                                                 // SoftLight
        if (cs <= 0.5f) return cb - (1 - 2 * cs) * cb * (1 - cb);
        float d = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : sqrtf(cb);
        return cb + (2 * cs - 1) * (d - cb);
    }
    case 10: return fabsf(cb - cs);            // Difference
    case 11: return cb + cs - 2 * cb * cs;     // Exclusion
    default: return cs;
    }
}
inline float lumOf(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
inline void setLum(float c[3], float l) {
    float d = l - lumOf(c);
    for (int k = 0; k < 3; k++) c[k] += d;
    l = lumOf(c);   // (ClipColor)
    float n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
    for (int k = 0; k < 3; k++) {
        if (n < 0 && l - n > 1e-6f) c[k] = l + (c[k] - l) * l / (l - n);
        if (x > 1 && x - l > 1e-6f) c[k] = l + (c[k] - l) * (1 - l) / (x - l);
    }
}
inline void setSat(float c[3], float s) {
    int mx = 0, mn = 0;
    for (int k = 1; k < 3; k++) mx = c[k] > c[mx] ? k : mx, mn = c[k] < c[mn] ? k : mn;
    if (mx == mn) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    int md = 3 - mx - mn;
    c[md] = (c[md] - c[mn]) * s / (c[mx] - c[mn]);
    c[mx] = s, c[mn] = 0;
}
// a non-separable one's (Hue, Saturation, Color, Luminosity)
inline void blendNonSep(int bm, const float cb[3], const float cs[3], float out[3]) {
    auto sat = [](const float c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); };
    const float *from = bm == 12 || bm == 14 ? cs : cb;
    for (int k = 0; k < 3; k++) out[k] = from[k];
    if (bm == 12) setSat(out, sat(cb));
    if (bm == 13) setSat(out, sat(cs));
    setLum(out, bm == 15 ? lumOf(cs) : lumOf(cb));
}

// The layer src composed onto dst at (ox, oy): its pixels by alpha and the mask, in blend mode bm
void compose(plutovg_surface_t *dst, plutovg_surface_t *src, int ox, int oy, const TileCache::Mask *mk, float alpha, int bm) {
    int dw = plutovg_surface_get_width(dst), dh = plutovg_surface_get_height(dst), ds = plutovg_surface_get_stride(dst);
    int sw = plutovg_surface_get_width(src), sh = plutovg_surface_get_height(src), ss = plutovg_surface_get_stride(src);
    uint8_t *dp = plutovg_surface_get_data(dst);
    const uint8_t *sp = plutovg_surface_get_data(src);
    for (int y = 0; y < sh; y++) {
        int dy = oy + y;
        if (dy < 0 || dy >= dh) continue;
        const uint32_t *srow = (const uint32_t *)(sp + (size_t)y * ss);
        uint32_t *drow = (uint32_t *)(dp + (size_t)dy * ds);
        for (int x = 0; x < sw; x++) {
            int dx = ox + x;
            uint32_t s = srow[x];
            if (dx < 0 || dx >= dw || !(s >> 24)) continue;
            float f = alpha * (mk ? mk->at(dx, dy) / 255.f : 1.f) / 255.f;
            if (f <= 0) continue;
            float sa = (s >> 24) * f, S[3] = {(s >> 16 & 255) * f, (s >> 8 & 255) * f, (s & 255) * f};
            uint32_t d = drow[dx];
            float da = (d >> 24) / 255.f, D[3] = {(d >> 16 & 255) / 255.f, (d >> 8 & 255) / 255.f, (d & 255) / 255.f}, o[3];
            if (!bm || da <= 0) {
                for (int k = 0; k < 3; k++) o[k] = S[k] + D[k] * (1 - sa);
            } else {   // (premultiplied: (1 - ab) Cs + (1 - as) Cb + as ab B(cb, cs))
                float cs[3], cb[3], B[3];
                for (int k = 0; k < 3; k++) cs[k] = std::min(1.f, S[k] / sa), cb[k] = std::min(1.f, D[k] / da);
                if (bm < 12)
                    for (int k = 0; k < 3; k++) B[k] = blendSep(bm, cb[k], cs[k]);
                else blendNonSep(bm, cb, cs, B);
                for (int k = 0; k < 3; k++) o[k] = (1 - da) * S[k] + (1 - sa) * D[k] + sa * da * clampf(B[k], 0, 1);
            }
            float a = sa + da - sa * da;
            auto c8 = [&](float v) { return (uint32_t)lrintf(clampf(v, 0, a) * 255); };
            drow[dx] = c8(a) << 24 | c8(o[0]) << 16 | c8(o[1]) << 8 | c8(o[2]);
        }
    }
}

// a box of the page's display coordinates on the canvas: its pixels, margin around, within the canvas
void pixelRect(const nd::Box &b, const plutovg_matrix_t &view, int W, int H, int margin, int &x0, int &y0, int &x1, int &y1) {
    float a0 = 1e30f, b0 = 1e30f, a1 = -1e30f, b1 = -1e30f;
    for (int k = 0; k < 4; k++) {
        plutovg_point_t p = {k & 1 ? b.x1 : b.x0, k & 2 ? b.y1 : b.y0}, q;
        plutovg_matrix_map_point(&view, &p, &q);
        a0 = std::min(a0, q.x), a1 = std::max(a1, q.x), b0 = std::min(b0, q.y), b1 = std::max(b1, q.y);
    }
    x0 = (int)std::max(0.f, std::min((float)W, floorf(a0) - margin)), y0 = (int)std::max(0.f, std::min((float)H, floorf(b0) - margin));
    x1 = (int)std::max(0.f, std::min((float)W, ceilf(a1) + margin)), y1 = (int)std::max(0.f, std::min((float)H, ceilf(b1) + margin));
    if (!(b.x1 >= b.x0 && b.y1 >= b.y0)) x1 = x0, y1 = y0;
}

// A soft mask over a canvas of W x H under view: drawn once a render (TileCache keeps the last few)
const TileCache::Mask *maskFor(nitropdf &P, const SoftMask &sm, const std::vector<Clip> &clips, int W, int H, const plutovg_matrix_t &view,
                               const double vb[4], double scale, const nd::Cancel &cancel, int &rc, TileCache &tc) {
    float key[6] = {view.a, view.b, view.c, view.d, view.e, view.f};
    for (auto &m : tc.masks)
        if (m->m == &sm && m->cw == W && m->ch == H && !memcmp(m->view, key, sizeof key)) return m.get();
    auto mk = std::make_unique<TileCache::Mask>();
    mk->m = &sm, mk->cw = W, mk->ch = H;
    memcpy(mk->view, key, sizeof key);
    uint32_t bc = sm.bc;
    mk->outside = sm.luminosity ? sm.tr[((bc >> 16 & 255) * 77 + (bc >> 8 & 255) * 151 + (bc & 255) * 28 + 128) >> 8] : sm.tr[0];
    int x0, y0, x1, y1;
    pixelRect(sm.box, view, W, H, 2, x0, y0, x1, y1);
    mk->x0 = x0, mk->y0 = y0, mk->w = x1 - x0, mk->h = y1 - y0;
    plutovg_surface_t *s = mk->w > 0 && mk->h > 0 ? plutovg_surface_create(mk->w, mk->h) : nullptr;
    if (s) {
        uint8_t *px = plutovg_surface_get_data(s);
        int stride = plutovg_surface_get_stride(s);
        if (sm.luminosity)   // (over the backdrop colour, opaque)
            for (int y = 0; y < mk->h; y++)
                for (int x = 0; x < mk->w; x++) ((uint32_t *)(px + (size_t)y * stride))[x] = 0xff000000u | bc;
        plutovg_canvas_t *mc = plutovg_canvas_create(s);
        plutovg_matrix_t lv = view;
        lv.e -= (float)x0, lv.f -= (float)y0;
        if (mc) drawItems(P, sm.items.data(), sm.items.data() + sm.items.size(), clips, mc, lv, vb, scale, cancel, rc, &tc);
        plutovg_canvas_destroy(mc);
        mk->v.resize((size_t)mk->w * mk->h);
        for (int y = 0; y < mk->h; y++) {
            const uint32_t *row = (const uint32_t *)(px + (size_t)y * stride);
            for (int x = 0; x < mk->w; x++) {
                uint32_t p = row[x];
                mk->v[(size_t)y * mk->w + x] =
                    sm.tr[sm.luminosity ? ((p >> 16 & 255) * 77 + (p >> 8 & 255) * 151 + (p & 255) * 28 + 128) >> 8 : p >> 24];
            }
        }
        plutovg_surface_destroy(s);
    } else mk->w = mk->h = 0;
    if (tc.masks.size() >= 4) tc.masks.erase(tc.masks.begin());
    tc.masks.push_back(std::move(mk));
    return tc.masks.back().get();
}

// An item with a blend mode or soft mask, or a group: drawn into a layer over its part of the canvas
// (and the mask's, where nothing is beyond it), the layer composed onto the canvas
void composeItem(nitropdf &P, plutovg_canvas_t *c, const Item &it, const std::vector<Clip> &clips, const plutovg_matrix_t &view,
                 const double vb[4], double scale, const nd::Cancel &cancel, int &rc, TileCache &tc) {
    plutovg_surface_t *cs = plutovg_canvas_get_surface(c);
    int W = plutovg_surface_get_width(cs), H = plutovg_surface_get_height(cs);
    int x0, y0, x1, y1;
    pixelRect(it.box, view, W, H, 2, x0, y0, x1, y1);
    if (it.smask) {
        const TileCache::Mask *mk = maskFor(P, *it.smask, clips, W, H, view, vb, scale, cancel, rc, tc);
        if (rc) return;
        if (!mk->outside) {
            x0 = std::max(x0, mk->x0), y0 = std::max(y0, mk->y0);
            x1 = std::min(x1, mk->x0 + mk->w), y1 = std::min(y1, mk->y0 + mk->h);
        }
    }
    if (x1 <= x0 || y1 <= y0) return;
    plutovg_surface_t *layer = plutovg_surface_create(x1 - x0, y1 - y0);
    if (!layer) return;
    plutovg_canvas_t *lc = plutovg_canvas_create(layer);
    plutovg_matrix_t lv = view;
    lv.e -= (float)x0, lv.f -= (float)y0;
    if (lc) {
        if (it.kind == Item::GROUP)
            drawItems(P, it.group->items.data(), it.group->items.data() + it.group->items.size(), clips, lc, lv, vb, scale, cancel, rc, &tc);
        else drawItems(P, &it, &it + 1, clips, lc, lv, vb, scale, cancel, rc, &tc, true);
        plutovg_canvas_destroy(lc);
    }
    if (!rc) {   // (the mask asked for again: drawing the layer may have let it go)
        const TileCache::Mask *mk = it.smask ? maskFor(P, *it.smask, clips, W, H, view, vb, scale, cancel, rc, tc) : nullptr;
        if (!rc) compose(cs, layer, x0, y0, mk, it.kind == Item::GROUP ? it.alpha : 1, it.blend);
    }
    plutovg_surface_destroy(layer);
}

void drawItems(nitropdf &P, const Item *first, const Item *last, const std::vector<Clip> &clips, plutovg_canvas_t *c,
               const plutovg_matrix_t &view, const double vb[4], double scale, const nd::Cancel &cancel, int &rc, TileCache *tc,
               bool plain) {
    TileCache own;
    if (!tc) tc = &own;
    plutovg_canvas_save(c);
    int curClip = -1;
    size_t k = 0;
    for (const Item *ip = first; ip != last; ip++) {
        const Item &it = *ip;
        if ((++k & 255) == 0 && cancel()) {
            rc = 1;
            break;
        }
        if (!nd::touches(vb, it.box.x0, it.box.y0, it.box.x1, it.box.y1, 2 / scale)) continue;
        if (it.kind == Item::GROUP || (!plain && (it.blend || it.smask))) {   // (in a layer of its own, composed)
            composeItem(P, c, it, clips, view, vb, scale, cancel, rc, *tc);
            if (rc) break;
            continue;
        }
        // (a stroke within a pixel of a rectangle clip left out: drawn at least a pixel wide, it may reach out)
        int clip = it.kind == Item::STROKE && it.clipOwn != it.clip && it.clipGap * scale < 1 ? it.clipOwn : it.clip;
        if (clip != curClip) {   // (the clip chain, from its outermost)
            plutovg_canvas_restore(c);
            plutovg_canvas_save(c);
            std::vector<int> chain;
            for (int x = clip; x >= 0 && chain.size() < 1024; x = clips[x].parent) chain.push_back(x);
            for (size_t i = chain.size(); i-- > 0;) {
                const Clip &cl = clips[chain[i]];
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
        if (it.kind == Item::FILL && it.shade && it.shade->mesh) {
            meshFill(c, it, m, view);
        } else if (it.kind == Item::FILL && it.tile) {
            tileFill(P, c, it, m, *tc, cancel, rc);
            if (rc) break;
        } else if (it.kind == Item::FILL) {
            if (it.shade) {
                const Shade &s = *it.shade;
                std::vector<plutovg_gradient_stop_t> stops = s.stops;
                for (auto &st : stops) st.color.a = it.alpha;
                if (s.type == 2)
                    plutovg_canvas_set_linear_gradient(c, s.c[0], s.c[1], s.c[2], s.c[3], PLUTOVG_SPREAD_METHOD_PAD, stops.data(),
                                                       (int)stops.size(), &it.shadeM);
                else   // (SVG's terms: the start circle its focal one)
                    plutovg_canvas_set_radial_gradient(c, s.c[3], s.c[4], s.c[5], s.c[0], s.c[1], s.c[2], PLUTOVG_SPREAD_METHOD_PAD,
                                                       stops.data(), (int)stops.size(), &it.shadeM);
            } else nd::setRgb(c, it.rgb, it.alpha);
            plutovg_canvas_set_fill_rule(c, it.evenOdd ? PLUTOVG_FILL_RULE_EVEN_ODD : PLUTOVG_FILL_RULE_NON_ZERO);
            plutovg_canvas_fill_path(c, it.path->p);
        } else if (it.kind == Item::STROKE) {
            float k2 = sqrtf(fabsf(m.a * m.d - m.b * m.c));   // (device pixels per unit)
            float w = k2 > 0 ? std::max(it.width, 1 / k2) : it.width;
            // Dashes with a period under half a device pixel (they cannot be told apart), or more than
            // MAX_DASHES of them, are not made one by one: a solid line as light as they cover it. (A
            // pattern of a millionth of a point on one line across the page: plutovg made every dash,
            // tens of gigabytes of them.)
            float alpha = it.alpha;
            bool dashed = false;
            if (it.dash) {
                size_t nd = it.dash->size();
                float sum = 0, on = 0;
                for (size_t i = 0; i < nd; i++) sum += (*it.dash)[i], on += i & 1 ? 0 : (*it.dash)[i];
                // (an odd pattern runs twice, its dashes and gaps changing places: half of it is drawn)
                float period = nd & 1 ? 2 * sum : sum, perPeriod = nd & 1 ? (float)nd : nd / 2.f;
                if (nd & 1) on = sum;
                if (it.cap != PLUTOVG_LINE_CAP_BUTT) on += w * perPeriod;   // (each dash longer by its caps)
                dashed = period * k2 >= 0.5f && it.length / period * perPeriod <= MAX_DASHES;
                if (!dashed) alpha *= clampf(on / period, 0, 1);
            }
            nd::setRgb(c, it.rgb, alpha);
            // (2 pixels wide at most: round caps and joins drawn square and bevelled, half a pixel apart
            // at most; the stroker's arcs took most of the time of a CAD plot's round-capped hairlines)
            bool thin = w * k2 <= 2;
            plutovg_canvas_set_line_width(c, w);
            plutovg_canvas_set_line_cap(c, thin && it.cap == PLUTOVG_LINE_CAP_ROUND ? PLUTOVG_LINE_CAP_SQUARE : (plutovg_line_cap_t)it.cap);
            plutovg_canvas_set_line_join(c, thin && it.join == PLUTOVG_LINE_JOIN_ROUND ? PLUTOVG_LINE_JOIN_BEVEL : (plutovg_line_join_t)it.join);
            plutovg_canvas_set_miter_limit(c, it.miter);
            if (dashed) plutovg_canvas_set_dash(c, it.dashOffset, it.dash->data(), (int)it.dash->size());
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

namespace {

// The PDFs embedded in the document (the /EmbeddedFiles name tree, 7.11.4): each file whose data
// starts as a PDF, opened as a document of its own (depth: how deep this one is embedded)
nitropdf *loadDepth(const void *data, size_t len, int depth);
void embeddedPdfs(nitropdf &P, int depth) {
    pp::Doc &doc = P.doc;
    Val tree = doc.get(doc.get(doc.root(), "Names"), "EmbeddedFiles");
    std::vector<Val> stack{tree};
    size_t nodes = 0;
    while (!stack.empty() && nodes++ < 10000 && P.parts.size() < 256) {
        Val node = doc.resolve(stack.back());
        stack.pop_back();
        if (node.t != pp::DICT) continue;
        Val kids = doc.get(node, "Kids");
        for (size_t k = kids.size(); k-- > 0;) stack.push_back(kids.at(k));   // (in order: the first kid on top)
        Val names = doc.get(node, "Names");
        for (size_t k = 1; k < names.size(); k += 2) {
            Val ef = doc.get(doc.resolve(names.at(k)), "EF");   // (the file specification's embedded file stream)
            Val ref = ef.get("F").t == pp::REF ? ef.get("F") : ef.get("UF");
            const pp::Stream *st = ref.t == pp::REF ? doc.stream(ref.oid()) : nullptr;
            if (!st) continue;
            pp::Decoded dec;
            doc.decode(*st, dec, 512u << 20);
            const std::string &d = dec.data;
            if (d.size() < 8 || d.substr(0, 1024).find("%PDF-") == std::string::npos) continue;
            if (nitropdf *part = loadDepth(d.data(), d.size(), depth + 1)) P.parts.emplace_back(part);
        }
    }
}

nitropdf *loadDepth(const void *data, size_t len, int depth) {
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
        p->dl.push_back(std::make_shared<PageDL>());
    }
    if (depth < 2) embeddedPdfs(*p, depth);   // (a portfolio in a portfolio, no deeper)
    // a portfolio (/Collection): its documents, not its cover (a page for the viewers that cannot
    // show them, "open this in Acrobat"); a document with attachments: its pages, then theirs
    bool portfolio = p->doc.get(p->doc.root(), "Collection").t != pp::NUL && !p->parts.empty();
    if (!portfolio)
        for (int k = 0; k < (int)p->pages.size(); k++) p->shown.emplace_back(p, k);
    for (auto &part : p->parts) p->shown.insert(p->shown.end(), part->shown.begin(), part->shown.end());
    if (p->shown.empty()) {
        delete p;
        return nullptr;
    }
    readOptionalContent(*p);
    return p;
}

}  // namespace

uint32_t nitropdf_glyph_unicode(const char *name) { return glyphUnicode(name); }

namespace {
// An .ai saved as a PDF keeps Illustrator's own data too (/PieceInfo /Illustrator /Private, in blocks).
// CorelDRAW writes the format of Illustrator 1-8 there (PostScript) and leaves the page empty: that,
// for such a page (Illustrator's own data, compressed, is not read).
std::string illustratorPrivate(nitropdf &P) {
    if (P.pages.size() != 1 || !P.parts.empty()) return std::string();
    pp::Doc &doc = P.doc;
    Val pg = P.pages[0].dict;
    Val priv = doc.get(doc.get(doc.get(pg, "PieceInfo"), "Illustrator"), "Private");
    if (priv.t != pp::DICT) return std::string();
    Val c = pg.get("Contents");
    std::vector<Val> parts;
    if (doc.resolve(c).t == pp::ARRAY)
        for (size_t k = 0; k < doc.resolve(c).size(); k++) parts.push_back(doc.resolve(c).at(k));
    else
        parts.push_back(c);
    size_t content = 0;
    for (Val v : parts)
        if (const pp::Stream *st = v.t == pp::REF ? doc.stream(v.oid()) : nullptr) {
            pp::Decoded dec;
            doc.decode(*st, dec, 4096);
            content += dec.data.size();
        }
    if (content > 256) return std::string();   // (a page drawn)
    // (the blocks together: no more than a drawing's text may be; and they are read on only while
    // the PostScript's first line is, or may yet come, in their first megabyte)
    const size_t LIMIT = 256u << 20, HEAD = 1u << 20;
    std::string all;
    size_t at = std::string::npos;
    int n = std::min(toInt(doc.get(priv, "NumBlock").num(0)), 1000);
    for (int k = 1; k <= n; k++) {
        Val r = priv.get(("AIPrivateData" + std::to_string(k)).c_str());
        const pp::Stream *st = r.t == pp::REF ? doc.stream(r.oid()) : nullptr;
        if (!st) return std::string();
        pp::Decoded dec;
        doc.decode(*st, dec, LIMIT - all.size());
        all += dec.data;
        if (at == std::string::npos) at = all.substr(0, HEAD + 16).find("%!PS-Adobe");   // (CorelDRAW's first block: a thumbnail)
        if (all.size() >= LIMIT || (at == std::string::npos && all.size() > HEAD + 16)) return std::string();
    }
    return at == std::string::npos || at > HEAD ? std::string() : all.substr(at);
}
}  // namespace

nitropdf *nitropdf_load(const void *data, size_t len) {
    if (data && nitroai_is(data, len)) {   // an Illustrator document before version 9: as the PDF it makes
        std::string pdf = nitroai_pdf(data, len);
        return pdf.empty() ? nullptr : loadDepth(pdf.data(), pdf.size(), 0);
    }
    nitropdf *p = loadDepth(data, len, 0);
    if (p) {
        std::string ai = illustratorPrivate(*p);
        std::string pdf = ai.empty() ? std::string() : nitroai_pdf(ai.data(), ai.size());
        if (nitropdf *q = pdf.empty() ? nullptr : loadDepth(pdf.data(), pdf.size(), 0)) {
            delete p;
            return q;
        }
    }
    return p;
}

void nitropdf_free(nitropdf *pdf) { delete pdf; }

int nitropdf_pages(const nitropdf *pdf) { return pdf ? (int)pdf->shown.size() : 0; }

void nitropdf_set_background(nitropdf *pdf, int background) {
    if (!pdf) return;
    pdf->background = background;
    for (auto &part : pdf->parts) nitropdf_set_background(part.get(), background);
}

void nitropdf_size(const nitropdf *pdf, int page, double *w, double *h) {
    *w = *h = 0;
    if (!pdf || page < 0 || page >= (int)pdf->shown.size()) return;
    const auto &sp = pdf->shown[page];   // (its document's own page)
    pageGeometry(sp.first->pages[sp.second], w, h, nullptr);
}

// About the memory a page's list holds: its pictures (each once: a page may draw one many times;
// with their halved copies, a third more) and its items
size_t listBytes(const PageDL &pg, std::unordered_set<const void *> &seen);
size_t listBytes(const std::vector<Item> &items, std::unordered_set<const void *> &seen) {
    size_t n = items.size() * sizeof(Item);
    for (const Item &it : items) {
        if (it.img && it.img->s && seen.insert(it.img.get()).second)
            n += (size_t)plutovg_surface_get_stride(it.img->s) * plutovg_surface_get_height(it.img->s) * 4 / 3;
        if (it.tile && seen.insert(it.tile.get()).second) n += listBytes(*it.tile->cell, seen);   // (a pattern's cell)
        if (it.group && seen.insert(it.group.get()).second) n += listBytes(it.group->items, seen);
        if (it.smask && seen.insert(it.smask.get()).second) n += listBytes(it.smask->items, seen);
    }
    return n;
}
size_t listBytes(const PageDL &pg, std::unordered_set<const void *> &seen) {
    return pg.clips.size() * sizeof(Clip) + listBytes(pg.items, seen);
}
size_t listBytes(const PageDL &pg) {
    std::unordered_set<const void *> seen;
    return listBytes(pg, seen);
}

// The lists beyond MAX_LIST_BYTES let go of, the least recently drawn first; never keep's (the page
// just built) nor the last MIN_LISTS drawn. (A picture several pages share stays while one holds it.)
void trimLists(nitropdf &P, int keep) {
    std::lock_guard<std::mutex> g(P.dlLock);
    size_t total = 0, built = 0;
    for (auto &l : P.dl)
        if (l->built.load(std::memory_order_relaxed)) total += l->bytes, built++;
    while (total > MAX_LIST_BYTES && built > MIN_LISTS) {
        int oldest = -1;
        for (int k = 0; k < (int)P.dl.size(); k++)
            if (k != keep && P.dl[k]->built.load(std::memory_order_relaxed) && (oldest < 0 || P.dl[k]->lastUse < P.dl[oldest]->lastUse))
                oldest = k;
        if (oldest < 0) break;
        total -= P.dl[oldest]->bytes, built--;
        P.dl[oldest] = std::make_shared<PageDL>();   // (freed now, or when the render drawing it ends)
    }
}

// The display list of a page of pdf (its own page: not a shown one), built if it is not: the page's content
// read, once, under the lock. nullptr: cancelled meanwhile (not built: the next call reads it again).
static std::shared_ptr<PageDL> pageList(nitropdf *pdf, int page, nd::Cancel &cancel) {
    std::shared_ptr<PageDL> list;
    {
        std::lock_guard<std::mutex> g(pdf->dlLock);
        list = pdf->dl[page];
        list->lastUse = ++pdf->useClock;
    }
    PageDL &pg = *list;
    if (!pg.built.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> g(pdf->lock);
        if (!pg.built.load(std::memory_order_relaxed)) {
            if (!buildPage(*pdf, page, pg, cancel)) return nullptr;
            pg.bytes = listBytes(pg);
            pg.built.store(true, std::memory_order_release);
            trimLists(*pdf, page);
        }
    }
    return list;
}

int nitropdf_prepare(nitropdf *pdf, int page) {
    if (!pdf || page < 0 || page >= (int)pdf->shown.size()) return -1;
    const std::pair<nitropdf *, int> sp = pdf->shown[page];
    nd::Cancel cancel{nullptr, nullptr};
    return pageList(sp.first, sp.second, cancel) ? 0 : 1;
}

int nitropdf_render(nitropdf *pdf, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                    int (*cancelled)(void *ctx), void *ctx) {
    if (!pdf || page < 0 || page >= (int)pdf->shown.size() || !nd::renderArgs(w, h, pitch, scale)) return -1;
    const std::pair<nitropdf *, int> sp = pdf->shown[page];   // (its document's own page)
    pdf = sp.first, page = sp.second;
    nd::Cancel cancel{cancelled, ctx};
    std::shared_ptr<PageDL> list = pageList(pdf, page, cancel);
    if (!list) return 1;
    PageDL &pg = *list;
    // The page is an isolated group (11.4.7): what is drawn in a blend mode blends with what is drawn
    // before it, not with the paper (a light colour in Lighten over nothing is that colour, over white
    // white). Drawn on nothing then, the page composed onto its background at the end.
    uint32_t bg = pdf->background == 3 ? nd::background(3) : nd::background(0);
    bool onNothing = pg.blends && bg;
    nd::Target t(bgra, w, h, pitch, onNothing ? 0 : bg);
    if (!t.canvas) return -1;
    plutovg_matrix_t view;
    plutovg_matrix_init(&view, (float)scale, 0, 0, (float)scale, (float)(-x0 * scale), (float)(-y0 * scale));
    double vb[4] = {x0, y0, x0 + w / scale, y0 + h / scale};
    int rc = 0;
    drawItems(*pdf, pg, t.canvas, view, vb, scale, cancel, rc);
    if (onNothing)   // (premultiplied: the background where the page is not opaque; it is opaque itself)
        for (int y = 0; y < h; y++) {
            uint32_t *row = (uint32_t *)(bgra + (size_t)y * pitch);
            for (int x = 0; x < w; x++) {
                uint32_t p = row[x], na = 255 - (p >> 24), o = 0xff000000u;
                for (int sh = 0; sh < 24; sh += 8) o |= std::min(255u, (p >> sh & 255) + ((bg >> sh & 255) * na + 127) / 255) << sh;
                row[x] = o;
            }
        }
    return rc;
}
