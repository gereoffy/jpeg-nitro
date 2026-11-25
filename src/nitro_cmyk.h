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

// A file's own ICC profile of n components (1 gray, 3 RGB, 4 CMYK) -> sRGB. NULL if it is not a profile
// of that many components, cannot be converted with, or (gray, RGB) is sRGB within 2 of 255: then the
// values are as good as they are. A CMYK one gets a table like the built-in profile's (0.5 MB, a few
// ms); an RGB one is converted by lcms2. Thread-safe once opened.
typedef struct ncm_profile ncm_profile;
ncm_profile *ncm_profile_open(const uint8_t *icc, size_t len, int n);
void ncm_profile_close(ncm_profile *p);
// one colour: its n components 0..1 (CMYK: 1 full ink) -> R G B 0..1
void ncm_profile_color(const ncm_profile *p, const float *c, float rgb[3]);
// count pixels of n bytes each (gray; R G B; C M Y K, 255 full ink) -> B G R A (A 255); CMYK may be in place
void ncm_profile_row_bgra(const ncm_profile *p, const uint8_t *src, uint8_t *bgra, size_t count);

#ifdef __cplusplus
}
#endif
