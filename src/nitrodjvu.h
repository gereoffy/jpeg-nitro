// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodjvu: DjVu documents for the viewers, their mask layer alone: the bilevel picture of the
// text and line art (JB2), black on white - all there is to a scanned black and white book, which
// most DjVu files are. Not decoded: the wavelet layers (IW44: a page's photographs and coloured
// background, the colour of its text), so a compound page shows its text and drawings without
// them, and a page of a picture alone is a grey sheet; the hidden text, outlines and annotations;
// indirect documents (their pages in files of their own). Written from the format's public
// reference (DjVu Reference, version 3: Lizardtech 2005) without another decoder's code: the IFF
// container and the bundled documents' directory (BZZ), the Z'-coder, JB2 with its shared shape
// dictionaries.
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

// Renders a page into a w x h BGRA buffer (opaque): pixel (i, j) shows the point
// (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale) of the page; smaller than its pixels, each is
// the average of the pixels it covers (grey). Several renders at once (the bands of one): the
// first one of a page decodes it. Returns 0 on success, 1 if cancelled (may be NULL) said to stop.
int nitrodjvu_render(nitrodjvu *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
