// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrocdr: CorelDRAW (CDR, CMX) drawings for the viewers. libcdr (MPL-2.0, see
// scripts/get-deps.sh) parses the file and describes it through librevenge's drawing
// interface; nitrocdr records that as a display list (paths, styles, text, images, in
// points with y down) and draws it with plutovg, at any scale and offset, like
// nitrosvg. Embedded bitmaps stay decoded in memory, not as text.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitrocdr nitrocdr;

// NULL if it is not a drawing libcdr can read, or it has no page.
nitrocdr *nitrocdr_load(const void *data, size_t len);
void nitrocdr_free(nitrocdr *cdr);

int nitrocdr_pages(const nitrocdr *cdr);
// The background it is drawn over: 0 white (the default), 1 dark grey (AutoCAD's), 2 black, as for
// SVG and DXF drawings. Set before rendering, not while it renders.
void nitrocdr_set_background(nitrocdr *cdr, int background);
// Size of the shown area in points (1/72 inch): the drawing's bounding box with a small
// margin (not the page size set in the file). Render coordinates below start at its top left.
void nitrocdr_size(const nitrocdr *cdr, int page, double *w, double *h);

// Renders a page into a w x h BGRA buffer (premultiplied) over the opaque
// background: pixel (i, j) shows the point (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale) of the
// shown area, in points. Returns 0 on success.
int nitrocdr_render(nitrocdr *cdr, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0);

#ifdef __cplusplus
}
#endif
