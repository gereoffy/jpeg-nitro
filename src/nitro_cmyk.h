// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_cmyk: CMYK -> sRGB through a printing profile, as Acrobat shows CMYK (the simple formula's
// colours are far too bright): libcdr's generic CMYK profile (Argyll CMS, public domain), turned by
// lcms2 into a 17^4 table the first time it is needed (a few ms), then interpolated (4D simplex,
// 5 of its points a pixel; within a few levels of lcms2's own result, ten times faster). Thread-safe.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// n pixels of C M Y K bytes (255: full ink) -> B G R A (A 255); bgra may be cmyk (in place)
void ncm_row_bgra(const uint8_t *cmyk, uint8_t *bgra, size_t n);
// one colour: C M Y K 0..1 -> R G B 0..1
void ncm_color(const float cmyk[4], float rgb[3]);

#ifdef __cplusplus
}
#endif
