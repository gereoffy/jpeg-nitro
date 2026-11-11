// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_convert: decoded planes (nitro_load's NvImage) -> RGB pixels, for whoever shows the image
// without a GPU shader: the GNOME viewer, other programs on the library. Multi-threaded
// (nitro_os.h), plain C.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "nitro_load.h"
#include "nitrojpeg.h"

#ifdef __cplusplus
extern "C" {
#endif

// An NV_YUV image (nitro_load.h: a JPEG's planes from nj_decode_planes, any sampling; HEIC's and
// WebP's 4:2:0) in buf -> RGB, 3 bytes per pixel, rows 'stride' bytes apart. Chroma upsampling
// and YCbCr->RGB as libjpeg(-turbo) does it ("fancy" upsampling for 4:2:2, 4:2:0 and 4:4:0,
// replication for other factors, the same integer arithmetic): the output is identical to
// libjpeg-turbo's RGB output. Limited range (img->limited: WebP) is scaled to full, BT.601.
void nc_yuv_to_rgb(const NvImage *img, const uint8_t *buf, uint8_t *dst, size_t stride);

// A JPEG's planes from nj_decode_planes (its nj_info: any sampling, 1, 3 or 4 components) -> opaque
// BGRA, 4 bytes per pixel, upsampled as above: gray, YCbCr, RGB (Adobe's transform 0), CMYK and
// YCCK by cmyk (a row of C M Y K bytes, 255 full ink, to B G R A in place: nitro_cmyk's
// ncm_row_bgra) or, if NULL, the simple formula (no profile); inverted: the CMYK values are as
// Adobe stores them (255 no ink), else 255 is full ink.
typedef void (*nc_cmyk_row)(const uint8_t *cmyk, uint8_t *bgra, size_t n);
void nc_jpeg_to_bgra(const nj_info *fi, uint8_t *const planes[4], const size_t pitch[4], int inverted, nc_cmyk_row cmyk,
                     uint8_t *dst, size_t stride);

// An NV_PLANAR image (a PSD's planes from ps_decode) in buf -> RGB. Merged transparency
// (Photoshop stores the colour matted with white) is shown over black, as the macOS and
// Windows viewers do.
void nc_psd_to_rgb(const NvImage *img, const uint8_t *buf, uint8_t *dst, size_t stride);

#ifdef __cplusplus
}
#endif
