// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// Written from ITU-T T.800 (06/2019), without another decoder's code: codestream syntax
// (annex A), geometry, packet headers and tag trees (B), MQ (C), coefficient coding (D),
// inverse quantization (E), inverse wavelets (F), component transforms (G), ROI (H), JP2 (I).
#include "nitrojpx.h"
#include "nitro_os.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LEVELS 33
#define BANDS (3 * (LEVELS - 1) + 1)
#define MEM_LIMIT ((size_t)512 << 20)
#define SAMPLE_LIMIT ((size_t)64 << 20)
#define ITEM_LIMIT 1000000

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int cdiv(int a, int b) { return (int)(((int64_t)a + b - 1) / b); }
static int cshift(int a, int n) { return (int)(((int64_t)a + ((int64_t)1 << n) - 1) >> n); }
static int ilog(unsigned a) { int n = 0; while (a >>= 1) n++; return n; }

typedef struct {
    int nl, cbx, cby, style, reversible, pp[LEVELS];
    int quant, guard, nq, exp[BANDS], mant[BANDS], roi;
    int have_cod, have_qcd;
} Coding;
typedef struct {
    const uint8_t *p, *end;
    unsigned byte;
    int left, error;
} Bits;

// Packet headers and raw coding segments both stuff one bit after an FF byte (B.10.1, D.6).
static unsigned bits(Bits *b, int n) {
    unsigned v = 0;
    if (n < 0 || n > 31) { b->error = 1; return 0; }
    while (n--) {
        if (!b->left) {
            int count = b->byte == 255 ? 7 : 8;
            if (b->p == b->end) { b->error = 1; return 0; }
            b->byte = *b->p++;
            if (count == 7 && (b->byte & 128)) b->error = 1;
            b->left = count;
        }
        v = (v << 1) | ((b->byte >> --b->left) & 1);
    }
    return v;
}

typedef struct { uint16_t qe; uint8_t nmps, nlps, sw; } Qe;
static const Qe qe[47] = {
    {0x5601,1,1,1},{0x3401,2,6,0},{0x1801,3,9,0},{0x0ac1,4,12,0},{0x0521,5,29,0},
    {0x0221,38,33,0},{0x5601,7,6,1},{0x5401,8,14,0},{0x4801,9,14,0},{0x3801,10,14,0},
    {0x3001,11,17,0},{0x2401,12,18,0},{0x1c01,13,20,0},{0x1601,29,21,0},{0x5601,15,14,1},
    {0x5401,16,14,0},{0x5101,17,15,0},{0x4801,18,16,0},{0x3801,19,17,0},{0x3401,20,18,0},
    {0x3001,21,19,0},{0x2801,22,19,0},{0x2401,23,20,0},{0x2201,24,21,0},{0x1c01,25,22,0},
    {0x1801,26,23,0},{0x1601,27,24,0},{0x1401,28,25,0},{0x1201,29,26,0},{0x1101,30,27,0},
    {0x0ac1,31,28,0},{0x09c1,32,29,0},{0x08a1,33,30,0},{0x0521,34,31,0},{0x0441,35,32,0},
    {0x02a1,36,33,0},{0x0221,37,34,0},{0x0141,38,35,0},{0x0111,39,36,0},{0x0085,40,37,0},
    {0x0049,41,38,0},{0x0025,42,39,0},{0x0015,43,40,0},{0x0009,44,41,0},{0x0005,45,42,0},
    {0x0001,45,43,0},{0x5601,46,46,0}
};
typedef struct { const uint8_t *p; size_t n, at; uint32_t a, c; int ct; } Mq;
static unsigned mqbyte(const Mq *m, size_t at) { return at < m->n ? m->p[at] : 255; }
static void mq_bytein(Mq *m) {
    unsigned next = mqbyte(m, m->at + 1);
    if (mqbyte(m, m->at) == 255) {
        if (next > 0x8f) { m->c += 0xff00; m->ct = 8; }
        else { if (m->at < m->n) m->at++; m->c += next << 9; m->ct = 7; }
    } else { if (m->at < m->n) m->at++; m->c += next << 8; m->ct = 8; }
}
static void mq_init(Mq *m, const uint8_t *p, size_t n) {
    memset(m, 0, sizeof(*m)); m->p = p; m->n = n;
    m->c = mqbyte(m, 0) << 16; mq_bytein(m); m->c <<= 7; m->ct -= 7; m->a = 0x8000;
}
static int mq_bit(Mq *m, uint8_t *cx) {
    unsigned i = *cx >> 1, mps = *cx & 1, q = qe[i].qe;
    int d;
    m->a -= q;
    if ((m->c >> 16) < q) {
        if (m->a < q) { d = (int)mps; *cx = (uint8_t)(qe[i].nmps << 1 | mps); }
        else { d = 1 - (int)mps; *cx = (uint8_t)(qe[i].nlps << 1 | (mps ^ qe[i].sw)); }
        m->a = q;
    } else {
        m->c -= q << 16;
        if (m->a & 0x8000) return (int)mps;
        if (m->a < q) { d = 1 - (int)mps; *cx = (uint8_t)(qe[i].nlps << 1 | (mps ^ qe[i].sw)); }
        else { d = (int)mps; *cx = (uint8_t)(qe[i].nmps << 1 | mps); }
    }
    do { if (!m->ct) mq_bytein(m); m->a <<= 1; m->c <<= 1; m->ct--; } while (!(m->a & 0x8000));
    return d;
}
static void contexts(uint8_t cx[19]) { memset(cx, 0, 19); cx[0] = 4 << 1; cx[17] = 3 << 1; cx[18] = 46 << 1; }

typedef struct { int low, value; } Node;
typedef struct { int levels, w[16], h[16], off[16]; Node *node; } Tree;
typedef struct Segment { struct Segment *next; uint8_t *data; size_t len; int first, passes, capacity; } Segment;
typedef struct {
    int x0, y0, x1, y1, passes, zbp, lblock;
    Segment *first, *last;
} Block;
typedef struct { int orient, x0, y0, x1, y1, nx, ny, mb; double step; Block *cb; Tree inc, zero; } Band;
typedef struct { Band band[3]; } Precinct;
typedef struct { int x0, y0, x1, y1, px0, py0, nx, ny; Precinct *prec; } Resolution;
typedef struct { int x0, y0, x1, y1; Resolution res[LEVELS]; double *plane; } Component;
typedef struct { uint64_t key[5]; int c, r, p, l; } Packet;
typedef struct {
    int started, parts, done, x0, y0, x1, y1, layers, order, mct, flags;
    Coding coding[NJPX_MAX_COMPS]; Component comp[NJPX_MAX_COMPS];
    Packet *packets; size_t npackets, next;
} Tile;
typedef struct {
    size_t memory, items;
    int xs, ys, x0, y0, tw, th, tx0, ty0, nx, ny, nc, siz, layers, order, mct, flags;
    int prec[NJPX_MAX_COMPS], sign[NJPX_MAX_COMPS], dx[NJPX_MAX_COMPS], dy[NJPX_MAX_COMPS];
    Coding coding[NJPX_MAX_COMPS]; Tile *tiles;
    int colorspace, alpha, premult, raw_colors, serial;
    int reduce, ox0, oy0, oxs, oys;   // (NJPX_REDUCE: the levels left out; the output grid, reduced)
    const uint8_t *palette, *cmap, *cdef; size_t plen, mlen, dlen;
    uint16_t *output;
} Jpx;

// Account for every allocation, including compressed code-block data and packet/tag-tree state.
// Coefficient workers share this budget; reserve before allocating, including on failure.
static int reserve(Jpx *j, size_t n) {
    size_t used = __atomic_load_n(&j->memory, __ATOMIC_RELAXED);
    do {
        if (n > MEM_LIMIT - used) return -1;
    } while (!__atomic_compare_exchange_n(&j->memory, &used, used + n, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return 0;
}
static void *jalloc(Jpx *j, size_t n) {
    if (n > SIZE_MAX - sizeof(size_t) || reserve(j, n)) return NULL;
    size_t *p = calloc(1, n + sizeof(size_t));
    if (!p) { __atomic_fetch_sub(&j->memory, n, __ATOMIC_RELAXED); return NULL; }
    *p = n; return p + 1;
}
static void jfree(Jpx *j, void *v) {
    if (v) { size_t *p = (size_t *)v - 1; __atomic_fetch_sub(&j->memory, *p, __ATOMIC_RELAXED); free(p); }
}
static int append(Jpx *j, Segment *s, const uint8_t *p, size_t n) {
    if (reserve(j, n)) return -1;
    size_t *q = s->data ? (size_t *)s->data - 1 : NULL;
    q = realloc(q, sizeof(size_t) + s->len + n);
    if (!q) { __atomic_fetch_sub(&j->memory, n, __ATOMIC_RELAXED); return -1; }
    q[0] = s->len + n; s->data = (uint8_t *)(q + 1);
    memcpy(s->data + s->len, p, n); s->len += n; return 0;
}
static int tree_new(Jpx *j, Tree *t, int w, int h) {
    int total = 0;
    while (1) {
        int l = t->levels++;
        if (l >= 16 || (int64_t)w * h > ITEM_LIMIT - total) return -1;
        t->w[l] = w; t->h[l] = h; t->off[l] = total; total += w * h;
        if (w == 1 && h == 1) break;
        w = (w + 1) / 2; h = (h + 1) / 2;
    }
    t->node = jalloc(j, (size_t)total * sizeof(Node));
    if (!t->node) return -1;
    for (int i = 0; i < total; i++) t->node[i].value = INT_MAX;
    return 0;
}
static int tree_read(Tree *t, int x, int y, int threshold, Bits *b) {
    int low = 0, value = INT_MAX;
    for (int l = t->levels - 1; l >= 0; l--) {
        Node *n = &t->node[t->off[l] + (y >> l) * t->w[l] + (x >> l)];
        if (low > n->low) n->low = low;
        while (n->low < threshold && n->low < n->value && !b->error) {
            if (bits(b, 1)) n->value = n->low;
            else n->low++;
        }
        low = n->low; value = n->value;
    }
    return value;
}

// --- marker segments (annex A) ---------------------------------------------------------------
static int coding(Coding *c, const uint8_t *p, size_t n, int explicit_precincts) {
    if (n < 5 || p[0] >= LEVELS || p[1] > 8 || p[2] > 8 || p[1] + p[2] > 8 || p[3] > 63 || p[4] > 1) return -1;
    c->nl = p[0]; c->cbx = p[1] + 2; c->cby = p[2] + 2; c->style = p[3]; c->reversible = p[4];
    if (n != (size_t)(5 + (explicit_precincts ? c->nl + 1 : 0))) return -1;
    for (int r = 0; r <= c->nl; r++) {
        c->pp[r] = explicit_precincts ? p[5 + r] : 255;
        if (r && (!(c->pp[r] & 15) || !(c->pp[r] >> 4))) return -1;
    }
    c->have_cod = 1; return 0;
}
static int quant(Coding *c, const uint8_t *p, size_t n) {
    if (n < 2 || (p[0] & 31) > 2) return -1;
    c->quant = p[0] & 31; c->guard = p[0] >> 5;
    if (c->quant && ((n - 1) & 1)) return -1;
    c->nq = (int)(n - 1) / (c->quant ? 2 : 1);
    if (c->nq < 1 || c->nq > BANDS || (c->quant == 1 && c->nq != 1)) return -1;
    for (int i = 0; i < c->nq; i++) {
        unsigned v = c->quant ? be16(p + 1 + 2 * i) : (unsigned)p[1 + i] << 8;
        c->exp[i] = v >> 11; c->mant[i] = v & 2047;
    }
    c->have_qcd = 1; return 0;
}
static int marker(Jpx *j, Tile *t, unsigned m, const uint8_t *p, size_t n) {
    Coding *c = t ? t->coding : j->coding;
    if (m == 0xff51) {
        if (t || j->siz || n < 36) return -1;
        // Part 2/HT capabilities cannot be interpreted as Part 1 coding styles.
        if (be16(p) & 0xc000) return -1;
        uint32_t v[8];
        for (int i = 0; i < 8; i++) { v[i] = be32(p + 2 + 4 * i); if (v[i] > (1u << 28)) return -1; }
        j->xs = (int)v[0]; j->ys = (int)v[1]; j->x0 = (int)v[2]; j->y0 = (int)v[3];
        j->tw = (int)v[4]; j->th = (int)v[5]; j->tx0 = (int)v[6]; j->ty0 = (int)v[7]; j->nc = (int)be16(p + 34);
        if (j->nc < 1 || j->nc > NJPX_MAX_COMPS || n != 36u + 3u * j->nc ||
            j->xs <= j->x0 || j->ys <= j->y0 || !j->tw || !j->th || j->tx0 > j->x0 || j->ty0 > j->y0 ||
            j->tx0 + j->tw <= j->x0 || j->ty0 + j->th <= j->y0 ||
            (uint64_t)(j->xs - j->x0) * (j->ys - j->y0) * j->nc > SAMPLE_LIMIT) return -1;
        j->nx = cdiv(j->xs - j->tx0, j->tw); j->ny = cdiv(j->ys - j->ty0, j->th);
        if ((int64_t)j->nx * j->ny > 65535) return -1;
        for (int i = 0; i < j->nc; i++) {
            j->prec[i] = (p[36 + 3 * i] & 127) + 1; j->sign[i] = p[36 + 3 * i] >> 7;
            j->dx[i] = p[37 + 3 * i]; j->dy[i] = p[38 + 3 * i];
            if (j->prec[i] > 16 || !j->dx[i] || !j->dy[i]) return -1;
            if (cdiv(j->xs, j->dx[i]) <= cdiv(j->x0, j->dx[i]) ||
                cdiv(j->ys, j->dy[i]) <= cdiv(j->y0, j->dy[i])) return -1;
        }
        j->siz = 1;
    } else if (m == 0xff52) {
        if (!j->siz || n < 10 || p[0] > 7 || p[1] > 4 || !be16(p + 2) || p[4] > 1) return -1;
        if (t) { t->flags = p[0]; t->order = p[1]; t->layers = (int)be16(p + 2); t->mct = p[4]; }
        else { j->flags = p[0]; j->order = p[1]; j->layers = (int)be16(p + 2); j->mct = p[4]; }
        for (int i = 0; i < j->nc; i++) if (coding(c + i, p + 5, n - 5, p[0] & 1)) return -1;
    } else if (m == 0xff53) {
        if (!j->siz || n < 7 || p[0] >= j->nc || p[1] > 1) return -1;
        return coding(c + p[0], p + 2, n - 2, p[1] & 1);
    } else if (m == 0xff5c) {
        if (!j->siz) return -1;
        for (int i = 0; i < j->nc; i++) if (quant(c + i, p, n)) return -1;
    } else if (m == 0xff5d) {
        if (!j->siz || n < 3 || p[0] >= j->nc) return -1;
        return quant(c + p[0], p + 1, n - 1);
    } else if (m == 0xff5e) {
        if (!j->siz || n != 3 || p[0] >= j->nc || p[1] || p[2] > 30) return -1;
        c[p[0]].roi = p[2];
    } else if (m != 0xff64 && m != 0xff55 && m != 0xff57 && m != 0xff58) {
        // COM, TLM, PLM, PLT have no effect on the decoded samples; do not silently ignore
        // coding extensions, progression changes, or packet headers stored elsewhere.
        return -1;
    }
    return 0;
}

// --- tile geometry, precincts, code-blocks, progression (annex B) -------------------------------
static int band_new(Jpx *j, Band *b, const Coding *c, const Component *tc, int r, int orient, int px, int py, int precision) {
    int n = c->nl - r + (r != 0), bx = orient & 1, by = orient >> 1;
    b->orient = orient;
    int x0 = cshift(tc->x0 - (bx ? (1 << (n - 1)) : 0), n);
    int y0 = cshift(tc->y0 - (by ? (1 << (n - 1)) : 0), n);
    int x1 = cshift(tc->x1 - (bx ? (1 << (n - 1)) : 0), n);
    int y1 = cshift(tc->y1 - (by ? (1 << (n - 1)) : 0), n);
    int ppx = (c->pp[r] & 15) - (r != 0), ppy = (c->pp[r] >> 4) - (r != 0);
    b->x0 = imax(x0, px << ppx); b->x1 = imin(x1, (px + 1) << ppx);
    b->y0 = imax(y0, py << ppy); b->y1 = imin(y1, (py + 1) << ppy);
    if (b->x1 <= b->x0 || b->y1 <= b->y0) return 0;
    int cbx = imin(c->cbx, ppx), cby = imin(c->cby, ppy);
    int cx0 = b->x0 >> cbx, cy0 = b->y0 >> cby;
    b->nx = cshift(b->x1, cbx) - cx0; b->ny = cshift(b->y1, cby) - cy0;
    if ((size_t)b->nx * b->ny > ITEM_LIMIT - j->items) return -1;
    j->items += (size_t)b->nx * b->ny;
    int q = r ? 3 * (r - 1) + orient : 0;
    int exponent = c->exp[c->quant == 1 ? 0 : q];
    if (c->quant == 1 && q) exponent -= (q - 1) / 3;
    if (c->quant != 1 && q >= c->nq) return -1;
    b->mb = c->guard + exponent - 1 + c->roi;
    if (b->mb < 0 || b->mb > 30) return -1;
    b->step = c->reversible ? 1 : ldexp(1 + c->mant[c->quant == 1 ? 0 : q] / 2048.0,
                                                         precision + bx + by - exponent);
    b->cb = jalloc(j, (size_t)b->nx * b->ny * sizeof(Block));
    if (!b->cb || tree_new(j, &b->inc, b->nx, b->ny) || tree_new(j, &b->zero, b->nx, b->ny)) return -1;
    for (int y = 0; y < b->ny; y++) for (int x = 0; x < b->nx; x++) {
        Block *cb = b->cb + y * b->nx + x;
        cb->x0 = imax(b->x0, (cx0 + x) << cbx); cb->x1 = imin(b->x1, (cx0 + x + 1) << cbx);
        cb->y0 = imax(b->y0, (cy0 + y) << cby); cb->y1 = imin(b->y1, (cy0 + y + 1) << cby);
        cb->lblock = 3;
    }
    return 0;
}
static int packet_cmp(const void *aa, const void *bb) {
    const Packet *a = aa, *b = bb;
    for (int i = 0; i < 5; i++) if (a->key[i] != b->key[i]) return a->key[i] < b->key[i] ? -1 : 1;
    return 0;
}
static int tile_new(Jpx *j, Tile *t) {
    size_t count = 0;
    if (!t->layers || (t->mct && j->nc < 3)) return -1;
    for (int c = 0; c < j->nc; c++) {
        Component *tc = t->comp + c; Coding *co = t->coding + c;
        if (!co->have_cod || !co->have_qcd || co->nl > 28) return -1;
        tc->x0 = cdiv(t->x0, j->dx[c]); tc->x1 = cdiv(t->x1, j->dx[c]);
        tc->y0 = cdiv(t->y0, j->dy[c]); tc->y1 = cdiv(t->y1, j->dy[c]);
        for (int r = 0; r <= co->nl; r++) {
            Resolution *rs = tc->res + r;
            int n = co->nl - r, px = co->pp[r] & 15, py = co->pp[r] >> 4;
            rs->x0 = cshift(tc->x0, n); rs->x1 = cshift(tc->x1, n);
            rs->y0 = cshift(tc->y0, n); rs->y1 = cshift(tc->y1, n);
            if (rs->x0 == rs->x1 || rs->y0 == rs->y1) continue;
            rs->px0 = rs->x0 >> px; rs->py0 = rs->y0 >> py;
            rs->nx = cshift(rs->x1, px) - rs->px0; rs->ny = cshift(rs->y1, py) - rs->py0;
            size_t np = (size_t)rs->nx * rs->ny;
            if (np > (ITEM_LIMIT - count) / t->layers) return -1;
            count += np * t->layers;
            rs->prec = jalloc(j, np * sizeof(Precinct));
            if (!rs->prec) return -1;
            for (int y = 0; y < rs->ny; y++) for (int x = 0; x < rs->nx; x++)
                for (int b = 0; b < (r ? 3 : 1); b++)
                    if (band_new(j, rs->prec[y * rs->nx + x].band + b, co, tc, r, r ? b + 1 : 0,
                                 x + rs->px0, y + rs->py0, j->prec[c])) return -1;
        }
    }
    if (t->mct) for (int c = 1; c < 3; c++)
        if (j->dx[c] != j->dx[0] || j->dy[c] != j->dy[0] || j->prec[c] != j->prec[0] ||
            t->coding[c].reversible != t->coding[0].reversible) return -1;
    if (count > ITEM_LIMIT - j->items) return -1;
    j->items += count;
    t->packets = jalloc(j, count * sizeof(Packet)); t->npackets = count;
    if (!t->packets) return -1;
    size_t at = 0;
    for (int c = 0; c < j->nc; c++) for (int r = 0; r <= t->coding[c].nl; r++) {
        Resolution *rs = &t->comp[c].res[r]; int pp = t->coding[c].pp[r], n = t->coding[c].nl - r;
        for (int y = 0; y < rs->ny; y++) for (int x = 0; x < rs->nx; x++) for (int l = 0; l < t->layers; l++) {
            Packet *p = &t->packets[at++];
            uint64_t xx = (uint64_t)(x + rs->px0) * ((uint64_t)1 << ((pp & 15) + n)) * j->dx[c];
            uint64_t yy = (uint64_t)(y + rs->py0) * ((uint64_t)1 << ((pp >> 4) + n)) * j->dy[c];
            if (!x && xx < (unsigned)t->x0) xx = (unsigned)t->x0;
            if (!y && yy < (unsigned)t->y0) yy = (unsigned)t->y0;
            p->c = c; p->r = r; p->p = y * rs->nx + x; p->l = l;
            uint64_t keys[5][5] = {{l,r,c,p->p,0},{r,l,c,p->p,0},{r,yy,xx,c,l},{yy,xx,c,r,l},{c,yy,xx,r,l}};
            memcpy(p->key, keys[t->order], sizeof(p->key));
        }
    }
    qsort(t->packets, count, sizeof(Packet), packet_cmp);
    t->started = 1; return 0;
}

// A codeword segment ends at each pass (TERMALL), or at the lazy-coding boundaries (D.6).
static int seg_capacity(int first, int style) {
    if (style & 4) return 1;
    if (!(style & 1)) return 109;
    if (first < 10) return 10 - first;
    return first % 3 == 1 ? 2 : 1;
}
static int numpasses(Bits *b) {
    if (!bits(b, 1)) return 1;
    if (!bits(b, 1)) return 2;
    unsigned n = bits(b, 2); if (n < 3) return 3 + (int)n;
    n = bits(b, 5); if (n < 31) return 6 + (int)n;
    return 37 + (int)bits(b, 7);
}
typedef struct { Segment *s; size_t len; } Contribution;
static int packet(Jpx *j, Tile *t, const Packet *pk, const uint8_t **pos, const uint8_t *end) {
    const uint8_t *p = *pos;
    if (t->flags & 2) { if ((size_t)(end - p) < 6 || be16(p) != 0xff91 || be16(p + 2) != 4) return -1; p += 6; }
    Bits h = {p, end, 0, 0, 0};
    // At most 109 contributions per code-block; bounded dynamically instead of a large stack.
    Contribution *list = NULL; size_t count = 0, cap = 0;
    Precinct *pr = &t->comp[pk->c].res[pk->r].prec[pk->p];
    int ok = 0;
    if (bits(&h, 1)) for (int b = 0; b < (pk->r ? 3 : 1); b++) {
        Band *band = pr->band + b;
        for (int y = 0; y < band->ny; y++) for (int x = 0; x < band->nx; x++) {
            Block *cb = band->cb + y * band->nx + x;
            int included = cb->passes ? (int)bits(&h, 1) : tree_read(&band->inc, x, y, pk->l + 1, &h) <= pk->l;
            if (!included) continue;
            if (!cb->passes) {
                cb->zbp = tree_read(&band->zero, x, y, band->mb + 1, &h);
                if (cb->zbp > band->mb) goto done;
            }
            int n = numpasses(&h);
            if (n > 3 * (band->mb - cb->zbp) - 2 - cb->passes) goto done;
            while (bits(&h, 1)) { if (++cb->lblock > 30 || h.error) goto done; }
            while (n > 0) {
                Segment *s = cb->last;
                if (!s || s->passes == s->capacity) {
                    s = jalloc(j, sizeof(*s)); if (!s) goto done;
                    s->first = cb->passes; s->capacity = seg_capacity(cb->passes, t->coding[pk->c].style);
                    if (cb->last) cb->last->next = s; else cb->first = s;
                    cb->last = s;
                }
                int take = imin(n, s->capacity - s->passes), nb = cb->lblock + ilog((unsigned)take);
                size_t bytes = bits(&h, nb);
                if (h.error || nb > 31) goto done;
                if (count == cap) {
                    size_t newcap = cap ? cap * 2 : 32;
                    if (newcap > ITEM_LIMIT) goto done;
                    Contribution *q = jalloc(j, newcap * sizeof(*q)); if (!q) goto done;
                    if (count) memcpy(q, list, count * sizeof(*q)); jfree(j, list); list = q; cap = newcap;
                }
                list[count++] = (Contribution){s, bytes};
                s->passes += take; cb->passes += take; n -= take;
            }
        }
    }
    if (h.error) goto done;
    // A header ending in FF has one more stuffed bit even when already byte-aligned.
    if (h.byte == 255) { h.left = 0; bits(&h, 1); if (h.error) goto done; }
    p = h.p;
    if (t->flags & 4) { if ((size_t)(end - p) < 2 || be16(p) != 0xff92) goto done; p += 2; }
    for (size_t i = 0; i < count; i++) {
        if (list[i].len > (size_t)(end - p) || append(j, list[i].s, p, list[i].len)) goto done;
        p += list[i].len;
    }
    *pos = p; ok = 1;
done:
    jfree(j, list); return ok ? 0 : -1;
}

// --- coefficient bit-planes (annex D) --------------------------------------------------------
enum { SIG = 1, NEG = 2, VISIT = 4, REFINE = 8,
       NW = 1 << 4, N = 1 << 5, NE = 1 << 6, W = 1 << 7, E = 1 << 8,
       SW = 1 << 9, S = 1 << 10, SE = 1 << 11, NEIGHBOURS = 0xff0, BELOW = SW | S | SE };

// D.1's significance contexts, indexed by the eight cached neighbour significance bits.
// Rows: LL/LH, HL (horizontal/vertical exchanged), HH. A significance transition updates
// its eight neighbours once, instead of fetching all eight states on every coding pass.
static const uint8_t zc_lut[3][256] = {
    {
        0,1,3,3,1,2,3,3,5,6,7,7,6,6,7,7,5,6,7,7,6,6,7,7,8,8,8,8,8,8,8,8,
        1,2,3,3,2,2,3,3,6,6,7,7,6,6,7,7,6,6,7,7,6,6,7,7,8,8,8,8,8,8,8,8,
        3,3,4,4,3,3,4,4,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8,
        3,3,4,4,3,3,4,4,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8,
        1,2,3,3,2,2,3,3,6,6,7,7,6,6,7,7,6,6,7,7,6,6,7,7,8,8,8,8,8,8,8,8,
        2,2,3,3,2,2,3,3,6,6,7,7,6,6,7,7,6,6,7,7,6,6,7,7,8,8,8,8,8,8,8,8,
        3,3,4,4,3,3,4,4,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8,
        3,3,4,4,3,3,4,4,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8,
    },
    {
        0,1,5,6,1,2,6,6,3,3,7,7,3,3,7,7,3,3,7,7,3,3,7,7,4,4,7,7,4,4,7,7,
        1,2,6,6,2,2,6,6,3,3,7,7,3,3,7,7,3,3,7,7,3,3,7,7,4,4,7,7,4,4,7,7,
        5,6,8,8,6,6,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,
        6,6,8,8,6,6,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,
        1,2,6,6,2,2,6,6,3,3,7,7,3,3,7,7,3,3,7,7,3,3,7,7,4,4,7,7,4,4,7,7,
        2,2,6,6,2,2,6,6,3,3,7,7,3,3,7,7,3,3,7,7,3,3,7,7,4,4,7,7,4,4,7,7,
        6,6,8,8,6,6,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,
        6,6,8,8,6,6,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,7,7,8,8,
    },
    {
        0,3,1,4,3,6,4,7,1,4,2,5,4,7,5,7,1,4,2,5,4,7,5,7,2,5,2,5,5,7,5,7,
        3,6,4,7,6,8,7,8,4,7,5,7,7,8,7,8,4,7,5,7,7,8,7,8,5,7,5,7,7,8,7,8,
        1,4,2,5,4,7,5,7,2,5,2,5,5,7,5,7,2,5,2,5,5,7,5,7,2,5,2,5,5,7,5,7,
        4,7,5,7,7,8,7,8,5,7,5,7,7,8,7,8,5,7,5,7,7,8,7,8,5,7,5,7,7,8,7,8,
        3,6,4,7,6,8,7,8,4,7,5,7,7,8,7,8,4,7,5,7,7,8,7,8,5,7,5,7,7,8,7,8,
        6,8,7,8,8,8,8,8,7,8,7,8,8,8,8,8,7,8,7,8,8,8,8,8,7,8,7,8,8,8,8,8,
        4,7,5,7,7,8,7,8,5,7,5,7,7,8,7,8,5,7,5,7,7,8,7,8,5,7,5,7,7,8,7,8,
        7,8,7,8,8,8,8,8,7,8,7,8,8,8,8,8,7,8,7,8,8,8,8,8,7,8,7,8,8,8,8,8,
    },
};
typedef struct {
    int w, h, stride, orient, causal, raw;
    uint16_t *state; uint8_t cx[19]; const uint8_t *zc; uint32_t *mag;
    Mq mq; Bits rb;
} Coeff;
static unsigned neighbours(const Coeff *c, int k, int y) {
    unsigned flags = c->state[k];
    if (c->causal && (y & 3) == 3) flags &= ~BELOW;
    return (flags & NEIGHBOURS) >> 4;
}
static int zcontext(const Coeff *c, int x, int y) {
    return c->zc[neighbours(c, (y + 1) * c->stride + x + 1, y)];
}
static int sign_contrib(uint16_t s) { return s & SIG ? (s & NEG ? -1 : 1) : 0; }
static int decision(Coeff *c, int cx) {
    if (!c->raw) return mq_bit(&c->mq, c->cx + cx);
    Bits *b = &c->rb;
    if (!b->left) {
        b->left = b->byte == 255 ? 7 : 8;
        // D.6 permits truncating a raw segment's trailing FF bytes, just as MQ does.
        if (b->p != b->end) {
            b->byte = *b->p++;
            if (b->left == 7 && (b->byte & 128)) b->error = 1;
        } else b->byte = 255;
    }
    return (b->byte >> --b->left) & 1;
}
static void significant(Coeff *c, int x, int y, int bp) {
    int k = (y + 1) * c->stride + x + 1, s;
    if (c->raw) s = decision(c, 0);
    else {
        int h = sign_contrib(c->state[k - 1]) + sign_contrib(c->state[k + 1]);
        int v = sign_contrib(c->state[k - c->stride]);
        if (!(c->causal && (y & 3) == 3)) v += sign_contrib(c->state[k + c->stride]);
        h = imax(-1, imin(1, h)); v = imax(-1, imin(1, v));
        int flip = h < 0 || (!h && v < 0);
        if (flip) { h = -h; v = -v; }
        int cx = h ? (v < 0 ? 11 : v ? 13 : 12) : v ? 10 : 9;
        s = mq_bit(&c->mq, c->cx + cx) ^ flip;
    }
    c->state[k] |= (uint16_t)(SIG | (s ? NEG : 0));
    int stride = c->stride;
    c->state[k - stride - 1] |= SE; c->state[k - stride] |= S; c->state[k - stride + 1] |= SW;
    c->state[k - 1] |= E; c->state[k + 1] |= W;
    c->state[k + stride - 1] |= NE; c->state[k + stride] |= N; c->state[k + stride + 1] |= NW;
    c->mag[k] = 3u << bp;   // twice the magnitude, with the unknown suffix at its midpoint
}
static int block_decode(Jpx *j, const Block *cb, const Band *b, const Coding *co, double *dst, int stride, int ox, int oy) {
    if (!cb->passes) return 0;
    Coeff c = {0}; c.w = cb->x1 - cb->x0; c.h = cb->y1 - cb->y0; c.stride = c.w + 2;
    c.orient = b->orient; c.causal = co->style & 8;
    c.zc = zc_lut[b->orient == 3 ? 2 : b->orient == 1 ? 1 : 0];
    size_t count = (size_t)c.stride * (c.h + 2);
    c.state = jalloc(j, count * sizeof(uint16_t)); c.mag = jalloc(j, count * sizeof(uint32_t));
    if (!c.state || !c.mag) { jfree(j, c.state); jfree(j, c.mag); return -1; }
    contexts(c.cx);
    int rc = -1;
    for (const Segment *s = cb->first; s; s = s->next) {
        c.raw = (co->style & 1) && s->first >= 10 && s->first % 3 != 0;
        if (c.raw) c.rb = (Bits){s->data, s->data ? s->data + s->len : NULL, 0, 0, 0};
        else mq_init(&c.mq, s->data, s->len);
        for (int pass = s->first; pass < s->first + s->passes; pass++) {
            int type = pass % 3;   // 0 cleanup, 1 significance propagation, 2 refinement
            int bp = b->mb - cb->zbp - 1 - (pass + 2) / 3;
            if (bp < 0 || bp > 29) goto done;
            for (int y0 = 0; y0 < c.h; y0 += 4) for (int x = 0; x < c.w; x++) {
                int start = y0, stop = imin(y0 + 4, c.h), run = type == 0 && stop == y0 + 4;
                if (run) for (int y = start; y < stop; y++) {
                    int k = (y + 1) * c.stride + x + 1;
                    if ((c.state[k] & (SIG | VISIT)) || neighbours(&c, k, y)) { run = 0; break; }
                }
                if (run) {
                    if (!mq_bit(&c.mq, c.cx + 17)) continue;
                    int skip = mq_bit(&c.mq, c.cx + 18) << 1; skip |= mq_bit(&c.mq, c.cx + 18);
                    start += skip; significant(&c, x, start++, bp);
                }
                for (int y = start; y < stop; y++) {
                    int k = (y + 1) * c.stride + x + 1;
                    uint16_t state = c.state[k];
                    if (type == 2) {
                        if (!(state & SIG) || (state & VISIT)) continue;
                        int cx = state & REFINE ? 16 : neighbours(&c, k, y) ? 15 : 14;
                        unsigned bit = (unsigned)decision(&c, cx);
                        c.mag[k] = (c.mag[k] & ~(1u << (bp + 1))) | (bit << (bp + 1)) | (1u << bp);
                        c.state[k] |= REFINE;
                    } else {
                        if (state & (SIG | VISIT)) continue;
                        int cx = zcontext(&c, x, y);
                        if (type == 1 && !cx) continue;
                        if (decision(&c, cx)) significant(&c, x, y, bp);
                        if (type == 1) c.state[k] |= VISIT;
                    }
                }
            }
            if (type == 0) {
                for (size_t i = 0; i < count; i++) c.state[i] &= (uint16_t)~VISIT;
                if (co->style & 32) {
                    unsigned v = 0; for (int i = 0; i < 4; i++) v = (v << 1) | mq_bit(&c.mq, c.cx + 18);
                    if (v != 10) goto done;
                }
            }
            if (c.raw && c.rb.error) goto done;
            if (co->style & 2) contexts(c.cx);
        }
    }
    for (int y = 0; y < c.h; y++) for (int x = 0; x < c.w; x++) {
        int k = (y + 1) * c.stride + x + 1;
        uint32_t mag = c.mag[k];
        if (co->roi && (mag >> 1) >= (1u << co->roi)) mag >>= co->roi;
        double v = co->reversible ? (double)(mag >> 1) : mag * 0.5 * b->step;
        dst[(size_t)(oy + y) * stride + ox + x] = c.state[k] & NEG ? -v : v;
    }
    rc = 0;
done:
    jfree(j, c.state); jfree(j, c.mag); return rc;
}

// --- inverse wavelets and component transforms (annexes E, F, G) ------------------------------
// v is in spatial parity order, not packed low/high order. Reflection duplicates the single
// available neighbour at an edge. Double arithmetic keeps reversible sums exact for our limits.
static void inverse_line(double *v, int n, int origin, int reversible) {
    if (n == 1) { if (origin & 1) v[0] *= 0.5; return; }
    int even = origin & 1, odd = 1 - even;
    if (reversible) {
        for (int i = even; i < n; i += 2) {
            double a = v[i ? i - 1 : i + 1], b = v[i + 1 < n ? i + 1 : i - 1];
            v[i] -= floor((a + b + 2) / 4);
        }
        for (int i = odd; i < n; i += 2) {
            double a = v[i ? i - 1 : i + 1], b = v[i + 1 < n ? i + 1 : i - 1];
            v[i] += floor((a + b) / 2);
        }
    } else {
        const double k = 1.230174104914001;
        for (int i = 0; i < n; i++) v[i] *= ((i + origin) & 1) ? 1 / k : k;
        static const double coeff[4] = {-0.443506852043971, -0.882911075530934, 0.052980118572961, 1.586134342059924};
        for (int s = 0; s < 4; s++) for (int i = (s & 1) ? odd : even; i < n; i += 2) {
            double a = v[i ? i - 1 : i + 1], b = v[i + 1 < n ? i + 1 : i - 1];
            v[i] += coeff[s] * (a + b);
        }
    }
}
// Work on a narrow strip of adjacent columns in row order. Gathering/scattering a whole
// column touches one cache line per sample; strips reuse the lines and let the lifting
// steps operate on consecutive doubles (SIMD on the host), with the same rounding order.
#define COLUMN_BATCH 16
static void inverse_columns(double *plane, int stride, int w, int h, int origin, int low,
                            int reversible, double *tmp) {
    int batch = imin(w, COLUMN_BATCH);
    int even = origin & 1, odd = 1 - even;
    for (int x0 = 0; x0 < w; x0 += batch) {
        int cols = imin(batch, w - x0), lo = 0, hi = low;
        for (int y = 0; y < h; y++) {
            int sy = ((y + origin) & 1) ? hi++ : lo++;
            memcpy(tmp + (size_t)y * batch, plane + (size_t)sy * stride + x0, (size_t)cols * sizeof(double));
        }
        if (h == 1) {
            if (origin & 1) for (int x = 0; x < cols; x++) tmp[x] *= 0.5;
        } else if (reversible) {
            for (int y = even; y < h; y += 2) {
                double *v = tmp + (size_t)y * batch;
                const double *a = tmp + (size_t)(y ? y - 1 : y + 1) * batch;
                const double *b = tmp + (size_t)(y + 1 < h ? y + 1 : y - 1) * batch;
                for (int x = 0; x < cols; x++) v[x] -= floor((a[x] + b[x] + 2) / 4);
            }
            for (int y = odd; y < h; y += 2) {
                double *v = tmp + (size_t)y * batch;
                const double *a = tmp + (size_t)(y ? y - 1 : y + 1) * batch;
                const double *b = tmp + (size_t)(y + 1 < h ? y + 1 : y - 1) * batch;
                for (int x = 0; x < cols; x++) v[x] += floor((a[x] + b[x]) / 2);
            }
        } else {
            const double k = 1.230174104914001;
            for (int y = 0; y < h; y++) {
                double scale = ((y + origin) & 1) ? 1 / k : k;
                for (int x = 0; x < cols; x++) tmp[(size_t)y * batch + x] *= scale;
            }
            static const double coeff[4] = {-0.443506852043971, -0.882911075530934, 0.052980118572961, 1.586134342059924};
            for (int s = 0; s < 4; s++) for (int y = (s & 1) ? odd : even; y < h; y += 2) {
                double *v = tmp + (size_t)y * batch;
                const double *a = tmp + (size_t)(y ? y - 1 : y + 1) * batch;
                const double *b = tmp + (size_t)(y + 1 < h ? y + 1 : y - 1) * batch;
                for (int x = 0; x < cols; x++) v[x] += coeff[s] * (a[x] + b[x]);
            }
        }
        for (int y = 0; y < h; y++)
            memcpy(plane + (size_t)y * stride + x0, tmp + (size_t)y * batch, (size_t)cols * sizeof(double));
    }
}
typedef struct {
    const Block *cb; const Band *band; const Coding *coding;
    double *plane; int stride, x, y;
} BlockJob;

static int component_wavelet(Jpx *j, Component *tc, const Coding *co) {
    int stride = tc->x1 - tc->x0, height = tc->y1 - tc->y0;
    if (!stride || !height || co->nl <= j->reduce) return 0;
    double *line = jalloc(j, (size_t)imax(stride, height * imin(stride, COLUMN_BATCH)) * sizeof(double));
    if (!line) return -1;
    for (int r = 1; r <= co->nl - j->reduce; r++) {
        Resolution *rs = tc->res + r;
        int lw = cshift(tc->x1, co->nl - r + 1) - cshift(tc->x0, co->nl - r + 1);
        int lh = cshift(tc->y1, co->nl - r + 1) - cshift(tc->y0, co->nl - r + 1);
        int w = rs->x1 - rs->x0, h = rs->y1 - rs->y0;
        // Invert rows, then columns (F.3).
        for (int y = 0; y < h; y++) {
            int lo = 0, hi = lw;
            for (int x = 0; x < w; x++) line[x] = tc->plane[(size_t)y * stride + (((x + rs->x0) & 1) ? hi++ : lo++)];
            inverse_line(line, w, rs->x0, co->reversible);
            memcpy(tc->plane + (size_t)y * stride, line, (size_t)w * sizeof(double));
        }
        inverse_columns(tc->plane, stride, w, h, rs->y0, lh, co->reversible, line);
    }
    jfree(j, line); return 0;
}
static int tile_decode(Jpx *j, Tile *t) {
    size_t count = 0, samples = 0;
    for (int c = 0; c < j->nc; c++) {
        Component *tc = t->comp + c;
        if (t->coding[c].nl < j->reduce) return -1;
        size_t n = (size_t)(tc->x1 - tc->x0) * (tc->y1 - tc->y0);
        if (n && !(tc->plane = jalloc(j, n * sizeof(double)))) return -1;
        samples += n;
        for (int r = 0; r <= t->coding[c].nl - j->reduce; r++) {
            Resolution *rs = tc->res + r;
            for (int p = 0; p < rs->nx * rs->ny; p++) for (int b = 0; b < (r ? 3 : 1); b++) {
                Band *band = rs->prec[p].band + b;
                for (int i = 0; i < band->nx * band->ny; i++) if (band->cb[i].passes) count++;
            }
        }
    }
    BlockJob *jobs = jalloc(j, count * sizeof(BlockJob));
    if (!jobs) return -1;
    size_t at = 0;
    for (int c = 0; c < j->nc; c++) {
        Component *tc = t->comp + c; const Coding *co = t->coding + c;
        int stride = tc->x1 - tc->x0;
        for (int r = 0; r <= co->nl - j->reduce; r++) {
            Resolution *rs = tc->res + r;
            int lw = r ? cshift(tc->x1, co->nl - r + 1) - cshift(tc->x0, co->nl - r + 1) : 0;
            int lh = r ? cshift(tc->y1, co->nl - r + 1) - cshift(tc->y0, co->nl - r + 1) : 0;
            for (int p = 0; p < rs->nx * rs->ny; p++) for (int b = 0; b < (r ? 3 : 1); b++) {
                Band *band = rs->prec[p].band + b;
                int n = co->nl - r + (r != 0), bx = band->orient & 1, by = band->orient >> 1;
                int x0 = cshift(tc->x0 - (bx ? 1 << (n - 1) : 0), n);
                int y0 = cshift(tc->y0 - (by ? 1 << (n - 1) : 0), n);
                for (int i = 0; i < band->nx * band->ny; i++) {
                    Block *cb = band->cb + i;
                    if (cb->passes) jobs[at++] = (BlockJob){cb, band, co, tc->plane, stride,
                                                          cb->x0 - x0 + bx * lw, cb->y0 - y0 + by * lh};
                }
            }
        }
    }
    // Packed sub-bands occupy disjoint rectangles, including those of different resolutions.
    // Decode every code-block before combining resolutions; workers never share coefficient state.
    int error = 0, *err = &error;
    int parallel = !j->serial && samples >= (64u << 10) && count >= 16;
    nitro_body decode = ^(size_t i) {
        const BlockJob *job = jobs + i;
        if (!__atomic_load_n(err, __ATOMIC_RELAXED) &&
            block_decode(j, job->cb, job->band, job->coding, job->plane, job->stride, job->x, job->y))
            __atomic_store_n(err, 1, __ATOMIC_RELAXED);
    };
    if (parallel) nitro_parallel(count, decode);
    else for (size_t i = 0; i < count; i++) decode(i);
    jfree(j, jobs);
    if (error) return -1;
    nitro_body wavelet = ^(size_t c) {
        if (component_wavelet(j, t->comp + c, t->coding + c)) __atomic_store_n(err, 1, __ATOMIC_RELAXED);
    };
    if (parallel && j->nc > 1) nitro_parallel((size_t)j->nc, wavelet);
    else for (int c = 0; c < j->nc; c++) wavelet((size_t)c);
    if (error) return -1;
    // (the samples: the resolution reduce levels below the finest, the plane's top left corner)
    if (t->mct) {
        const Resolution *rs = t->comp[0].res + t->coding[0].nl - j->reduce;
        int stride = t->comp[0].x1 - t->comp[0].x0;
        for (int y = 0; y < rs->y1 - rs->y0; y++) {
            double *a = t->comp[0].plane + (size_t)y * stride, *b = t->comp[1].plane + (size_t)y * stride, *c = t->comp[2].plane + (size_t)y * stride;
            for (int i = 0; i < rs->x1 - rs->x0; i++) {
                double yy = a[i], u = b[i], v = c[i];
                if (t->coding[0].reversible) { b[i] = yy - floor((u + v) / 4); a[i] = v + b[i]; c[i] = u + b[i]; }
                else { a[i] = yy + 1.402 * v; b[i] = yy - 0.34413 * u - 0.71414 * v; c[i] = yy + 1.772 * u; }
            }
        }
    }
    int w = j->oxs - j->ox0;
    for (int c = 0; c < j->nc; c++) {
        Component *tc = t->comp + c;
        const Resolution *rs = tc->res + t->coding[c].nl - j->reduce;
        for (int sy = rs->y0; sy < rs->y1; sy++) for (int sx = rs->x0; sx < rs->x1; sx++) {
            double v = tc->plane[(size_t)(sy - rs->y0) * (tc->x1 - tc->x0) + sx - rs->x0];
            // Both signed and unsigned samples are exposed with an unsigned bias to the caller.
            v += (unsigned)1 << (j->prec[c] - 1);
            double max = ((unsigned)1 << j->prec[c]) - 1;
            if (!isfinite(v)) return -1;
            int x = sx * j->dx[c] - j->ox0, y = sy * j->dy[c] - j->oy0;
            if (x < 0 || y < 0 || x >= w || y >= j->oys - j->oy0) continue;   // (reduced: a sub-sampled component's edge)
            j->output[((size_t)y * w + x) * j->nc + c] = (uint16_t)(v <= 0 ? 0 : v >= max ? max : floor(v + 0.5));
        }
    }
    return 0;
}

static void tile_free(Jpx *j, Tile *t) {
    for (int c = 0; c < j->nc; c++) {
        for (int r = 0; r < LEVELS; r++) {
            Resolution *rs = &t->comp[c].res[r];
            if (!rs->prec) continue;
            for (int p = 0; p < rs->nx * rs->ny; p++) for (int b = 0; b < (r ? 3 : 1); b++) {
                Band *band = rs->prec[p].band + b;
                if (band->cb) for (int i = 0; i < band->nx * band->ny; i++) {
                    Segment *s = band->cb[i].first;
                    while (s) { Segment *next = s->next; jfree(j, s->data); jfree(j, s); s = next; }
                }
                jfree(j, band->cb); jfree(j, band->inc.node); jfree(j, band->zero.node);
            }
            jfree(j, rs->prec); rs->prec = NULL;
        }
        jfree(j, t->comp[c].plane); t->comp[c].plane = NULL;
    }
    jfree(j, t->packets); t->packets = NULL;
}

// --- JP2 boxes and channel mapping (annex I) --------------------------------------------------
static int boxes(Jpx *j, const uint8_t *p, size_t len, const uint8_t **stream, size_t *slen, int header) {
    size_t at = 0;
    while (at < len) {
        if (len - at < 8) return -1;
        const uint8_t *b = p + at; uint64_t n = be32(b); unsigned type = be32(b + 4); size_t head = 8;
        if (n == 1) { if (len - at < 16) return -1; n = (uint64_t)be32(b + 8) << 32 | be32(b + 12); head = 16; }
        else if (!n) n = len - at;
        if (n < head || n > len - at) return -1;
        const uint8_t *d = b + head; size_t size = (size_t)n - head;
        if (type == 0x6a703268 && !header) { if (boxes(j, d, size, stream, slen, 1)) return -1; } // jp2h
        else if (type == 0x6a703263 && !header) { // jp2c
            if (*stream) return -1; *stream = d; *slen = size;
        } else if (header && type == 0x636f6c72) { // colr: PDF's /ColorSpace, when given, wins
            if (size < 3) return -1;
            if (d[0] == 1) { if (size != 7) return -1; if (!j->colorspace) j->colorspace = (int)be32(d + 3); }
            else if (d[0] == 2 && size >= 3 + 128 && !j->colorspace) {
                unsigned cs = be32(d + 3 + 16); // ICC's data colour space; like nitropdf's ICC alternate
                j->colorspace = cs == 0x47524159 ? NJPX_GRAY : cs == 0x52474220 ? NJPX_SRGB : cs == 0x434d594b ? NJPX_CMYK : 0;
            }
        } else if (header && type == 0x70636c72) { if (j->palette) return -1; j->palette = d; j->plen = size; }
        else if (header && type == 0x636d6170) { if (j->cmap) return -1; j->cmap = d; j->mlen = size; }
        else if (header && type == 0x63646566) { if (j->cdef) return -1; j->cdef = d; j->dlen = size; }
        at += (size_t)n;
    }
    return 0;
}
static int channels(Jpx *j, njpx_image *out) {
    int n = j->nc, precision[NJPX_MAX_COMPS], map[NJPX_MAX_COMPS], col[NJPX_MAX_COMPS], order[NJPX_MAX_COMPS];
    size_t coff[NJPX_MAX_COMPS], rowbytes = 0; int entries = 0, columns = 0;
    // In a PDF, /ColorSpace takes precedence over JP2's colour specification, including
    // a palette. /Indexed needs the original indices, not palette-expanded RGB values.
    if (j->raw_colors) {
        if (j->cdef) {
            if (j->dlen < 2 || j->dlen != 2u + 6u * be16(j->cdef)) return -1;
            for (unsigned i = 0; i < be16(j->cdef); i++) {
                const uint8_t *d = j->cdef + 2 + 6 * i;
                unsigned ch = be16(d), type = be16(d + 2);
                if (type != 1 && type != 2) continue;
                if (j->cmap) {
                    if (j->mlen % 4 || ch >= j->mlen / 4 || j->cmap[4 * ch + 2]) return -1;
                    ch = be16(j->cmap + 4 * ch);
                }
                if (ch >= (unsigned)j->nc || be16(d + 4) || j->alpha >= 0) return -1;
                j->alpha = (int)ch; j->premult = type == 2;
            }
        }
        j->palette = j->cmap = j->cdef = NULL;
    }
    if (j->palette) {
        if (j->plen < 3) return -1;
        entries = (int)be16(j->palette); columns = j->palette[2];
        if (!entries || columns < 1 || columns > NJPX_MAX_COMPS || j->plen < 3u + columns) return -1;
        for (int c = 0; c < columns; c++) {
            unsigned b = j->palette[3 + c];
            if ((b & 127) > 15) return -1;
            coff[c] = rowbytes; rowbytes += ((b & 127) + 8) / 8;
        }
        if (j->plen != 3u + columns + (size_t)entries * rowbytes) return -1;
        // Some PDF writers omit CMAP for a single index component. Its only possible
        // palette mapping is component 0 to each column (PDF /ColorSpace still overrides it).
        if (!j->cmap) { if (j->nc != 1) return -1; n = columns; }
    }
    if (j->cmap) {
        if (!j->mlen || j->mlen % 4 || j->mlen / 4 > NJPX_MAX_COMPS) return -1;
        n = (int)(j->mlen / 4);
    }
    for (int c = 0; c < n; c++) {
        order[c] = c; map[c] = j->cmap ? (int)be16(j->cmap + 4 * c) : j->palette ? 0 : c;
        if (map[c] >= j->nc) return -1;
        col[c] = j->palette && !j->cmap ? c : -1;
        if (j->cmap) {
            unsigned type = j->cmap[4 * c + 2], pc = j->cmap[4 * c + 3];
            if (type > 1 || (!type && pc) || (type && (!j->palette || pc >= (unsigned)columns))) return -1;
            if (type) col[c] = (int)pc;
        }
        precision[c] = col[c] < 0 ? j->prec[map[c]] : (j->palette[3 + col[c]] & 127) + 1;
    }
    if (j->cdef) {
        if (j->dlen < 2 || j->dlen != 2u + 6u * be16(j->cdef)) return -1;
        int used[NJPX_MAX_COMPS] = {0}, seen[NJPX_MAX_COMPS] = {0}, alpha = -1;
        for (unsigned i = 0; i < be16(j->cdef); i++) {
            const uint8_t *d = j->cdef + 2 + 6 * i;
            int ch = (int)be16(d), type = (int)be16(d + 2), asoc = (int)be16(d + 4);
            if (ch >= n || seen[ch]++) return -1;
            if (type == 1 || type == 2) {
                if (alpha >= 0 || asoc != 0) return -1; // per-colour opacity cannot be one surface alpha
                alpha = ch; j->premult = type == 2;
            } else if (!type && asoc && asoc != 65535) {
                if (asoc > n || used[asoc - 1]++) return -1;
                order[asoc - 1] = ch;
            } else if (type != 0 && type != 65535) return -1;
        }
        // Opacity is exposed last; colour association order is honoured without duplicates.
        if (alpha >= 0) { order[n - 1] = alpha; j->alpha = n - 1; }
        memset(used, 0, sizeof(used));
        for (int c = 0; c < n; c++) { if (used[order[c]]++) return -1; }
    }
    size_t pixels = (size_t)(j->xs - j->x0) * (j->ys - j->y0);
    if (pixels * n > SAMPLE_LIMIT) return -1;
    int change = n != j->nc;
    for (int c = 0; c < n; c++) if (col[c] >= 0 || map[c] != c || order[c] != c) change = 1;
    if (change) {
        uint16_t *p = jalloc(j, pixels * n * sizeof(uint16_t));
        if (!p) return -1;
        for (size_t i = 0; i < pixels; i++) for (int c = 0; c < n; c++) {
            int k = order[c], column = col[k]; unsigned v = j->output[i * j->nc + map[k]];
            if (column >= 0) {
                if (v >= (unsigned)entries) v = entries - 1;
                const uint8_t *d = j->palette + 3 + columns + v * rowbytes + coff[column];
                int b = precision[k]; v = b > 8 ? be16(d) : *d;
                v &= (1u << b) - 1;
                if (j->palette[3 + column] & 128) v ^= 1u << (b - 1);
            }
            p[i * n + c] = (uint16_t)v;
        }
        jfree(j, j->output); j->output = p;
    }
    out->width = j->xs - j->x0; out->height = j->ys - j->y0; out->ncomp = n;
    for (int c = 0; c < n; c++) out->precision[c] = precision[order[c]];
    out->colorspace = j->colorspace; out->alpha = j->alpha; out->premultiplied = j->premult;
    out->samples = j->output; j->output = NULL;
    return 0;
}

int njpx_decode(const uint8_t *data, size_t len, unsigned flags, njpx_image *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out)); out->alpha = -1;
    if (!data || len < 4 || (flags & ~(NJPX_RAW_COLORS | NJPX_SINGLE_THREAD | NJPX_REDUCE(7)))) return -1;
    Jpx j = {0}; j.alpha = -1; j.raw_colors = (flags & NJPX_RAW_COLORS) != 0; j.serial = (flags & NJPX_SINGLE_THREAD) != 0;
    j.reduce = (int)(flags >> 8 & 7);
    const uint8_t *stream = data; size_t slen = len;
    if (be16(data) != 0xff4f) {
        if (len < 12 || be32(data) != 12 || be32(data + 4) != 0x6a502020 || be32(data + 8) != 0x0d0a870a) return -1;
        stream = NULL;
        if (boxes(&j, data, len, &stream, &slen, 0) || !stream || slen < 4) return -1;
    }
    if (be16(stream) != 0xff4f) return -1;
    const uint8_t *p = stream + 2, *end = stream + slen;
    int rc = -1, eoc = 0;
    while ((size_t)(end - p) >= 2) {
        const uint8_t *start = p; unsigned m = be16(p); p += 2;
        if (m == 0xffd9) { eoc = 1; break; }
        if ((size_t)(end - p) < 2) goto done;
        unsigned n = be16(p); if (n < 2 || n > (size_t)(end - p)) goto done;
        const uint8_t *d = p + 2; p += n;
        if (m != 0xff90) {
            if (j.tiles || marker(&j, NULL, m, d, n - 2)) goto done;
            continue;
        }
        if (!j.siz || !j.layers || n != 10) goto done;
        if (!j.tiles) {
            j.tiles = jalloc(&j, (size_t)j.nx * j.ny * sizeof(Tile));
            j.ox0 = cshift(j.x0, j.reduce), j.oy0 = cshift(j.y0, j.reduce), j.oxs = cshift(j.xs, j.reduce), j.oys = cshift(j.ys, j.reduce);
            j.output = jalloc(&j, (size_t)(j.oxs - j.ox0) * (j.oys - j.oy0) * j.nc * sizeof(uint16_t));
            if (!j.tiles || !j.output) goto done;
        }
        unsigned index = be16(d), part = d[6]; size_t psot = be32(d + 2);
        if (index >= (unsigned)(j.nx * j.ny) || (psot && (psot < 14 || psot > (size_t)(end - start)))) goto done;
        Tile *t = j.tiles + index;
        // Some Adobe writers put 5 in TNsot for six resolution tile-parts. Treat the
        // declared count as advisory; sequence numbers, lengths, packets and EOC still check.
        if (t->done || part != (unsigned)t->parts) goto done;
        if (!t->parts) {
            t->x0 = imax(j.x0, j.tx0 + (index % j.nx) * j.tw); t->x1 = imin(j.xs, j.tx0 + (index % j.nx + 1) * j.tw);
            t->y0 = imax(j.y0, j.ty0 + (index / j.nx) * j.th); t->y1 = imin(j.ys, j.ty0 + (index / j.nx + 1) * j.th);
            t->layers = j.layers; t->order = j.order; t->mct = j.mct; t->flags = j.flags;
            memcpy(t->coding, j.coding, sizeof(j.coding));
        }
        const uint8_t *partend = psot ? start + psot : end - 2;
        if (partend < p) goto done;
        while ((size_t)(partend - p) >= 2 && be16(p) != 0xff93) {
            unsigned tm = be16(p); p += 2;
            if ((size_t)(partend - p) < 2) goto done;
            unsigned tn = be16(p); if (tn < 2 || tn > (size_t)(partend - p)) goto done;
            if (t->started && tm != 0xff64 && tm != 0xff58) goto done;
            if (marker(&j, t, tm, p + 2, tn - 2)) goto done;
            p += tn;
        }
        if ((size_t)(partend - p) < 2 || be16(p) != 0xff93) goto done;
        p += 2;
        if (!t->started && tile_new(&j, t)) goto done;
        while (p < partend) {
            if (t->next >= t->npackets || packet(&j, t, &t->packets[t->next++], &p, partend)) goto done;
        }
        t->parts++;
        if (t->next == t->npackets) {
            if (tile_decode(&j, t)) goto done;
            tile_free(&j, t); t->done = 1;
        }
    }
    if (!eoc || !j.tiles) goto done;
    for (int i = 0; i < j.nx * j.ny; i++) if (!j.tiles[i].done) goto done;
    j.x0 = j.ox0, j.y0 = j.oy0, j.xs = j.oxs, j.ys = j.oys;   // (from here the output's grid)
    // Expand sub-sampled components only after all tiles have supplied their grid points:
    // a pixel just across an odd tile boundary can use a sample from the preceding tile.
    for (int c = 0; c < j.nc; c++) {
        if (j.dx[c] == 1 && j.dy[c] == 1) continue;
        int cx0 = cdiv(j.x0, j.dx[c]), cy0 = cdiv(j.y0, j.dy[c]), w = j.xs - j.x0;
        for (int y = j.y0; y < j.ys; y++) for (int x = j.x0; x < j.xs; x++) {
            int sx = imax(cx0, x / j.dx[c]) * j.dx[c] - j.x0;
            int sy = imax(cy0, y / j.dy[c]) * j.dy[c] - j.y0;
            j.output[((size_t)(y - j.y0) * w + x - j.x0) * j.nc + c] = j.output[((size_t)sy * w + sx) * j.nc + c];
        }
    }
    rc = channels(&j, out);
done:
    if (j.tiles) for (int i = 0; i < j.nx * j.ny; i++) tile_free(&j, j.tiles + i);
    jfree(&j, j.tiles); jfree(&j, j.output);
    return rc;
}
void njpx_free(njpx_image *image) {
    if (image) { if (image->samples) free((size_t *)image->samples - 1); memset(image, 0, sizeof(*image)); image->alpha = -1; }
}
