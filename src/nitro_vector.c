// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_vector: the vector drawings for the four viewers (see nitro_vector.h)
#include "nitro_vector.h"
#include "nitro_load.h"   // nv_has_ext, NV_MODE_*
#ifdef NV_SVG
#include "nitrosvg.h"
#include "nitrodxf.h"
#endif
#ifdef NV_CDR
#include "nitrocdr.h"
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

const char *const nv_vector_exts[] = {
#ifdef NV_SVG
    "svg", "dxf",
#endif
#ifdef NV_CDR
    "cdr", "cmx",
#endif
    NULL};

static _Atomic int g_background;   // (nv_vector_background)

struct NvVector {
    void *doc;
    int (*render)(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                  int (*cancelled)(void *), void *ctx);
    void (*size)(void *doc, int page, double *w, double *h);
    void (*free)(void *doc);
    int pages, mode, background;
    _Atomic int refs;
};

#ifdef NV_SVG
static int svg_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                      int (*cancelled)(void *), void *ctx) {
    (void)page, (void)cancelled, (void)ctx;   // (lunasvg draws a document in one call)
    return nitrosvg_render(doc, bgra, w, h, pitch, scale, x0, y0);
}
static void svg_size(void *doc, int page, double *w, double *h) {
    (void)page;
    nitrosvg_size(doc, w, h);
}
static void svg_free(void *doc) { nitrosvg_free(doc); }
static int dxf_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                      int (*cancelled)(void *), void *ctx) {
    return nitrodxf_render(doc, page, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
}
static void dxf_size(void *doc, int page, double *w, double *h) { nitrodxf_size(doc, page, w, h); }
static void dxf_free(void *doc) { nitrodxf_free(doc); }
#endif
#ifdef NV_CDR
static int cdr_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                      int (*cancelled)(void *), void *ctx) {
    return nitrocdr_render(doc, page, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
}
static void cdr_size(void *doc, int page, double *w, double *h) { nitrocdr_size(doc, page, w, h); }
static void cdr_free(void *doc) { nitrocdr_free(doc); }
#endif

NvVector *nv_vector_open(const char *name, const void *data, size_t len) {
    NvVector *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->background = atomic_load(&g_background);
#ifdef NV_SVG
    static const char *const svg_exts[] = {"svg", NULL}, *const dxf_exts[] = {"dxf", NULL};
    const uint8_t *p = data;   // (SVG also by content: text starting with '<', after a byte order mark and white space)
    size_t i = len >= 3 && !memcmp(p, "\xef\xbb\xbf", 3) ? 3 : 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
    if ((nv_has_ext(name, svg_exts) || (i < len && p[i] == '<')) && (v->doc = nitrosvg_load(data, len))) {
        nitrosvg_set_background(v->doc, v->background);
        v->render = svg_render;
        v->size = svg_size;
        v->free = svg_free;
        v->pages = 1;
        v->mode = NV_MODE_SVG;
    }
    if (!v->doc && nv_has_ext(name, dxf_exts) && (v->doc = nitrodxf_load(data, len))) {
        nitrodxf_set_background(v->doc, v->background);
        v->render = dxf_render;
        v->size = dxf_size;
        v->free = dxf_free;
        v->pages = nitrodxf_pages(v->doc);
        v->mode = NV_MODE_DXF;
    }
#endif
#ifdef NV_CDR
    static const char *const cdr_exts[] = {"cdr", "cmx", NULL};
    if (!v->doc && nv_has_ext(name, cdr_exts) && (v->doc = nitrocdr_load(data, len))) {
        nitrocdr_set_background(v->doc, v->background);
        v->render = cdr_render;
        v->size = cdr_size;
        v->free = cdr_free;
        v->pages = nitrocdr_pages(v->doc);
        v->mode = NV_MODE_CDR;
    }
#endif
    (void)name, (void)data, (void)len;
    if (!v->doc) {
        free(v);
        return NULL;
    }
    atomic_store(&v->refs, 1);
    return v;
}

NvVector *nv_vector_ref(NvVector *v) {
    if (v) atomic_fetch_add(&v->refs, 1);
    return v;
}

void nv_vector_unref(NvVector *v) {
    if (v && atomic_fetch_sub(&v->refs, 1) == 1) {
        v->free(v->doc);
        free(v);
    }
}

int nv_vector_pages(const NvVector *v) { return v ? v->pages : 0; }
int nv_vector_mode(const NvVector *v) { return v->mode; }
int nv_vector_background_of(const NvVector *v) { return v->background; }

void nv_vector_size(const NvVector *v, int page, double *w, double *h) {
    *w = *h = 0;
    v->size(v->doc, page, w, h);
}

int nv_vector_render(NvVector *v, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx) {
    return v->render(v->doc, page, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
}

double nv_vector_page_size(const NvVector *v, int page, int tw, int th, int max_tex, int *pw, int *ph) {
    double w, h;
    nv_vector_size(v, page, &w, &h);
    *pw = *ph = 0;
    if (!(w > 0 && h > 0)) return 0;
    double l = w > h ? w : h, k = l < 1024 ? 1024 / l : l > 4096 ? 4096 / l : 1;
    if (tw > 0 && th > 0) {
        double f = tw / w < th / h ? tw / w : th / h;
        if (f > k) k = f;
    }
    if (k > max_tex / l) k = max_tex / l;
    long x = lround(w * k), y = lround(h * k);
    *pw = (int)(x < 1 ? 1 : x > max_tex ? max_tex : x);
    *ph = (int)(y < 1 ? 1 : y > max_tex ? max_tex : y);
    return k;
}

int nv_vector_too_small(int pw, int ph, int tw, int th, int max_tex) {
    if (tw <= 0 || th <= 0 || pw <= 0 || ph <= 0) return 0;
    double f = (double)tw / pw < (double)th / ph ? (double)tw / pw : (double)th / ph;
    return f > 1.01 && (pw > ph ? pw : ph) < max_tex;
}

int nv_vector_background(void) { return atomic_load(&g_background); }

int nv_vector_cycle_background(int delta) {
    int b = (atomic_load(&g_background) + (delta < 0 ? 2 : 1)) % 3;
    atomic_store(&g_background, b);
    return b;
}

const char *nv_vector_background_name(int background) {
    return background == 1 ? "dark background" : background == 2 ? "black background" : "";
}
