// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitroiw44: see nitroiw44.h. Sections of the DjVu Reference (appendix 1) in brackets. Where its
// text and the files disagree, the files are followed (checked against DjVuLibre's pictures, which
// these are bit for bit): the context of a bucket's decision counts the coefficients that are not
// 0 [10.5.3 says those that are], the odd samples' prediction is the interpolating one [10.6.4
// prints the lifting step's formula for it again], a newly active coefficient's value [10.5.4].
#include "nitroiw44.h"
#include "nitro_zp.h"
#include <stdlib.h>
#include <string.h>

#define IW_MAX_SIDE 16384
#define IW_MAX_PIXELS (64u << 20)

// A colour component: a block's 1024 coefficients one after the other (6 fractional bits), the
// step sizes its coefficients are known to [10.4.2], the band its next slice refines, its contexts.
typedef struct {
    int16_t *co;
    uint8_t *known;   // for each block's 64 buckets: how many of its 16 coefficients are not 0
    uint32_t step[16];
    int band;
    uint8_t c_block, c_bucket[10][8], c_activate[16], c_increase;
} Comp;

struct niw44 {
    int w, h, bw, bh;   // the picture; its blocks of 32 x 32 across and up
    int ncomp, delay;   // 1 or 3; slices to go before the chroma's first [10.4.1.1]
    int chunks;         // the serial number the next chunk has
    Comp c[3];
};

// The initial step sizes [table 4]; the step size a coefficient of band 0 goes by [table 3: for
// the bands above, entry 6 + band]; a band's first bucket and how many it has [table 2].
static const uint32_t STEP0[16] = {0x04000, 0x08000, 0x08000, 0x10000, 0x10000, 0x10000, 0x20000, 0x20000,
                                   0x20000, 0x40000, 0x40000, 0x40000, 0x80000, 0x40000, 0x40000, 0x80000};
static const uint8_t LOW_STEP[16] = {0, 1, 2, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6};
static const uint8_t BAND_FIRST[10] = {0, 1, 2, 3, 4, 8, 12, 16, 32, 48}, BAND_BUCKETS[10] = {1, 1, 1, 1, 4, 4, 4, 16, 16, 16};

enum { ACTIVE = 1, POTENTIAL = 2, DECODE = 4 };

niw44 *niw44_new(void) { return calloc(1, sizeof(niw44)); }
void niw44_free(niw44 *d) {
    if (!d) return;
    for (int k = 0; k < 3; k++) free(d->c[k].co), free(d->c[k].known);
    free(d);
}
int niw44_size(const niw44 *d, int *w, int *h) {
    if (!d->chunks) return 0;
    *w = d->w, *h = d->h;
    return d->ncomp;
}

// A block's coefficients of one band refined [10.5]: flags from what is known so far, then the four
// passes. known: the block's counts (a bucket with none, in a band above 0, needs no look at its
// coefficients: most of a picture's buckets stay so).
static void block_band(Zp *z, Comp *c, int band, int16_t *co, uint8_t *known) {
    int first = BAND_FIRST[band], nb = BAND_BUCKETS[band];
    uint32_t high = c->step[6 + band];
    uint8_t bf[16];
    uint16_t was[16];   // a bucket's coefficients that were active as the band began (a bit each)
    unsigned all = 0;
    // [10.5.1] a coefficient with a step size in use: active (not 0) or potential (0, may become so)
    for (int b = 0; b < nb; b++) {
        unsigned f = 0, n = known[first + b];
        if (band) f = (n ? ACTIVE : 0) | (n < 16 ? POTENTIAL : 0);
        else {
            const int16_t *q = co;
            for (int i = 0; i < 16; i++) {
                uint32_t s = c->step[LOW_STEP[i]];
                if (s && s < 0x8000) f |= q[i] ? ACTIVE : POTENTIAL;
            }
        }
        bf[b] = (uint8_t)f, all |= f;
    }
    // [10.5.2] a band of 16 buckets with nothing active in the block: one decision for all of it
    int buckets = 1;
    if (nb == 16 && !(all & ACTIVE)) buckets = all & POTENTIAL ? zp_decode(z, &c->c_block) : 0;
    if (buckets) {
        // [10.5.3] the buckets that may get new coefficients: by how many of the four coefficients a
        // bucket hangs from (4 x its number: in a band below) are not 0, and whether the block has any
        for (int b = 0; b < nb; b++) {
            if (!(bf[b] & POTENTIAL)) continue;
            int n = 0;
            if (band) {
                const int16_t *q = co + (first + b) * 4;
                n = (q[0] != 0) + (q[1] != 0) + (q[2] != 0);
                if (n < 3 && q[3]) n++;
            }
            if (zp_decode(z, &c->c_bucket[band][n + (all & ACTIVE ? 4 : 0)])) bf[b] |= DECODE;
        }
        // [10.5.4] the coefficients that become active, with their sign. The reference puts one at 3/2
        // of the step size, the middle of its interval; DjVuLibre at 11/8 (small values are the likelier),
        // moved to the middle when its next bit comes [10.5.5]: done the same here, so that a picture is
        // bit for bit what that decoder shows
        for (int b = 0; b < nb; b++) {
            if (!(bf[b] & DECODE)) continue;
            int16_t *q = co + (first + b) * 16;
            unsigned active = 0, potential = 0;
            int np = 0;
            for (int i = 0; i < 16; i++) {
                uint32_t s = band ? high : c->step[LOW_STEP[i]];
                if (!s || s >= 0x8000) continue;
                if (q[i]) active |= 1u << i;
                else potential |= 1u << i, np++;
            }
            was[b] = (uint16_t)active;
            for (int i = 0; i < 16; i++) {
                if (!(potential >> i & 1)) continue;
                if (zp_decode(z, &c->c_activate[(np < 7 ? np : 7) + (bf[b] & ACTIVE ? 8 : 0)])) {
                    uint32_t s = band ? high : c->step[LOW_STEP[i]];
                    int v = (int)(s + (s >> 1) - (s >> 3));
                    q[i] = (int16_t)(zp_raw(z) ? -v : v);
                    known[first + b] += v != 0;
                    np = 0;
                } else if (np > 0)
                    np--;
            }
        }
    }
    // [10.5.5] the active ones: half a step up or down (their second bit with a context, the rest without)
    if (all & ACTIVE)
        for (int b = 0; b < nb; b++) {
            if (!(bf[b] & ACTIVE)) continue;
            int16_t *q = co + (first + b) * 16;
            int decoded = buckets && (bf[b] & DECODE);
            for (int i = 0; i < 16; i++) {
                uint32_t s = band ? high : c->step[LOW_STEP[i]];
                if (decoded ? !(was[b] >> i & 1) : !q[i] || !s || s >= 0x8000) continue;
                int m = q[i] < 0 ? -q[i] : q[i], up;
                if ((uint32_t)m <= 3 * s) m += (int)(s >> 2), up = zp_decode(z, &c->c_increase);
                else up = zp_raw(z);
                m += up ? (int)(s >> 1) : (int)(s >> 1) - (int)s;
                if (m > 32767) m = 32767;
                q[i] = (int16_t)(q[i] < 0 ? -m : m);
                known[first + b] -= !m;
            }
        }
}

// A component's part of a slice: its current band in every block, the bottom row's first, left to
// right [10.4.1]; then that band's step sizes halved and the next band is due [10.4.2.2].
static void slice(niw44 *d, Zp *z, Comp *c) {
    int band = c->band, any = 0;
    if (band) any = c->step[6 + band] && c->step[6 + band] < 0x8000;
    else
        for (int i = 0; i < 7; i++) any |= c->step[i] && c->step[i] < 0x8000;
    if (any) {
        size_t blocks = (size_t)d->bw * (size_t)d->bh;
        for (size_t b = 0; b < blocks && z->over <= ZP_PAST; b++) block_band(z, c, band, c->co + b * 1024, c->known + b * 64);
    }
    if (band) c->step[6 + band] >>= 1;
    else
        for (int i = 0; i < 7; i++) c->step[i] >>= 1;
    c->band = band == 9 ? 0 : band + 1;
}

int niw44_chunk(niw44 *d, const uint8_t *p, size_t len) {
    if (len < 2 || p[0] != (uint8_t)d->chunks || (d->chunks > 255)) return -1;
    int slices = p[1];
    size_t at = 2;
    if (!d->chunks) {   // [10.3] the first one: the version, the kind, the size, the chroma's delay
        if (len < 8) return -1;
        int major = p[2], minor = p[3], w = p[4] << 8 | p[5], h = p[6] << 8 | p[7];
        at = 8;
        if ((major & 0x7f) != 1 || w < 1 || h < 1 || w > IW_MAX_SIDE || h > IW_MAX_SIDE || (size_t)w * (size_t)h > IW_MAX_PIXELS) return -1;
        int delay = 0;
        if (minor >= 2) {   // (the files have it from version 1.2 on)
            if (len < 9) return -1;
            delay = p[8] & 0x7f, at = 9;
        }
        int ncomp = major & 0x80 ? 1 : 3, bw = (w + 31) / 32, bh = (h + 31) / 32;
        int16_t *co[3] = {NULL, NULL, NULL};
        uint8_t *known[3] = {NULL, NULL, NULL};
        int ok = 1;
        for (int k = 0; k < ncomp; k++) {
            co[k] = calloc((size_t)bw * (size_t)bh, 1024 * sizeof(int16_t));
            known[k] = calloc((size_t)bw * (size_t)bh, 64);
            ok &= co[k] && known[k];
        }
        if (!ok) {
            for (int k = 0; k < 3; k++) free(co[k]), free(known[k]);
            return -2;
        }
        d->w = w, d->h = h, d->bw = bw, d->bh = bh, d->ncomp = ncomp, d->delay = delay;
        for (int k = 0; k < ncomp; k++) {
            d->c[k].co = co[k], d->c[k].known = known[k];
            memcpy(d->c[k].step, STEP0, sizeof STEP0);
        }
    }
    d->chunks++;
    Zp z;
    zp_init(&z, p + at, len - at);
    for (int s = 0; s < slices && z.over <= ZP_PAST; s++) {
        slice(d, &z, &d->c[0]);
        if (d->ncomp == 3 && !d->delay) slice(d, &z, &d->c[1]), slice(d, &z, &d->c[2]);
        if (d->delay) d->delay--;
    }
    return 0;
}

// The inverse transform at the scale s [10.6.4]: every s-th row and column takes part; first down
// the columns, then along the rows. In each direction lifting (the even samples lose a weighted
// sum of their odd neighbours: -1 9 9 -1 over 32; one beyond an end counts as 0), then prediction
// (the odd ones gain the even ones' interpolation: -1 9 9 -1 over 16 of the four around; near an
// end the mean of the two beside it, or the one). The columns are done a row at a time (zero: a
// row of zeros for the neighbours beyond the ends).
static void inverse(int16_t *p, int w, int h, int s, const int16_t *zero) {
    int kmax = (h - 1) / s;
    size_t st = (size_t)w * (size_t)s;
    for (int k = 0; k <= kmax; k += 2) {
        int16_t *c = p + (size_t)k * st;
        const int16_t *m1 = k >= 1 ? c - st : zero, *p1 = k + 1 <= kmax ? c + st : zero;
        const int16_t *m3 = k >= 3 ? c - 3 * st : zero, *p3 = k + 3 <= kmax ? c + 3 * st : zero;
        for (int x = 0; x < w; x += s) c[x] = (int16_t)(c[x] - ((9 * (m1[x] + p1[x]) - (m3[x] + p3[x]) + 16) >> 5));
    }
    for (int k = 1; k <= kmax; k += 2) {
        int16_t *c = p + (size_t)k * st;
        const int16_t *m1 = c - st;
        if (k >= 3 && k + 3 <= kmax) {
            const int16_t *p1 = c + st, *m3 = c - 3 * st, *p3 = c + 3 * st;
            for (int x = 0; x < w; x += s) c[x] = (int16_t)(c[x] + ((9 * (m1[x] + p1[x]) - (m3[x] + p3[x]) + 8) >> 4));
        } else if (k + 1 <= kmax) {
            const int16_t *p1 = c + st;
            for (int x = 0; x < w; x += s) c[x] = (int16_t)(c[x] + ((m1[x] + p1[x] + 1) >> 1));
        } else
            for (int x = 0; x < w; x += s) c[x] = (int16_t)(c[x] + m1[x]);
    }
    kmax = (w - 1) / s;
    for (int y = 0; y < h; y += s) {
        int16_t *c = p + (size_t)y * (size_t)w;
        for (int k = 0; k <= kmax; k += 2) {
            int a = (k >= 1 ? c[(k - 1) * s] : 0) + (k + 1 <= kmax ? c[(k + 1) * s] : 0);
            int b = (k >= 3 ? c[(k - 3) * s] : 0) + (k + 3 <= kmax ? c[(k + 3) * s] : 0);
            c[k * s] = (int16_t)(c[k * s] - ((9 * a - b + 16) >> 5));
        }
        for (int k = 1; k <= kmax; k += 2) {
            if (k >= 3 && k + 3 <= kmax)
                c[k * s] = (int16_t)(c[k * s] + ((9 * (c[(k - 1) * s] + c[(k + 1) * s]) - (c[(k - 3) * s] + c[(k + 3) * s]) + 8) >> 4));
            else if (k + 1 <= kmax)
                c[k * s] = (int16_t)(c[k * s] + ((c[(k - 1) * s] + c[(k + 1) * s] + 1) >> 1));
            else
                c[k * s] = (int16_t)(c[k * s] + c[(k - 1) * s]);
        }
    }
}

int niw44_planes(const niw44 *d, uint8_t *y, uint8_t *cb, uint8_t *cr, size_t pitch) {
    if (!d->chunks) return -1;
    int w = d->w, h = d->h;
    // where a block's coefficient goes in its 32 x 32 square [10.6.3]: the index's odd bits are the
    // row's (the lowest of them the row's highest), its even bits the column's
    uint8_t row[1024], col[1024];
    for (int i = 0; i < 1024; i++) {
        row[i] = (uint8_t)((i >> 1 & 1) << 4 | (i >> 3 & 1) << 3 | (i >> 5 & 1) << 2 | (i >> 7 & 1) << 1 | (i >> 9 & 1));
        col[i] = (uint8_t)((i & 1) << 4 | (i >> 2 & 1) << 3 | (i >> 4 & 1) << 2 | (i >> 6 & 1) << 1 | (i >> 8 & 1));
    }
    int16_t *a = malloc((size_t)w * (size_t)h * sizeof *a);   // (rows from the bottom, as the blocks are)
    int16_t *zero = calloc((size_t)w, sizeof *zero);
    if (!a || !zero) {
        free(a), free(zero);
        return -1;
    }
    uint8_t *out[3] = {y, cb, cr};
    for (int k = 0; k < d->ncomp; k++) {
        const int16_t *co = d->c[k].co;
        const uint8_t *known = d->c[k].known;
        memset(a, 0, (size_t)w * (size_t)h * sizeof *a);
        for (int by = 0; by < d->bh; by++)
            for (int bx = 0; bx < d->bw; bx++, co += 1024, known += 64)
                for (int b = 0; b < 64; b++) {
                    if (!known[b]) continue;   // (a bucket of zeros)
                    for (int i = b * 16; i < b * 16 + 16; i++) {
                        int px = bx * 32 + col[i], py = by * 32 + row[i];
                        if (px < w && py < h) a[(size_t)py * (size_t)w + (size_t)px] = co[i];
                    }
                }
        for (int s = 16; s >= 1; s >>= 1) inverse(a, w, h, s, zero);
        // [10.6.5, 10.6.6] to 8 bits: Y 0 .. 255, the chroma around 128; a grey picture is stored inverted
        for (int j = 0; j < h; j++) {
            const int16_t *src = a + (size_t)(h - 1 - j) * (size_t)w;
            uint8_t *dst = out[k] + (size_t)j * pitch;
            for (int i = 0; i < w; i++) {
                int v = (src[i] + 32) >> 6;
                if (v < -128) v = -128;
                if (v > 127) v = 127;
                dst[i] = (uint8_t)(d->ncomp == 1 ? 127 - v : v + 128);
            }
        }
    }
    free(a), free(zero);
    return 0;
}
