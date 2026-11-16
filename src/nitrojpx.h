// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrojpx: JPEG 2000 Part 1 images as PDFs embed them (/JPXDecode), without dependencies.
// Raw codestreams and JP2 containers; 1..16 components, 1..16 bits, signed or unsigned,
// tiles, tile-parts, precincts, layers, all five progression orders, 5/3 and 9/7 wavelets,
// RCT/ICT, all Part 1 code-block styles and maxshift ROI. JP2 palette/channel mapping and
// channel definitions are applied. Not supported: POC, packed packet headers (PPM/PPT),
// Part 2 extensions and HTJ2K. Unsupported/damaged images return an error.
// Up to 28 decomposition levels and 30 coded magnitude bit-planes (including ROI).
// ICC colour spaces are identified by their device space; ICC transforms are the caller's.
#ifndef NITROJPX_H
#define NITROJPX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NJPX_MAX_COMPS 16
// JP2 enumerated colour spaces; 0 means unspecified (e.g. a raw codestream).
enum { NJPX_UNKNOWN = 0, NJPX_CMYK = 12, NJPX_SRGB = 16, NJPX_GRAY = 17, NJPX_SYCC = 18 };
// A PDF with /ColorSpace supplies its own interpretation of the codestream's components.
// Ignore JP2 palette/colour-channel mapping in that case, retaining opacity information.
enum { NJPX_RAW_COLORS = 1 };
// Default: nitro_os's worker pool decodes the code-blocks, the wavelets' rows and column strips
// and the output's rows at once (many tiles: the tiles). The caller can keep a decode serial
// when distributing several images across its own workers.
enum { NJPX_SINGLE_THREAD = 2 };
// NJPX_IMAGE: a picture file a viewer opens, not an image a document embeds: a picture's limits
// (2 GiB working memory, 1 Gi output samples; with NJPX_REDUCE: of the reduced image).
enum { NJPX_IMAGE = 4 };
// NJPX_REDUCE(n): the image at 1/2^n of its size (n = 1..7), its n finest resolution levels left
// out (their code-blocks not decoded, the inverse wavelets stopped there): the reduced grid,
// ceil(x / 2^n) of each coordinate. An error if a tile-component has fewer than n levels.
#define NJPX_REDUCE(n) ((unsigned)(n) << 8)

typedef struct {
    int width, height, ncomp;
    int precision[NJPX_MAX_COMPS];
    int colorspace;
    int alpha;                  // -1: no opacity channel; otherwise its index in samples
    int premultiplied;          // channel definition type 2 (associated opacity)
    uint16_t *samples;          // interleaved, width * height * ncomp, owned by this image
    const uint8_t *icc;         // a JP2's ICC profile (colr, method 2), in the data given; NULL: none
    size_t icc_len;
    // Signed samples are biased into 0..(2^precision-1); sub-sampled components are
    // expanded to the reference grid by nearest neighbour. RCT/ICT is already undone;
    // enumerated sYCC is left to the caller, as is any ICC / PDF colour conversion.
} njpx_image;

// On failure *out is empty. On success release its samples with njpx_free.
// Resource limits: 512 MiB working memory, 64 million output samples, 1 million packets.
int njpx_decode(const uint8_t *data, size_t len, unsigned flags, njpx_image *out);
void njpx_free(njpx_image *image);

// The image's size on its full grid (before NJPX_REDUCE), from the codestream's SIZ marker; the
// start of a file is enough where that is. -1: not JPEG 2000 (a JP2 file or a codestream).
int njpx_size(const uint8_t *data, size_t len, int *width, int *height);

// The decoded image as 8-bit BGRA, pitch bytes a row (premultiplied; opaque, X 255, without an
// opacity channel): grey, RGB and sYCC images of any precision, and unspecified ones of one or
// three colour channels (a raw codestream). -1: another colour space (CMYK ...) or channel count.
int njpx_bgra(const njpx_image *image, uint8_t *bgra, size_t pitch);

#ifdef __cplusplus
}
#endif
#endif
