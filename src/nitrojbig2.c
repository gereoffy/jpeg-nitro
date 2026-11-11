// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrojbig2: see nitrojbig2.h. Written from the open standard alone, ITU-T T.88 (02/2000), with no
// other decoder's code: the segment header (7.2), the page information (7.4.8), the region segment
// information (7.4.1), the generic region (6.2, 7.4.6) and the arithmetic decoder (annex E: its
// table E.1 and flow charts).
#include "nitrojbig2.h"
#include "nitrotiff.h"   // nt_ccitt: MMR is CCITT G4
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// --- the MQ arithmetic decoder (annex E) ---------------------------------------------------------

// Table E.1: Qe, the next index after an MPS and after an LPS, the MPS switch
static const struct { uint16_t qe; uint8_t nmps, nlps, sw; } QE[47] = {
    {0x5601, 1, 1, 1},   {0x3401, 2, 6, 0},   {0x1801, 3, 9, 0},   {0x0AC1, 4, 12, 0},  {0x0521, 5, 29, 0},
    {0x0221, 38, 33, 0}, {0x5601, 7, 6, 1},   {0x5401, 8, 14, 0},  {0x4801, 9, 14, 0},  {0x3801, 10, 14, 0},
    {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0}, {0x1C01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1},
    {0x5401, 16, 14, 0}, {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0}, {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0},
    {0x3001, 21, 19, 0}, {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0}, {0x1C01, 25, 22, 0},
    {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0}, {0x1401, 28, 25, 0}, {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
    {0x0AC1, 31, 28, 0}, {0x09C1, 32, 29, 0}, {0x08A1, 33, 30, 0}, {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0},
    {0x02A1, 36, 33, 0}, {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0}, {0x0085, 40, 37, 0},
    {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0}, {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0},
    {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0},
};

typedef struct {
    const uint8_t *p, *end;   // B, the byte at p (0xFF past the end)
    uint32_t c, a;
    int ct;
} Mq;

static inline unsigned mq_at(const Mq *m, const uint8_t *p) { return p < m->end ? *p : 0xff; }

static void mq_bytein(Mq *m) {   // (figure E.19)
    if (mq_at(m, m->p) == 0xff) {
        unsigned b1 = mq_at(m, m->p + 1);
        if (b1 > 0x8f)
            m->ct = 8;   // (a marker: 1 bits from here)
        else {
            m->p++;
            m->c += 0xfe00 - (b1 << 9);
            m->ct = 7;
        }
    } else {
        m->p++;
        m->c += 0xff00 - (mq_at(m, m->p) << 8);
        m->ct = 8;
    }
}

static void mq_init(Mq *m, const uint8_t *p, const uint8_t *end) {   // (figure E.20)
    m->p = p, m->end = end;
    m->c = (mq_at(m, p) ^ 0xff) << 16;
    mq_bytein(m);
    m->c <<= 7;
    m->ct -= 7;
    m->a = 0x8000;
}

// A decision in the context cx (its index << 1 | its MPS). (figures E.15 to E.18)
static inline int mq_decode(Mq *m, uint8_t *cx) {
    unsigned i = *cx >> 1, mps = *cx & 1, qe = QE[i].qe;
    int d;
    m->a -= qe;
    if ((m->c >> 16) < m->a) {
        if (m->a & 0x8000) return (int)mps;
        if (m->a < qe) {
            d = 1 - (int)mps;
            *cx = (uint8_t)(QE[i].nlps << 1 | (mps ^ QE[i].sw));
        } else {
            d = (int)mps;
            *cx = (uint8_t)(QE[i].nmps << 1 | mps);
        }
    } else {
        m->c -= m->a << 16;
        if (m->a < qe) {
            m->a = qe;
            d = (int)mps;
            *cx = (uint8_t)(QE[i].nmps << 1 | mps);
        } else {
            m->a = qe;
            d = 1 - (int)mps;
            *cx = (uint8_t)(QE[i].nlps << 1 | (mps ^ QE[i].sw));
        }
    }
    do {
        if (!m->ct) mq_bytein(m);
        m->a <<= 1;
        m->c <<= 1;
        m->ct--;
    } while (!(m->a & 0x8000));
    return d;
}

// --- the generic region (6.2) --------------------------------------------------------------------

// A bitmap a byte a pixel (1: black), with 4 blank columns either side and 2 blank rows above: the
// templates' fixed pixels read without checks.
typedef struct {
    uint8_t *mem;
    int w, h;
    size_t stride;
} Bm;
#define MX 4

static int bm_new(Bm *b, int w, int h) {
    b->w = w, b->h = h, b->stride = (size_t)w + 2 * MX;
    b->mem = calloc((size_t)(h + 2) * b->stride, 1);
    return b->mem ? 0 : -1;
}
static inline uint8_t *bm_row(const Bm *b, int y) { return b->mem + (size_t)(y + 2) * b->stride + MX; }

// The context of the pixel x of row r (above it: the rows r1, r2) in the template t (figures 3 to 6),
// with the adaptive pixels at.
static inline unsigned context(int t, const Bm *b, int x, int y, const uint8_t *r, const uint8_t *r1,
                               const uint8_t *r2, const int8_t *at) {
#define AT(k) (x + at[2 * (k)] >= 0 && x + at[2 * (k)] < b->w && y + at[2 * (k) + 1] >= 0 \
                   ? bm_row(b, y + at[2 * (k) + 1])[x + at[2 * (k)]] : 0u)
    switch (t) {
    case 0:
        return r[x - 1] | r[x - 2] << 1 | r[x - 3] << 2 | r[x - 4] << 3 | AT(0) << 4 | r1[x + 2] << 5 |
               r1[x + 1] << 6 | r1[x] << 7 | r1[x - 1] << 8 | r1[x - 2] << 9 | AT(1) << 10 | AT(2) << 11 |
               r2[x + 1] << 12 | r2[x] << 13 | r2[x - 1] << 14 | AT(3) << 15;
    case 1:
        return r[x - 1] | r[x - 2] << 1 | r[x - 3] << 2 | AT(0) << 3 | r1[x + 2] << 4 | r1[x + 1] << 5 |
               r1[x] << 6 | r1[x - 1] << 7 | r1[x - 2] << 8 | r2[x + 2] << 9 | r2[x + 1] << 10 | r2[x] << 11 |
               r2[x - 1] << 12;
    case 2:
        return r[x - 1] | r[x - 2] << 1 | AT(0) << 2 | r1[x + 1] << 3 | r1[x] << 4 | r1[x - 1] << 5 |
               r1[x - 2] << 6 | r2[x + 1] << 7 | r2[x] << 8 | r2[x - 1] << 9;
    default:
        return r[x - 1] | r[x - 2] << 1 | r[x - 3] << 2 | r[x - 4] << 3 | AT(0) << 4 | r1[x + 1] << 5 |
               r1[x] << 6 | r1[x - 1] << 7 | r1[x - 2] << 8 | r1[x - 3] << 9;
    }
#undef AT
}

// (one loop for each template: the context's form a constant in it)
static inline __attribute__((always_inline)) void generic_rows(int t, Mq *m, uint8_t *cx, Bm *b, int tpgdon,
                                                               const int8_t *at) {
    static const unsigned sltp[4] = {0x9b25, 0x0795, 0x00e5, 0x0195};   // (6.2.5.7: the LTP's context)
    int ltp = 0;
    for (int y = 0; y < b->h; y++) {
        uint8_t *r = bm_row(b, y), *r1 = bm_row(b, y - 1), *r2 = bm_row(b, y - 2);
        if (tpgdon) {
            ltp ^= mq_decode(m, &cx[sltp[t]]);
            if (ltp) {   // (the row the same as the one above)
                memcpy(r, r1, (size_t)b->w);
                continue;
            }
        }
        for (int x = 0; x < b->w; x++) r[x] = (uint8_t)mq_decode(m, &cx[context(t, b, x, y, r, r1, r2, at)]);
    }
}

static int generic_arith(const uint8_t *d, const uint8_t *end, Bm *b, int t, int tpgdon, const int8_t *at) {
    uint8_t *cx = calloc(1 << 16, 1);
    if (!cx) return -1;
    Mq m;
    mq_init(&m, d, end);
    switch (t) {
    case 0: generic_rows(0, &m, cx, b, tpgdon, at); break;
    case 1: generic_rows(1, &m, cx, b, tpgdon, at); break;
    case 2: generic_rows(2, &m, cx, b, tpgdon, at); break;
    default: generic_rows(3, &m, cx, b, tpgdon, at); break;
    }
    free(cx);
    return 0;
}

static int generic_mmr(const uint8_t *d, size_t len, Bm *b) {
    size_t prow = ((size_t)b->w + 7) / 8;
    uint8_t *bits = calloc(prow * (size_t)b->h, 1);
    if (!bits) return -1;
    nt_ccitt(d, len, -1, b->w, b->h, 1, 0, bits, prow);   // (a damaged row and the rest: white)
    for (int y = 0; y < b->h; y++) {
        uint8_t *r = bm_row(b, y);
        const uint8_t *s = bits + (size_t)y * prow;
        for (int x = 0; x < b->w; x++) r[x] = s[x >> 3] >> (7 - (x & 7)) & 1;
    }
    free(bits);
    return 0;
}

// --- the segments --------------------------------------------------------------------------------

typedef struct {
    Bm page;
    int haveInfo;
} Dec;

// The region r onto the page at x, y by the combination operator op (7.4.1.5: OR AND XOR XNOR REPLACE).
static void compose(Bm *page, const Bm *r, int x0, int y0, int op) {
    for (int y = 0; y < r->h; y++) {
        int py = y0 + y;
        if (py < 0 || py >= page->h) continue;
        const uint8_t *s = bm_row(r, y);
        uint8_t *p = bm_row(page, py);
        for (int x = 0; x < r->w; x++) {
            int px = x0 + x;
            if (px < 0 || px >= page->w) continue;
            uint8_t v = s[x];
            switch (op) {
            case 0: p[px] |= v; break;
            case 1: p[px] &= v; break;
            case 2: p[px] ^= v; break;
            case 3: p[px] = (uint8_t)!(p[px] ^ v); break;
            default: p[px] = v; break;
            }
        }
    }
}

// An immediate generic region segment's data: d .. d + n. (7.4.6; n unknown: SIZE_MAX, then its end
// is found (7.2.7) and *used tells the bytes it took.)
static int generic_region(Dec *dec, const uint8_t *d, size_t n, size_t *used) {
    if (n < 18) return -1;
    uint32_t w = be32(d), h = be32(d + 4), x = be32(d + 8), y = be32(d + 12);
    int op = d[16] & 7, fl = d[17];
    int mmr = fl & 1, t = fl >> 1 & 3, tpgdon = fl >> 3 & 1;
    if (fl & 0x10) return -1;   // (EXTTEMPLATE: T.88 amendment 2)
    size_t i = 18;
    int8_t at[8] = {0};
    if (!mmr) {
        int k = t == 0 ? 8 : 2;
        if (n < i + (size_t)k) return -1;
        memcpy(at, d + i, (size_t)k);
        i += (size_t)k;
    }
    size_t end = n;
    if (n == SIZE_MAX) {   // (the end marker, then the row count)
        end = *used;   // (the bytes there are, here)
        size_t j = i;
        for (; j + 6 <= end; j++)
            if (mmr ? d[j] == 0 && d[j + 1] == 0 : d[j] == 0xff && d[j + 1] == 0xac) break;
        if (j + 6 > end) return -1;
        h = be32(d + j + 2);
        *used = j + 6;
        end = j + 2;
    } else
        *used = n;
    if (!w || !h || w > 1u << 20 || h > 1u << 20 || (uint64_t)w * h > 1u << 28) return -1;
    Bm r;
    if (bm_new(&r, (int)w, (int)h)) return -1;
    int rc = mmr ? generic_mmr(d + i, end - i, &r) : generic_arith(d + i, d + end, &r, t, tpgdon, at);
    if (!rc) compose(&dec->page, &r, (int)x, (int)y, op);
    free(r.mem);
    return rc;
}

// The segments of d .. d + len. 1: the end of the page (or of the file), 0: all of them read, -1: one
// not decoded.
static int segments(Dec *dec, const uint8_t *d, size_t len, int width, int height) {
    size_t i = 0;
    while (i + 11 <= len) {
        uint32_t num = be32(d + i);
        int fl = d[i + 4], type = fl & 63;
        i += 5;
        uint32_t nref = d[i] >> 5;
        if (nref == 7) {
            if (i + 4 > len) return -1;
            nref = be32(d + i) & 0x1fffffff;
            i += 4 + (nref + 8) / 8;
        } else
            i++;
        i += nref * (num <= 256 ? 1u : num <= 65536 ? 2u : 4u);
        i += fl & 64 ? 4 : 1;
        if (i + 4 > len) return -1;
        uint32_t n = be32(d + i);
        i += 4;
        if (n != 0xffffffff && n > len - i) return -1;
        switch (type) {
        case 48:   // page information
            if (n < 19 || dec->haveInfo) break;
            dec->haveInfo = 1;
            if (bm_new(&dec->page, width, height)) return -1;
            if (d[i + 16] & 4)   // (the default pixel black)
                for (int y = 0; y < height; y++) memset(bm_row(&dec->page, y), 1, (size_t)width);
            break;
        case 38:
        case 39: {   // immediate generic region
            if (!dec->haveInfo) return -1;
            size_t used = len - i;
            if (generic_region(dec, d + i, n == 0xffffffff ? SIZE_MAX : n, &used)) return -1;
            n = (uint32_t)used;
            break;
        }
        case 49:   // end of page, of file
        case 51:
            return 1;
        case 0:    // symbol dictionary, pattern dictionary: only used by the regions below
        case 16:
        case 36:   // intermediate generic region (for a refinement)
        case 50:   // end of stripe
        case 52:   // profiles, tables, extension
        case 53:
        case 62:
            break;
        default:   // text, halftone, refinement regions ...
            return -1;
        }
        if (n == 0xffffffff) return -1;
        i += n;
    }
    return 0;
}

int nj2_decode(const uint8_t *data, size_t len, const uint8_t *globals, size_t glen, int width, int height,
               uint8_t *dst, size_t prow) {
    if (width <= 0 || height <= 0 || prow < ((size_t)width + 7) / 8) return -1;
    Dec dec;
    memset(&dec, 0, sizeof dec);
    int rc = globals && glen ? segments(&dec, globals, glen, width, height) : 0;
    if (rc >= 0) rc = segments(&dec, data, len, width, height);
    if (rc < 0 || !dec.haveInfo) {
        free(dec.page.mem);
        return -1;
    }
    for (int y = 0; y < height; y++) {
        const uint8_t *s = bm_row(&dec.page, y);
        uint8_t *o = dst + (size_t)y * prow;
        memset(o, 0, prow);
        for (int x = 0; x < width; x++)
            if (!s[x]) o[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
    free(dec.page.mem);
    return 0;
}
