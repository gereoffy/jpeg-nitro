// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_vector: the vector drawings for the four viewers, SVG (nitrosvg), DXF (nitrodxf) and
// CorelDRAW CDR / CMX (nitrocdr), behind one interface: which files they are, loading one, its
// pages and their size, rendering any part at any scale, the size a page is rendered at in
// full, and the background they are drawn over (keys E / Shift+E). Compiled into each viewer
// with its NV_SVG / NV_CDR; without either, nothing opens. The viewers keep what is theirs:
// pixel buffers and textures, threads, the sharp detail drawn when zoomed in.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "nitro_load.h"   // NvDecoder: the buffer callbacks a page is rendered into

#if defined(NV_SVG) || defined(NV_CDR)
#define NV_VECTOR 1   // a viewer with vector drawings: rendered again, sharp, when zoomed in
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The file extensions of the drawings this build opens (NULL-terminated, lower case, without
// the dot): what a viewer opens from a folder beside nv_image_exts, and lists in its file dialog.
extern const char *const nv_vector_exts[];

// A loaded drawing, shared by the images made from it (its pages, the sharp detail of a zoomed-in
// view rendered on another thread): reference counted, freed by whichever drops it last.
typedef struct NvVector NvVector;

// Loads a drawing: by the extension of name (a file name or path; SVG also by content: text
// starting with '<'), over
// the current background (nv_vector_background). NULL if it is not one, or it draws nothing.
// One reference.
NvVector *nv_vector_open(const char *name, const void *data, size_t len);
NvVector *nv_vector_ref(NvVector *v);   // (NULL stays NULL)
void nv_vector_unref(NvVector *v);

int nv_vector_pages(const NvVector *v);        // 0 for NULL
int nv_vector_mode(const NvVector *v);         // NV_MODE_SVG / NV_MODE_DXF / NV_MODE_META / NV_MODE_CDR / NV_MODE_VISIO (nitro_load.h)
int nv_vector_background_of(const NvVector *v);   // the background it was loaded with
// Size of a page in its own units (render coordinates start at its top left, y down).
void nv_vector_size(const NvVector *v, int page, double *w, double *h);
// Renders w x h pixels of a page into a BGRA buffer (premultiplied, opaque): pixel (i, j) shows
// the point (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale). 0 on success. Any thread, several
// at once. cancelled (may be NULL) is asked now and then while it renders: nonzero stops it
// (DXF and CorelDRAW; an SVG renders to its end), and it returns nonzero.
int nv_vector_render(NvVector *v, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx);
// The threads a render may use (the viewers' -j): n bands at most, 1 on the calling thread alone;
// 0 (the default): as many as the CPU's threads. (Also those a CorelDRAW / Visio document's
// pictures are decoded with while it loads.)
void nv_vector_set_threads(int n);

// The size a page is rendered at in full: the long side 1024..4096 pixels (small icons enlarged,
// huge drawings reduced), at least the target tw x th (the screen, when shown fitted to it; 0:
// none), at most max_tex on a side. Returns the scale (pixels per document unit) and the pixel
// size; 0 (nothing to draw) if the page has no size.
double nv_vector_page_size(const NvVector *v, int page, int tw, int th, int max_tex, int *pw, int *ph);
// A page rendered pw x ph pixels is too small for a target of tw x th now (nv_vector_page_size
// would render it larger): rendered again.
int nv_vector_too_small(int pw, int ph, int tw, int th, int max_tex);

// The target the pages are rendered for (nv_vector_render_page): the screen's size in pixels while
// the viewer shows images fitted to it (full screen or key 0: the pages are then rendered at least
// that large, the prefetched ones too, sharp from the start rather than enlarged and sharpened a
// moment later by the detail render), else 0 x 0. Set by the main thread, read by any; returns
// whether it changed (the viewer then drops the prefetched pages too small for it).
int nv_vector_set_target(int tw, int th);
// nv_vector_too_small for that target.
int nv_vector_page_too_small(int pw, int ph, int max_tex);

// A page rendered in full, as the viewers show a drawing: the pixels of nv_vector_page_size (for
// the target, and dec->max_tex), BGRA, over the background it was loaded with, in a buffer from
// dec's callbacks (get, put, ctx and align as nv_decode uses them, nitro_load.h: the rows' pitch
// is w * 4 rounded up to align). Returns the buffer and fills p; NULL if the page has no size, no
// buffer came, or the render failed (the buffer is put back).
typedef struct {
    int w, h;               // the pixels
    size_t pitch, cap;      // the rows' pitch; the buffer's size (from get)
    double scale;           // pixels per document unit
    int mode, background;   // nv_vector_mode, nv_vector_background_of
} NvPage;
uint8_t *nv_vector_render_page(NvVector *v, int page, const NvDecoder *dec, NvPage *p);

// A drawing zoomed in beyond its decoded size (more than one device pixel per decoded pixel): the
// viewers render the visible part again at screen resolution, sharp, and draw it over the image.
// That part: of an image of iw x ih decoded pixels shown at scale device pixels per decoded
// pixel, its top left corner at ox, oy (device pixels, whole numbers) in a view of vw x vh,
// what is visible plus margin device pixels around it (for panning; 0: none), clipped to the
// image. Returns whether there is any.
typedef struct {
    double ox, oy;           // the image's top left corner on the screen (device pixels)
    int x, y, w, h;          // the part: from that corner, in device pixels (its size on the screen)
    double dx, dy, dw, dh;   // the same in decoded pixels
    double scale;            // device pixels per decoded pixel
} NvDetailRect;
int nv_vector_detail_rect(double iw, double ih, double scale, double ox, double oy, double vw, double vh, double margin,
                          NvDetailRect *r);
// That part of a page rendered: r->w x r->h BGRA pixels (pitch r->w * 4; from malloc, free them),
// sharp at r->scale device pixels per decoded pixel of a page decoded at vscale pixels per
// document unit. Any thread. NULL: no memory, or cancelled meanwhile (as nv_vector_render's).
uint8_t *nv_vector_detail_render(NvVector *v, int page, double vscale, const NvDetailRect *r, int (*cancelled)(void *ctx),
                                 void *ctx);

// The background the drawings loaded from now on are drawn over: 0 white, 1 dark grey (AutoCAD's
// model space), 2 black. Any thread.
int nv_vector_background(void);
// The next (delta > 0) or previous one, cyclically; returns the new setting.
int nv_vector_cycle_background(int delta);
// For the window title: "" for white, else "dark background" / "black background".
const char *nv_vector_background_name(int background);

#ifdef __cplusplus
}
#endif
