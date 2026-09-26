// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Where does PNG decoding time go? Times zlib inflate of the IDAT stream and
// the (sequential) filter reversal separately, and prints filter statistics.
// 8-bit RGB / RGBA / gray, non-interlaced only.   pngsplit files.png...
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static inline uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

int main(int argc, char **argv) {
    double ti = 0, tf = 0;
    long rows = 0, ft[5] = {0};
    int files = 0;
    for (int f = 1; f < argc; f++) {
        FILE *fp = fopen(argv[f], "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); size_t n = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint8_t *d = malloc(n); fread(d, 1, n, fp); fclose(fp);
        uint32_t w = 0, h = 0; int ct = -1, bd = 0, il = 0;
        uint8_t *z = malloc(n); size_t zl = 0;
        for (size_t i = 8; i + 12 <= n;) {
            uint32_t len = be32(d + i);
            if (!memcmp(d + i + 4, "IHDR", 4)) { w = be32(d + i + 8); h = be32(d + i + 12); bd = d[i + 16]; ct = d[i + 17]; il = d[i + 20]; }
            if (!memcmp(d + i + 4, "IDAT", 4)) { memcpy(z + zl, d + i + 8, len); zl += len; }
            i += 12 + len;
        }
        int bpp = ct == 2 ? 3 : ct == 6 ? 4 : ct == 0 ? 1 : ct == 4 ? 2 : 0;
        if (bd != 8 || !bpp || il) { printf("%s: unsupported (bit depth %d, color type %d, interlace %d)\n", argv[f], bd, ct, il); continue; }
        size_t stride = (size_t)w * bpp, raw = (stride + 1) * h;
        uint8_t *u = malloc(raw);
        double t0 = now_ms();
        uLongf ul = raw;
        if (uncompress(u, &ul, z, zl) != Z_OK || ul != raw) { printf("%s: inflate failed\n", argv[f]); continue; }
        double t1 = now_ms();
        uint8_t *prev = calloc(stride, 1);
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *r = u + y * (stride + 1), t = r[0], *p = r + 1;
            ft[t < 5 ? t : 0]++;
            switch (t) {
            case 1: for (size_t x = bpp; x < stride; x++) p[x] += p[x - bpp]; break;
            case 2: for (size_t x = 0; x < stride; x++) p[x] += prev[x]; break;
            case 3:
                for (size_t x = 0; x < stride; x++) p[x] += ((x >= (size_t)bpp ? p[x - bpp] : 0) + prev[x]) >> 1;
                break;
            case 4:
                for (size_t x = 0; x < stride; x++)
                    p[x] += paeth(x >= (size_t)bpp ? p[x - bpp] : 0, prev[x], x >= (size_t)bpp ? prev[x - bpp] : 0);
                break;
            }
            prev = p;
        }
        double t2 = now_ms();
        ti += t1 - t0; tf += t2 - t1; rows += h; files++;
        free(u); free(z); free(d);
    }
    if (!files) return 1;
    printf("%d files: zlib inflate %.1f ms/img, filter reversal (plain C) %.1f ms/img\n", files, ti / files, tf / files);
    printf("row filters: none %.1f%%  sub %.1f%%  up %.1f%%  average %.1f%%  paeth %.1f%%\n", 100.0 * ft[0] / rows,
           100.0 * ft[1] / rows, 100.0 * ft[2] / rows, 100.0 * ft[3] / rows, 100.0 * ft[4] / rows);
    return 0;
}
