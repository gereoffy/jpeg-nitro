// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrojbig2: JBIG2 bi-level images as PDFs embed them (/JBIG2Decode, ITU T.88 annex D.3: the
// segments without the file header), for nitropdf: generic regions (arithmetic or MMR coded),
// symbol dictionaries and text regions (arithmetic coded) with their refinements. Not decoded:
// Huffman coded dictionaries and text regions, halftone regions, refinement region segments.
#ifndef NITROJBIG2_H
#define NITROJBIG2_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The page of data (after the segments of globals, /JBIG2Globals, when given) into rows of packed
// bits, width x height pixels, prow bytes apart; a black pixel is 0, as the PDF filter gives it.
// Returns 0 on success, -1 on damaged data or segments not supported.
int nj2_decode(const uint8_t *data, size_t len, const uint8_t *globals, size_t glen, int width, int height,
               uint8_t *dst, size_t prow);

#ifdef __cplusplus
}
#endif

#endif
