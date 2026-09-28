// SPDX-License-Identifier: MIT
#pragma once

#include <gio/gio.h>
#include <gdk/gdk.h>

typedef struct {
    GdkTexture *texture;
    int width;
    int height;
    int source_width;
    int source_height;
    double decode_ms;
} NitroImage;

void nitro_image_set_threads(int threads);
NitroImage *nitro_image_load(GFile *file, GError **error);
void nitro_image_free(NitroImage *image);
