// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// JPEG 2000 throughput, input read once; allocation/decoding timed, checksum outside timing.
//   make bench/jpxbench
//   bench/jpxbench [-r repetitions] [-1] images.jp2 images.j2k ...
#include "../src/nitrojpx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec * 1e-6;
}
static int order(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
int main(int argc, char **argv) {
    int reps = 5, files = 0;
    unsigned flags = NJPX_RAW_COLORS;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-1")) { flags |= NJPX_SINGLE_THREAD; continue; }
        if (!strcmp(argv[a], "-r") && a + 1 < argc) {
            reps = atoi(argv[++a]);
            if (reps < 1 || reps > 10000) return 2;
            continue;
        }
        FILE *f = fopen(argv[a], "rb");
        if (!f) { perror(argv[a]); return 2; }
        if (fseek(f, 0, SEEK_END)) return 2;
        long size = ftell(f); rewind(f);
        if (size <= 0) return 2;
        uint8_t *data = malloc((size_t)size);
        double *times = malloc((size_t)reps * sizeof(double));
        if (!data || !times || fread(data, 1, (size_t)size, f) != (size_t)size) return 2;
        fclose(f);
        int width = 0, height = 0, nc = 0;
        uint64_t hash = 0;
        for (int r = 0; r < reps; r++) {
            njpx_image im;
            double start = now();
            int rc = njpx_decode(data, (size_t)size, flags, &im);
            times[r] = now() - start;
            if (rc) { fprintf(stderr, "decode failed: %s\n", argv[a]); free(times); free(data); return 1; }
            width = im.width; height = im.height; nc = im.ncomp;
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < (size_t)width * height * nc; i++) h = (h ^ im.samples[i]) * 1099511628211ull;
            njpx_free(&im);
            if (r && h != hash) { fprintf(stderr, "unstable pixels: %s\n", argv[a]); return 1; }
            hash = h;
        }
        qsort(times, (size_t)reps, sizeof(double), order);
        double median = (times[(reps - 1) / 2] + times[reps / 2]) * 0.5;
        printf("%s: %dx%d, %d components, median %.2f ms, min %.2f ms, hash %016llx%s\n", argv[a],
               width, height, nc, median, times[0], (unsigned long long)hash, flags & NJPX_SINGLE_THREAD ? " (serial)" : "");
        free(times); free(data); files++;
    }
    if (!files) { fprintf(stderr, "usage: jpxbench [-r repetitions] [-1] files.jp2 files.j2k ...\n"); return 2; }
    return 0;
}
