// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrojxl test decoder: a JPEG XL file to a 16-bit RGBA PAM (premultiplied alpha), with the time.
// Usage: jxldec [-1] in.jxl [out.pam]
#include "../src/nitrojxl.h"
#include "../src/nitro_os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    unsigned flags = 0;
    int a = 1;
    if (a < argc && !strcmp(argv[a], "-1")) { flags |= NJXL_SINGLE_THREAD; a++; }
    if (a >= argc) { fprintf(stderr, "usage: jxldec [-1] in.jxl [out.pam]\n"); return 2; }
    FILE *f = fopen(argv[a], "rb");
    if (!f) { perror(argv[a]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n + 64);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read error\n"); return 1; }
    fclose(f);
    njxl_info info;
    int rc = njxl_info_read(d, (size_t)n, &info);
    if (rc) { printf("%s: header error %d\n", argv[a], rc); return 1; }
    size_t pitch = (size_t)info.width * 8;
    uint16_t *px = malloc(pitch * (size_t)info.height);
    if (!px) { printf("no memory\n"); return 1; }
    double t0 = nitro_now_ms();
    rc = njxl_decode(d, (size_t)n, flags, NJXL_RGBA16, px, pitch, &info);
    double t1 = nitro_now_ms();
    printf("%s: %dx%d bits %d%s%s%s orient %d icc %zu: rc %d, %.1f ms\n", argv[a], info.width, info.height, info.bits,
           info.is_float ? " float" : "", info.xyb ? " xyb" : "", info.alpha ? " alpha" : "", info.orientation, info.icc_len, rc, t1 - t0);
    if (!rc && a + 1 < argc) {
        FILE *o = fopen(argv[a + 1], "wb");
        if (!o) { perror(argv[a + 1]); return 1; }
        fprintf(o, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 65535\nTUPLTYPE RGB_ALPHA\nENDHDR\n", info.width, info.height);
        size_t cnt = (size_t)info.width * info.height * 4;
        uint8_t *be = malloc(cnt * 2);
        for (size_t i = 0; i < cnt; i++) { be[2 * i] = (uint8_t)(px[i] >> 8); be[2 * i + 1] = (uint8_t)px[i]; }
        fwrite(be, 2, cnt, o);
        fclose(o);
        free(be);
    }
    njxl_info_free(&info);
    free(px); free(d);
    return rc ? 1 : 0;
}
