// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodxf: AutoCAD DXF drawings (ASCII DXF, R12 - 2018; DWG R13 - R2018 but R2007 through
// nitrodwg, as the pairs of its DXF) for the viewers. Our own parser reads the model space
// (layers, blocks, line types, text styles) and the layouts' sheets (the paper space and the
// model space in its viewports) as pages: the model space first, then the layouts in their tabs'
// order (empty ones left out). Each is a display list in drawing units, drawn with
// plutovg (the one of nitrosvg), at any scale and offset, like nitrocdr. Lines are hairlines
// (one device pixel), as AutoCAD shows them on screen, over white (as the SVGs), or AutoCAD's
// dark model space or black (nitrodxf_set_background).
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitrodxf nitrodxf;

// NULL if it is not a DXF (or a DWG read here) or it draws nothing.
nitrodxf *nitrodxf_load(const void *data, size_t len);
void nitrodxf_free(nitrodxf *dxf);

int nitrodxf_pages(const nitrodxf *dxf);
// The background it is drawn over: 0 white (the default; AutoCAD's white lines black, light colours
// darkened), 1 AutoCAD's dark model space, 2 black. Set before rendering, not while it renders.
void nitrodxf_set_background(nitrodxf *dxf, int background);
// Size of a page's shown area in drawing units: its extents (far-off stray objects left out)
// with a small margin; 0 x 0 if there is no such page. Render coordinates below start at its top
// left, y down.
void nitrodxf_size(const nitrodxf *dxf, int page, double *w, double *h);

// Renders a page into a w x h BGRA buffer (premultiplied, opaque): pixel (i, j) shows the point
// (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale) of the shown area. Returns 0 on success, 1 if
// cancelled (may be NULL; asked now and then while it renders) returned nonzero: then the buffer
// is drawn in part. Thread-safe: several renders of the same drawing may run at once.
int nitrodxf_render(nitrodxf *dxf, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                    int (*cancelled)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
