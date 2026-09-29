// SPDX-License-Identifier: MIT
#pragma once

#include <gio/gio.h>
#include <gdk/gdk.h>

typedef struct {
    GdkTexture *texture;   // the pixels as stored in the file
    int orientation;       // EXIF orientation (1..8), applied when drawing
    int width;             // display size (after the orientation)
    int height;
    int source_width;
    int source_height;
    double decode_ms;
} NitroImage;

void nitro_image_set_threads(int threads);
NitroImage *nitro_image_load(GFile *file, GError **error);
void nitro_image_free(NitroImage *image);
