// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitropdf: PDF documents for the viewers. pdfparse3's object layer (github.com/gereoffy/pdfparse3,
// see scripts/get-deps.sh) reads the file with its tolerance; nitropdf interprets a page's content
// (paths, painting and clipping, the graphics state, the colour spaces with their functions, images
// and their masks, text, form XObjects, optional content) into a display list the first time the
// page is drawn, and draws it with plutovg at any scale and offset, like the other readers. Text: the
// fonts it embeds through FreeType (TrueType, Type 1, CFF, CID-keyed, OpenType), Type 3 fonts, and
// system fonts standing in for the others (nitro_font). Annotations: their appearance streams. Not
// yet: shadings and tiling patterns (a shading pattern fills with its middle colour), soft masks of
// groups, blend modes.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitropdf nitropdf;

// NULL if it is not a PDF or it has no page. The data is copied.
nitropdf *nitropdf_load(const void *data, size_t len);
void nitropdf_free(nitropdf *pdf);

int nitropdf_pages(const nitropdf *pdf);
// The background around and under the pages: 3 transparent; anything else white (a page is paper).
// Set before rendering, not while it renders.
void nitropdf_set_background(nitropdf *pdf, int background);
// Size of a page in points (1/72 inch, times its /UserUnit): its crop box, turned by its /Rotate.
// Render coordinates start at its top left, y down.
void nitropdf_size(const nitropdf *pdf, int page, double *w, double *h);

// Renders a page into a w x h BGRA buffer (premultiplied): pixel (i, j) shows the point
// (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale) of the page, in points. Several renders of the
// same document may run at once (the bands of one): the first one of a page reads its content.
// Returns 0 on success, 1 if cancelled (may be NULL; asked now and then) returned nonzero: then the
// buffer is drawn in part.
int nitropdf_render(nitropdf *pdf, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                    int (*cancelled)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
