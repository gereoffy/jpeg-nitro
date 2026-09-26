// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Sustained-load test: decodes the given files over and over for N seconds
// and prints, every 0.5 s, the decode speed and the all-core clock measured
// right after (a ~1 ms dependent-add probe on every thread). Shows whether
// thermal throttling / clock changes affect the results.
//
//   sustain SECONDS WORKERS files...
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/nitrojpeg.h"

static double now_s(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static double probe_ghz(long iters) {
    double t0 = now_s();
    long x = 0;
    for (long i = 0; i < iters; i++)
#if defined(__x86_64__)
        __asm__ volatile(".rept 100\n\taddq %1, %0\n\t.endr" : "+r"(x) : "r"(1L));
#elif defined(__aarch64__)   // untested
        __asm__ volatile(".rept 100\n\tadd %0, %0, %1\n\t.endr" : "+r"(x) : "r"(1L));
#else
#error "clock probe: unsupported CPU"
#endif
    return (double)iters * 100 / (now_s() - t0) / 1e9;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: sustain SECONDS WORKERS files...\n"); return 1; }
    double secs = atof(argv[1]);
    int workers = atoi(argv[2]);
    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    nj_set_max_workers(workers);
    int n = argc - 3;
    uint8_t **data = calloc(n, sizeof *data);
    size_t *len = calloc(n, sizeof *len);
    nj_info *fi = calloc(n, sizeof *fi);
    size_t maxtot = 0;
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(argv[i + 3], "rb");
        fseek(f, 0, SEEK_END); len[i] = ftell(f); fseek(f, 0, SEEK_SET);
        data[i] = malloc(len[i]); fread(data[i], 1, len[i], f); fclose(f);
        nj_read_info(data[i], len[i], &fi[i]);
        size_t tot = 0;
        for (int c = 0; c < fi[i].ncomp; c++) tot += ((fi[i].plane_w[c] + 255) & ~255) * (size_t)fi[i].plane_h[c];
        if (tot > maxtot) maxtot = tot;
    }
    uint8_t *out = malloc(maxtot);
    memset(out, 0, maxtot);
    double *gs = calloc(ncpu, sizeof *gs);
    printf("%d workers, %d files, %.0f s\n", workers ? workers : ncpu, n, secs);
    double start = now_s(), wstart = start;
    int wimgs = 0, k = 0;
    double wdec = 0;
    while (now_s() - start < secs) {
        int i = k++ % n;
        size_t pitch[3]; uint8_t *pl[3]; uint8_t *p = out;
        for (int c = 0; c < fi[i].ncomp; c++) { pitch[c] = (fi[i].plane_w[c] + 255) & ~255; pl[c] = p; p += pitch[c] * fi[i].plane_h[c]; }
        double t0 = now_s();
        nj_decode_planes(data[i], len[i], &fi[i], pl, pitch, 0, NULL);
        wdec += now_s() - t0;
        wimgs++;
        if (now_s() - wstart >= 0.5) {
            dispatch_apply((size_t)ncpu, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t t) {
                gs[t] = probe_ghz(40000);
            });
            double g = 0, mn = 1e9;
            for (int t = 0; t < ncpu; t++) { g += gs[t]; if (gs[t] < mn) mn = gs[t]; }
            printf("  t=%5.1fs  %6.2f ms/img  (%3d imgs)   clock after: avg %.2f GHz, min %.2f\n",
                   now_s() - start, wdec / wimgs * 1e3, wimgs, g / ncpu, mn);
            fflush(stdout);
            wstart = now_s(); wimgs = 0; wdec = 0;
        }
    }
    return 0;
}
