// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Damaged JPEG XL files under ASan + UBSan: truncations and byte / bit mutations of the
// codestream (headers, TOC, entropy-coded sections). Use small seeds:
//   make bench/jxlrobust && bench/jxlrobust files.jxl ...
#include "../src/nitrojxl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static size_t cases, decoded;
static void run(const uint8_t *d, size_t n) {
    njxl_info info;
    int w, h, o;
    njxl_size(d, n, &w, &h, &o);
    if (njxl_info_read(d, n, &info)) return;
    size_t px = (size_t)info.width * info.height, pitch = (size_t)info.width * 4;
    njxl_info_free(&info);
    if (px > ((size_t)1 << 22)) return;   // (keeps the run short)
    uint8_t *out = malloc(px * 4 + 1);
    if (!out) abort();
    int rc = njxl_decode(d, n, (rnd() & 1) ? NJXL_SINGLE_THREAD : 0, NJXL_BGRA8, out, pitch, &info);
    cases++;
    if (!rc) decoded++;
    else if (info.icc || info.width) abort();   // (a failure leaves info empty)
    njxl_info_free(&info);
    free(out);
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: jxlrobust small.jxl ...\n"); return 2; }
    run(NULL, 0);
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { perror(argv[a]); return 2; }
        if (fseek(f, 0, SEEK_END)) return 2;
        long size = ftell(f); rewind(f);
        if (size <= 0 || size > (1 << 20)) { fclose(f); continue; }
        size_t n = (size_t)size;
        uint8_t *original = malloc(n), *d = malloc(n);
        if (!original || !d || fread(original, 1, n, f) != n) return 2;
        fclose(f);
        run(original, n);
        for (int t = 0; t < 40; t++) {   // truncations
            size_t m = (size_t)(rnd() % n);
            memcpy(d, original, m);
            run(d, m);
        }
        for (int t = 0; t < 400; t++) {   // mutations: mostly in the first KB (headers, TOC, histograms)
            memcpy(d, original, n);
            int k = 1 + (int)(rnd() % 4);
            for (int i = 0; i < k; i++) {
                size_t p = (rnd() & 3) ? rnd() % (n < 1024 ? n : 1024) : rnd() % n;
                if (rnd() & 1) d[p] ^= (uint8_t)(1u << (rnd() % 8)); else d[p] = (uint8_t)rnd();
            }
            run(d, n);
        }
        free(original); free(d);
        fprintf(stderr, "%s: %zu cases, %zu decoded\n", argv[a], cases, decoded);
    }
    printf("%zu cases, %zu decoded\n", cases, decoded);
    return 0;
}
