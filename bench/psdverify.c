// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitropsd check: single- vs multi-threaded output must match, and every
// compression variant must match the raw variant next to it (NAME_raw.psd).
// Optionally dumps the planes (DUMP=dir) for an external comparison.
//   psdverify files.psd...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/nitro_os.h"   // nitro_now_ms
#include "../src/nitropsd.h"

static double now_ms(void) { return nitro_now_ms(); }
static uint8_t *load(const char *fn, size_t *n) {
    FILE *f = fopen(fn, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n); fread(b, 1, *n, f); fclose(f); return b;
}
static uint8_t *decode(const char *fn, ps_info *fi, int nt, double *ms) {
    size_t n; uint8_t *d = load(fn, &n);
    if (!d || ps_read_info(d, n, fi) || !fi->supported) { free(d); return NULL; }
    uint8_t *o = malloc(fi->plane_size * (fi->ncolor + fi->alpha));
    double t = now_ms();
    int rc = ps_decode(d, n, fi, o, nt, NULL);
    if (ms) *ms = now_ms() - t;
    free(d);
    if (rc) { free(o); return NULL; }
    return o;
}

int main(int argc, char **argv) {
    int bad = 0;
    static const char *cn[] = {"raw", "RLE", "ZIP", "ZIP+pred"};
    for (int i = 1; i < argc; i++) {
        const char *nm = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        ps_info fi; double ms1 = 0, msn = 0;
        uint8_t *a = decode(argv[i], &fi, 1, &ms1), *b = decode(argv[i], &fi, 0, &msn);
        if (!a || !b) {
            printf("%-26s %s (depth %d, mode %d, compression %d)\n", nm, fi.supported ? "DECODE FAILED" : "unsupported",
                   fi.depth, fi.mode, fi.compression);
            if (fi.supported) bad++;
            free(a); free(b); continue;
        }
        size_t sz = fi.plane_size * (fi.ncolor + fi.alpha);
        int same = !memcmp(a, b, sz);
        // raw sibling: replace the part after the last '_' before the extension with "raw"
        char ref[4096]; const char *us = strrchr(argv[i], '_'), *dot = strrchr(argv[i], '.');
        const char *vs = "";
        if (us && dot && dot > us && fi.compression != 0) {
            snprintf(ref, sizeof ref, "%.*s_raw.psd", (int)(us - argv[i]), argv[i]);
            ps_info rf; uint8_t *r = decode(ref, &rf, 1, NULL);
            if (r && rf.plane_size == fi.plane_size && rf.ncolor + rf.alpha >= fi.ncolor + fi.alpha)
                vs = !memcmp(r, a, sz) ? "  = raw" : "  != RAW";
            if (strstr(vs, "!=")) bad++;
            free(r);
        }
        const char *dump = getenv("DUMP");
        if (dump) { char p[4096]; snprintf(p, sizeof p, "%s/%s.planes", dump, nm); FILE *f = fopen(p, "wb"); fwrite(b, 1, sz, f); fclose(f); }
        printf("%-26s %5dx%-5d %s%s %-8s 1 thread %7.1f ms, parallel %6.1f ms  %s%s\n", nm, fi.width, fi.height,
               fi.ncolor == 3 ? "RGB" : "gray", fi.alpha ? "+A" : "  ", cn[fi.compression], ms1, msn,
               same ? "1=N" : "1!=N MISMATCH", vs);
        if (!same) bad++;
        free(a); free(b);
    }
    printf(bad ? "FAILURES: %d\n" : "all OK\n", bad);
    return bad != 0;
}
