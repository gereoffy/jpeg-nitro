// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_vector: the vector drawings for the four viewers (see nitro_vector.h)
#include "nitro_vector.h"
#include "nitro_load.h"   // nv_has_ext, NV_MODE_*
#include "nitro_os.h"     // nitro_parallel, nitro_ncpu
#ifdef NV_VECTOR
#include "nitrosvg.h"
#include "nitrodxf.h"
#include "nitrometa.h"
#include "nitrocdr.h"
#include "nitrodio.h"
#include "nitropdf.h"
#include "nitrotype.h"
#include "nitrodjvu.h"
#include <zlib.h>   // (a gzip'd drawing: SVGZ, EMZ, WMZ)
#endif
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const nv_vector_exts[] = {
#ifdef NV_VECTOR   // (the library built with the readers: NITROVEC = 1)
    "svg", "svgz", "dxf", "dwg", "wmf", "emf", "wmz", "emz", "cdr", "cmx", "vsd", "vsdx", "vsdm", "vdx", "vst", "vstx", "vstm", "vtx", "vss", "vssx", "vssm", "vsx",
    "drawio", "dio", "pdf", "ai", "ttf", "otf", "ttc", "otc", "woff", "woff2", "eot", "pfb", "pfa", "djvu", "djv",
#endif
    NULL};

const char *nv_vector_formats(void) {
#ifdef NV_VECTOR
    return "SVG, DXF / DWG, WMF / EMF, CorelDRAW, Visio, draw.io, PDF, DjVu and fonts";
#else
    return "";
#endif
}

const char *nv_vector_mimes(void) {
#ifdef NV_VECTOR
    return "image/svg+xml;image/svg+xml-compressed;image/vnd.dxf;image/vnd.dwg;image/wmf;image/emf;image/x-wmf;image/x-emf;"
           "image/x-coreldraw;application/vnd.visio;application/vnd.ms-visio.drawing.main+xml;application/vnd.ms-visio.drawing.macroEnabled.main+xml;"
           "application/vnd.ms-visio.stencil.main+xml;application/vnd.ms-visio.template.main+xml;application/pdf;application/illustrator;"
           "font/ttf;font/otf;font/collection;font/woff;font/woff2;application/x-font-ttf;application/x-font-otf;application/vnd.ms-opentype;application/vnd.ms-fontobject;application/x-font-type1;image/vnd.djvu;image/x-djvu;";
#else
    return "";
#endif
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

#ifdef NV_VECTOR
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
static int pdf_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                      int (*cancelled)(void *), void *ctx) {
    return nitropdf_render(doc, page, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
}
static void pdf_size(void *doc, int page, double *w, double *h) { nitropdf_size(doc, page, w, h); }
static void pdf_free(void *doc) { nitropdf_free(doc); }
static int type_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                       int (*cancelled)(void *), void *ctx) {
    return nitrotype_render(doc, page, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
}
static void type_size(void *doc, int page, double *w, double *h) { nitrotype_size(doc, page, w, h); }
static void type_free(void *doc) { nitrotype_free(doc); }
static int djvu_render(void *doc, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                       int (*cancelled)(void *), void *ctx) {
    return nitrodjvu_render(doc, page, bgra, w, h, pitch, scale, x0, y0, cancelled, ctx);
}
static void djvu_size(void *doc, int page, double *w, double *h) { nitrodjvu_size(doc, page, w, h); }
static void djvu_free(void *doc) { nitrodjvu_free(doc); }

// The drawings by their content, whatever the name (a file opened or dropped on its own): text, magic numbers,
// a ZIP's or an OLE file's part names. The loaders check the files again.
static int has(const uint8_t *p, size_t len, const char *s) {   // s in p[0, len)
    size_t n = strlen(s);
    for (const uint8_t *q = p, *end = p + len; (size_t)(end - q) >= n && (q = memchr(q, s[0], (size_t)(end - q) - n + 1)); q++)
        if (!memcmp(q, s, n)) return 1;
    return 0;
}
static int is_space(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int dxf_text(const uint8_t *p, size_t len) {   // "0" then "SECTION", or a "999" comment first
    size_t i = 0;
    while (i < len && is_space(p[i])) i++;
    if (i + 4 <= len && !memcmp(p + i, "999", 3) && (p[i + 3] == '\r' || p[i + 3] == '\n')) return 1;
    if (i >= len || p[i++] != '0') return 0;
    while (i < len && (p[i] == ' ' || p[i] == '\r')) i++;
    if (i >= len || p[i] != '\n') return 0;
    while (i < len && is_space(p[i])) i++;
    return i + 7 <= len && !memcmp(p + i, "SECTION", 7);
}
static unsigned u16le(const uint8_t *p) { return p[0] | p[1] << 8; }
static int metafile(const uint8_t *p, size_t len) {   // EMF: its header record; WMF: the placeable or the plain header
    if (len >= 44 && !memcmp(p, "\1\0\0\0", 4) && !memcmp(p + 40, " EMF", 4)) return 1;
    if (len >= 22 && !memcmp(p, "\xd7\xcd\xc6\x9a", 4)) return 1;
    return len >= 18 && (u16le(p) == 1 || u16le(p) == 2) && u16le(p + 2) == 9 && (u16le(p + 4) == 0x100 || u16le(p + 4) == 0x300);
}
static int corel(const uint8_t *p, size_t len) {   // RIFF CDR / CMX; CorelDRAW X4+: a ZIP
    if (len >= 12 && !memcmp(p, "RIFF", 4)) {
        char t[4];
        for (int k = 0; k < 4; k++) t[k] = (char)(p[8 + k] | 0x20);
        return !memcmp(t, "cdr", 3) || !memcmp(t, "cmx1", 4);
    }
    return len >= 4 && !memcmp(p, "PK\3\4", 4) && (has(p, len, "content/riffData.cdr") || has(p, len, "content/root.dat"));
}
static int visio(const uint8_t *p, size_t len, size_t head) {   // VSD: an OLE file (or Visio 1-4); VSDX: a ZIP; VDX: XML
    if (len >= 8 && !memcmp(p, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)) return 1;
    if (len >= 18 && !memcmp(p, "Visio (TM) Drawing", 18)) return 1;
    if (len >= 4 && !memcmp(p, "PK\3\4", 4)) return has(p, len, "visio/");
    return has(p, head, "<VisioDocument");
}
static unsigned u16be(const uint8_t *p) { return p[0] << 8 | p[1]; }
// A gzip file's content (SVGZ, EMZ, WMZ: an SVG, an EMF or a WMF as Office and Inkscape pack them), from
// malloc; NULL: not gzip, damaged, or more than 256 MB.
static uint8_t *gunzip(const uint8_t *p, size_t len, size_t *out_len) {
    if (len < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8 || len > 0xffffffffu) return NULL;
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, 15 + 16) != Z_OK) return NULL;
    size_t cap = len * 4 + 4096, n = 0;
    uint8_t *out = malloc(cap);
    int rc = out ? Z_OK : Z_MEM_ERROR;
    z.next_in = (Bytef *)p, z.avail_in = (uInt)len;
    while (rc == Z_OK) {
        if (n == cap) {
            uint8_t *more = cap < (256u << 20) ? realloc(out, cap * 2) : NULL;
            if (!more) {
                rc = Z_MEM_ERROR;
                break;
            }
            out = more, cap *= 2;
        }
        size_t room = cap - n < 0x40000000u ? cap - n : 0x40000000u;
        z.next_out = out + n, z.avail_out = (uInt)room;
        rc = inflate(&z, Z_NO_FLUSH);
        n += room - z.avail_out;
    }
    inflateEnd(&z);
    if (rc != Z_STREAM_END || !n) {
        free(out);
        return NULL;
    }
    *out_len = n;
    return out;
}
// A font file: an sfnt (TrueType, OpenType: its table count and the search range that goes with it), a collection,
// WOFF and WOFF2, Embedded OpenType (its magic, and its size the file's), Type 1 (PFB's first segment header before the "%!",
// PFA's first line)
static int font_file(const uint8_t *p, size_t len) {
    if (len < 12) return 0;
    if (!memcmp(p, "\0\1\0\0", 4) || !memcmp(p, "OTTO", 4) || !memcmp(p, "true", 4) || !memcmp(p, "typ1", 4)) {
        unsigned tables = u16be(p + 4), pow2 = 1;
        while (pow2 * 2 <= tables) pow2 *= 2;
        return tables > 0 && tables <= 512 && u16be(p + 6) == pow2 * 16;
    }
    if (!memcmp(p, "ttcf", 4)) return !memcmp(p + 4, "\0\1\0\0", 4) || !memcmp(p + 4, "\0\2\0\0", 4);
    if (!memcmp(p, "wOFF", 4) || !memcmp(p, "wOF2", 4)) return 1;
    if (len >= 82 && p[34] == 0x4c && p[35] == 0x50 && (p[0] | p[1] << 8 | p[2] << 16 | (size_t)p[3] << 24) == len) return 1;
    if (p[0] == 0x80 && p[1] == 1) return !memcmp(p + 6, "%!", 2);
    return len >= 14 && (!memcmp(p, "%!PS-AdobeFont", 14) || !memcmp(p, "%!FontType1", 11));
}
#endif

NvVector *nv_vector_open(const char *name, const void *data, size_t len) {
    NvVector *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->background = atomic_load(&g_background);
#ifdef NV_VECTOR
    const uint8_t *p = data;
    // a gzip'd drawing: what is in it (the readers copy what they keep: it is freed below)
    size_t plain_len = 0;
    uint8_t *plain = gunzip(p, len, &plain_len);
    if (plain) data = p = plain, len = plain_len;
    size_t head = len < 1024 ? len : 1024;   // (the part where the text formats' and PDF's marks are looked for)
    static const char *const dio_exts[] = {"drawio", "dio", NULL};   // (before SVG: its text starts with '<' too)
    if ((nv_has_ext(name, dio_exts) || has(p, head, "<mxfile") || has(p, head, "<mxGraphModel")) && (v->doc = nitrodio_load(data, len))) {
        nitrodio_set_background(v->doc, v->background);
        v->render = dio_render;
        v->size = dio_size;
        v->free = dio_free;
        v->pages = nitrodio_pages(v->doc);
        v->mode = NV_MODE_DRAWIO;
    }
    static const char *const svg_exts[] = {"svg", "svgz", NULL}, *const dxf_exts[] = {"dxf", "dwg", NULL};   // (a DWG: nitrodxf reads it too, nitrodwg)
    // (SVG also by content: text starting with '<', after a byte order mark and white space)
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
    int dwg = len >= 6 && !memcmp(p, "AC10", 4) && p[4] >= '0' && p[4] <= '9' && p[5] >= '0' && p[5] <= '9';
    if (!v->doc && (nv_has_ext(name, dxf_exts) || dwg || dxf_text(p, head)) && (v->doc = nitrodxf_load(data, len))) {
        nitrodxf_set_background(v->doc, v->background);
        v->render = dxf_render;
        v->size = dxf_size;
        v->free = dxf_free;
        v->pages = nitrodxf_pages(v->doc);
        v->mode = NV_MODE_DXF;
    }
    static const char *const meta_exts[] = {"wmf", "emf", "wmz", "emz", NULL};
    if (!v->doc && (nv_has_ext(name, meta_exts) || metafile(p, len)) && (v->doc = nitrometa_load(data, len))) {
        nitrometa_set_background(v->doc, v->background);
        v->render = meta_render;
        v->size = meta_size;
        v->free = meta_free;
        v->pages = 1;
        v->mode = NV_MODE_META;
    }
    static const char *const cdr_exts[] = {"cdr", "cmx", NULL};
    static const char *const visio_exts[] = {"vsd", "vsdx", "vsdm", "vdx", "vst", "vstx", "vstm", "vtx", "vss", "vssx", "vssm", "vsx", NULL};
    int cdr_ext = nv_has_ext(name, cdr_exts), visio_ext = nv_has_ext(name, visio_exts);
    int is_visio = visio_ext || (!cdr_ext && !v->doc && visio(p, len, head));
    if (!v->doc && (cdr_ext || is_visio || corel(p, len)) && (v->doc = nitrocdr_load(data, len))) {
        nitrocdr_set_background(v->doc, v->background);
        v->render = cdr_render;
        v->size = cdr_size;
        v->free = cdr_free;
        v->pages = nitrocdr_pages(v->doc);
        v->mode = is_visio ? NV_MODE_VISIO : NV_MODE_CDR;
    }
    static const char *const pdf_exts[] = {"pdf", "ai", NULL};   // (Illustrator 9+ saves a PDF; also by content: "%PDF-" in the first 1 KB)
    if (!v->doc && (nv_has_ext(name, pdf_exts) || has(p, head, "%PDF-")) && (v->doc = nitropdf_load(data, len))) {
        nitropdf_set_background(v->doc, v->background);
        v->render = pdf_render;
        v->size = pdf_size;
        v->free = pdf_free;
        v->pages = nitropdf_pages(v->doc);
        v->mode = NV_MODE_PDF;
    }
    static const char *const font_exts[] = {"ttf", "otf", "ttc", "otc", "woff", "woff2", "eot", "pfb", "pfa", NULL};
    if (!v->doc && (nv_has_ext(name, font_exts) || font_file(p, len)) && (v->doc = nitrotype_load(data, len))) {
        nitrotype_set_background(v->doc, v->background);
        v->render = type_render;
        v->size = type_size;
        v->free = type_free;
        v->pages = nitrotype_pages(v->doc);
        v->mode = NV_MODE_FONT;
    }
    // DjVu (its mask layer: nitrodjvu); its file says what it is, whatever its name
    if (!v->doc && len >= 16 && !memcmp(p, "AT&TFORM", 8) && (v->doc = nitrodjvu_load(data, len))) {
        v->render = djvu_render;
        v->size = djvu_size;
        v->free = djvu_free;
        v->pages = nitrodjvu_pages(v->doc);
        v->mode = NV_MODE_DJVU;
    }
    free(plain);
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
#ifdef NV_VECTOR
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
