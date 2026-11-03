// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrocdr: CorelDRAW (CDR, CMX) and Visio (VSD, VDX, VSDX, stencils and templates) drawings
// for the viewers. libcdr and libvisio (MPL-2.0, see scripts/get-deps.sh) parse the file and
// describe it through librevenge's drawing interface; nitrocdr records that as a display list
// (paths, styles, text, images, in points with y down) and draws it with plutovg, at any scale
// and offset, like nitrosvg. Embedded bitmaps stay decoded in memory, not as text.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitrocdr nitrocdr;

// NULL if it is not a drawing libcdr or libvisio can read, or it has no page.
nitrocdr *nitrocdr_load(const void *data, size_t len);
void nitrocdr_free(nitrocdr *cdr);
// The threads loading may decode a document's pictures with (the viewers' -j; 0, the default: as
// many as the CPU's threads). Any thread.
void nitrocdr_set_threads(int n);

int nitrocdr_pages(const nitrocdr *cdr);
// The background it is drawn over: 0 white (the default), 1 dark grey (AutoCAD's), 2 black, as for
// SVG and DXF drawings. Set before rendering, not while it renders.
void nitrocdr_set_background(nitrocdr *cdr, int background);
// Size of the shown area in points (1/72 inch): the drawing's bounding box with a small
// margin (not the page size set in the file). Render coordinates below start at its top left.
void nitrocdr_size(const nitrocdr *cdr, int page, double *w, double *h);

// Renders a page into a w x h BGRA buffer (premultiplied) over the opaque
// background: pixel (i, j) shows the point (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale) of the
// shown area, in points. Returns 0 on success, 1 if cancelled (may be NULL; asked now and then
// while it renders) returned nonzero: then the buffer is drawn in part.
int nitrocdr_render(nitrocdr *cdr, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                    int (*cancelled)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
