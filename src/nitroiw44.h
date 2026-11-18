// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitroiw44: DjVu's wavelet pictures (IW44: the BG44, FG44, PM44 and BM44 chunks), written from the
// DjVu Reference (v3, appendix 1): a page's background and foreground layers, photographs. A
// layer's chunks are slices of one progressive stream (each refines the coefficients the earlier
// ones left); the picture comes out as planes, Y Cb Cr or grey, to be turned into RGB where it is
// drawn (niw44_rgb: DjVu's own YCbCr, not JPEG's).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct niw44 niw44;

// A layer to decode; NULL: out of memory.
niw44 *niw44_new(void);
void niw44_free(niw44 *d);

// The layer's next chunk, in the order the file has them (the first one has the size). 0: its
// slices are in (a chunk cut short: as many as it has); -1: not the chunk that is due, a header
// that makes no sense, a picture over the limits; -2: out of memory. Either way the layer stays as it was.
int niw44_chunk(niw44 *d, const uint8_t *data, size_t len);

// Its size and components (3: Y Cb Cr, 1: grey); 0 before the first chunk.
int niw44_size(const niw44 *d, int *w, int *h);

// The picture as the coefficients stand: planes of w x h bytes, rows top to bottom, pitch bytes
// apart. y: 0 (black) .. 255; cb, cr (not written for a grey layer; may be NULL then): the signed
// chroma + 128. 0, or -1: no picture yet, or out of memory.
int niw44_planes(const niw44 *d, uint8_t *y, uint8_t *cb, uint8_t *cr, size_t pitch);

// A pixel of the planes as RGB [10.6.7]: R = Y + 3/2 Cr, G = Y - 1/4 Cb - 3/4 Cr, B = Y + 7/4 Cb.
static inline void niw44_rgb(int y, int cb, int cr, uint8_t *rgb) {
    cb -= 128, cr -= 128;
    int t1 = cb >> 2, t2 = cr + (cr >> 1), t3 = y - t1;
    int r = y + t2, g = t3 - (t2 >> 1), b = t3 + cb * 2;
    rgb[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    rgb[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    rgb[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

#ifdef __cplusplus
}
#endif
