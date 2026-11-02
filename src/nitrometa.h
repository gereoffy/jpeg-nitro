// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrometa: Windows metafiles, WMF (with or without the placeable header) and EMF, for the
// viewers and for the drawings that embed them (the pictures of Visio's stencils, CorelDRAW's
// OLE objects). Our own reader turns the GDI records (pens, brushes, polygons, Béziers, arcs,
// paths, text, DIB bitmaps, gradient fills, world transforms) into a display list in the
// metafile's device space, drawn with plutovg (the one of nitrosvg) at any scale, like nitrodxf.
// EMF+ records (GDI+ drawings inside EMF comments) are not read: dual files draw from their GDI part.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitrometa nitrometa;
struct plutovg_canvas;   // (plutovg.h: the renderers' canvas; the viewers need no plutovg)

// NULL if it is not a metafile or it draws nothing. Takes nitro_font's lock itself (the glyphs of
// its text): not to be called holding it.
nitrometa *nitrometa_load(const void *data, size_t len);
void nitrometa_free(nitrometa *meta);

// The size of its frame in points (1/72 inch): EMF from its frame in 0.01 mm, a placeable WMF from
// its bounding box and its units per inch, another WMF as if its device had 96 pixels per inch.
void nitrometa_size(const nitrometa *meta, double *w, double *h);

// Draws it into the rectangle x, y, w, h of the canvas (in the canvas's current coordinates), its
// frame stretched to the rectangle and clipped to it; needs no lock.
void nitrometa_draw(const nitrometa *meta, struct plutovg_canvas *canvas, float x, float y, float w, float h);

// The viewers' interface (as nitrodxf's): the background 0 white, 1 dark grey, 2 black; a render
// of w x h pixels, pixel (i, j) showing the point (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale)
// of the frame in points; 0 on success, 1 if cancelled (may be NULL) returned nonzero meanwhile.
void nitrometa_set_background(nitrometa *meta, int background);
int nitrometa_render(nitrometa *meta, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
