// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrojxl: JPEG XL (ISO/IEC 18181) still images, without dependencies. Raw codestreams and
// containers (jxlc, jxlp); VarDCT (every transform, progressive passes, chroma subsampled
// YCbCr of recompressed JPEGs) and Modular (MA trees, RCT, palette, squeeze); Gaborish and
// EPF; alpha; 8..16 bit integer and float samples; the embedded ICC profile.
// XYB images are given in sRGB (HDR ones tone mapped), images in an enumerated colour space
// converted to sRGB; with an ICC profile the pixels stay in its space (njxl_info.icc).
// Layers and tiles (frames that replace a rectangle of the image) are put together.
// Not supported (NJXL_UNSUPPORTED): patches, splines, upsampling, LF frames, blended layers
// (an animation gives its first frame), noise is left out.
// A cut-off or damaged image decodes what can be read (njxl_info.damaged); the headers and the frame's
// LfGlobal must be readable. VarDCT can display its LF image if later data is missing.
#ifndef NITROJXL_H
#define NITROJXL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NJXL_OK = 0,
    NJXL_INVALID = -1,          // not JXL, damaged or incomplete
    NJXL_UNSUPPORTED = -2,      // a feature this decoder does not have
    NJXL_NOMEM = -3,
    NJXL_LIMIT = -4
};

typedef struct {
    const uint8_t *data;        // codestream, beginning FF 0A
    size_t len;
    uint8_t *storage;           // owned allocation for concatenated jxlp; otherwise NULL
    int level;                 // container: 5 or 10; raw codestream: 0 (not declared)
} njxl_stream;

// A complete file: raw codestream or version-0 container with jxlc / ordered jxlp.
// Exif, XMP, JPEG reconstruction data and unknown boxes are skipped.
// Checks framing, lengths and fragment sequence, NOT the compressed image's validity.
// Maximum codestream size: 512 MiB. At most one million boxes.
// On failure *out is empty. Keep input alive until njxl_stream_free: raw / jxlc data
// is borrowed. Always release the result, including failures, with njxl_stream_free.
// out must not own a previous result when called; NULL out is an error.
int njxl_unwrap(const uint8_t *data, size_t len, njxl_stream *out);
void njxl_stream_free(njxl_stream *stream);

typedef struct {
    int width, height;          // as stored: before the orientation
    int orientation;            // EXIF 1..8
    int bits, is_float, grey;   // the original samples
    int alpha, premultiplied;
    int xyb, animated;
    uint8_t *icc;               // njxl_decode: the pixels are in this profile's colour space; NULL: sRGB
    size_t icc_len;             //   (owned: njxl_info_free)
    int damaged;                // njxl_decode: cut off or damaged; what could be read is shown (a VarDCT group's
                                // damaged pass: its LF image; a lost LF group or Modular group: empty)
} njxl_info;

enum { NJXL_SINGLE_THREAD = 1 };       // flags: decode on the calling thread only
enum { NJXL_BGRA8 = 0, NJXL_RGBA16 = 1,     // formats: premultiplied 8-bit BGRA; 16-bit RGBA (straight alpha
                                             // premultiplied as well), pitch in bytes;
       NJXL_SAMPLES16 = 2 };                 // the samples as coded (a lossless image's values exactly): 16-bit
                                             // numbers 0 .. 2^bits - 1, grey: 1 a pixel, else R G B; no alpha, no
                                             // colour conversion. Integer samples of at most 16 bits; XYB ones
                                             // back in their own encoding when that is grey or sRGB primaries
                                             // with a linear or sRGB curve (else NJXL_UNSUPPORTED): DICOM's pixels

// The headers only, and the ICC profile the decoded pixels will be in (a file's start is enough
// when the codestream header and the profile are in it).
int njxl_info_read(const uint8_t *data, size_t len, njxl_info *info);
// The whole image into pixels (info->width x info->height, from njxl_info_read). info may be NULL;
// otherwise release it with njxl_info_free, also after a failure.
int njxl_decode(const uint8_t *data, size_t len, unsigned flags, int format, void *pixels, size_t pitch, njxl_info *info);
void njxl_info_free(njxl_info *info);

// The size and orientation from the start of a file (a container's first codestream box may be cut
// off). -1: not JPEG XL, or the header is not there.
int njxl_size(const uint8_t *data, size_t len, int *width, int *height, int *orientation);

#ifdef __cplusplus
}
#endif
#endif
