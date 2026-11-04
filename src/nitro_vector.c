// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_vector: the vector drawings for the four viewers (see nitro_vector.h)
#include "nitro_vector.h"
#include "nitro_load.h"   // nv_has_ext, NV_MODE_*
#include "nitro_os.h"     // nitro_parallel, nitro_ncpu
#ifdef NV_SVG
#include "nitrosvg.h"
#include "nitrodxf.h"
#include "nitrometa.h"
#endif
#ifdef NV_CDR
#include "nitrocdr.h"
#include "nitrodio.h"
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const nv_vector_exts[] = {
#ifdef NV_SVG
    "svg", "dxf", "dwg", "wmf", "emf",
#endif
#ifdef NV_CDR
    "cdr", "cmx", "vsd", "vsdx", "vsdm", "vdx", "vst", "vstx", "vstm", "vtx", "vss", "vssx", "vssm", "vsx", "drawio", "dio",
#endif
    NULL};

const char *nv_vector_formats(void) {
#if defined(NV_SVG) && defined(NV_CDR)
    return "SVG, DXF / DWG, WMF / EMF, CorelDRAW, Visio and draw.io";
#elif defined(NV_SVG)
    return "SVG, DXF / DWG and WMF / EMF";
#elif defined(NV_CDR)
    return "CorelDRAW, Visio and draw.io";
#else
    return "";
#endif
}

const char *nv_vector_mimes(void) {
    return
#ifdef NV_SVG
        "image/svg+xml;image/vnd.dxf;image/vnd.dwg;image/wmf;image/emf;image/x-wmf;image/x-emf;"
#endif
#ifdef NV_CDR
        "image/x-coreldraw;application/vnd.visio;application/vnd.ms-visio.drawing.main+xml;application/vnd.ms-visio.drawing.macroEnabled.main+xml;"
        "application/vnd.ms-visio.stencil.main+xml;application/vnd.ms-visio.template.main+xml;"
#endif
        "";
}

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
static int meta_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                       int (*cancelled)(void *), void *ctx) {
    (void)page;
    return nitrometa_render(doc, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
}
static void meta_size(void *doc, int page, double *w, double *h) {
    (void)page;
    nitrometa_size(doc, w, h);
}
static void meta_free(void *doc) { nitrometa_free(doc); }
#endif
#ifdef NV_CDR
static int cdr_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                      int (*cancelled)(void *), void *ctx) {
    return nitrocdr_render(doc, page, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
}
static void cdr_size(void *doc, int page, double *w, double *h) { nitrocdr_size(doc, page, w, h); }
static void cdr_free(void *doc) { nitrocdr_free(doc); }
static int dio_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                      int (*cancelled)(void *), void *ctx) {
    return nitrodio_render(doc, page, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
}
static void dio_size(void *doc, int page, double *w, double *h) { nitrodio_size(doc, page, w, h); }
static void dio_free(void *doc) { nitrodio_free(doc); }
#endif

NvVector *nv_vector_open(const char *name, const void *data, size_t len) {
    NvVector *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->background = atomic_load(&g_background);
#ifdef NV_CDR
    static const char *const dio_exts[] = {"drawio", "dio", NULL};   // (before SVG: its text starts with '<' too)
    if (nv_has_ext(name, dio_exts) && (v->doc = nitrodio_load(data, len))) {
        nitrodio_set_background(v->doc, v->background);
        v->render = dio_render;
        v->size = dio_size;
        v->free = dio_free;
        v->pages = nitrodio_pages(v->doc);
        v->mode = NV_MODE_DRAWIO;
    }
#endif
#ifdef NV_SVG
    static const char *const svg_exts[] = {"svg", NULL}, *const dxf_exts[] = {"dxf", "dwg", NULL};   // (a DWG: nitrodxf reads it too, nitrodwg)
    const uint8_t *p = data;   // (SVG also by content: text starting with '<', after a byte order mark and white space)
    size_t i = len >= 3 && !memcmp(p, "\xef\xbb\xbf", 3) ? 3 : 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
    if (!v->doc && (nv_has_ext(name, svg_exts) || (i < len && p[i] == '<')) && (v->doc = nitrosvg_load(data, len))) {
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
    static const char *const meta_exts[] = {"wmf", "emf", NULL};
    if (!v->doc && nv_has_ext(name, meta_exts) && (v->doc = nitrometa_load(data, len))) {
        nitrometa_set_background(v->doc, v->background);
        v->render = meta_render;
        v->size = meta_size;
        v->free = meta_free;
        v->pages = 1;
        v->mode = NV_MODE_META;
    }
#endif
#ifdef NV_CDR
    static const char *const cdr_exts[] = {"cdr", "cmx", NULL};
    static const char *const visio_exts[] = {"vsd", "vsdx", "vsdm", "vdx", "vst", "vstx", "vstm", "vtx", "vss", "vssx", "vssm", "vsx", NULL};
    if (!v->doc && (nv_has_ext(name, cdr_exts) || nv_has_ext(name, visio_exts)) && (v->doc = nitrocdr_load(data, len))) {
        nitrocdr_set_background(v->doc, v->background);
        v->render = cdr_render;
        v->size = cdr_size;
        v->free = cdr_free;
        v->pages = nitrocdr_pages(v->doc);
        v->mode = nv_has_ext(name, visio_exts) ? NV_MODE_VISIO : NV_MODE_CDR;
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
int nv_vector_mode(const NvVector *v) { return v ? v->mode : 0; }
int nv_vector_background_of(const NvVector *v) { return v ? v->background : 0; }

void nv_vector_size(const NvVector *v, int page, double *w, double *h) {
    *w = *h = 0;
    v->size(v->doc, page, w, h);
}

// In bands of 128 rows at least, as many as the CPU's threads (or -j), rendered at once (each band skips
// what is outside it; SVG renders whole: lunasvg's calls take turns). One split render at a time:
// another one meanwhile (a zoomed view's detail while a page renders) is drawn whole on its own
// thread rather than waiting for the bands' threads. Every band asks cancelled; a stopped one
// stops the render.
static _Atomic int g_split, g_threads;

void nv_vector_set_threads(int n) {
    atomic_store(&g_threads, n > 0 ? n : 0);
#ifdef NV_CDR
    nitrocdr_set_threads(n);   // (a CorelDRAW / Visio document's pictures, decoded while it loads)
#endif
}

int nv_vector_render(NvVector *v, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx) {
    int threads = atomic_load(&g_threads), idle = 0;
    int bands = v->mode == NV_MODE_SVG ? 1 : MIN(threads ? threads : nitro_ncpu(), h / 128);
    if (bands < 2 || !atomic_compare_exchange_strong(&g_split, &idle, 1))
        return v->render(v->doc, page, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
    int rcs[256] = {0}, *rc = rcs;
    if (bands > 256) bands = 256;
    nitro_parallel((size_t)bands, ^(size_t i) {
        int b0 = (int)((long)h * (long)i / bands), b1 = (int)((long)h * (long)(i + 1) / bands);
        rc[i] = v->render(v->doc, page, bgra + (size_t)b0 * pitch, w, b1 - b0, pitch, scale, x0, y0 + b0 / scale, cancelled, ctx);
    });
    atomic_store(&g_split, 0);
    for (int i = 0; i < bands; i++)
        if (rcs[i]) return rcs[i];
    return 0;
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

static _Atomic int g_target_w, g_target_h;   // (nv_vector_set_target)

int nv_vector_set_target(int tw, int th) {
    if (tw == atomic_load(&g_target_w) && th == atomic_load(&g_target_h)) return 0;
    atomic_store(&g_target_w, tw);
    atomic_store(&g_target_h, th);
    return 1;
}

int nv_vector_page_too_small(int pw, int ph, int max_tex) {
    return nv_vector_too_small(pw, ph, atomic_load(&g_target_w), atomic_load(&g_target_h), max_tex);
}

uint8_t *nv_vector_render_page(NvVector *v, int page, const NvDecoder *dec, NvPage *p) {
    memset(p, 0, sizeof *p);
    p->scale = nv_vector_page_size(v, page, atomic_load(&g_target_w), atomic_load(&g_target_h), dec->max_tex, &p->w, &p->h);
    if (!p->scale) return NULL;
    size_t align = dec->align > 1 ? dec->align : 1;
    p->pitch = ((size_t)p->w * 4 + align - 1) / align * align;
    uint8_t *buf = dec->get(p->pitch * p->h, &p->cap, dec->ctx);
    if (!buf) return NULL;
    if (nv_vector_render(v, page, buf, p->w, p->h, p->pitch, p->scale, 0, 0, NULL, NULL)) {
        dec->put(buf, p->cap, dec->ctx);
        return NULL;
    }
    p->mode = v->mode;
    p->background = v->background;
    return buf;
}

int nv_vector_detail_rect(double iw, double ih, double scale, double ox, double oy, double vw, double vh, NvDetailRect *r) {
    memset(r, 0, sizeof *r);
    r->ox = ox;
    r->oy = oy;
    r->scale = scale;
    double l = MAX(0, ox), t = MAX(0, oy);
    double rr = MIN(vw, ox + iw * scale), b = MIN(vh, oy + ih * scale);
    r->w = (int)ceil(rr - l);
    r->h = (int)ceil(b - t);
    if (r->w <= 0 || r->h <= 0) return 0;
    r->x = (int)(l - ox);
    r->y = (int)(t - oy);
    r->dx = (l - ox) / scale;
    r->dy = (t - oy) / scale;
    r->dw = r->w / scale;
    r->dh = r->h / scale;
    return 1;
}

uint8_t *nv_vector_detail_render(NvVector *v, int page, double vscale, const NvDetailRect *r, int (*cancelled)(void *ctx),
                                 void *ctx) {
    if (r->w <= 0 || r->h <= 0 || (size_t)r->w > SIZE_MAX / 4 / (size_t)r->h) return NULL;
    size_t pitch = (size_t)r->w * 4;
    uint8_t *px = malloc(pitch * r->h);
    if (px && nv_vector_render(v, page, px, r->w, r->h, pitch, vscale * r->scale, r->dx / vscale, r->dy / vscale, cancelled, ctx)) {
        free(px);
        return NULL;
    }
    return px;
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

const char *nv_vector_title_suffix(const NvVector *v, int page, int background, char *buf, size_t n) {
    size_t k = 0;
    if (n) buf[0] = 0;
    if (v && v->pages > 1) k = (size_t)snprintf(buf, n, "  [page %d/%d]", page + 1, v->pages);
    if (v && background && k < n) snprintf(buf + k, n - k, "  %s", nv_vector_background_name(background));
    return buf;
}
