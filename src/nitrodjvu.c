// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodjvu: see nitrodjvu.h. Sections of the DjVu Reference in brackets. Where the reference's
// text leaves something out (the two coding templates' figures, the relative locations' rules) the
// code says so: those follow what the files themselves decode to.
#include "nitrodjvu.h"
#include "nitro_bilevel.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// --- the Z'-coder (appendix 3) --------------------------------------------------------------------

// Its table 9: the interval's reduction for a more probable symbol, the threshold above which one
// moves the state on, and the next state after a more and after a less probable symbol.
static const struct { uint16_t p, m; uint8_t up, dn; } ZP[251] = {
    {0x8000, 0x0000,  84, 145}, {0x8000, 0x0000,   3,   4}, {0x8000, 0x0000,   4,   3}, {0x6bbd, 0x10a5,   5,   1},
    {0x6bbd, 0x10a5,   6,   2}, {0x5d45, 0x1f28,   7,   3}, {0x5d45, 0x1f28,   8,   4}, {0x51b9, 0x2bd3,   9,   5},
    {0x51b9, 0x2bd3,  10,   6}, {0x4813, 0x36e3,  11,   7}, {0x4813, 0x36e3,  12,   8}, {0x3fd5, 0x408c,  13,   9},
    {0x3fd5, 0x408c,  14,  10}, {0x38b1, 0x48fd,  15,  11}, {0x38b1, 0x48fd,  16,  12}, {0x3275, 0x505d,  17,  13},
    {0x3275, 0x505d,  18,  14}, {0x2cfd, 0x56d0,  19,  15}, {0x2cfd, 0x56d0,  20,  16}, {0x2825, 0x5c71,  21,  17},
    {0x2825, 0x5c71,  22,  18}, {0x23ab, 0x615b,  23,  19}, {0x23ab, 0x615b,  24,  20}, {0x1f87, 0x65a5,  25,  21},
    {0x1f87, 0x65a5,  26,  22}, {0x1bbb, 0x6962,  27,  23}, {0x1bbb, 0x6962,  28,  24}, {0x1845, 0x6ca2,  29,  25},
    {0x1845, 0x6ca2,  30,  26}, {0x1523, 0x6f74,  31,  27}, {0x1523, 0x6f74,  32,  28}, {0x1253, 0x71e6,  33,  29},
    {0x1253, 0x71e6,  34,  30}, {0x0fcf, 0x7404,  35,  31}, {0x0fcf, 0x7404,  36,  32}, {0x0d95, 0x75d6,  37,  33},
    {0x0d95, 0x75d6,  38,  34}, {0x0b9d, 0x7768,  39,  35}, {0x0b9d, 0x7768,  40,  36}, {0x09e3, 0x78c2,  41,  37},
    {0x09e3, 0x78c2,  42,  38}, {0x0861, 0x79ea,  43,  39}, {0x0861, 0x79ea,  44,  40}, {0x0711, 0x7ae7,  45,  41},
    {0x0711, 0x7ae7,  46,  42}, {0x05f1, 0x7bbe,  47,  43}, {0x05f1, 0x7bbe,  48,  44}, {0x04f9, 0x7c75,  49,  45},
    {0x04f9, 0x7c75,  50,  46}, {0x0425, 0x7d0f,  51,  47}, {0x0425, 0x7d0f,  52,  48}, {0x0371, 0x7d91,  53,  49},
    {0x0371, 0x7d91,  54,  50}, {0x02d9, 0x7dfe,  55,  51}, {0x02d9, 0x7dfe,  56,  52}, {0x0259, 0x7e5a,  57,  53},
    {0x0259, 0x7e5a,  58,  54}, {0x01ed, 0x7ea6,  59,  55}, {0x01ed, 0x7ea6,  60,  56}, {0x0193, 0x7ee6,  61,  57},
    {0x0193, 0x7ee6,  62,  58}, {0x0149, 0x7f1a,  63,  59}, {0x0149, 0x7f1a,  64,  60}, {0x010b, 0x7f45,  65,  61},
    {0x010b, 0x7f45,  66,  62}, {0x00d5, 0x7f6b,  67,  63}, {0x00d5, 0x7f6b,  68,  64}, {0x00a5, 0x7f8d,  69,  65},
    {0x00a5, 0x7f8d,  70,  66}, {0x007b, 0x7faa,  71,  67}, {0x007b, 0x7faa,  72,  68}, {0x0057, 0x7fc3,  73,  69},
    {0x0057, 0x7fc3,  74,  70}, {0x003b, 0x7fd7,  75,  71}, {0x003b, 0x7fd7,  76,  72}, {0x0023, 0x7fe7,  77,  73},
    {0x0023, 0x7fe7,  78,  74}, {0x0013, 0x7ff2,  79,  75}, {0x0013, 0x7ff2,  80,  76}, {0x0007, 0x7ffa,  81,  77},
    {0x0007, 0x7ffa,  82,  78}, {0x0001, 0x7fff,  81,  79}, {0x0001, 0x7fff,  82,  80}, {0x5695, 0x0000,   9,  85},
    {0x24ee, 0x0000,  86, 226}, {0x8000, 0x0000,   5,   6}, {0x0d30, 0x0000,  88, 176}, {0x481a, 0x0000,  89, 143},
    {0x0481, 0x0000,  90, 138}, {0x3579, 0x0000,  91, 141}, {0x017a, 0x0000,  92, 112}, {0x24ef, 0x0000,  93, 135},
    {0x007b, 0x0000,  94, 104}, {0x1978, 0x0000,  95, 133}, {0x0028, 0x0000,  96, 100}, {0x10ca, 0x0000,  97, 129},
    {0x000d, 0x0000,  82,  98}, {0x0b5d, 0x0000,  99, 127}, {0x0034, 0x0000,  76,  72}, {0x078a, 0x0000, 101, 125},
    {0x00a0, 0x0000,  70, 102}, {0x050f, 0x0000, 103, 123}, {0x0117, 0x0000,  66,  60}, {0x0358, 0x0000, 105, 121},
    {0x01ea, 0x0000, 106, 110}, {0x0234, 0x0000, 107, 119}, {0x0144, 0x0000,  66, 108}, {0x0173, 0x0000, 109, 117},
    {0x0234, 0x0000,  60,  54}, {0x00f5, 0x0000, 111, 115}, {0x0353, 0x0000,  56,  48}, {0x00a1, 0x0000,  69, 113},
    {0x05c5, 0x0000, 114, 134}, {0x011a, 0x0000,  65,  59}, {0x03cf, 0x0000, 116, 132}, {0x01aa, 0x0000,  61,  55},
    {0x0285, 0x0000, 118, 130}, {0x0286, 0x0000,  57,  51}, {0x01ab, 0x0000, 120, 128}, {0x03d3, 0x0000,  53,  47},
    {0x011a, 0x0000, 122, 126}, {0x05c5, 0x0000,  49,  41}, {0x00ba, 0x0000, 124,  62}, {0x08ad, 0x0000,  43,  37},
    {0x007a, 0x0000,  72,  66}, {0x0ccc, 0x0000,  39,  31}, {0x01eb, 0x0000,  60,  54}, {0x1302, 0x0000,  33,  25},
    {0x02e6, 0x0000,  56,  50}, {0x1b81, 0x0000,  29, 131}, {0x045e, 0x0000,  52,  46}, {0x24ef, 0x0000,  23,  17},
    {0x0690, 0x0000,  48,  40}, {0x2865, 0x0000,  23,  15}, {0x09de, 0x0000,  42, 136}, {0x3987, 0x0000, 137,   7},
    {0x0dc8, 0x0000,  38,  32}, {0x2c99, 0x0000,  21, 139}, {0x10ca, 0x0000, 140, 172}, {0x3b5f, 0x0000,  15,   9},
    {0x0b5d, 0x0000, 142, 170}, {0x5695, 0x0000,   9,  85}, {0x078a, 0x0000, 144, 168}, {0x8000, 0x0000, 141, 248},
    {0x050f, 0x0000, 146, 166}, {0x24ee, 0x0000, 147, 247}, {0x0358, 0x0000, 148, 164}, {0x0d30, 0x0000, 149, 197},
    {0x0234, 0x0000, 150, 162}, {0x0481, 0x0000, 151,  95}, {0x0173, 0x0000, 152, 160}, {0x017a, 0x0000, 153, 173},
    {0x00f5, 0x0000, 154, 158}, {0x007b, 0x0000, 155, 165}, {0x00a1, 0x0000,  70, 156}, {0x0028, 0x0000, 157, 161},
    {0x011a, 0x0000,  66,  60}, {0x000d, 0x0000,  81, 159}, {0x01aa, 0x0000,  62,  56}, {0x0034, 0x0000,  75,  71},
    {0x0286, 0x0000,  58,  52}, {0x00a0, 0x0000,  69, 163}, {0x03d3, 0x0000,  54,  48}, {0x0117, 0x0000,  65,  59},
    {0x05c5, 0x0000,  50,  42}, {0x01ea, 0x0000, 167, 171}, {0x08ad, 0x0000,  44,  38}, {0x0144, 0x0000,  65, 169},
    {0x0ccc, 0x0000,  40,  32}, {0x0234, 0x0000,  59,  53}, {0x1302, 0x0000,  34,  26}, {0x0353, 0x0000,  55,  47},
    {0x1b81, 0x0000,  30, 174}, {0x05c5, 0x0000, 175, 193}, {0x24ef, 0x0000,  24,  18}, {0x03cf, 0x0000, 177, 191},
    {0x2b74, 0x0000, 178, 222}, {0x0285, 0x0000, 179, 189}, {0x201d, 0x0000, 180, 218}, {0x01ab, 0x0000, 181, 187},
    {0x1715, 0x0000, 182, 216}, {0x011a, 0x0000, 183, 185}, {0x0fb7, 0x0000, 184, 214}, {0x00ba, 0x0000,  69,  61},
    {0x0a67, 0x0000, 186, 212}, {0x01eb, 0x0000,  59,  53}, {0x06e7, 0x0000, 188, 210}, {0x02e6, 0x0000,  55,  49},
    {0x0496, 0x0000, 190, 208}, {0x045e, 0x0000,  51,  45}, {0x030d, 0x0000, 192, 206}, {0x0690, 0x0000,  47,  39},
    {0x0206, 0x0000, 194, 204}, {0x09de, 0x0000,  41, 195}, {0x0155, 0x0000, 196, 202}, {0x0dc8, 0x0000,  37,  31},
    {0x00e1, 0x0000, 198, 200}, {0x2b74, 0x0000, 199, 243}, {0x0094, 0x0000,  72,  64}, {0x201d, 0x0000, 201, 239},
    {0x0188, 0x0000,  62,  56}, {0x1715, 0x0000, 203, 237}, {0x0252, 0x0000,  58,  52}, {0x0fb7, 0x0000, 205, 235},
    {0x0383, 0x0000,  54,  48}, {0x0a67, 0x0000, 207, 233}, {0x0547, 0x0000,  50,  44}, {0x06e7, 0x0000, 209, 231},
    {0x07e2, 0x0000,  46,  38}, {0x0496, 0x0000, 211, 229}, {0x0bc0, 0x0000,  40,  34}, {0x030d, 0x0000, 213, 227},
    {0x1178, 0x0000,  36,  28}, {0x0206, 0x0000, 215, 225}, {0x19da, 0x0000,  30,  22}, {0x0155, 0x0000, 217, 223},
    {0x24ef, 0x0000,  26,  16}, {0x00e1, 0x0000, 219, 221}, {0x320e, 0x0000,  20, 220}, {0x0094, 0x0000,  71,  63},
    {0x432a, 0x0000,  14,   8}, {0x0188, 0x0000,  61,  55}, {0x447d, 0x0000,  14, 224}, {0x0252, 0x0000,  57,  51},
    {0x5ece, 0x0000,   8,   2}, {0x0383, 0x0000,  53,  47}, {0x8000, 0x0000, 228,  87}, {0x0547, 0x0000,  49,  43},
    {0x481a, 0x0000, 230, 246}, {0x07e2, 0x0000,  45,  37}, {0x3579, 0x0000, 232, 244}, {0x0bc0, 0x0000,  39,  33},
    {0x24ef, 0x0000, 234, 238}, {0x1178, 0x0000,  35,  27}, {0x1978, 0x0000, 138, 236}, {0x19da, 0x0000,  29,  21},
    {0x2865, 0x0000,  24,  16}, {0x24ef, 0x0000,  25,  15}, {0x3987, 0x0000, 240,   8}, {0x320e, 0x0000,  19, 241},
    {0x2c99, 0x0000,  22, 242}, {0x432a, 0x0000,  13,   7}, {0x3b5f, 0x0000,  16,  10}, {0x447d, 0x0000,  13, 245},
    {0x5695, 0x0000,  10,   2}, {0x5ece, 0x0000,   7,   1}, {0x8000, 0x0000, 244,  83}, {0x8000, 0x0000, 249, 250},
    {0x5695, 0x0000,  10,   2}, {0x481a, 0x0000,  89, 143}, {0x481a, 0x0000, 230, 246},
};

#define ZP_PAST 32   // bytes read past the end before a stream counts as cut short (its last decisions take them as 0xff)

typedef struct {
    const uint8_t *p, *end;
    uint32_t a, c, fence;   // (fence: below it a more probable symbol needs no look at c)
    uint8_t byte;
    int nbits, over;        // bits left of byte; the bytes read past the end (they read as 0xff)
} Zp;

static inline unsigned zp_bit(Zp *z) {
    if (!z->nbits) {
        if (z->p < z->end) z->byte = *z->p++;
        else z->byte = 0xff, z->over++;
        z->nbits = 8;
    }
    return z->byte >> --z->nbits & 1;
}
static void zp_init(Zp *z, const uint8_t *p, size_t len) {
    memset(z, 0, sizeof *z);
    z->p = p, z->end = p + len;
    for (int k = 0; k < 16; k++) z->c = z->c << 1 | zp_bit(z);
    z->fence = z->c >= 0x8000 ? 0x7fff : z->c;
}
// (after a less probable symbol: the interval grown back to at least half; after a more probable one, once)
static inline void zp_shift(Zp *z) {
    do {
        z->a = z->a << 1 & 0xffff;
        z->c = (z->c << 1 & 0xffff) | zp_bit(z);
    } while (z->a >= 0x8000);
    z->fence = z->c >= 0x8000 ? 0x7fff : z->c;
}
// A decision in the context k (its state, whose parity is the more probable symbol). The state moves
// on after a less probable symbol, and after a more probable one that fills the interval (figure 1).
static inline int zp_decode(Zp *z, uint8_t *k) {
    unsigned s = *k, zz = z->a + ZP[s].p;
    if (zz <= z->fence) {
        z->a = zz;
        return (int)(s & 1);
    }
    unsigned d = 0x6000 + ((zz + z->a) >> 2);
    if (zz > d) zz = d;
    if (zz > z->c) {
        zz = 0x10000 - zz;
        z->a += zz, z->c += zz;
        *k = ZP[s].dn;
        zp_shift(z);
        return (int)(~s & 1);
    }
    if (z->a >= ZP[s].m) *k = ZP[s].up;
    z->a = zz;
    zp_shift(z);
    return (int)(s & 1);
}
// a decision without a context: both values near enough as likely (figure 2)
static inline int zp_raw(Zp *z) {
    unsigned zz = 0x8000 + ((z->a * 3) >> 3);
    if (zz > z->c) {
        zz = 0x10000 - zz;
        z->a += zz, z->c += zz;
        zp_shift(z);
        return 1;
    }
    z->a = zz;
    zp_shift(z);
    return 0;
}

// --- BZZ (appendix 4): the directory's names ------------------------------------------------------

// a number of bits by a binary tree of contexts (cx[1 .. 2^bits - 1]), or without contexts
static unsigned bzz_bin(Zp *z, uint8_t *cx, int bits) {
    unsigned n = 1, m = 1u << bits;
    while (n < m) n = n << 1 | (unsigned)zp_decode(z, &cx[n]);
    return n - m;
}
static unsigned bzz_raw(Zp *z, int bits) {
    unsigned n = 1, m = 1u << bits;
    while (n < m) n = n << 1 | (unsigned)zp_raw(z);
    return n - m;
}

// The BZZ stream decoded, at most limit bytes (from malloc; NULL: damaged or too long). Blocks, each:
// its size, the speed of the frequency estimate, its symbols as positions in a move-to-front array
// kept in order of an estimate of their frequencies, then the Burrows-Wheeler transform undone.
static uint8_t *bzz_decode(const uint8_t *p, size_t len, size_t limit, size_t *out_len) {
    Zp z;
    zp_init(&z, p, len);
    uint8_t cx[300], *out = NULL, *blk = NULL;
    uint32_t *pos = NULL;
    size_t n = 0, cap = 0;
    memset(cx, 0, sizeof cx);
    for (;;) {
        unsigned size = bzz_raw(&z, 24);
        if (!size) break;   // (the end)
        if (size > (4096u << 10) || z.over > 8) goto fail;
        int fshift = 0;
        if (zp_raw(&z)) fshift = zp_raw(&z) ? 2 : 1;
        uint8_t mtf[256];
        for (int k = 0; k < 256; k++) mtf[k] = (uint8_t)k;
        uint32_t freq[4] = {0, 0, 0, 0}, fadd = 4;
        unsigned mtfno = 3;
        int marker = -1;
        free(blk), free(pos);
        blk = malloc(size), pos = malloc((size_t)size * sizeof *pos);
        if (!blk || !pos) goto fail;
        for (unsigned i = 0; i < size; i++) {
            unsigned id = mtfno < 2 ? mtfno : 2;
            uint8_t *c = cx;
            if (zp_decode(&z, &c[id])) mtfno = 0;
            else if (zp_decode(&z, &c[3 + id])) mtfno = 1;
            else {
                c += 6;
                int bits = 1;
                for (; bits <= 7; bits++) {   // (a group of 2^bits positions: its flag, then which of them)
                    if (zp_decode(&z, &c[0])) break;
                    c += 1u << bits;
                }
                if (bits > 7) {   // the end-of-block symbol: where the transform's thread starts
                    mtfno = 256;
                    blk[i] = 0;
                    marker = (int)i;
                    continue;
                }
                mtfno = (1u << bits) + bzz_bin(&z, c, bits);
            }
            if (z.over > 8) goto fail;
            uint8_t sym = mtf[mtfno];
            blk[i] = sym;
            // the symbol moves towards the front, as far as its frequency estimate takes it
            fadd += fadd >> fshift;
            if (fadd > 0x10000000) {
                fadd >>= 24;
                for (int k = 0; k < 4; k++) freq[k] >>= 24;
            }
            uint32_t fc = fadd + (mtfno < 4 ? freq[mtfno] : 0);
            unsigned k = mtfno;
            for (; k >= 4; k--) mtf[k] = mtf[k - 1];
            for (; k > 0 && fc >= freq[k - 1]; k--) mtf[k] = mtf[k - 1], freq[k] = freq[k - 1];
            mtf[k] = sym, freq[k] = fc;
        }
        if (marker < 1) goto fail;
        // the inverse transform: each symbol's rank among its equals, the start of each symbol's run
        // in the sorted block (the end-of-block symbol first), then the thread followed backwards
        uint32_t count[256] = {0};
        for (unsigned i = 0; i < size; i++) {
            if ((int)i == marker) continue;
            uint8_t c = blk[i];
            pos[i] = (uint32_t)c << 24 | (count[c]++ & 0xffffff);
        }
        uint32_t last = 1;
        for (int k = 0; k < 256; k++) {
            uint32_t t = count[k];
            count[k] = last, last += t;
        }
        if (n + size - 1 > limit) goto fail;
        if (n + size > cap) {
            cap = (n + size) * 2;
            uint8_t *more = realloc(out, cap);
            if (!more) goto fail;
            out = more;
        }
        uint32_t i = 0;
        for (last = size - 1; last > 0;) {
            if (i >= size || (int)i == marker) goto fail;
            uint32_t v = pos[i];
            out[n + --last] = (uint8_t)(v >> 24);
            i = count[v >> 24] + (v & 0xffffff);
        }
        if ((int)i != marker) goto fail;
        n += size - 1;
    }
    free(blk), free(pos);
    if (!out) out = malloc(1);
    *out_len = n;
    return out;
fail:
    free(blk), free(pos), free(out);
    return NULL;
}

// --- JB2 (appendix 2) ------------------------------------------------------------------------------

#define JB2_BIG 262142        // the largest number coded (11.5.2)
#define JB2_MAX_CELLS (1u << 22)
#define JB2_MAX_PIXELS (200u << 20)   // a page, and all of a library's shapes together

// A symbol of the library: its bitmap, and the box of its black pixels in it (columns; rows counted
// from the bottom, as the format's coordinates are: an empty one has right < left, top < bottom).
typedef struct {
    Bm bm;
    int left, right, bottom, top;
} Shape;

typedef struct {   // a decoded shape dictionary (Djbz), shared by the pages that include it
    Shape *shapes;
    int n, inherited;   // (its first inherited shapes are another dictionary's)
} Dict;

typedef struct {
    Zp z;
    // the numbers' contexts: binary trees grown as they are walked (11.2.2); a node's index 0: not there yet
    uint8_t *bit;
    uint32_t *left, *right, ncells, cap;
    uint32_t c_record, c_size, c_match, c_w, c_h, c_dw, c_dh, c_col, c_row, c_hsame, c_vsame, c_hnew, c_vnew, c_clen, c_cbyte, c_dict;
    uint8_t c_refine, c_offset, direct[1024], cross[2048];
    Shape *lib;
    int nlib, caplib, inherited;   // (the first inherited ones are a dictionary's: not freed here)
    size_t pixels;
    int started, width, height, err, old;   // (old: files up to version 18 code a copy's place by its whole bitmap)
    Bm page;
    int have_page;
    // where the last symbols were put (rows from the bottom, 1 the lowest: 11.3)
    int last_right, last_row_left, last_row_bottom, base[3], base_at;
} Jb2;

static uint32_t jb2_cell(Jb2 *j) {
    if (j->ncells >= j->cap) {
        uint32_t cap = j->cap ? j->cap * 2 : 4096;
        if (cap > JB2_MAX_CELLS) return 0;
        uint8_t *b = realloc(j->bit, cap);
        if (b) j->bit = b;
        uint32_t *l = realloc(j->left, (size_t)cap * sizeof *l);
        if (l) j->left = l;
        uint32_t *r = realloc(j->right, (size_t)cap * sizeof *r);
        if (r) j->right = r;
        if (!b || !l || !r) return 0;
        j->cap = cap;
    }
    uint32_t c = j->ncells++;
    j->bit[c] = 0, j->left[c] = j->right[c] = 0;
    return c;
}

// A number in low .. high by its context's tree (11.2.5): its sign, the range its magnitude is in
// (0, 1-2, 3-6, 7-14 ...), then its bits; a decision its bounds settle is not coded.
static int jb2_num(Jb2 *j, uint32_t *root, int low, int high) {
    int negative = 0, cutoff = 0, phase = 1;
    uint32_t range = 0xffffffffu, *at = root;
    while (range != 1) {
        if (!*at) {
            // (a new node: the arrays may move, so the place it hangs from is found again by its index)
            uint32_t *base_l = j->left, *base_r = j->right;
            int in_left = at >= base_l && at < base_l + j->cap, in_right = at >= base_r && at < base_r + j->cap;
            size_t idx = in_left ? (size_t)(at - base_l) : in_right ? (size_t)(at - base_r) : 0;
            uint32_t c = jb2_cell(j);
            if (!c) {
                j->err = 1;
                return 0;
            }
            if (in_left) at = j->left + idx;
            else if (in_right) at = j->right + idx;
            *at = c;
        }
        uint32_t node = *at;
        int decision = low < cutoff && high >= cutoff ? zp_decode(&j->z, &j->bit[node]) : low >= cutoff;
        at = decision ? &j->right[node] : &j->left[node];
        switch (phase) {
        case 1:
            negative = !decision;
            if (negative) {
                int t = -low - 1;
                low = -high - 1, high = t;
            }
            phase = 2, cutoff = 1;
            break;
        case 2:
            if (!decision) {
                phase = 3;
                range = (uint32_t)(cutoff + 1) / 2;
                if (range == 1) cutoff = 0;
                else cutoff -= (int)(range / 2);
            } else {
                cutoff += cutoff + 1;
                if (cutoff > 2 * JB2_BIG + 2) {   // (damage: beyond every range there is)
                    j->err = 1;
                    return 0;
                }
            }
            break;
        default:
            range /= 2;
            if (range != 1) cutoff += decision ? (int)(range / 2) : -(int)(range / 2);
            else if (!decision) cutoff--;
            break;
        }
    }
    if (j->z.over > ZP_PAST) j->err = 1;
    return negative ? -cutoff - 1 : cutoff;
}

// (a "reset" record: the numbers' trees forgotten; the pixels' contexts stay)
static void jb2_reset(Jb2 *j) {
    j->ncells = 1;   // (0: "no node")
    j->c_record = j->c_size = j->c_match = j->c_w = j->c_h = j->c_dw = j->c_dh = j->c_col = j->c_row = 0;
    j->c_hsame = j->c_vsame = j->c_hnew = j->c_vnew = j->c_clen = j->c_cbyte = j->c_dict = 0;
}

static int shape_new(Jb2 *j, Shape *s, int w, int h) {
    memset(s, 0, sizeof *s);
    if (w < 0 || h < 0 || w > 65535 || h > 65535 || (size_t)w * (size_t)h > JB2_MAX_PIXELS - j->pixels) return -1;
    if (bm_new(&s->bm, w, h)) return -1;
    j->pixels += (size_t)w * (size_t)h + 64;
    return 0;
}

// A bitmap coded on its own (11.5.9), its rows from the top: each pixel in the context of ten
// already coded ones, three in the row two above (x-1 .. x+1), five in the row above (x-2 .. x+2)
// and the two before it. (The reference's figure 2 is not in its text: the template is the one
// the files decode with.)
static void jb2_direct(Jb2 *j, Bm *b) {
    for (int y = 0; y < b->h && j->z.over <= ZP_PAST; y++) {
        uint8_t *r = bm_row(b, y);
        const uint8_t *r1 = bm_row(b, y - 1), *r2 = bm_row(b, y - 2);
        unsigned cx = (unsigned)(r2[0] << 8 | r2[1] << 7 | r1[0] << 4 | r1[1] << 3 | r1[2] << 2);
        for (int x = 0; x < b->w; x++) {
            int v = zp_decode(&j->z, &j->direct[cx]);
            r[x] = (uint8_t)v;
            cx = ((cx << 1) & 0x37a) | (unsigned)(r2[x + 2] << 7 | r1[x + 3] << 2 | v);
        }
    }
}

// A bitmap coded as a refinement of a library shape (11.5.10): each pixel in the context of four of
// its own (the row above: x-1 .. x+1; the one before it) and seven of the shape's, where the two
// lie centre on centre: the pixel above, the three around the same place, the three below. The
// centres: of the bitmap, and of the shape's box of black pixels; of two middle columns the left
// one, of two middle rows the lower one.
static void jb2_cross(Jb2 *j, Bm *b, const Shape *ref) {
    int dw = b->w, dh = b->h, ch = ref->bm.h;
    int xd = (dw / 2 - dw + 1) - ((ref->right - ref->left + 1) / 2 - ref->right);
    int yd = (dh / 2 - dh + 1) - ((ref->top - ref->bottom + 1) / 2 - ref->top);   // (rows from the bottom)
    int ydt = (ch - dh) - yd;   // (the shape's row, from its top, beside the bitmap's row y: y + ydt)
    for (int y = 0; y < dh && j->z.over <= ZP_PAST; y++) {
        uint8_t *r = bm_row(b, y);
        const uint8_t *r1 = bm_row(b, y - 1);
        int ry = y + ydt;
        for (int x = 0; x < dw; x++) {
            int rx = x + xd;
            unsigned cx = (unsigned)(r1[x - 1] << 10 | r1[x] << 9 | r1[x + 1] << 8 | r[x - 1] << 7) | bm_px(&ref->bm, rx, ry - 1) << 6 |
                          bm_px(&ref->bm, rx - 1, ry) << 5 | bm_px(&ref->bm, rx, ry) << 4 | bm_px(&ref->bm, rx + 1, ry) << 3 |
                          bm_px(&ref->bm, rx - 1, ry + 1) << 2 | bm_px(&ref->bm, rx, ry + 1) << 1 | bm_px(&ref->bm, rx + 1, ry + 1);
            r[x] = (uint8_t)zp_decode(&j->z, &j->cross[cx]);
        }
    }
}

// the box of a shape's black pixels
static void shape_box(Shape *s) {
    const Bm *b = &s->bm;
    int l = b->w, r = -1, t = b->h, bt = -1;
    for (int y = 0; y < b->h; y++) {
        const uint8_t *row = bm_row(b, y);
        for (int x = 0; x < b->w; x++)
            if (row[x]) {
                if (x < l) l = x;
                if (x > r) r = x;
                if (y < t) t = y;
                bt = y;
            }
    }
    if (r < 0) s->left = 0, s->right = -1, s->bottom = 0, s->top = -1;
    else s->left = l, s->right = r, s->bottom = b->h - 1 - bt, s->top = b->h - 1 - t;
}

static int jb2_add(Jb2 *j, Shape *s) {
    if (j->nlib == j->caplib) {
        int cap = j->caplib ? j->caplib * 2 : 256;
        Shape *more = realloc(j->lib, (size_t)cap * sizeof *more);
        if (!more) return -1;
        j->lib = more, j->caplib = cap;
    }
    shape_box(s);
    j->lib[j->nlib++] = *s;
    return 0;
}

static int median3(const int *v) {
    int a = v[0], b = v[1], c = v[2];
    return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
}

// A symbol's place relative to those before it (11.5.8; the reference's text ends halfway, the
// rest is as the files are coded): the first of a new line of text by the first of the line before
// (its left edge, and its bottom edge to the new one's top), any other by the one before it (that
// one's right edge, and the line's baseline: the median of the last three bottoms). rows x cols:
// the symbol's; *left, *bottom: its lower left pixel, columns and rows from 1, the rows from the bottom.
static void jb2_place(Jb2 *j, int rows, int cols, int *left, int *bottom) {
    if (zp_decode(&j->z, &j->c_offset)) {
        int dx = jb2_num(j, &j->c_hnew, -JB2_BIG - 1, JB2_BIG), dy = jb2_num(j, &j->c_vnew, -JB2_BIG - 1, JB2_BIG);
        *left = j->last_row_left + dx;
        *bottom = j->last_row_bottom + dy - rows + 1;
        j->last_row_left = *left, j->last_row_bottom = *bottom;
        j->last_right = *left + cols - 1;
        j->base[0] = j->base[1] = j->base[2] = *bottom, j->base_at = 0;
    } else {
        int dx = jb2_num(j, &j->c_hsame, -JB2_BIG - 1, JB2_BIG), dy = jb2_num(j, &j->c_vsame, -JB2_BIG - 1, JB2_BIG);
        *left = j->last_right + dx;
        *bottom = median3(j->base) + dy;
        j->last_right = *left + cols - 1;
        if (++j->base_at == 3) j->base_at = 0;
        j->base[j->base_at] = *bottom;
    }
}

// the shape onto the page, its lower left pixel at left, bottom (from 1; rows from the bottom)
static void jb2_blit(Jb2 *j, const Bm *b, int left, int bottom) {
    if (j->have_page && left > -70000 && left < 70000 && bottom > -70000 && bottom < 70000)
        bm_compose(&j->page, b, left - 1, j->height - (bottom - 1 + b->h), 0);
}

// The records of a JB2 stream (11.4): a page's (page: its picture built) or a dictionary's. dict:
// the shapes it inherits (NULL: none; needed if the stream asks for them). 0: read to its end; -1:
// it stopped before (cut short or damaged: a page has what was drawn until then, j->have_page).
static int jb2_decode(Jb2 *j, const uint8_t *p, size_t len, const Dict *dict, int page) {
    zp_init(&j->z, p, len);
    jb2_reset(j);
    if (!jb2_cell(j)) return -1;   // (node 0)
    for (long records = 0; records < 50000000 && !j->err; records++) {
        int type = jb2_num(j, &j->c_record, 0, 11);
        if (j->err) break;
        if (type == 11) return 0;
        if (type == 0) {   // start of image: its size (a dictionary's: 0 x 0)
            int w = jb2_num(j, &j->c_size, 0, JB2_BIG), h = jb2_num(j, &j->c_size, 0, JB2_BIG);
            zp_decode(&j->z, &j->c_refine);   // (the "eventual refinement" flag: never set)
            if (j->err) break;
            if (j->started) break;   // (a second one: damage)
            if (page) {
                if (w < 1 || h < 1 || w > 65535 || h > 65535 || (size_t)w * (size_t)h > JB2_MAX_PIXELS || bm_new(&j->page, w, h)) break;
                j->have_page = 1;
            }
            j->width = w, j->height = h, j->started = 1;
            j->last_right = j->last_row_left = 0, j->last_row_bottom = h;
            j->base[0] = j->base[1] = j->base[2] = h, j->base_at = 0;
            continue;
        }
        if (type == 9) {   // before the start: the shapes of a shared dictionary; after it: the numbers' contexts reset
            if (j->started) {
                jb2_reset(j);
                if (!jb2_cell(j)) break;
                continue;
            }
            int n = jb2_num(j, &j->c_dict, 0, JB2_BIG);
            if (j->err || n < 0 || j->nlib || (n && (!dict || n > dict->n))) break;
            j->lib = malloc((size_t)(n ? n : 1) * sizeof *j->lib);
            if (!j->lib) break;
            if (n) memcpy(j->lib, dict->shapes, (size_t)n * sizeof *j->lib);
            j->nlib = j->caplib = j->inherited = n;
            continue;
        }
        if (type == 10) {   // a comment
            int n = jb2_num(j, &j->c_clen, 0, JB2_BIG);
            for (int k = 0; k < n && !j->err; k++) jb2_num(j, &j->c_cbyte, 0, 255);
            continue;
        }
        if (!j->started) break;
        Shape s;
        int left = 0, bottom = 0;
        if (type == 7) {   // a library shape as it is: placed by the box of its black pixels
            int m = j->nlib ? jb2_num(j, &j->c_match, 0, j->nlib - 1) : -1;
            if (j->err || m < 0) break;
            const Shape *ref = &j->lib[m];
            if (j->old) jb2_place(j, ref->bm.h, ref->bm.w, &left, &bottom);
            else {
                jb2_place(j, ref->top - ref->bottom + 1, ref->right - ref->left + 1, &left, &bottom);
                left -= ref->left, bottom -= ref->bottom;
            }
            if (j->err) break;
            jb2_blit(j, &ref->bm, left, bottom);
            continue;
        }
        if (type <= 3 || type == 8) {   // a new bitmap, coded on its own
            int w = jb2_num(j, &j->c_w, 0, JB2_BIG), h = jb2_num(j, &j->c_h, 0, JB2_BIG);
            if (j->err || shape_new(j, &s, w, h)) break;
            jb2_direct(j, &s.bm);
        } else {   // (4, 5, 6) a refinement of a library shape: its size by that shape's box
            int m = j->nlib ? jb2_num(j, &j->c_match, 0, j->nlib - 1) : -1;
            if (j->err || m < 0) break;
            const Shape *ref = &j->lib[m];
            int w = ref->right - ref->left + 1 + jb2_num(j, &j->c_dw, -JB2_BIG - 1, JB2_BIG);
            int h = ref->top - ref->bottom + 1 + jb2_num(j, &j->c_dh, -JB2_BIG - 1, JB2_BIG);
            if (j->err || shape_new(j, &s, w, h)) break;
            jb2_cross(j, &s.bm, &j->lib[m]);
        }
        if (j->z.over > ZP_PAST) {
            free(s.bm.mem);
            break;
        }
        int to_lib = type == 1 || type == 2 || type == 4 || type == 5, to_image = type == 1 || type == 3 || type == 4 || type == 6 || type == 8;
        if (type == 8) {   // not a symbol: at its place on the page, its top left given
            left = jb2_num(j, &j->c_col, 1, j->width > 0 ? j->width : 1);
            bottom = jb2_num(j, &j->c_row, 1, j->height > 0 ? j->height : 1) - s.bm.h + 1;
        } else if (to_image)
            jb2_place(j, s.bm.h, s.bm.w, &left, &bottom);
        if (to_image && !j->err) jb2_blit(j, &s.bm, left, bottom);
        if (j->err || !to_lib || jb2_add(j, &s)) {   // (not kept: its pixels are free for others again)
            j->pixels -= (size_t)s.bm.w * (size_t)s.bm.h + 64;
            free(s.bm.mem);
            if (!j->err && to_lib) break;
        }
    }
    return -1;
}

static void jb2_free(Jb2 *j, int keep_lib) {
    free(j->bit), free(j->left), free(j->right);
    if (!keep_lib) {
        for (int k = j->inherited; k < j->nlib; k++) free(j->lib[k].bm.mem);
        free(j->lib);
    }
    free(j->page.mem);
}

// --- the document ----------------------------------------------------------------------------------

typedef struct {   // a component file of the document: a page, or one others include
    size_t at, len;   // its FORM's content (after its "DJVU" / "DJVI")
    char *id;
    int is_page;
    Dict *dict;       // the shape dictionary it brings, decoded (its own Djbz, or the one it includes: dict_own 0)
    int dict_tried, dict_own;
} Comp;

typedef struct {
    int comp, w, h, turn, version;   // (turn: quarter turns clockwise to show it upright)
    uint8_t *bits;                    // decoded: rows of packed bits (1: black), turned; NULL: not (or no mask: blank)
    size_t rowbytes;
    int bw, bh, blank, users, stamp;
} Page;

#define KEEP_PAGES 4

struct nitrodjvu {
    uint8_t *data;
    size_t len;
    Comp *comps;
    int ncomps;
    Page *pages;
    int npages, stamp;
    pthread_mutex_t lock;
};

// the next chunk of a FORM's content from *at: its id, its data; 0 at the end
static int chunk(const nitrodjvu *d, size_t *at, size_t end, const uint8_t **id, const uint8_t **data, size_t *len) {
    if (*at + 8 > end) return 0;
    const uint8_t *p = d->data + *at;
    size_t n = be32(p + 4);
    if (n > end - *at - 8) n = end - *at - 8;   // (a file cut short: what is there)
    *id = p, *data = p + 8, *len = n;
    *at += 8 + n + (n & 1);
    return 1;
}

static int comp_by_id(const nitrodjvu *d, const uint8_t *id, size_t n) {
    while (n && (id[n - 1] == 0 || id[n - 1] == ' ' || id[n - 1] == '\n' || id[n - 1] == '\r')) n--;
    for (int k = 0; k < d->ncomps; k++)
        if (d->comps[k].id && strlen(d->comps[k].id) == n && !memcmp(d->comps[k].id, id, n)) return k;
    return -1;
}

// The shape dictionary a component brings: its own Djbz, which may build on one it includes in turn
// (depth: no deeper than a few). NULL: none, or damaged. Decoded once, kept with the document.
static const Dict *comp_dict(nitrodjvu *d, int c, int depth) {
    Comp *co = &d->comps[c];
    if (co->dict_tried) return co->dict;
    co->dict_tried = 1;
    if (depth > 4) return NULL;
    const Dict *inherited = NULL;
    const uint8_t *id, *data, *djbz = NULL;
    size_t at = co->at, n, djbz_len = 0;
    while (chunk(d, &at, co->at + co->len, &id, &data, &n)) {
        if (!memcmp(id, "INCL", 4)) {
            int k = comp_by_id(d, data, n);
            const Dict *x = k >= 0 && k != c ? comp_dict(d, k, depth + 1) : NULL;
            if (x) inherited = x;
        } else if (!memcmp(id, "Djbz", 4) && !djbz)
            djbz = data, djbz_len = n;
    }
    if (!djbz) {
        co->dict = (Dict *)inherited;   // (a file that only passes another's on)
        return inherited;
    }
    Jb2 *j = calloc(1, sizeof *j);
    Dict *out = calloc(1, sizeof *out);
    if (j && out && !jb2_decode(j, djbz, djbz_len, inherited, 0)) {
        out->shapes = j->lib, out->n = j->nlib, out->inherited = j->inherited;
        jb2_free(j, 1);
        free(j);
        co->dict = out, co->dict_own = 1;
        return out;
    }
    if (j) jb2_free(j, 0);
    free(j), free(out);
    return NULL;
}

// A page decoded: its mask (Sjbz) with the dictionary its file has or includes, turned as its INFO
// says, as packed rows. Without a mask: blank (a grey sheet); a mask cut short or damaged: as much
// of it as was drawn (what follows a damaged spot decodes as noise: a stray letter here and there).
static void page_decode(nitrodjvu *d, Page *pg) {
    Comp *co = &d->comps[pg->comp];
    const Dict *dict = comp_dict(d, pg->comp, 0);
    const uint8_t *id, *data, *sjbz = NULL;
    size_t at = co->at, n, sjbz_len = 0;
    while (chunk(d, &at, co->at + co->len, &id, &data, &n))
        if (!memcmp(id, "Sjbz", 4) && !sjbz) sjbz = data, sjbz_len = n;
    pg->blank = 1;
    Jb2 *j = sjbz ? calloc(1, sizeof *j) : NULL;
    if (!j) return;
    j->old = pg->version <= 18;
    jb2_decode(j, sjbz, sjbz_len, dict, 1);   // (cut short or damaged: what there is of it)
    if (j->have_page) {
        const Bm *b = &j->page;
        int turned = pg->turn & 1, ow = turned ? b->h : b->w, oh = turned ? b->w : b->h;
        size_t rb = ((size_t)ow + 7) / 8;
        uint8_t *bits = calloc(rb * (size_t)oh, 1);
        if (bits) {
            for (int y = 0; y < b->h; y++) {
                const uint8_t *s = bm_row(b, y);
                for (int x = 0; x < b->w; x++) {
                    if (!s[x]) continue;
                    int ox = pg->turn == 1 ? b->h - 1 - y : pg->turn == 2 ? b->w - 1 - x : pg->turn == 3 ? y : x;
                    int oy = pg->turn == 1 ? x : pg->turn == 2 ? b->h - 1 - y : pg->turn == 3 ? b->w - 1 - x : y;
                    bits[(size_t)oy * rb + (ox >> 3)] |= (uint8_t)(0x80 >> (ox & 7));
                }
            }
            pg->bits = bits, pg->rowbytes = rb, pg->bw = ow, pg->bh = oh, pg->blank = 0;
        }
    }
    jb2_free(j, 0);
    free(j);
}

// a page's file read for what a page is: its INFO (the size, the version, the turn)
static int page_info(const nitrodjvu *d, const Comp *co, Page *pg) {
    const uint8_t *id, *data;
    size_t at = co->at, n;
    if (!chunk(d, &at, co->at + co->len, &id, &data, &n) || memcmp(id, "INFO", 4) || n < 4) return -1;
    int w = data[0] << 8 | data[1], h = data[2] << 8 | data[3], flags = n >= 10 ? data[9] & 7 : 0;
    if (w < 1 || h < 1) return -1;
    pg->version = n >= 5 ? data[4] : 0;
    // (the low bits of the flags: 1 upright, 6 a quarter turn counter-clockwise, 2 upside down, 5 clockwise)
    pg->turn = flags == 6 ? 3 : flags == 2 ? 2 : flags == 5 ? 1 : 0;
    pg->w = pg->turn & 1 ? h : w, pg->h = pg->turn & 1 ? w : h;
    return 0;
}

nitrodjvu *nitrodjvu_load(const void *data, size_t len) {
    const uint8_t *p = data;
    if (!p || len < 16 || len > (size_t)INT_MAX || memcmp(p, "AT&TFORM", 8)) return NULL;
    int multi = !memcmp(p + 12, "DJVM", 4);
    if (!multi && memcmp(p + 12, "DJVU", 4)) return NULL;
    nitrodjvu *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    pthread_mutex_init(&d->lock, NULL);
    d->data = malloc(len);
    if (!d->data) goto fail;
    memcpy(d->data, p, len);
    d->len = len;
    size_t end = 12 + (size_t)be32(p + 8);
    if (end > len) end = len;
    if (!multi) {
        d->comps = calloc(1, sizeof *d->comps);
        if (!d->comps) goto fail;
        d->ncomps = 1;
        d->comps[0].at = 16, d->comps[0].len = end - 16, d->comps[0].is_page = 1;
    } else {
        // the directory (8.3.2): bundled or not, the count, each file's offset; then, BZZ coded, their
        // sizes, their flags (the low six bits 1: a page) and their names
        const uint8_t *id, *dir;
        size_t at = 16, n;
        if (!chunk(d, &at, end, &id, &dir, &n) || memcmp(id, "DIRM", 4) || n < 3 || !(dir[0] & 0x80)) goto fail;   // (not bundled: its pages are other files)
        int count = dir[1] << 8 | dir[2];
        if (!count || n < 3 + 4 * (size_t)count) goto fail;
        size_t names_len = 0;
        uint8_t *names = bzz_decode(dir + 3 + 4 * (size_t)count, n - 3 - 4 * (size_t)count, 16u << 20, &names_len);
        d->comps = calloc((size_t)count, sizeof *d->comps);
        if (!d->comps) {
            free(names);
            goto fail;
        }
        d->ncomps = count;
        size_t s = 4 * (size_t)count;   // (after the sizes and the flags: the names)
        for (int k = 0; k < count; k++) {
            Comp *co = &d->comps[k];
            size_t off = be32(dir + 3 + 4 * (size_t)k);
            int flags = names && names_len >= 4 * (size_t)count ? names[3 * (size_t)count + k] : -1;
            if (off + 12 <= len && !memcmp(d->data + off, "FORM", 4)) {
                size_t fl = be32(d->data + off + 4);
                if (fl > len - off - 8) fl = len - off - 8;
                co->at = off + 12, co->len = fl >= 4 ? fl - 4 : 0;
                // (without the directory's flags: a FORM:DJVU is a page)
                co->is_page = flags >= 0 ? (flags & 0x3f) == 1 : !memcmp(d->data + off + 8, "DJVU", 4);
            }
            for (int str = 0; names && flags >= 0 && str < 1 + !!(flags & 0x80) + !!(flags & 0x40); str++) {   // its id, then a name, a title
                size_t e = s;
                while (e < names_len && names[e]) e++;
                if (!str && (co->id = malloc(e - s + 1))) memcpy(co->id, names + s, e - s), co->id[e - s] = 0;
                s = e < names_len ? e + 1 : e;
            }
        }
        free(names);
    }
    for (int k = 0; k < d->ncomps; k++) d->npages += d->comps[k].is_page;
    d->pages = calloc((size_t)(d->npages ? d->npages : 1), sizeof *d->pages);
    if (!d->pages) goto fail;
    d->npages = 0;
    for (int k = 0; k < d->ncomps; k++) {
        Page pg;
        memset(&pg, 0, sizeof pg);
        pg.comp = k;
        if (d->comps[k].is_page && !page_info(d, &d->comps[k], &pg)) d->pages[d->npages++] = pg;
    }
    if (d->npages) return d;
fail:
    nitrodjvu_free(d);
    return NULL;
}

void nitrodjvu_free(nitrodjvu *d) {
    if (!d) return;
    for (int k = 0; k < d->ncomps; k++) {
        Comp *co = &d->comps[k];
        if (co->dict_own) {   // (its own shapes: those after the ones it inherited)
            for (int n = co->dict->inherited; n < co->dict->n; n++) free(co->dict->shapes[n].bm.mem);
            free(co->dict->shapes);
            free(co->dict);
        }
        free(co->id);
    }
    for (int k = 0; k < d->npages; k++) free(d->pages[k].bits);
    free(d->comps), free(d->pages), free(d->data);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

int nitrodjvu_pages(const nitrodjvu *d) { return d ? d->npages : 0; }

void nitrodjvu_size(const nitrodjvu *d, int page, double *w, double *h) {
    *w = *h = 0;
    if (!d || page < 0 || page >= d->npages) return;
    *w = d->pages[page].w, *h = d->pages[page].h;
}

// the black pixels of a row of packed bits in the columns x0 .. x1 - 1
static unsigned black(const uint8_t *row, int x0, int x1) {
    static const uint8_t ones[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    unsigned n = 0;
    for (int x = x0; x < x1;) {
        if (!(x & 7) && x + 8 <= x1) {
            uint8_t b = row[x >> 3];
            n += ones[b >> 4] + ones[b & 15];
            x += 8;
        } else {
            n += row[x >> 3] >> (7 - (x & 7)) & 1;
            x++;
        }
    }
    return n;
}

int nitrodjvu_render(nitrodjvu *d, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx) {
    if (!d || page < 0 || page >= d->npages || w <= 0 || h <= 0 || pitch < (size_t)w * 4 || !(scale > 0)) return -1;
    Page *pg = &d->pages[page];
    // decoded now, or still kept: the pages drawn longest ago are let go, but for those being drawn
    pthread_mutex_lock(&d->lock);
    if (!pg->bits && !pg->blank) {
        page_decode(d, pg);
        int kept = 0;
        for (int k = 0; k < d->npages; k++) kept += d->pages[k].bits != NULL;
        while (kept > KEEP_PAGES) {
            Page *old = NULL;
            for (int k = 0; k < d->npages; k++) {
                Page *o = &d->pages[k];
                if (o->bits && o != pg && !o->users && (!old || o->stamp < old->stamp)) old = o;
            }
            if (!old) break;
            free(old->bits);
            old->bits = NULL, kept--;
        }
    }
    pg->stamp = ++d->stamp;
    pg->users++;
    pthread_mutex_unlock(&d->lock);
    int rc = 0;
    for (int j = 0; j < h; j++) {
        if ((j & 63) == 0 && cancelled && cancelled(ctx)) {
            rc = 1;
            break;
        }
        uint32_t *o = (uint32_t *)(bgra + (size_t)j * pitch);
        if (!pg->bits) {   // (no mask: a grey sheet)
            for (int i = 0; i < w; i++) o[i] = 0xffd8d8d8u;
            continue;
        }
        // the page's rows this row of pixels covers (enlarged: the one at its middle)
        double ya = y0 + j / scale, yb = y0 + (j + 1) / scale;
        int r0 = scale >= 1 ? (int)((ya + yb) / 2) : (int)ya, r1 = scale >= 1 ? r0 + 1 : (int)yb + (yb > (int)yb);
        if (ya < 0 && scale >= 1) r0 = -1, r1 = 0;
        if (r0 < 0) r0 = 0;
        if (r1 > pg->bh) r1 = pg->bh;
        for (int i = 0; i < w; i++) {
            double xa = x0 + i / scale, xb = x0 + (i + 1) / scale;
            int c0 = scale >= 1 ? (int)((xa + xb) / 2) : (int)xa, c1 = scale >= 1 ? c0 + 1 : (int)xb + (xb > (int)xb);
            if (xa < 0 && scale >= 1) c0 = -1, c1 = 0;
            if (c0 < 0) c0 = 0;
            if (c1 > pg->bw) c1 = pg->bw;
            unsigned n = 0, area = r1 > r0 && c1 > c0 ? (unsigned)(r1 - r0) * (unsigned)(c1 - c0) : 0;
            for (int r = r0; r < r1 && c1 > c0; r++) n += black(pg->bits + (size_t)r * pg->rowbytes, c0, c1);
            unsigned v = area ? 255 - (n * 255 + area / 2) / area : 255;
            o[i] = 0xff000000u | v << 16 | v << 8 | v;
        }
    }
    pthread_mutex_lock(&d->lock);
    pg->users--;
    pthread_mutex_unlock(&d->lock);
    return rc;
}
