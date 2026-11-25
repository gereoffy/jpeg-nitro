// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_cmyk: see nitro_cmyk.h.
#include "nitro_cmyk.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "lcms2.h"
#include "nitro_cmyk_table.h"   // (ncm_cmyk9: PDFium's table, 9 steps an ink)

#define G 17   // grid points per component
enum { S0 = G * G * G, S1 = G * G, S2 = G, S3 = 1 };

enum { N = G * G * G * G };
typedef uint16_t Lut[N][3];                // B G R at each grid point, 8.8 fixed point
static Lut g_lut;                          // (the built-in profile's)
static uint32_t g_idx[4][256];             // a byte's grid cell, as an offset in each dimension
static uint16_t g_fr[256];                 // its place in the cell, 0..256
static pthread_once_t g_once = PTHREAD_ONCE_INIT, g_once_idx = PTHREAD_ONCE_INIT;

static void build_idx(void) {
    static const int st[4] = {S0, S1, S2, S3};
    for (int v = 0; v < 256; v++) {
        int x = v * (G - 1), i = x / 255, f = (x % 255 * 256 + 127) / 255;
        if (i == G - 1) i--, f = 256;
        for (int c = 0; c < 4; c++) g_idx[c][v] = (uint32_t)(i * st[c]);
        g_fr[v] = (uint16_t)f;
    }
}

// a CMYK profile's table (cmyk NULL, or no transform from it: the simple formula's colours at the grid points)
static void fill(cmsHPROFILE cmyk, Lut lut, int intent, int flags) {
    uint16_t (*in)[4] = malloc(sizeof(uint16_t[4]) * N), (*out)[4] = malloc(sizeof(uint16_t[4]) * N);
    if (!in || !out) {
        free(in), free(out);
        memset(lut, 0xff, sizeof(Lut));
        return;
    }
    for (int i = 0; i < N; i++) {
        int v = i;
        for (int c = 3; c >= 0; c--, v /= G) in[i][c] = (uint16_t)(v % G * 65535 / (G - 1));
    }
    cmsHPROFILE srgb = cmsCreate_sRGBProfile();
    cmsHTRANSFORM t = cmyk && srgb ? cmsCreateTransform(cmyk, TYPE_CMYK_16, srgb, TYPE_BGRA_16, intent, cmsFLAGS_NOCACHE | flags) : NULL;
    if (t) {
        cmsDoTransform(t, in, out, N);
        cmsDeleteTransform(t);
    } else
        for (int i = 0; i < N; i++) {
            unsigned k = 65535u - in[i][3];
            for (int c = 0; c < 3; c++) out[i][2 - c] = (uint16_t)((65535u - in[i][c]) * k / 65535u);
        }
    if (srgb) cmsCloseProfile(srgb);
    for (int i = 0; i < N; i++)
        for (int c = 0; c < 3; c++) lut[i][c] = (uint16_t)((out[i][c] * (255u * 256u) + 32767u) / 65535u);
    free(in), free(out);
}

// the built-in table: PDFium's 9 steps an ink spread to the 17 here (every second point one of its own,
// the ones between by the mean of their neighbours in each ink)
static void build(void) {
    pthread_once(&g_once_idx, build_idx);
    for (int i = 0; i < N; i++) {
        int at[4], half[4], v = i;
        for (int c = 3; c >= 0; c--, v /= G) at[c] = v % G / 2, half[c] = v % G & 1;
        unsigned sum[3] = {0, 0, 0}, n = 0;
        for (int corner = 0; corner < 16; corner++) {
            int idx = 0, skip = 0;
            for (int c = 0; c < 4; c++) {
                int up = corner >> c & 1;
                if (up && !half[c]) skip = 1;   // (on a point of its own in this ink: that one only)
                idx = idx * 9 + at[c] + up;
            }
            if (skip) continue;
            for (int c = 0; c < 3; c++) sum[c] += ncm_cmyk9[idx][c];
            n++;
        }
        for (int c = 0; c < 3; c++) g_lut[i][2 - c] = (uint16_t)((sum[c] * 256u + n / 2) / n);
    }
}

// the fractions packed with their dimension's stride, sorted descending: the simplex's path from the
// cell's corner, one dimension at a time
#define SORT2(a, b) { uint32_t hi_ = a > b ? a : b, lo_ = a > b ? b : a; a = hi_; b = lo_; }
#define SORT4(f0, f1, f2, f3) SORT2(f0, f1) SORT2(f2, f3) SORT2(f0, f2) SORT2(f1, f3) SORT2(f1, f2)

static void row_bgra(const uint16_t (*g_lut)[3], const uint8_t *cmyk, uint8_t *bgra, size_t n) {
    for (size_t i = 0; i < n; i++, cmyk += 4, bgra += 4) {
        uint8_t c = cmyk[0], m = cmyk[1], y = cmyk[2], k = cmyk[3];
        uint32_t base = g_idx[0][c] + g_idx[1][m] + g_idx[2][y] + g_idx[3][k];
        uint32_t f0 = (uint32_t)g_fr[c] << 16 | S0, f1 = (uint32_t)g_fr[m] << 16 | S1, f2 = (uint32_t)g_fr[y] << 16 | S2,
                 f3 = (uint32_t)g_fr[k] << 16 | S3;
        SORT4(f0, f1, f2, f3)
        int w0 = 256 - (int)(f0 >> 16), w1 = (int)(f0 >> 16) - (int)(f1 >> 16), w2 = (int)(f1 >> 16) - (int)(f2 >> 16),
            w3 = (int)(f2 >> 16) - (int)(f3 >> 16), w4 = (int)(f3 >> 16);
        const uint16_t *a0 = g_lut[base], *a1 = g_lut[base += f0 & 0xffff], *a2 = g_lut[base += f1 & 0xffff],
                       *a3 = g_lut[base += f2 & 0xffff], *a4 = g_lut[base + (f3 & 0xffff)];
        for (int j = 0; j < 3; j++)
            bgra[j] = (uint8_t)((a0[j] * w0 + a1[j] * w1 + a2[j] * w2 + a3[j] * w3 + a4[j] * w4 + (1 << 15)) >> 16);
        bgra[3] = 255;
    }
}

void ncm_row_bgra(const uint8_t *cmyk, uint8_t *bgra, size_t n) {
    pthread_once(&g_once, build);
    row_bgra(g_lut, cmyk, bgra, n);
}

static void color(const uint16_t (*g_lut)[3], const float cmyk[4], float rgb[3]) {
    static const int st[4] = {S0, S1, S2, S3};
    int base = 0, dim[4];
    float fr[4];
    for (int c = 0; c < 4; c++) {
        float v = cmyk[c] < 0 ? 0 : cmyk[c] > 1 ? 1 : cmyk[c];
        float x = v * (G - 1);
        int i = (int)x;
        if (i > G - 2) i = G - 2;
        base += i * st[c];
        fr[c] = x - i;
        dim[c] = c;
    }
    for (int a = 1; a < 4; a++)   // (descending fraction)
        for (int b = a; b > 0 && fr[dim[b]] > fr[dim[b - 1]]; b--) {
            int t = dim[b];
            dim[b] = dim[b - 1], dim[b - 1] = t;
        }
    float acc[3], w = 1 - fr[dim[0]];
    for (int j = 0; j < 3; j++) acc[j] = g_lut[base][j] * w;
    for (int s = 0; s < 4; s++) {
        base += st[dim[s]];
        w = fr[dim[s]] - (s < 3 ? fr[dim[s + 1]] : 0);
        for (int j = 0; j < 3; j++) acc[j] += g_lut[base][j] * w;
    }
    for (int j = 0; j < 3; j++) rgb[j] = acc[2 - j] / (255.f * 256.f);   // (the table is B G R)
}

void ncm_color(const float cmyk[4], float rgb[3]) {
    pthread_once(&g_once, build);
    color(g_lut, cmyk, rgb);
}

// --- a file's own profile ---------------------------------------------------------------

struct ncm_profile {
    int n;
    uint32_t gray[256];         // 1 component: each byte's B G R A
    cmsHTRANSFORM row, one;     // 3: R G B bytes -> B G R A bytes, and one colour of 16 bits a component
    uint16_t (*lut)[3];         // 4: the table as above
};

ncm_profile *ncm_profile_open(const uint8_t *icc, size_t len, int n) {
    if ((n != 1 && n != 3 && n != 4) || len < 128 || len > (64u << 20)) return NULL;
    cmsHPROFILE in = cmsOpenProfileFromMem(icc, (cmsUInt32Number)len);
    if (!in) return NULL;
    ncm_profile *p = NULL;
    cmsHPROFILE srgb = NULL;
    cmsColorSpaceSignature sig = cmsGetColorSpace(in);
    if (sig != (n == 1 ? cmsSigGrayData : n == 3 ? cmsSigRgbData : cmsSigCmykData)) goto done;
    if (!(p = calloc(1, sizeof *p))) goto done;
    p->n = n;
    if (n == 4) {
        pthread_once(&g_once_idx, build_idx);
        cmsHTRANSFORM probe = (srgb = cmsCreate_sRGBProfile()) ?
            cmsCreateTransform(in, TYPE_CMYK_16, srgb, TYPE_BGRA_16, INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE) : NULL;
        if (probe) cmsDeleteTransform(probe);
        if (!probe || !(p->lut = malloc(sizeof(Lut)))) goto fail;   // (not a profile to convert with: the built-in one)
        fill(in, p->lut, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION);
        goto done;
    }
    if (!(srgb = cmsCreate_sRGBProfile())) goto fail;
    if (n == 1) {
        uint8_t v[256];
        for (int i = 0; i < 256; i++) v[i] = (uint8_t)i, p->gray[i] = 0xff000000u;
        cmsHTRANSFORM t = cmsCreateTransform(in, TYPE_GRAY_8, srgb, TYPE_BGRA_8, INTENT_RELATIVE_COLORIMETRIC,
                                             cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION);
        if (!t) goto fail;
        cmsDoTransform(t, v, p->gray, 256);
        cmsDeleteTransform(t);
        int same = 1;
        for (int i = 0; i < 256; i++) {
            const uint8_t *o = (const uint8_t *)&p->gray[i];
            for (int c = 0; c < 3; c++) same &= abs(o[c] - i) <= 2;
            ((uint8_t *)&p->gray[i])[3] = 255;
        }
        if (same) goto fail;
        goto done;
    }
    p->row = cmsCreateTransform(in, TYPE_RGB_8, srgb, TYPE_BGRA_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION);
    p->one = cmsCreateTransform(in, TYPE_RGB_16, srgb, TYPE_RGB_16, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE | cmsFLAGS_BLACKPOINTCOMPENSATION);
    if (!p->row || !p->one) goto fail;
    {   // (sRGB itself, or as good as: nothing to convert)
        uint8_t v[6 * 6 * 6][3], o[6 * 6 * 6][4];
        for (int i = 0; i < 216; i++) v[i][0] = (uint8_t)(i / 36 * 51), v[i][1] = (uint8_t)(i / 6 % 6 * 51), v[i][2] = (uint8_t)(i % 6 * 51);
        cmsDoTransform(p->row, v, o, 216);
        int same = 1;
        for (int i = 0; i < 216; i++)
            for (int c = 0; c < 3; c++) same &= abs(o[i][2 - c] - v[i][c]) <= 2;
        if (same) goto fail;
    }
    goto done;
fail:
    ncm_profile_close(p);
    p = NULL;
done:
    if (srgb) cmsCloseProfile(srgb);
    cmsCloseProfile(in);
    return p;
}

void ncm_profile_close(ncm_profile *p) {
    if (!p) return;
    if (p->row) cmsDeleteTransform(p->row);
    if (p->one) cmsDeleteTransform(p->one);
    free(p->lut);
    free(p);
}

void ncm_profile_color(const ncm_profile *p, const float *c, float rgb[3]) {
    if (p->n == 4) {
        color((const uint16_t (*)[3])p->lut, c, rgb);
        return;
    }
    float v[3];
    for (int k = 0; k < p->n; k++) v[k] = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
    if (p->n == 1) {   // (between the two bytes' colours)
        float x = v[0] * 255;
        int i = (int)x;
        if (i > 254) i = 254;
        const uint8_t *a = (const uint8_t *)&p->gray[i], *b = (const uint8_t *)&p->gray[i + 1];
        for (int k = 0; k < 3; k++) rgb[k] = (a[2 - k] + (b[2 - k] - a[2 - k]) * (x - i)) / 255.f;
        return;
    }
    uint16_t in[3] = {(uint16_t)(v[0] * 65535 + .5f), (uint16_t)(v[1] * 65535 + .5f), (uint16_t)(v[2] * 65535 + .5f)}, out[3];
    cmsDoTransform(p->one, in, out, 1);
    for (int k = 0; k < 3; k++) rgb[k] = out[k] / 65535.f;
}

void ncm_profile_row_bgra(const ncm_profile *p, const uint8_t *src, uint8_t *bgra, size_t n) {
    if (p->n == 4) row_bgra((const uint16_t (*)[3])p->lut, src, bgra, n);
    else if (p->n == 1)
        for (size_t i = 0; i < n; i++) memcpy(bgra + 4 * i, &p->gray[src[i]], 4);
    else {
        cmsDoTransform(p->row, src, bgra, (cmsUInt32Number)n);
        for (size_t i = 0; i < n; i++) bgra[4 * i + 3] = 255;
    }
}
