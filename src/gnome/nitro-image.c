// SPDX-License-Identifier: MIT
#include "nitro-image.h"
#include "nitro-convert.h"

#include <gtk/gtk.h>   // GTK_CHECK_VERSION

#include "../nitrojpeg.h"
#include "../nitropng.h"
#include "../nitropsd.h"

#include <string.h>

#define NITRO_READ_PADDING 64

static int decode_threads;

// Decoded pixels in a format GdkMemoryTexture takes as it is (no RGBA copy); the EXIF
// orientation is applied when drawing (nitro-view.c), not by moving pixels.
typedef struct {
    GBytes *bytes;
    GdkMemoryFormat format;
    gsize stride;
    int width;
    int height;
    int orientation;
} DecodedPixels;

static guint16 read_be16(const guint8 *p) {
    return ((guint16)p[0] << 8) | p[1];
}

static guint32 read_be32(const guint8 *p) {
    return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3];
}

static guint16 read_tiff16(const guint8 *p, gboolean little_endian) {
    return little_endian ? ((guint16)p[0] | ((guint16)p[1] << 8)) : read_be16(p);
}

static guint32 read_tiff32(const guint8 *p, gboolean little_endian) {
    if (little_endian)
        return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16) | ((guint32)p[3] << 24);
    return read_be32(p);
}

static int parse_tiff_orientation(const guint8 *data, gsize len) {
    if (len < 8) return 1;

    gboolean little_endian;
    if (data[0] == 'I' && data[1] == 'I') little_endian = TRUE;
    else if (data[0] == 'M' && data[1] == 'M') little_endian = FALSE;
    else return 1;

    if (read_tiff16(data + 2, little_endian) != 42) return 1;
    guint32 ifd = read_tiff32(data + 4, little_endian);
    if ((gsize)ifd + 2 > len) return 1;

    guint16 count = read_tiff16(data + ifd, little_endian);
    for (guint16 i = 0; i < count; i++) {
        gsize entry = (gsize)ifd + 2 + (gsize)i * 12;
        if (entry + 12 > len) break;
        if (read_tiff16(data + entry, little_endian) != 0x0112) continue;

        guint16 type = read_tiff16(data + entry + 2, little_endian);
        guint32 values = read_tiff32(data + entry + 4, little_endian);
        if (type != 3 || values < 1) return 1;

        guint16 orientation = read_tiff16(data + entry + 8, little_endian);
        return orientation >= 1 && orientation <= 8 ? orientation : 1;
    }

    return 1;
}

static int png_orientation(const guint8 *data, gsize len) {
    if (len < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) return 1;

    gsize offset = 8;
    while (offset + 12 <= len) {
        guint32 chunk_len = read_be32(data + offset);
        if ((gsize)chunk_len > len - offset - 12) break;
        const guint8 *type = data + offset + 4;
        const guint8 *chunk = data + offset + 8;

        if (memcmp(type, "eXIf", 4) == 0) return parse_tiff_orientation(chunk, chunk_len);
        if (memcmp(type, "IEND", 4) == 0) break;
        offset += 12 + (gsize)chunk_len;
    }

    return 1;
}

static gboolean checked_size(int width, int height, gsize bpp, gsize *stride, gsize *size, GError **error) {
    if (width <= 0 || height <= 0 || (gsize)width > G_MAXSIZE / bpp ||
        (gsize)height > G_MAXSIZE / ((gsize)width * bpp)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid image dimensions");
        return FALSE;
    }
    *stride = (gsize)width * bpp;
    *size = *stride * (gsize)height;
    return TRUE;
}

static gboolean decode_jpeg(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    nj_info info;
    if (nj_read_info(data, len, &info) != 0 || !info.supported) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported JPEG");
        return FALSE;
    }

    guint8 *planes[3] = {NULL, NULL, NULL};
    gsize pitch[3] = {0, 0, 0};
    gboolean ok = FALSE;

    for (int c = 0; c < info.ncomp; c++) {
        if (info.plane_w[c] <= 0 || info.plane_h[c] <= 0 ||
            (gsize)info.plane_h[c] > G_MAXSIZE / (gsize)info.plane_w[c]) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid JPEG plane dimensions");
            goto done;
        }
        pitch[c] = (gsize)info.plane_w[c];
        planes[c] = g_malloc(pitch[c] * (gsize)info.plane_h[c]);
    }

    // nthreads 0: nitrojpeg picks its piece count itself; -j limits the workers
    // (nj_set_max_workers in nitro_image_set_threads)
    nj_stats stats;
    if (nj_decode_planes(data, len, &info, planes, pitch, 0, &stats) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "JPEG decode failed");
        goto done;
    }

    gsize stride, size;
    if (!checked_size(info.width, info.height, 3, &stride, &size, error)) goto done;
    guint8 *rgb = g_malloc(size);
    nc_jpeg_to_rgb(&info, planes, pitch, rgb, stride);   // multi-threaded, identical to libjpeg-turbo

    out->bytes = g_bytes_new_take(rgb, size);
    out->format = GDK_MEMORY_R8G8B8;
    out->stride = stride;
    out->width = info.width;
    out->height = info.height;
    out->orientation = info.orientation;
    ok = TRUE;

done:
    for (int c = 0; c < 3; c++) g_free(planes[c]);
    return ok;
}

static gboolean decode_png(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    np_info info;
    if (np_read_info(data, len, &info) != 0 || !info.supported) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported PNG");
        return FALSE;
    }

    guint8 *raw = g_malloc(info.raw_size);
    np_stats stats;
    if (np_decode(data, len, &info, raw, decode_threads, &stats) != 0) {
        g_free(raw);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "PNG decode failed");
        return FALSE;
    }

    // The unfiltered rows are already in a texture format: row y at raw + y * (stride + 1) + 1
    // (after its filter-type byte), so GTK gets them as they are, without a copy.
    GdkMemoryFormat format;
    switch (info.channels) {
    case 4: format = GDK_MEMORY_R8G8B8A8; break;
    case 3: format = GDK_MEMORY_R8G8B8; break;
#if GTK_CHECK_VERSION(4, 12, 0)
    case 2: format = GDK_MEMORY_G8A8; break;
    default: format = GDK_MEMORY_G8; break;
#else
    default: {   // gray (+ alpha) without GTK 4.12's gray formats: expand to RGBA
        gsize stride, size;
        if (!checked_size(info.width, info.height, 4, &stride, &size, error)) { g_free(raw); return FALSE; }
        guint8 *rgba = g_malloc(size);
        for (int y = 0; y < info.height; y++) {
            const guint8 *src = raw + (gsize)y * (info.stride + 1) + 1;
            guint8 *dst = rgba + (gsize)y * stride;
            for (int x = 0; x < info.width; x++) {
                guint8 v = src[x * info.channels];
                dst[4 * x] = dst[4 * x + 1] = dst[4 * x + 2] = v;
                dst[4 * x + 3] = info.channels == 2 ? src[x * 2 + 1] : 255;
            }
        }
        g_free(raw);
        out->bytes = g_bytes_new_take(rgba, size);
        out->format = GDK_MEMORY_R8G8B8A8;
        out->stride = stride;
        out->width = info.width;
        out->height = info.height;
        out->orientation = png_orientation(data, len);
        return TRUE;
    }
#endif
    }
    GBytes *all = g_bytes_new_take(raw, info.raw_size);
    out->bytes = g_bytes_new_from_bytes(all, 1, info.raw_size - 1);   // skips row 0's filter byte
    g_bytes_unref(all);
    out->format = format;
    out->stride = info.stride + 1;
    out->width = info.width;
    out->height = info.height;
    out->orientation = png_orientation(data, len);
    return TRUE;
}

static gboolean decode_psd(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    ps_info info;
    if (ps_read_info(data, len, &info) != 0 || !info.supported) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported PSD/PSB");
        return FALSE;
    }

    gsize plane_count = (gsize)(info.ncolor + info.alpha);
    if (plane_count == 0 || info.plane_size > G_MAXSIZE / plane_count) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid PSD plane dimensions");
        return FALSE;
    }

    guint8 *raw = g_malloc(info.plane_size * plane_count);
    ps_stats stats;
    if (ps_decode(data, len, &info, raw, decode_threads, &stats) != 0) {
        g_free(raw);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "PSD decode failed");
        return FALSE;
    }

    gsize stride, size;
    if (!checked_size(info.width, info.height, 3, &stride, &size, error)) {
        g_free(raw);
        return FALSE;
    }
    guint8 *rgb = g_malloc(size);
    nc_psd_to_rgb(&info, raw, rgb, stride);   // multi-threaded; transparency over black
    g_free(raw);

    out->bytes = g_bytes_new_take(rgb, size);
    out->format = GDK_MEMORY_R8G8B8;
    out->stride = stride;
    out->width = info.width;
    out->height = info.height;
    out->orientation = 1;
    return TRUE;
}

void nitro_image_set_threads(int threads) {
    decode_threads = MAX(threads, 0);
    nj_set_max_workers(decode_threads);
}

NitroImage *nitro_image_load(GFile *file, GError **error) {
    g_return_val_if_fail(G_IS_FILE(file), NULL);

    gint64 start = g_get_monotonic_time();
    gchar *contents = NULL;
    gsize len = 0;
    if (!g_file_load_contents(file, NULL, &contents, &len, NULL, error)) return NULL;

    if (len > G_MAXSIZE - NITRO_READ_PADDING) {
        g_free(contents);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE, "Image file is too large");
        return NULL;
    }

    guint8 *data = g_malloc(len + NITRO_READ_PADDING);
    memcpy(data, contents, len);
    memset(data + len, 0, NITRO_READ_PADDING);
    g_free(contents);

    DecodedPixels pixels = {0};
    GError *nitro_error = NULL;
    gboolean ok = FALSE;
    if (len >= 3 && data[0] == 0xff && data[1] == 0xd8)
        ok = decode_jpeg(data, len, &pixels, &nitro_error);
    else if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        ok = decode_png(data, len, &pixels, &nitro_error);
    else if (len >= 4 && memcmp(data, "8BPS", 4) == 0)
        ok = decode_psd(data, len, &pixels, &nitro_error);

    GdkTexture *texture = NULL;
    if (ok) {
        texture = gdk_memory_texture_new(pixels.width, pixels.height, pixels.format, pixels.bytes, pixels.stride);
        g_bytes_unref(pixels.bytes);
        g_clear_error(&nitro_error);
        g_free(data);
    } else {
        // Fallback: GTK's own loaders (progressive / CMYK JPEG, palette / 16-bit / interlaced
        // PNG, TIFF, and what the installed gdk-pixbuf loaders read: GIF, BMP, WebP ...).
        // The EXIF orientation still comes from the file header where we can read it.
        pixels.orientation = 1;
        if (len >= 3 && data[0] == 0xff && data[1] == 0xd8) {
            nj_info ji;
            if (nj_read_info(data, len, &ji) == 0) pixels.orientation = ji.orientation;
        } else if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
            pixels.orientation = png_orientation(data, len);
        }
        GBytes *file_bytes = g_bytes_new_take(data, len);   // (the padding is not part of it)
        GError *gdk_error = NULL;
        texture = gdk_texture_new_from_bytes(file_bytes, &gdk_error);
        g_bytes_unref(file_bytes);
        if (!texture) {
            // report why our decoder refused it if it is one of ours, else GTK's reason
            if (nitro_error) {
                g_propagate_error(error, nitro_error);
                g_clear_error(&gdk_error);
            } else {
                g_propagate_error(error, gdk_error);
            }
            return NULL;
        }
        g_clear_error(&nitro_error);
        pixels.width = gdk_texture_get_width(texture);
        pixels.height = gdk_texture_get_height(texture);
    }

    int orientation = pixels.orientation >= 1 && pixels.orientation <= 8 ? pixels.orientation : 1;
    int swap = orientation >= 5;   // EXIF 5..8: rotated by 90 degrees
    NitroImage *image = g_new0(NitroImage, 1);
    image->texture = texture;
    image->orientation = orientation;
    image->width = swap ? pixels.height : pixels.width;
    image->height = swap ? pixels.width : pixels.height;
    image->source_width = pixels.width;
    image->source_height = pixels.height;
    image->decode_ms = (g_get_monotonic_time() - start) / 1000.0;
    return image;
}

void nitro_image_free(NitroImage *image) {
    if (!image) return;
    g_clear_object(&image->texture);
    g_free(image);
}
