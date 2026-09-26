// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Checks nitropng against zlib + a straightforward filter reversal (byte-exact),
// and times both.   pngverify files.png...   (NP_THREADS=n to set threads)
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "../src/nitropng.h"

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int ref_decode(const uint8_t *d, size_t n, const np_info *fi, uint8_t *u) {
    uint8_t *z = malloc(n); size_t zl = 0;
    for (size_t i = 8; i + 12 <= n;) {
        uint32_t len = be32(d + i);
        if (!memcmp(d + i + 4, "IDAT", 4)) { memcpy(z + zl, d + i + 8, len); zl += len; }
        i += 12 + len;
    }
    uLongf ul = fi->raw_size;
    int ok = uncompress(u, &ul, z, zl) == Z_OK && ul == fi->raw_size;
    free(z);
    if (!ok) return -1;
    size_t rs = fi->stride + 1; int bpp = fi->channels;
    for (int y = 0; y < fi->height; y++) {
        uint8_t *p = u + (size_t)y * rs + 1, *q = y ? p - rs : NULL;
        for (size_t x = 0; x < fi->stride; x++) {
            int a = x >= (size_t)bpp ? p[x - bpp] : 0, b = q ? q[x] : 0, c = q && x >= (size_t)bpp ? q[x - bpp] : 0;
            switch (p[-1]) {
            case 1: p[x] += a; break;
            case 2: p[x] += b; break;
            case 3: p[x] += (a + b) >> 1; break;
            case 4: { int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c);
                      p[x] += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; } break;
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    int nt = getenv("NP_THREADS") ? atoi(getenv("NP_THREADS")) : 0, bad = 0, n = 0;
    if (getenv("NP_CHUNKS")) np_set_chunks(atoi(getenv("NP_CHUNKS")));
    double tz = 0, tn = 0, ti = 0, tu = 0;
    for (int f = 1; f < argc; f++) {
        FILE *fp = fopen(argv[f], "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); size_t len = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint8_t *d = malloc(len); fread(d, 1, len, fp); fclose(fp);
        const char *nm = strrchr(argv[f], '/') ? strrchr(argv[f], '/') + 1 : argv[f];
        np_info fi;
        if (np_read_info(d, len, &fi) || !fi.supported) {
            printf("%-34.34s unsupported (bit depth %d, color type %d, interlace %d)\n", nm, fi.bit_depth, fi.color_type, fi.interlace);
            free(d); continue;
        }
        uint8_t *a = malloc(fi.raw_size), *b = malloc(fi.raw_size);
        double t0 = now_ms();
        int r1 = ref_decode(d, len, &fi, a);
        double t1 = now_ms();
        np_stats st;
        int r2 = np_decode(d, len, &fi, b, nt, &st);
        double t2 = now_ms();
        // compare pixels only (filter bytes are kept as in the file by both)
        int same = !r1 && !r2 && !memcmp(a, b, fi.raw_size);
        printf("%-34.34s %5dx%-5d ch %d  zlib+ref %7.1f ms  nitropng %7.1f ms (inflate %6.1f, unfilter %5.1f, %3d chunks)  %s\n",
               nm, fi.width, fi.height, fi.channels, t1 - t0, t2 - t1, st.inflate_ms, st.unfilter_ms, st.chunks,
               same ? "IDENTICAL" : r2 ? "NITROPNG FAILED" : r1 ? "REF FAILED" : "MISMATCH");
        if (!same) bad++;
        else { tz += t1 - t0; tn += t2 - t1; ti += st.inflate_ms; tu += st.unfilter_ms; n++; }
        free(a); free(b); free(d);
    }
    if (n) printf("\n%d files: zlib+ref %.1f ms/img, nitropng %.1f ms/img (inflate %.1f, unfilter %.1f)\n", n, tz / n, tn / n, ti / n, tu / n);
    printf(bad ? "FAILURES: %d\n" : "all IDENTICAL\n", bad);
    return bad != 0;
}
