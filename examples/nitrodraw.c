// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodraw: an example of using libnitrovec - a page of any drawing the library renders (SVG,
// DXF / DWG, WMF / EMF, CorelDRAW, Visio, draw.io) -> an uncompressed 24-bit BMP, over a white,
// dark or black background.
//
//   nitrodraw drawing.svg out.bmp              the first page, its long side 1024 pixels (small icons
//                                              enlarged, huge drawings reduced: nv_vector_page_size)
//   nitrodraw -p 2 -s 2048 -b 1 plan.dwg out.bmp   page 2 (counted from 1), 2048 pixels, the dark background
//   nitrodraw -t plan.dwg                      no BMP: just the pages, sizes and the load / render times
//
// nv_read_file reads the file, nv_vector_open loads it with the reader of its format (by the
// extension, SVG by content too), nv_vector_page_size picks the size and scale a page is shown at,
// nv_vector_render draws it into a BGRA buffer. Build (see the Makefile's examples/nitrodraw):
// clang -O3 -fblocks -Isrc examples/nitrodraw.c build/libnitrovec.a build/libnitro.a -lz -lc++ -liconv
// (on Linux -lstdc++ -lpthread instead of -lc++ -liconv; libdav1d.a and the frameworks as for
// nitroconvert, BUILDING.md).
#include "nitro_load.h"
#include "nitro_vector.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// a w x h BGRA buffer (pitch bytes per row) as a 24-bit BMP: 54-byte header, rows bottom-up, B G R padded to 4 bytes
static int write_bmp(const char *path, const uint8_t *bgra, int w, int h, size_t pitch) {
    size_t row = ((size_t)w * 3 + 3) & ~(size_t)3;
    uint8_t hdr[54] = {'B', 'M'}, *line = calloc(row, 1);
    le32(hdr + 2, (uint32_t)(54 + row * (size_t)h));
    le32(hdr + 10, 54);
    le32(hdr + 14, 40);
    le32(hdr + 18, (uint32_t)w);
    le32(hdr + 22, (uint32_t)h);
    hdr[26] = 1;
    hdr[28] = 24;
    le32(hdr + 34, (uint32_t)(row * (size_t)h));
    FILE *f = line ? fopen(path, "wb") : NULL;
    if (!f) { free(line); return -1; }
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *s = bgra + (size_t)y * pitch;   // (B G R of B G R A: the first three bytes)
        for (int x = 0; x < w; x++) { line[3 * x] = s[4 * x]; line[3 * x + 1] = s[4 * x + 1]; line[3 * x + 2] = s[4 * x + 2]; }
        fwrite(line, 1, row, f);
    }
    free(line);
    return fclose(f) ? -1 : 0;
}

int main(int argc, char **argv) {
    int page = 1, side = 1024, background = 0, times = 0, a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (!strcmp(argv[a], "-p") && a + 1 < argc) page = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-s") && a + 1 < argc) side = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-b") && a + 1 < argc) background = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-t")) times = 1;
        else break;
    }
    if (a >= argc || (!times && a + 1 >= argc)) {
        fprintf(stderr, "usage: nitrodraw [-p page] [-s pixels] [-b 0|1|2] <drawing> <out.bmp>\n       nitrodraw -t <drawing>   (the pages and the times only)\n");
        return 2;
    }
    const char *in = argv[a], *out = a + 1 < argc ? argv[a + 1] : NULL;
    if (side < 16 || side > 16384) { fprintf(stderr, "the size: 16 .. 16384 pixels\n"); return 2; }

    size_t len = 0;
    uint8_t *data = nv_read_file(in, &len);
    if (!data) { fprintf(stderr, "%s: cannot read\n", in); return 1; }
    nv_vector_cycle_background(background);   // (the background the drawings loaded from now on are over: 0, 1, 2)
    double t0 = now_ms();
    NvVector *v = nv_vector_open(in, data, len);
    double load = now_ms() - t0;
    free(data);
    if (!v) { fprintf(stderr, "%s: not a drawing the library renders\n", in); return 1; }
    int pages = nv_vector_pages(v);
    printf("%s: %s, %d page%s", in, nv_mode_name(nv_vector_mode(v)), pages, pages > 1 ? "s" : "");
    if (times) printf(", loaded in %.0f ms", load);
    printf("\n");

    int first = times ? 0 : page - 1, last = times ? pages - 1 : page - 1, rc = 0;
    if (first < 0 || first >= pages) { fprintf(stderr, "%s: no page %d\n", in, page); nv_vector_unref(v); return 1; }
    for (int p = first; p <= last; p++) {
        double w, h;
        nv_vector_size(v, p, &w, &h);
        int pw, ph;
        double k = nv_vector_page_size(v, p, 0, 0, side, &pw, &ph);   // (the long side 1024 .. side, as the viewers show it)
        if (k > 0 && (pw > ph ? pw : ph) < side) k = nv_vector_page_size(v, p, side, side, side, &pw, &ph);   // (at least side: fitted into side x side)
        printf("  page %d: %.1f x %.1f units", p + 1, w, h);
        if (!(k > 0)) { printf(", nothing to draw\n"); continue; }
        size_t pitch = (size_t)pw * 4;
        uint8_t *buf = malloc(pitch * (size_t)ph);
        if (!buf) { fprintf(stderr, "out of memory\n"); rc = 1; break; }
        t0 = now_ms();
        int err = nv_vector_render(v, p, buf, pw, ph, pitch, k, 0, 0, NULL, NULL);
        double render = now_ms() - t0;
        printf(", %d x %d pixels (%.3f per unit)", pw, ph, k);
        if (times) printf(", rendered in %.0f ms", render);
        printf("\n");
        if (err) { fprintf(stderr, "%s: page %d: render failed\n", in, p + 1); rc = 1; }
        else if (out && write_bmp(out, buf, pw, ph, pitch)) { fprintf(stderr, "%s: cannot write\n", out); rc = 1; }
        else if (out) printf("-> %s: %dx%d, 24-bit BMP\n", out, pw, ph);
        free(buf);
    }
    nv_vector_unref(v);
    return rc;
}
