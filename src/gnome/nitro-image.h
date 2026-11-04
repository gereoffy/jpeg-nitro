// SPDX-License-Identifier: MIT
#pragma once

#include <gio/gio.h>
#include <gdk/gdk.h>

#include "../nitro_vector.h"   // the vector drawings, SVG / DXF / CorelDRAW (shared by the viewers)

// A drawing (nitro_vector), shared by the images made from it (the pages of a drawing): rendered
// again, sharp, when zoomed in (see nitro-view.c).
typedef NvVector NitroVector;

typedef struct {
    GdkTexture *texture;   // the pixels (a vector drawing: rendered at a fixed size)
    NitroVector *vector;   // a vector drawing: its document (NULL otherwise) ...
    int page;              // ... the page shown (from 0) ...
    double vector_scale;   // ... texture pixels per document unit ...
    int background;        // ... and the background it was drawn over (nv_vector_background)
    int orientation;       // EXIF orientation (1..8), applied when drawing
    int width;             // display size (after the orientation)
    int height;
    int source_width;
    int source_height;
    int dng, ev3;          // a DNG developed by nitrodng, with this exposure correction (1/3 EV)
    double decode_ms;
    unsigned serial;       // (from nitro_loader_get: the cached image this was copied from)
} NitroImage;

// GTK < 4.10 draws a texture with a plain linear filter, without mipmaps: a photo reduced a lot
// shimmers (moire). So a large image's texture also carries its halves, quarters ... (2x2 box
// filter, object data NITRO_MIPS); the view draws the one at most 2x larger than on screen.
#define NITRO_MIPS "nitro-mips"
#define NITRO_MAX_TEX 16384   // the largest texture (a page of a drawing is rendered up to it)
typedef struct {
    int n;
    GdkTexture *t[16];
} NitroMips;

void nitro_image_set_threads(int threads);
// DNG exposure correction in 1/3 EV for the images decoded from now on (keys E / Shift+E)
void nitro_image_set_dng_ev3(int ev3);
int nitro_image_get_dng_ev3(void);
// The part r of a page (nv_vector_detail_rect) rendered sharp, as a texture, of an image of
// vector_scale texture pixels per document unit (nv_vector_detail_render). NULL on failure, or
// when cancellable (may be NULL) is cancelled meanwhile. Any thread.
GdkTexture *nitro_vector_detail(NitroVector *vector, int page, double vector_scale, const NvDetailRect *r, GCancellable *cancellable);
// Another page of a drawing, as an image (rendered as nitro_image_load does). Any thread.
NitroImage *nitro_image_load_page(NitroVector *vector, int page);
NitroImage *nitro_image_load(GFile *file, GError **error);
void nitro_image_free(NitroImage *image);
