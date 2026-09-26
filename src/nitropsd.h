// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitropsd: multi-threaded decoding of the merged (composite) image of a
// Photoshop PSD / PSB file. Layers and other metadata are skipped.
//
// The merged image is stored planar (all rows of channel 0, then channel 1 ...)
// with one of four compressions, all decoded in parallel:
//   0 raw        - copied
//   1 RLE        - PackBits per row; the table of row lengths gives every row's
//                  position, so rows are decoded independently
//   2 ZIP        - one zlib stream: nitropng's parallel inflate
//   3 ZIP+pred.  - same, then per-row differences are undone (rows independent)
// Handles 8-bit RGB and grayscale (+ the merged transparency channel);
// everything else (16/32-bit, CMYK, Lab, indexed ...) reports supported = 0.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width, height;
    int channels, depth, mode;   // as in the header (mode 1 gray, 3 RGB)
    int version;                 // 1 PSD, 2 PSB
    int compression;             // 0 raw, 1 RLE, 2 ZIP, 3 ZIP with prediction
    int ncolor;                  // colour planes decoded: 1 or 3
    int alpha;                   // 1: a transparency plane follows (negative layer count)
    size_t plane_size;           // width * height
    size_t data_off;             // offset of the image data (after the compression field)
    int supported;
} ps_info;

typedef struct {
    double decode_ms;
    int mode;                    // compression used
} ps_stats;

// Parses the header and skips to the merged image data. Returns 0 if it is a PSD/PSB.
int ps_read_info(const uint8_t *data, size_t len, ps_info *info);

// Decodes ncolor (+ alpha) planes of plane_size bytes each into out:
// plane c at out + c * plane_size. The colour of transparent pixels is as
// Photoshop stores it (matted with white). nthreads <= 0: all CPUs.
// Returns 0 on success.
int ps_decode(const uint8_t *data, size_t len, const ps_info *info, uint8_t *out, int nthreads, ps_stats *stats);

#ifdef __cplusplus
}
#endif
