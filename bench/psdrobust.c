// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Robustness test for nitropsd: damaged variants of the given PSDs (header,
// section lengths, RLE row table, compressed data, truncation), decoded single-
// and multi-threaded. Build with ASan + UBSan (make bench/psdrobust). Must not crash.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/nitropsd.h"

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static int cases, decoded;

static void run(const uint8_t *d, size_t n) {
    ps_info fi;
    cases++;
    if (ps_read_info(d, n, &fi) || !fi.supported) return;
    size_t sz = fi.plane_size * (fi.ncolor + fi.alpha);
    if (sz > ((size_t)1 << 31)) return;
    uint8_t *o = malloc(sz);
    int a = ps_decode(d, n, &fi, o, 1, NULL), b = ps_decode(d, n, &fi, o, 0, NULL);
    decoded += !a || !b;
    free(o);
}

int main(int argc, char **argv) {
    for (int f = 1; f < argc; f++) {
        FILE *fp = fopen(argv[f], "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); size_t n = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint8_t *o = malloc(n), *d = malloc(n + 64);
        fread(o, 1, n, fp); fclose(fp);
        run(o, n);
        ps_info fi; ps_read_info(o, n, &fi);
        size_t ds = fi.data_off;
        for (int k = 0; k < 40; k++) { size_t m = (size_t)((double)n * k / 40); memcpy(d, o, m); run(d, m); }
        size_t fixed[] = {4, 12, 26, 30, ds - 2, ds, ds + 1, ds + 7, n - 1};
        for (size_t k = 0; k < sizeof fixed / sizeof *fixed; k++) if (fixed[k] <= n) { memcpy(d, o, fixed[k]); run(d, fixed[k]); }
        for (int k = 0; k < 60; k++) {   // header and section lengths
            memcpy(d, o, n);
            size_t p = rnd() % (ds + 8 < n ? ds + 8 : n);
            d[p] = (uint8_t)rnd();
            run(d, n);
        }
        for (int k = 0; k < 60; k++) {   // RLE row table / compressed data
            memcpy(d, o, n);
            int cnt = 1 + (int)(rnd() % 50);
            for (int i = 0; i < cnt; i++) d[ds + rnd() % (n - ds)] = (uint8_t)rnd();
            run(d, n);
        }
        for (int k = 0; k < 10; k++) {   // row table entries set to huge / zero
            memcpy(d, o, n);
            size_t p = ds + 2 * (rnd() % 64);
            if (p + 2 <= n) { d[p] = k & 1 ? 0xFF : 0; d[p + 1] = k & 1 ? 0xFF : 0; }
            run(d, n);
        }
        free(o); free(d);
    }
    printf("%d cases (%d decoded). No crash.\n", cases, decoded);
    return 0;
}
