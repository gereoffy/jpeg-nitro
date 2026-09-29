// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// The GNOME viewer's JPEG -> RGB conversion (src/gnome/nitro-convert.c) must give exactly
// libjpeg-turbo's RGB output: nitrojpeg planes + nc_jpeg_to_rgb vs tj3Decompress8(TJPF_RGB).
//   rgbverify files.jpg...
#include "../src/gnome/nitro-convert.h"
#include "../src/nitro_os.h"
#include <turbojpeg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    int bad = 0, n = 0, nseen = 0;
    double tconv = 0, mp = 0;
    char seen[32][24];
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *d = malloc((size_t)len + 64);
        if (fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); continue; }
        fclose(f);
        memset(d + len, 0, 64);
        nj_info fi;
        if (nj_read_info(d, (size_t)len, &fi) || !fi.supported) { free(d); continue; }
        size_t pitch[3], off[3], tot = 0;
        for (int c = 0; c < fi.ncomp; c++) { pitch[c] = (size_t)fi.plane_w[c]; off[c] = tot; tot += pitch[c] * fi.plane_h[c]; }
        unsigned char *pl = malloc(tot), *planes[3] = {NULL, NULL, NULL};
        for (int c = 0; c < fi.ncomp; c++) planes[c] = pl + off[c];
        if (nj_decode_planes(d, (size_t)len, &fi, planes, pitch, 0, NULL)) { free(pl); free(d); continue; }
        size_t sz = (size_t)fi.width * fi.height * 3;
        unsigned char *A = malloc(sz), *B = malloc(sz);
        double t0 = nitro_now_ms();
        nc_jpeg_to_rgb(&fi, planes, pitch, A, (size_t)fi.width * 3);
        tconv += nitro_now_ms() - t0;
        mp += fi.width * (double)fi.height / 1e6;
        tjhandle h = tj3Init(TJINIT_DECOMPRESS);
        tj3Decompress8(h, d, (size_t)len, B, fi.width * 3, TJPF_RGB);
        tj3Destroy(h);
        size_t diff = 0;
        int maxd = 0;
        for (size_t k = 0; k < sz; k++)
            if (A[k] != B[k]) { diff++; int e = abs(A[k] - B[k]); if (e > maxd) maxd = e; }
        char key[24];
        snprintf(key, sizeof key, "%s %dx%d", fi.ncomp == 1 ? "gray" : "YCbCr",
                 fi.ncomp > 1 ? fi.hmax / fi.h[1] : 1, fi.ncomp > 1 ? fi.vmax / fi.v[1] : 1);
        int k = 0;
        while (k < nseen && strcmp(seen[k], key)) k++;
        if (k == nseen && nseen < 32) strcpy(seen[nseen++], key);
        if (diff) { bad++; printf("DIFF %s  %dx%d %s: %zu values differ, max %d\n", argv[a], fi.width, fi.height, key, diff, maxd); }
        n++;
        free(A); free(B); free(pl); free(d);
    }
    printf("chroma upsampling seen:");
    for (int k = 0; k < nseen; k++) printf(" [%s]", seen[k]);
    printf("\n%d files: %s; conversion %.2f ms per megapixel\n", n, bad ? "DIFFERENT" : "all identical to libjpeg-turbo",
           mp > 0 ? tconv / mp : 0);
    return bad != 0;
}
