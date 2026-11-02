// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrosvg: a small C interface to lunasvg (third_party/lunasvg, MIT; see
// scripts/get-deps.sh) for the viewers: load an SVG, ask its size, render any part
// of it at any scale. No filters (blur, drop shadow ...): lunasvg skips them.
// The calls are serialised internally (under nitro_font's lock: the fonts, shared with
// nitrodxf and nitrocdr, have glyph caches without a lock), so they can come from any thread.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitrosvg nitrosvg;

// NULL if it is not an SVG lunasvg can parse, or it has no size and draws nothing.
nitrosvg *nitrosvg_load(const void *data, size_t len);
void nitrosvg_free(nitrosvg *svg);

// Intrinsic size in CSS pixels (width / height / viewBox; without those: the
// bounding box of what it draws).
void nitrosvg_size(const nitrosvg *svg, double *w, double *h);

// The background it is drawn over: 0 white (the default), 1 dark grey (AutoCAD's), 2 black, as
// for DXF and CorelDRAW drawings. Set before rendering, not while it renders.
void nitrosvg_set_background(nitrosvg *svg, int background);

// Renders into a w x h BGRA buffer (premultiplied, as Metal's BGRA8Unorm expects)
// over the opaque background: pixel (i, j) shows the SVG point
// (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale), in the units of nitrosvg_size.
// Returns 0 on success.
int nitrosvg_render(nitrosvg *svg, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0);

#ifdef __cplusplus
}
#endif
