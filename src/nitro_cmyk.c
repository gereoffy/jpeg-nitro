// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_cmyk: see nitro_cmyk.h.
#include "nitro_cmyk.h"
#include <pthread.h>
#include "lcms2.h"

// libcdr's CMYK profile (src/lib/CDRColorProfiles.h of our fork: one copy, linked from there)
extern const unsigned char CMYK_icc[];
extern const unsigned CMYK_icc_size;

#define G 17   // grid points per component
enum { S0 = G * G * G, S1 = G * G, S2 = G, S3 = 1 };

static uint16_t g_lut[G * G * G * G][3];   // B G R at each grid point, 8.8 fixed point
static uint32_t g_idx[4][256];             // a byte's grid cell, as an offset in each dimension
static uint16_t g_fr[256];                 // its place in the cell, 0..256
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void build(void) {
    static const int st[4] = {S0, S1, S2, S3};
    for (int v = 0; v < 256; v++) {
        int x = v * (G - 1), i = x / 255, f = (x % 255 * 256 + 127) / 255;
        if (i == G - 1) i--, f = 256;
        for (int c = 0; c < 4; c++) g_idx[c][v] = (uint32_t)(i * st[c]);
        g_fr[v] = (uint16_t)f;
    }
    enum { N = G * G * G * G };
    static uint16_t in[N][4], out[N][4];
    for (int i = 0; i < N; i++) {
        int v = i;
        for (int c = 3; c >= 0; c--, v /= G) in[i][c] = (uint16_t)(v % G * 65535 / (G - 1));
    }
    cmsHPROFILE cmyk = cmsOpenProfileFromMem(CMYK_icc, CMYK_icc_size), srgb = cmsCreate_sRGBProfile();
    cmsHTRANSFORM t = cmyk && srgb ? cmsCreateTransform(cmyk, TYPE_CMYK_16, srgb, TYPE_BGRA_16, INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE) : NULL;
    if (t) {
        cmsDoTransform(t, in, out, N);
        cmsDeleteTransform(t);
    } else   // (no profile: the simple formula's colours at the grid points)
        for (int i = 0; i < N; i++) {
            unsigned k = 65535u - in[i][3];
            for (int c = 0; c < 3; c++) out[i][2 - c] = (uint16_t)((65535u - in[i][c]) * k / 65535u);
        }
    if (cmyk) cmsCloseProfile(cmyk);
    if (srgb) cmsCloseProfile(srgb);
    for (int i = 0; i < N; i++)
        for (int c = 0; c < 3; c++) g_lut[i][c] = (uint16_t)((out[i][c] * (255u * 256u) + 32767u) / 65535u);
}

// the fractions packed with their dimension's stride, sorted descending: the simplex's path from the
// cell's corner, one dimension at a time
#define SORT2(a, b) { uint32_t hi_ = a > b ? a : b, lo_ = a > b ? b : a; a = hi_; b = lo_; }
#define SORT4(f0, f1, f2, f3) SORT2(f0, f1) SORT2(f2, f3) SORT2(f0, f2) SORT2(f1, f3) SORT2(f1, f2)

void ncm_row_bgra(const uint8_t *cmyk, uint8_t *bgra, size_t n) {
    pthread_once(&g_once, build);
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

void ncm_color(const float cmyk[4], float rgb[3]) {
    pthread_once(&g_once, build);
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
