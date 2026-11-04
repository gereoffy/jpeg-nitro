// SPDX-License-Identifier: MIT
#include "nitro-image.h"

#include <gtk/gtk.h>   // GTK_CHECK_VERSION

#include "../nitro_load.h"      // reading and decoding with our decoders (the library)
#include "../nitro_convert.h"   // their planes -> RGB (the library too)
#include "../nitrojpeg.h"    // nj_set_max_workers

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NITRO_READ_PADDING 64

static int decode_threads;
static gint dng_ev3;   // DNG exposure correction in 1/3 EV (key E / Shift+E)

// ---------------------------------------------------------------------------
// Vector drawings (nitro_vector: SVG, DXF, CorelDRAW): rendered at a fixed size here, the visible
// part again at screen resolution when zoomed in (nitro-view.c). Without nitrosvg, SVG goes
// through GTK's loaders.

static int vector_cancelled(void *cancellable) { return g_cancellable_is_cancelled(cancellable); }

// A texture of w x h BGRA pixels (premultiplied) from malloc, stride bytes per row (freed with it).
static GdkTexture *bgra_texture(guint8 *px, int w, int h, gsize stride) {
    GBytes *bytes = g_bytes_new_with_free_func(px, stride * (gsize)h, free, px);
    GdkTexture *t = gdk_memory_texture_new(w, h, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, stride);
    g_bytes_unref(bytes);
    return t;
}

GdkTexture *nitro_vector_detail(NitroVector *vector, int page, double vector_scale, const NvDetailRect *r, GCancellable *cancellable) {
    guint8 *px = vector ? nv_vector_detail_render(vector, page, vector_scale, r, cancellable ? vector_cancelled : NULL, cancellable) : NULL;
    return px ? bgra_texture(px, r->w, r->h, (gsize)r->w * 4) : NULL;
}

// A page rendered in full (nv_vector_render_page), as the other viewers do (takes a reference
// to vector).
static uint8_t *page_get(size_t len, size_t *cap, void *ctx) { *cap = len; return malloc(len); }
static void page_put(uint8_t *p, size_t cap, void *ctx) { free(p); }

static NitroImage *vector_image(NitroVector *vector, int page) {
    static const NvDecoder dec = {.get = page_get, .put = page_put, .align = 1, .max_tex = 16384};
    NvPage p;
    guint8 *px = nv_vector_render_page(vector, page, &dec, &p);
    if (!px) return NULL;
    NitroImage *image = g_new0(NitroImage, 1);
    image->texture = bgra_texture(px, p.w, p.h, p.pitch);
    image->vector = nv_vector_ref(vector);
    image->page = page;
    image->vector_scale = p.scale;
    image->background = p.background;
    image->orientation = 1;
    image->width = image->source_width = p.w;
    image->height = image->source_height = p.h;
    return image;
}

NitroImage *nitro_image_load_page(NitroVector *vector, int page) {
    if (!vector || page < 0 || page >= nv_vector_pages(vector)) return NULL;
    gint64 start = g_get_monotonic_time();
    NitroImage *image = vector_image(vector, page);
    if (image) image->decode_ms = (g_get_monotonic_time() - start) / 1000.0;
    return image;
}

// A drawing (an SVG, DXF or CorelDRAW file: nv_vector_open); NULL if it is not one.
static NitroImage *load_vector(GFile *file, const guint8 *data, gsize len) {
    char *name = g_file_get_basename(file);
    NitroVector *vector = name ? nv_vector_open(name, data, len) : NULL;
    g_free(name);
    if (!vector) return NULL;
    NitroImage *image = vector_image(vector, 0);
    nv_vector_unref(vector);   // (the image keeps it)
    return image;
}

#if !GTK_CHECK_VERSION(4, 10, 0)
static void mips_free(gpointer data) {   // (see NitroMips)
    NitroMips *m = data;
    for (int i = 0; i < m->n; i++) g_object_unref(m->t[i]);
    g_free(m);
}

static void attach_mips(GdkTexture *texture) {
    int w = gdk_texture_get_width(texture), h = gdk_texture_get_height(texture);
    if (MAX(w, h) <= 1024 || (gsize)w > G_MAXSIZE / 4 / (gsize)h) return;
    gsize stride = (gsize)w * 4;
    guint8 *first = g_try_malloc(stride * (gsize)h);
    if (!first) return;
    gdk_texture_download(texture, first, stride);   // (B G R A premultiplied: GDK_MEMORY_DEFAULT)
    const guint8 *src = first;
    NitroMips *m = g_new0(NitroMips, 1);
    while (MAX(w, h) > 512 && m->n < 16) {
        int nw = (w + 1) / 2, nh = (h + 1) / 2;
        gsize ns = (gsize)nw * 4;
        guint8 *dst = g_try_malloc(ns * (gsize)nh);
        if (!dst) break;
        for (int y = 0; y < nh; y++) {
            const guint8 *r0 = src + (gsize)(2 * y) * stride, *r1 = src + (gsize)MIN(2 * y + 1, h - 1) * stride;
            guint8 *o = dst + (gsize)y * ns;
            for (int x = 0; x < nw; x++) {
                int a = 8 * x, b = 4 * MIN(2 * x + 1, w - 1);
                for (int c = 0; c < 4; c++) o[4 * x + c] = (guint8)((r0[a + c] + r0[b + c] + r1[a + c] + r1[b + c] + 2) >> 2);
            }
        }
        GBytes *bytes = g_bytes_new_take(dst, ns * (gsize)nh);
        m->t[m->n++] = gdk_memory_texture_new(nw, nh, GDK_MEMORY_DEFAULT, bytes, ns);
        g_bytes_unref(bytes);   // (the texture keeps it: the next level reads it)
        src = dst;
        w = nw;
        h = nh;
        stride = ns;
    }
    g_free(first);
    if (m->n) g_object_set_data_full(G_OBJECT(texture), NITRO_MIPS, m, mips_free);
    else g_free(m);
}
#endif

void nitro_image_set_dng_ev3(int ev3) { g_atomic_int_set(&dng_ev3, ev3); }
int nitro_image_get_dng_ev3(void) { return g_atomic_int_get(&dng_ev3); }

void nitro_image_set_threads(int threads) {
    decode_threads = MAX(threads, 0);
    nj_set_max_workers(decode_threads);
    nv_vector_set_threads(decode_threads);
}

static uint8_t *image_get(size_t len, size_t *cap, void *ctx) {
    (void)ctx;
    *cap = len;
    return g_try_malloc(len);
}
static void image_put(uint8_t *p, size_t cap, void *ctx) {
    (void)cap, (void)ctx;
    g_free(p);
}

// The decoded pixels (nitro_load's NvImage in buf, which this takes) as a texture: in a format
// GdkMemoryTexture takes as it is where there is one (BGRX, a PNG's rows), else converted to RGB.
static GdkTexture *texture_of(const NvImage *img, uint8_t *buf, GError **error) {
    gsize stride = (gsize)img->width * 3, size = stride * (gsize)img->height;
    GBytes *bytes;
    GdkMemoryFormat format;
    switch (img->kind) {
    case NV_BGRA:
        bytes = g_bytes_new_take(buf, img->size);
        format = GDK_MEMORY_B8G8R8A8;   // (X: 255)
        stride = img->pitch[0];
        break;
    case NV_PNG: {
        // The unfiltered rows are already in a texture format: row y at buf + y * (stride + 1) + 1
        // (after its filter-type byte), so GTK gets them as they are, without a copy.
        switch (img->ncomp) {
        case 4: format = GDK_MEMORY_R8G8B8A8; break;
        case 3: format = GDK_MEMORY_R8G8B8; break;
#if GTK_CHECK_VERSION(4, 12, 0)
        case 2: format = GDK_MEMORY_G8A8; break;
        default: format = GDK_MEMORY_G8; break;
#else
        default: {   // gray (+ alpha) without GTK 4.12's gray formats: expand to RGBA
            gsize rs = (gsize)img->width * 4;
            guint8 *rgba = g_try_malloc(rs * (gsize)img->height);
            if (!rgba) {
                g_free(buf);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
                return NULL;
            }
            for (int y = 0; y < img->height; y++) {
                const guint8 *src = buf + (gsize)y * img->pitch[0] + 1;
                guint8 *dst = rgba + (gsize)y * rs;
                for (int x = 0; x < img->width; x++) {
                    guint8 v = src[x * img->ncomp];
                    dst[4 * x] = dst[4 * x + 1] = dst[4 * x + 2] = v;
                    dst[4 * x + 3] = img->ncomp == 2 ? src[x * 2 + 1] : 255;
                }
            }
            g_free(buf);
            GBytes *b = g_bytes_new_take(rgba, rs * (gsize)img->height);
            GdkTexture *t = gdk_memory_texture_new(img->width, img->height, GDK_MEMORY_R8G8B8A8, b, rs);
            g_bytes_unref(b);
            return t;
        }
#endif
        }
        GBytes *all = g_bytes_new_take(buf, img->size);
        bytes = g_bytes_new_from_bytes(all, 1, img->size - 1);   // skips row 0's filter byte
        g_bytes_unref(all);
        stride = img->pitch[0];
        break;
    }
    default: {   // NV_YUV (JPEG, HEIC, WebP), NV_PLANAR (PSD): RGB (multi-threaded)
        guint8 *rgb = g_try_malloc(size);
        if (!rgb) {
            g_free(buf);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
            return NULL;
        }
        if (img->kind == NV_YUV) nc_yuv_to_rgb(img, buf, rgb, stride);   // identical to libjpeg-turbo
        else nc_psd_to_rgb(img, buf, rgb, stride);
        g_free(buf);
        bytes = g_bytes_new_take(rgb, size);
        format = GDK_MEMORY_R8G8B8;
    }
    }
    GdkTexture *t = gdk_memory_texture_new(img->width, img->height, format, bytes, stride);
    g_bytes_unref(bytes);
    return t;
}

NitroImage *nitro_image_load(GFile *file, GError **error) {
    g_return_val_if_fail(G_IS_FILE(file), NULL);

    gint64 start = g_get_monotonic_time();
    // a local file: nitro_load reads it (a PSD's merged image only, a camera raw's JPEG only);
    // else (a GVfs location) GIO, whose error says why if it cannot
    char *path = g_file_get_path(file);
    size_t len = 0;
    int raw_orientation = 0, raw_read = 0;
    uint8_t *data = path ? nv_read_image(path, &len, G_MAXINT, &raw_orientation, &raw_read) : NULL;
    g_free(path);
    if (!data) {
        gchar *contents = NULL;
        gsize n = 0;
        if (!g_file_load_contents(file, NULL, &contents, &n, NULL, error)) return NULL;
        if (n > G_MAXSIZE - NITRO_READ_PADDING || !(data = malloc(n + NITRO_READ_PADDING))) {
            g_free(contents);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "Image file is too large");
            return NULL;
        }
        memcpy(data, contents, n);
        memset(data + n, 0, NITRO_READ_PADDING);
        g_free(contents);
        len = n;
    }

    NitroImage *vector_image = load_vector(file, data, len);
    if (vector_image) {
        free(data);
        vector_image->decode_ms = (g_get_monotonic_time() - start) / 1000.0;
        return vector_image;
    }

    NvDecoder dec = {image_get, image_put, NULL, 1, G_MAXINT, decode_threads, g_atomic_int_get(&dng_ev3)};
    NvImage img;
    uint8_t *buf = NULL;
    size_t cap = 0;
    int rc = nv_decode(data, &len, raw_orientation, raw_read, &dec, &img, &buf, &cap);
    if (rc < 0) {
        free(data);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Out of memory");
        return NULL;
    }
    GdkTexture *texture = NULL;
    int width, height, orientation = img.orientation;
    if (rc == 0) {
        free(data);
        texture = texture_of(&img, buf, error);
        if (!texture) return NULL;
        width = img.width;
        height = img.height;
    } else {
        // Fallback: GTK's own loaders (CMYK / cut-off JPEG, interlaced PNG, JPEG / YCbCr TIFF, and
        // what the installed gdk-pixbuf loaders read: GIF, BMP, WebP ...). The EXIF orientation
        // still comes from the file header where we can read it (img.orientation).
        GBytes *file_bytes = g_bytes_new_with_free_func(data, len, free, data);   // (the padding is not part of it)
        GError *gdk_error = NULL;
        texture = gdk_texture_new_from_bytes(file_bytes, &gdk_error);
        g_bytes_unref(file_bytes);
        if (!texture) {   // report why our decoder refused it if it is one of ours, else GTK's reason
            if (img.error) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, img.error);
                g_clear_error(&gdk_error);
            } else {
                g_propagate_error(error, gdk_error);
            }
            return NULL;
        }
        width = gdk_texture_get_width(texture);
        height = gdk_texture_get_height(texture);
    }

#if !GTK_CHECK_VERSION(4, 10, 0)
    attach_mips(texture);
#endif
    orientation = orientation >= 1 && orientation <= 8 ? orientation : 1;
    int swap = orientation >= 5;   // EXIF 5..8: rotated by 90 degrees
    NitroImage *image = g_new0(NitroImage, 1);
    image->texture = texture;
    image->orientation = orientation;
    image->width = swap ? height : width;
    image->height = swap ? width : height;
    image->source_width = width;
    image->source_height = height;
    image->dng = rc == 0 && img.mode == NV_MODE_NITRODNG;
    image->ev3 = image->dng ? img.ev3 : 0;
    image->decode_ms = (g_get_monotonic_time() - start) / 1000.0;
    return image;
}

void nitro_image_free(NitroImage *image) {
    if (!image) return;
    g_clear_object(&image->texture);
    g_clear_pointer(&image->vector, nv_vector_unref);
    g_free(image);
}
