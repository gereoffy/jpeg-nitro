// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrojbig2: see nitrojbig2.h. Written from the open standard alone, ITU-T T.88 (02/2000), with no
// other decoder's code: the segment header (7.2), the page information (7.4.8), the region segment
// information (7.4.1), the generic region (6.2, 7.4.6), the generic refinement (6.3), the text region
// (6.4, 7.4.3), the symbol dictionary (6.5, 7.4.2), the integer decoding (annex A) and the
// arithmetic decoder (annex E: its table E.1 and flow charts).
#include "nitrojbig2.h"
#include "nitrotiff.h"   // nt_ccitt: MMR is CCITT G4
#include <limits.h>
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

// The bitmap r onto page at x, y by the combination operator op (7.4.1.5: OR AND XOR XNOR REPLACE).
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

static void generic_decode(Mq *m, uint8_t *cx, Bm *b, int t, int tpgdon, const int8_t *at) {
    switch (t) {
    case 0: generic_rows(0, m, cx, b, tpgdon, at); break;
    case 1: generic_rows(1, m, cx, b, tpgdon, at); break;
    case 2: generic_rows(2, m, cx, b, tpgdon, at); break;
    default: generic_rows(3, m, cx, b, tpgdon, at); break;
    }
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

// --- the generic refinement region (6.3) ---------------------------------------------------------

static inline unsigned px(const Bm *b, int x, int y) {
    return x >= 0 && x < b->w && y >= 0 && y < b->h ? bm_row(b, y)[x] : 0u;
}

// b decoded as a refinement of ref, ref's pixel (x - dx, y - dy) over b's (x, y), in the template t
// (figures 12, 13) with its adaptive pixels at. (TPGRON: only for refinement region segments.)
static void refine(Mq *m, uint8_t *cx, Bm *b, const Bm *ref, int dx, int dy, int t, const int8_t *at) {
    for (int y = 0; y < b->h; y++) {
        uint8_t *r = bm_row(b, y), *r1 = bm_row(b, y - 1);
        for (int x = 0; x < b->w; x++) {
            int rx = x - dx, ry = y - dy;
            unsigned c;
            if (t == 0)
                c = r[x - 1] | r1[x + 1] << 1 | r1[x] << 2 | px(b, x + at[0], y + at[1]) << 3 |
                    px(ref, rx + 1, ry + 1) << 4 | px(ref, rx, ry + 1) << 5 | px(ref, rx - 1, ry + 1) << 6 |
                    px(ref, rx + 1, ry) << 7 | px(ref, rx, ry) << 8 | px(ref, rx - 1, ry) << 9 |
                    px(ref, rx + 1, ry - 1) << 10 | px(ref, rx, ry - 1) << 11 | px(ref, rx + at[2], ry + at[3]) << 12;
            else
                c = r[x - 1] | r1[x + 1] << 1 | r1[x] << 2 | r1[x - 1] << 3 | px(ref, rx + 1, ry + 1) << 4 |
                    px(ref, rx, ry + 1) << 5 | px(ref, rx + 1, ry) << 6 | px(ref, rx, ry) << 7 |
                    px(ref, rx - 1, ry) << 8 | px(ref, rx, ry - 1) << 9;
            r[x] = (uint8_t)mq_decode(m, &cx[c]);
        }
    }
}

// --- integers (annex A) --------------------------------------------------------------------------

#define OOB INT_MIN

typedef struct {   // the contexts of a symbol dictionary's or a text region's decoding
    uint8_t dh[512], dw[512], ex[512], ai[512], dt[512], fs[512], ds[512], it[512], ri[512];
    uint8_t rdw[512], rdh[512], rdx[512], rdy[512];
    uint8_t gb[1 << 16], gr[1 << 13];
    uint8_t *id;   // (IAID: 1 << idlen of them)
    int idlen;
} Ia;

static Ia *ia_new(int nsyms) {
    Ia *ia = calloc(1, sizeof *ia);
    if (!ia) return NULL;
    while (ia->idlen < 30 && (1 << ia->idlen) < nsyms) ia->idlen++;
    ia->id = calloc((size_t)2 << ia->idlen, 1);
    if (!ia->id) {
        free(ia);
        return NULL;
    }
    return ia;
}
static void ia_free(Ia *ia) {
    if (ia) free(ia->id);
    free(ia);
}

static inline int ia_bit(Mq *m, uint8_t *cx, unsigned *prev) {
    int d = mq_decode(m, &cx[*prev]);
    *prev = *prev < 256 ? *prev << 1 | (unsigned)d : ((*prev << 1 | (unsigned)d) & 511) | 256;
    return d;
}

// The integer arithmetic decoding procedure (A.2): a value, or OOB
static int ia_int(Mq *m, uint8_t *cx) {
    unsigned prev = 1;
    int s = ia_bit(m, cx, &prev), n;
    int64_t off;
    if (!ia_bit(m, cx, &prev)) n = 2, off = 0;
    else if (!ia_bit(m, cx, &prev)) n = 4, off = 4;
    else if (!ia_bit(m, cx, &prev)) n = 6, off = 20;
    else if (!ia_bit(m, cx, &prev)) n = 8, off = 84;
    else if (!ia_bit(m, cx, &prev)) n = 12, off = 340;
    else n = 32, off = 4436;
    int64_t v = 0;
    for (int k = 0; k < n; k++) v = v << 1 | ia_bit(m, cx, &prev);
    v += off;
    if (s && !v) return OOB;
    if (v > INT_MAX / 2) v = INT_MAX / 2;   // (damage: kept from overflowing the sums)
    return s ? -(int)v : (int)v;
}

// The symbol ID (A.3)
static unsigned ia_id(Mq *m, Ia *ia) {
    unsigned prev = 1;
    for (int k = 0; k < ia->idlen; k++) prev = prev << 1 | (unsigned)mq_decode(m, &ia->id[prev]);
    return prev - (1u << ia->idlen);
}

// --- the text region (6.4) -----------------------------------------------------------------------

typedef struct {
    int strips, defpix, op, transposed, corner, dsoffset, refine, rt;
    int8_t rat[4];
    uint32_t ninst;
} TextParams;
enum { BOTTOMLEFT, TOPLEFT, BOTTOMRIGHT, TOPRIGHT };

// The region b from the symbols syms[nsyms] (6.4.5, arithmetic coded)
static int text_region(Mq *m, Ia *ia, Bm *b, const Bm *const *syms, uint32_t nsyms, const TextParams *p) {
    if (p->defpix)
        for (int y = 0; y < b->h; y++) memset(bm_row(b, y), 1, (size_t)b->w);
    int v = ia_int(m, ia->dt);
    if (v == OOB) return -1;
    int64_t stript = -(int64_t)v * p->strips, firsts = 0;
    uint32_t ninst = 0;
    while (ninst < p->ninst) {
        if ((v = ia_int(m, ia->dt)) == OOB) return -1;
        stript += (int64_t)v * p->strips;
        int64_t curs = 0;
        for (int first = 1; ninst < p->ninst; first = 0) {
            if (first) {
                if ((v = ia_int(m, ia->fs)) == OOB) return -1;
                curs = firsts += v;
            } else {
                if ((v = ia_int(m, ia->ds)) == OOB) break;   // (the strip's end)
                curs += v + p->dsoffset;
            }
            int curt = 0;
            if (p->strips > 1 && (curt = ia_int(m, ia->it)) == OOB) return -1;
            int64_t t = stript + curt;
            unsigned id = ia_id(m, ia);
            if (id >= nsyms) return -1;
            const Bm *ib = syms[id];
            Bm rb = {0};
            if (p->refine && (v = ia_int(m, ia->ri)) != 0) {
                if (v == OOB) return -1;
                int rdw = ia_int(m, ia->rdw), rdh = ia_int(m, ia->rdh), rdx = ia_int(m, ia->rdx), rdy = ia_int(m, ia->rdy);
                if (rdw == OOB || rdh == OOB || rdx == OOB || rdy == OOB) return -1;
                int64_t w = (int64_t)ib->w + rdw, h = (int64_t)ib->h + rdh;
                if (w < 0 || h < 0 || w > 1 << 16 || h > 1 << 16 || bm_new(&rb, (int)w, (int)h)) return -1;
                refine(m, ia->gr, &rb, ib, (rdw >> 1) + rdx, (rdh >> 1) + rdy, p->rt, p->rat);
                ib = &rb;
            }
            int wi = ib->w, hi = ib->h;
            if (!p->transposed && (p->corner == TOPRIGHT || p->corner == BOTTOMRIGHT)) curs += wi - 1;
            if (p->transposed && (p->corner == BOTTOMLEFT || p->corner == BOTTOMRIGHT)) curs += hi - 1;
            int64_t x = p->transposed ? t : curs, y = p->transposed ? curs : t;
            if (p->corner == TOPRIGHT || p->corner == BOTTOMRIGHT) x -= wi - 1;
            if (p->corner == BOTTOMLEFT || p->corner == BOTTOMRIGHT) y -= hi - 1;
            if (x > -(1 << 24) && x < 1 << 24 && y > -(1 << 24) && y < 1 << 24) compose(b, ib, (int)x, (int)y, p->op);
            if (!p->transposed && (p->corner == TOPLEFT || p->corner == BOTTOMLEFT)) curs += wi - 1;
            if (p->transposed && (p->corner == TOPLEFT || p->corner == TOPRIGHT)) curs += hi - 1;
            free(rb.mem);
            ninst++;
        }
    }
    return 0;
}

// --- the segments --------------------------------------------------------------------------------

typedef struct {   // a symbol dictionary's exported symbols
    uint32_t num;
    Bm *syms;
    uint32_t n;
} Dict;

typedef struct {
    Bm page;
    int haveInfo;
    Dict *dicts;
    int ndicts;
} Dec;

// The symbols of the dictionaries among the referred segments refs[nrefs] (*n of them; NULL: none, or
// no memory: *n -1)
static const Bm **referred_symbols(const Dec *dec, const uint32_t *refs, uint32_t nrefs, uint32_t *n) {
    uint64_t total = 0;
    for (uint32_t r = 0; r < nrefs; r++)
        for (int k = 0; k < dec->ndicts; k++)
            if (dec->dicts[k].num == refs[r]) total += dec->dicts[k].n;
    *n = 0;
    if (total > 1u << 24) return NULL;
    const Bm **s = malloc((size_t)(total + 1) * sizeof *s);
    if (!s) return NULL;
    for (uint32_t r = 0; r < nrefs; r++)
        for (int k = 0; k < dec->ndicts; k++)
            if (dec->dicts[k].num == refs[r])
                for (uint32_t j = 0; j < dec->dicts[k].n; j++) s[(*n)++] = &dec->dicts[k].syms[j];
    return s;
}

// A symbol dictionary segment (7.4.2, 6.5; arithmetic coded only)
static int symbol_dict(Dec *dec, uint32_t num, const uint8_t *d, size_t n, const uint32_t *refs, uint32_t nrefs) {
    if (n < 2) return -1;
    int fl = d[0] << 8 | d[1];
    if (fl & 1 || fl & 0x100) return -1;   // (Huffman coded; the contexts of an earlier dictionary)
    int refagg = fl >> 1 & 1, t = fl >> 10 & 3, rt = fl >> 12 & 1;
    size_t i = 2;
    int8_t at[8] = {0}, rat[4] = {0};
    int k = t == 0 ? 8 : 2;
    if (n < i + (size_t)k) return -1;
    memcpy(at, d + i, (size_t)k);
    i += (size_t)k;
    if (refagg && !rt) {
        if (n < i + 4) return -1;
        memcpy(rat, d + i, 4);
        i += 4;
    }
    if (n < i + 8) return -1;
    uint32_t nex = be32(d + i), nnew = be32(d + i + 4);
    i += 8;
    uint32_t nin;
    const Bm **in = referred_symbols(dec, refs, nrefs, &nin);
    if (!in || nnew > 1u << 20 || nex > nin + nnew) {
        free(in);
        return -1;
    }
    uint32_t total = nin + nnew;
    const Bm **all = malloc((size_t)(total + 1) * sizeof *all);
    Bm *news = calloc((size_t)nnew + 1, sizeof *news);
    Ia *ia = ia_new((int)total);
    Dict dict = {num, calloc((size_t)nex + 1, sizeof(Bm)), 0};
    int rc = -1;
    if (!all || !news || !ia || !dict.syms) goto done;
    memcpy(all, in, (size_t)nin * sizeof *all);
    Mq m;
    mq_init(&m, d + i, d + n);
    uint32_t nd = 0;
    int64_t hcheight = 0;
    while (nd < nnew) {
        int v = ia_int(&m, ia->dh);
        if (v == OOB || (hcheight += v) < 0 || hcheight > 1 << 16) goto done;
        int64_t symwidth = 0;
        for (;;) {
            if ((v = ia_int(&m, ia->dw)) == OOB) break;
            if (nd >= nnew || (symwidth += v) < 0 || symwidth > 1 << 16) goto done;
            Bm *b = &news[nd];
            if (bm_new(b, (int)symwidth, (int)hcheight)) goto done;
            if (!refagg)
                generic_decode(&m, ia->gb, b, t, 0, at);
            else {
                int ninst = ia_int(&m, ia->ai);
                if (ninst == OOB || ninst < 1) goto done;
                if (ninst == 1) {
                    unsigned id = ia_id(&m, ia);
                    int rdx = ia_int(&m, ia->rdx), rdy = ia_int(&m, ia->rdy);
                    if (id >= nin + nd || rdx == OOB || rdy == OOB) goto done;
                    refine(&m, ia->gr, b, all[id], rdx, rdy, rt, rat);
                } else {   // (an aggregate: a text region of the symbols so far)
                    TextParams p = {1, 0, 0, 0, TOPLEFT, 0, 1, rt, {rat[0], rat[1], rat[2], rat[3]}, (uint32_t)ninst};
                    if (text_region(&m, ia, b, all, nin + nd, &p)) goto done;
                }
            }
            all[nin + nd++] = b;
        }
    }
    // the exported ones: runs of not exported and exported symbols in turn (6.5.10)
    for (uint32_t x = 0, on = 0; x < total; on ^= 1) {
        int run = ia_int(&m, ia->ex);
        if (run == OOB || run < 0 || (uint32_t)run > total - x) goto done;
        for (int r = 0; r < run; r++, x++)
            if (on) {
                if (dict.n >= nex) goto done;
                Bm *e = &dict.syms[dict.n];
                if (bm_new(e, all[x]->w, all[x]->h)) goto done;
                dict.n++;
                memcpy(e->mem, all[x]->mem, (size_t)(e->h + 2) * e->stride);
            }
    }
    Dict *nd2 = realloc(dec->dicts, (size_t)(dec->ndicts + 1) * sizeof *nd2);
    if (!nd2) goto done;
    dec->dicts = nd2;
    dec->dicts[dec->ndicts++] = dict;
    dict.syms = NULL, dict.n = 0;
    rc = 0;
done:
    for (uint32_t j = 0; j < dict.n; j++) free(dict.syms[j].mem);
    free(dict.syms);
    if (news)
        for (uint32_t j = 0; j < nnew; j++) free(news[j].mem);
    free(news);
    free(all);
    free(in);
    ia_free(ia);
    return rc;
}

// A text region segment (7.4.3; arithmetic coded only): onto the page
static int text_segment(Dec *dec, const uint8_t *d, size_t n, const uint32_t *refs, uint32_t nrefs) {
    if (n < 19) return -1;
    uint32_t w = be32(d), h = be32(d + 4), x = be32(d + 8), y = be32(d + 12);
    int op = d[16] & 7, fl = d[17] << 8 | d[18];
    if (fl & 1) return -1;   // (Huffman coded)
    TextParams p;
    memset(&p, 0, sizeof p);
    p.refine = fl >> 1 & 1;
    p.strips = 1 << (fl >> 2 & 3);
    p.corner = fl >> 4 & 3;
    p.transposed = fl >> 6 & 1;
    p.op = fl >> 7 & 3;
    p.defpix = fl >> 9 & 1;
    p.dsoffset = fl >> 10 & 31;
    if (p.dsoffset > 15) p.dsoffset -= 32;
    p.rt = fl >> 15 & 1;
    size_t i = 19;
    if (p.refine && !p.rt) {
        if (n < i + 4) return -1;
        memcpy(p.rat, d + i, 4);
        i += 4;
    }
    if (n < i + 4) return -1;
    p.ninst = be32(d + i);
    i += 4;
    if (!w || !h || w > 1u << 20 || h > 1u << 20 || (uint64_t)w * h > 1u << 28) return -1;
    uint32_t nsyms;
    const Bm **syms = referred_symbols(dec, refs, nrefs, &nsyms);
    Ia *ia = syms ? ia_new((int)nsyms) : NULL;
    Bm r = {0};
    int rc = -1;
    if (ia && !bm_new(&r, (int)w, (int)h)) {
        Mq m;
        mq_init(&m, d + i, d + n);
        rc = text_region(&m, ia, &r, syms, nsyms, &p);
        if (!rc) compose(&dec->page, &r, (int)x, (int)y, op);
    }
    free(r.mem);
    ia_free(ia);
    free(syms);
    return rc;
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
    int rc = 0;
    if (mmr)
        rc = generic_mmr(d + i, end - i, &r);
    else {
        uint8_t *cx = calloc(1 << 16, 1);
        Mq m;
        mq_init(&m, d + i, d + end);
        if (cx) generic_decode(&m, cx, &r, t, tpgdon, at);
        else rc = -1;
        free(cx);
    }
    if (!rc) compose(&dec->page, &r, (int)x, (int)y, op);
    free(r.mem);
    return rc;
}

// The segments of d .. d + len. 1: the end of the page (or of the file), 0: all of them read, -1: one
// not decoded.
static int segments(Dec *dec, const uint8_t *d, size_t len, int width, int height) {
    size_t i = 0;
    uint32_t *refs = NULL;
    int rc = -1;
    while (i + 11 <= len) {
        uint32_t num = be32(d + i);
        int fl = d[i + 4], type = fl & 63;
        i += 5;
        uint32_t nref = d[i] >> 5;
        if (nref == 7) {
            if (i + 4 > len) goto out;
            nref = be32(d + i) & 0x1fffffff;
            i += 4 + (nref + 8) / 8;
        } else
            i++;
        unsigned rs = num <= 256 ? 1 : num <= 65536 ? 2 : 4;
        if (nref > (len - i) / rs) goto out;
        free(refs);
        refs = malloc(((size_t)nref + 1) * sizeof *refs);
        if (!refs) goto out;
        for (uint32_t r = 0; r < nref; r++, i += rs)
            refs[r] = rs == 1 ? d[i] : rs == 2 ? (uint32_t)(d[i] << 8 | d[i + 1]) : be32(d + i);
        i += fl & 64 ? 4 : 1;
        if (i + 4 > len) goto out;
        uint32_t n = be32(d + i);
        i += 4;
        if (n != 0xffffffff && n > len - i) goto out;
        switch (type) {
        case 48:   // page information
            if (n < 19 || dec->haveInfo) break;
            dec->haveInfo = 1;
            if (bm_new(&dec->page, width, height)) goto out;
            if (d[i + 16] & 4)   // (the default pixel black)
                for (int y = 0; y < height; y++) memset(bm_row(&dec->page, y), 1, (size_t)width);
            break;
        case 0:   // symbol dictionary
            if (n == 0xffffffff || symbol_dict(dec, num, d + i, n, refs, nref)) goto out;
            break;
        case 6:
        case 7:   // immediate text region
            if (!dec->haveInfo || n == 0xffffffff || text_segment(dec, d + i, n, refs, nref)) goto out;
            break;
        case 38:
        case 39: {   // immediate generic region
            if (!dec->haveInfo) goto out;
            size_t used = len - i;
            if (generic_region(dec, d + i, n == 0xffffffff ? SIZE_MAX : n, &used)) goto out;
            n = (uint32_t)used;
            break;
        }
        case 49:   // end of page, of file
        case 51:
            rc = 1;
            goto out;
        case 4:    // intermediate regions: for a refinement region
        case 16:   // pattern dictionary: for a halftone region
        case 36:
        case 50:   // end of stripe
        case 52:   // profiles, tables, extension
        case 53:
        case 62:
            break;
        default:   // halftone, refinement regions
            goto out;
        }
        if (n == 0xffffffff) goto out;
        i += n;
    }
    rc = 0;
out:
    free(refs);
    return rc;
}

int nj2_decode(const uint8_t *data, size_t len, const uint8_t *globals, size_t glen, int width, int height,
               uint8_t *dst, size_t prow) {
    if (width <= 0 || height <= 0 || prow < ((size_t)width + 7) / 8) return -1;
    Dec dec;
    memset(&dec, 0, sizeof dec);
    int rc = globals && glen ? segments(&dec, globals, glen, width, height) : 0;
    if (rc >= 0) rc = segments(&dec, data, len, width, height);
    if (rc >= 0 && dec.haveInfo)
        for (int y = 0; y < height; y++) {
            const uint8_t *s = bm_row(&dec.page, y);
            uint8_t *o = dst + (size_t)y * prow;
            memset(o, 0, prow);
            for (int x = 0; x < width; x++)
                if (!s[x]) o[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        }
    for (int k = 0; k < dec.ndicts; k++) {
        for (uint32_t j = 0; j < dec.dicts[k].n; j++) free(dec.dicts[k].syms[j].mem);
        free(dec.dicts[k].syms);
    }
    free(dec.dicts);
    free(dec.page.mem);
    return rc >= 0 && dec.haveInfo ? 0 : -1;
}
