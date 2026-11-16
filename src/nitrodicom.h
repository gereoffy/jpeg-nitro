// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodicom: the picture of a DICOM file (medical images: X-ray, CT, MR, ultrasound ...) as 8-bit
// BGRX, written from the DICOM standard (PS3.5: the data elements and the pixel data's encodings;
// PS3.3 C.7.6.3 and C.11: what the samples mean and how they are shown).
//
// The file: a Part 10 file (the 128 byte preamble, "DICM", the meta group), or the bare data set
// some old programs wrote (ACR-NEMA too); implicit or explicit VR, little or big endian (and GE's
// private syntax: a little endian data set with big endian samples).
// The pixel data: uncompressed (1, 8, 16 or 32 bits a sample), RLE, JPEG baseline (nitrojpeg),
// lossless JPEG (nitrodng's decoder), JPEG 2000 (nitrojpx).
// The picture: grey (MONOCHROME1 / 2) through the rescale (slope, intercept) and the window the
// file gives (its first window, or its VOI lookup table; neither, or a window that would leave the
// picture all white: 8-bit pictures as they are, the others from their smallest to their largest
// value, the thousandth of the pixels at either end not counted), made lighter or darker by
// info.ev; palette
// colour, RGB and YBR_FULL (_422); a grey picture with a supplemental palette (perfusion maps: the
// values the palette is for in its colours, the others grey).
// Of a file of several frames one frame at a time (all of the same size: the viewers step through them).
// Not handled (info.error says which): JPEG-LS, 12-bit lossy JPEG, HTJ2K, JPEG XL, the video
// codecs, the deflated transfer syntaxes, floating point pixel data, segmented palettes; overlays,
// a modality lookup table and the pixels' aspect ratio are ignored.
#ifndef NITRODICOM_H
#define NITRODICOM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int n, first, bits;   // a lookup table's descriptor: entries, the value mapped to the first one, bits of an entry
    size_t pos, len;      // its data in the file
} ndcm_lut;

typedef struct {
    int width, height;   // of a frame
    int frames;
    const char *error;   // why the picture cannot be decoded (a static text); NULL: ndcm_decode can
    float ev;            // exposure correction in stops: set by the caller after ndcm_read_info (0: none). The
                         // shades are multiplied by 2^ev before they are cut to black .. white
    // internal
    int syntax;                  // how the pixel data is coded
    int big_endian;              // the data set's numbers
    int samples_big_endian;      // the uncompressed samples (as the data set, but for GE's old private syntax)
    int spp, alloc, stored, high, is_signed, planar, photometric;
    size_t pixel_pos, pixel_len; // the pixel data element's value (encapsulated: its items)
    int encapsulated, pixel_words;   // pixel_words: its VR is OW (8-bit samples of a big endian file are swapped in pairs)
    int has_window, has_padding, padding;
    int supplemental;            // a grey picture with a supplemental palette: the values from the palette's first on are colours
    double center, width_w;      // the window
    double slope, intercept;     // the rescale
    ndcm_lut palette[3], voi;
} ndcm_info;

// data must be followed by 64 readable bytes (as nv_read_file's), here and below.
// Parses the data set. Returns 0 if it is a DICOM file with a picture (info->error may say that
// it cannot be decoded); the start of a file is enough for that when the pixel data comes later
// (the size of a picture from its header). -1: not one.
int ndcm_read_info(const uint8_t *data, size_t len, ndcm_info *info);

// A frame (0 .. frames - 1) as 8-bit pixels, B G R X order (width x height, pitch bytes per row).
// nthreads 1: on the calling thread, else in parallel. Returns 0 on success.
int ndcm_decode(const uint8_t *data, size_t len, const ndcm_info *info, int frame, uint8_t *bgrx, size_t pitch, int nthreads);

// The frame's stored values, before any lookup table: width * height * samples per pixel numbers
// (the samples of a pixel one after the other; *spp: how many), for tests and tools. A lossy JPEG
// in colour comes as its R G B bytes. Returns 0 on success.
int ndcm_values(const uint8_t *data, size_t len, const ndcm_info *info, int frame, int32_t *out, int *spp);

#ifdef __cplusplus
}
#endif
#endif
