// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Measures the real core clock without root: a chain of dependent integer
// adds executes at exactly one add per cycle, so adds / time = frequency.
// (The TSC runs at a fixed rate, so it can't be used for this.)
//
//   freqprobe            single-core clock, then all-core clock under load
//                        sampled every 100 ms for a few seconds (throttling curve)
//   freqprobe N SECONDS  N threads, sampling for SECONDS
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static double now_s(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

// 100 dependent adds per iteration; the loop counter runs in parallel.
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
    double t = now_s() - t0;
    return (double)iters * 100 / t / 1e9;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    double secs = argc > 2 ? atof(argv[2]) : 4.0;
    probe_ghz(200000);
    printf("single core: %.2f GHz\n", probe_ghz(400000));
    printf("%d threads, all-core load, per-thread clock every 100 ms:\n", n);
    int samples = (int)(secs * 10);
    double *ghz = calloc((size_t)samples * n, sizeof *ghz);
    dispatch_apply((size_t)n, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^(size_t t) {
        double start = now_s();
        for (int s = 0; s < samples; s++) {
            double sum = 0; int k = 0;
            while (now_s() - start < (s + 1) * 0.1) { sum += probe_ghz(20000); k++; }   // ~0.5 ms each
            ghz[s * n + t] = k ? sum / k : 0;
        }
    });
    for (int s = 0; s < samples; s++) {
        double sum = 0, mn = 1e9, mx = 0;
        for (int t = 0; t < n; t++) {
            double g = ghz[s * n + t];
            sum += g; if (g < mn) mn = g; if (g > mx) mx = g;
        }
        printf("  t=%4.1fs  avg %.2f GHz  (min %.2f  max %.2f)\n", (s + 1) * 0.1, sum / n, mn, mx);
    }
    return 0;
}
