// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodio: draw.io (diagrams.net) diagrams for the viewers. Our own reader of the mxGraph XML
// (libxml2; a page compressed as draw.io stores it: base64, raw deflate (zlib), URL encoding):
// each diagram a page, its cells as a display list (shapes, connectors with their arrows, labels
// in px with y down), drawn with plutovg at any scale and offset, like nitrocdr. Labels: plain
// text or the HTML draw.io keeps in them (only bold, italic and the line breaks are kept), in the
// fonts of nitro_font. Shapes of draw.io's stencil libraries (mxgraph.*) and pictures are drawn
// as their frame with the label.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitrodio nitrodio;

// NULL if it is not a draw.io file or it draws nothing.
nitrodio *nitrodio_load(const void *data, size_t len);
void nitrodio_free(nitrodio *dio);

int nitrodio_pages(const nitrodio *dio);
// The background it is drawn over: 0 white (the default), 1 dark grey, 2 black, as for the other
// drawings (a diagram's own background colour is drawn over it). Set before rendering.
void nitrodio_set_background(nitrodio *dio, int background);
// Size of a page's shown area in px: its cells' bounding box with a small margin. Render
// coordinates below start at its top left.
void nitrodio_size(const nitrodio *dio, int page, double *w, double *h);
// Renders a page into a w x h BGRA buffer (premultiplied, opaque): pixel (i, j) shows the point
// (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale) of the shown area. Returns 0 on success, 1 if
// cancelled (may be NULL; asked now and then while it renders) returned nonzero. Thread-safe:
// several renders of the same diagram may run at once.
int nitrodio_render(nitrodio *dio, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                    int (*cancelled)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
