// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrodjvu: see nitrodjvu.h. Sections of the DjVu Reference in brackets. Where the reference's
// text leaves something out (the two coding templates' figures, the relative locations' rules) the
// code says so: those follow what the files themselves decode to. The pictures: nitroiw44.
#include "nitrodjvu.h"
#include "nitro_bilevel.h"
#include "nitro_zp.h"
#include "nitroiw44.h"
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// --- BZZ (appendix 4): the directory's names ------------------------------------------------------

// a number of bits by a binary tree of contexts (cx[1 .. 2^bits - 1]), or without contexts
static unsigned bzz_bin(Zp *z, uint8_t *cx, int bits) {
    unsigned n = 1, m = 1u << bits;
    while (n < m) n = n << 1 | (unsigned)zp_decode(z, &cx[n]);
    return n - m;
}
static unsigned bzz_raw(Zp *z, int bits) {
    unsigned n = 1, m = 1u << bits;
    while (n < m) n = n << 1 | (unsigned)zp_raw(z);
    return n - m;
}

// The BZZ stream decoded, at most limit bytes (from malloc; NULL: damaged or too long). Blocks, each:
// its size, the speed of the frequency estimate, its symbols as positions in a move-to-front array
// kept in order of an estimate of their frequencies, then the Burrows-Wheeler transform undone.
static uint8_t *bzz_decode(const uint8_t *p, size_t len, size_t limit, size_t *out_len) {
    Zp z;
    zp_init(&z, p, len);
    uint8_t cx[300], *out = NULL, *blk = NULL;
    uint32_t *pos = NULL;
    size_t n = 0, cap = 0;
    memset(cx, 0, sizeof cx);
    for (;;) {
        unsigned size = bzz_raw(&z, 24);
        if (!size) break;   // (the end)
        if (size > (4096u << 10) || z.over > 8) goto fail;
        int fshift = 0;
        if (zp_raw(&z)) fshift = zp_raw(&z) ? 2 : 1;
        uint8_t mtf[256];
        for (int k = 0; k < 256; k++) mtf[k] = (uint8_t)k;
        uint32_t freq[4] = {0, 0, 0, 0}, fadd = 4;
        unsigned mtfno = 3;
        int marker = -1;
        free(blk), free(pos);
        blk = malloc(size), pos = malloc((size_t)size * sizeof *pos);
        if (!blk || !pos) goto fail;
        for (unsigned i = 0; i < size; i++) {
            unsigned id = mtfno < 2 ? mtfno : 2;
            uint8_t *c = cx;
            if (zp_decode(&z, &c[id])) mtfno = 0;
            else if (zp_decode(&z, &c[3 + id])) mtfno = 1;
            else {
                c += 6;
                int bits = 1;
                for (; bits <= 7; bits++) {   // (a group of 2^bits positions: its flag, then which of them)
                    if (zp_decode(&z, &c[0])) break;
                    c += 1u << bits;
                }
                if (bits > 7) {   // the end-of-block symbol: where the transform's thread starts
                    mtfno = 256;
                    blk[i] = 0;
                    marker = (int)i;
                    continue;
                }
                mtfno = (1u << bits) + bzz_bin(&z, c, bits);
            }
            if (z.over > 8) goto fail;
            uint8_t sym = mtf[mtfno];
            blk[i] = sym;
            // the symbol moves towards the front, as far as its frequency estimate takes it
            fadd += fadd >> fshift;
            if (fadd > 0x10000000) {
                fadd >>= 24;
                for (int k = 0; k < 4; k++) freq[k] >>= 24;
            }
            uint32_t fc = fadd + (mtfno < 4 ? freq[mtfno] : 0);
            unsigned k = mtfno;
            for (; k >= 4; k--) mtf[k] = mtf[k - 1];
            for (; k > 0 && fc >= freq[k - 1]; k--) mtf[k] = mtf[k - 1], freq[k] = freq[k - 1];
            mtf[k] = sym, freq[k] = fc;
        }
        if (marker < 1) goto fail;
        // the inverse transform: each symbol's rank among its equals, the start of each symbol's run
        // in the sorted block (the end-of-block symbol first), then the thread followed backwards
        uint32_t count[256] = {0};
        for (unsigned i = 0; i < size; i++) {
            if ((int)i == marker) continue;
            uint8_t c = blk[i];
            pos[i] = (uint32_t)c << 24 | (count[c]++ & 0xffffff);
        }
        uint32_t last = 1;
        for (int k = 0; k < 256; k++) {
            uint32_t t = count[k];
            count[k] = last, last += t;
        }
        if (n + size - 1 > limit) goto fail;
        if (n + size > cap) {
            cap = (n + size) * 2;
            uint8_t *more = realloc(out, cap);
            if (!more) goto fail;
            out = more;
        }
        uint32_t i = 0;
        for (last = size - 1; last > 0;) {
            if (i >= size || (int)i == marker) goto fail;
            uint32_t v = pos[i];
            out[n + --last] = (uint8_t)(v >> 24);
            i = count[v >> 24] + (v & 0xffffff);
        }
        if ((int)i != marker) goto fail;
        n += size - 1;
    }
    free(blk), free(pos);
    if (!out) out = malloc(1);
    *out_len = n;
    return out;
fail:
    free(blk), free(pos), free(out);
    return NULL;
}

// --- JB2 (appendix 2) ------------------------------------------------------------------------------

#define JB2_BIG 262142        // the largest number coded (11.5.2)
#define JB2_MAX_CELLS (1u << 22)
#define JB2_MAX_PIXELS (200u << 20)   // a page, and all of a library's shapes together

// A symbol of the library: its bitmap, and the box of its black pixels in it (columns; rows counted
// from the bottom, as the format's coordinates are: an empty one has right < left, top < bottom).
typedef struct {
    Bm bm;
    int left, right, bottom, top;
} Shape;

typedef struct {   // a decoded shape dictionary (Djbz), shared by the pages that include it
    Shape *shapes;
    int n, inherited;   // (its first inherited shapes are another dictionary's)
} Dict;

typedef struct {
    Zp z;
    // the numbers' contexts: binary trees grown as they are walked (11.2.2); a node's index 0: not there yet
    uint8_t *bit;
    uint32_t *left, *right, ncells, cap;
    uint32_t c_record, c_size, c_match, c_w, c_h, c_dw, c_dh, c_col, c_row, c_hsame, c_vsame, c_hnew, c_vnew, c_clen, c_cbyte, c_dict;
    uint8_t c_refine, c_offset, direct[1024], cross[2048];
    Shape *lib;
    int nlib, caplib, inherited;   // (the first inherited ones are a dictionary's: not freed here)
    size_t pixels;
    int started, width, height, err, old;   // (old: files up to version 18 code a copy's place by its whole bitmap)
    Bm page;
    int have_page;
    // a page whose shapes have colours of their own (FGbz): the palette entry of each shape put on
    // the page, in their order; `entry`: the entries of the shapes' pixels (width x height, rows from
    // the top; made at the start of the image when blit_entry is set, NULL if there was no memory)
    const uint8_t *blit_entry;
    int blit_entries, blits;
    uint8_t *entry;
    // where the last symbols were put (rows from the bottom, 1 the lowest: 11.3)
    int last_right, last_row_left, last_row_bottom, base[3], base_at;
} Jb2;

static uint32_t jb2_cell(Jb2 *j) {
    if (j->ncells >= j->cap) {
        uint32_t cap = j->cap ? j->cap * 2 : 4096;
        if (cap > JB2_MAX_CELLS) return 0;
        uint8_t *b = realloc(j->bit, cap);
        if (b) j->bit = b;
        uint32_t *l = realloc(j->left, (size_t)cap * sizeof *l);
        if (l) j->left = l;
        uint32_t *r = realloc(j->right, (size_t)cap * sizeof *r);
        if (r) j->right = r;
        if (!b || !l || !r) return 0;
        j->cap = cap;
    }
    uint32_t c = j->ncells++;
    j->bit[c] = 0, j->left[c] = j->right[c] = 0;
    return c;
}

// A number in low .. high by its context's tree (11.2.5): its sign, the range its magnitude is in
// (0, 1-2, 3-6, 7-14 ...), then its bits; a decision its bounds settle is not coded.
static int jb2_num(Jb2 *j, uint32_t *root, int low, int high) {
    int negative = 0, cutoff = 0, phase = 1;
    uint32_t range = 0xffffffffu, *at = root;
    while (range != 1) {
        if (!*at) {
            // (a new node: the arrays may move, so the place it hangs from is found again by its index)
            uint32_t *base_l = j->left, *base_r = j->right;
            int in_left = at >= base_l && at < base_l + j->cap, in_right = at >= base_r && at < base_r + j->cap;
            size_t idx = in_left ? (size_t)(at - base_l) : in_right ? (size_t)(at - base_r) : 0;
            uint32_t c = jb2_cell(j);
            if (!c) {
                j->err = 1;
                return 0;
            }
            if (in_left) at = j->left + idx;
            else if (in_right) at = j->right + idx;
            *at = c;
        }
        uint32_t node = *at;
        int decision = low < cutoff && high >= cutoff ? zp_decode(&j->z, &j->bit[node]) : low >= cutoff;
        at = decision ? &j->right[node] : &j->left[node];
        switch (phase) {
        case 1:
            negative = !decision;
            if (negative) {
                int t = -low - 1;
                low = -high - 1, high = t;
            }
            phase = 2, cutoff = 1;
            break;
        case 2:
            if (!decision) {
                phase = 3;
                range = (uint32_t)(cutoff + 1) / 2;
                if (range == 1) cutoff = 0;
                else cutoff -= (int)(range / 2);
            } else {
                cutoff += cutoff + 1;
                if (cutoff > 2 * JB2_BIG + 2) {   // (damage: beyond every range there is)
                    j->err = 1;
                    return 0;
                }
            }
            break;
        default:
            range /= 2;
            if (range != 1) cutoff += decision ? (int)(range / 2) : -(int)(range / 2);
            else if (!decision) cutoff--;
            break;
        }
    }
    if (j->z.over > ZP_PAST) j->err = 1;
    return negative ? -cutoff - 1 : cutoff;
}

// (a "reset" record: the numbers' trees forgotten; the pixels' contexts stay)
static void jb2_reset(Jb2 *j) {
    j->ncells = 1;   // (0: "no node")
    j->c_record = j->c_size = j->c_match = j->c_w = j->c_h = j->c_dw = j->c_dh = j->c_col = j->c_row = 0;
    j->c_hsame = j->c_vsame = j->c_hnew = j->c_vnew = j->c_clen = j->c_cbyte = j->c_dict = 0;
}

static int shape_new(Jb2 *j, Shape *s, int w, int h) {
    memset(s, 0, sizeof *s);
    if (w < 0 || h < 0 || w > 65535 || h > 65535 || (size_t)w * (size_t)h > JB2_MAX_PIXELS - j->pixels) return -1;
    if (bm_new(&s->bm, w, h)) return -1;
    j->pixels += (size_t)w * (size_t)h + 64;
    return 0;
}

// A bitmap coded on its own (11.5.9), its rows from the top: each pixel in the context of ten
// already coded ones, three in the row two above (x-1 .. x+1), five in the row above (x-2 .. x+2)
// and the two before it. (The reference's figure 2 is not in its text: the template is the one
// the files decode with.)
static void jb2_direct(Jb2 *j, Bm *b) {
    for (int y = 0; y < b->h && j->z.over <= ZP_PAST; y++) {
        uint8_t *r = bm_row(b, y);
        const uint8_t *r1 = bm_row(b, y - 1), *r2 = bm_row(b, y - 2);
        unsigned cx = (unsigned)(r2[0] << 8 | r2[1] << 7 | r1[0] << 4 | r1[1] << 3 | r1[2] << 2);
        for (int x = 0; x < b->w; x++) {
            int v = zp_decode(&j->z, &j->direct[cx]);
            r[x] = (uint8_t)v;
            cx = ((cx << 1) & 0x37a) | (unsigned)(r2[x + 2] << 7 | r1[x + 3] << 2 | v);
        }
    }
}

// A bitmap coded as a refinement of a library shape (11.5.10): each pixel in the context of four of
// its own (the row above: x-1 .. x+1; the one before it) and seven of the shape's, where the two
// lie centre on centre: the pixel above, the three around the same place, the three below. The
// centres: of the bitmap, and of the shape's box of black pixels; of two middle columns the left
// one, of two middle rows the lower one.
static void jb2_cross(Jb2 *j, Bm *b, const Shape *ref) {
    int dw = b->w, dh = b->h, ch = ref->bm.h;
    int xd = (dw / 2 - dw + 1) - ((ref->right - ref->left + 1) / 2 - ref->right);
    int yd = (dh / 2 - dh + 1) - ((ref->top - ref->bottom + 1) / 2 - ref->top);   // (rows from the bottom)
    int ydt = (ch - dh) - yd;   // (the shape's row, from its top, beside the bitmap's row y: y + ydt)
    for (int y = 0; y < dh && j->z.over <= ZP_PAST; y++) {
        uint8_t *r = bm_row(b, y);
        const uint8_t *r1 = bm_row(b, y - 1);
        int ry = y + ydt;
        for (int x = 0; x < dw; x++) {
            int rx = x + xd;
            unsigned cx = (unsigned)(r1[x - 1] << 10 | r1[x] << 9 | r1[x + 1] << 8 | r[x - 1] << 7) | bm_px(&ref->bm, rx, ry - 1) << 6 |
                          bm_px(&ref->bm, rx - 1, ry) << 5 | bm_px(&ref->bm, rx, ry) << 4 | bm_px(&ref->bm, rx + 1, ry) << 3 |
                          bm_px(&ref->bm, rx - 1, ry + 1) << 2 | bm_px(&ref->bm, rx, ry + 1) << 1 | bm_px(&ref->bm, rx + 1, ry + 1);
            r[x] = (uint8_t)zp_decode(&j->z, &j->cross[cx]);
        }
    }
}

// the box of a shape's black pixels
static void shape_box(Shape *s) {
    const Bm *b = &s->bm;
    int l = b->w, r = -1, t = b->h, bt = -1;
    for (int y = 0; y < b->h; y++) {
        const uint8_t *row = bm_row(b, y);
        for (int x = 0; x < b->w; x++)
            if (row[x]) {
                if (x < l) l = x;
                if (x > r) r = x;
                if (y < t) t = y;
                bt = y;
            }
    }
    if (r < 0) s->left = 0, s->right = -1, s->bottom = 0, s->top = -1;
    else s->left = l, s->right = r, s->bottom = b->h - 1 - bt, s->top = b->h - 1 - t;
}

static int jb2_add(Jb2 *j, Shape *s) {
    if (j->nlib == j->caplib) {
        int cap = j->caplib ? j->caplib * 2 : 256;
        Shape *more = realloc(j->lib, (size_t)cap * sizeof *more);
        if (!more) return -1;
        j->lib = more, j->caplib = cap;
    }
    shape_box(s);
    j->lib[j->nlib++] = *s;
    return 0;
}

static int median3(const int *v) {
    int a = v[0], b = v[1], c = v[2];
    return a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
}

// A symbol's place relative to those before it (11.5.8; the reference's text ends halfway, the
// rest is as the files are coded): the first of a new line of text by the first of the line before
// (its left edge, and its bottom edge to the new one's top), any other by the one before it (that
// one's right edge, and the line's baseline: the median of the last three bottoms). rows x cols:
// the symbol's; *left, *bottom: its lower left pixel, columns and rows from 1, the rows from the bottom.
static void jb2_place(Jb2 *j, int rows, int cols, int *left, int *bottom) {
    if (zp_decode(&j->z, &j->c_offset)) {
        int dx = jb2_num(j, &j->c_hnew, -JB2_BIG - 1, JB2_BIG), dy = jb2_num(j, &j->c_vnew, -JB2_BIG - 1, JB2_BIG);
        *left = j->last_row_left + dx;
        *bottom = j->last_row_bottom + dy - rows + 1;
        j->last_row_left = *left, j->last_row_bottom = *bottom;
        j->last_right = *left + cols - 1;
        j->base[0] = j->base[1] = j->base[2] = *bottom, j->base_at = 0;
    } else {
        int dx = jb2_num(j, &j->c_hsame, -JB2_BIG - 1, JB2_BIG), dy = jb2_num(j, &j->c_vsame, -JB2_BIG - 1, JB2_BIG);
        *left = j->last_right + dx;
        *bottom = median3(j->base) + dy;
        j->last_right = *left + cols - 1;
        if (++j->base_at == 3) j->base_at = 0;
        j->base[j->base_at] = *bottom;
    }
}

// the shape onto the page, its lower left pixel at left, bottom (from 1; rows from the bottom)
static void jb2_blit(Jb2 *j, const Bm *b, int left, int bottom) {
    int k = j->blits++;
    if (!j->have_page || left <= -70000 || left >= 70000 || bottom <= -70000 || bottom >= 70000) return;
    int x0 = left - 1, y0 = j->height - (bottom - 1 + b->h);
    bm_compose(&j->page, b, x0, y0, 0);
    if (!j->entry || k >= j->blit_entries) return;
    uint8_t e = j->blit_entry[k];
    for (int y = y0 < 0 ? -y0 : 0; y < b->h && y0 + y < j->height; y++) {
        const uint8_t *s = bm_row(b, y);
        uint8_t *o = j->entry + (size_t)(y0 + y) * (size_t)j->width;
        for (int x = x0 < 0 ? -x0 : 0; x < b->w && x0 + x < j->width; x++)
            if (s[x]) o[x0 + x] = e;
    }
}

// The records of a JB2 stream (11.4): a page's (page: its picture built) or a dictionary's. dict:
// the shapes it inherits (NULL: none; needed if the stream asks for them). 0: read to its end; -1:
// it stopped before (cut short or damaged: a page has what was drawn until then, j->have_page).
static int jb2_decode(Jb2 *j, const uint8_t *p, size_t len, const Dict *dict, int page) {
    zp_init(&j->z, p, len);
    jb2_reset(j);
    if (!jb2_cell(j)) return -1;   // (node 0)
    for (long records = 0; records < 50000000 && !j->err; records++) {
        int type = jb2_num(j, &j->c_record, 0, 11);
        if (j->err) break;
        if (type == 11) return 0;
        if (type == 0) {   // start of image: its size (a dictionary's: 0 x 0)
            int w = jb2_num(j, &j->c_size, 0, JB2_BIG), h = jb2_num(j, &j->c_size, 0, JB2_BIG);
            zp_decode(&j->z, &j->c_refine);   // (the "eventual refinement" flag: never set)
            if (j->err) break;
            if (j->started) break;   // (a second one: damage)
            if (page) {
                if (w < 1 || h < 1 || w > 65535 || h > 65535 || (size_t)w * (size_t)h > JB2_MAX_PIXELS || bm_new(&j->page, w, h)) break;
                j->have_page = 1;
                if (j->blit_entry) j->entry = calloc((size_t)w, (size_t)h);
            }
            j->width = w, j->height = h, j->started = 1;
            j->last_right = j->last_row_left = 0, j->last_row_bottom = h;
            j->base[0] = j->base[1] = j->base[2] = h, j->base_at = 0;
            continue;
        }
        if (type == 9) {   // before the start: the shapes of a shared dictionary; after it: the numbers' contexts reset
            if (j->started) {
                jb2_reset(j);
                if (!jb2_cell(j)) break;
                continue;
            }
            int n = jb2_num(j, &j->c_dict, 0, JB2_BIG);
            if (j->err || n < 0 || j->nlib || (n && (!dict || n > dict->n))) break;
            j->lib = malloc((size_t)(n ? n : 1) * sizeof *j->lib);
            if (!j->lib) break;
            if (n) memcpy(j->lib, dict->shapes, (size_t)n * sizeof *j->lib);
            j->nlib = j->caplib = j->inherited = n;
            continue;
        }
        if (type == 10) {   // a comment
            int n = jb2_num(j, &j->c_clen, 0, JB2_BIG);
            for (int k = 0; k < n && !j->err; k++) jb2_num(j, &j->c_cbyte, 0, 255);
            continue;
        }
        if (!j->started) break;
        Shape s;
        int left = 0, bottom = 0;
        if (type == 7) {   // a library shape as it is: placed by the box of its black pixels
            int m = j->nlib ? jb2_num(j, &j->c_match, 0, j->nlib - 1) : -1;
            if (j->err || m < 0) break;
            const Shape *ref = &j->lib[m];
            if (j->old) jb2_place(j, ref->bm.h, ref->bm.w, &left, &bottom);
            else {
                jb2_place(j, ref->top - ref->bottom + 1, ref->right - ref->left + 1, &left, &bottom);
                left -= ref->left, bottom -= ref->bottom;
            }
            if (j->err) break;
            jb2_blit(j, &ref->bm, left, bottom);
            continue;
        }
        if (type <= 3 || type == 8) {   // a new bitmap, coded on its own
            int w = jb2_num(j, &j->c_w, 0, JB2_BIG), h = jb2_num(j, &j->c_h, 0, JB2_BIG);
            if (j->err || shape_new(j, &s, w, h)) break;
            jb2_direct(j, &s.bm);
        } else {   // (4, 5, 6) a refinement of a library shape: its size by that shape's box
            int m = j->nlib ? jb2_num(j, &j->c_match, 0, j->nlib - 1) : -1;
            if (j->err || m < 0) break;
            const Shape *ref = &j->lib[m];
            int w = ref->right - ref->left + 1 + jb2_num(j, &j->c_dw, -JB2_BIG - 1, JB2_BIG);
            int h = ref->top - ref->bottom + 1 + jb2_num(j, &j->c_dh, -JB2_BIG - 1, JB2_BIG);
            if (j->err || shape_new(j, &s, w, h)) break;
            jb2_cross(j, &s.bm, &j->lib[m]);
        }
        if (j->z.over > ZP_PAST) {
            free(s.bm.mem);
            break;
        }
        int to_lib = type == 1 || type == 2 || type == 4 || type == 5, to_image = type == 1 || type == 3 || type == 4 || type == 6 || type == 8;
        if (type == 8) {   // not a symbol: at its place on the page, its top left given
            left = jb2_num(j, &j->c_col, 1, j->width > 0 ? j->width : 1);
            bottom = jb2_num(j, &j->c_row, 1, j->height > 0 ? j->height : 1) - s.bm.h + 1;
        } else if (to_image)
            jb2_place(j, s.bm.h, s.bm.w, &left, &bottom);
        if (to_image && !j->err) jb2_blit(j, &s.bm, left, bottom);
        if (j->err || !to_lib || jb2_add(j, &s)) {   // (not kept: its pixels are free for others again)
            j->pixels -= (size_t)s.bm.w * (size_t)s.bm.h + 64;
            free(s.bm.mem);
            if (!j->err && to_lib) break;
        }
    }
    return -1;
}

static void jb2_free(Jb2 *j, int keep_lib) {
    free(j->bit), free(j->left), free(j->right);
    if (!keep_lib) {
        for (int k = j->inherited; k < j->nlib; k++) free(j->lib[k].bm.mem);
        free(j->lib);
    }
    free(j->page.mem), free(j->entry);
}

// --- the document ----------------------------------------------------------------------------------

typedef struct {   // a component file of the document: a page, or one others include
    size_t at, len;   // its FORM's content (after its "DJVU" / "DJVI")
    char *id;
    int is_page;
    Dict *dict;       // the shape dictionary it brings, decoded (its own Djbz, or the one it includes: dict_own 0)
    int dict_tried, dict_own;
} Comp;

// A picture layer of a page (its background or its foreground colours), decoded: planes as nitroiw44
// gives them, turned like the page. It is smaller than the page by a whole factor, and lies on it
// from its bottom left corner (the picture's top row may reach above the page's).
typedef struct {
    uint8_t *y, *cb, *cr;   // (cb, cr NULL: grey)
    int w, h, r;            // r: the page's pixels to one of its
    int ox, oy;             // where its top left corner is on the turned page (0 or less)
} Layer;

typedef struct {
    int comp, w, h, turn, version;   // (turn: quarter turns clockwise to show it upright)
    int decoded;                      // what it has is in memory: the mask, the layers
    int unread;                       // it has a mask or a picture, but none that could be decoded (a grey sheet;
                                      //   a page with nothing on it is a white one)
    uint8_t *bits;                    // the mask: rows of packed bits (1: foreground), turned; NULL: none
    size_t rowbytes;
    int bw, bh;
    Layer bg, fg;                     // (y NULL: none. Without foreground colours the mask is black)
    uint8_t *pal, *entry;             // the foreground's colours by a palette (FGbz): RGB triplets; each mask
                                      //   pixel's entry (bw x bh; NULL: all of the mask is entry 0)
    size_t bytes;
    int users, stamp;
} Page;

#define KEEP_PAGES 4            // decoded pages kept (the ones drawn last), and no more of them than
#define KEEP_BYTES (96u << 20)  // this (a photograph page at full size is tens of MB)

struct nitrodjvu {
    uint8_t *data;
    size_t len;
    Comp *comps;
    int ncomps;
    Page *pages;
    int npages, stamp;
    pthread_mutex_t lock;
};

// the next chunk of a FORM's content from *at: its id, its data; 0 at the end
static int chunk(const nitrodjvu *d, size_t *at, size_t end, const uint8_t **id, const uint8_t **data, size_t *len) {
    if (*at + 8 > end) return 0;
    const uint8_t *p = d->data + *at;
    size_t n = be32(p + 4);
    if (n > end - *at - 8) n = end - *at - 8;   // (a file cut short: what is there)
    *id = p, *data = p + 8, *len = n;
    *at += 8 + n + (n & 1);
    return 1;
}

static int comp_by_id(const nitrodjvu *d, const uint8_t *id, size_t n) {
    while (n && (id[n - 1] == 0 || id[n - 1] == ' ' || id[n - 1] == '\n' || id[n - 1] == '\r')) n--;
    for (int k = 0; k < d->ncomps; k++)
        if (d->comps[k].id && strlen(d->comps[k].id) == n && !memcmp(d->comps[k].id, id, n)) return k;
    return -1;
}

// The shape dictionary a component brings: its own Djbz, which may build on one it includes in turn
// (depth: no deeper than a few). NULL: none, or damaged. Decoded once, kept with the document.
static const Dict *comp_dict(nitrodjvu *d, int c, int depth) {
    Comp *co = &d->comps[c];
    if (co->dict_tried) return co->dict;
    co->dict_tried = 1;
    if (depth > 4) return NULL;
    const Dict *inherited = NULL;
    const uint8_t *id, *data, *djbz = NULL;
    size_t at = co->at, n, djbz_len = 0;
    while (chunk(d, &at, co->at + co->len, &id, &data, &n)) {
        if (!memcmp(id, "INCL", 4)) {
            int k = comp_by_id(d, data, n);
            const Dict *x = k >= 0 && k != c ? comp_dict(d, k, depth + 1) : NULL;
            if (x) inherited = x;
        } else if (!memcmp(id, "Djbz", 4) && !djbz)
            djbz = data, djbz_len = n;
    }
    if (!djbz) {
        co->dict = (Dict *)inherited;   // (a file that only passes another's on)
        return inherited;
    }
    Jb2 *j = calloc(1, sizeof *j);
    Dict *out = calloc(1, sizeof *out);
    if (j && out && !jb2_decode(j, djbz, djbz_len, inherited, 0)) {
        out->shapes = j->lib, out->n = j->nlib, out->inherited = j->inherited;
        jb2_free(j, 1);
        free(j);
        co->dict = out, co->dict_own = 1;
        return out;
    }
    if (j) jb2_free(j, 0);
    free(j), free(out);
    return NULL;
}

// a plane of w x h bytes turned by quarter turns clockwise, into a new one (NULL: out of memory); the old one is freed
static uint8_t *plane_turn(uint8_t *src, int w, int h, int turn) {
    if (!src || !turn) return src;
    int ow = turn & 1 ? h : w;
    uint8_t *dst = malloc((size_t)w * (size_t)h);
    for (int y = 0; dst && y < h; y++)
        for (int x = 0; x < w; x++) {
            int ox = turn == 1 ? h - 1 - y : turn == 2 ? w - 1 - x : y;
            int oy = turn == 1 ? x : turn == 2 ? h - 1 - y : w - 1 - x;
            dst[(size_t)oy * (size_t)ow + (size_t)ox] = src[(size_t)y * (size_t)w + (size_t)x];
        }
    free(src);
    return dst;
}

static void layer_free(Layer *l) {
    free(l->y), free(l->cb), free(l->cr);
    memset(l, 0, sizeof *l);
}

// A layer of a page decoded: all its chunks of the kind `id` (BG44: the slices of one picture, each
// refining it; FG44: one), as planes turned like the page. pw x ph: the page as its file has it.
static void layer_decode(const nitrodjvu *d, const Comp *co, const char *kind, const Page *pg, int pw, int ph, Layer *l) {
    niw44 *iw = NULL;
    const uint8_t *id, *data;
    size_t at = co->at, n;
    while (chunk(d, &at, co->at + co->len, &id, &data, &n))
        if (!memcmp(id, kind, 4) && (iw || (iw = niw44_new())) && niw44_chunk(iw, data, n)) break;
    int w, h, nc = iw ? niw44_size(iw, &w, &h) : 0;
    if (nc) {
        // how many times smaller than the page it is [10.3: its size is the page's divided by 1 .. 12, rounded up]
        int r = 0;
        for (int k = 1; k <= 12 && !r; k++)
            if ((pw + k - 1) / k == w && (ph + k - 1) / k == h) r = k;
        if (!r) r = (pw + w / 2) / w;
        if (r < 1) r = 1;
        size_t px = (size_t)w * (size_t)h;
        uint8_t *y = malloc(px), *cb = nc == 3 ? malloc(px) : NULL, *cr = nc == 3 ? malloc(px) : NULL;
        if (y && (nc == 1 || (cb && cr)) && !niw44_planes(iw, y, cb, cr, (size_t)w)) {
            // its corners on the page: the left and the bottom edges are the page's; then turned with it
            int x0 = 0, y0 = ph - h * r, x1 = w * r, y1 = ph, t = pg->turn;
            l->ox = t == 1 ? ph - y1 : t == 2 ? pw - x1 : t == 3 ? y0 : x0;
            l->oy = t == 1 ? x0 : t == 2 ? ph - y1 : t == 3 ? pw - x1 : y0;
            l->w = t & 1 ? h : w, l->h = t & 1 ? w : h, l->r = r;
            l->y = plane_turn(y, w, h, t), l->cb = plane_turn(cb, w, h, t), l->cr = plane_turn(cr, w, h, t);
            if (!l->y || (nc == 3 && (!l->cb || !l->cr))) layer_free(l);
        } else
            free(y), free(cb), free(cr);
    }
    niw44_free(iw);
}

// The colours of a page's shapes [8.3.10]: a palette (BGR), then for each shape put on the page, in
// their order, its palette index (16 bits each, BZZ coded). Out: the colours in use as RGB triplets
// (at most 256: one beyond takes the nearest of those), and each blit's entry among them. 0: none.
static int fgbz(const uint8_t *p, size_t n, uint8_t **pal_out, uint8_t **entries_out, int *blits_out) {
    if (n < 6 || !(p[0] & 0x80)) return 0;   // (without the per-shape part there is nothing to colour by)
    size_t np = (size_t)(p[1] << 8 | p[2]);
    if (!np || n < 3 + 3 * np + 3) return 0;
    const uint8_t *bgr = p + 3, *q = bgr + 3 * np;
    size_t blits = (size_t)q[0] << 16 | (size_t)q[1] << 8 | q[2], len = 0;
    if (!blits || blits > (4u << 20)) return 0;
    uint8_t *idx = bzz_decode(q + 3, n - 3 - 3 * np - 3, 2 * blits, &len);
    if (!idx) return 0;
    if (len / 2 < blits) blits = len / 2;
    uint8_t *pal = malloc(256 * 3), *entries = malloc(blits ? blits : 1);
    int *entry_of = malloc(np * sizeof *entry_of), used = 0;
    if (!pal || !entries || !entry_of || !blits) {
        free(idx), free(pal), free(entries), free(entry_of);
        return 0;
    }
    for (size_t k = 0; k < np; k++) entry_of[k] = -1;
    for (size_t k = 0; k < blits; k++) {
        size_t i = (size_t)(idx[2 * k] << 8 | idx[2 * k + 1]);
        if (i >= np) i = 0;
        if (entry_of[i] < 0) {
            const uint8_t *c = bgr + 3 * i;
            if (used < 256) {
                pal[3 * used] = c[2], pal[3 * used + 1] = c[1], pal[3 * used + 2] = c[0];
                entry_of[i] = used++;
            } else {
                long best = LONG_MAX;
                for (int e = 0; e < 256; e++) {
                    long dr = pal[3 * e] - c[2], dg = pal[3 * e + 1] - c[1], db = pal[3 * e + 2] - c[0], dist = dr * dr + dg * dg + db * db;
                    if (dist < best) best = dist, entry_of[i] = e;
                }
            }
        }
        entries[k] = (uint8_t)entry_of[i];
    }
    free(idx), free(entry_of);
    *pal_out = pal, *entries_out = entries, *blits_out = (int)blits;
    return used;
}

// A page decoded: its mask (Sjbz) with the dictionary its file has or includes, as packed rows; its
// background picture (BG44) and its foreground's colours (a palette entry for each shape: FGbz, or
// a picture: FG44); all turned as its INFO says. A mask
// cut short or damaged: as much of it as was drawn (what follows a damaged spot decodes as noise:
// a stray letter here and there); a picture likewise: as sharp as the slices before the damage make it.
static void page_decode(nitrodjvu *d, Page *pg) {
    Comp *co = &d->comps[pg->comp];
    const Dict *dict = comp_dict(d, pg->comp, 0);
    const uint8_t *id, *data, *sjbz = NULL, *colours = NULL;
    size_t at = co->at, n, sjbz_len = 0, colours_len = 0;
    int layers = 0;
    while (chunk(d, &at, co->at + co->len, &id, &data, &n)) {
        if (!memcmp(id, "Sjbz", 4) && !sjbz) sjbz = data, sjbz_len = n;
        if (!memcmp(id, "FGbz", 4) && !colours) colours = data, colours_len = n;
        layers |= !memcmp(id, "Sjbz", 4) || !memcmp(id, "Smmr", 4) || !memcmp(id, "BG44", 4) || !memcmp(id, "BGjp", 4);
    }
    pg->decoded = 1, pg->bytes = 0;
    int pw = pg->turn & 1 ? pg->h : pg->w, ph = pg->turn & 1 ? pg->w : pg->h;
    layer_decode(d, co, "BG44", pg, pw, ph, &pg->bg);
    Jb2 *j = sjbz ? calloc(1, sizeof *j) : NULL;
    if (j) {
        j->old = pg->version <= 18;
        uint8_t *entries = NULL;
        int used = colours ? fgbz(colours, colours_len, &pg->pal, &entries, &j->blit_entries) : 0;
        if (used > 1) j->blit_entry = entries;   // (one colour: no need to tell the pixels apart)
        jb2_decode(j, sjbz, sjbz_len, dict, 1);   // (cut short or damaged: what there is of it)
        free(entries);
        if (j->have_page) {
            const Bm *b = &j->page;
            int turned = pg->turn & 1, ow = turned ? b->h : b->w, oh = turned ? b->w : b->h;
            size_t rb = ((size_t)ow + 7) / 8;
            uint8_t *bits = calloc(rb * (size_t)oh, 1);
            if (bits) {
                for (int y = 0; y < b->h; y++) {
                    const uint8_t *s = bm_row(b, y);
                    for (int x = 0; x < b->w; x++) {
                        if (!s[x]) continue;
                        int ox = pg->turn == 1 ? b->h - 1 - y : pg->turn == 2 ? b->w - 1 - x : pg->turn == 3 ? y : x;
                        int oy = pg->turn == 1 ? x : pg->turn == 2 ? b->h - 1 - y : pg->turn == 3 ? b->w - 1 - x : y;
                        bits[(size_t)oy * rb + (ox >> 3)] |= (uint8_t)(0x80 >> (ox & 7));
                    }
                }
                pg->bits = bits, pg->rowbytes = rb, pg->bw = ow, pg->bh = oh;
                pg->bytes += rb * (size_t)oh;
                if (j->entry) {
                    pg->entry = plane_turn(j->entry, b->w, b->h, pg->turn), j->entry = NULL;
                    if (pg->entry) pg->bytes += (size_t)ow * (size_t)oh;
                }
            }
        }
        jb2_free(j, 0);
        free(j);
    }
    if (pg->bits && !pg->pal) layer_decode(d, co, "FG44", pg, pw, ph, &pg->fg);
    for (int k = 0; k < 2; k++) {
        const Layer *l = k ? &pg->fg : &pg->bg;
        if (l->y) pg->bytes += (size_t)l->w * (size_t)l->h * (l->cb ? 3 : 1);
    }
    pg->unread = layers && !pg->bits && !pg->bg.y;
}

static void page_drop(Page *pg) {
    free(pg->bits), free(pg->pal), free(pg->entry);
    pg->bits = pg->pal = pg->entry = NULL;
    layer_free(&pg->bg), layer_free(&pg->fg);
    pg->decoded = 0, pg->bytes = 0;
}

// a page's file read for what a page is: its INFO (the size, the version, the turn)
static int page_info(const nitrodjvu *d, const Comp *co, Page *pg) {
    const uint8_t *id, *data;
    size_t at = co->at, n;
    if (!chunk(d, &at, co->at + co->len, &id, &data, &n) || memcmp(id, "INFO", 4) || n < 4) return -1;
    int w = data[0] << 8 | data[1], h = data[2] << 8 | data[3], flags = n >= 10 ? data[9] & 7 : 0;
    if (w < 1 || h < 1) return -1;
    pg->version = n >= 5 ? data[4] : 0;
    // (the low bits of the flags: 1 upright, 6 a quarter turn counter-clockwise, 2 upside down, 5 clockwise)
    pg->turn = flags == 6 ? 3 : flags == 2 ? 2 : flags == 5 ? 1 : 0;
    pg->w = pg->turn & 1 ? h : w, pg->h = pg->turn & 1 ? w : h;
    return 0;
}

nitrodjvu *nitrodjvu_load(const void *data, size_t len) {
    const uint8_t *p = data;
    if (!p || len < 16 || len > (size_t)INT_MAX || memcmp(p, "AT&TFORM", 8)) return NULL;
    int multi = !memcmp(p + 12, "DJVM", 4);
    if (!multi && memcmp(p + 12, "DJVU", 4)) return NULL;
    nitrodjvu *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    pthread_mutex_init(&d->lock, NULL);
    d->data = malloc(len);
    if (!d->data) goto fail;
    memcpy(d->data, p, len);
    d->len = len;
    size_t end = 12 + (size_t)be32(p + 8);
    if (end > len) end = len;
    if (!multi) {
        d->comps = calloc(1, sizeof *d->comps);
        if (!d->comps) goto fail;
        d->ncomps = 1;
        d->comps[0].at = 16, d->comps[0].len = end - 16, d->comps[0].is_page = 1;
    } else {
        // the directory (8.3.2): bundled or not, the count, each file's offset; then, BZZ coded, their
        // sizes, their flags (the low six bits 1: a page) and their names
        const uint8_t *id, *dir;
        size_t at = 16, n;
        if (!chunk(d, &at, end, &id, &dir, &n) || memcmp(id, "DIRM", 4) || n < 3 || !(dir[0] & 0x80)) goto fail;   // (not bundled: its pages are other files)
        int count = dir[1] << 8 | dir[2];
        if (!count || n < 3 + 4 * (size_t)count) goto fail;
        size_t names_len = 0;
        uint8_t *names = bzz_decode(dir + 3 + 4 * (size_t)count, n - 3 - 4 * (size_t)count, 16u << 20, &names_len);
        d->comps = calloc((size_t)count, sizeof *d->comps);
        if (!d->comps) {
            free(names);
            goto fail;
        }
        d->ncomps = count;
        size_t s = 4 * (size_t)count;   // (after the sizes and the flags: the names)
        for (int k = 0; k < count; k++) {
            Comp *co = &d->comps[k];
            size_t off = be32(dir + 3 + 4 * (size_t)k);
            int flags = names && names_len >= 4 * (size_t)count ? names[3 * (size_t)count + k] : -1;
            if (off + 12 <= len && !memcmp(d->data + off, "FORM", 4)) {
                size_t fl = be32(d->data + off + 4);
                if (fl > len - off - 8) fl = len - off - 8;
                co->at = off + 12, co->len = fl >= 4 ? fl - 4 : 0;
                // (without the directory's flags: a FORM:DJVU is a page)
                co->is_page = flags >= 0 ? (flags & 0x3f) == 1 : !memcmp(d->data + off + 8, "DJVU", 4);
            }
            for (int str = 0; names && flags >= 0 && str < 1 + !!(flags & 0x80) + !!(flags & 0x40); str++) {   // its id, then a name, a title
                size_t e = s;
                while (e < names_len && names[e]) e++;
                if (!str && (co->id = malloc(e - s + 1))) memcpy(co->id, names + s, e - s), co->id[e - s] = 0;
                s = e < names_len ? e + 1 : e;
            }
        }
        free(names);
    }
    for (int k = 0; k < d->ncomps; k++) d->npages += d->comps[k].is_page;
    d->pages = calloc((size_t)(d->npages ? d->npages : 1), sizeof *d->pages);
    if (!d->pages) goto fail;
    d->npages = 0;
    for (int k = 0; k < d->ncomps; k++) {
        Page pg;
        memset(&pg, 0, sizeof pg);
        pg.comp = k;
        if (d->comps[k].is_page && !page_info(d, &d->comps[k], &pg)) d->pages[d->npages++] = pg;
    }
    if (d->npages) return d;
fail:
    nitrodjvu_free(d);
    return NULL;
}

void nitrodjvu_free(nitrodjvu *d) {
    if (!d) return;
    for (int k = 0; k < d->ncomps; k++) {
        Comp *co = &d->comps[k];
        if (co->dict_own) {   // (its own shapes: those after the ones it inherited)
            for (int n = co->dict->inherited; n < co->dict->n; n++) free(co->dict->shapes[n].bm.mem);
            free(co->dict->shapes);
            free(co->dict);
        }
        free(co->id);
    }
    for (int k = 0; k < d->npages; k++) page_drop(&d->pages[k]);
    free(d->comps), free(d->pages), free(d->data);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

int nitrodjvu_pages(const nitrodjvu *d) { return d ? d->npages : 0; }

void nitrodjvu_size(const nitrodjvu *d, int page, double *w, double *h) {
    *w = *h = 0;
    if (!d || page < 0 || page >= d->npages) return;
    *w = d->pages[page].w, *h = d->pages[page].h;
}

// the black pixels of a row of packed bits in the columns x0 .. x1 - 1
static unsigned black(const uint8_t *row, int x0, int x1) {
    static const uint8_t ones[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    unsigned n = 0;
    for (int x = x0; x < x1;) {
        if (!(x & 7) && x + 8 <= x1) {
            uint8_t b = row[x >> 3];
            n += ones[b >> 4] + ones[b & 15];
            x += 8;
        } else {
            n += row[x >> 3] >> (7 - (x & 7)) & 1;
            x++;
        }
    }
    return n;
}

// The pixels of a layer an output pixel takes along one axis. Enlarged (f >= 0): a and b = a + 1,
// b's share f / 256. Reduced (f < 0): every -f-th of the run a .. b - 1, averaged.
typedef struct {
    int a, b, f;
} Span;

// (a coordinate far off any page: kept within what an int holds)
static int whole(double v) { return v < -1e9 ? -1000000000 : v > 1e9 ? 1000000000 : (int)v; }

// pa .. pb: the output pixel's extent on the page; o, r: the layer's origin and reduction; n: its size
static void span(Span *s, double pa, double pb, int o, int r, int n) {
    double la = (pa - o) / r, lb = (pb - o) / r;
    if (lb - la > 1) {
        int a = whole(floor(la)), b = whole(ceil(lb));
        if (a < 0) a = 0;
        if (b > n) b = n;
        if (b <= a) a = a >= n ? n - 1 : a, b = a + 1;
        s->a = a, s->b = b, s->f = -((b - a + 3) / 4);
    } else {
        double v = (la + lb) / 2 - 0.5, fl = floor(v);
        int a = whole(fl), f = (int)((v - fl) * 256);
        if (a < 0) a = 0, f = 0;
        if (a >= n - 1) a = n - 1, f = 0;
        s->a = a, s->b = a + 1 < n ? a + 1 : a, s->f = f;
    }
}

// the layer's colour for an output pixel, as RGB
static void layer_rgb(const Layer *l, const Span *sx, const Span *sy, uint8_t *rgb) {
    const uint8_t *pl[3] = {l->y, l->cb, l->cr};
    int v[3] = {0, 128, 128}, nc = l->cb ? 3 : 1;
    size_t w = (size_t)l->w;
    if (sx->f >= 0) {
        int fx = sx->f, fy = sy->f;
        for (int k = 0; k < nc; k++) {
            const uint8_t *r0 = pl[k] + (size_t)sy->a * w, *r1 = pl[k] + (size_t)sy->b * w;
            int top = r0[sx->a] * (256 - fx) + r0[sx->b] * fx, bot = r1[sx->a] * (256 - fx) + r1[sx->b] * fx;
            v[k] = (top * (256 - fy) + bot * fy + 32768) >> 16;
        }
    } else {
        for (int k = 0; k < nc; k++) {
            unsigned sum = 0, cnt = 0;
            for (int y = sy->a; y < sy->b; y -= sy->f) {
                const uint8_t *r = pl[k] + (size_t)y * w;
                for (int x = sx->a; x < sx->b; x -= sx->f) sum += r[x], cnt++;
            }
            v[k] = (int)((sum + cnt / 2) / cnt);
        }
    }
    if (nc == 3) niw44_rgb(v[0], v[1], v[2], rgb);
    else rgb[0] = rgb[1] = rgb[2] = (uint8_t)v[0];
}

// the palette colours of the mask's pixels in the rows r0 .. r1 - 1 and columns c0 .. c1 - 1, added up
static void palette_sum(const Page *pg, int r0, int r1, int c0, int c1, unsigned *sum) {
    for (int r = r0; r < r1; r++) {
        const uint8_t *row = pg->bits + (size_t)r * pg->rowbytes, *e = pg->entry + (size_t)r * (size_t)pg->bw;
        for (int x = c0; x < c1; x++) {
            if (!(x & 7) && x + 8 <= c1 && !row[x >> 3]) {
                x += 7;
                continue;
            }
            if (row[x >> 3] >> (7 - (x & 7)) & 1) {
                const uint8_t *c = pg->pal + 3 * e[x];
                sum[0] += c[0], sum[1] += c[1], sum[2] += c[2];
            }
        }
    }
}

// A page for drawing (a user more of it, until page_release): decoded now, or still kept; the pages drawn
// longest ago are let go, but for those being drawn.
static Page *page_hold(nitrodjvu *d, int page) {
    Page *pg = &d->pages[page];
    pthread_mutex_lock(&d->lock);
    if (!pg->decoded) {
        page_decode(d, pg);
        for (;;) {
            int kept = 0;
            size_t bytes = 0;
            Page *old = NULL;
            for (int k = 0; k < d->npages; k++) {
                Page *o = &d->pages[k];
                if (!o->decoded) continue;
                kept++, bytes += o->bytes;
                if (o != pg && !o->users && (!old || o->stamp < old->stamp)) old = o;
            }
            if (!old || (kept <= KEEP_PAGES && bytes <= KEEP_BYTES)) break;
            page_drop(old);
        }
    }
    pg->stamp = ++d->stamp;
    pg->users++;
    pthread_mutex_unlock(&d->lock);
    return pg;
}
static void page_release(nitrodjvu *d, Page *pg) {
    pthread_mutex_lock(&d->lock);
    pg->users--;
    pthread_mutex_unlock(&d->lock);
}

void nitrodjvu_prepare(nitrodjvu *d, int page) {
    if (d && page >= 0 && page < d->npages) page_release(d, page_hold(d, page));
}

int nitrodjvu_render(nitrodjvu *d, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx) {
    if (!d || page < 0 || page >= d->npages || w <= 0 || h <= 0 || pitch < (size_t)w * 4 || !(scale > 0)) return -1;
    Page *pg = page_hold(d, page);
    int rc = 0;
    const Layer *bg = pg->bg.y ? &pg->bg : NULL, *fg = pg->fg.y ? &pg->fg : NULL;
    // per column: the mask's columns it covers (enlarged: the one at its middle), the layers' pixels
    int *c0 = NULL, *c1 = NULL;
    Span *bx = NULL, *fx = NULL;
    if (pg->bits || bg) {
        c0 = malloc((size_t)w * 2 * sizeof *c0), c1 = c0 ? c0 + w : NULL;
        bx = malloc((size_t)w * 2 * sizeof *bx), fx = bx ? bx + w : NULL;
        if (!c0 || !bx) {
            free(c0), free(bx);
            page_release(d, pg);
            return -1;
        }
        for (int i = 0; i < w; i++) {
            double xa = x0 + i / scale, xb = x0 + (i + 1) / scale;
            int a = whole(floor(scale >= 1 ? (xa + xb) / 2 : xa)), b = scale >= 1 ? a + 1 : whole(ceil(xb));
            if (a < 0) a = 0;
            if (b > pg->bw) b = pg->bw;
            c0[i] = a, c1[i] = b;
            if (bg) span(&bx[i], xa, xb, bg->ox, bg->r, bg->w);
            if (fg) span(&fx[i], xa, xb, fg->ox, fg->r, fg->w);
        }
    }
    for (int j = 0; j < h; j++) {
        if ((j & 63) == 0 && cancelled && cancelled(ctx)) {
            rc = 1;
            break;
        }
        uint32_t *o = (uint32_t *)(bgra + (size_t)j * pitch);
        if (!pg->bits && !bg) {   // (neither a mask nor a picture: an empty page, or one that cannot be read)
            for (int i = 0; i < w; i++) o[i] = pg->unread ? 0xffd8d8d8u : 0xffffffffu;
            continue;
        }
        double ya = y0 + j / scale, yb = y0 + (j + 1) / scale;
        int r0 = whole(floor(scale >= 1 ? (ya + yb) / 2 : ya)), r1 = scale >= 1 ? r0 + 1 : whole(ceil(yb));
        if (r0 < 0) r0 = 0;
        if (r1 > pg->bh) r1 = pg->bh;
        Span by, fy;
        if (bg) span(&by, ya, yb, bg->oy, bg->r, bg->h);
        if (fg) span(&fy, ya, yb, fg->oy, fg->r, fg->h);
        for (int i = 0; i < w; i++) {
            // the mask's share of the pixel: the foreground's colour over the background's
            unsigned n = 0, area = pg->bits && r1 > r0 && c1[i] > c0[i] ? (unsigned)(r1 - r0) * (unsigned)(c1[i] - c0[i]) : 0;
            if (area == 1) n = pg->bits[(size_t)r0 * pg->rowbytes + (size_t)(c0[i] >> 3)] >> (7 - (c0[i] & 7)) & 1;
            else
                for (int r = r0; area && r < r1; r++) n += black(pg->bits + (size_t)r * pg->rowbytes, c0[i], c1[i]);
            uint8_t b[3] = {255, 255, 255}, f[3] = {0, 0, 0};
            if (bg && n < area + !area) layer_rgb(bg, &bx[i], &by, b);
            if (n) {
                if (pg->entry) {
                    unsigned sum[3] = {0, 0, 0};
                    palette_sum(pg, r0, r1, c0[i], c1[i], sum);
                    for (int k = 0; k < 3; k++) f[k] = (uint8_t)((sum[k] + n / 2) / n);
                } else if (pg->pal)
                    memcpy(f, pg->pal, 3);
                else if (fg)
                    layer_rgb(fg, &fx[i], &fy, f);
                for (int k = 0; k < 3; k++) b[k] = (uint8_t)((b[k] * (area - n) + f[k] * n + area / 2) / area);
            }
            o[i] = 0xff000000u | (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2];
        }
    }
    free(c0), free(bx);
    page_release(d, pg);
    return rc;
}
