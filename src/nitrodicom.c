// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// nitrodicom: see nitrodicom.h
#include "nitrodicom.h"
#include "nitro_convert.h"   // nc_jpeg_to_bgra
#include "nitro_num.h"       // nn_strtod: decimal strings read the same in every locale
#include "nitro_os.h"        // parallel loop (a thread pool)
#include "nitrodng.h"        // the lossless JPEG decoder
#include "nitrojpeg.h"
#include "nitrojpx.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { S_RAW, S_RLE, S_JPEG, S_LJPEG, S_J2K };                      // info.syntax
enum { P_MONO1, P_MONO2, P_PALETTE, P_RGB, P_YBR, P_YBR422 };       // info.photometric
#define MAX_DEPTH 24      // sequences in sequences
#define MAX_SIDE 65535    // Rows and Columns are 16-bit numbers

static inline uint32_t rd16(const uint8_t *p, int be) { return be ? (uint32_t)p[0] << 8 | p[1] : (uint32_t)p[1] << 8 | p[0]; }
static inline uint32_t rd32(const uint8_t *p, int be) {
    return be ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
              : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

// ---------------------------------------------------------------------------
// the data set (PS3.5 7): elements of (group, element) tag, [VR], length, value; a sequence's value
// is items of elements again. Only the elements the picture needs are looked at: those of the top
// level, a multi-frame picture's shared (or first frame's) functional groups, the first VOI table.

enum { C_TOP, C_GROUPS, C_GROUP, C_VOI, C_SKIP };   // where the walk is

typedef struct {
    const uint8_t *d;
    size_t n;
    ndcm_info *in;
    int meta;            // in the file meta group (always explicit VR, little endian)
    char ts[32];         // the transfer syntax UID
    int ts_known;
    int rescale_top, window_top;   // from the top level (those of a functional group do not replace them)
    int float_pixels;
} Parse;

static int is_vr(const uint8_t *p) { return p[0] >= 'A' && p[0] <= 'Z' && p[1] >= 'A' && p[1] <= 'Z'; }
static int long_vr(uint32_t vr) {   // the VRs with a 32-bit length after two reserved bytes
    static const char *const L[] = {"OB", "OW", "OF", "SQ", "UT", "UN", "OD", "OL", "UC", "UR", "OV", "SV", "UV"};
    for (size_t i = 0; i < sizeof L / sizeof *L; i++)
        if (vr == ((uint32_t)L[i][0] << 8 | (uint32_t)L[i][1])) return 1;
    return 0;
}

// a decimal or integer string's first value ("40\400": 40)
static double text_number(const uint8_t *v, uint32_t len, double otherwise) {
    char b[40];
    size_t k = 0;
    for (uint32_t i = 0; i < len && v[i] != '\\' && k < sizeof b - 1; i++) b[k++] = (char)v[i];
    b[k] = 0;
    char *e = NULL;
    double x = nn_strtod(b, &e);
    return e == b || !isfinite(x) ? otherwise : x;
}

static void descriptor(ndcm_lut *l, const uint8_t *v, uint32_t len, int be) {
    if (len < 6) return;
    l->n = (int)rd16(v, be);
    if (!l->n) l->n = 65536;
    l->first = (int)rd16(v + 2, be);   // (of a signed picture: read again as signed, see lut_first)
    l->bits = (int)rd16(v + 4, be);
}

static void element(Parse *P, int ctx, uint32_t tag, uint32_t vr, size_t val, uint32_t len, int be) {
    ndcm_info *in = P->in;
    const uint8_t *v = P->d + val;
    int us = len >= 2 ? (int)rd16(v, be) : 0;
    if (ctx == C_GROUP) {
        if (tag == 0x00281052 && !P->rescale_top) in->intercept = text_number(v, len, 0);
        if (tag == 0x00281053 && !P->rescale_top) in->slope = text_number(v, len, 1);
        if (tag == 0x00281050 && !P->window_top) in->center = text_number(v, len, 0);
        if (tag == 0x00281051 && !P->window_top) in->width_w = text_number(v, len, 0), in->has_window = 1;
        return;
    }
    if (ctx == C_VOI) {
        if (tag == 0x00283002) descriptor(&in->voi, v, len, be);
        if (tag == 0x00283006) in->voi.pos = val, in->voi.len = len;
        return;
    }
    if (ctx != C_TOP) return;
    switch (tag) {
    case 0x00020010: {
        size_t k = len < sizeof P->ts - 1 ? len : sizeof P->ts - 1;
        memcpy(P->ts, v, k);
        while (k && (P->ts[k - 1] == 0 || P->ts[k - 1] == ' ')) k--;
        P->ts[k] = 0;
        P->ts_known = 1;
        break;
    }
    case 0x00280002: in->spp = us; break;
    case 0x00280004: {
        char s[20] = {0};
        size_t k = len < sizeof s - 1 ? len : sizeof s - 1;
        memcpy(s, v, k);
        while (k && (s[k - 1] == 0 || s[k - 1] == ' ')) s[--k] = 0;
        in->photometric = !strcmp(s, "MONOCHROME1") ? P_MONO1 : !strncmp(s, "PALETTE", 7) ? P_PALETTE : !strcmp(s, "RGB") ? P_RGB
                        : !strcmp(s, "YBR_FULL") ? P_YBR : !strcmp(s, "YBR_FULL_422") || !strcmp(s, "YBR_PARTIAL_422") ? P_YBR422
                        : !strncmp(s, "YBR_", 4) ? P_RGB   // (YBR_RCT / YBR_ICT / YBR_PARTIAL_420: the codec's own transform)
                        : !strncmp(s, "MONOCHROME", 10) ? P_MONO2 : -1;
        break;
    }
    case 0x00280006: in->planar = us == 1; break;
    case 0x00280008: in->frames = (int)text_number(v, len, 1); break;
    case 0x00280010: in->height = us; break;
    case 0x00280011: in->width = us; break;
    case 0x00280100: in->alloc = us; break;
    case 0x00280101: in->stored = us; break;
    case 0x00280102: in->high = us; break;
    case 0x00280103: in->is_signed = us == 1; break;
    case 0x00280120: if (len >= 2) in->padding = us, in->has_padding = 1; break;
    case 0x00281050: in->center = text_number(v, len, 0); P->window_top = 1; break;
    case 0x00281051: in->width_w = text_number(v, len, 0), in->has_window = 1; P->window_top = 1; break;
    case 0x00281052: in->intercept = text_number(v, len, 0); P->rescale_top = 1; break;
    case 0x00281053: in->slope = text_number(v, len, 1); P->rescale_top = 1; break;
    case 0x00281101: case 0x00281102: case 0x00281103: descriptor(&in->palette[tag - 0x00281101], v, len, be); break;
    case 0x00281201: case 0x00281202: case 0x00281203:
        in->palette[tag - 0x00281201].pos = val;
        in->palette[tag - 0x00281201].len = len;
        break;
    case 0x7FE00008: case 0x7FE00009: P->float_pixels = 1; break;
    case 0x7FE00010:
        in->pixel_pos = val;
        in->pixel_len = len;
        in->encapsulated = 0;
        in->pixel_words = vr == ('O' << 8 | 'W');
        break;
    }
}

// the context of a sequence's first item and of its other items
static void child(int ctx, uint32_t tag, int *first, int *rest) {
    *first = *rest = C_SKIP;
    if (ctx == C_TOP && tag == 0x52009229) *first = *rest = C_GROUPS;   // shared functional groups
    if (ctx == C_TOP && tag == 0x52009230) *first = C_GROUPS;           // per-frame ones: the first frame's
    if (ctx == C_TOP && tag == 0x00283010) *first = C_VOI;              // the VOI lookup tables: the first
    if (ctx == C_GROUPS && (tag == 0x00289145 || tag == 0x00289132)) *first = C_GROUP;   // pixel value transformation, frame VOI LUT
}

static size_t walk(Parse *P, size_t pos, size_t end, int ex, int be, int ctx, int depth, int in_item);

// a sequence's items from pos; undefined: up to its delimiter, else to end. Returns where it ends.
static size_t sequence(Parse *P, size_t pos, size_t end, int ex, int be, int first, int rest, int depth, int undefined) {
    if (depth >= MAX_DEPTH) return end;
    for (int item = 0; pos + 8 <= end; item++) {
        const uint8_t *p = P->d + pos;
        uint32_t tag = rd16(p, be) << 16 | rd16(p + 2, be), len = rd32(p + 4, be);
        if (tag == 0xFFFEE0DD) return pos + 8;
        if (tag != 0xFFFEE000) return undefined ? end : end;   // (not an item: damaged)
        int ctx = item ? rest : first;
        if (len == 0xFFFFFFFF) {
            pos = walk(P, pos + 8, end, ex, be, ctx, depth + 1, 1);
        } else {
            if (len > end - pos - 8) return end;
            if (ctx != C_SKIP) walk(P, pos + 8, pos + 8 + len, ex, be, ctx, depth + 1, 0);
            pos += 8 + (size_t)len;
        }
    }
    return end;
}

// The elements from pos to end; in_item: up to the item's delimiter. Returns where it stopped.
static size_t walk(Parse *P, size_t pos, size_t end, int ex, int be, int ctx, int depth, int in_item) {
    while (pos + 8 <= end) {
        const uint8_t *p = P->d + pos;
        if (P->meta && depth == 0 && rd16(p, 0) != 2) {   // the meta group is over: the data set, in its transfer syntax
            P->meta = 0;
            ex = strcmp(P->ts, "1.2.840.10008.1.2") != 0;
            be = !strcmp(P->ts, "1.2.840.10008.1.2.2");
            if (!strcmp(P->ts, "1.2.840.10008.1.2.1.99")) return pos;   // (deflated from here on)
            if (!P->ts_known || (!ex && is_vr(p + 4) && rd32(p + 4, 0) > end - pos)) {   // none given (or a wrong one): as it looks
                be = p[0] == 0 && p[1] != 0;
                ex = is_vr(p + 4);
            }
            P->in->big_endian = be;
        }
        int mex = P->meta && depth == 0 ? 1 : ex, mbe = P->meta && depth == 0 ? 0 : be;
        uint32_t tag = rd16(p, mbe) << 16 | rd16(p + 2, mbe), vr = 0, len;
        size_t val;
        if (tag >> 16 == 0xFFFE) {   // an item's or a sequence's delimiter
            if (in_item && tag == 0xFFFEE00D) return pos + 8;
            if (depth) return pos;   // (the sequence's: sequence() takes it)
            pos += 8;                // (a stray one at the top: passed over)
            continue;
        }
        if (mex && is_vr(p + 4)) {
            vr = (uint32_t)p[4] << 8 | p[5];
            if (long_vr(vr)) {
                if (pos + 12 > end) return end;
                len = rd32(p + 8, mbe);
                val = pos + 12;
            } else {
                len = rd16(p + 6, mbe);
                val = pos + 8;
            }
        } else {   // implicit VR (or an element without one in an explicit file)
            len = rd32(p + 4, mbe);
            val = pos + 8;
        }
        if (len == 0xFFFFFFFF) {
            if (tag == 0x7FE00010 && depth == 0) {   // encapsulated pixel data: its items are the offset table and the fragments
                P->in->pixel_pos = val;
                P->in->pixel_len = P->n - val;
                P->in->encapsulated = 1;
                return end;
            }
            int first, rest;
            child(ctx, tag, &first, &rest);
            int un = vr == ('U' << 8 | 'N');   // (a sequence of unknown VR: its elements are implicit, little endian)
            pos = sequence(P, val, end, un ? 0 : ex, un ? 0 : be, first, rest, depth, 1);
            continue;
        }
        if (len > end - val) {   // cut short (the start of a file: the pixel data may be the rest)
            if (tag == 0x7FE00010 && depth == 0 && ctx == C_TOP) element(P, ctx, tag, vr, val, (uint32_t)(end - val), mbe);
            return end;
        }
        if (ctx != C_SKIP) {
            int first, rest;
            child(ctx, tag, &first, &rest);
            if (first != C_SKIP) sequence(P, val, val + len, ex, be, first, rest, depth, 0);
            else element(P, ctx, tag, vr, val, len, mbe);
        }
        pos = val + (size_t)len;
        if (tag == 0x7FE00010 && depth == 0) return pos;   // (nothing of the picture comes after it)
    }
    return end;
}

// a data set without the preamble and "DICM": its first element is of group 0002 .. 0008
static int bare(const uint8_t *d, size_t n) {
    if (n < 16) return 0;
    uint32_t g = rd16(d, 0), e = rd16(d + 2, 0);
    if (g >= 2 && g <= 8 && e <= 0x0100) return is_vr(d + 4) ? rd16(d + 6, 0) <= n : rd32(d + 4, 0) <= n - 8;
    return d[0] == 0 && d[1] == 8 && d[2] == 0 && is_vr(d + 4);   // (big endian, explicit)
}

static int jpeg_frame(const uint8_t *s, size_t n, int *sof, int *prec, int *ycc);

int ndcm_read_info(const uint8_t *d, size_t n, ndcm_info *in) {
    memset(in, 0, sizeof *in);
    in->frames = 1;
    in->slope = 1;
    in->photometric = -1;
    in->high = -1;
    Parse P = {.d = d, .n = n, .in = in};
    size_t pos = 0;
    if (n >= 140 && !memcmp(d + 128, "DICM", 4)) pos = 132, P.meta = 1;
    else if (bare(d, n)) P.meta = 1;   // (when it starts with the meta group; else left at once, the syntax guessed)
    else return -1;
    walk(&P, pos, n, 1, 0, C_TOP, 0, 0);
    if (!strcmp(P.ts, "1.2.840.10008.1.2.1.99")) {   // (nothing of the data set could be read: no size either)
        in->error = "DICOM: deflated data is not supported";
        return 0;
    }
    if (in->width <= 0 || in->height <= 0 || in->width > MAX_SIDE || in->height > MAX_SIDE) return -1;
    if (!in->pixel_pos && !P.float_pixels) {   // no pixel data seen: the start of a file may end before it
        if (!in->alloc) return -1;
        in->error = "DICOM: no pixel data";
    }

    const char *ts = P.ts;
    static const char R[] = "1.2.840.10008.1.2";
    in->syntax = S_RAW;
    if (!strncmp(ts, R, sizeof R - 1) && ts[sizeof R - 1] == '.') {
        const char *t = ts + sizeof R;
        if (!strcmp(t, "5")) in->syntax = S_RLE;
        else if (!strcmp(t, "4.50") || !strcmp(t, "4.51")) in->syntax = S_JPEG;
        else if (!strcmp(t, "4.57") || !strcmp(t, "4.70")) in->syntax = S_LJPEG;
        else if (!strcmp(t, "4.90") || !strcmp(t, "4.91") || !strcmp(t, "4.92") || !strcmp(t, "4.93")) in->syntax = S_J2K;
        else if (!strcmp(t, "4.80") || !strcmp(t, "4.81")) in->error = "DICOM: JPEG-LS is not supported";
        else if (!strncmp(t, "4.20", 4)) in->error = "DICOM: HTJ2K is not supported";
        else if (!strncmp(t, "4.11", 4)) in->error = "DICOM: JPEG XL is not supported";
        else if (!strcmp(t, "1.99") || !strncmp(t, "8.", 2)) in->error = "DICOM: deflated data is not supported";
        else if (!strncmp(t, "4.", 2)) in->error = "DICOM: this compression is not supported (video, JPEG process ...)";
    }
    if (in->frames < 1) in->frames = 1;
    if (in->spp < 1) in->spp = 1;
    if (in->photometric < 0) in->photometric = in->spp == 3 ? P_RGB : P_MONO2;
    if (in->spp == 1 && in->photometric > P_PALETTE) in->photometric = P_MONO2;
    if (in->spp == 3 && in->photometric <= P_PALETTE) in->photometric = P_RGB;
    if (in->syntax == S_J2K || in->syntax == S_LJPEG || in->syntax == S_JPEG) {   // (the codestream has the precision)
        if (in->alloc != 8 && in->alloc != 16) in->alloc = 16;
    }
    if (in->stored < 1 || in->stored > in->alloc) in->stored = in->alloc;
    if (in->high < in->stored - 1 || in->high >= in->alloc) in->high = in->stored - 1;
    if (in->error) return 0;
    if (in->syntax != S_RAW && !in->encapsulated && in->pixel_len >= 8 && in->pixel_pos + 8 <= n && rd32(d + in->pixel_pos, 0) == 0xE000FFFE)
        in->encapsulated = 1;   // (the fragments in an element of a defined length)
    if ((in->syntax == S_JPEG || in->syntax == S_LJPEG) && in->encapsulated) {   // which JPEG it is: its frame header
        size_t end = in->pixel_len < n - in->pixel_pos ? in->pixel_pos + in->pixel_len : n, p = in->pixel_pos;
        int sof = 0, prec = 0, ycc = 0;
        if (rd32(d + p, 0) == 0xE000FFFE && rd32(d + p + 4, 0) <= end - p - 8) {   // the offset table, then the first fragment
            p += 8 + (size_t)rd32(d + p + 4, 0);
            if (p + 8 <= end && rd32(d + p, 0) == 0xE000FFFE) {
                size_t sl = rd32(d + p + 4, 0);
                if (sl > end - p - 8) sl = end - p - 8;
                if (!jpeg_frame(d + p + 8, sl, &sof, &prec, &ycc)) {
                    if (sof == 0xC3) in->syntax = S_LJPEG;
                    else if (sof > 0xC2) in->error = "DICOM: this JPEG process is not supported (arithmetic, hierarchical)";
                    else if (prec != 8) in->error = "DICOM: 12-bit lossy JPEG is not supported";
                    else in->syntax = S_JPEG;
                }
            }
        }
        if (in->error) return 0;
    }
    if (P.float_pixels && !in->pixel_pos) in->error = "DICOM: floating point pixel data is not supported";
    else if (in->spp != 1 && in->spp != 3) in->error = "DICOM: unsupported samples per pixel";
    else if (in->alloc != 1 && in->alloc != 8 && in->alloc != 16 && in->alloc != 32) in->error = "DICOM: unsupported bits per sample";
    else if (in->alloc == 1 && (in->spp != 1 || in->syntax > S_RLE)) in->error = "DICOM: unsupported one bit picture";
    else if (in->encapsulated != (in->syntax != S_RAW)) in->error = "DICOM: the pixel data does not match the transfer syntax";
    else if (in->photometric == P_YBR422 && in->syntax == S_RAW && in->alloc != 8) in->error = "DICOM: unsupported YBR_FULL_422";
    else if (in->photometric == P_PALETTE) {
        for (int c = 0; c < 3; c++)
            if (!in->palette[c].pos || in->palette[c].n < 1 || in->palette[c].len > n - in->palette[c].pos)
                in->error = "DICOM: unsupported palette (segmented, or none)";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// a frame's bytes

// The frame's compressed data: the fragments of the encapsulated pixel data (PS3.5 A.4) that hold
// it, in *own (to free) when they are more than one.
static int fragments(const uint8_t *d, size_t n, const ndcm_info *in, int frame, const uint8_t **fp, size_t *fl, uint8_t **own) {
    size_t pos = in->pixel_pos, end = in->pixel_len < n - pos ? pos + in->pixel_len : n;
    if (pos + 8 > end || rd32(d + pos, 0) != 0xE000FFFE) return -1;   // the basic offset table (may be empty)
    uint32_t tl = rd32(d + pos + 4, 0);
    if (tl > end - pos - 8) return -1;
    const uint8_t *table = d + pos + 8;
    size_t entries = tl / 4, first = pos + 8 + tl;
    // the fragments: counted, then those of the frame
    size_t count = 0;
    for (size_t p = first; p + 8 <= end && rd32(d + p, 0) == 0xE000FFFE; count++) {
        uint32_t l = rd32(d + p + 4, 0);
        if (l > end - p - 8) l = (uint32_t)(end - p - 8);   // (cut short: what there is)
        p += 8 + (size_t)l;
    }
    if (!count) return -1;
    size_t from = 0, to = count;   // the frame's fragments: [from, to)
    size_t off0 = 0, off1 = 0;     // or by their offsets from `first` (the table's)
    int by_offset = 0, by_end = 0;
    if (in->frames > 1) {
        if (count == (size_t)in->frames) from = (size_t)frame, to = from + 1;
        else if (entries >= (size_t)in->frames) {
            by_offset = 1;
            off0 = rd32(table + 4 * (size_t)frame, 0);
            off1 = frame + 1 < in->frames ? rd32(table + 4 * ((size_t)frame + 1), 0) : (uint32_t)-1;
        } else by_end = 1;   // no table: a frame ends where a fragment ends with the codestream's end marker
    }
    size_t total = 0, k = 0, nf = 0;
    int at = 0;   // (by_end: the frame the walk is in)
    const uint8_t *one = NULL;
    for (int pass = 0; pass < 2; pass++) {
        uint8_t *o = NULL;
        if (pass) {
            if (!nf) return -1;
            if (nf == 1) {
                *fp = one;
                *fl = total;
                return 0;
            }
            o = *own = malloc(total + 64);
            if (!o) return -1;
            memset(o + total, 0, 64);
            *fp = o;
            *fl = total;
        }
        k = 0, at = 0;
        size_t w = 0;
        for (size_t p = first; k < count; k++) {
            uint32_t l = rd32(d + p + 4, 0);
            if (l > end - p - 8) l = (uint32_t)(end - p - 8);
            const uint8_t *f = d + p + 8;
            int mine = by_offset ? p - first >= off0 && p - first < off1 : by_end ? at == frame : k >= from && k < to;
            if (mine) {
                if (pass) memcpy(o + w, f, l), w += l;
                else total += l, nf++, one = f;
            }
            if (by_end) {   // FF D9 (EOI / EOC), maybe before a padding byte
                size_t e = l;
                if (e && !f[e - 1]) e--;
                if (e >= 2 && f[e - 2] == 0xFF && f[e - 1] == 0xD9) at++;
            }
            p += 8 + (size_t)l;
        }
    }
    return 0;
}

// What a frame is decoded into: samples as the file's uncompressed ones are laid out.
typedef struct {
    const uint8_t *p;
    size_t len;          // bytes at p
    uint8_t *own;        // to free
    njpx_image jpx;      // (its samples are p)
    int have_jpx;
    int bytes;           // of a sample: 1, 2, 4; 0: one bit each
    int be, swap8;       // big endian samples; bytes swapped in pairs
    int planar;          // the components' planes one after the other
    int spp;
    int stored, shift, is_signed;   // a sample's value: `stored` bits from bit `shift`
    int ycc, packed422;  // Y Cb Cr samples; as Y Y Cb Cr of two pixels
    size_t bit0;         // one bit each: the first one
    int done;            // the picture is in the output already (a lossy JPEG in colour)
} Frame;

static void frame_free(Frame *f) {
    if (f->have_jpx) njpx_free(&f->jpx);
    free(f->own);
}

static inline int host_be(void) {
    const uint16_t one = 1;
    return !*(const uint8_t *)&one;
}

// RLE (PS3.5 G): a header of 16 numbers (the segments' count and offsets), a segment for every
// byte of a sample (the most significant first, of each component in turn), PackBits coded.
static int rle(const uint8_t *s, size_t sl, const ndcm_info *in, Frame *f) {
    size_t px = (size_t)in->width * in->height, plane = in->alloc == 1 ? (px + 7) / 8 : px;
    int bytes = in->alloc == 1 ? 1 : in->alloc / 8, segs = in->spp * bytes;
    if (sl < 64 || (int)rd32(s, 0) < segs || segs > 15) return -1;
    uint8_t *out = calloc(plane * (size_t)segs + 64, 1);
    if (!out) return -1;
    for (int k = 0; k < segs; k++) {
        size_t a = rd32(s + 4 + 4 * k, 0), b = k + 1 < (int)rd32(s, 0) ? rd32(s + 8 + 4 * k, 0) : sl;
        if (b > sl) b = sl;
        if (a < 64 || a > b) continue;   // (a damaged one: the plane stays zero)
        int c = k / bytes, j = k % bytes;   // component, its byte (0: the most significant)
        uint8_t *o = out + (size_t)c * plane * bytes + (size_t)(bytes - 1 - j);   // little endian samples
        size_t w = 0;
        for (size_t i = a; i < b && w < plane;) {
            int ctl = (int8_t)s[i++];
            if (ctl >= 0) {
                for (int m = 0; m <= ctl && i < b && w < plane; m++) o[bytes * w++] = s[i++];
            } else if (ctl != -128 && i < b) {
                uint8_t v = s[i++];
                for (int m = 0; m <= -ctl && w < plane; m++) o[bytes * w++] = v;
            }
        }
    }
    f->p = f->own = out;
    f->len = plane * (size_t)segs;
    f->bytes = in->alloc == 1 ? 0 : bytes;
    f->planar = 1;
    return 0;
}

// A JPEG stream's frame header: *sof its marker (0xC0 ...), *prec the samples' bits; 0 if it has one.
// *ycc: the stream itself says its three components are Y Cb Cr (a JFIF marker, components of the
// ids 1 2 3, or the last two with fewer samples), whatever the data set says.
static int jpeg_frame(const uint8_t *s, size_t n, int *sof, int *prec, int *ycc) {
    int jfif = 0;
    if (n < 4 || s[0] != 0xFF || s[1] != 0xD8) return -1;
    for (size_t p = 2; p + 4 <= n;) {
        if (s[p] != 0xFF) return -1;
        int m = s[p + 1];
        if (m == 0xFF) {
            p++;
            continue;
        }
        size_t l = (size_t)s[p + 2] << 8 | s[p + 3];
        if (m == 0xDA || m == 0xD9 || l < 2 || p + 2 + l > n) return -1;
        const uint8_t *seg = s + p + 4;
        if (m == 0xE0 && l >= 7 && !memcmp(seg, "JFIF", 4)) jfif = 1;
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (l < 8) return -1;
            *sof = m;
            *prec = seg[0];
            *ycc = jfif;
            if (seg[5] == 3 && l >= 8 + 9) {
                if (seg[6] == 1 && seg[9] == 2 && seg[12] == 3) *ycc = 1;
                if (seg[7] != seg[10] || seg[7] != seg[13]) *ycc = 1;
            }
            return 0;
        }
        p += 2 + l;
    }
    return -1;
}

// The frame decoded. bgrx (may be NULL: then never written): where a lossy JPEG in colour goes.
static int frame_get(const uint8_t *d, size_t n, const ndcm_info *in, int frame, Frame *f, uint8_t *bgrx, size_t pitch, int nthreads) {
    memset(f, 0, sizeof *f);
    if (in->error || frame < 0 || frame >= in->frames || in->pixel_pos > n) return -1;
    size_t px = (size_t)in->width * in->height;
    f->spp = in->spp;
    f->stored = in->stored;
    f->shift = in->high + 1 - in->stored;
    f->is_signed = in->is_signed;
    // (YBR_FULL_422 of a lossless codec's picture cannot be: its components come in full; such files hold R G B,
    // the name is left from the lossy JPEG they were made of)
    f->ycc = in->photometric == P_YBR || (in->photometric == P_YBR422 && in->syntax == S_RAW);
    f->planar = in->planar && in->spp > 1;
    if (in->syntax == S_RAW) {
        size_t avail = in->pixel_len < n - in->pixel_pos ? in->pixel_len : n - in->pixel_pos, need, at;
        if (in->alloc == 1) {   // the frames' bits follow each other without padding
            f->bit0 = px * (size_t)frame;
            at = 0;
            need = (f->bit0 + px + 7) / 8;
        } else {
            f->packed422 = in->photometric == P_YBR422;
            need = f->packed422 ? px * 2 : px * (size_t)in->spp * (size_t)(in->alloc / 8);
            at = need * (size_t)frame;
            if (f->packed422 && (in->width & 1)) return -1;
        }
        if (at > avail || need > avail - at) return -1;
        f->p = d + in->pixel_pos + at;
        f->len = need;
        f->bytes = in->alloc / 8;
        f->be = in->big_endian;
        f->swap8 = in->big_endian && in->pixel_words && in->alloc == 8;
        if (f->packed422) f->planar = 0;
        return 0;
    }
    const uint8_t *s = NULL;
    size_t sl = 0;
    if (fragments(d, n, in, frame, &s, &sl, &f->own)) return -1;
    uint8_t *held = f->own;   // (the compressed data, when it was put together)
    f->own = NULL;
    if (in->syntax != S_RLE) {   // a fragment is padded to an even length: zeros after the stream's end marker (FF D9) are not its
        size_t e = sl;
        while (e > 2 && !s[e - 1]) e--;
        if (e >= 2 && s[e - 2] == 0xFF && s[e - 1] == 0xD9) sl = e;
    }
    int rc = -1;
    if (in->syntax == S_RLE) {
        rc = rle(s, sl, in, f);
    } else if (in->syntax == S_LJPEG) {
        int w, h, nc, prec;
        if (!ndng_ljpeg_info(s, sl, &w, &h, &nc, &prec) && w == in->width && h == in->height && nc == in->spp) {
            uint16_t *out = malloc(px * (size_t)nc * 2 + 64);
            if (out && !ndng_ljpeg_decode(s, sl, out, w * nc, h, (size_t)w * nc, nthreads != 1)) {
                f->p = f->own = (uint8_t *)out;
                f->len = px * (size_t)nc * 2;
                f->bytes = 2;
                f->be = host_be();
                f->planar = 0;
                if (prec < f->stored + f->shift) f->stored = prec, f->shift = 0;   // (fewer bits than the data set says)
                rc = 0;
            } else free(out);
        }
    } else if (in->syntax == S_J2K) {
        int jw = 0, jh = 0;   // (its size first: a damaged one is not decoded at whatever size it says)
        if (!njpx_size(s, sl, &jw, &jh) && jw == in->width && jh == in->height &&
            !njpx_decode(s, sl, NJPX_RAW_COLORS | NJPX_IMAGE | (nthreads == 1 ? NJPX_SINGLE_THREAD : 0), &f->jpx)) {
            f->have_jpx = 1;
            if (f->jpx.width == in->width && f->jpx.height == in->height && f->jpx.ncomp == in->spp) {
                // the codestream says how many bits and whether signed (SIZ: Ssiz of the first component);
                // nitrojpx gives signed samples moved up by half their range: back to two's complement
                size_t siz = 0;
                while (siz + 44 < sl && siz < 4096 && !(s[siz] == 0xFF && s[siz + 1] == 0x4F && s[siz + 2] == 0xFF && s[siz + 3] == 0x51)) siz++;
                int sgn = siz + 44 < sl && siz < 4096 ? s[siz + 42] >> 7 : in->is_signed, prec = f->jpx.precision[0];
                if (sgn && prec >= 1 && prec <= 16) {
                    uint16_t flip = (uint16_t)(1u << (prec - 1));
                    for (size_t i = 0; i < px * (size_t)in->spp; i++) f->jpx.samples[i] ^= flip;
                }
                f->p = (const uint8_t *)f->jpx.samples;
                f->len = px * (size_t)in->spp * 2;
                f->bytes = 2;
                f->be = host_be();
                f->planar = 0;
                // (signed when either says so: a codestream or a data set that says unsigned of a CT's values is wrong)
                f->stored = prec, f->shift = 0, f->is_signed = sgn || in->is_signed;
                rc = 0;
            }
        }
    } else if (in->syntax == S_JPEG) {
        nj_info fi;
        if (!nj_read_info(s, sl, &fi) && (fi.supported || fi.rgb) && fi.width == in->width && fi.height == in->height &&
            (fi.ncomp == 1 || fi.ncomp == 3) && fi.ncomp == in->spp) {
            size_t size[3] = {0}, total = 0, pitches[4] = {0};
            for (int c = 0; c < fi.ncomp; c++) size[c] = (size_t)fi.plane_w[c] * fi.plane_h[c], total += size[c], pitches[c] = (size_t)fi.plane_w[c];
            uint8_t *buf = malloc(total + 64), *planes[4] = {buf, buf + size[0], buf + size[0] + size[1], NULL};
            nj_stats st;
            if (buf && !nj_decode_planes(s, sl, &fi, planes, pitches, nthreads == 1 ? 1 : 0, &st)) {
                if (fi.ncomp == 1) {   // the rows moved together: samples of 8 bits
                    for (int y = 1; y < in->height; y++) memmove(buf + (size_t)y * in->width, buf + (size_t)y * fi.plane_w[0], (size_t)in->width);
                    f->p = f->own = buf;
                    f->len = px;
                    f->bytes = 1;
                    f->stored = 8, f->shift = 0, f->is_signed = 0;
                    buf = NULL;
                } else {
                    // R G B components as they are when the stream says so (Adobe's marker, the ids R G B), or the
                    // data set says RGB and the stream does not say Y Cb Cr (some say RGB of what they wrote as Y Cb Cr)
                    int sof = 0, prec = 0, ycc = 0;
                    jpeg_frame(s, sl, &sof, &prec, &ycc);
                    fi.adobe = fi.rgb || (in->photometric == P_RGB && fi.adobe != 1 && !ycc) ? 0 : 1;
                    if (bgrx) nc_jpeg_to_bgra(&fi, planes, pitches, 0, NULL, bgrx, pitch);
                    f->done = 1;
                }
                rc = 0;
            }
            free(buf);
        }
    }
    free(held);
    if (rc) frame_free(f), memset(f, 0, sizeof *f);
    return rc;
}

// ---------------------------------------------------------------------------
// the samples of a row, and what they show

// Row y's samples as they are stored: width * spp numbers, a pixel's one after the other.
static void row_samples(const Frame *f, int w, int h, int y, uint32_t *out) {
    const uint8_t *p = f->p;
    if (f->packed422) {   // Y0 Y1 Cb Cr: two pixels
        const uint8_t *r = p + (size_t)y * w * 2;
        for (int x = 0; x < w; x++) {
            const uint8_t *q = r + (size_t)(x >> 1) * 4;
            out[3 * x] = q[x & 1], out[3 * x + 1] = q[2], out[3 * x + 2] = q[3];
        }
        return;
    }
    for (int c = 0; c < f->spp; c++) {
        size_t i = f->planar ? ((size_t)c * h + (size_t)y) * w : (size_t)y * w * f->spp + (size_t)c, step = f->planar ? 1 : (size_t)f->spp;
        uint32_t *o = out + c;
        switch (f->bytes) {
        case 0:
            for (int x = 0; x < w; x++) {
                size_t b = f->bit0 + (size_t)y * w + (size_t)x;
                o[(size_t)x * f->spp] = p[b >> 3] >> (b & 7) & 1;
            }
            break;
        case 1:
            for (int x = 0; x < w; x++, i += step) o[(size_t)x * f->spp] = p[i ^ (size_t)f->swap8];
            break;
        case 2:
            for (int x = 0; x < w; x++, i += step) o[(size_t)x * f->spp] = rd16(p + 2 * i, f->be);
            break;
        default:
            for (int x = 0; x < w; x++, i += step) o[(size_t)x * f->spp] = rd32(p + 4 * i, f->be);
        }
    }
}

static inline uint32_t field_mask(int stored) { return stored >= 32 ? 0xFFFFFFFFu : (1u << stored) - 1; }
// a sample's bits as a number
static inline int64_t value_of(uint32_t bits, int stored, int is_signed) {
    if (is_signed && stored < 32 && bits >> (stored - 1) & 1) return (int64_t)bits - ((int64_t)1 << stored);
    if (is_signed && stored >= 32) return (int32_t)bits;
    return bits;
}

static inline uint8_t clamp8(double v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5); }

// a lookup table's entry k (of 8 bits each when its data is that short, else 16-bit words)
static inline uint32_t lut_entry(const uint8_t *d, const ndcm_lut *l, int be, int k) {
    return l->len < 2 * (size_t)l->n ? d[l->pos + (size_t)k] : rd16(d + l->pos + 2 * (size_t)k, be);
}
static int lut_entries(const ndcm_lut *l) {   // those its data has
    size_t have = l->len < 2 * (size_t)l->n ? l->len : l->len / 2;
    return have < (size_t)l->n ? (int)have : l->n;
}
static int lut_first(const ndcm_lut *l, int is_signed) { return is_signed ? (int16_t)l->first : l->first; }

// How a grey value becomes a shade: through the VOI table (v), or a * (value * slope + intercept) + b.
typedef struct {
    double slope, intercept, a, b, gain;
    const uint8_t *d;       // the file (the table's data)
    const ndcm_lut *voi;    // NULL: the line
    int voi_n, voi_first, be, invert;
    double voi_lo, voi_scale;
} Shade;

static inline uint8_t shade(const Shade *s, int64_t v) {
    double m = (double)v * s->slope + s->intercept, y;
    if (s->voi) {
        double k = m - s->voi_first;
        int i = k <= 0 ? 0 : k >= s->voi_n - 1 ? s->voi_n - 1 : (int)k;
        y = (lut_entry(s->d, s->voi, s->be, i) - s->voi_lo) * s->voi_scale;
    } else y = s->a * m + s->b;
    if (s->invert) y = 255 - y;
    return clamp8(y * s->gain);   // (the exposure correction: before the cut to black .. white)
}

static void ycc_rgb(uint32_t y, uint32_t cb, uint32_t cr, uint8_t *bgr) {
    double Y = y, B = (double)cb - 128, R = (double)cr - 128;
    bgr[0] = clamp8(Y + 1.772 * B);
    bgr[1] = clamp8(Y - 0.344136 * B - 0.714136 * R);
    bgr[2] = clamp8(Y + 1.402 * R);
}

// Does the file's window show anything of the frame? Not when every sample (of rows all over it, the padding
// left out) lies above the window: the picture would be all white. There are files whose window was written
// for other numbers than their samples are (X-rays whose samples start at 32768). All below it is left alone:
// a slice of a CT series with nothing but air in it is rightly black.
static int window_fits(const ndcm_info *in, const Frame *f, double slope, double intercept) {
    int w = in->width, h = in->height, step = h > 64 ? h / 64 : 1;
    double lo = in->center - 0.5 - (in->width_w - 1) / 2, hi = in->center - 0.5 + (in->width_w - 1) / 2;
    uint32_t mask = field_mask(f->stored), pad = (uint32_t)in->padding & mask, *row = malloc((size_t)w * sizeof *row);
    if (!row) return 1;
    size_t below = 0, above = 0, inside = 0;
    for (int y = step / 2; y < h && !inside; y += step) {
        row_samples(f, w, h, y, row);
        for (int x = 0; x < w; x++) {
            uint32_t bits = row[x] >> f->shift & mask;
            if (in->has_padding && bits == pad) continue;
            double m = (double)value_of(bits, f->stored, f->is_signed) * slope + intercept;
            if (m <= lo) below++;
            else if (m > hi) above++;
            else inside++;
        }
    }
    free(row);
    return inside || below || !above;
}

// The decoded frame shown: BGRX rows.
static int show(const uint8_t *d, const ndcm_info *in, const Frame *f, uint8_t *bgrx, size_t pitch, int nthreads) {
    int w = in->width, h = in->height, spp = f->spp, stored = f->stored, sgn = f->is_signed;
    uint32_t mask = field_mask(stored);
    uint8_t *lut = NULL;      // grey: a shade for every sample value (of 16 bits at most)
    uint32_t *plut = NULL;    // palette: a colour for every sample value
    Shade S = {.slope = in->slope, .intercept = in->intercept, .d = d, .be = in->big_endian, .invert = in->photometric == P_MONO1};
    if (!isfinite(S.slope) || !isfinite(S.intercept) || S.slope == 0) S.slope = 1, S.intercept = 0;
    S.gain = isfinite(in->ev) ? pow(2.0, in->ev) : 1;
    int grey = spp == 1 && in->photometric != P_PALETTE;
    if (grey) {
        if (in->voi.pos && in->voi.n > 0 && lut_entries(&in->voi) > 0) {   // the file's VOI table: its entries spread over black .. white
            S.voi = &in->voi;
            S.voi_n = lut_entries(&in->voi);
            S.voi_first = lut_first(&in->voi, sgn);
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (int k = 0; k < S.voi_n; k++) {
                uint32_t e = lut_entry(d, &in->voi, in->big_endian, k);
                if (e < lo) lo = e;
                if (e > hi) hi = e;
            }
            S.voi_lo = lo;
            S.voi_scale = hi > lo ? 255.0 / (hi - lo) : 0;
        } else if (in->has_window && isfinite(in->center) && isfinite(in->width_w) && in->width_w >= 1 &&
                   window_fits(in, f, S.slope, S.intercept)) {   // PS3.3 C.11.2.1.2
            double c = in->center - 0.5, ww = in->width_w - 1;
            if (ww > 0) S.a = 255.0 / ww, S.b = (0.5 - c / ww) * 255.0;
            else S.a = 1e30, S.b = -c * 1e30;   // (a width of 1: black up to the centre, white above)
        } else if (stored <= 8) {   // an 8-bit picture as it is
            double lo = sgn ? -(double)(1 << (stored - 1)) : 0, top = (double)mask;
            S.a = top > 0 ? 255.0 / (top * fabs(S.slope)) : 0;
            S.b = -(S.slope > 0 ? lo * S.slope + S.intercept : (lo + top) * S.slope + S.intercept) * S.a;
        } else {   // from the smallest to the largest value of the frame (those of the padding left out)
            int64_t lo = INT64_MAX, hi = INT64_MIN;
            uint32_t pad = (uint32_t)in->padding & mask, *row = malloc((size_t)w * sizeof *row);
            if (!row) return -1;
            for (int y = 0; y < h; y++) {
                row_samples(f, w, h, y, row);
                for (int x = 0; x < w; x++) {
                    uint32_t bits = row[x] >> f->shift & mask;
                    if (in->has_padding && bits == pad) continue;
                    int64_t v = value_of(bits, stored, sgn);
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
            }
            free(row);
            if (lo > hi) lo = hi = 0;
            double m0 = (double)lo * S.slope + S.intercept, m1 = (double)hi * S.slope + S.intercept;
            if (m0 > m1) { double t = m0; m0 = m1; m1 = t; }
            S.a = m1 > m0 ? 255.0 / (m1 - m0) : 0;
            S.b = -m0 * S.a;
        }
        if (stored <= 16) {
            lut = malloc((size_t)mask + 1);
            if (!lut) return -1;
            for (uint32_t b = 0; b <= mask; b++) lut[b] = shade(&S, value_of(b, stored, sgn));
        }
    } else if (in->photometric == P_PALETTE) {
        if (stored > 16) return -1;
        plut = malloc(((size_t)mask + 1) * sizeof *plut);
        if (!plut) return -1;
        int n[3], first[3], wide = 0;   // wide: the entries are of 16 bits (some say 8 and are not)
        for (int c = 0; c < 3; c++) {
            n[c] = lut_entries(&in->palette[c]);
            first[c] = lut_first(&in->palette[c], sgn);
            for (int k = 0; k < n[c]; k++) wide |= lut_entry(d, &in->palette[c], in->big_endian, k) > 255;
        }
        for (uint32_t b = 0; b <= mask; b++) {
            uint32_t px = 0xFF000000u;
            int64_t v = value_of(b, stored, sgn);
            for (int c = 0; c < 3; c++) {   // (c 0: red, bits 16..23 of B G R X as a little endian number)
                int64_t k = v - first[c];
                uint32_t e = n[c] ? lut_entry(d, &in->palette[c], in->big_endian, k <= 0 ? 0 : k >= n[c] ? n[c] - 1 : (int)k) : 0;
                px |= (wide ? e >> 8 : e & 255) << (16 - 8 * c);
            }
            plut[b] = px;
        }
    }
    int bands = h < 64 ? 1 : h / 32 < 64 ? h / 32 : 64, rgb_shift = stored > 8 ? stored - 8 : 0;
    uint8_t gain[256];   // the exposure correction of a colour picture's bytes
    for (int k = 0; k < 256; k++) gain[k] = clamp8(k * S.gain);
    const uint8_t *pg = gain;
    const Shade *ps = &S;
    atomic_int failed = 0, *pf = &failed;   // (a pointer: the block copies it; no block runtime needed)
    nitro_parallel_n((size_t)bands, nthreads, ^(size_t band) {
        uint32_t *row = malloc((size_t)w * (size_t)(f->packed422 ? 3 : spp) * sizeof *row);
        if (!row) {
            atomic_store(pf, 1);
            return;
        }
        for (int y = (int)((size_t)h * band / (size_t)bands), y1 = (int)((size_t)h * (band + 1) / (size_t)bands); y < y1; y++) {
            uint8_t *o = bgrx + (size_t)y * pitch;
            row_samples(f, w, h, y, row);
            if (lut) {
                for (int x = 0; x < w; x++) {
                    uint8_t g = lut[row[x] >> f->shift & mask];
                    o[4 * x] = o[4 * x + 1] = o[4 * x + 2] = g, o[4 * x + 3] = 255;
                }
            } else if (grey) {
                for (int x = 0; x < w; x++) {
                    uint8_t g = shade(ps, value_of(row[x] >> f->shift & mask, stored, sgn));
                    o[4 * x] = o[4 * x + 1] = o[4 * x + 2] = g, o[4 * x + 3] = 255;
                }
            } else if (plut) {
                for (int x = 0; x < w; x++) {
                    uint32_t px = plut[row[x] >> f->shift & mask];
                    o[4 * x] = pg[px & 255], o[4 * x + 1] = pg[px >> 8 & 255], o[4 * x + 2] = pg[px >> 16 & 255], o[4 * x + 3] = 255;
                }
            } else {
                for (int x = 0; x < w; x++) {
                    uint32_t a = (row[3 * x] >> f->shift & mask) >> rgb_shift, b = (row[3 * x + 1] >> f->shift & mask) >> rgb_shift,
                             c = (row[3 * x + 2] >> f->shift & mask) >> rgb_shift;
                    if (f->ycc) ycc_rgb(a, b, c, o + 4 * x);
                    else o[4 * x] = (uint8_t)c, o[4 * x + 1] = (uint8_t)b, o[4 * x + 2] = (uint8_t)a;
                    o[4 * x] = pg[o[4 * x]], o[4 * x + 1] = pg[o[4 * x + 1]], o[4 * x + 2] = pg[o[4 * x + 2]], o[4 * x + 3] = 255;
                }
            }
        }
        free(row);
    });
    free(lut);
    free(plut);
    return atomic_load(&failed) ? -1 : 0;
}

int ndcm_decode(const uint8_t *d, size_t n, const ndcm_info *in, int frame, uint8_t *bgrx, size_t pitch, int nthreads) {
    Frame f;
    if (frame_get(d, n, in, frame, &f, bgrx, pitch, nthreads)) return -1;
    int rc = f.done ? 0 : show(d, in, &f, bgrx, pitch, nthreads);
    if (f.done && in->ev != 0 && isfinite(in->ev)) {   // (a lossy JPEG in colour: decoded straight into the output)
        double g = pow(2.0, in->ev);
        uint8_t gain[256];
        for (int k = 0; k < 256; k++) gain[k] = clamp8(k * g);
        for (int y = 0; y < in->height; y++) {
            uint8_t *o = bgrx + (size_t)y * pitch;
            for (int x = 0; x < in->width; x++) o[4 * x] = gain[o[4 * x]], o[4 * x + 1] = gain[o[4 * x + 1]], o[4 * x + 2] = gain[o[4 * x + 2]];
        }
    }
    frame_free(&f);
    return rc;
}

int ndcm_values(const uint8_t *d, size_t n, const ndcm_info *in, int frame, int32_t *out, int *spp) {
    Frame f;
    int w = in->width, h = in->height;
    size_t px = (size_t)w * h;
    uint8_t *bgrx = in->syntax == S_JPEG && in->spp == 3 && !in->error ? malloc(px * 4) : NULL;
    if (frame_get(d, n, in, frame, &f, bgrx, (size_t)w * 4, 1)) {
        free(bgrx);
        return -1;
    }
    int rc = 0;
    *spp = f.done ? 3 : f.packed422 ? 3 : f.spp;
    if (f.done) {
        if (!bgrx) rc = -1;
        else for (size_t i = 0; i < px; i++) out[3 * i] = bgrx[4 * i + 2], out[3 * i + 1] = bgrx[4 * i + 1], out[3 * i + 2] = bgrx[4 * i];
    } else {
        uint32_t *row = malloc((size_t)w * (size_t)*spp * sizeof *row), mask = field_mask(f.stored);
        if (!row) rc = -1;
        else for (int y = 0; y < h; y++) {
            row_samples(&f, w, h, y, row);
            for (size_t i = 0; i < (size_t)w * (size_t)*spp; i++)
                out[(size_t)y * w * (size_t)*spp + i] = (int32_t)value_of(row[i] >> f.shift & mask, f.stored, f.is_signed);
        }
        free(row);
    }
    free(bgrx);
    frame_free(&f);
    return rc;
}
