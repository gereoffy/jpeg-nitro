// SPDX-License-Identifier: MIT
#include "nitro-image.h"

#include "../nitrojpeg.h"
#include "../nitropng.h"
#include "../nitropsd.h"

#include <math.h>
#include <string.h>

#define NITRO_READ_PADDING 64

static int decode_threads;

typedef struct {
    guint8 *rgba;
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

static gboolean checked_rgba_size(int width, int height, gsize *stride, gsize *size, GError **error) {
    if (width <= 0 || height <= 0 || (gsize)width > G_MAXSIZE / 4 ||
        (gsize)height > G_MAXSIZE / ((gsize)width * 4)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid image dimensions");
        return FALSE;
    }

    *stride = (gsize)width * 4;
    *size = *stride * (gsize)height;
    return TRUE;
}

static guint8 clamp_u8(float value) {
    if (value <= 0.0f) return 0;
    if (value >= 255.0f) return 255;
    return (guint8)(value + 0.5f);
}

static float sample_plane(const guint8 *plane, gsize pitch, int width, int height, float x, float y) {
    float fx = x - 0.5f;
    float fy = y - 0.5f;
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    float wx = fx - (float)x0;
    float wy = fy - (float)y0;
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    x0 = CLAMP(x0, 0, width - 1);
    x1 = CLAMP(x1, 0, width - 1);
    y0 = CLAMP(y0, 0, height - 1);
    y1 = CLAMP(y1, 0, height - 1);

    float a = plane[(gsize)y0 * pitch + (gsize)x0];
    float b = plane[(gsize)y0 * pitch + (gsize)x1];
    float c = plane[(gsize)y1 * pitch + (gsize)x0];
    float d = plane[(gsize)y1 * pitch + (gsize)x1];
    float top = a + (b - a) * wx;
    float bottom = c + (d - c) * wx;
    return top + (bottom - top) * wy;
}

static gboolean decode_jpeg(const guint8 *data, gsize len, DecodedPixels *out, GError **error) {
    nj_info info;
    if (nj_read_info(data, len, &info) != 0 || !info.supported) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported JPEG");
        return FALSE;
    }

    guint8 *planes[3] = {NULL, NULL, NULL};
    gsize pitch[3] = {0, 0, 0};

    for (int c = 0; c < info.ncomp; c++) {
        if (info.plane_w[c] <= 0 || info.plane_h[c] <= 0 ||
            (gsize)info.plane_h[c] > G_MAXSIZE / (gsize)info.plane_w[c]) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid JPEG plane dimensions");
            goto fail;
        }
        pitch[c] = (gsize)info.plane_w[c];
        planes[c] = g_malloc(pitch[c] * (gsize)info.plane_h[c]);
    }

    nj_stats stats;
    if (nj_decode_planes(data, len, &info, planes, pitch, decode_threads, &stats) != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "JPEG decode failed");
        goto fail;
    }

    gsize stride, size;
    if (!checked_rgba_size(info.width, info.height, &stride, &size, error)) goto fail;
    guint8 *rgba = g_malloc(size);

    for (int y = 0; y < info.height; y++) {
        guint8 *dst = rgba + (gsize)y * stride;
        for (int x = 0; x < info.width; x++) {
            float yy = planes[0][(gsize)y * pitch[0] + (gsize)x];
            float r = yy, g = yy, b = yy;

            if (info.ncomp == 3) {
                float cbx = ((float)x + 0.5f) * (float)info.h[1] / (float)info.hmax;
                float cby = ((float)y + 0.5f) * (float)info.v[1] / (float)info.vmax;
                float crx = ((float)x + 0.5f) * (float)info.h[2] / (float)info.hmax;
                float cry = ((float)y + 0.5f) * (float)info.v[2] / (float)info.vmax;
                float cb = sample_plane(planes[1], pitch[1], info.plane_w[1], info.plane_h[1], cbx, cby) - 128.0f;
                float cr = sample_plane(planes[2], pitch[2], info.plane_w[2], info.plane_h[2], crx, cry) - 128.0f;
                r = yy + 1.402f * cr;
                g = yy - 0.344136f * cb - 0.714136f * cr;
                b = yy + 1.772f * cb;
            }

            dst[(gsize)x * 4 + 0] = clamp_u8(r);
            dst[(gsize)x * 4 + 1] = clamp_u8(g);
            dst[(gsize)x * 4 + 2] = clamp_u8(b);
            dst[(gsize)x * 4 + 3] = 255;
        }
    }

    for (int c = 0; c < info.ncomp; c++) g_free(planes[c]);
    out->rgba = rgba;
    out->width = info.width;
    out->height = info.height;
    out->orientation = info.orientation;
    return TRUE;

fail:
    for (int c = 0; c < 3; c++) g_free(planes[c]);
    return FALSE;
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

    gsize stride, size;
    if (!checked_rgba_size(info.width, info.height, &stride, &size, error)) {
        g_free(raw);
        return FALSE;
    }
    guint8 *rgba = g_malloc(size);

    for (int y = 0; y < info.height; y++) {
        const guint8 *src = raw + (gsize)y * (info.stride + 1) + 1;
        guint8 *dst = rgba + (gsize)y * stride;
        for (int x = 0; x < info.width; x++) {
            const guint8 *p = src + (gsize)x * (gsize)info.channels;
            guint8 r, g, b, a = 255;
            if (info.channels == 4) { r = p[0]; g = p[1]; b = p[2]; a = p[3]; }
            else if (info.channels == 3) { r = p[0]; g = p[1]; b = p[2]; }
            else if (info.channels == 2) { r = g = b = p[0]; a = p[1]; }
            else { r = g = b = p[0]; }
            dst[(gsize)x * 4 + 0] = r;
            dst[(gsize)x * 4 + 1] = g;
            dst[(gsize)x * 4 + 2] = b;
            dst[(gsize)x * 4 + 3] = a;
        }
    }

    g_free(raw);
    out->rgba = rgba;
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
    if (!checked_rgba_size(info.width, info.height, &stride, &size, error)) {
        g_free(raw);
        return FALSE;
    }
    guint8 *rgba = g_malloc(size);

    const guint8 *p0 = raw;
    const guint8 *p1 = info.ncolor == 3 ? raw + info.plane_size : p0;
    const guint8 *p2 = info.ncolor == 3 ? raw + info.plane_size * 2 : p0;
    const guint8 *pa = info.alpha ? raw + info.plane_size * (gsize)info.ncolor : NULL;

    for (gsize i = 0; i < info.plane_size; i++) {
        int r = p0[i], g = p1[i], b = p2[i];
        if (pa) {
            int a = pa[i];
            r = MAX(r + a - 255, 0);
            g = MAX(g + a - 255, 0);
            b = MAX(b + a - 255, 0);
        }
        rgba[i * 4 + 0] = (guint8)r;
        rgba[i * 4 + 1] = (guint8)g;
        rgba[i * 4 + 2] = (guint8)b;
        rgba[i * 4 + 3] = 255;
    }

    g_free(raw);
    out->rgba = rgba;
    out->width = info.width;
    out->height = info.height;
    out->orientation = 1;
    return TRUE;
}

static guint8 *apply_orientation(guint8 *src, int width, int height, int orientation,
                                 int *out_width, int *out_height, GError **error) {
    if (orientation < 2 || orientation > 8) {
        *out_width = width;
        *out_height = height;
        return src;
    }

    int dw = orientation >= 5 ? height : width;
    int dh = orientation >= 5 ? width : height;
    gsize stride, size;
    if (!checked_rgba_size(dw, dh, &stride, &size, error)) {
        g_free(src);
        return NULL;
    }

    guint8 *dst = g_malloc(size);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int dx = x, dy = y;
            switch (orientation) {
            case 2: dx = width - 1 - x; dy = y; break;
            case 3: dx = width - 1 - x; dy = height - 1 - y; break;
            case 4: dx = x; dy = height - 1 - y; break;
            case 5: dx = y; dy = x; break;
            case 6: dx = height - 1 - y; dy = x; break;
            case 7: dx = height - 1 - y; dy = width - 1 - x; break;
            case 8: dx = y; dy = width - 1 - x; break;
            default: break;
            }
            memcpy(dst + ((gsize)dy * dw + dx) * 4,
                   src + ((gsize)y * width + x) * 4, 4);
        }
    }

    g_free(src);
    *out_width = dw;
    *out_height = dh;
    return dst;
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
    gboolean ok;
    if (len >= 3 && data[0] == 0xff && data[1] == 0xd8)
        ok = decode_jpeg(data, len, &pixels, error);
    else if (len >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        ok = decode_png(data, len, &pixels, error);
    else if (len >= 4 && memcmp(data, "8BPS", 4) == 0)
        ok = decode_psd(data, len, &pixels, error);
    else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Not a supported JPEG, PNG, PSD or PSB image");
        ok = FALSE;
    }
    g_free(data);
    if (!ok) return NULL;

    int display_width, display_height;
    guint8 *rgba = apply_orientation(pixels.rgba, pixels.width, pixels.height, pixels.orientation,
                                     &display_width, &display_height, error);
    if (!rgba) return NULL;

    gsize stride, size;
    if (!checked_rgba_size(display_width, display_height, &stride, &size, error)) {
        g_free(rgba);
        return NULL;
    }

    GBytes *bytes = g_bytes_new_take(rgba, size);
    GdkTexture *texture = gdk_memory_texture_new(display_width, display_height,
                                                 GDK_MEMORY_R8G8B8A8, bytes, stride);
    g_bytes_unref(bytes);

    NitroImage *image = g_new0(NitroImage, 1);
    image->texture = texture;
    image->width = display_width;
    image->height = display_height;
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
