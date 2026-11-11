// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitro_convert: decoded planes -> RGB, multi-threaded. See nitro_convert.h.
#include "nitro_convert.h"
#include "nitro_os.h"

#include <stdlib.h>
#include <string.h>

// libjpeg's YCbCr -> RGB (jdcolor.c): 16-bit fixed point, the same rounding.
#define SCALEBITS 16
#define ONE_HALF ((int32_t)1 << (SCALEBITS - 1))
#define FIX(x) ((int32_t)((x) * (1L << SCALEBITS) + 0.5))

typedef struct {
    int y[256], cr_r[256], cb_b[256];
    int32_t cr_g[256], cb_g[256];
} Tables;

// full range (JPEG, HEIC); limited range (WebP / VP8: Y 16..235, Cb Cr 16..240): the same, scaled
static void build_tables(Tables *t, int limited) {
    double ky = limited ? 255.0 / 219 : 1, kc = limited ? 255.0 / 224 : 1;
    for (int i = 0; i < 256; i++) {
        int32_t x = i - 128;
        t->y[i] = limited ? (int)((FIX(ky) * (i - 16) + ONE_HALF) >> SCALEBITS) : i;
        t->cr_r[i] = (int)((FIX(1.40200 * kc) * x + ONE_HALF) >> SCALEBITS);
        t->cb_b[i] = (int)((FIX(1.77200 * kc) * x + ONE_HALF) >> SCALEBITS);
        t->cr_g[i] = -FIX(0.71414 * kc) * x;
        t->cb_g[i] = -FIX(0.34414 * kc) * x + ONE_HALF;
    }
}

static inline uint8_t clamp255(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

typedef struct {
    const uint8_t *p;
    size_t pitch;
    int cw, ch;   // valid (downsampled) size, as libjpeg computes it
    int hr, vr;   // upsampling factors: hmax / h, vmax / v (0: not a whole number)
    int h, hmax, v, vmax;   // the sampling factors themselves (replication: also for 3 / 2 ...)
} Comp;

// One component's samples for output row y, upsampled to full width, into out
// (at least 2 * cw bytes). cs: scratch of cw ints.
static void upsample_row(const Comp *c, int y, int width, uint8_t *out, int *cs) {
    const int n = c->cw;
    if (c->hr == 1 && c->vr == 1) {
        memcpy(out, c->p + (size_t)y * c->pitch, (size_t)width);
    } else if (c->hr == 2 && c->vr == 1 && n > 2) {   // h2v1 "fancy": 3/4 near + 1/4 far
        const uint8_t *in = c->p + (size_t)y * c->pitch;
        out[0] = in[0];
        out[1] = (uint8_t)((in[0] * 3 + in[1] + 2) >> 2);
        for (int i = 1; i < n - 1; i++) {
            int v = in[i] * 3;
            out[2 * i] = (uint8_t)((v + in[i - 1] + 1) >> 2);
            out[2 * i + 1] = (uint8_t)((v + in[i + 1] + 2) >> 2);
        }
        out[2 * n - 2] = (uint8_t)((in[n - 1] * 3 + in[n - 2] + 1) >> 2);
        out[2 * n - 1] = in[n - 1];
    } else if (c->hr == 2 && c->vr == 2 && n > 2) {   // h2v2 "fancy": the row above / below, then across
        int r = y >> 1, nb = (y & 1) ? (r + 1 < c->ch ? r + 1 : c->ch - 1) : (r > 0 ? r - 1 : 0);
        const uint8_t *in0 = c->p + (size_t)r * c->pitch, *in1 = c->p + (size_t)nb * c->pitch;
        for (int i = 0; i < n; i++) cs[i] = in0[i] * 3 + in1[i];
        out[0] = (uint8_t)((cs[0] * 4 + 8) >> 4);
        out[1] = (uint8_t)((cs[0] * 3 + cs[1] + 7) >> 4);
        for (int i = 1; i < n - 1; i++) {
            out[2 * i] = (uint8_t)((cs[i] * 3 + cs[i - 1] + 8) >> 4);
            out[2 * i + 1] = (uint8_t)((cs[i] * 3 + cs[i + 1] + 7) >> 4);
        }
        out[2 * n - 2] = (uint8_t)((cs[n - 1] * 3 + cs[n - 2] + 8) >> 4);
        out[2 * n - 1] = (uint8_t)((cs[n - 1] * 4 + 7) >> 4);
    } else if (c->hr == 1 && c->vr == 2) {             // h1v2 "fancy"
        int r = y >> 1, nb = (y & 1) ? (r + 1 < c->ch ? r + 1 : c->ch - 1) : (r > 0 ? r - 1 : 0);
        int bias = (y & 1) ? 2 : 1;
        const uint8_t *in0 = c->p + (size_t)r * c->pitch, *in1 = c->p + (size_t)nb * c->pitch;
        for (int x = 0; x < width; x++) out[x] = (uint8_t)((in0[x] * 3 + in1[x] + bias) >> 2);
    } else {                                            // other factors (and tiny images): replication
        const uint8_t *in = c->p + (size_t)y * c->v / c->vmax * c->pitch;
        // h <= hmax: advance by at most one source sample per output pixel.
        // This is exactly floor(x * h / hmax), without a division per pixel.
        int sx = 0, phase = 0;
        for (int x = 0; x < width; x++) {
            out[x] = in[sx];
            phase += c->h;
            if (phase >= c->hmax) { phase -= c->hmax; sx++; }
        }
    }
}

static void comp_init(Comp *c, const uint8_t *p, size_t pitch, int W, int H, int h, int v, int hmax, int vmax) {
    c->p = p;
    c->pitch = pitch;
    c->cw = (W * h + hmax - 1) / hmax;
    c->ch = (H * v + vmax - 1) / vmax;
    c->h = h;
    c->hmax = hmax;
    c->v = v;
    c->vmax = vmax;
    c->hr = hmax % h ? 0 : hmax / h;   // (h 2 of hmax 3: replication)
    c->vr = vmax % v ? 0 : vmax / v;
}

// What convert writes: 3 bytes R G B, or 4 bytes B G R A (A 255). ycc: 3 components are YCbCr (else
// R G B), 4 are YCCK (else C M Y K); inverted: CMYK values as Adobe stores them (255: no ink).
typedef struct {
    int bgra, ycc, inverted, limited;
} Out;

static inline uint8_t mul255(int a, int b) { return (uint8_t)((a * b + 127) / 255); }

static void convert(const Comp *comps, int nc, int W, int H, Out out, uint8_t *dst, size_t stride) {
    size_t bands = (size_t)W * (size_t)H <= 1024 ? 1 : (size_t)nitro_ncpu() * 4;
    if (bands > (size_t)H) bands = (size_t)H;
    int maxcw = W;
    for (int k = 0; k < nc; k++)
        if (2 * comps[k].cw > maxcw) maxcw = 2 * comps[k].cw;
    Tables tab;
    build_tables(&tab, out.limited);
    const Comp *cp = comps;
    const Tables *t = &tab;
    const size_t rowlen = (size_t)maxcw + 16;
    nitro_parallel(bands, ^(size_t b) {
        int y0 = (int)((size_t)H * b / bands), y1 = (int)((size_t)H * (b + 1) / bands);
        uint8_t *rows = malloc(4 * rowlen);
        int *cs = malloc(sizeof(int) * rowlen);
        if (!rows || !cs) { free(rows); free(cs); return; }
        uint8_t *yr = rows, *cbr = rows + rowlen, *crr = rows + 2 * rowlen, *kr = rows + 3 * rowlen;
        const int px = out.bgra ? 4 : 3, ri = out.bgra ? 2 : 0, bi = out.bgra ? 0 : 2;
        for (int y = y0; y < y1; y++) {
            uint8_t *o = dst + (size_t)y * stride;
            const uint8_t *yrow = yr;
            if (cp[0].hr == 1 && cp[0].vr == 1)
                yrow = cp[0].p + (size_t)y * cp[0].pitch;
            else
                upsample_row(&cp[0], y, W, yr, cs);
            if (nc == 1) {
                for (int x = 0; x < W; x++) {
                    o[px * x] = o[px * x + 1] = o[px * x + 2] = yrow[x];
                    if (out.bgra) o[4 * x + 3] = 255;
                }
                continue;
            }
            upsample_row(&cp[1], y, W, cbr, cs);
            upsample_row(&cp[2], y, W, crr, cs);
            if (nc == 4) upsample_row(&cp[3], y, W, kr, cs);
            for (int x = 0; x < W; x++) {
                int r = yrow[x], g = cbr[x], b = crr[x];
                if (out.ycc) {
                    int Y = t->y[r], cb = g, cr = b;
                    r = clamp255(Y + t->cr_r[cr]);
                    g = clamp255(Y + (int)((t->cb_g[cb] + t->cr_g[cr]) >> SCALEBITS));
                    b = clamp255(Y + t->cb_b[cb]);
                    if (nc == 4) r = 255 - r, g = 255 - g, b = 255 - b;   // (YCCK: Y Cb Cr are the inverted C M Y, as libjpeg)
                }
                if (nc == 4) {   // C M Y K -> R G B, the simple way
                    int k = kr[x];
                    if (out.inverted) r = mul255(r, k), g = mul255(g, k), b = mul255(b, k);
                    else r = mul255(255 - r, 255 - k), g = mul255(255 - g, 255 - k), b = mul255(255 - b, 255 - k);
                }
                o[px * x + ri] = (uint8_t)r;
                o[px * x + 1] = (uint8_t)g;
                o[px * x + bi] = (uint8_t)b;
                if (out.bgra) o[4 * x + 3] = 255;
            }
        }
        free(rows);
        free(cs);
    });
}

void nc_yuv_to_rgb(const NvImage *img, const uint8_t *buf, uint8_t *dst, size_t stride) {
    const int W = img->width, H = img->height;
    Comp comps[3];
    for (int k = 0; k < img->ncomp; k++)
        comp_init(&comps[k], buf + img->offset[k], img->pitch[k], W, H, img->h[k], img->v[k], img->hmax, img->vmax);
    convert(comps, img->ncomp, W, H, (Out){0, 1, 0, img->limited}, dst, stride);
}

void nc_jpeg_to_bgra(const nj_info *fi, uint8_t *const planes[4], const size_t pitch[4], int inverted, uint8_t *dst, size_t stride) {
    Comp comps[4];
    int nc = fi->ncomp;
    for (int k = 0; k < nc; k++) comp_init(&comps[k], planes[k], pitch[k], fi->width, fi->height, fi->h[k], fi->v[k], fi->hmax, fi->vmax);
    // (3: YCbCr unless Adobe's transform says none; 4: YCCK only when it says so)
    int ycc = nc == 3 ? fi->adobe != 0 : nc == 4 && fi->adobe == 2;
    convert(comps, nc, fi->width, fi->height, (Out){1, ycc, inverted, 0}, dst, stride);
}

void nc_psd_to_rgb(const NvImage *img, const uint8_t *buf, uint8_t *dst, size_t stride) {
    const int W = img->width, H = img->height;
    const size_t ps = (size_t)W * H;   // (ps_info.plane_size)
    const uint8_t *p0 = buf, *p1 = buf + img->offset[1], *p2 = buf + img->offset[2];
    const uint8_t *pa = img->hasa ? buf + ps * (size_t)img->ncomp : NULL;
    size_t bands = (size_t)nitro_ncpu() * 4;
    if (bands > (size_t)H) bands = (size_t)H;
    nitro_parallel(bands, ^(size_t b) {
        int y0 = (int)((size_t)H * b / bands), y1 = (int)((size_t)H * (b + 1) / bands);
        for (int y = y0; y < y1; y++) {
            size_t i = (size_t)y * W;
            uint8_t *o = dst + (size_t)y * stride;
            for (int x = 0; x < W; x++, i++) {
                int r = p0[i], g = p1[i], bl = p2[i];
                if (pa) {   // matted with white -> over black
                    int a = pa[i] - 255;
                    r = r + a < 0 ? 0 : r + a;
                    g = g + a < 0 ? 0 : g + a;
                    bl = bl + a < 0 ? 0 : bl + a;
                }
                o[3 * x] = (uint8_t)r;
                o[3 * x + 1] = (uint8_t)g;
                o[3 * x + 2] = (uint8_t)bl;
            }
        }
    });
}
