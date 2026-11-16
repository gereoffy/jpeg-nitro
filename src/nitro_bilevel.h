// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_bilevel: the bilevel bitmap the symbol coders build a page of, shared by nitrojbig2
// (JBIG2) and nitrodjvu (DjVu's JB2): a byte a pixel (1: black), with BM_MX blank columns either
// side and 2 blank rows above, so that a coding template's fixed pixels are read without checks.
// Header only (C).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *mem;
    int w, h;
    size_t stride;
} Bm;
#define BM_MX 4

static inline int bm_new(Bm *b, int w, int h) {
    b->w = w, b->h = h, b->stride = (size_t)w + 2 * BM_MX;
    b->mem = calloc((size_t)(h + 2) * b->stride, 1);
    return b->mem ? 0 : -1;
}
static inline uint8_t *bm_row(const Bm *b, int y) { return b->mem + (size_t)(y + 2) * b->stride + BM_MX; }
// a pixel anywhere: 0 outside the bitmap
static inline unsigned bm_px(const Bm *b, int x, int y) {
    return x >= 0 && x < b->w && y >= 0 && y < b->h ? bm_row(b, y)[x] : 0u;
}

// The bitmap r onto page at x0, y0 (its top left; any part may lie outside) by the operator op:
// 0 OR, 1 AND, 2 XOR, 3 XNOR, else REPLACE (JBIG2's combination operators; JB2 only ORs).
static inline void bm_compose(Bm *page, const Bm *r, int x0, int y0, int op) {
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

// The bitmap as rows of packed bits, the first pixel a byte's high bit, prow bytes a row (at least
// (w + 7) / 8): a pixel's bit is 1 where it equals one (1: the black ones set, 0: the white ones).
static inline void bm_pack(const Bm *b, uint8_t *dst, size_t prow, int one) {
    for (int y = 0; y < b->h; y++) {
        const uint8_t *s = bm_row(b, y);
        uint8_t *o = dst + (size_t)y * prow;
        memset(o, 0, prow);
        for (int x = 0; x < b->w; x++)
            if (s[x] == one) o[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
}
