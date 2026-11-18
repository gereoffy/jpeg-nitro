// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodjvu: DjVu documents for the viewers. A page is a mask (the bilevel picture of its text and
// line art: JB2, with shared shape dictionaries) that picks, pixel by pixel, the foreground's colour
// over the background picture: the background (the paper, photographs: IW44 wavelets, nitroiw44) is
// several times smaller than the page, the foreground's colours come as a palette entry for each
// shape (FGbz) or as a still smaller picture (FG44); a mask may be coded as a fax is (Smmr: CCITT
// G4, nitrotiff's). A black and white book is its mask alone, a photograph page its background alone. The layers are decoded when a page is first drawn and kept
// as they are (the mask as bits, the pictures as their small YCbCr planes); a render mixes and
// scales them for the pixels it draws. Not read: JPEG coded layers (BGjp, FGjp), the hidden text,
// outlines and annotations; indirect documents (their pages in files of their own). Written from
// the format's public reference (DjVu Reference, version 3: Lizardtech 2005): the IFF container
// and the bundled documents' directory (BZZ), the Z'-coder, JB2.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitrodjvu nitrodjvu;

// NULL if it is not a DjVu file this reads (a single page or a bundled document) or it has no page.
// The data is copied; the pages are decoded when they are first drawn.
nitrodjvu *nitrodjvu_load(const void *data, size_t len);
void nitrodjvu_free(nitrodjvu *doc);

int nitrodjvu_pages(const nitrodjvu *doc);
// A page's size in its own pixels (turned as its INFO says); render coordinates start at its top left.
void nitrodjvu_size(const nitrodjvu *doc, int page, double *w, double *h);

// Decodes a page if it is not decoded (or was let go of): what the first render of a page does before
// it draws. For a caller that wants the two apart (the times of a viewer).
void nitrodjvu_prepare(nitrodjvu *doc, int page);

// How the mask is enlarged (a page drawn larger than its pixels): 0 as it is, each pixel a square
// (the default); 1 and 2 with its outline smoothed - the steps of a slanted line or a curve drawn
// as a slanted edge, sharp at any enlargement. 1 keeps every pixel of the mask and takes the steps'
// corners off; 2 is stronger (slanted lines straight, curves round, the strokes a touch bolder).
// Set before the document is drawn.
void nitrodjvu_set_smooth(nitrodjvu *doc, int mode);

// Renders a page into a w x h BGRA buffer (opaque): pixel (i, j) shows the point
// (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale) of the page; smaller than its pixels, each is
// the average of the pixels it covers; the pictures are interpolated up to the page. An empty page
// is white; one whose layers cannot be read (damaged, JPEG coded) is a grey sheet. Several renders
// at once (the bands of one): the first one of a page decodes it. Returns 0 on success, 1 if
// cancelled (may be NULL) said to stop, -1 out of memory.
int nitrodjvu_render(nitrodjvu *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
