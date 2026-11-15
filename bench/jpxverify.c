// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Optional reference test, OpenJPEG is needed only for this executable:
//   make bench/jpxverify                         (pkg-config libopenjp2)
//   mkdir -p /tmp/jpx-tests
//   bench/jpxverify --selftest /tmp/jpx-tests     (all coding styles, geometry, depth, layers)
//   bench/jpxverify images.jp2 images.j2k ...    (existing codestreams)
#include "../src/nitrojpx.h"
#include <openjpeg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checked, failed, rejected, worst_diff;
static unsigned rng = 7;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void loud(const char *message, void *ctx) { (void)ctx; fputs(message, stderr); }
static void quiet(const char *message, void *ctx) { (void)message; (void)ctx; }
static uint8_t *load(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f); rewind(f);
    uint8_t *p = n > 0 ? malloc((size_t)n) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); p = NULL; }
    fclose(f); *len = n > 0 ? (size_t)n : 0; return p;
}
static int verify(const char *path, int tolerance) {
    size_t len = 0; uint8_t *d = load(path, &len);
    if (!d || len < 2) { free(d); fprintf(stderr, "cannot read %s\n", path); failed++; return -1; }
    njpx_image im;
    int ours = njpx_decode(d, len, 0, &im);
    opj_dparameters_t params; opj_set_default_decoder_parameters(&params);
    opj_codec_t *codec = opj_create_decompress(d[0] == 255 && d[1] == 79 ? OPJ_CODEC_J2K : OPJ_CODEC_JP2);
    opj_set_error_handler(codec, quiet, NULL); opj_set_warning_handler(codec, quiet, NULL);
    opj_stream_t *stream = opj_stream_create_default_file_stream(path, OPJ_TRUE);
    opj_image_t *ref = NULL;
    int ok = codec && stream && opj_setup_decoder(codec, &params) && opj_read_header(stream, codec, &ref) &&
             opj_decode(codec, stream, ref) && opj_end_decompress(codec, stream);
    int bad = 0, maxdiff = 0;
    if (!ok) { rejected++; printf("reference rejected (%s): %s\n", ours ? "both reject" : "nitrojpx decoded", path); }
    else if (ours || im.width != (int)(ref->x1 - ref->x0) || im.height != (int)(ref->y1 - ref->y0) || im.ncomp != (int)ref->numcomps) bad = 1;
    else {
        for (int y = 0; y < im.height; y++) for (int x = 0; x < im.width; x++) for (int c = 0; c < im.ncomp; c++) {
            const opj_image_comp_t *q = ref->comps + c;
            int sx = (int)((ref->x0 + x) / q->dx) - (int)q->x0, sy = (int)((ref->y0 + y) / q->dy) - (int)q->y0;
            if (sx < 0) sx = 0; if (sy < 0) sy = 0;
            if (sx >= (int)q->w) sx = (int)q->w - 1; if (sy >= (int)q->h) sy = (int)q->h - 1;
            int v = q->data[(size_t)sy * q->w + sx] + (q->sgnd ? 1 << (q->prec - 1) : 0);
            int diff = abs(v - im.samples[((size_t)y * im.width + x) * im.ncomp + c]);
            if (diff > maxdiff) maxdiff = diff;
            int tol = tolerance >= 0 ? tolerance : q->prec > 8 ? 4 : 2;
            if (diff > tol || im.precision[c] != (int)q->prec) bad = 1;
        }
    }
    if (ok) { checked++; if (bad) { failed++; printf("FAIL decode=%d maxdiff=%d: %s\n", ours, maxdiff, path); } }
    if (ok && maxdiff > worst_diff) worst_diff = maxdiff;
    njpx_free(&im); opj_image_destroy(ref); opj_stream_destroy(stream); opj_destroy_codec(codec); free(d);
    return bad ? -1 : 0;
}
static int generate(const char *path, int style, int config) {
    opj_cparameters_t params; opj_set_default_encoder_parameters(&params);
    int n = config < 0 ? 1 : 1 + config % 4;
    int precision = config < 0 ? 8 : (int[]){1, 8, 12, 16}[(config / 4) % 4];
    int sgnd = config >= 0 && (config / 16) % 2, lossy = config >= 0 && (config / 32) % 2;
    int subsample = config >= 0 && n >= 3 && config % 3 == 0;
    int ox = config >= 0 ? 3 : 0, oy = config >= 0 ? 5 : 0;
    opj_image_cmptparm_t cp[4] = {{0}};
    for (int c = 0; c < n; c++) {
        cp[c].dx = c && subsample ? 2 : 1; cp[c].dy = c && subsample ? 2 : 1;
        cp[c].x0 = (ox + cp[c].dx - 1) / cp[c].dx; cp[c].y0 = (oy + cp[c].dy - 1) / cp[c].dy;
        cp[c].w = (79 + ox + cp[c].dx - 1) / cp[c].dx - cp[c].x0;
        cp[c].h = (77 + oy + cp[c].dy - 1) / cp[c].dy - cp[c].y0;
        cp[c].prec = precision; cp[c].sgnd = sgnd;
    }
    opj_image_t *im = opj_image_create(n, cp, n < 3 ? OPJ_CLRSPC_GRAY : OPJ_CLRSPC_SRGB);
    if (!im) return -1;
    im->x0 = ox; im->y0 = oy; im->x1 = ox + 79; im->y1 = oy + 77;
    for (int c = 0; c < n; c++) for (unsigned i = 0; i < cp[c].w * cp[c].h; i++)
        im->comps[c].data[i] = (int)(rnd() & ((1u << precision) - 1)) - (sgnd ? 1 << (precision - 1) : 0);
    params.numresolution = 4; params.tcp_numlayers = 3;
    params.tcp_rates[0] = 8; params.tcp_rates[1] = 2; params.tcp_rates[2] = 1;
    params.cp_disto_alloc = 1; params.mode = style; params.irreversible = lossy;
    params.tcp_mct = n >= 3 && !subsample;
    if (config >= 0) {
        params.prog_order = (OPJ_PROG_ORDER)(config % 5);
        if (config & 1) { params.tile_size_on = 1; params.cp_tdx = 31; params.cp_tdy = 29; }
        if ((config & 2) && precision > 1) {
            params.csty = 1; params.res_spec = 1;
            params.prcw_init[0] = params.prch_init[0] = 64;
            params.cblockw_init = params.cblockh_init = 8;
        }
        if (config % 7 == 0 && precision <= 12) { params.roi_compno = 0; params.roi_shift = 5; }
    }
    opj_codec_t *codec = opj_create_compress(OPJ_CODEC_J2K);
    opj_set_error_handler(codec, loud, NULL); opj_set_warning_handler(codec, quiet, NULL);
    opj_stream_t *stream = opj_stream_create_default_file_stream(path, OPJ_FALSE);
    // Tiny offset/sub-sampled components exercise the full guard-bit range as well.
    int ok = codec && stream && opj_setup_encoder(codec, &params, im) &&
             opj_encoder_set_extra_options(codec, (const char *const[]){"GUARD_BITS=7", "PLT=YES", NULL}) && opj_start_compress(codec, im, stream) &&
             opj_encode(codec, stream) && opj_end_compress(codec, stream);
    opj_stream_destroy(stream); opj_destroy_codec(codec); opj_image_destroy(im);
    if (!ok) { fprintf(stderr, "reference encoder failed: %s\n", path); failed++; return -1; }
    return verify(path, lossy ? (precision > 8 ? 4 : 2) : 0);
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: jpxverify [--selftest existing-directory] [files.jp2 files.j2k ...]\n"); return 2; }
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--selftest") && a + 1 < argc) {
            const char *dir = argv[++a]; char path[4096];
            for (int style = 0; style < 64; style++) {
                snprintf(path, sizeof(path), "%s/style-%d.j2k", dir, style); generate(path, style, -1);
            }
            for (int config = 0; config < 160; config++) {
                snprintf(path, sizeof(path), "%s/config-%d.j2k", dir, config); generate(path, 0, config);
            }
        } else verify(argv[a], -1);
    }
    printf("%d compared, %d failed, %d reference-rejected, max difference %d.\n", checked, failed, rejected, worst_diff);
    return failed != 0;
}
