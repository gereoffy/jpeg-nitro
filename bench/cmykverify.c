// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// CMYK / YCCK JPEGs (4 components, as PDFs hold them): nitrojpeg's planes + nc_jpeg_to_bgra must give
// what libjpeg-turbo's CMYK output gives through the same simple formula, pixel for pixel.
//   cmykverify files.jpg...
#include "nitro_convert.h"
#include "nitrojpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

static uint8_t *load(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = len > 0 ? malloc((size_t)len) : NULL;
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) { free(d); d = NULL; }
    fclose(f);
    *n = (size_t)len;
    return d;
}

static inline int mul255(int a, int b) { return (a * b + 127) / 255; }

int main(int argc, char **argv) {
    int bad = 0, done = 0;
    for (int i = 1; i < argc; i++) {
        size_t n;
        uint8_t *d = load(argv[i], &n);
        nj_info fi;
        if (!d || nj_read_info(d, n, &fi) || fi.ncomp != 4 || !fi.supported) {
            printf("%s: not a 4-component JPEG nitrojpeg decodes\n", argv[i]);
            free(d);
            continue;
        }
        size_t pitch[4], at[4], tot = 0;
        for (int c = 0; c < 4; c++) pitch[c] = (size_t)fi.plane_w[c], at[c] = tot, tot += pitch[c] * fi.plane_h[c];
        uint8_t *buf = malloc(tot), *ours = malloc((size_t)fi.width * fi.height * 4), *ref = malloc((size_t)fi.width * fi.height * 4);
        uint8_t *planes[4] = {buf + at[0], buf + at[1], buf + at[2], buf + at[3]};
        int r1 = nj_decode_planes(d, n, &fi, planes, pitch, 0, NULL);
        if (!r1) nc_jpeg_to_bgra(&fi, planes, pitch, 0, NULL, ours, (size_t)fi.width * 4);
        tjhandle h = tj3Init(TJINIT_DECOMPRESS);
        int r2 = tj3DecompressHeader(h, d, n) || tj3Decompress8(h, d, n, ref, fi.width * 4, TJPF_CMYK);
        tj3Destroy(h);
        size_t diff = 0;
        int maxd = 0;
        for (size_t p = 0; !r1 && !r2 && p < (size_t)fi.width * fi.height; p++) {
            const uint8_t *q = ref + 4 * p, *o = ours + 4 * p;
            int k = q[3], want[3] = {mul255(255 - q[0], 255 - k), mul255(255 - q[1], 255 - k), mul255(255 - q[2], 255 - k)};
            int got[3] = {o[2], o[1], o[0]};   // (B G R A)
            for (int c = 0; c < 3; c++)
                if (got[c] != want[c]) {
                    diff++;
                    if (abs(got[c] - want[c]) > maxd) maxd = abs(got[c] - want[c]);
                }
        }
        const char *nm = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        printf("%-24s %5dx%-5d %s %dx%d,%dx%d,%dx%d,%dx%d  %s\n", nm, fi.width, fi.height, fi.adobe == 2 ? "YCCK" : "CMYK", fi.h[0], fi.v[0], fi.h[1],
               fi.v[1], fi.h[2], fi.v[2], fi.h[3], fi.v[3], r1 || r2 ? "DECODE FAILED" : diff ? "DIFF" : "OK");
        if (diff) printf("    %zu values differ, max %d\n", diff, maxd);
        bad += r1 || r2 || diff;
        done++;
        free(buf), free(ours), free(ref), free(d);
    }
    printf("%d files: %s\n", done, bad ? "DIFFERENT" : "all identical to libjpeg-turbo");
    return bad != 0;
}
