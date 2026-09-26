// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Checks that nitrojpeg's parallel output is bit-identical to a
// single-threaded libjpeg-turbo decode, and prints the split statistics.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>
#include "../src/nitrojpeg.h"

static uint8_t *load(const char *fn, size_t *n) {
    FILE *f = fopen(fn, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n); fread(b, 1, *n, f); fclose(f); return b;
}

static long hist[8];          // sync distance histogram in MCUs
static const char *hname[8] = {"0", "1", "2", "3-4", "5-8", "9-16", "17+", "never"};
static long nthr, tot_mcus, max_mcus, tot_bits, max_bits;

static void sync_summary(const nj_stats *st, char *out, size_t n) {
    if (st->mode != 2 || !st->nsync) { snprintf(out, n, "-"); return; }
    int mx = 0, never = 0; long sum = 0; uint32_t mb = 0; long sb = 0;
    for (int i = 0; i < st->nsync; i++) {
        int m = st->sync_mcus[i];
        int b = m < 0 ? 7 : m == 0 ? 0 : m == 1 ? 1 : m == 2 ? 2 : m <= 4 ? 3 : m <= 8 ? 4 : m <= 16 ? 5 : 6;
        hist[b]++;
        if (m < 0) { never++; continue; }
        nthr++; sum += m; tot_mcus += m; sb += st->sync_bits[i]; tot_bits += st->sync_bits[i];
        if (m > mx) mx = m;
        if (m > max_mcus) max_mcus = m;
        if (st->sync_bits[i] > mb) mb = st->sync_bits[i];
        if (st->sync_bits[i] > max_bits) max_bits = st->sync_bits[i];
    }
    int ok = st->nsync - never;
    snprintf(out, n, "sync %2d thr: avg %4.1f max %3d MCU, avg %5.0f max %5u bit%s", st->nsync,
             ok ? (double)sum / ok : 0, mx, ok ? (double)sb / ok : 0, mb, never ? " NEVER!" : "");
}

int main(int argc, char **argv) {
    int bad = 0;
    if (getenv("NJ_SEQ")) nj_set_sequential_scan(atoi(getenv("NJ_SEQ")));
    int engine = getenv("NJ_ENGINE") ? atoi(getenv("NJ_ENGINE")) : 0;
    nj_set_engine(engine);
    if (getenv("NJ_SCALAR")) nj_set_scalar_idct(1);
    for (int i = 1; i < argc; i++) {
        size_t n; uint8_t *d = load(argv[i], &n);
        nj_info fi;
        if (!d || nj_read_info(d, n, &fi) || !fi.supported) { printf("%s: unsupported\n", argv[i]); continue; }
        // planar: parallel vs 1 thread
        size_t pitch[3], tot = 0; uint8_t *pa[3], *pb[3];
        for (int c = 0; c < fi.ncomp; c++) { pitch[c] = (fi.plane_w[c] + 255) & ~255; tot += pitch[c] * fi.plane_h[c]; }
        uint8_t *A = calloc(1, tot), *B = calloc(1, tot), *p = A, *q = B;
        for (int c = 0; c < fi.ncomp; c++) { pa[c] = p; pb[c] = q; p += pitch[c] * fi.plane_h[c]; q += pitch[c] * fi.plane_h[c]; }
        nj_stats st;
        int r1 = nj_decode_planes(d, n, &fi, pa, pitch, 0, &st);
        nj_set_engine(1);   // reference: libjpeg-turbo, single thread
        int r2 = nj_decode_planes(d, n, &fi, pb, pitch, 1, NULL);
        nj_set_engine(engine);
        int planes_ok = !r1 && !r2 && !memcmp(A, B, tot);
        // BGRX: parallel vs TurboJPEG
        size_t sz = (size_t)fi.width * fi.height * 4;
        uint8_t *C = malloc(sz), *D = malloc(sz);
        int r3 = nj_decode_bgrx(d, n, &fi, C, fi.width * 4, 0, NULL);
        tjhandle h = tj3Init(TJINIT_DECOMPRESS);
        int r4 = tj3Decompress8(h, d, n, D, fi.width * 4, TJPF_BGRX);
        tj3Destroy(h);
        size_t diff = 0; int maxd = 0;
        for (size_t k = 0; k < sz; k++) if ((k & 3) != 3 && C[k] != D[k]) { diff++; int e = abs(C[k] - D[k]); if (e > maxd) maxd = e; }
        char ss[160];
        sync_summary(&st, ss, sizeof ss);
        const char *nm = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        printf("%-24s %5dx%-5d %s %3d bands  scan %4.1f sync %4.1f dec %5.1f ms | %s | planes %s bgrx %s\n",
               nm, fi.width, fi.height, st.mode == 2 ? "split" : st.mode == 1 ? "RST  " : "single", st.bands,
               st.scan_ms, st.sync_ms, st.decode_ms, ss,
               planes_ok ? "OK" : "MISMATCH", (!r3 && !r4 && !diff) ? "OK" : "DIFF");
        if (!planes_ok || r3) bad++;
        free(A); free(B); free(C); free(D); free(d);
    }
    if (nthr || hist[7]) {
        printf("\nsynchronisation over all images (%ld threads):\n", nthr + hist[7]);
        for (int k = 0; k < 8; k++)
            printf("  %-6s MCU: %6ld  (%5.1f%%)\n", hname[k], hist[k], 100.0 * hist[k] / (nthr + hist[7]));
        printf("  avg %.2f MCU / %.0f bits, max %ld MCU / %ld bits\n\n", (double)tot_mcus / nthr,
               (double)tot_bits / nthr, max_mcus, max_bits);
    }
    printf(bad ? "FAILURES: %d\n" : "all OK\n", bad);
    return bad != 0;
}
