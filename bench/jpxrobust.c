// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Damaged JP2 / raw codestreams under ASan + UBSan: truncations, header/packet/entropy
// mutations, and both JP2 colour mappings and PDF's raw component path. Use small seeds:
//   make bench/jpxrobust && bench/jpxrobust files.jp2 files.j2k ...
#include "../src/nitrojpx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static size_t cases, decoded;
static void run(const uint8_t *d, size_t n) {
    for (unsigned flags = 0; flags <= NJPX_RAW_COLORS; flags++) {
        njpx_image im;
        int rc = njpx_decode(d, n, flags, &im);
        cases++;
        if (!rc) {
            if (!im.samples || im.width <= 0 || im.height <= 0 || im.ncomp < 1 || im.ncomp > NJPX_MAX_COMPS || im.alpha >= im.ncomp) abort();
            size_t count = (size_t)im.width * im.height * im.ncomp;
            for (size_t i = 0; i < count; i++) if (im.samples[i] >= (1u << im.precision[i % im.ncomp])) abort();
            decoded++;
        } else if (im.samples || im.width || im.height || im.ncomp) abort();
        njpx_free(&im);
        njpx_free(&im); // releasing an empty result is safe
    }
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: jpxrobust small.jp2 small.j2k ...\n"); return 2; }
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
        // Every short header length, then representative whole-file truncations.
        for (size_t i = 0; i < n && i < 256; i++) run(original, i);
        for (size_t i = 1; i < 100; i++) run(original, n * i / 100);
        for (int k = 0; k < 600; k++) {
            memcpy(d, original, n);
            int flips = 1 + (int)(rnd() % 8);
            for (int i = 0; i < flips; i++) d[rnd() % n] ^= (uint8_t)(1u << (rnd() & 7));
            run(d, n);
        }
        for (int k = 0; k < 80; k++) {
            memcpy(d, original, n);
            size_t at = rnd() % n, len = rnd() % (n - at + 1);
            memset(d + at, k & 1 ? 255 : 0, len); run(d, n);
        }
        free(d); free(original);
        printf("%s: checked\n", argv[a]); fflush(stdout);
    }
    printf("%zu cases (%zu decoded), no sanitizer errors.\n", cases, decoded);
    return 0;
}
