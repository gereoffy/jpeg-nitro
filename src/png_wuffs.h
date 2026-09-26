// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Optional Wuffs-based PNG decoding for nitroview (see png_wuffs.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Image size from the PNG header. Returns 0 on success.
int nv_png_size(const uint8_t *data, size_t len, int *w, int *h);

// Decodes into premultiplied 8-bit BGRA (any PNG type: palette, gray, 16-bit,
// alpha, interlaced). dst holds h rows of 'pitch' bytes. Returns 0 on success.
int nv_png_decode_bgra(const uint8_t *data, size_t len, uint8_t *dst, size_t pitch, int w, int h);

#ifdef __cplusplus
}
#endif
