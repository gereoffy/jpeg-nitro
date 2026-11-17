// SPDX-License-Identifier: MIT AND BSD-3-Clause
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrojxl: JPEG XL (ISO/IEC 18181) still images without dependencies.
//
// Parts are taken from two decoders:
// - J40 by Kang Seonghoon (public domain / MIT No Attribution; third_party/j40, not in this
//   repository): the prefix and ANS codes, the hybrid integers, LZ77, the DCTs and the AFV basis,
//   most of the quantisation weights;
// - libjxl (https://github.com/libjxl/libjxl, BSD-3-Clause, its license below): what J40 has no
//   code for or does differently - the ICC profile's reconstruction, the weighted predictor,
//   palette, squeeze, the HF contexts, the LF smoothing over the whole frame, EPF, the default
//   tables of the 64 x 64 and larger transforms, the colour conversions.
// The rest follows the specification. Written in the style of the other nitro decoders: one
// pass over a codestream held in memory, the groups decoded on nitro_os's worker pool.
//
// Copyright (c) the JPEG XL Project Authors.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include "nitrojxl.h"
#include "nitro_os.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STREAM_LIMIT ((size_t)512 << 20)
#define BOX_LIMIT 1000000u
#define TAG(a,b,c,d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (d))

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// ---------------------------------------------------------------------------
// container (ISO/IEC 18181-2)

typedef struct { const uint8_t *data; size_t len, next; uint32_t type; } Box;

static int box_read(const uint8_t *data, size_t len, size_t pos, Box *box) {
    if (pos > len || len - pos < 8) return NJXL_INVALID;
    size_t header = 8;
    uint64_t size = be32(data + pos);
    box->type = be32(data + pos + 4);
    if (size == 1) {
        if (len - pos < 16) return NJXL_INVALID;
        size = (uint64_t)be32(data + pos + 8) << 32 | be32(data + pos + 12);
        header = 16;
    } else if (!size) {
        size = len - pos;
    }
    if (size < header || size > len - pos) return NJXL_INVALID;
    box->data = data + pos + header;
    box->len = (size_t)size - header;
    box->next = pos + (size_t)size;
    return NJXL_OK;
}

static int codestream_magic(const uint8_t *data, size_t len) {
    return len >= 2 && data[0] == 0xff && data[1] == 0x0a;
}

int njxl_unwrap(const uint8_t *data, size_t len, njxl_stream *out) {
    static const uint8_t signature[12] = {0,0,0,12,'J','X','L',' ',13,10,135,10};
    if (!out) return NJXL_INVALID;
    memset(out, 0, sizeof(*out));
    if (!data) return NJXL_INVALID;
    if (codestream_magic(data, len)) {
        if (len > STREAM_LIMIT) return NJXL_LIMIT;
        out->data = data; out->len = len;
        return NJXL_OK;
    }
    if (len < sizeof signature || memcmp(data, signature, sizeof signature)) return NJXL_INVALID;

    // The file type box immediately follows the signature. Version 1's out-of-order
    // fragments are deliberately unsupported; the Lightroom exports use version 0.
    Box box;
    if (box_read(data, len, sizeof signature, &box) || box.type != TAG('f','t','y','p') ||
        box.len < 12 || (box.len & 3)) return NJXL_INVALID;
    if (be32(box.data) != TAG('j','x','l',' ') || be32(box.data + 4)) return NJXL_UNSUPPORTED;
    int compatible = 0;
    for (size_t i = 8; i < box.len; i += 4)
        compatible |= be32(box.data + i) == TAG('j','x','l',' ');
    if (!compatible) return NJXL_UNSUPPORTED;

    size_t start = box.next, pos = start, total = 0;
    const uint8_t *single = NULL;
    unsigned boxes = 2, parts = 0;
    int level = 5, have_level = 0, mode = 0, last = 0;
    while (pos < len) {
        if (++boxes > BOX_LIMIT) return NJXL_LIMIT;
        if (box_read(data, len, pos, &box)) return NJXL_INVALID;
        pos = box.next;
        switch (box.type) {
        case TAG('J','X','L',' '):
        case TAG('f','t','y','p'):
            return NJXL_INVALID;
        case TAG('j','x','l','l'):
            if (have_level || mode || box.len != 1) return NJXL_INVALID;
            level = box.data[0]; have_level = 1;
            if (level != 5 && level != 10) return NJXL_UNSUPPORTED;
            break;
        case TAG('j','x','l','c'):
            if (mode || !codestream_magic(box.data, box.len)) return NJXL_INVALID;
            if (box.len > STREAM_LIMIT) return NJXL_LIMIT;
            mode = 1; single = box.data; total = box.len;
            break;
        case TAG('j','x','l','p'): {
            if (mode == 1 || last || box.len < 4) return NJXL_INVALID;
            uint32_t index = be32(box.data);
            if ((index & 0x7fffffff) != parts) return NJXL_INVALID;
            if (box.len - 4 > STREAM_LIMIT - total) return NJXL_LIMIT;
            mode = 2; parts++; last = (int)(index >> 31);
            total += box.len - 4;
            break;
        }
        default:
            break;  // Metadata is not needed to locate or decode the image.
        }
    }
    if (!mode || (mode == 2 && (!last || total < 2))) return NJXL_INVALID;
    if (mode == 1) {
        out->data = single; out->len = total; out->level = level;
        return NJXL_OK;
    }

    // Validate everything before allocating. A second scan avoids a fragment table
    // and repeated reallocations, even with tiny / empty jxlp boxes.
    uint8_t *joined = malloc(total);
    if (!joined) return NJXL_NOMEM;
    size_t written = 0;
    for (pos = start; pos < len; pos = box.next) {
        if (box_read(data, len, pos, &box)) { free(joined); return NJXL_INVALID; }
        if (box.type == TAG('j','x','l','p')) {
            memcpy(joined + written, box.data + 4, box.len - 4);
            written += box.len - 4;
        }
    }
    if (!codestream_magic(joined, total)) { free(joined); return NJXL_INVALID; }
    out->data = out->storage = joined; out->len = total; out->level = level;
    return NJXL_OK;
}

void njxl_stream_free(njxl_stream *stream) {
    if (stream) { free(stream->storage); memset(stream, 0, sizeof(*stream)); }
}

// ---------------------------------------------------------------------------
// small helpers

#define ERR_INVALID NJXL_INVALID
#define ERR_UNSUP NJXL_UNSUPPORTED
#define ERR_NOMEM NJXL_NOMEM
#define ERR_LIMIT NJXL_LIMIT
#define TRY(x) do { int e_ = (x); if (e_) return e_; } while (0)

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int cdiv(int a, int b) { return (a + b - 1) / b; }
static inline int floor_log2(uint64_t x) { return 63 - __builtin_clzll(x); }   // x > 0
static inline int ceil_log2(uint64_t x) { return x <= 1 ? 0 : floor_log2(x - 1) + 1; }
static inline int32_t unpack_signed(uint32_t v) { return (int32_t)((v >> 1) ^ (0u - (v & 1))); }

static void *xcalloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    return calloc(n ? n : 1, size ? size : 1);
}
static void *xmalloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    return malloc(n * size ? n * size : 1);
}

// 8 floats: clang's vector extension (AVX2, two SSE or NEON registers, as the target has)
typedef float vf __attribute__((ext_vector_type(8)));
typedef int32_t vi __attribute__((ext_vector_type(8)));
static inline vf vload(const float *p) { vf v; memcpy(&v, p, sizeof v); return v; }
static inline void vstore(float *p, vf v) { memcpy(p, &v, sizeof v); }
static inline vf vmin(vf a, vf b) { return a < b ? a : b; }
static inline vf vmax(vf a, vf b) { return a > b ? a : b; }
static inline vf vclamp01(vf a) { vf z = 0, o = 1; return vmin(vmax(a, z), o); }
static inline vf vabs(vf a) { return a < 0 ? -a : a; }
typedef float vfu __attribute__((ext_vector_type(8), aligned(4)));   // (in unaligned memory)

// ---------------------------------------------------------------------------
// bit reader: least significant bits first. Reading past the end gives zero bits and counts
// them; br_overrun() tells whether any of those were actually consumed.

typedef struct {
    const uint8_t *p, *start, *end;
    uint64_t buf;
    int n;            // valid bits in buf
    size_t zeros;     // zero bytes appended past end
} Br;

static void br_init(Br *br, const uint8_t *p, size_t len) {
    br->p = br->start = p; br->end = p + len;
    br->buf = 0; br->n = 0; br->zeros = 0;
}
static inline void br_refill(Br *br) {
    if (br->end - br->p >= 8) {   // 7 or 8 bytes at once, without a loop (the buffer then holds 56..63 bits)
        uint64_t v;
        memcpy(&v, br->p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        v = __builtin_bswap64(v);
#endif
        br->buf |= v << br->n;
        br->p += (63 - br->n) >> 3;
        br->n |= 56;
        return;
    }
    while (br->n <= 56) {
        if (br->p < br->end) br->buf |= (uint64_t)*br->p++ << br->n;
        else br->zeros++;
        br->n += 8;
    }
}
static inline uint32_t br_u(Br *br, int n) {   // n <= 32
    if (!n) return 0;
    if (br->n < n) br_refill(br);
    uint32_t v = (uint32_t)(br->buf & ((((uint64_t)1) << n) - 1));
    br->buf >>= n; br->n -= n;
    return v;
}
static inline int br_overrun(const Br *br) { return br->zeros * 8 > (size_t)br->n; }
static int br_skip_bits(Br *br, uint64_t nbits) {
    while (nbits && br->n) { int k = (int)(nbits < (uint64_t)br->n ? nbits : (uint64_t)br->n); if (k > 32) k = 32; br_u(br, k); nbits -= k; }
    if (nbits >> 3 > (uint64_t)(br->end - br->p)) return ERR_INVALID;
    br->p += nbits >> 3;
    br_u(br, (int)(nbits & 7));
    return br_overrun(br) ? ERR_INVALID : 0;
}
static int br_byte_align(Br *br) {
    int k = br->n & 7;
    return br_u(br, k) ? ERR_INVALID : 0;   // the padding must be zero
}
// position of the next unread byte (after byte alignment)
static const uint8_t *br_byte_pos(const Br *br) { return br->p - (br->n >> 3) + (ptrdiff_t)br->zeros; }

static uint32_t br_u32(Br *br, uint32_t o0, int n0, uint32_t o1, int n1, uint32_t o2, int n2, uint32_t o3, int n3) {
    uint32_t o[4] = {o0, o1, o2, o3}; int n[4] = {n0, n1, n2, n3};
    int s = (int)br_u(br, 2);
    return o[s] + br_u(br, n[s]);
}
static uint64_t br_u64(Br *br) {
    int sel = (int)br_u(br, 2);
    if (sel == 0) return 0;
    if (sel == 1) return 1 + br_u(br, 4);
    if (sel == 2) return 17 + br_u(br, 8);
    uint64_t v = br_u(br, 12);
    int shift = 12;
    while (br_u(br, 1)) {
        if (shift == 60) { v |= (uint64_t)br_u(br, 4) << 60; break; }
        v |= (uint64_t)br_u(br, 8) << shift;
        shift += 8;
    }
    return v;
}
static int br_f16(Br *br, float *out) {
    uint32_t bits = br_u(br, 16);
    int e = bits >> 10 & 31;
    if (e == 31) return ERR_INVALID;
    float m = (float)(bits & 0x3ff);
    float v = e ? ldexpf(m + 1024.f, e - 25) : ldexpf(m, -24);
    *out = bits >> 15 ? -v : v;
    return 0;
}
static int br_enum(Br *br, int *out) {
    uint32_t v = br_u32(br, 0, 0, 1, 0, 2, 4, 18, 6);
    if (v > 63) return ERR_INVALID;
    *out = (int)v;
    return 0;
}
static int br_extensions(Br *br) {
    uint64_t ext = br_u64(br), total = 0;
    for (int i = 0; i < 64; i++) {
        if (ext >> i & 1) {
            uint64_t n = br_u64(br);
            if (n > ((uint64_t)1 << 40) || total + n > ((uint64_t)1 << 40)) return ERR_INVALID;
            total += n;
        }
    }
    return br_skip_bits(br, total);
}

// ---------------------------------------------------------------------------
// prefix codes (Brotli's, RFC 7932 section 3), from J40. A table: the first 2^fast_len entries
// are indexed by the next fast_len bits; an entry is (symbol << 16 | code length - fast_len
// [bits 0..3] | the code past fast_len bits [4..15], for overflow entries), a negative entry
// points to the overflow entries of the longer codes, matched in order.

enum { MAX_TYPICAL_FAST_LEN = 7, MAX_TABLE_GROWTH = 2 };

static int32_t prefix_overflow(Br *br, int fast_len, const int32_t *table) {
    br->buf >>= fast_len; br->n -= fast_len;
    int32_t entry;
    for (;;) {
        entry = *table++;
        int code = (entry >> 4) & 0xfff, len = entry & 15;
        if (code == (int)(br->buf & ((1u << len) - 1))) break;
    }
    return entry;
}

static inline int32_t prefix_symbol(Br *br, int fast_len, int max_len, const int32_t *table) {
    if (br->n < max_len) br_refill(br);
    int32_t entry = table[br->buf & ((1u << fast_len) - 1)];
    if (entry < 0 && fast_len < max_len) entry = prefix_overflow(br, fast_len, table - entry);
    int len = entry & 15;
    br->buf >>= len; br->n -= len;
    return entry >> 16;
}

static int prefix_tree(Br *br, int l2size, int *out_fast, int *out_max, int32_t **out_table) {
    static const uint8_t REV5[32] = {
        0, 16, 8, 24, 4, 20, 12, 28, 2, 18, 10, 26, 6, 22, 14, 30,
        1, 17, 9, 25, 5, 21, 13, 29, 3, 19, 11, 27, 7, 23, 15, 31,
    };
    enum { L1SIZE = 18, L0MAXLEN = 4, L1MAXLEN = 5, L2MAXLEN = 15 };
    enum { L1CODESUM = 1 << L1MAXLEN, L2CODESUM = 1 << L2MAXLEN };
    static const int32_t L0TABLE[1 << L0MAXLEN] = {
        0x00002, 0x40002, 0x30002, 0x20003, 0x00002, 0x40002, 0x30002, 0x10004,
        0x00002, 0x40002, 0x30002, 0x20003, 0x00002, 0x40002, 0x30002, 0x50004,
    };
    static const uint8_t L1ZIGZAG[L1SIZE] = {1,2,3,4,0,5,17,6,16,7,8,9,10,11,12,13,14,15};

    int32_t l1lengths[L1SIZE] = {0}, *l2lengths = NULL;
    int32_t l1counts[L1MAXLEN + 1] = {0}, l2counts[L2MAXLEN + 1] = {0};
    int32_t l1starts[L1MAXLEN + 1], l2starts[L2MAXLEN + 1], l2overflows[L2MAXLEN + 1];
    int32_t l1table[1 << L1MAXLEN] = {0}, *l2table = NULL;
    int32_t total, code, hskip, fast_len, i, j;
    int err = ERR_INVALID;

    *out_table = NULL;
    if (l2size == 1) {
        *out_fast = *out_max = 0;
        if (!(*out_table = xcalloc(1, sizeof(int32_t)))) return ERR_NOMEM;
        return 0;
    }

    hskip = (int32_t)br_u(br, 2);
    if (hskip == 1) {   // simple prefix codes (section 3.4)
        static const struct { int8_t maxlen, sortfrom, sortto, len[8], symref[8]; } TEMPLATES[5] = {
            { 3, 2, 4, {1,2,1,3,1,2,1,3}, {0,1,0,2,0,1,0,3} },  // NSYM=4 tree-select 1 (1233)
            { 0, 0, 0, {0}, {0} },                              // NSYM=1 (0)
            { 1, 0, 2, {1,1}, {0,1} },                          // NSYM=2 (11)
            { 2, 1, 3, {1,2,1,2}, {0,1,0,2} },                  // NSYM=3 (122)
            { 2, 0, 4, {2,2,2,2}, {0,1,2,3} },                  // NSYM=4 tree-select 0 (2222)
        };
        int32_t nsym = (int32_t)br_u(br, 2) + 1, syms[4], tmp, nbits = ceil_log2((uint64_t)l2size);
        for (i = 0; i < nsym; ++i) {
            syms[i] = (int32_t)br_u(br, nbits);
            if (syms[i] >= l2size) return ERR_INVALID;
            for (j = 0; j < i; ++j) if (syms[i] == syms[j]) return ERR_INVALID;
        }
        if (nsym == 4 && br_u(br, 1)) nsym = 0;   // tree-select
        for (i = TEMPLATES[nsym].sortfrom + 1; i < TEMPLATES[nsym].sortto; ++i) {
            for (j = i; j > TEMPLATES[nsym].sortfrom && syms[j - 1] > syms[j]; --j) {
                tmp = syms[j - 1]; syms[j - 1] = syms[j]; syms[j] = tmp;
            }
        }
        *out_fast = *out_max = TEMPLATES[nsym].maxlen;
        if (!(*out_table = xmalloc((size_t)1 << *out_max, sizeof(int32_t)))) return ERR_NOMEM;
        for (i = 0; i < (1 << *out_max); ++i)
            (*out_table)[i] = (syms[TEMPLATES[nsym].symref[i]] << 16) | (int32_t)TEMPLATES[nsym].len[i];
        return 0;
    }

    // complex prefix codes (section 3.5): the layer 1 code lengths with the layer 0 code
    total = 0;
    for (i = l1counts[0] = hskip; i < L1SIZE && total < L1CODESUM; ++i) {
        l1lengths[L1ZIGZAG[i]] = code = prefix_symbol(br, L0MAXLEN, L0MAXLEN, L0TABLE);
        ++l1counts[code];
        if (code) total += L1CODESUM >> code;
    }
    // Brotli: complete, or a single code (decoded with no bits)
    if (l1counts[0] == i - 1) {
        for (i = 0; !l1lengths[i]; ++i);
        for (code = 0; code < L1CODESUM; ++code) l1table[code] = i << 16;
    } else if (total != L1CODESUM) {
        return ERR_INVALID;
    } else {
        l1starts[1] = 0;
        for (i = 2; i <= L1MAXLEN; ++i) l1starts[i] = l1starts[i - 1] + (l1counts[i - 1] << (L1MAXLEN - (i - 1)));
        for (i = 0; i < L1SIZE; ++i) {
            int32_t n = l1lengths[i], *start = &l1starts[n];
            if (n == 0) continue;
            for (code = (int32_t)REV5[*start]; code < L1CODESUM; code += 1 << n) l1table[code] = (i << 16) | n;
            *start += L1CODESUM >> n;
        }
    }

    {   // the layer 2 code lengths with the layer 1 code
        int32_t prev = 8, rep, prev_rep = 0;
        if (!(l2lengths = xcalloc((size_t)l2size, sizeof(int32_t)))) return ERR_NOMEM;
        for (i = total = 0; i < l2size && total < L2CODESUM; ) {
            code = prefix_symbol(br, L1MAXLEN, L1MAXLEN, l1table);
            if (code < 16) {
                l2lengths[i++] = code;
                ++l2counts[code];
                if (code) { total += L2CODESUM >> code; prev = code; }
                prev_rep = 0;
            } else if (code == 16) {   // repeat non-zero 3+u(2) times
                if (prev_rep < 0) prev_rep = 0;
                rep = (prev_rep > 0 ? 4 * prev_rep - 5 : 3) + (int32_t)br_u(br, 2);
                if (i + (rep - prev_rep) > l2size) goto fail;
                total += (L2CODESUM * (rep - prev_rep)) >> prev;
                l2counts[prev] += rep - prev_rep;
                for (; prev_rep < rep; ++prev_rep) l2lengths[i++] = prev;
            } else {   // 17: repeat zero 3+u(3) times
                if (prev_rep > 0) prev_rep = 0;
                rep = (prev_rep < 0 ? 8 * prev_rep + 13 : -3) - (int32_t)br_u(br, 3);
                if (i + (prev_rep - rep) > l2size) goto fail;
                for (; prev_rep > rep; --prev_rep) l2lengths[i++] = 0;
            }
            if (br_overrun(br)) goto fail;
        }
        if (total != L2CODESUM) goto fail;
    }

    l2starts[1] = 0;
    *out_max = 1;
    for (i = 2; i <= L2MAXLEN; ++i) {
        l2starts[i] = l2starts[i - 1] + (l2counts[i - 1] << (L2MAXLEN - (i - 1)));
        if (l2counts[i]) *out_max = i;
    }
    if (*out_max <= MAX_TYPICAL_FAST_LEN) {
        fast_len = *out_max;
        if (!(l2table = xmalloc((size_t)1 << fast_len, sizeof(int32_t)))) { err = ERR_NOMEM; goto fail; }
    } else {
        int32_t size, size_limit, size_used;
        fast_len = MAX_TYPICAL_FAST_LEN;
        size = 1 << fast_len;
        for (i = fast_len + 1; i <= *out_max; ++i) size += l2counts[i];
        size_used = size;
        size_limit = size * MAX_TABLE_GROWTH;
        for (i = fast_len + 1; i <= *out_max; ++i) {
            size = size + (1 << i) - l2counts[i];
            if (size <= size_limit) { size_used = size; fast_len = i; }
        }
        l2overflows[fast_len + 1] = 1 << fast_len;
        for (i = fast_len + 2; i <= *out_max; ++i) l2overflows[i] = l2overflows[i - 1] + l2counts[i - 1];
        if (!(l2table = xmalloc((size_t)size_used + 1, sizeof(int32_t)))) { err = ERR_NOMEM; goto fail; }
        l2table[size_used] = 0;
    }
    for (i = 0; i < l2size; ++i) {
        int32_t n = l2lengths[i], *start = &l2starts[n];
        if (n == 0) continue;
        code = ((int32_t)REV5[*start & 31] << 10) | ((int32_t)REV5[*start >> 5 & 31] << 5) | ((int32_t)REV5[*start >> 10]);
        if (n <= fast_len) {
            for (; code < (1 << fast_len); code += 1 << n) l2table[code] = (i << 16) | n;
            *start += L2CODESUM >> n;
        } else {
            if ((code >> fast_len) == 0) l2table[code] = -l2overflows[n];
            *start += L2CODESUM >> n;
            l2table[l2overflows[n]++] = (i << 16) | (code >> fast_len << 4) | (n - fast_len);
        }
    }
    *out_fast = fast_len;
    *out_table = l2table;
    free(l2lengths);
    return 0;
fail:
    free(l2lengths);
    free(l2table);
    return err;
}

// ---------------------------------------------------------------------------
// hybrid integers, ANS, entropy codes (J40, with the libjxl names in comments)

typedef struct { int8_t split_exp, msb, lsb; int32_t max_token; } HybridCfg;

static int read_hybrid_cfg(Br *br, int log_alpha, HybridCfg *c) {
    c->split_exp = (int8_t)br_u(br, ceil_log2((uint64_t)log_alpha + 1));
    if (c->split_exp > log_alpha) return ERR_INVALID;
    if (c->split_exp != log_alpha) {
        c->msb = (int8_t)br_u(br, ceil_log2((uint64_t)c->split_exp + 1));
        if (c->msb > c->split_exp) return ERR_INVALID;
        c->lsb = (int8_t)br_u(br, ceil_log2((uint64_t)(c->split_exp - c->msb) + 1));
        if (c->lsb > c->split_exp - c->msb) return ERR_INVALID;
    } else {
        c->msb = c->lsb = 0;
    }
    c->max_token = (1 << c->split_exp) + ((32 - c->split_exp) << (c->lsb + c->msb)) - 1;
    return 0;
}

// returns the value; *bad set on a token past max_token (whose value would not fit 32 bits: up to
// max_token the value has at most 32 bits, nbits at most 31)
static inline uint32_t hybrid_value(Br *br, uint32_t token, const HybridCfg *c, int *bad) {
    uint32_t split = 1u << c->split_exp;
    if (token < split) return token;
    if (__builtin_expect(token > (uint32_t)c->max_token, 0)) { *bad = 1; return 0; }
    int bits_in_token = c->msb + c->lsb;
    int nbits = c->split_exp - bits_in_token + (int)((token - split) >> bits_in_token);
    uint32_t low = token & ((1u << c->lsb) - 1);
    token >>= c->lsb;
    uint32_t top = (token & ((1u << c->msb) - 1)) | (1u << c->msb);
    return (((top << nbits) | br_u(br, nbits)) << c->lsb) | low;
}

enum { DIST_BITS = 12, ANS_INIT_STATE = 0x130000 };
typedef struct { int16_t cutoff, offset, symbol; uint16_t freq_lo, freq_hi; } Alias;   // (freq: of the bucket's own symbol, of `symbol`)

static int init_alias(const int16_t *D, int log_alpha, Alias **out) {
    int16_t log_bucket = (int16_t)(DIST_BITS - log_alpha), bucket_size = (int16_t)(1 << log_bucket);
    int16_t table_size = (int16_t)(1 << log_alpha);
    Alias *b = xmalloc((size_t)table_size, sizeof(Alias));
    int16_t u = -1, o = -1, i, j;
    if (!b) return ERR_NOMEM;
    for (i = 0; i < table_size && !D[i]; ++i);
    for (j = (int16_t)(i + 1); j < table_size && !D[j]; ++j);
    if (i < table_size && j >= table_size) {   // D[i] is the only non-zero probability
        for (j = 0; j < table_size; ++j) { b[j].symbol = i; b[j].offset = (int16_t)(j << log_bucket); b[j].cutoff = 0; }
        *out = b;
        return 0;
    }
    for (i = 0; i < table_size; ++i) {
        int16_t cutoff = D[i];
        b[i].cutoff = cutoff;
        if (cutoff > bucket_size) { b[i].offset = o; o = i; }        // offset: next in the stack
        else if (cutoff < bucket_size) { b[i].offset = u; u = i; }
        else { b[i].symbol = i; b[i].offset = 0; }
    }
    while (o >= 0) {
        int16_t by, tmp;
        if (u < 0) { free(b); return ERR_INVALID; }
        by = (int16_t)(bucket_size - b[u].cutoff);
        tmp = b[u].offset;
        b[o].cutoff = (int16_t)(b[o].cutoff - by);
        b[u].symbol = o;
        b[u].offset = (int16_t)(b[o].cutoff - b[u].cutoff);
        u = tmp;
        if (b[o].cutoff < bucket_size) {
            tmp = b[o].offset; b[o].offset = u; u = o; o = tmp;
        } else if (b[o].cutoff == bucket_size) {
            tmp = b[o].offset; b[o].offset = 0; o = tmp;
        }
    }
    *out = b;
    return 0;
}

static inline uint32_t u8_ans(Br *br) {   // U8() of the ANS distributions
    if (!br_u(br, 1)) return 0;
    int n = (int)br_u(br, 3);
    return br_u(br, n) + (1u << n);
}

static int ans_table(Br *br, int log_alpha, int16_t **out) {
    enum { DISTSUM = 1 << DIST_BITS };
    int32_t table_size = 1 << log_alpha, i;
    int16_t *D = xcalloc((size_t)table_size, sizeof(int16_t));
    if (!D) return ERR_NOMEM;
    switch (br_u(br, 2)) {   // two Bool()s in one u(2): bits swapped
    case 1: {
        uint32_t v = u8_ans(br);
        if (v >= (uint32_t)table_size) goto bad;
        D[v] = DISTSUM;
        break;
    }
    case 3: {
        uint32_t v1 = u8_ans(br), v2 = u8_ans(br);
        if (v1 == v2 || v1 >= (uint32_t)table_size || v2 >= (uint32_t)table_size) goto bad;
        D[v1] = (int16_t)br_u(br, DIST_BITS);
        D[v2] = (int16_t)(DISTSUM - D[v1]);
        break;
    }
    case 2: {
        int32_t alpha_size = (int32_t)u8_ans(br) + 1;
        int16_t d = (int16_t)(DISTSUM / alpha_size), bias = (int16_t)(DISTSUM % alpha_size);
        if (alpha_size > table_size) goto bad;
        for (i = 0; i < bias; ++i) D[i] = (int16_t)(d + 1);
        for (; i < alpha_size; ++i) D[i] = d;
        break;
    }
    case 0: {
        int32_t len, shift, alpha_size, omit_log, omit_pos, code, total, n;
        int32_t ncodes, codes[259];   // exponents if >= 0, negated repeat count if < 0
        len = br_u(br, 1) ? br_u(br, 1) ? br_u(br, 1) ? 3 : 2 : 1 : 0;
        shift = (int32_t)br_u(br, len) + (1 << len) - 1;
        if (shift > 13) goto bad;
        alpha_size = (int32_t)u8_ans(br) + 3;
        omit_log = -1;
        for (i = ncodes = 0; i < alpha_size; ) {
            static const int32_t TABLE[] = {   // kLogCountLut
                0xa0003,     -16, 0x70003, 0x30004, 0x60003, 0x80003, 0x90003, 0x50004,
                0xa0003, 0x40004, 0x70003, 0x10004, 0x60003, 0x80003, 0x90003, 0x20004,
                0x00011, 0xb0022, 0xc0003, 0xd0043,
            };
            code = prefix_symbol(br, 4, 7, TABLE);
            if (code < 13) {
                ++i;
                codes[ncodes++] = code;
                if (omit_log < code) omit_log = code;
            } else {
                i += code = (int32_t)u8_ans(br) + 4;
                codes[ncodes++] = -code;
            }
            if (ncodes >= 259) goto bad;
        }
        if (i != alpha_size || omit_log < 0) goto bad;
        omit_pos = -1;
        for (i = n = total = 0; i < ncodes && n < table_size; ++i) {
            code = codes[i];
            if (code < 0) {
                int16_t prev = n > 0 ? D[n - 1] : 0;
                if (prev < 0) goto bad;
                code = imin(-code, table_size - n);
                total += (int32_t)prev * code;
                while (code-- > 0) D[n++] = prev;
            } else if (code == omit_log) {
                omit_pos = n;
                omit_log = -1;
                D[n++] = -1;
            } else if (code < 2) {
                total += code;
                D[n++] = (int16_t)code;
            } else {
                int32_t bitcount;
                --code;
                bitcount = imin(imax(0, shift - ((DIST_BITS - code) >> 1)), code);
                code = (1 << code) + ((int32_t)br_u(br, bitcount) << (code - bitcount));
                total += code;
                D[n++] = (int16_t)code;
            }
        }
        if (omit_pos < 0 || total > DISTSUM) goto bad;
        D[omit_pos] = (int16_t)(DISTSUM - total);
        break;
    }
    }
    if (br_overrun(br)) goto bad;
    *out = D;
    return 0;
bad:
    free(D);
    return ERR_INVALID;
}

typedef struct {
    HybridCfg cfg;
    int16_t *D; Alias *alias;           // ANS
    int fast_len, max_len; int32_t *table;   // prefix
    int count;                          // (while reading: the prefix alphabet size)
} Cluster;

typedef struct {
    int num_dist, lz77, use_prefix, log_alpha;
    uint32_t min_symbol, min_length;
    HybridCfg lz_len;
    int nclusters;
    uint8_t *map;
    Cluster *cl;
    Alias *alias;     // ANS: the clusters' alias tables one after the other (cluster << log_alpha | bucket)
    HybridCfg *cfg;   // the clusters' hybrid integer configurations
} Code;

typedef struct {
    const Code *code;
    uint32_t state;           // ANS; 0 before the first symbol
    int32_t *window;          // LZ77, 2^20 entries
    uint32_t ndec, copy_pos, to_copy;
    int dist_mult;
    int bad;                  // a value that did not fit, or a broken LZ77 copy
} Sym;

static void code_free(Code *c) {
    if (c->cl) {
        for (int i = 0; i < c->nclusters; i++) { free(c->cl[i].D); free(c->cl[i].alias); free(c->cl[i].table); }
        free(c->cl);
    }
    free(c->map); free(c->alias); free(c->cfg);
    memset(c, 0, sizeof(*c));
}

static int read_code(Br *br, int num_dist, int allow_lz77, Code *c);
// (ANS: the initial state is read here, as libjxl's ANSSymbolReader does)
static void sym_init(Sym *s, const Code *c, int dist_mult, Br *br) {
    memset(s, 0, sizeof(*s)); s->code = c; s->dist_mult = dist_mult;
    if (!c->use_prefix) { s->state = br_u(br, 16); s->state |= br_u(br, 16) << 16; }
}
static inline uint32_t sym_read(Br *br, Sym *s, int ctx);
static int sym_finish(Br *br, Sym *s);   // checks the ANS final state, frees the window

static int cluster_map(Br *br, int num_dist, int max_allowed, int *nclusters, uint8_t **outmap) {
    uint32_t seen[8] = {0};
    uint8_t *map;
    int i, j;
    if (max_allowed > num_dist) max_allowed = num_dist;
    *outmap = NULL;
    if (!(map = xcalloc((size_t)num_dist, 1))) return ERR_NOMEM;
    if (num_dist == 1) { *nclusters = 1; *outmap = map; return 0; }
    if (br_u(br, 1)) {   // simple
        int nbits = (int)br_u(br, 2);
        for (i = 0; i < num_dist; ++i) {
            map[i] = (uint8_t)br_u(br, nbits);
            if (map[i] >= max_allowed) goto bad;
        }
    } else {
        int use_mtf = (int)br_u(br, 1);
        Code sub = {0};
        Sym s;
        int e = read_code(br, 1, num_dist > 2, &sub);
        if (e) { free(map); return e; }
        sym_init(&s, &sub, 0, br);
        for (i = 0; i < num_dist; ++i) {
            uint32_t v = sym_read(br, &s, 0);
            if (v >= (uint32_t)max_allowed || s.bad) { sym_finish(br, &s); code_free(&sub); goto bad; }
            map[i] = (uint8_t)v;
        }
        e = sym_finish(br, &s);
        code_free(&sub);
        if (e) { free(map); return e; }
        if (use_mtf) {
            uint8_t mtf[256], moved;
            for (i = 0; i < 256; ++i) mtf[i] = (uint8_t)i;
            for (i = 0; i < num_dist; ++i) {
                j = map[i];
                map[i] = moved = mtf[j];
                for (; j > 0; --j) mtf[j] = mtf[j - 1];
                mtf[0] = moved;
            }
        }
    }
    for (i = 0; i < num_dist; ++i) seen[map[i] >> 5] |= 1u << (map[i] & 31);
    for (i = 0; i < 256 && (seen[i >> 5] >> (i & 31) & 1); ++i);
    *nclusters = i;
    for (; i < 256 && !(seen[i >> 5] >> (i & 31) & 1); ++i);
    if (i != 256 || br_overrun(br)) goto bad;
    *outmap = map;
    return 0;
bad:
    free(map);
    return ERR_INVALID;
}

static int read_code(Br *br, int num_dist, int allow_lz77, Code *c) {
    int i, e;
    memset(c, 0, sizeof(*c));
    c->lz77 = (int)br_u(br, 1);
    if (c->lz77) {
        if (!allow_lz77) return ERR_INVALID;
        c->min_symbol = br_u32(br, 224, 0, 512, 0, 4096, 0, 8, 15);
        c->min_length = br_u32(br, 3, 0, 4, 0, 5, 2, 9, 8);
        TRY(read_hybrid_cfg(br, 8, &c->lz_len));
        ++num_dist;   // the last distribution: LZ77 distances
    } else {
        c->min_symbol = c->min_length = 0xffffffffu;
    }
    if ((e = cluster_map(br, num_dist, 256, &c->nclusters, &c->map))) return e;
    if (!(c->cl = xcalloc((size_t)c->nclusters, sizeof(Cluster)))) { code_free(c); return ERR_NOMEM; }
    c->use_prefix = (int)br_u(br, 1);
    if (c->use_prefix) {
        for (i = 0; i < c->nclusters; ++i) if ((e = read_hybrid_cfg(br, 15, &c->cl[i].cfg))) goto fail;
        for (i = 0; i < c->nclusters; ++i) {
            if (br_u(br, 1)) {
                int n = (int)br_u(br, 4);
                c->cl[i].count = 1 + (1 << n) + (int)br_u(br, n);
                if (c->cl[i].count > (1 << 15)) { e = ERR_INVALID; goto fail; }
            } else {
                c->cl[i].count = 1;
            }
        }
        for (i = 0; i < c->nclusters; ++i) {
            Cluster *k = &c->cl[i];
            if ((e = prefix_tree(br, k->count, &k->fast_len, &k->max_len, &k->table))) goto fail;
        }
    } else {
        c->log_alpha = 5 + (int)br_u(br, 2);
        for (i = 0; i < c->nclusters; ++i) if ((e = read_hybrid_cfg(br, c->log_alpha, &c->cl[i].cfg))) goto fail;
        for (i = 0; i < c->nclusters; ++i) {
            Cluster *k = &c->cl[i];
            if ((e = ans_table(br, c->log_alpha, &k->D))) goto fail;
            if ((e = init_alias(k->D, c->log_alpha, &k->alias))) goto fail;
            for (int j = 0; j < (1 << c->log_alpha); j++) {
                Alias *a = &k->alias[j];
                a->freq_lo = (uint16_t)k->D[j];
                a->freq_hi = (uint16_t)(a->symbol >= 0 && a->symbol < (1 << c->log_alpha) ? k->D[a->symbol] : 0);
            }
        }
    }
    c->num_dist = num_dist;
    if (br_overrun(br)) { e = ERR_INVALID; goto fail; }
    if (!(c->cfg = xmalloc((size_t)c->nclusters, sizeof(HybridCfg)))) { e = ERR_NOMEM; goto fail; }
    for (i = 0; i < c->nclusters; i++) c->cfg[i] = c->cl[i].cfg;
    if (!c->use_prefix) {
        size_t n = (size_t)1 << c->log_alpha;
        if (!(c->alias = xmalloc((size_t)c->nclusters * n, sizeof(Alias)))) { e = ERR_NOMEM; goto fail; }
        for (i = 0; i < c->nclusters; i++) memcpy(c->alias + (size_t)i * n, c->cl[i].alias, n * sizeof(Alias));
    }
    return 0;
fail:
    code_free(c);
    return e;
}

// LZ77 (the rare paths, kept out of sym_read)
static __attribute__((noinline)) uint32_t lz77_copy(Sym *s) {
    enum { WMASK = 0xfffff };
    s->to_copy--;
    int32_t v = s->window[s->copy_pos++ & WMASK];
    s->window[s->ndec++ & WMASK] = v;
    return (uint32_t)v;
}
static inline uint32_t ans_token(Br *br, const Code *c, Sym *s, uint32_t cl);
static __attribute__((noinline)) uint32_t lz77_start(Br *br, Sym *s, uint32_t token) {
    enum { WMASK = 0xfffff };
    const Code *c = s->code;
    uint32_t lcl = c->map[c->num_dist - 1];
    uint32_t n = hybrid_value(br, token - c->min_symbol, &c->lz_len, &s->bad) + c->min_length;
    uint32_t dtoken = c->use_prefix ? (uint32_t)prefix_symbol(br, c->cl[lcl].fast_len, c->cl[lcl].max_len, c->cl[lcl].table)
                                    : ans_token(br, c, s, lcl);
    uint32_t distance = hybrid_value(br, dtoken, &c->cfg[lcl], &s->bad);
    if (s->bad) return 0;
    if (!s->dist_mult) {
        distance++;
    } else if (distance >= 120) {
        distance -= 119;
    } else {
        static const uint8_t SPECIAL_DISTANCES[120] = {   // {a,b} encoded as (a+7)*16+b
            0x71, 0x80, 0x81, 0x61, 0x72, 0x90, 0x82, 0x62, 0x91, 0x51, 0x92, 0x52,
            0x73, 0xa0, 0x83, 0x63, 0xa1, 0x41, 0x93, 0x53, 0xa2, 0x42, 0x74, 0xb0,
            0x84, 0x64, 0xb1, 0x31, 0xa3, 0x43, 0x94, 0x54, 0xb2, 0x32, 0x75, 0xa4,
            0x44, 0xb3, 0x33, 0xc0, 0x85, 0x65, 0xc1, 0x21, 0x95, 0x55, 0xc2, 0x22,
            0xb4, 0x34, 0xa5, 0x45, 0xc3, 0x23, 0x76, 0xd0, 0x86, 0x66, 0xd1, 0x11,
            0x96, 0x56, 0xd2, 0x12, 0xb5, 0x35, 0xc4, 0x24, 0xa6, 0x46, 0xd3, 0x13,
            0x77, 0xe0, 0x87, 0x67, 0xc5, 0x25, 0xe1, 0x01, 0xb6, 0x36, 0xd4, 0x14,
            0x97, 0x57, 0xe2, 0x02, 0xa7, 0x47, 0xe3, 0x03, 0xc6, 0x26, 0xd5, 0x15,
            0xf0, 0xb7, 0x37, 0xe4, 0x04, 0xf1, 0xf2, 0xd6, 0x16, 0xf3, 0xc7, 0x27,
            0xe5, 0x05, 0xf4, 0xd7, 0x17, 0xe6, 0x06, 0xf5, 0xe7, 0x07, 0xf6, 0xf7,
        };
        int special = SPECIAL_DISTANCES[distance];
        int64_t d = ((special >> 4) - 7) + (int64_t)s->dist_mult * (special & 7);
        distance = d < 1 ? 1 : (uint32_t)d;
    }
    if (distance > s->ndec) distance = s->ndec;
    if (distance > (1u << 20)) distance = 1u << 20;
    if (!s->window && !(s->window = xcalloc((size_t)1 << 20, sizeof(int32_t)))) { s->bad = 1; return 0; }
    s->copy_pos = s->ndec - distance;
    s->to_copy = n - 1;
    int32_t v = s->window[s->copy_pos++ & WMASK];   // (distance 0: the zeros of an empty window)
    s->window[s->ndec++ & WMASK] = v;
    return (uint32_t)v;
}
static __attribute__((noinline)) void lz77_store(Sym *s, uint32_t v) {
    if (!s->window && !(s->window = xcalloc((size_t)1 << 20, sizeof(int32_t)))) { s->bad = 1; return; }
    s->window[s->ndec++ & 0xfffff] = (int32_t)v;
}

// the state update of one ANS step, without branches (the alias choice and the renormalisation are
// unpredictable): as libjxl's ANSSymbolReader
static inline __attribute__((always_inline)) uint32_t ans_step(Br *br, const Alias *b, uint32_t i, uint32_t pos, uint32_t *state) {
    uint32_t st = *state, m = 0u - (uint32_t)((int32_t)pos < b->cutoff);   // all ones: the bucket's own symbol
    uint32_t symbol = (i & m) | ((uint32_t)b->symbol & ~m);
    uint32_t freq = ((uint32_t)b->freq_lo & m) | ((uint32_t)b->freq_hi & ~m);
    st = freq * (st >> 12) + ((uint32_t)(uint16_t)b->offset & ~m) + pos;
    if (br->n < 16) br_refill(br);
    uint32_t norm = 0u - (uint32_t)(st < (1u << 16)), k = norm & 16;
    st = ((st << 16 | (uint32_t)(br->buf & 0xffff)) & norm) | (st & ~norm);
    br->buf >>= k; br->n -= (int)k;
    *state = st;
    return symbol;
}

// one ANS step with cluster cl's distribution
static inline __attribute__((always_inline)) uint32_t ans_token(Br *br, const Code *c, Sym *s, uint32_t cl) {
    uint32_t st = s->state, log_bucket = (uint32_t)(DIST_BITS - c->log_alpha);
    uint32_t index = st & 0xfff, i = index >> log_bucket, pos = index & ((1u << log_bucket) - 1);
    const Alias *b = &c->alias[(cl << c->log_alpha) | i];
    uint32_t symbol = ans_step(br, b, i, pos, &st);
    s->state = st;
    return symbol;
}

// The same for ANS without LZ77, the state and the bit reader in the caller's locals: a decoding loop
// that also stores through pointers (coefficients, pixels) can keep them in registers.
static inline __attribute__((always_inline)) uint32_t ans_fast(Br *br, const uint8_t *map, const Alias *alias, int log_alpha,
                                                                const HybridCfg *cfg, uint32_t *state, int *bad, int ctx) {
    uint32_t cl = map[ctx], st = *state, log_bucket = (uint32_t)(DIST_BITS - log_alpha);
    uint32_t index = st & 0xfff, i = index >> log_bucket, pos = index & ((1u << log_bucket) - 1);
    const Alias *b = &alias[(cl << log_alpha) | i];
    uint32_t token = ans_step(br, b, i, pos, &st);
    *state = st;
    const HybridCfg *h = &cfg[cl];
    return token < (1u << h->split_exp) ? token : hybrid_value(br, token, h, bad);
}

// the next value with context ctx (DecodeHybridVarLenUint)
static inline __attribute__((always_inline)) uint32_t sym_read(Br *br, Sym *s, int ctx) {
    const Code *c = s->code;
    if (__builtin_expect(s->to_copy != 0, 0)) return lz77_copy(s);
    uint32_t cl = c->map[ctx], token;
    if (c->use_prefix) {
        const Cluster *k = &c->cl[cl];
        token = (uint32_t)prefix_symbol(br, k->fast_len, k->max_len, k->table);
    } else {
        token = ans_token(br, c, s, cl);
    }
    if (__builtin_expect(token >= c->min_symbol, 0)) return lz77_start(br, s, token);   // (only when lz77 is on)
    const HybridCfg *h = &c->cfg[cl];
    uint32_t v = token < (1u << h->split_exp) ? token : hybrid_value(br, token, h, &s->bad);
    if (__builtin_expect(c->lz77, 0)) lz77_store(s, v);
    return v;
}

static int sym_finish(Br *br, Sym *s) {
    int e = s->bad ? ERR_INVALID : 0;
    if (!e && !s->code->use_prefix && s->state != ANS_INIT_STATE) e = ERR_INVALID;
    if (!e && br_overrun(br)) e = ERR_INVALID;
    free(s->window);
    s->window = NULL;
    return e;
}

// ---------------------------------------------------------------------------
// image header (SizeHeader, ImageMetadata, ColourEncoding ...)

enum { EC_ALPHA = 0, EC_DEPTH, EC_SPOT, EC_SELECTION, EC_BLACK, EC_CFA, EC_THERMAL };
enum { CS_RGB = 0, CS_GREY = 1, CS_XYB = 2, CS_UNKNOWN = 3 };
enum { TF_709 = 1, TF_UNKNOWN = 2, TF_LINEAR = 8, TF_SRGB = 13, TF_PQ = 16, TF_DCI = 17, TF_HLG = 18 };
#define MAX_EC 64

typedef struct {
    int type, bits, exp_bits, dim_shift, alpha_associated;
} ExtraChannel;

typedef struct {
    int width, height, orientation;
    int have_preview, preview_w, preview_h;
    int animated, have_timecodes;
    int bits, exp_bits, modular_16bit;
    int num_ec;
    ExtraChannel ec[MAX_EC];
    int xyb;
    // colour encoding
    int want_icc, cspace, gamma;   // gamma: 0, or the encoded 1/gamma * 1e7
    int tf;                        // transfer function (when gamma is 0)
    float white[2], prim[3][2];
    float intensity_target;
    float opsin_inv[3][3], opsin_bias[3], quant_bias[3], quant_bias_num;
    float up2[15], up4[55], up8[210];
    int custom_up;                 // bits: up2 / up4 / up8 weights given
    uint8_t *icc; size_t icc_len;
} Meta;

static int size_header(Br *br, int *w, int *h) {
    int div8 = (int)br_u(br, 1);
    uint32_t hh = div8 ? (br_u(br, 5) + 1) * 8 : br_u32(br, 1, 9, 1, 13, 1, 18, 1, 30);
    uint64_t ww;
    switch (br_u(br, 3)) {
    case 0: ww = div8 ? (br_u(br, 5) + 1) * 8 : br_u32(br, 1, 9, 1, 13, 1, 18, 1, 30); break;
    case 1: ww = hh; break;
    case 2: ww = (uint64_t)hh * 6 / 5; break;
    case 3: ww = (uint64_t)hh * 4 / 3; break;
    case 4: ww = (uint64_t)hh * 3 / 2; break;
    case 5: ww = (uint64_t)hh * 16 / 9; break;
    case 6: ww = (uint64_t)hh * 5 / 4; break;
    default: ww = (uint64_t)hh * 2; break;
    }
    if (!hh || !ww || hh > 0x3fffffff || ww > 0x3fffffff) return ERR_LIMIT;
    *w = (int)ww; *h = (int)hh;
    return 0;
}

static int bit_depth(Br *br, int *bits, int *exp_bits) {
    if (br_u(br, 1)) {   // float
        *bits = (int)br_u32(br, 32, 0, 16, 0, 24, 0, 1, 6);
        *exp_bits = (int)br_u(br, 4) + 1;
        int mant = *bits - *exp_bits - 1;
        if (mant < 2 || mant > 23 || *exp_bits < 2 || *exp_bits > 8) return ERR_INVALID;
    } else {
        *bits = (int)br_u32(br, 8, 0, 10, 0, 12, 0, 1, 6);
        *exp_bits = 0;
        if (*bits < 1 || *bits > 31) return ERR_INVALID;
    }
    return 0;
}

static int skip_name(Br *br) {
    uint32_t len = br_u32(br, 0, 0, 0, 4, 16, 5, 48, 10);
    return br_skip_bits(br, (uint64_t)len * 8);
}

static int custom_xy(Br *br, float xy[2]) {
    for (int i = 0; i < 2; i++)
        xy[i] = (float)unpack_signed(br_u32(br, 0, 19, 0x80000, 19, 0x100000, 20, 0x200000, 21)) / 100000.0f;
    return 0;
}

static void colour_defaults(Meta *m) {   // sRGB
    static const float SRGB[4][2] = {{0.3127f, 0.3290f}, {0.639998686f, 0.330010138f},
                                     {0.300003784f, 0.600003357f}, {0.150002046f, 0.059997204f}};
    memcpy(m->white, SRGB[0], sizeof m->white);
    memcpy(m->prim, SRGB + 1, sizeof m->prim);
    m->want_icc = 0; m->cspace = CS_RGB; m->gamma = 0; m->tf = TF_SRGB;
}

static int colour_encoding(Br *br, Meta *m) {
    colour_defaults(m);
    if (br_u(br, 1)) return 0;   // all_default
    int v;
    m->want_icc = (int)br_u(br, 1);
    TRY(br_enum(br, &v));
    if (v > 3) return ERR_INVALID;
    m->cspace = v;
    if (m->want_icc) return 0;
    if (m->cspace != CS_XYB) {
        TRY(br_enum(br, &v));
        switch (v) {
        case 1: break;   // D65
        case 2: custom_xy(br, m->white); break;
        case 10: m->white[0] = m->white[1] = 1 / 3.f; break;
        case 11: m->white[0] = 0.314f; m->white[1] = 0.351f; break;
        default: return ERR_INVALID;
        }
        if (m->cspace != CS_GREY) {
            static const float BT2100[3][2] = {{0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}};
            static const float P3[3][2] = {{0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}};
            TRY(br_enum(br, &v));
            switch (v) {
            case 1: break;
            case 2: for (int i = 0; i < 3; i++) custom_xy(br, m->prim[i]); break;
            case 9: memcpy(m->prim, BT2100, sizeof BT2100); break;
            case 11: memcpy(m->prim, P3, sizeof P3); break;
            default: return ERR_INVALID;
            }
        }
    }
    if (br_u(br, 1)) {   // have_gamma
        m->gamma = (int)br_u(br, 24);
        if (m->gamma <= 0 || m->gamma > 10000000) return ERR_INVALID;
    } else {
        TRY(br_enum(br, &v));
        if (v != TF_709 && v != TF_UNKNOWN && v != TF_LINEAR && v != TF_SRGB && v != TF_PQ && v != TF_DCI && v != TF_HLG)
            return ERR_INVALID;
        m->tf = v;
    }
    TRY(br_enum(br, &v));   // rendering intent
    if (v > 3) return ERR_INVALID;
    return 0;
}

static int image_metadata(Br *br, Meta *m) {
    static const float OPSIN_INV[3][3] = {
        {11.031566901960783f, -9.866943921568629f, -0.16462299647058826f},
        {-3.254147380392157f, 4.418770392156863f, -0.16462299647058826f},
        {-3.6588512862745097f, 2.7129230470588235f, 1.9459282392156863f},
    };
    m->orientation = 1;
    m->bits = 8; m->exp_bits = 0; m->modular_16bit = 1;
    m->xyb = 1;
    m->intensity_target = 255.f;
    memcpy(m->opsin_inv, OPSIN_INV, sizeof OPSIN_INV);
    m->opsin_bias[0] = m->opsin_bias[1] = m->opsin_bias[2] = -0.0037930732552754493f;
    m->quant_bias[0] = 1.0f - 0.05465007330715401f;
    m->quant_bias[1] = 1.0f - 0.07005449891748593f;
    m->quant_bias[2] = 1.0f - 0.049935103337343655f;
    m->quant_bias_num = 0.145f;
    colour_defaults(m);
    if (br_u(br, 1) == 0) {   // !all_default
        int extra = (int)br_u(br, 1);
        if (extra) {
            m->orientation = (int)br_u(br, 3) + 1;
            if (br_u(br, 1)) { int w, h; TRY(size_header(br, &w, &h)); }   // intrinsic size
            if ((m->have_preview = (int)br_u(br, 1))) {
                int div8 = (int)br_u(br, 1);
                uint32_t ph = div8 ? br_u32(br, 16, 0, 32, 0, 1, 5, 33, 9) * 8 : br_u32(br, 1, 6, 65, 8, 321, 10, 1345, 12);
                uint64_t pw;
                switch (br_u(br, 3)) {
                case 0: pw = div8 ? br_u32(br, 16, 0, 32, 0, 1, 5, 33, 9) * 8 : br_u32(br, 1, 6, 65, 8, 321, 10, 1345, 12); break;
                case 1: pw = ph; break;
                case 2: pw = (uint64_t)ph * 6 / 5; break;
                case 3: pw = (uint64_t)ph * 4 / 3; break;
                case 4: pw = (uint64_t)ph * 3 / 2; break;
                case 5: pw = (uint64_t)ph * 16 / 9; break;
                case 6: pw = (uint64_t)ph * 5 / 4; break;
                default: pw = (uint64_t)ph * 2; break;
                }
                if (!pw || !ph || pw > 4096 || ph > 4096) return ERR_INVALID;
                m->preview_w = (int)pw; m->preview_h = (int)ph;
            }
            if ((m->animated = (int)br_u(br, 1))) {
                br_u32(br, 100, 0, 1000, 0, 1, 10, 1, 30);
                br_u32(br, 1, 0, 1001, 0, 1, 8, 1, 10);
                br_u32(br, 0, 0, 0, 3, 0, 16, 0, 32);
                m->have_timecodes = (int)br_u(br, 1);
            }
        }
        TRY(bit_depth(br, &m->bits, &m->exp_bits));
        m->modular_16bit = (int)br_u(br, 1);
        m->num_ec = (int)br_u32(br, 0, 0, 1, 0, 2, 4, 1, 12);
        if (m->num_ec > MAX_EC) return ERR_UNSUP;
        for (int i = 0; i < m->num_ec; i++) {
            ExtraChannel *ec = &m->ec[i];
            memset(ec, 0, sizeof *ec);
            if (br_u(br, 1)) {   // d_alpha
                ec->type = EC_ALPHA; ec->bits = 8;
            } else {
                TRY(br_enum(br, &ec->type));
                TRY(bit_depth(br, &ec->bits, &ec->exp_bits));
                ec->dim_shift = (int)br_u32(br, 0, 0, 3, 0, 4, 0, 1, 3);
                TRY(skip_name(br));
                switch (ec->type) {
                case EC_ALPHA: ec->alpha_associated = (int)br_u(br, 1); break;
                case EC_SPOT: { float f; for (int k = 0; k < 4; k++) TRY(br_f16(br, &f)); break; }
                case EC_CFA: br_u32(br, 1, 0, 0, 2, 3, 4, 19, 8); break;
                case EC_DEPTH: case EC_SELECTION: case EC_BLACK: case EC_THERMAL: case 15: case 16: break;
                default: return ERR_INVALID;
                }
            }
        }
        m->xyb = (int)br_u(br, 1);
        TRY(colour_encoding(br, m));
        if (extra && !br_u(br, 1)) {   // ToneMapping, not all_default
            float min_nits, linear_below;
            TRY(br_f16(br, &m->intensity_target));
            TRY(br_f16(br, &min_nits));
            br_u(br, 1);
            TRY(br_f16(br, &linear_below));
            if (!(m->intensity_target > 0)) return ERR_INVALID;
        }
        TRY(br_extensions(br));
    }
    if (!br_u(br, 1)) {   // !default_m
        if (m->xyb && !br_u(br, 1)) {   // OpsinInverseMatrix, not all_default
            for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) TRY(br_f16(br, &m->opsin_inv[i][j]));
            for (int i = 0; i < 3; i++) TRY(br_f16(br, &m->opsin_bias[i]));
            for (int i = 0; i < 3; i++) TRY(br_f16(br, &m->quant_bias[i]));
            TRY(br_f16(br, &m->quant_bias_num));
        }
        m->custom_up = (int)br_u(br, 3);
        if (m->custom_up & 1) for (int i = 0; i < 15; i++) TRY(br_f16(br, &m->up2[i]));
        if (m->custom_up & 2) for (int i = 0; i < 55; i++) TRY(br_f16(br, &m->up4[i]));
        if (m->custom_up & 4) for (int i = 0; i < 210; i++) TRY(br_f16(br, &m->up8[i]));
    }
    return br_overrun(br) ? ERR_INVALID : 0;
}

// ---------------------------------------------------------------------------
// the ICC profile: entropy-coded bytes, then libjxl's predictive reconstruction (UnpredictICC)

static uint64_t icc_varint(const uint8_t *in, size_t size, size_t *pos) {
    uint64_t v = 0;
    size_t i;
    for (i = 0; *pos + i < size && i < 10; ++i) {
        v |= (uint64_t)(in[*pos + i] & 127) << (7 * i);
        if (!(in[*pos + i] & 128)) break;
    }
    *pos += i + 1;
    return v;
}

typedef struct { uint8_t *p; size_t n, cap; } Bytes;
static int bytes_push(Bytes *b, uint8_t v) {
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        uint8_t *p = realloc(b->p, cap);
        if (!p) return ERR_NOMEM;
        b->p = p; b->cap = cap;
    }
    b->p[b->n++] = v;
    return 0;
}
static int bytes_u32(Bytes *b, uint32_t v) {
    for (int i = 3; i >= 0; i--) TRY(bytes_push(b, (uint8_t)(v >> (i * 8))));
    return 0;
}
static int bytes_tag(Bytes *b, const char *t) {
    for (int i = 0; i < 4; i++) TRY(bytes_push(b, (uint8_t)t[i]));
    return 0;
}

static void icc_shuffle(uint8_t *data, size_t size, size_t width, uint8_t *tmp) {
    size_t height = (size + width - 1) / width, s = 0, j = 0;
    for (size_t i = 0; i < size; i++) {
        tmp[i] = data[j];
        j += height;
        if (j >= size) j = ++s;
    }
    memcpy(data, tmp, size);
}

static uint64_t icc_predict(uint64_t p1, uint64_t p2, uint64_t p3, int order) {
    return order == 0 ? p1 : order == 1 ? 2 * p1 - p2 : 3 * p1 - 3 * p2 + p3;
}
static uint32_t icc_be32_at(const uint8_t *d, size_t size, size_t pos) { return pos + 4 > size ? 0 : be32(d + pos); }

static uint8_t icc_linear_predict(const uint8_t *d, size_t start, size_t i, size_t stride, size_t width, int order) {
    size_t pos = start + i;
    if (width == 1) {
        return (uint8_t)icc_predict(d[pos - stride], d[pos - stride * 2], d[pos - stride * 3], order);
    } else if (width == 2) {
        size_t p = start + (i & ~(size_t)1);
        uint16_t p1 = (uint16_t)(d[p - stride] << 8 | d[p - stride + 1]);
        uint16_t p2 = (uint16_t)(d[p - stride * 2] << 8 | d[p - stride * 2 + 1]);
        uint16_t p3 = (uint16_t)(d[p - stride * 3] << 8 | d[p - stride * 3 + 1]);
        uint16_t pred = (uint16_t)icc_predict(p1, p2, p3, order);
        return (uint8_t)((i & 1) ? pred & 255 : pred >> 8);
    } else {
        size_t p = start + (i & ~(size_t)3);
        uint32_t p1 = icc_be32_at(d, pos, p - stride), p2 = icc_be32_at(d, pos, p - stride * 2), p3 = icc_be32_at(d, pos, p - stride * 3);
        uint32_t pred = (uint32_t)icc_predict(p1, p2, p3, order);
        return (uint8_t)(pred >> ((3 - (i & 3)) * 8));
    }
}

static int icc_unpredict(const uint8_t *enc, size_t size, Bytes *out) {
    static const char *const TAGS[17] = {"cprt", "wtpt", "bkpt", "rXYZ", "gXYZ", "bXYZ", "kXYZ", "rTRC", "gTRC",
                                         "bTRC", "kTRC", "chad", "desc", "chrm", "dmnd", "dmdd", "lumi"};
    static const char *const TYPES[8] = {"XYZ ", "desc", "text", "mluc", "para", "curv", "sf32", "gbd "};
    static const uint8_t HEADER[128] = {
        0,   0,   0,   0,   0,   0,   0,   0,   4, 0, 0, 0, 'm', 'n', 't', 'r',
        'R', 'G', 'B', ' ', 'X', 'Y', 'Z', ' ', 0, 0, 0, 0, 0,   0,   0,   0,
        0,   0,   0,   0,   'a', 'c', 's', 'p', 0, 0, 0, 0, 0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   246, 214, 0, 1, 0, 0, 0,   0,   211, 45,
    };
    size_t pos = 0;
    if (pos >= size) return ERR_INVALID;
    uint64_t osize = icc_varint(enc, size, &pos);
    if (pos >= size) return ERR_INVALID;
    uint64_t csize = icc_varint(enc, size, &pos);
    if (osize >> 28 || csize > size - pos) return ERR_INVALID;
    size_t cpos = pos, cend = pos + (size_t)csize;
    pos = cend;
    uint8_t header[128];
    memcpy(header, HEADER, 128);
    for (int i = 0; i < 4; i++) header[i] = (uint8_t)(osize >> (24 - 8 * i));
    for (size_t i = 0; i <= 128; i++) {
        if (out->n == osize) return cpos == cend && pos == size ? 0 : ERR_INVALID;
        if (i == 128) break;
        // ICCPredictHeader
        const uint8_t *icc = out->p;
        if (i == 8 && out->n >= 8) memcpy(header + 80, icc + 4, 4);
        if (i == 41 && out->n >= 41) {
            if (icc[40] == 'A') { header[41] = 'P'; header[42] = 'P'; header[43] = 'L'; }
            if (icc[40] == 'M') { header[41] = 'S'; header[42] = 'F'; header[43] = 'T'; }
        }
        if (i == 42 && out->n >= 42) {
            if (icc[40] == 'S' && icc[41] == 'G') { header[42] = 'I'; header[43] = ' '; }
            if (icc[40] == 'S' && icc[41] == 'U') { header[42] = 'N'; header[43] = 'W'; }
        }
        if (pos >= size) return ERR_INVALID;
        TRY(bytes_push(out, (uint8_t)(enc[pos++] + header[i])));
    }
    if (cpos >= cend) return ERR_INVALID;
    uint64_t numtags = icc_varint(enc, size, &cpos);
    if (numtags) {
        numtags--;
        if (numtags >> 32) return ERR_INVALID;
        TRY(bytes_u32(out, (uint32_t)numtags));
        uint64_t prevstart = 128 + numtags * 12, prevsize = 0;
        for (;;) {
            if (out->n > osize || cpos > cend) return ERR_INVALID;
            if (cpos == cend) break;
            uint8_t command = enc[cpos++];
            int code = command & 63;
            char tag[5] = {0};
            if (code == 0) break;
            if (code == 1) {
                if (pos + 4 > size) return ERR_INVALID;
                memcpy(tag, enc + pos, 4);
                pos += 4;
            } else if (code == 2) {
                memcpy(tag, "rTRC", 4);
            } else if (code == 3) {
                memcpy(tag, "rXYZ", 4);
            } else {
                if (code - 4 >= 17) return ERR_INVALID;
                memcpy(tag, TAGS[code - 4], 4);
            }
            TRY(bytes_tag(out, tag));
            uint64_t tagstart, tagsize = prevsize;
            if (!strcmp(tag, "rXYZ") || !strcmp(tag, "gXYZ") || !strcmp(tag, "bXYZ") || !strcmp(tag, "kXYZ") ||
                !strcmp(tag, "wtpt") || !strcmp(tag, "bkpt") || !strcmp(tag, "lumi")) tagsize = 20;
            if (command & 64) {
                if (cpos >= cend) return ERR_INVALID;
                tagstart = icc_varint(enc, size, &cpos);
            } else {
                tagstart = prevstart + prevsize;
            }
            if (tagstart >> 32) return ERR_INVALID;
            TRY(bytes_u32(out, (uint32_t)tagstart));
            if (command & 128) {
                if (cpos >= cend) return ERR_INVALID;
                tagsize = icc_varint(enc, size, &cpos);
            }
            if (tagsize >> 32) return ERR_INVALID;
            TRY(bytes_u32(out, (uint32_t)tagsize));
            prevstart = tagstart; prevsize = tagsize;
            if (code == 2) {
                TRY(bytes_tag(out, "gTRC")); TRY(bytes_u32(out, (uint32_t)tagstart)); TRY(bytes_u32(out, (uint32_t)tagsize));
                TRY(bytes_tag(out, "bTRC")); TRY(bytes_u32(out, (uint32_t)tagstart)); TRY(bytes_u32(out, (uint32_t)tagsize));
            }
            if (code == 3) {
                if ((tagstart + tagsize * 2) >> 32) return ERR_INVALID;
                TRY(bytes_tag(out, "gXYZ")); TRY(bytes_u32(out, (uint32_t)(tagstart + tagsize))); TRY(bytes_u32(out, (uint32_t)tagsize));
                TRY(bytes_tag(out, "bXYZ")); TRY(bytes_u32(out, (uint32_t)(tagstart + tagsize * 2))); TRY(bytes_u32(out, (uint32_t)tagsize));
            }
        }
    }
    for (;;) {
        if (out->n > osize || cpos > cend) return ERR_INVALID;
        if (cpos == cend) break;
        uint8_t command = enc[cpos++];
        if (command == 1) {   // insert
            if (cpos >= cend) return ERR_INVALID;
            uint64_t num = icc_varint(enc, size, &cpos);
            if (num > size - pos) return ERR_INVALID;
            for (uint64_t i = 0; i < num; i++) TRY(bytes_push(out, enc[pos++]));
        } else if (command == 2 || command == 3) {   // shuffle 2 / 4
            if (cpos >= cend) return ERR_INVALID;
            uint64_t num = icc_varint(enc, size, &cpos);
            if (num > size - pos) return ERR_INVALID;
            uint8_t *sh = malloc(num * 2 + 1);
            if (!sh) return ERR_NOMEM;
            memcpy(sh, enc + pos, num);
            icc_shuffle(sh, num, command == 2 ? 2 : 4, sh + num);
            for (uint64_t i = 0; i < num; i++) if (bytes_push(out, sh[i])) { free(sh); return ERR_NOMEM; }
            free(sh);
            pos += num;
        } else if (command == 4) {   // predict
            if (cpos + 2 > cend) return ERR_INVALID;
            uint8_t flags = enc[cpos++];
            size_t width = (flags & 3) + 1;
            int order = (flags & 12) >> 2;
            if (width == 3 || order == 3) return ERR_INVALID;
            uint64_t stride = width;
            if (flags & 16) {
                if (cpos >= cend) return ERR_INVALID;
                stride = icc_varint(enc, size, &cpos);
                if (stride < width) return ERR_INVALID;
            }
            if (!out->n || ((out->n - 1) >> 2) < stride) return ERR_INVALID;
            if (cpos >= cend) return ERR_INVALID;
            uint64_t num = icc_varint(enc, size, &cpos);
            if (num > size - pos) return ERR_INVALID;
            uint8_t *sh = malloc(num * 2 + 1);
            if (!sh) return ERR_NOMEM;
            memcpy(sh, enc + pos, num);
            if (width > 1) icc_shuffle(sh, num, width, sh + num);
            size_t start = out->n;
            for (uint64_t i = 0; i < num; i++) {
                uint8_t pred = icc_linear_predict(out->p, start, i, stride, width, order);
                if (bytes_push(out, (uint8_t)(pred + sh[i]))) { free(sh); return ERR_NOMEM; }
            }
            free(sh);
            pos += num;
        } else if (command == 10) {   // XYZ
            TRY(bytes_tag(out, "XYZ ")); TRY(bytes_u32(out, 0));
            if (pos + 12 > size) return ERR_INVALID;
            for (int i = 0; i < 12; i++) TRY(bytes_push(out, enc[pos++]));
        } else if (command >= 16 && command < 24) {
            TRY(bytes_tag(out, TYPES[command - 16])); TRY(bytes_u32(out, 0));
        } else {
            return ERR_INVALID;
        }
    }
    return pos == size && out->n == osize ? 0 : ERR_INVALID;
}

static int read_icc(Br *br, Meta *m) {
    uint64_t enc_size = br_u64(br);
    if (enc_size > (1u << 28)) return ERR_LIMIT;
    Code code;
    Sym s;
    TRY(read_code(br, 41, 1, &code));
    uint8_t *enc = xmalloc(enc_size, 1);
    Bytes out = {0};
    int e = 0;
    if (!enc) { code_free(&code); return ERR_NOMEM; }
    sym_init(&s, &code, 0, br);
    for (uint64_t i = 0; i < enc_size; i++) {
        int ctx = 0;
        if (i > 128) {
            uint8_t b1 = enc[i - 1], b2 = enc[i - 2];
            int k1 = ((b1 | 32) >= 'a' && (b1 | 32) <= 'z') ? 0 : (b1 >= '0' && b1 <= '9') || b1 == '.' || b1 == ',' ? 1 :
                     b1 == 0 ? 2 : b1 == 1 ? 3 : b1 < 16 ? 4 : b1 == 255 ? 6 : b1 > 240 ? 5 : 7;
            int k2 = ((b2 | 32) >= 'a' && (b2 | 32) <= 'z') ? 0 : (b2 >= '0' && b2 <= '9') || b2 == '.' || b2 == ',' ? 1 :
                     b2 < 16 ? 2 : b2 > 240 ? 3 : 4;
            ctx = 1 + k1 + k2 * 8;
        }
        uint32_t v = sym_read(br, &s, ctx);
        if (v > 255 || s.bad) { e = ERR_INVALID; break; }
        enc[i] = (uint8_t)v;
        if ((i & 0xffff) == 0 && br_overrun(br)) { e = ERR_INVALID; break; }
    }
    int e2 = sym_finish(br, &s);
    if (!e) e = e2;
    code_free(&code);
    if (!e) e = icc_unpredict(enc, (size_t)enc_size, &out);
    free(enc);
    if (e) { free(out.p); return e; }
    m->icc = out.p; m->icc_len = out.n;
    return 0;
}

// ---------------------------------------------------------------------------
// Modular: MA tree, predictors, transforms

typedef struct { int w, h, hshift, vshift; int32_t *p; } Chan;   // stride = w

typedef struct {
    int32_t prop;        // < 0: leaf
    int32_t val;         // branch: split value; leaf: context
    int32_t left, right; // branch: children indices (left: property > val)
    int32_t pred, offset, mult;   // leaf
} Node;

typedef struct { Node *nodes; int n; Code code; } Tree;

static void tree_free(Tree *t) { free(t->nodes); code_free(&t->code); memset(t, 0, sizeof(*t)); }

static int read_tree(Br *br, int64_t max_size, Tree *t) {
    Code c;
    Sym s;
    int cap = 64, n = 0, nodes_left = 1, ctx_id = 0, e = 0;
    memset(t, 0, sizeof(*t));
    TRY(read_code(br, 6, 1, &c));
    sym_init(&s, &c, 0, br);
    Node *nodes = xmalloc((size_t)cap, sizeof(Node));
    if (!nodes) { code_free(&c); return ERR_NOMEM; }
    // breadth first: the children of node i are appended in order, their index is n + nodes_left
    int depth_limit = 2048, depth = 0, depth_end = 1;
    while (nodes_left-- > 0) {
        if (n == depth_end) { if (++depth > depth_limit) { e = ERR_INVALID; break; } depth_end += nodes_left + 1; }
        if (n == cap) {
            Node *p = realloc(nodes, sizeof(Node) * (size_t)(cap *= 2));
            if (!p) { e = ERR_NOMEM; break; }
            nodes = p;
        }
        Node *d = &nodes[n];
        uint32_t prop = sym_read(br, &s, 1);
        if (prop > 0) {
            d->prop = (int32_t)prop - 1;
            d->val = unpack_signed(sym_read(br, &s, 0));
            d->left = n + nodes_left + 1;
            d->right = n + nodes_left + 2;
            nodes_left += 2;
        } else {
            d->prop = -1;
            d->val = ctx_id++;
            d->pred = (int32_t)sym_read(br, &s, 2);
            d->offset = unpack_signed(sym_read(br, &s, 3));
            uint32_t shift = sym_read(br, &s, 4), mv = sym_read(br, &s, 5);
            if (d->pred > 13 || shift > 31 || ((uint64_t)mv + 1) << shift > 0x7fffffff) { e = ERR_INVALID; break; }
            d->mult = (int32_t)((mv + 1) << shift);
        }
        n++;
        if (n + nodes_left > max_size || s.bad || br_overrun(br)) { e = ERR_INVALID; break; }
    }
    int e2 = sym_finish(br, &s);
    code_free(&c);
    if (!e) e = e2;
    if (e) { free(nodes); return e; }
    t->nodes = nodes; t->n = n;
    if ((e = read_code(br, ctx_id, 1, &t->code))) { tree_free(t); return e; }
    return 0;
}

// weighted predictor (libjxl's weighted::State)
typedef struct { int p1, p2, p3[5], w[4]; } WpHeader;

typedef struct {
    WpHeader h;
    int w;
    uint32_t *perr[4];  // (w + 2) * 2 each (libjxl's types: the values wrap as there)
    int32_t *err;       // (w + 2) * 2
    int64_t pred[4], pr;
} Wp;

static const uint32_t DIVLOOKUP[64] = {
    16777216, 8388608, 5592405, 4194304, 3355443, 2796202, 2396745, 2097152, 1864135, 1677721, 1525201, 1398101,
    1290555, 1198372, 1118481, 1048576, 986895, 932067, 883011, 838860, 798915, 762600, 729444, 699050, 671088,
    645277, 621378, 599186, 578524, 559240, 541200, 524288, 508400, 493447, 479349, 466033, 453438, 441505, 430185,
    419430, 409200, 399457, 390167, 381300, 372827, 364722, 356962, 349525, 342392, 335544, 328965, 322638, 316551,
    310689, 305040, 299593, 294337, 289262, 284359, 279620, 275036, 270600, 266305, 262144,
};

static int wp_init(Wp *s, const WpHeader *h, int w) {
    memset(s, 0, sizeof(*s));
    s->h = *h; s->w = w;
    uint32_t *mem = xcalloc((size_t)(w + 2) * 2 * 5, sizeof(uint32_t));
    if (!mem) return ERR_NOMEM;
    for (int i = 0; i < 4; i++) s->perr[i] = mem + (size_t)(w + 2) * 2 * i;
    s->err = (int32_t *)(mem + (size_t)(w + 2) * 2 * 4);
    return 0;
}
static void wp_free(Wp *s) { free(s->perr[0]); s->perr[0] = NULL; }

static inline uint32_t wp_error_weight(uint64_t x, uint32_t maxweight) {
    int shift = floor_log2(x + 1) - 5;
    if (shift < 0) shift = 0;
    return 4 + ((maxweight * DIVLOOKUP[x >> shift]) >> shift);
}

// returns the prediction; *maxerr: property 15
static inline int64_t wp_predict(Wp *s, int x, int y, int64_t N, int64_t W, int64_t NE, int64_t NW, int64_t NN, int64_t *maxerr) {
    size_t xs = (size_t)s->w;
    size_t cur = y & 1 ? 0 : xs + 2, prev = y & 1 ? xs + 2 : 0;
    size_t pN = prev + (size_t)x, pNE = (size_t)x < xs - 1 ? pN + 1 : pN, pNW = x > 0 ? pN - 1 : pN;
    uint32_t wt[4];
    for (int i = 0; i < 4; i++) {
        uint64_t e = (uint32_t)(s->perr[i][pN] + s->perr[i][pNE] + s->perr[i][pNW]);
        wt[i] = wp_error_weight(e, (uint32_t)s->h.w[i]);
    }
    N *= 8; W *= 8; NE *= 8; NW *= 8; NN *= 8;
    int64_t teW = x == 0 ? 0 : s->err[cur + (size_t)x - 1], teN = s->err[pN], teNW = s->err[pNW], teNE = s->err[pNE];
    int64_t sumWN = teN + teW;
    int64_t p = teW;
    if (llabs(teN) > llabs(p)) p = teN;
    if (llabs(teNW) > llabs(p)) p = teNW;
    if (llabs(teNE) > llabs(p)) p = teNE;
    *maxerr = p;
    s->pred[0] = W + NE - N;
    s->pred[1] = N - (((sumWN + teNE) * s->h.p1) >> 5);
    s->pred[2] = W - (((sumWN + teNW) * s->h.p2) >> 5);
    s->pred[3] = N - ((teNW * s->h.p3[0] + teN * s->h.p3[1] + teNE * s->h.p3[2] + (NN - N) * s->h.p3[3] + (NW - W) * s->h.p3[4]) >> 5);
    uint32_t wsum = wt[0] + wt[1] + wt[2] + wt[3];
    int logw = floor_log2(wsum);
    wsum = 0;
    for (int i = 0; i < 4; i++) { wt[i] >>= logw - 4; wsum += wt[i]; }
    uint64_t sum = (uint64_t)(int64_t)((wsum >> 1) - 1);
    for (int i = 0; i < 4; i++) sum += (uint64_t)s->pred[i] * wt[i];
    int64_t pred = (int64_t)(sum * DIVLOOKUP[wsum - 1]) >> 24;
    if (((teN ^ teW) | (teN ^ teNW)) <= 0) {
        int64_t mx = W > NE ? W : NE, mn = W < NE ? W : NE;
        if (N > mx) mx = N;
        if (N < mn) mn = N;
        pred = pred < mn ? mn : pred > mx ? mx : pred;
    }
    s->pr = pred;
    return (pred + 3) >> 3;
}

static inline void wp_update(Wp *s, int64_t val, int x, int y) {
    size_t xs = (size_t)s->w;
    size_t cur = y & 1 ? 0 : xs + 2, prev = y & 1 ? xs + 2 : 0;
    val *= 8;
    s->err[cur + (size_t)x] = (int32_t)(s->pr - val);
    for (int i = 0; i < 4; i++) {
        uint32_t e = (uint32_t)((llabs(s->pred[i] - val) + 3) >> 3);
        s->perr[i][cur + (size_t)x] = e;
        s->perr[i][prev + (size_t)x + 1] += e;
    }
}

static inline int64_t clamped_gradient(int64_t n, int64_t w, int64_t l) {
    int64_t m = n < w ? n : w, M = n < w ? w : n, g = n + w - l;
    return l < m ? M : l > M ? m : g;
}

static inline int64_t predict_one(int pred, int64_t left, int64_t top, int64_t toptop, int64_t topleft,
                                  int64_t topright, int64_t leftleft, int64_t toprightright, int64_t wp) {
    switch (pred) {
    case 0: return 0;
    case 1: return left;
    case 2: return top;
    case 3: return (left + top) / 2;
    case 4: { int64_t p = left + top - topleft; return llabs(p - left) < llabs(p - top) ? left : top; }
    case 5: return clamped_gradient(left, top, topleft);
    case 6: return wp;
    case 7: return topright;
    case 8: return topleft;
    case 9: return leftleft;
    case 10: return (left + topleft) / 2;
    case 11: return (topleft + top) / 2;
    case 12: return (top + topright) / 2;
    default: return (6 * top - 2 * toptop + 7 * left + leftleft + toprightright + 3 * topright + 8) / 16;
    }
}

typedef struct {
    int nchan, nmeta;
    Chan *ch;
    int bits;       // bit depth for the palette's implicit colours
    WpHeader wp;
    int ntr;
    struct Transform { int id; int begin_c, rct_type, num_c, nb_colours, nb_deltas, d_pred; int horizontal, in_place; } *tr;
} Modular;

enum { TR_RCT = 0, TR_PALETTE = 1, TR_SQUEEZE = 2 };

static void modular_free(Modular *m) {
    if (m->ch) for (int i = 0; i < m->nchan; i++) free(m->ch[i].p);
    free(m->ch); free(m->tr);
    memset(m, 0, sizeof(*m));
}

// The tree for one channel: the branches on properties fixed in it (0: the channel, 1: the stream,
// those of previous channels it does not have: 0) decided, two levels in a node (libjxl's
// FlatDecisionNode): the node's comparison picks one of its two children, whose comparisons were
// made at the same time, and that one of the four grandchildren at child .. child + 3 (left first:
// property > value). A child that is a leaf fills both its grandchildren. A leaf: prop0 -1, split0
// the index of the original node.
typedef struct { int32_t prop0, split0, prop1[2], split1[2], child; } FNode;

static int tree_resolve(const Tree *t, int i, int ci, uint32_t stream_id, int nprops) {
    for (int steps = 0; t->nodes[i].prop >= 0 && steps < t->n; steps++) {
        const Node *n = &t->nodes[i];
        int32_t pv;
        if (n->prop == 0) pv = ci;
        else if (n->prop == 1) pv = (int32_t)stream_id;
        else if (n->prop >= nprops) pv = 0;
        else break;
        i = pv > n->val ? n->left : n->right;
    }
    return i;
}

static int tree_specialise(const Tree *t, int ci, uint32_t stream_id, int nprops, FNode *out, int cap, int *queue, int *count) {
    int n = 1, head = 0, tail = 0;
    queue[tail++] = tree_resolve(t, 0, ci, stream_id, nprops);   // (queue: the source node of out[head ...])
    while (head < tail) {
        int src = queue[head], d = head++;
        const Node *nd = &t->nodes[src];
        if (nd->prop < 0) { out[d] = (FNode){-1, src, {0, 0}, {0, 0}, 0}; continue; }
        if (n + 4 > cap) return ERR_INVALID;
        FNode f = {nd->prop, nd->val, {0, 0}, {0, 0}, n};
        for (int k = 0; k < 2; k++) {
            int c = tree_resolve(t, k ? nd->right : nd->left, ci, stream_id, nprops);
            const Node *cn = &t->nodes[c];
            if (cn->prop < 0) {
                queue[tail++] = c; queue[tail++] = c;
            } else {
                f.prop1[k] = cn->prop; f.split1[k] = cn->val;
                queue[tail++] = tree_resolve(t, cn->left, ci, stream_id, nprops);
                queue[tail++] = tree_resolve(t, cn->right, ci, stream_id, nprops);
            }
        }
        out[d] = f;
        n += 4;
    }
    *count = n;
    return 0;
}

// decodes one channel with the MA tree; fast: ANS without LZ77 (the reader's state in locals)
static inline __attribute__((always_inline)) int channel_pixels(Br *brp, Sym *sp, int fast, const Tree *t, const FNode *tn,
                                                                const Modular *m, int ci, int use_wp, int max_prop,
                                                                const int *refs, int nref) {
    Chan *c = &m->ch[ci];
    int w = c->w, h = c->h;
    int32_t *p = c->p;
    const Code *code = sp->code;
    const uint8_t *map = code->map;
    const Alias *alias = code->alias;
    const HybridCfg *cfg = code->cfg;
    int la = code->log_alpha, bad = 0;
    Br lbr = *brp, *br = fast ? &lbr : brp;
    uint32_t st = sp->state;
    Wp wp;
    if (use_wp) TRY(wp_init(&wp, &m->wp, w));
    int32_t props[16 + 4 * 64];   // (int32 as libjxl's: they wrap)
    memset(props, 0, sizeof props);
    int single = tn[0].prop0 < 0, e = 0;
    for (int y = 0; y < h && !e; y++) {
        int32_t *row = p + (size_t)y * w;
        const int32_t *prow = y ? row - w : NULL, *pprow = y > 1 ? row - 2 * w : NULL;
        int32_t prev_grad = 0;
        props[2] = y;
        for (int x = 0; x < w; x++) {
            int64_t left = x ? row[x - 1] : y ? prow[x] : 0;
            int64_t top = y ? prow[x] : left;
            int64_t topleft = x && y ? prow[x - 1] : left;
            int64_t topright = x + 1 < w && y ? prow[x + 1] : top;
            int64_t leftleft = x > 1 ? row[x - 2] : left;
            int64_t toptop = y > 1 ? pprow[x] : top;
            int64_t toprightright = x + 2 < w && y ? prow[x + 2] : topright;
            int64_t wpred = 0, maxerr = 0;
            if (use_wp) wpred = wp_predict(&wp, x, y, top, left, topright, topleft, toptop, &maxerr);
            const FNode *n = tn;
            if (!single) {
                props[3] = x;
                props[4] = (int32_t)llabs(top); props[5] = (int32_t)llabs(left);
                props[6] = (int32_t)top; props[7] = (int32_t)left;
                props[8] = (int32_t)(left - prev_grad);
                props[9] = (int32_t)(left + top - topleft);
                props[10] = (int32_t)(left - topleft); props[11] = (int32_t)(topleft - top); props[12] = (int32_t)(top - topright);
                props[13] = (int32_t)(top - toptop); props[14] = (int32_t)(left - leftleft);
                props[15] = (int32_t)maxerr;
                if (max_prop >= 16) {
                    for (int k = 0; k < nref && 16 + 4 * k <= max_prop; k++) {
                        const Chan *r = &m->ch[refs[k]];
                        const int32_t *rr = r->p + (size_t)y * w;
                        int64_t v = rr[x];
                        int64_t vl = x ? rr[x - 1] : 0;
                        int64_t vt = y ? rr[x - w] : vl;
                        int64_t vtl = x && y ? rr[x - 1 - w] : vl;
                        int64_t pr = clamped_gradient(vl, vt, vtl);
                        props[16 + 4 * k] = (int32_t)llabs(v); props[17 + 4 * k] = (int32_t)v;
                        props[18 + 4 * k] = (int32_t)llabs(v - pr); props[19 + 4 * k] = (int32_t)(v - pr);
                    }
                }
                while (n->prop0 >= 0) {
                    int k = props[n->prop0] <= n->split0;
                    n = tn + n->child + 2 * k + (props[n->prop1[k]] <= n->split1[k]);
                }
                prev_grad = props[9];
            }
            const Node *leaf = &t->nodes[n->split0];
            uint32_t tok = fast ? ans_fast(br, map, alias, la, cfg, &st, &bad, leaf->val) : sym_read(br, sp, leaf->val);
            int64_t v = (int64_t)unpack_signed(tok) * leaf->mult + leaf->offset +
                        predict_one(leaf->pred, left, top, toptop, topleft, topright, leftleft, toprightright, wpred);
            row[x] = (int32_t)v;
            if (use_wp) wp_update(&wp, row[x], x, y);
        }
        if (bad || sp->bad || ((y & 15) == 15 && br_overrun(br))) e = ERR_INVALID;
    }
    if (use_wp) wp_free(&wp);
    if (fast) { *brp = lbr; sp->state = st; sp->bad |= bad; }
    return e;
}

// A tree that branches on property 9 only (within -512 .. 510), every leaf the gradient predictor (libjxl's
// usual LF image): the leaf from a table of the clamped property, as libjxl's is_gradient_only path.
enum { GRAD_RANGE = 512 };
typedef struct { int32_t ctx, mult, offset; } GradLeaf;
static int gradient_table(const Tree *t, const FNode *tn, int count, GradLeaf *lut) {
    for (int i = 0; i < count; i++) {
        const FNode *n = &tn[i];
        if (n->prop0 < 0) { if (t->nodes[n->split0].pred != 5) return 0; continue; }
        if (n->prop0 != 9 || n->split0 < -GRAD_RANGE || n->split0 >= GRAD_RANGE - 1) return 0;
        for (int k = 0; k < 2; k++)   // (0, 0: a leaf child)
            if ((n->prop1[k] || n->split1[k]) && (n->prop1[k] != 9 || n->split1[k] < -GRAD_RANGE || n->split1[k] >= GRAD_RANGE - 1)) return 0;
    }
    for (int v = -GRAD_RANGE; v < GRAD_RANGE; v++) {
        const FNode *n = tn;
        while (n->prop0 >= 0) {
            int k = v <= n->split0;
            n = tn + n->child + 2 * k + (n->prop1[k] == 9 ? v <= n->split1[k] : 0);
        }
        const Node *leaf = &t->nodes[n->split0];
        lut[v + GRAD_RANGE] = (GradLeaf){leaf->val, leaf->mult, leaf->offset};
    }
    return 1;
}

static inline __attribute__((always_inline)) int channel_gradient(Br *brp, Sym *sp, int fast, const GradLeaf *lut, Chan *c) {
    int w = c->w, h = c->h;
    int32_t *p = c->p;
    const Code *code = sp->code;
    const uint8_t *map = code->map;
    const Alias *alias = code->alias;
    const HybridCfg *cfg = code->cfg;
    int la = code->log_alpha, bad = 0, e = 0;
    Br lbr = *brp, *br = fast ? &lbr : brp;
    uint32_t st = sp->state;
    for (int y = 0; y < h && !e; y++) {
        int32_t *row = p + (size_t)y * w;
        const int32_t *prow = y ? row - w : NULL;
        for (int x = 0; x < w; x++) {
            int64_t left = x ? row[x - 1] : y ? prow[x] : 0;
            int64_t top = y ? prow[x] : left;
            int64_t topleft = x && y ? prow[x - 1] : left;
            int32_t g = (int32_t)(left + top - topleft);   // (property 9: int32 as libjxl's)
            const GradLeaf *lf = &lut[(g < -GRAD_RANGE ? -GRAD_RANGE : g > GRAD_RANGE - 1 ? GRAD_RANGE - 1 : g) + GRAD_RANGE];
            uint32_t tok = fast ? ans_fast(br, map, alias, la, cfg, &st, &bad, lf->ctx) : sym_read(br, sp, lf->ctx);
            row[x] = (int32_t)((int64_t)unpack_signed(tok) * lf->mult + lf->offset + clamped_gradient(left, top, topleft));
        }
        if (bad || sp->bad || ((y & 15) == 15 && br_overrun(br))) e = ERR_INVALID;
    }
    if (fast) { *brp = lbr; sp->state = st; sp->bad |= bad; }
    return e;
}

static int decode_channel(Br *br, Sym *s, const Tree *t, const Modular *m, int ci, uint32_t stream_id) {
    const Chan *c = &m->ch[ci];
    // the previous channels of the same size and shifts (properties 16 ...)
    int refs[64], nref = 0;
    for (int j = ci - 1; j >= 0 && nref < 64; j--) {
        const Chan *r = &m->ch[j];
        if (r->w == c->w && r->h == c->h && r->hshift == c->hshift && r->vshift == c->vshift) refs[nref++] = j;
    }
    int cap = 2 * t->n + 8, count = 0;   // (each node holds two levels; a leaf may fill two places)
    FNode *tn = xmalloc((size_t)cap, sizeof(FNode));
    int *queue = xmalloc((size_t)cap, sizeof(int));
    int e = !tn || !queue ? ERR_NOMEM : tree_specialise(t, ci, stream_id, 16 + 4 * nref, tn, cap, queue, &count);
    free(queue);
    if (e) { free(tn); return e; }
    int use_wp = 0, max_prop = 0;   // (in the channel's tree)
    for (int i = 0; i < count; i++) {
        if (tn[i].prop0 >= 0) {
            int pr[3] = {tn[i].prop0, tn[i].prop1[0], tn[i].prop1[1]};
            for (int k = 0; k < 3; k++) {
                if (pr[k] == 15) use_wp = 1;
                if (pr[k] > max_prop) max_prop = pr[k];
            }
        } else if (t->nodes[tn[i].split0].pred == 6) {
            use_wp = 1;
        }
    }
    int fast = !s->code->use_prefix && !s->code->lz77;
    GradLeaf *lut = !use_wp && max_prop <= 9 && count > 1 ? xmalloc(2 * GRAD_RANGE, sizeof(GradLeaf)) : NULL;
    if (lut && gradient_table(t, tn, count, lut))
        e = fast ? channel_gradient(br, s, 1, lut, &m->ch[ci]) : channel_gradient(br, s, 0, lut, &m->ch[ci]);
    else
        e = fast ? channel_pixels(br, s, 1, t, tn, m, ci, use_wp, max_prop, refs, nref)
                 : channel_pixels(br, s, 0, t, tn, m, ci, use_wp, max_prop, refs, nref);
    free(lut);
    free(tn);
    return e;
}

// the transforms' effect on the channel list (MetaApply)
static int meta_apply(Modular *m, struct Transform *t) {
    if (t->id == TR_RCT) {
        int b = t->begin_c;
        if (b < 0 || b + 3 > m->nchan) return ERR_INVALID;
        for (int i = 1; i < 3; i++)
            if (m->ch[b + i].w != m->ch[b].w || m->ch[b + i].h != m->ch[b].h ||
                m->ch[b + i].hshift != m->ch[b].hshift || m->ch[b + i].vshift != m->ch[b].vshift) return ERR_INVALID;
        return 0;
    }
    if (t->id == TR_PALETTE) {
        int b = t->begin_c, e = b + t->num_c - 1;
        if (b < 0 || t->num_c < 1 || e >= m->nchan) return ERR_INVALID;
        for (int i = b + 1; i <= e; i++)
            if (m->ch[i].w != m->ch[b].w || m->ch[i].h != m->ch[b].h ||
                m->ch[i].hshift != m->ch[b].hshift || m->ch[i].vshift != m->ch[b].vshift) return ERR_INVALID;
        if (b >= m->nmeta) m->nmeta++;
        else { if (e >= m->nmeta) return ERR_INVALID; m->nmeta += 2 - t->num_c; }
        Chan *nc = xmalloc((size_t)(m->nchan - t->num_c + 2), sizeof(Chan));
        if (!nc) return ERR_NOMEM;
        nc[0].w = t->nb_colours + t->nb_deltas; nc[0].h = t->num_c; nc[0].hshift = nc[0].vshift = -1; nc[0].p = NULL;
        memcpy(nc + 1, m->ch, sizeof(Chan) * (size_t)(b + 1));
        memcpy(nc + b + 2, m->ch + e + 1, sizeof(Chan) * (size_t)(m->nchan - e - 1));
        free(m->ch);
        m->ch = nc;
        m->nchan = m->nchan - t->num_c + 2;
        return 0;
    }
    return ERR_INVALID;
}

static int read_squeeze(Br *br, Modular *m, int *cap) {   // the squeeze steps (or the default ones) as transforms
    uint32_t num = br_u32(br, 0, 0, 1, 4, 9, 6, 41, 8);
    struct Transform sq[256];
    int n = 0;
    if (num) {
        for (uint32_t i = 0; i < num && i < 256; i++) {
            sq[n].id = TR_SQUEEZE;
            sq[n].horizontal = (int)br_u(br, 1);
            sq[n].in_place = (int)br_u(br, 1);
            sq[n].begin_c = (int)br_u32(br, 0, 3, 8, 6, 72, 10, 1096, 13);
            sq[n].num_c = (int)br_u32(br, 1, 0, 2, 0, 3, 0, 4, 4);
            n++;
        }
        if (num > 256) return ERR_UNSUP;
    } else {   // DefaultSqueezeParameters
        if (m->nmeta >= m->nchan) return ERR_INVALID;
        int nb = m->nchan - m->nmeta;
        int w = m->ch[m->nmeta].w, h = m->ch[m->nmeta].h;
        int wide = w > h;
        if (nb > 2 && m->ch[m->nmeta + 1].w == w && m->ch[m->nmeta + 1].h == h) {
            sq[n++] = (struct Transform){TR_SQUEEZE, m->nmeta + 1, 0, 2, 0, 0, 0, 1, 0};
            sq[n++] = (struct Transform){TR_SQUEEZE, m->nmeta + 1, 0, 2, 0, 0, 0, 0, 0};
        }
        struct Transform p = {TR_SQUEEZE, m->nmeta, 0, nb, 0, 0, 0, 0, 1};
        if (!wide && h > 8) { p.horizontal = 0; sq[n++] = p; h = (h + 1) / 2; }
        while ((w > 8 || h > 8) && n < 250) {
            if (w > 8) { p.horizontal = 1; sq[n++] = p; w = (w + 1) / 2; }
            if (h > 8) { p.horizontal = 0; sq[n++] = p; h = (h + 1) / 2; }
        }
    }
    // MetaSqueeze
    for (int i = 0; i < n; i++) {
        struct Transform *t = &sq[i];
        int b = t->begin_c, e = b + t->num_c - 1;
        if (b < 0 || e >= m->nchan || e < b) return ERR_INVALID;
        if (b < m->nmeta) {
            if (e >= m->nmeta || !t->in_place) return ERR_INVALID;
            m->nmeta += t->num_c;
        }
        int offset = t->in_place ? e + 1 : m->nchan;
        Chan *nc = xmalloc((size_t)(m->nchan + t->num_c), sizeof(Chan));
        if (!nc) return ERR_NOMEM;
        memcpy(nc, m->ch, sizeof(Chan) * (size_t)m->nchan);
        free(m->ch); m->ch = nc;
        for (int c = b; c <= e; c++) {
            Chan *ch = &m->ch[c];
            if (ch->hshift > 30 || ch->vshift > 30 || !ch->w || !ch->h) return ERR_INVALID;
            Chan r = {0};
            if (t->horizontal) {
                r.w = ch->w - (ch->w + 1) / 2; r.h = ch->h;
                ch->w = (ch->w + 1) / 2;
                if (ch->hshift >= 0) ch->hshift++;
            } else {
                r.h = ch->h - (ch->h + 1) / 2; r.w = ch->w;
                ch->h = (ch->h + 1) / 2;
                if (ch->vshift >= 0) ch->vshift++;
            }
            r.hshift = ch->hshift; r.vshift = ch->vshift;
            int at = offset + (c - b);
            memmove(m->ch + at + 1, m->ch + at, sizeof(Chan) * (size_t)(m->nchan - at));
            m->ch[at] = r;
            m->nchan++;
        }
    }
    // append to the transform list
    if (m->ntr + n > *cap) {
        int nc = m->ntr + n + 8;
        struct Transform *p = realloc(m->tr, sizeof(*p) * (size_t)nc);
        if (!p) return ERR_NOMEM;
        m->tr = p; *cap = nc;
    }
    if (n) memcpy(m->tr + m->ntr, sq, sizeof(*sq) * (size_t)n);
    m->ntr += n;
    return 0;
}

// GroupHeader: the transforms and the tree (if not the global one). m->ch holds the channel sizes
// before the transforms; after this, those of the coded channels (not yet allocated).
static int modular_header(Br *br, Modular *m, const Tree *global, Tree *local, const Tree **tree) {
    int use_global = (int)br_u(br, 1);
    if (br_u(br, 1)) {   // default WP
        WpHeader d = {16, 10, {7, 7, 7, 0, 0}, {13, 12, 12, 12}};
        m->wp = d;
    } else {
        m->wp.p1 = (int)br_u(br, 5); m->wp.p2 = (int)br_u(br, 5);
        for (int i = 0; i < 5; i++) m->wp.p3[i] = (int)br_u(br, 5);
        for (int i = 0; i < 4; i++) m->wp.w[i] = (int)br_u(br, 4);
    }
    uint32_t ntr = br_u32(br, 0, 0, 1, 0, 2, 4, 18, 8);
    int cap = 0;
    m->ntr = 0;
    for (uint32_t i = 0; i < ntr; i++) {
        int id = (int)br_u(br, 2);
        if (id == TR_SQUEEZE) {
            TRY(read_squeeze(br, m, &cap));
            continue;
        }
        if (id > 2) return ERR_INVALID;
        if (m->ntr == cap) {
            int nc = cap * 2 + 8;
            struct Transform *p = realloc(m->tr, sizeof(*p) * (size_t)nc);
            if (!p) return ERR_NOMEM;
            m->tr = p; cap = nc;
        }
        struct Transform *t = &m->tr[m->ntr++];
        memset(t, 0, sizeof(*t));
        t->id = id;
        t->begin_c = (int)br_u32(br, 0, 3, 8, 6, 72, 10, 1096, 13);
        if (id == TR_RCT) {
            t->rct_type = (int)br_u32(br, 6, 0, 0, 2, 2, 4, 10, 6);
            if (t->rct_type >= 42) return ERR_INVALID;
        } else {
            t->num_c = (int)br_u32(br, 1, 0, 3, 0, 4, 0, 1, 13);
            t->nb_colours = (int)br_u32(br, 0, 8, 256, 10, 1280, 12, 5376, 16);
            t->nb_deltas = (int)br_u32(br, 0, 0, 1, 8, 257, 10, 1281, 16);
            t->d_pred = (int)br_u(br, 4);
            if (t->d_pred > 13) return ERR_INVALID;
        }
        TRY(meta_apply(m, t));
        if (m->nchan > 4096) return ERR_LIMIT;
    }
    if (br_overrun(br)) return ERR_INVALID;
    if (use_global) {
        if (!global || !global->nodes) return ERR_INVALID;
        *tree = global;
    } else {
        *tree = local;
    }
    return 0;
}

// The channels from `first` on that are decoded here: all of them, or (max_size > 0, the global
// stream) up to the first non-meta channel larger than a group.
static int modular_decode_channels(Br *br, Modular *m, const Tree *global, uint32_t stream_id, int max_size) {
    Tree local = {0};
    const Tree *tree;
    int e = modular_header(br, m, global, &local, &tree);
    if (e) return e;
    int64_t pixels = 0, dist_mult = 0, nch = 0;
    for (int i = 0; i < m->nchan; i++) {
        Chan *c = &m->ch[i];
        if (!c->w || !c->h) continue;
        if (max_size && i >= m->nmeta && (c->w > max_size || c->h > max_size)) break;
        if (c->w > dist_mult) dist_mult = c->w;
        pixels += (int64_t)c->w * c->h;
        nch++;
    }
    for (int i = 0; i < m->nchan; i++) {   // allocate every channel (the later ones are filled by the groups)
        Chan *c = &m->ch[i];
        if (c->w && c->h && !c->p && !(c->p = xcalloc((size_t)c->w * c->h, sizeof(int32_t)))) return ERR_NOMEM;
    }
    if (!nch) return 0;
    if (tree == &local) {
        int64_t max_tree = 1024 + pixels;
        if (max_tree > (1 << 20)) max_tree = 1 << 20;
        TRY(read_tree(br, max_tree, &local));
    }
    Sym s;
    sym_init(&s, &tree->code, (int)(dist_mult > (1 << 21) ? (1 << 21) : dist_mult), br);
    for (int i = 0; i < m->nchan && !e; i++) {
        Chan *c = &m->ch[i];
        if (!c->w || !c->h) continue;
        if (max_size && i >= m->nmeta && (c->w > max_size || c->h > max_size)) break;
        e = decode_channel(br, &s, tree, m, i, stream_id);
    }
    int e2 = sym_finish(br, &s);
    if (!e) e = e2;
    tree_free(&local);
    return e;
}

static inline int32_t palette_value(const int32_t *pal, int index, int c, int pal_size, int onerow, int bits) {
    static const int16_t DELTA[72][3] = {
        {0, 0, 0}, {4, 4, 4}, {11, 0, 0}, {0, 0, -13}, {0, -12, 0}, {-10, -10, -10},
        {-18, -18, -18}, {-27, -27, -27}, {-18, -18, 0}, {0, 0, -32}, {-32, 0, 0}, {-37, -37, -37},
        {0, -32, -32}, {24, 24, 45}, {50, 50, 50}, {-45, -24, -24}, {-24, -45, -45}, {0, -24, -24},
        {-34, -34, 0}, {-24, 0, -24}, {-45, -45, -24}, {64, 64, 64}, {-32, 0, -32}, {0, -32, 0},
        {-32, 0, 32}, {-24, -45, -24}, {45, 24, 45}, {24, -24, -45}, {-45, -24, 24}, {80, 80, 80},
        {64, 0, 0}, {0, 0, -64}, {0, -64, -64}, {-24, -24, 45}, {96, 96, 96}, {64, 64, 0},
        {45, -24, -24}, {34, -34, 0}, {112, 112, 112}, {24, -45, -45}, {45, 45, -24}, {0, -32, 32},
        {24, -24, 45}, {0, 96, 96}, {45, -24, 24}, {24, -45, -24}, {-24, -45, 24}, {0, -64, 0},
        {96, 0, 0}, {128, 128, 128}, {64, 0, 64}, {144, 144, 144}, {96, 96, 0}, {-36, -36, 36},
        {45, -24, -45}, {45, -45, -24}, {0, 0, -96}, {0, 128, 128}, {0, 96, 0}, {45, 24, -45},
        {-128, 0, 0}, {24, -45, 24}, {-45, 24, -45}, {64, 0, -64}, {64, -64, -64}, {96, 0, 96},
        {45, -45, 24}, {24, 45, -45}, {64, 64, -64}, {128, 128, 0}, {0, 0, -128}, {-24, 45, -45},
    };
    if (index < 0) {
        if (c >= 3) return 0;
        index = -(index + 1);
        index %= 1 + 2 * 71;
        int32_t r = DELTA[(index + 1) >> 1][c] * (index & 1 ? 1 : -1);
        if (bits > 8) r *= 1 << (bits - 8);
        return r;
    }
    if (index >= pal_size && index < pal_size + 64) {
        if (c >= 3) return 0;
        index -= pal_size;
        index >>= c * 2;
        return (int32_t)(((uint64_t)(index % 4) * (((uint64_t)1 << bits) - 1)) >> 2) + (1 << (bits > 3 ? bits - 3 : 0));
    }
    if (index >= pal_size + 64) {
        if (c >= 3) return 0;
        index -= pal_size + 64;
        if (c == 1) index /= 5; else if (c == 2) index /= 25;
        return (int32_t)(((uint64_t)(index % 5) * (((uint64_t)1 << bits) - 1)) >> 2);
    }
    return pal[(size_t)c * onerow + (size_t)index];
}

static int inv_palette(Modular *m, const struct Transform *t) {
    if (m->nmeta < 1) return ERR_INVALID;
    int nb = m->ch[0].h, c0 = t->begin_c + 1;
    if (c0 >= m->nchan || nb < 1) return ERR_INVALID;
    int w = m->ch[c0].w, h = m->ch[c0].h;
    // insert nb - 1 channels after c0
    Chan *nc = xmalloc((size_t)(m->nchan + nb - 1), sizeof(Chan));
    if (!nc) return ERR_NOMEM;
    memcpy(nc, m->ch, sizeof(Chan) * (size_t)(c0 + 1));
    memcpy(nc + c0 + nb, m->ch + c0 + 1, sizeof(Chan) * (size_t)(m->nchan - c0 - 1));
    for (int i = 1; i < nb; i++) {
        nc[c0 + i] = (Chan){w, h, m->ch[c0].hshift, m->ch[c0].vshift, NULL};
        if (w && h && !(nc[c0 + i].p = xmalloc((size_t)w * h, sizeof(int32_t)))) {
            for (int k = 1; k < i; k++) free(nc[c0 + k].p);
            free(nc);
            return ERR_NOMEM;
        }
    }
    free(m->ch);
    m->ch = nc;
    m->nchan += nb - 1;
    const Chan *pal = &m->ch[0];
    int bits = m->bits < 24 ? m->bits : 24;
    if (w && h) {
        int32_t *idx = m->ch[c0].p;
        if (t->nb_deltas == 0 && t->d_pred == 0) {
            int32_t *tmp = nb > 1 ? xmalloc((size_t)w, sizeof(int32_t)) : NULL;
            if (nb > 1 && !tmp) return ERR_NOMEM;
            for (int y = 0; y < h; y++) {
                int32_t *ir = idx + (size_t)y * w;
                if (nb == 1) {
                    for (int x = 0; x < w; x++) {
                        int i = ir[x] < 0 ? 0 : ir[x] >= pal->w ? pal->w - 1 : ir[x];
                        ir[x] = pal->w ? palette_value(pal->p, i, 0, pal->w, pal->w, bits) : 0;
                    }
                } else {
                    memcpy(tmp, ir, sizeof(int32_t) * (size_t)w);
                    for (int c = 0; c < nb; c++) {
                        int32_t *o = m->ch[c0 + c].p + (size_t)y * w;
                        for (int x = 0; x < w; x++) o[x] = palette_value(pal->p, tmp[x], c, pal->w, pal->w, bits);
                    }
                }
            }
            free(tmp);
        } else {
            int32_t *indices = xmalloc((size_t)w * h, sizeof(int32_t));
            if (!indices) return ERR_NOMEM;
            memcpy(indices, idx, sizeof(int32_t) * (size_t)w * h);
            for (int c = 0; c < nb; c++) {
                int32_t *p = m->ch[c0 + c].p;
                Wp wp;
                int use_wp = t->d_pred == 6;
                if (use_wp && wp_init(&wp, &m->wp, w)) { free(indices); return ERR_NOMEM; }
                for (int y = 0; y < h; y++) {
                    int32_t *row = p + (size_t)y * w;
                    const int32_t *prow = y ? row - w : NULL, *pprow = y > 1 ? row - 2 * w : NULL;
                    for (int x = 0; x < w; x++) {
                        int index = indices[(size_t)y * w + x];
                        int64_t v = palette_value(pal->p, index, c, pal->w, pal->w, bits);
                        if (index < t->nb_deltas) {
                            int64_t left = x ? row[x - 1] : y ? prow[x] : 0;
                            int64_t top = y ? prow[x] : left;
                            int64_t topleft = x && y ? prow[x - 1] : left;
                            int64_t topright = x + 1 < w && y ? prow[x + 1] : top;
                            int64_t leftleft = x > 1 ? row[x - 2] : left;
                            int64_t toptop = y > 1 ? pprow[x] : top;
                            int64_t trr = x + 2 < w && y ? prow[x + 2] : topright;
                            int64_t wpred = 0, me;
                            if (use_wp) wpred = wp_predict(&wp, x, y, top, left, topright, topleft, toptop, &me);
                            v += predict_one(t->d_pred, left, top, toptop, topleft, topright, leftleft, trr, wpred);
                        }   // (libjxl updates the WP errors with the last prediction made)
                        row[x] = (int32_t)v;
                        if (use_wp) wp_update(&wp, row[x], x, y);
                    }
                }
                if (use_wp) wp_free(&wp);
            }
            free(indices);
        }
    }
    if (c0 >= m->nmeta) m->nmeta--;
    else { if (m->nmeta < 2 - nb) return ERR_INVALID; m->nmeta -= 2 - nb; }
    free(m->ch[0].p);
    memmove(m->ch, m->ch + 1, sizeof(Chan) * (size_t)(m->nchan - 1));
    m->nchan--;
    return 0;
}

static void inv_rct(Modular *m, const struct Transform *t) {
    int b = t->begin_c, perm = t->rct_type / 7, type = t->rct_type % 7;
    Chan c0 = m->ch[b], c1 = m->ch[b + 1], c2 = m->ch[b + 2];
    size_t n = (size_t)c0.w * c0.h;
    if (type && c0.p) {
        int32_t *a = c0.p, *bb = c1.p, *c = c2.p;
        for (size_t i = 0; i < n; i++) {
            int32_t A = a[i], B = bb[i], C = c[i];
            if (type == 6) {
                int32_t tmp = (int32_t)((uint32_t)A - (uint32_t)(C >> 1));
                int32_t G = (int32_t)((uint32_t)C + (uint32_t)tmp);
                int32_t Bv = (int32_t)((uint32_t)tmp - (uint32_t)(B >> 1));
                int32_t R = (int32_t)((uint32_t)Bv + (uint32_t)B);
                a[i] = R; bb[i] = G; c[i] = Bv;
            } else {
                if (type & 1) C = (int32_t)((uint32_t)C + (uint32_t)A);
                if ((type >> 1) == 1) B = (int32_t)((uint32_t)B + (uint32_t)A);
                else if ((type >> 1) == 2) B = (int32_t)((uint32_t)B + (uint32_t)(((int64_t)A + C) >> 1));
                bb[i] = B; c[i] = C;
            }
        }
    }
    m->ch[b + perm % 3] = c0;
    m->ch[b + (perm + 1 + perm / 3) % 3] = c1;
    m->ch[b + (perm + 2 - perm / 3) % 3] = c2;
}

static inline int64_t smooth_tendency(int64_t B, int64_t a, int64_t n) {
    int64_t diff = 0;
    if (B >= a && a >= n) {
        diff = (4 * B - 3 * n - a + 6) / 12;
        if (diff - (diff & 1) > 2 * (B - a)) diff = 2 * (B - a) + 1;
        if (diff + (diff & 1) > 2 * (a - n)) diff = 2 * (a - n);
    } else if (B <= a && a <= n) {
        diff = (4 * B - 3 * n - a - 6) / 12;
        if (diff + (diff & 1) < 2 * (B - a)) diff = 2 * (B - a) - 1;
        if (diff - (diff & 1) < 2 * (a - n)) diff = 2 * (a - n);
    }
    return diff;
}

static int inv_squeeze_one(Modular *m, int c, int rc, int horizontal) {
    Chan *in = &m->ch[c], *res = &m->ch[rc];
    if (horizontal) {
        if (in->w != (in->w + res->w + 1) / 2 || in->h != res->h) return ERR_INVALID;
        if (!res->w) { in->hshift--; return 0; }
        Chan out = {in->w + res->w, in->h, in->hshift - 1, in->vshift, NULL};
        if (out.w && out.h && !(out.p = xmalloc((size_t)out.w * out.h, sizeof(int32_t)))) return ERR_NOMEM;
        for (int y = 0; y < in->h; y++) {
            const int32_t *pr = res->p + (size_t)y * res->w, *pa = in->p + (size_t)y * in->w;
            int32_t *po = out.p + (size_t)y * out.w;
            for (int x = 0; x < res->w; x++) {
                int64_t avg = pa[x], navg = x + 1 < in->w ? pa[x + 1] : avg;
                int64_t left = x ? po[2 * x - 1] : avg;
                int64_t diff = pr[x] + smooth_tendency(left, avg, navg);
                int64_t A = avg + diff / 2;
                po[2 * x] = (int32_t)A;
                po[2 * x + 1] = (int32_t)(A - diff);
            }
            if (out.w & 1) po[out.w - 1] = pa[in->w - 1];
        }
        free(in->p);
        *in = out;
    } else {
        if (in->h != (in->h + res->h + 1) / 2 || in->w != res->w) return ERR_INVALID;
        if (!res->h) { in->vshift--; return 0; }
        Chan out = {in->w, in->h + res->h, in->hshift, in->vshift - 1, NULL};
        if (out.w && out.h && !(out.p = xmalloc((size_t)out.w * out.h, sizeof(int32_t)))) return ERR_NOMEM;
        int w = in->w;
        for (int y = 0; y < res->h; y++) {
            const int32_t *pr = res->p + (size_t)y * w, *pa = in->p + (size_t)y * w;
            const int32_t *pn = in->p + (size_t)(y + 1 < in->h ? y + 1 : y) * w;
            int32_t *po = out.p + (size_t)(2 * y) * w, *pno = po + w;
            const int32_t *pp = y ? po - w : pa;
            for (int x = 0; x < w; x++) {
                int64_t avg = pa[x], navg = pn[x], top = pp[x];
                int64_t diff = pr[x] + smooth_tendency(top, avg, navg);
                int64_t o = avg + diff / 2;
                po[x] = (int32_t)o;
                pno[x] = (int32_t)(o - diff);
            }
        }
        if (out.h & 1) memcpy(out.p + (size_t)(out.h - 1) * w, in->p + (size_t)(in->h - 1) * w, sizeof(int32_t) * (size_t)w);
        free(in->p);
        *in = out;
    }
    return 0;
}

static int inv_squeeze(Modular *m, const struct Transform *t) {
    int b = t->begin_c, e = b + t->num_c - 1;
    if (b < 0 || e >= m->nchan) return ERR_INVALID;
    int offset = t->in_place ? e + 1 : m->nchan + b - e - 1;
    if (b < m->nmeta) m->nmeta -= t->num_c;
    for (int c = b; c <= e; c++) {
        int rc = offset + c - b;
        if (rc >= m->nchan || m->ch[c].w < m->ch[rc].w || m->ch[c].h < m->ch[rc].h) return ERR_INVALID;
        TRY(inv_squeeze_one(m, c, rc, t->horizontal));
    }
    for (int i = 0; i < t->num_c; i++) free(m->ch[offset + i].p);
    memmove(m->ch + offset, m->ch + offset + t->num_c, sizeof(Chan) * (size_t)(m->nchan - offset - t->num_c));
    m->nchan -= t->num_c;
    return 0;
}

static int modular_inverse(Modular *m) {
    for (int i = m->ntr - 1; i >= 0; i--) {
        const struct Transform *t = &m->tr[i];
        if (t->id == TR_RCT) inv_rct(m, t);
        else if (t->id == TR_PALETTE) TRY(inv_palette(m, t));
        else TRY(inv_squeeze(m, t));
    }
    m->ntr = 0;
    return 0;
}

// a complete Modular sub-image (LF coefficients, HF metadata, a quantisation table, a group's
// channels): sizes in ch[], decoded and its transforms undone
static int modular_image(Br *br, Modular *m, const Tree *global, uint32_t stream_id) {
    TRY(modular_decode_channels(br, m, global, stream_id, 0));
    return modular_inverse(m);
}

static int modular_alloc(Modular *m, int n, int bits) {
    memset(m, 0, sizeof(*m));
    if (!(m->ch = xcalloc((size_t)n, sizeof(Chan)))) return ERR_NOMEM;
    m->nchan = n; m->bits = bits;
    return 0;
}

// ---------------------------------------------------------------------------
// DCTs (J40: Perera & Liu's radix-2 DCT-II/III; the 2x2, 4x4, 4x8, Hornuss and AFV transforms).
// A coefficient block is always stored wide: rows = the smaller, columns = the larger size.

static const float HALF_SECANTS[256] = {
	0, 0, // unused
	0.54119610f, 1.30656296f, // n=1 for DCT-4
	0.50979558f, 0.60134489f, 0.89997622f, 2.56291545f, // n=2 for DCT-8
	// n=3 for DCT-16
	0.50241929f, 0.52249861f, 0.56694403f, 0.64682178f, 0.78815462f, 1.06067769f, 1.72244710f, 5.10114862f,
	// n=4 for DCT-32
	0.50060300f, 0.50547096f, 0.51544731f, 0.53104259f, 0.55310390f, 0.58293497f, 0.62250412f, 0.67480834f,
	0.74453627f, 0.83934965f, 0.97256824f, 1.16943993f, 1.48416462f, 2.05778101f, 3.40760842f, 10.1900081f,
	// n=5 for DCT-64
	0.50015064f, 0.50135845f, 0.50378873f, 0.50747117f, 0.51245148f, 0.51879271f, 0.52657732f, 0.53590982f,
	0.54692044f, 0.55976981f, 0.57465518f, 0.59181854f, 0.61155735f, 0.63423894f, 0.66031981f, 0.69037213f,
	0.72512052f, 0.76549416f, 0.81270209f, 0.86834472f, 0.93458360f, 1.01440826f, 1.11207162f, 1.23383274f,
	1.38929396f, 1.59397228f, 1.87467598f, 2.28205007f, 2.92462843f, 4.08461108f, 6.79675071f, 20.3738782f,
	// n=6 for DCT-128
	0.50003765f, 0.50033904f, 0.50094272f, 0.50185052f, 0.50306519f, 0.50459044f, 0.50643095f, 0.50859242f,
	0.51108159f, 0.51390633f, 0.51707566f, 0.52059987f, 0.52449054f, 0.52876071f, 0.53342493f, 0.53849944f,
	0.54400225f, 0.54995337f, 0.55637499f, 0.56329167f, 0.57073059f, 0.57872189f, 0.58729894f, 0.59649876f,
	0.60636246f, 0.61693573f, 0.62826943f, 0.64042034f, 0.65345190f, 0.66743520f, 0.68245013f, 0.69858665f,
	0.71594645f, 0.73464482f, 0.75481294f, 0.77660066f, 0.80017990f, 0.82574877f, 0.85353675f, 0.88381100f,
	0.91688445f, 0.95312587f, 0.99297296f, 1.03694904f, 1.08568506f, 1.13994868f, 1.20068326f, 1.26906117f,
	1.34655763f, 1.43505509f, 1.53699410f, 1.65559652f, 1.79520522f, 1.96181785f, 2.16395782f, 2.41416000f,
	2.73164503f, 3.14746219f, 3.71524274f, 4.53629094f, 5.82768838f, 8.15384860f, 13.5842903f, 40.7446881f,
	// n=7 for DCT-256
	0.50000941f, 0.50008472f, 0.50023540f, 0.50046156f, 0.50076337f, 0.50114106f, 0.50159492f, 0.50212529f,
	0.50273257f, 0.50341722f, 0.50417977f, 0.50502081f, 0.50594098f, 0.50694099f, 0.50802161f, 0.50918370f,
	0.51042817f, 0.51175599f, 0.51316821f, 0.51466598f, 0.51625048f, 0.51792302f, 0.51968494f, 0.52153769f,
	0.52348283f, 0.52552196f, 0.52765682f, 0.52988922f, 0.53222108f, 0.53465442f, 0.53719139f, 0.53983424f,
	0.54258533f, 0.54544717f, 0.54842239f, 0.55151375f, 0.55472418f, 0.55805673f, 0.56151465f, 0.56510131f,
	0.56882030f, 0.57267538f, 0.57667051f, 0.58080985f, 0.58509780f, 0.58953898f, 0.59413825f, 0.59890075f,
	0.60383188f, 0.60893736f, 0.61422320f, 0.61969575f, 0.62536172f, 0.63122819f, 0.63730265f, 0.64359303f,
	0.65010770f, 0.65685553f, 0.66384594f, 0.67108889f, 0.67859495f, 0.68637535f, 0.69444203f, 0.70280766f,
	0.71148577f, 0.72049072f, 0.72983786f, 0.73954355f, 0.74962527f, 0.76010172f, 0.77099290f, 0.78232026f,
	0.79410679f, 0.80637720f, 0.81915807f, 0.83247799f, 0.84636782f, 0.86086085f, 0.87599311f, 0.89180358f,
	0.90833456f, 0.92563200f, 0.94374590f, 0.96273078f, 0.98264619f, 1.00355728f, 1.02553551f, 1.04865941f,
	1.07301549f, 1.09869926f, 1.12581641f, 1.15448427f, 1.18483336f, 1.21700940f, 1.25117548f, 1.28751481f,
	1.32623388f, 1.36756626f, 1.41177723f, 1.45916930f, 1.51008903f, 1.56493528f, 1.62416951f, 1.68832855f,
	1.75804061f, 1.83404561f, 1.91722116f, 2.00861611f, 2.10949453f, 2.22139378f, 2.34620266f, 2.48626791f,
	2.64454188f, 2.82479140f, 3.03189945f, 3.27231159f, 3.55471533f, 3.89110779f, 4.29853753f, 4.80207601f,
	5.44016622f, 6.27490841f, 7.41356676f, 9.05875145f, 11.6446273f, 16.3000231f, 27.1639777f, 81.4878422f,
};
static const float LF2LLF_SCALES[64] = {
	0, // unused
	1.00000000f, // N=1, n=8
	0.50000000f, 0.55446868f, // N=2, n=16
	0.25000000f, 0.25644002f, 0.27723434f, 0.31763984f, // N=4, n=32
	// N=8, n=64
	0.12500000f, 0.12579419f, 0.12822001f, 0.13241272f, 0.13861717f, 0.14722207f, 0.15881992f, 0.17431123f,
	// N=16, n=128
	0.06250000f, 0.06259894f, 0.06289709f, 0.06339849f, 0.06411001f, 0.06504154f, 0.06620636f, 0.06762155f,
	0.06930858f, 0.07129412f, 0.07361103f, 0.07629973f, 0.07940996f, 0.08300316f, 0.08715562f, 0.09196277f,
	// N=32, n=256
	0.03125000f, 0.03126236f, 0.03129947f, 0.03136146f, 0.03144855f, 0.03156101f, 0.03169925f, 0.03186372f,
	0.03205500f, 0.03227376f, 0.03252077f, 0.03279691f, 0.03310318f, 0.03344071f, 0.03381077f, 0.03421478f,
	0.03465429f, 0.03513107f, 0.03564706f, 0.03620441f, 0.03680552f, 0.03745302f, 0.03814986f, 0.03889931f,
	0.03970498f, 0.04057091f, 0.04150158f, 0.04250201f, 0.04357781f, 0.04473525f, 0.04598138f, 0.04732417f,
};

#define SQRT2 1.4142135623730951f

typedef struct { int logw, logh; float *ptr; } View;
static inline View mkview(int logw, int logh, float *p) { View v = {logw, logh, p}; return v; }
static inline void view_copy(View *o, View in) {
    o->logw = in.logw; o->logh = in.logh;
    memcpy(o->ptr, in.ptr, sizeof(float) << (in.logw + in.logh));
}
static inline void view_transpose(View *o, View in) {
    float *op = o->ptr;
    o->logw = in.logh; o->logh = in.logw;
    for (int y = 0; y < (1 << in.logh); ++y)
        for (int x = 0; x < (1 << in.logw); ++x) op[x << in.logh | y] = in.ptr[y << in.logw | x];
}
static inline void oddeven_columns_to_halves(View *o, View in) {
    o->logw = in.logw; o->logh = in.logh;
    for (int y = 0; y < (1 << in.logh); ++y)
        for (int x = 0; x < (1 << in.logw); ++x) {
            int ox = ((x & 1) << (in.logw - 1)) | (x >> 1);
            o->ptr[y << in.logw | ox] = in.ptr[y << in.logw | x];
        }
}
static inline void oddeven_rows_to_halves(View *o, View in) {
    o->logw = in.logw; o->logh = in.logh;
    for (int y = 0; y < (1 << in.logh); ++y) {
        int oy = ((y & 1) << (in.logh - 1)) | (y >> 1);
        for (int x = 0; x < (1 << in.logw); ++x) o->ptr[oy << in.logw | x] = in.ptr[y << in.logw | x];
    }
}
#define AT(v, x, y) ((v).ptr + ((y) << (v).logw | (x)))

// t: log2 size; the transforms run over `rep` columns at once (the data is [i * rep + r])
typedef void (*DctHalf)(float *out, float *in, int t, int rep);

static void idct_core(float *out, float *in, int t, int rep, DctHalf half) {
    int N = 1 << t;
    for (int i = 0; i < N / 2; ++i) for (int r = 0; r < rep; ++r) out[i * rep + r] = in[i * 2 * rep + r];
    for (int r = 0; r < rep; ++r) out[N / 2 * rep + r] = SQRT2 * in[1 * rep + r];
    for (int i = 1; i < N / 2; ++i)
        for (int r = 0; r < rep; ++r) out[(N / 2 + i) * rep + r] = in[(i * 2 - 1) * rep + r] + in[(i * 2 + 1) * rep + r];
    half(in, out, t - 1, rep);
    half(in + N / 2 * rep, out + N / 2 * rep, t - 1, rep);
    for (int i = 0; i < N / 2; ++i) {
        float mult = HALF_SECANTS[N / 2 + i];
        for (int r = 0; r < rep; ++r) {
            float x = in[i * rep + r], y = in[(N / 2 + i) * rep + r];
            out[i * rep + r] = x + y * mult;
            out[(N - i - 1) * rep + r] = x - y * mult;
        }
    }
}
static void fdct_core(float *out, float *in, int t, int rep, DctHalf half) {
    int N = 1 << t;
    for (int i = 0; i < N / 2; ++i) {
        float mult = HALF_SECANTS[N / 2 + i];
        for (int r = 0; r < rep; ++r) {
            float x = in[i * rep + r], y = in[(N - i - 1) * rep + r];
            out[i * rep + r] = x + y;
            out[(N / 2 + i) * rep + r] = (x - y) * mult;
        }
    }
    half(in, out, t - 1, rep);
    half(in + N / 2 * rep, out + N / 2 * rep, t - 1, rep);
    for (int i = 0; i < N / 2; ++i) for (int r = 0; r < rep; ++r) out[i * 2 * rep + r] = in[i * rep + r];
    for (int r = 0; r < rep; ++r) out[1 * rep + r] = SQRT2 * in[N / 2 * rep + r] + in[(N / 2 + 1) * rep + r];
    for (int i = 1; i < N / 2 - 1; ++i)
        for (int r = 0; r < rep; ++r) out[(i * 2 + 1) * rep + r] = in[(N / 2 + i) * rep + r] + in[(N / 2 + i + 1) * rep + r];
    for (int r = 0; r < rep; ++r) out[(N - 1) * rep + r] = in[(N - 1) * rep + r];
}
static void dct2(float *out, float *in, int t, int rep) {
    (void)t;
    for (int r = 0; r < rep; ++r) { float x = in[r], y = in[rep + r]; out[r] = x + y; out[rep + r] = x - y; }
}
static void idct_rec(float *out, float *in, int t, int rep) { idct_core(out, in, t, rep, t == 2 ? dct2 : idct_rec); }
static void fdct_rec(float *out, float *in, int t, int rep) { fdct_core(out, in, t, rep, t == 2 ? dct2 : fdct_rec); }

// the same 8 columns at a time (rep = 8 * rv)
typedef void (*DctHalfV)(vfu *out, vfu *in, int t, int rv);
static inline __attribute__((always_inline)) void idct_core_v(vfu *out, vfu *in, int t, int rv, DctHalfV half) {
    int N = 1 << t;
    for (int i = 0; i < N / 2; ++i) for (int r = 0; r < rv; ++r) out[i * rv + r] = in[i * 2 * rv + r];
    for (int r = 0; r < rv; ++r) out[N / 2 * rv + r] = SQRT2 * in[1 * rv + r];
    for (int i = 1; i < N / 2; ++i)
        for (int r = 0; r < rv; ++r) out[(N / 2 + i) * rv + r] = in[(i * 2 - 1) * rv + r] + in[(i * 2 + 1) * rv + r];
    half(in, out, t - 1, rv);
    half(in + N / 2 * rv, out + N / 2 * rv, t - 1, rv);
    for (int i = 0; i < N / 2; ++i) {
        float mult = HALF_SECANTS[N / 2 + i];
        for (int r = 0; r < rv; ++r) {
            vf x = in[i * rv + r], y = in[(N / 2 + i) * rv + r] * mult;
            out[i * rv + r] = x + y;
            out[(N - i - 1) * rv + r] = x - y;
        }
    }
}
static void dct2_v(vfu *out, vfu *in, int t, int rv) {
    (void)t;
    for (int r = 0; r < rv; ++r) { vf x = in[r], y = in[rv + r]; out[r] = x + y; out[rv + r] = x - y; }
}
static void idct8_h(vfu *out, vfu *in, int t, int rv);
static void idct16_h(vfu *out, vfu *in, int t, int rv) { (void)t; idct_core_v(out, in, 4, rv, idct8_h); }
static void idct32_h(vfu *out, vfu *in, int t, int rv) { (void)t; idct_core_v(out, in, 5, rv, idct16_h); }
static void idct_rec_v(vfu *out, vfu *in, int t, int rv) {
    if (t == 3) idct8_h(out, in, 3, rv);
    else if (t == 4) idct16_h(out, in, 4, rv);
    else if (t == 5) idct32_h(out, in, 5, rv);
    else idct_core_v(out, in, t, rv, t == 2 ? dct2_v : idct_rec_v);
}

// in is used as scratch
static void idct1d(float *out, float *in, int t, int rep) {
    if (t <= 0) memcpy(out, in, sizeof(float) * (size_t)rep);
    else if (!(rep & 7)) { if (t == 1) dct2_v((vfu *)out, (vfu *)in, 1, rep >> 3); else idct_rec_v((vfu *)out, (vfu *)in, t, rep >> 3); }
    else if (t == 1) dct2(out, in, 1, rep);
    else idct_rec(out, in, t, rep);
}
static void fdct1d(float *out, float *in, int t, int rep) {   // without the final division by 2^t
    if (t <= 0) memcpy(out, in, sizeof(float) * (size_t)rep);
    else if (t == 1) dct2(out, in, 1, rep);
    else fdct_rec(out, in, t, rep);
}
static inline void idct_view(View *o, View *in) { o->logw = in->logw; o->logh = in->logh; idct1d(o->ptr, in->ptr, in->logh, 1 << in->logw); }
static inline void fdct_view(View *o, View *in) { o->logw = in->logw; o->logh = in->logh; fdct1d(o->ptr, in->ptr, in->logh, 1 << in->logw); }

// LF (the block's 8x8 averages) to the LLF coefficients of a larger DCT; buf: log_rows x log_columns
// in, the result wide (max x min)
static void fdct2d_llf(float *buf, float *scratch, int log_rows, int log_cols) {
    View b = mkview(log_cols, log_rows, buf), s = mkview(log_cols, log_rows, scratch);
    fdct_view(&s, &b);
    view_transpose(&b, s);
    fdct_view(&s, &b);
    for (int y = 0; y < (1 << s.logh); y++)
        for (int x = 0; x < (1 << s.logw); x++)
            *AT(s, x, y) *= LF2LLF_SCALES[(1 << s.logw) + x] * LF2LLF_SCALES[(1 << s.logh) + y];
    if (log_cols > log_rows) view_transpose(&b, s); else view_copy(&b, s);
}

// the 8 x 8 transform unrolled: the same butterflies as idct_core (4 and 8 points), 8 columns at a
// time, the rows transposed in registers
static inline __attribute__((always_inline)) void idct4v(vf x0, vf x1, vf x2, vf x3, vf *y) {
    vf a0 = x0 + x2, a1 = x0 - x2, o0 = SQRT2 * x1, o1 = x1 + x3;
    vf b0 = (o0 + o1) * HALF_SECANTS[2], b1 = (o0 - o1) * HALF_SECANTS[3];
    y[0] = a0 + b0; y[3] = a0 - b0; y[1] = a1 + b1; y[2] = a1 - b1;
}
static inline __attribute__((always_inline)) void idct8v(vf *x) {
    vf e[4], o[4];
    idct4v(x[0], x[2], x[4], x[6], e);
    idct4v(SQRT2 * x[1], x[1] + x[3], x[3] + x[5], x[5] + x[7], o);
    for (int i = 0; i < 4; i++) {
        vf t = o[i] * HALF_SECANTS[4 + i];
        x[i] = e[i] + t; x[7 - i] = e[i] - t;
    }
}
static inline __attribute__((always_inline)) void transpose8v(vf *r) {
    vf t[8], u[8];
    for (int i = 0; i < 8; i += 2) {
        t[i] = __builtin_shufflevector(r[i], r[i + 1], 0, 8, 1, 9, 4, 12, 5, 13);
        t[i + 1] = __builtin_shufflevector(r[i], r[i + 1], 2, 10, 3, 11, 6, 14, 7, 15);
    }
    for (int i = 0; i < 8; i += 4)
        for (int j = 0; j < 2; j++) {
            u[i + j] = __builtin_shufflevector(t[i + j], t[i + j + 2], 0, 1, 8, 9, 4, 5, 12, 13);
            u[i + j + 2] = __builtin_shufflevector(t[i + j], t[i + j + 2], 2, 3, 10, 11, 6, 7, 14, 15);
        }
    static const int M[4] = {0, 2, 1, 3};   // (u[0..3]: columns 0 / 4, 2 / 6, 1 / 5, 3 / 7 of rows 0..3)
    for (int i = 0; i < 4; i++) {
        r[i] = __builtin_shufflevector(u[M[i]], u[M[i] + 4], 0, 1, 2, 3, 8, 9, 10, 11);
        r[i + 4] = __builtin_shufflevector(u[M[i]], u[M[i] + 4], 4, 5, 6, 7, 12, 13, 14, 15);
    }
}
static void idct8x8(float *buf) {
    vf r[8];
    for (int i = 0; i < 8; i++) r[i] = vload(buf + 8 * i);
    idct8v(r);
    transpose8v(r);
    idct8v(r);
    for (int i = 0; i < 8; i++) vstore(buf + 8 * i, r[i]);
}

// the 8-point transform over rv groups of 8 columns (element (i, r) at [i * rv + r])
static void idct8_h(vfu *out, vfu *in, int t, int rv) {
    (void)t;
    for (int r = 0; r < rv; r++) {
        vf x[8];
        for (int i = 0; i < 8; i++) x[i] = in[i * rv + r];
        idct8v(x);
        for (int i = 0; i < 8; i++) out[i * rv + r] = x[i];
    }
}

// out (2^logh rows of 2^logw) = in (2^logw rows of 2^logh) transposed; 8 x 8 blocks in registers
static void transpose_f(float *out, const float *in, int logw, int logh) {
    int w = 1 << logw, h = 1 << logh;   // (of out)
    if (logw < 3 || logh < 3) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) out[y * w + x] = in[x * h + y];
        return;
    }
    for (int by = 0; by < h; by += 8)
        for (int bx = 0; bx < w; bx += 8) {
            vf r[8];
            for (int i = 0; i < 8; i++) r[i] = vload(in + (size_t)(bx + i) * h + by);
            transpose8v(r);
            for (int i = 0; i < 8; i++) vstore(out + (size_t)(by + i) * w + bx, r[i]);
        }
}

// (scratch: the size of the block)
static void idct2d(float *buf, float *scratch, int log_rows, int log_cols) {
    if (log_rows == 3 && log_cols == 3) { idct8x8(buf); return; }
    int R = 1 << log_rows, C = 1 << log_cols;
    float *src = buf, *p, *q, *res;
    if (log_cols > log_rows) { transpose_f(scratch, buf, log_rows, log_cols); src = scratch; }   // (coefficients are stored wide)
    p = src == buf ? scratch : buf;
    idct1d(p, src, log_cols, R);         // columns: 2^log_cols rows of R
    q = p == buf ? scratch : buf;
    transpose_f(q, p, log_cols, log_rows);   // to R rows of C
    res = p;
    idct1d(res, q, log_rows, C);
    if (res != buf) memcpy(buf, res, sizeof(float) * (size_t)R * C);
}

static inline void aux_idct11(float *out, float *in, int x, int y, int S2) {
    int p = y * 8 + x, q = (y * 2) * 8 + (x * 2);
    float c00 = in[p], c01 = in[p + S2], c10 = in[p + S2 * 8], c11 = in[p + S2 * 9];
    out[q + 0] = c00 + c01 + c10 + c11;
    out[q + 1] = c00 + c01 - c10 - c11;
    out[q + 8] = c00 - c01 + c10 - c11;
    out[q + 9] = c00 - c01 - c10 + c11;
}
static void idct11(float *buf) {   // DCT2X2
    float scratch[64];
    aux_idct11(buf, buf, 0, 0, 1);
    memcpy(scratch, buf, sizeof scratch);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) aux_idct11(scratch, buf, x, y, 2);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) aux_idct11(buf, scratch, x, y, 4);
}
static void idct22(float *buf) {   // DCT4X4
    float scratch[64];
    aux_idct11(buf, buf, 0, 0, 1);
    idct1d(scratch, buf, 2, 16);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) buf[x * 8 + y] = scratch[y * 8 + x];
    idct1d(scratch, buf, 2, 16);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        buf[y * 8 + x] = scratch[(y * 2) * 8 + (x * 2)];
        buf[y * 8 + (x + 4)] = scratch[(y * 2 + 1) * 8 + (x * 2)];
        buf[(y + 4) * 8 + x] = scratch[(y * 2) * 8 + (x * 2 + 1)];
        buf[(y + 4) * 8 + (x + 4)] = scratch[(y * 2 + 1) * 8 + (x * 2 + 1)];
    }
}
static void ihornuss(float *buf) {   // IDENTITY
    float scratch[64];
    memcpy(scratch, buf, sizeof scratch);
    aux_idct11(scratch, buf, 0, 0, 1);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        int pos00 = y * 8 + x, pos11 = (y + 2) * 8 + (x + 2);
        float rsum[4] = {0}, sample11;
        for (int iy = 0; iy < 4; ++iy) for (int ix = 0; ix < 4; ++ix) rsum[ix] += scratch[(y + iy * 2) * 8 + (x + ix * 2)];
        sample11 = scratch[pos00] - (rsum[0] + rsum[1] + rsum[2] + rsum[3] - scratch[pos00]) * 0.0625f;
        scratch[pos00] = scratch[pos11];
        scratch[pos11] = 0.0f;
        for (int iy = 0; iy < 4; ++iy) for (int ix = 0; ix < 4; ++ix)
            buf[(4 * y + iy) * 8 + (4 * x + ix)] = scratch[(y + iy * 2) * 8 + (x + ix * 2)] + sample11;
    }
}
static void idct32(float *buf) {   // DCT8X4
    float scratch[64], tmp;
    View b = mkview(3, 3, buf), s = mkview(3, 3, scratch);
    tmp = *AT(b, 0, 0) + *AT(b, 0, 1);
    *AT(b, 0, 1) = *AT(b, 0, 0) - *AT(b, 0, 1);
    *AT(b, 0, 0) = tmp;
    b.logw = 4; b.logh = 2;
    idct_view(&s, &b);
    s.logw = 3; s.logh = 3;
    view_transpose(&b, s);
    idct_view(&s, &b);
    oddeven_columns_to_halves(&b, s);
}
static void idct23(float *buf) {   // DCT4X8
    float scratch[64];
    View b = mkview(3, 3, buf), s = mkview(3, 3, scratch);
    view_copy(&s, b);
    *AT(s, 0, 0) = *AT(b, 0, 0) + *AT(b, 0, 1);
    *AT(s, 0, 1) = *AT(b, 0, 0) - *AT(b, 0, 1);
    view_transpose(&b, s);
    idct_view(&s, &b);
    view_transpose(&b, s);
    b.logw = 4; b.logh = 2;
    idct_view(&s, &b);
    s.logw = 3; s.logh = 3;
    oddeven_rows_to_halves(&b, s);
}
static void iafv22(float *out, const float *in) {
	static const float AFV_BASIS[256] = { // AFVBasis in the specification, but transposed
		 0.25000000f,  0.87690293f,  0.00000000f,  0.00000000f,
		 0.00000000f, -0.41053776f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.25000000f,  0.22065181f,  0.00000000f,  0.00000000f,
		-0.70710678f,  0.62354854f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.25000000f, -0.10140050f,  0.40670076f, -0.21255748f,
		 0.00000000f, -0.06435072f, -0.45175566f, -0.30468475f,
		 0.30179295f,  0.40824829f,  0.17478670f, -0.21105601f,
		-0.14266085f, -0.13813540f, -0.17437603f,  0.11354987f,
		 0.25000000f, -0.10140050f,  0.44444817f,  0.30854971f,
		 0.00000000f, -0.06435072f,  0.15854504f,  0.51126161f,
		 0.25792363f,  0.00000000f,  0.08126112f,  0.18567181f,
		-0.34164468f,  0.33022826f,  0.07027907f, -0.07417505f,
		 0.25000000f,  0.22065181f,  0.00000000f,  0.00000000f,
		 0.70710678f,  0.62354854f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.00000000f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.25000000f, -0.10140050f,  0.00000000f,  0.47067023f,
		 0.00000000f, -0.06435072f, -0.04038515f,  0.00000000f,
		 0.16272340f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.73674975f,  0.08755115f, -0.29210266f,  0.19402893f,
		 0.25000000f, -0.10140050f,  0.19574399f, -0.16212052f,
		 0.00000000f, -0.06435072f,  0.00741823f, -0.29048013f,
		 0.09520023f,  0.00000000f, -0.36753980f,  0.49215859f,
		 0.24627108f, -0.07946707f,  0.36238173f, -0.43519050f,
		 0.25000000f, -0.10140050f,  0.29291001f,  0.00000000f,
		 0.00000000f, -0.06435072f,  0.39351034f, -0.06578702f,
		 0.00000000f, -0.40824829f, -0.30788221f, -0.38525014f,
		-0.08574019f, -0.46133749f,  0.00000000f,  0.21918685f,
		 0.25000000f, -0.10140050f, -0.40670076f, -0.21255748f,
		 0.00000000f, -0.06435072f, -0.45175566f,  0.30468475f,
		 0.30179295f, -0.40824829f, -0.17478670f,  0.21105601f,
		-0.14266085f, -0.13813540f, -0.17437603f,  0.11354987f,
		 0.25000000f, -0.10140050f, -0.19574399f, -0.16212052f,
		 0.00000000f, -0.06435072f,  0.00741823f,  0.29048013f,
		 0.09520023f,  0.00000000f,  0.36753980f, -0.49215859f,
		 0.24627108f, -0.07946707f,  0.36238173f, -0.43519050f,
		 0.25000000f, -0.10140050f,  0.00000000f, -0.47067023f,
		 0.00000000f, -0.06435072f,  0.11074166f,  0.00000000f,
		-0.16272340f,  0.00000000f,  0.00000000f,  0.00000000f,
		 0.14883399f,  0.49724647f,  0.29210266f,  0.55504438f,
		 0.25000000f, -0.10140050f,  0.11379074f, -0.14642919f,
		 0.00000000f, -0.06435072f,  0.08298163f, -0.23889774f,
		-0.35312385f, -0.40824829f,  0.48266891f,  0.17419413f,
		-0.04768680f,  0.12538059f, -0.43266080f, -0.25468277f,
		 0.25000000f, -0.10140050f, -0.44444817f,  0.30854971f,
		 0.00000000f, -0.06435072f,  0.15854504f, -0.51126161f,
		 0.25792363f,  0.00000000f, -0.08126112f, -0.18567181f,
		-0.34164468f,  0.33022826f,  0.07027907f, -0.07417505f,
		 0.25000000f, -0.10140050f, -0.29291001f,  0.00000000f,
		 0.00000000f, -0.06435072f,  0.39351034f,  0.06578702f,
		 0.00000000f,  0.40824829f,  0.30788221f,  0.38525014f,
		-0.08574019f, -0.46133749f,  0.00000000f,  0.21918685f,
		 0.25000000f, -0.10140050f, -0.11379074f, -0.14642919f,
		 0.00000000f, -0.06435072f,  0.08298163f,  0.23889774f,
		-0.35312385f,  0.40824829f, -0.48266891f, -0.17419413f,
		-0.04768680f,  0.12538059f, -0.43266080f, -0.25468277f,
		 0.25000000f, -0.10140050f,  0.00000000f,  0.42511496f,
		 0.00000000f, -0.06435072f, -0.45175566f,  0.00000000f,
		-0.60358590f,  0.00000000f,  0.00000000f,  0.00000000f,
		-0.14266085f, -0.13813540f,  0.34875205f,  0.11354987f,
	};

	int i, j;
	for (i = 0; i < 16; ++i) {
		float sum = 0.0f;
		for (j = 0; j < 16; ++j) sum += in[j] * AFV_BASIS[i * 16 + j];
		out[i] = sum;
	}
}

static void iafv(float *buf, int flipx, int flipy) {
    float scratch[64];
    float *bufafv = buf, *buf22 = buf + 16, *buf23 = buf + 32, *buf32 = buf23;
    float *scratchafv = scratch, *scratch22 = scratch + 16, *scratch23 = scratch + 32, *scratch32 = scratch23;
    for (int y = 0; y < 8; y += 2) for (int x = 0; x < 8; ++x) scratch[(x % 2) * 16 + (y / 2) * 4 + (x / 2)] = buf[y * 8 + x];
    for (int y = 1; y < 8; y += 2) for (int x = 0; x < 8; ++x) scratch32[x * 4 + (y / 2)] = buf[y * 8 + x];
    scratchafv[0] = (buf[0] + buf[1] + buf[8]) * 4.0f;
    scratch22[0] = buf[0] - buf[1] + buf[8];
    scratch32[0] = buf[0] - buf[8];
    iafv22(bufafv, scratchafv);
    idct1d(buf22, scratch22, 2, 4);
    idct1d(buf32, scratch32, 3, 4);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) scratchafv[y * 4 + x] = bufafv[y * 4 + x];
        for (int x = 0; x < 4; ++x) scratch22[x * 4 + y] = buf22[y * 4 + x];
    }
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 4; ++x) scratch23[x * 8 + y] = buf32[y * 4 + x];
    idct1d(buf22, scratch22, 2, 4);
    idct1d(buf23, scratch23, 2, 8);
    memcpy(scratch + 16, buf + 16, sizeof(float) * 48);
    for (int y = 0; y < 4; ++y) {
        static const int8_t FLIP[2][4] = {{0, 1, 2, 3}, {7, 6, 5, 4}};
        int afvpos = FLIP[flipy][y] * 8, d22pos = (flipy * 4 + y) * 8 + (!flipx * 4), d23pos = (!flipy * 4 + y) * 8;
        for (int x = 0; x < 4; ++x) buf[afvpos + FLIP[flipx][x]] = scratchafv[y * 4 + x];
        for (int x = 0; x < 4; ++x) buf[d22pos + x] = scratch22[y * 4 + x];
        for (int x = 0; x < 8; ++x) buf[d23pos + x] = scratch23[y * 8 + x];
    }
}

// ---------------------------------------------------------------------------
// varblock types (DctSelect / AcStrategy), dequantisation matrices, coefficient orders (J40)

enum { NUM_DCT = 27, NUM_DQ = 17, NUM_ORDERS = 13, MAX_PASSES = 11 };

typedef struct { int8_t log_rows, log_cols, param, order; } DctSelect;
static const DctSelect DCT_SELECT[NUM_DCT] = {
    {3, 3, 0, 0}, {3, 3, 1, 1}, {3, 3, 2, 1}, {3, 3, 3, 1},   // DCT8, IDENTITY (Hornuss), DCT2X2, DCT4X4
    {4, 4, 4, 2}, {5, 5, 5, 3}, {4, 3, 6, 4}, {3, 4, 6, 4},   // DCT16, DCT32, DCT16X8, DCT8X16
    {5, 3, 7, 5}, {3, 5, 7, 5}, {5, 4, 8, 6}, {4, 5, 8, 6},   // DCT32X8, DCT8X32, DCT32X16, DCT16X32
    {3, 3, 9, 1}, {3, 3, 9, 1}, {3, 3, 10, 1}, {3, 3, 10, 1}, // DCT4X8, DCT8X4, AFV0, AFV1
    {3, 3, 10, 1}, {3, 3, 10, 1}, {6, 6, 11, 7}, {6, 5, 12, 8},   // AFV2, AFV3, DCT64, DCT64X32
    {5, 6, 12, 8}, {7, 7, 13, 9}, {7, 6, 14, 10}, {6, 7, 14, 10}, // DCT32X64, DCT128, DCT128X64, DCT64X128
    {8, 8, 15, 11}, {8, 7, 16, 12}, {7, 8, 16, 12},               // DCT256, DCT256X128, DCT128X256
};

enum { DQ_LIBRARY = 0, DQ_HORNUSS, DQ_DCT2, DQ_DCT4, DQ_DCT4X8, DQ_AFV, DQ_DCT, DQ_RAW };
static const struct { int8_t log_rows, log_cols, def_offset, def_mode, def_n, def_m; } DCT_PARAMS[NUM_DQ] = {
    {3, 3, 0, DQ_DCT, 6, 0}, {3, 3, 6, DQ_HORNUSS, 0, 0}, {3, 3, 9, DQ_DCT2, 0, 0}, {3, 3, 15, DQ_DCT4, 4, 0},
    {4, 4, 21, DQ_DCT, 7, 0}, {5, 5, 28, DQ_DCT, 8, 0}, {3, 4, 36, DQ_DCT, 7, 0}, {3, 5, 43, DQ_DCT, 8, 0},
    {4, 5, 51, DQ_DCT, 8, 0}, {3, 3, 59, DQ_DCT4X8, 4, 0}, {3, 3, 64, DQ_AFV, 4, 4}, {6, 6, 81, DQ_DCT, 8, 0},
    {5, 6, 89, DQ_DCT, 8, 0}, {7, 7, 97, DQ_DCT, 8, 0}, {6, 7, 105, DQ_DCT, 8, 0}, {8, 8, 113, DQ_DCT, 8, 0},
    {7, 8, 121, DQ_DCT, 8, 0},
};
static const int8_t LOG_ORDER_SIZE[NUM_ORDERS][2] = {
    {3,3}, {3,3}, {4,4}, {5,5}, {3,4}, {3,5}, {4,5}, {6,6}, {5,6}, {7,7}, {6,7}, {8,8}, {7,8},
};
// (libjxl's sizes of the RAW tables, in 8 x 8 blocks)
static const int8_t RAW_SIZE_X[NUM_DQ] = {1, 1, 1, 1, 2, 4, 1, 1, 2, 1, 1, 8, 4, 16, 8, 32, 16};
static const int8_t RAW_SIZE_Y[NUM_DQ] = {1, 1, 1, 1, 2, 4, 2, 4, 4, 1, 1, 8, 8, 16, 16, 32, 32};

#define DCT4X4_DCT_PARAMS \
	{2200.0f, 392.0f, 112.0f}, {0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.5f} // (4)
#define DCT4X8_DCT_PARAMS \
	{2198.050556016380522f, 764.3655248643528689f, 527.107573587542228f}, \
	{-0.96269623020744692f, -0.92630200888366945f, -1.4594385811273854f}, \
	{-0.76194253026666783f, -0.9675229603596517f, -1.450082094097871593f}, \
	{-0.6551140670773547f, -0.27845290869168118f, -1.5843722511996204f} // (4)
// (libjxl's values: the square 64 / 128 / 256 tables have their own first weights; the Y channel's
// second band is negative - J40 has an older revision of these)
#define LARGE_DCT_PARAMS_(m0, m1, m2) \
	{m0, m1, m2}, \
	{-1.025f, -0.3041958212306401f, -1.2f}, {-0.78f, -0.3633036457487539f, -1.2f}, \
	{-0.65012f, -0.35660379990111464f, -0.8f}, {-0.19041574084286472f, -0.3443074455424403f, -0.7f}, \
	{-0.20819395464f, -0.33699592683512467f, -0.7f}, {-0.421064f, -0.30180866526242109f, -0.4f}, \
	{-0.32733845535848671f, -0.27321683125358037f, -0.5f} // (8)
#define LARGE_DCT_PARAMS(mult) LARGE_DCT_PARAMS_(mult * 23629.073922049845f, mult * 8611.3238710010046f, mult * 4492.2486445538634f)
#define LARGE_SQ_DCT_PARAMS(mult) LARGE_DCT_PARAMS_(mult * 26629.073922049845f, mult * 9311.3238710010046f, mult * 4992.2486445538634f)
static const float LIBRARY_DCT_PARAMS[129][4] = {
	// DCT33 dct_params (n=6)
	{3150.0f, 560.0f, 512.0f}, {0.0f, 0.0f, -2.0f}, {-0.4f, -0.3f, -1.0f},
	{-0.4f, -0.3f, 0.0f}, {-0.4f, -0.3f, -1.0f}, {-2.0f, -0.3f, -2.0f},
	// Hornuss params (3)
	{280.0f, 60.0f, 18.0f}, {3160.0f, 864.0f, 200.0f}, {3160.0f, 864.0f, 200.0f},
	// DCT11 params (6)
	{3840.0f, 960.0f, 640.0f}, {2560.0f, 640.0f, 320.0f}, {1280.0f, 320.0f, 128.0f},
	{640.0f, 180.0f, 64.0f}, {480.0f, 140.0f, 32.0f}, {300.0f, 120.0f, 16.0f},
	// DCT22 params (2) + dct_params (n=4)
	{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, DCT4X4_DCT_PARAMS,
	// DCT44 dct_params (n=7)
	{8996.8725711814115328f, 3191.48366296844234752f, 1157.50408145487200256f},
	{-1.3000777393353804f, -0.67424582104194355f, -2.0531423165804414f},
	{-0.49424529824571225f, -0.80745813428471001f, -1.4f},
	{-0.439093774457103443f, -0.44925837484843441f, -0.50687130033378396f},
	{-0.6350101832695744f, -0.35865440981033403f, -0.42708730624733904f},
	{-0.90177264050827612f, -0.31322389111877305f, -1.4856834539296244f},
	{-1.6162099239887414f, -0.37615025315725483f, -4.9209142884401604f},
	// DCT55 dct_params (n=8)
	{15718.40830982518931456f, 7305.7636810695983104f, 3803.53173721215041536f},
	{-1.025f, -0.8041958212306401f, -3.060733579805728f},
	{-0.98f, -0.7633036457487539f, -2.0413270132490346f},
	{-0.9012f, -0.55660379990111464f, -2.0235650159727417f},
	{-0.4f, -0.49785304658857626f, -0.5495389509954993f},
	{-0.48819395464f, -0.43699592683512467f, -0.4f},
	{-0.421064f, -0.40180866526242109f, -0.4f},
	{-0.27f, -0.27321683125358037f, -0.3f},
	// DCT34 dct_params (n=7)
	{7240.7734393502f, 1448.15468787004f, 506.854140754517f},
	{-0.7f, -0.5f, -1.4f}, {-0.7f, -0.5f, -0.2f}, {-0.2f, -0.5f, -0.5f},
	{-0.2f, -0.2f, -0.5f}, {-0.2f, -0.2f, -1.5f}, {-0.5f, -0.2f, -3.6f},
	// DCT35 dct_params (n=8)
	{16283.2494710648897f, 5089.15750884921511936f, 3397.77603275308720128f},
	{-1.7812845336559429f, -0.320049391452786891f, -0.321327362693153371f},
	{-1.6309059012653515f, -0.35362849922161446f, -0.34507619223117997f},
	{-1.0382179034313539f, -0.30340000000000003f, -0.70340000000000003f},
	{-0.85f, -0.61f, -0.9f}, {-0.7f, -0.5f, -1.0f}, {-0.9f, -0.5f, -1.0f},
	{-1.2360638576849587f, -0.6f, -1.1754605576265209f},
	// DCT45 dct_params (n=8)
	{13844.97076442300573f, 4798.964084220744293f, 1807.236946760964614f},
	{-0.97113799999999995f, -0.61125308982767057f, -1.2f},
	{-0.658f, -0.83770786552491361f, -1.2f}, {-0.42026f, -0.79014862079498627f, -0.7f},
	{-0.22712f, -0.2692727459704829f, -0.7f}, {-0.2206f, -0.38272769465388551f, -0.7f},
	{-0.226f, -0.22924222653091453f, -0.4f}, {-0.6f, -0.20719098826199578f, -0.5f},
	// DCT23 params (1) + dct_params (n=4)
	{1.0f, 1.0f, 1.0f}, DCT4X8_DCT_PARAMS,
	// AFV params (9) + dct_params (n=4) + dct4x4_params (m=4)
	{3072.0f, 1024.0f, 384.0f}, {3072.0f, 1024.0f, 384.0f}, {256.0f, 50.0f, 12.0f},
	{256.0f, 50.0f, 12.0f}, {256.0f, 50.0f, 12.0f}, {414.0f, 58.0f, 22.0f},
	{0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.25f}, {0.0f, 0.0f, -0.25f},
	DCT4X8_DCT_PARAMS, DCT4X4_DCT_PARAMS,

	LARGE_SQ_DCT_PARAMS(0.9f), // DCT66 dct_params (n=8)
	LARGE_DCT_PARAMS(0.65f), // DCT56 dct_params (n=8)
	LARGE_SQ_DCT_PARAMS(1.8f), // DCT77 dct_params (n=8)
	LARGE_DCT_PARAMS(1.3f), // DCT67 dct_params (n=8)
	LARGE_SQ_DCT_PARAMS(3.6f), // DCT88 dct_params (n=8)
	LARGE_DCT_PARAMS(2.6f), // DCT78 dct_params (n=8)
};

typedef float F4[4];

static float interpolate(float pos, int c, const F4 *bands, int len) {
    if (len == 1) return bands[0][c];
    float sp = pos * (float)(len - 1);
    int si = (int)sp;
    float frac = sp - (float)si;
    float a = bands[si][c], b = bands[si + 1][c];
    return a * powf(b / a, frac);
}
static int interp_bands(const F4 *params, int n, F4 *out) {
    for (int c = 0; c < 3; ++c) {
        out[0][c] = params[0][c];
        if (!(out[0][c] > 0)) return ERR_INVALID;
        for (int i = 1; i < n; ++i) {
            float v = params[i][c];
            out[i][c] = v > 0 ? out[i - 1][c] * (1.0f + v) : out[i - 1][c] / (1.0f - v);
            if (!(out[i][c] > 0)) return ERR_INVALID;
        }
    }
    return 0;
}
static void dct_quant_weights(int rows, int cols, const F4 *bands, int len, F4 *out) {
    float irm1 = 1.0f / (float)(rows - 1), icm1 = 1.0f / (float)(cols - 1);
    for (int c = 0; c < 3; ++c)
        for (int y = 0; y < rows; ++y) for (int x = 0; x < cols; ++x) {
            float d = hypotf((float)x * icm1, (float)y * irm1);
            out[y * cols + x][c] = interpolate(d * (1.0f / 1.414214562373095f), c, bands, len);
        }
}

typedef struct {
    int mode, n, m;   // n, m: band counts
    F4 params[9 + 16 + 16];
    float *raw;       // DQ_RAW: 3 planes of the table's values (already divided)
} DqEnc;

// the weights of table idx: out[3][rows * cols] (the coefficient multiplier is 1 / weight)
static int dq_weights(int idx, const DqEnc *enc, float *out) {
    int rows = 1 << DCT_PARAMS[idx].log_rows, cols = 1 << DCT_PARAMS[idx].log_cols, size = rows * cols;
    int mode = enc->mode, n, m;
    const F4 *params;
    F4 bands[16], scratch[64];
    if (mode == DQ_RAW) {
        for (int c = 0; c < 3; c++) for (int i = 0; i < size; i++) {
            float v = enc->raw[c * size + i];
            if (!(v > 0)) return ERR_INVALID;
            out[c * size + i] = v;
        }
        return 0;
    }
    if (mode == DQ_LIBRARY) {
        mode = DCT_PARAMS[idx].def_mode; n = DCT_PARAMS[idx].def_n; m = DCT_PARAMS[idx].def_m;
        params = (const F4 *)LIBRARY_DCT_PARAMS + DCT_PARAMS[idx].def_offset;
    } else {
        n = enc->n; m = enc->m; params = enc->params;
    }
    F4 *raw = xmalloc((size_t)size, sizeof(F4));
    if (!raw) return ERR_NOMEM;
    int e = 0;
    switch (mode) {
    case DQ_DCT:
        if (!(e = interp_bands(params, n, bands))) dct_quant_weights(rows, cols, bands, n, raw);
        break;
    case DQ_DCT4:
        if ((e = interp_bands(params + 2, n, bands))) break;
        dct_quant_weights(4, 4, bands, n, scratch);
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) raw[y * 8 + x][c] = scratch[(y / 2) * 4 + (x / 2)][c];
            raw[1][c] /= params[0][c];
            raw[8][c] /= params[0][c];
            raw[9][c] /= params[1][c];
        }
        break;
    case DQ_DCT2:
        for (int c = 0; c < 3; ++c) {
            static const int8_t MAP[64] = {
                0,0,2,2,4,4,4,4, 0,1,2,2,4,4,4,4, 2,2,3,3,4,4,4,4, 2,2,3,3,4,4,4,4,
                4,4,4,4,5,5,5,5, 4,4,4,4,5,5,5,5, 4,4,4,4,5,5,5,5, 4,4,4,4,5,5,5,5,
            };
            for (int i = 0; i < 64; ++i) raw[i][c] = params[MAP[i]][c];
            raw[0][c] = 1.0f;
        }
        break;
    case DQ_HORNUSS:
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < 64; ++i) raw[i][c] = params[0][c];
            raw[0][c] = 1.0f;
            raw[1][c] = raw[8][c] = params[1][c];
            raw[9][c] = params[2][c];
        }
        break;
    case DQ_DCT4X8:
        if ((e = interp_bands(params + 1, n, bands))) break;
        dct_quant_weights(4, 8, bands, n, scratch);
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) raw[y * 8 + x][c] = scratch[(y / 2) * 8 + x][c];
            raw[8][c] /= params[0][c];   // (libjxl: (1, 0); J40 divides (0, 1))
        }
        break;
    case DQ_AFV:
        if ((e = interp_bands(params + 9, n, bands))) break;
        dct_quant_weights(4, 8, bands, n, scratch);
        if ((e = interp_bands(params + 9 + n, m, bands))) break;
        dct_quant_weights(4, 4, bands, m, scratch + 32);
        if ((e = interp_bands(params + 5, 4, bands))) break;
        for (int c = 0; c < 3; ++c) {
            static const float FREQS[12] = {
                0.000000000f, 0.373436417f, 0.320380100f, 0.379332596f, 0.066671353f, 0.259756761f,
                0.530035651f, 0.789731061f, 0.149436598f, 0.559318823f, 0.669198646f, 0.999999917f,
            };
            scratch[0][c] = params[0][c];
            scratch[32][c] = params[1][c];
            for (int i = 0; i < 12; ++i) scratch[i + 48][c] = interpolate(FREQS[i], c, bands, 4);
            scratch[60][c] = 1.0f;
            for (int i = 0; i < 3; ++i) scratch[i + 61][c] = params[i + 2][c];
        }
        for (int c = 0; c < 3; ++c) {
            static const int8_t MAP[64] = {
                60, 32, 62, 33, 48, 34, 49, 35,  0,  1,  2,  3,  4,  5,  6,  7,
                61, 36, 63, 37, 50, 38, 51, 39,  8,  9, 10, 11, 12, 13, 14, 15,
                52, 40, 53, 41, 54, 42, 55, 43, 16, 17, 18, 19, 20, 21, 22, 23,
                56, 44, 57, 45, 58, 46, 59, 47, 24, 25, 26, 27, 28, 29, 30, 31,
            };
            for (int i = 0; i < 64; ++i) raw[i][c] = scratch[MAP[i]][c];
        }
        break;
    default:
        e = ERR_INVALID;
    }
    if (!e) for (int c = 0; c < 3; c++) for (int i = 0; i < size; i++) out[c * size + i] = raw[i][c];
    free(raw);
    return e;
}

// the natural coefficient order of a (wide) block
static int natural_order(int log_rows, int log_cols, int32_t *order) {
    int size = 1 << (log_rows + log_cols), log_slope = log_cols - log_rows;
    int rows8 = 1 << (log_rows - 3), cols8 = 1 << (log_cols - 3), o = 0;
    for (int y = 0; y < rows8; ++y) for (int x = 0; x < cols8; ++x) order[o++] = y << log_cols | x;
    for (int key1 = 1 << (log_cols - 3); o < size; ++key1) {
        int x0 = key1 & ((1 << log_slope) - 1), y0 = key1 >> log_slope, x1 = key1, y1 = 0, x, y;
        if (x1 >= (1 << log_cols)) {
            int excess = cdiv(x1 - ((1 << log_cols) - 1), 1 << log_slope);
            x1 -= excess << log_slope;
            y1 += excess;
        }
        if (y0 >= (1 << log_rows)) {
            int excess = y0 - ((1 << log_rows) - 1);
            x0 += excess << log_slope;
            y0 -= excess;
        }
        if (key1 & 1) {
            for (x = x1, y = y1; x >= x0; x -= 1 << log_slope, ++y)
                if (y >= rows8 || x >= cols8) order[o++] = y << log_cols | x;
        } else {
            for (x = x0, y = y0; x <= x1; x += 1 << log_slope, --y)
                if (y >= rows8 || x >= cols8) order[o++] = y << log_cols | x;
        }
    }
    return o == size ? 0 : ERR_INVALID;
}

// a permutation from its Lehmer code (libjxl's ReadPermutation, DecodeLehmerCode with a Fenwick tree)
static int read_permutation(Br *br, Sym *s, int size, int skip, int32_t *perm) {
    uint32_t end = sym_read(br, s, imin(7, ceil_log2((uint64_t)size + 1)));
    if (end > (uint32_t)(size - skip)) return ERR_INVALID;
    end += (uint32_t)skip;
    int32_t *code = xcalloc((size_t)size, sizeof(int32_t));
    int32_t *tree = xcalloc((size_t)size + 1, sizeof(int32_t));
    if (!code || !tree) { free(code); free(tree); return ERR_NOMEM; }
    uint32_t last = 0;
    for (uint32_t i = (uint32_t)skip; i < end; i++) {
        last = sym_read(br, s, imin(7, ceil_log2((uint64_t)last + 1)));
        if (last >= (uint32_t)size - i || s->bad) { free(code); free(tree); return ERR_INVALID; }
        code[i] = (int32_t)last;
    }
    // Fenwick tree of the unused elements
    for (int i = 1; i <= size; i++) { tree[i] += 1; int j = i + (i & -i); if (j <= size) tree[j] += tree[i]; }
    int logn = floor_log2((uint64_t)size);
    for (int i = 0; i < size; i++) {
        int rank = code[i] + 1, pos = 0;   // the rank-th unused element
        for (int b = logn; b >= 0; b--) {
            int np = pos + (1 << b);
            if (np <= size && tree[np] < rank) { pos = np; rank -= tree[np]; }
        }
        perm[i] = pos;   // (0-based: pos + 1 - 1)
        for (int j = pos + 1; j <= size; j += j & -j) tree[j]--;
    }
    free(code); free(tree);
    return 0;
}

// ---------------------------------------------------------------------------
// frame header

enum { FT_REGULAR = 0, FT_LF = 1, FT_REFONLY = 2, FT_SKIPPROG = 3 };
enum { BLEND_REPLACE = 0, BLEND_ADD = 1, BLEND_BLEND = 2, BLEND_MULADD = 3, BLEND_MUL = 4 };

typedef struct { int mode, alpha, clamp, src; } BlendInfo;

typedef struct {
    int type, modular, noise, patches, splines, use_lf_frame, skip_smooth;
    int ycbcr, hs[3], vs[3], maxhs, maxvs;
    int upsampling, ec_upsampling[MAX_EC];
    int gshift, gdim;              // group size: 128 << gshift
    int xqm, bqm;
    int npasses, pass_shift[MAX_PASSES], minshift[MAX_PASSES], maxshift[MAX_PASSES];
    int lf_level;
    int x0, y0, w, h;              // w, h: the frame's size (upsampled)
    int cw, ch;                    // coded size: / upsampling
    BlendInfo blend, ec_blend[MAX_EC];
    int is_last, save_as_ref, save_before_ct;
    uint64_t duration;
    int gab; float gabw[3][2];
    int epf_iters; float epf_sharp[8], epf_cs[3], epf_qmul, epf_s0, epf_s2, epf_bmul, epf_smod;
    // derived
    int bw, bh;                    // blocks (VarDCT), rounded up to the chroma subsampling
    int gx, gy, ngroups, lfgx, lfgy, nlfgroups;
} FrameHeader;

static int frame_header(Br *br, const Meta *m, int pw, int ph, FrameHeader *f) {
    memset(f, 0, sizeof(*f));
    f->is_last = 1;
    f->upsampling = 1;
    for (int i = 0; i < m->num_ec; i++) f->ec_upsampling[i] = 1;
    f->gshift = 1;
    f->xqm = 3; f->bqm = 2;
    f->npasses = 1;
    f->w = pw; f->h = ph;
    f->gab = 1;
    for (int c = 0; c < 3; c++) { f->gabw[c][0] = 0.115169525f; f->gabw[c][1] = 0.061248592f; }
    f->epf_iters = 2;
    for (int i = 0; i < 8; i++) f->epf_sharp[i] = (float)i / 7.0f;
    f->epf_cs[0] = 40.0f; f->epf_cs[1] = 5.0f; f->epf_cs[2] = 3.5f;
    f->epf_qmul = 0.46f; f->epf_s0 = 0.9f; f->epf_s2 = 6.5f; f->epf_bmul = 2.0f / 3.0f; f->epf_smod = 1.0f;
    f->save_before_ct = 1;
    int full_frame = 1, nds = 0, ds[4] = {0}, last[4] = {0};
    TRY(br_byte_align(br));
    if (!br_u(br, 1)) {   // !all_default
        f->type = (int)br_u(br, 2);
        f->modular = (int)br_u(br, 1);
        uint64_t flags = br_u64(br);
        f->noise = (int)(flags & 1);
        f->patches = (int)(flags >> 1 & 1);
        f->splines = (int)(flags >> 4 & 1);
        f->use_lf_frame = (int)(flags >> 5 & 1);
        f->skip_smooth = (int)(flags >> 7 & 1);
        if (!m->xyb) f->ycbcr = (int)br_u(br, 1);
        if (!f->use_lf_frame) {
            if (f->ycbcr) {
                static const int HS[4] = {0, 1, 1, 0}, VS[4] = {0, 1, 0, 1};
                int mode[3];
                for (int c = 0; c < 3; c++) {
                    mode[c] = (int)br_u(br, 2);
                    f->maxhs = imax(f->maxhs, HS[mode[c]]);
                    f->maxvs = imax(f->maxvs, VS[mode[c]]);
                }
                for (int c = 0; c < 3; c++) { f->hs[c] = f->maxhs - HS[mode[c]]; f->vs[c] = f->maxvs - VS[mode[c]]; }
            }
            f->upsampling = 1 << br_u(br, 2);
            for (int i = 0; i < m->num_ec; i++) f->ec_upsampling[i] = 1 << br_u(br, 2);
        }
        if (f->modular) f->gshift = (int)br_u(br, 2);
        else if (m->xyb) { f->xqm = (int)br_u(br, 3); f->bqm = (int)br_u(br, 3); }
        if (f->type != FT_REFONLY) {
            f->npasses = (int)br_u32(br, 1, 0, 2, 0, 3, 0, 4, 3);
            if (f->npasses > MAX_PASSES) return ERR_INVALID;
            if (f->npasses > 1) {
                nds = (int)br_u32(br, 0, 0, 1, 0, 2, 0, 3, 1);
                if (nds >= f->npasses) return ERR_INVALID;
                for (int i = 0; i < f->npasses - 1; i++) f->pass_shift[i] = (int)br_u(br, 2);
                for (int i = 0; i < nds; i++) ds[i] = 1 << br_u(br, 2);
                for (int i = 0; i < nds; i++) {
                    last[i] = (int)br_u32(br, 0, 0, 1, 0, 2, 0, 0, 3);
                    if (last[i] >= f->npasses) return ERR_INVALID;
                }
            }
        }
        if (f->type == FT_LF) {
            f->lf_level = (int)br_u(br, 2) + 1;
        } else if (br_u(br, 1)) {   // have_crop
            if (f->type != FT_REFONLY) {
                f->x0 = unpack_signed(br_u32(br, 0, 8, 256, 11, 2304, 14, 18688, 30));
                f->y0 = unpack_signed(br_u32(br, 0, 8, 256, 11, 2304, 14, 18688, 30));
            }
            f->w = (int)br_u32(br, 0, 8, 256, 11, 2304, 14, 18688, 30);
            f->h = (int)br_u32(br, 0, 8, 256, 11, 2304, 14, 18688, 30);
            full_frame = f->x0 <= 0 && f->y0 <= 0 && (int64_t)f->w + f->x0 >= pw && (int64_t)f->h + f->y0 >= ph;
        }
        if (f->type == FT_REGULAR || f->type == FT_SKIPPROG) {
            for (int i = -1; i < m->num_ec; i++) {
                BlendInfo *b = i < 0 ? &f->blend : &f->ec_blend[i];
                b->mode = (int)br_u32(br, 0, 0, 1, 0, 2, 0, 3, 2);
                if (b->mode > 4) return ERR_INVALID;
                if (m->num_ec > 0) {
                    if (b->mode == BLEND_BLEND || b->mode == BLEND_MULADD) {
                        b->alpha = (int)br_u32(br, 0, 0, 1, 0, 2, 0, 3, 3);
                        b->clamp = (int)br_u(br, 1);
                    } else if (b->mode == BLEND_MUL) {
                        b->clamp = (int)br_u(br, 1);
                    }
                }
                if (!full_frame || b->mode != BLEND_REPLACE) b->src = (int)br_u(br, 2);
            }
            if (m->animated) {
                f->duration = br_u32(br, 0, 0, 1, 0, 0, 8, 0, 32);
                if (m->have_timecodes) br_u(br, 32);
            }
            f->is_last = (int)br_u(br, 1);
        } else {
            f->is_last = 0;
        }
        if (f->type != FT_LF && !f->is_last) f->save_as_ref = (int)br_u(br, 2);
        if (f->type == FT_REFONLY || (full_frame && (f->type == FT_REGULAR || f->type == FT_SKIPPROG) &&
                                      f->blend.mode == BLEND_REPLACE && (f->duration == 0 || f->save_as_ref != 0) && !f->is_last))
            f->save_before_ct = (int)br_u(br, 1);
        else
            f->save_before_ct = f->type == FT_LF;
        TRY(skip_name(br));
        int rdefault = (int)br_u(br, 1);   // RestorationFilter
        if (!rdefault) {
            f->gab = (int)br_u(br, 1);
            if (f->gab && br_u(br, 1)) {
                for (int c = 0; c < 3; c++) for (int j = 0; j < 2; j++) TRY(br_f16(br, &f->gabw[c][j]));
            }
            f->epf_iters = (int)br_u(br, 2);
            if (f->epf_iters) {
                if (!f->modular && br_u(br, 1)) for (int i = 0; i < 8; i++) TRY(br_f16(br, &f->epf_sharp[i]));
                if (br_u(br, 1)) {
                    for (int c = 0; c < 3; c++) TRY(br_f16(br, &f->epf_cs[c]));
                    br_u(br, 32);
                }
                if (br_u(br, 1)) {
                    if (!f->modular) TRY(br_f16(br, &f->epf_qmul));
                    TRY(br_f16(br, &f->epf_s0));
                    TRY(br_f16(br, &f->epf_s2));
                    TRY(br_f16(br, &f->epf_bmul));
                }
                if (f->modular) TRY(br_f16(br, &f->epf_smod));
            }
            TRY(br_extensions(br));
        }
        TRY(br_extensions(br));
    }
    if (br_overrun(br)) return ERR_INVALID;
    for (int p = 0; p < f->npasses; p++) {   // GetDownsamplingBracket
        int maxs = 2, mins = 3;
        for (int i = 0;; i++) {
            for (int j = 0; j < nds; j++) if (i == last[j]) mins = ds[j] == 8 ? 3 : ds[j] == 4 ? 2 : ds[j] == 2 ? 1 : 0;
            if (i == f->npasses - 1) mins = 0;
            if (i == p) break;
            maxs = mins - 1;
        }
        f->minshift[p] = mins; f->maxshift[p] = maxs;
    }
    if (!m->xyb) f->xqm = f->bqm = 2;   // (no scaling outside XYB)
    if (m->xyb && m->want_icc) f->save_before_ct = 1;
    if (f->lf_level) {
        int s = 3 * f->lf_level;
        f->w = (int)(((int64_t)f->w + (1 << s) - 1) >> s);
        f->h = (int)(((int64_t)f->h + (1 << s) - 1) >> s);
    }
    if (f->w <= 0 || f->h <= 0) return ERR_INVALID;
    f->gdim = 128 << f->gshift;
    f->cw = cdiv(f->w, f->upsampling);
    f->ch = cdiv(f->h, f->upsampling);
    f->bw = cdiv(f->cw, 8 << f->maxhs) << f->maxhs;
    f->bh = cdiv(f->ch, 8 << f->maxvs) << f->maxvs;
    f->gx = cdiv(f->cw, f->gdim); f->gy = cdiv(f->ch, f->gdim);
    f->lfgx = cdiv(f->bw, f->gdim); f->lfgy = cdiv(f->bh, f->gdim);
    if ((int64_t)f->gx * f->gy > (1 << 22) || (int64_t)f->cw * f->ch > ((int64_t)1 << 32)) return ERR_LIMIT;
    f->ngroups = f->gx * f->gy;
    f->nlfgroups = f->lfgx * f->lfgy;
    return 0;
}

// ---------------------------------------------------------------------------
// working buffers kept for the next group / tile: a fresh large allocation is mapped, and its first
// use page-faults (the kernel clearing pages, the threads queueing for it: much of a decode's time)

enum { POOL_SLOTS = 64 };
typedef struct { void *slot[POOL_SLOTS]; } Pool;   // each a buffer after its size (a size_t)

static void *pool_get(Pool *p, size_t size) {
    for (int i = 0; i < POOL_SLOTS; i++) {
        size_t *b = __atomic_exchange_n((size_t **)&p->slot[i], NULL, __ATOMIC_ACQUIRE);
        if (!b) continue;
        if (b[0] >= size) return b + 2;   // (16 bytes: the size, padding)
        free(b);
        break;
    }
    size_t *b = size > SIZE_MAX - 16 ? NULL : malloc(size + 16);
    if (!b) return NULL;
    b[0] = size;
    return b + 2;
}
static void pool_put(Pool *p, void *mem) {
    if (!mem) return;
    size_t *b = (size_t *)mem - 2;
    for (int i = 0; i < POOL_SLOTS; i++) {
        void *expect = NULL;
        if (__atomic_compare_exchange_n(&p->slot[i], &expect, b, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return;
    }
    free(b);
}
static void pool_free(Pool *p) {
    for (int i = 0; i < POOL_SLOTS; i++) { free(p->slot[i]); p->slot[i] = NULL; }
}

// ---------------------------------------------------------------------------
// frame decoding

typedef struct {
    const Meta *m;
    FrameHeader h;
    int threads;
    Br *sec;                 // the sections in their logical order (one shared reader if single)
    int nsec, single;
    // LfGlobal
    float mlf[3];
    int global_scale, quant_lf;
    int nlfthr[3], lfthr[3][16], nqfthr;
    uint32_t qfthr[16];
    uint8_t *bctx; int nbctx, ndcctx;
    int color_factor; float base_x, base_b; int xfac_lf, bfac_lf;
    Tree gtree;
    Modular gm;
    int gm_begin;            // the first channel decoded by the groups
    int nb_color;            // colour channels in gm (Modular frames)
    // VarDCT
    float *lf[3];            // bw x bh
    uint8_t *lfidx;          // bw x bh
    uint8_t *acs;            // bw x bh: strategy, | 0x80 on a varblock's first block; 0xff: not set
    int32_t *qf;             // bw x bh, the varblock's quantisation
    uint8_t *sharp;          // bw x bh
    int8_t *ytox, *ytob;     // cdiv(bw, 8) x cdiv(bh, 8)
    float *dqinv[NUM_DQ];    // 3 x size each: 1 / weight
    int nhist;
    int32_t *orders[MAX_PASSES][NUM_ORDERS][3];
    Code coef[MAX_PASSES];
    uint32_t used_acs;       // the varblock types the frame has (bits)
    // output: the colour planes (VarDCT: padded to blocks), coded size
    float *px[3];
    Pool *pool;              // (group buffers, render tiles)
    int pxw[3], pxh[3];      // (pxh: the rows kept: all, or a ring of them while the frame is rendered as it goes)
    int err;
} Frame;

static void frame_free(Frame *f) {
    if (f->pool) { pool_free(f->pool); free(f->pool); }
    free(f->sec);
    free(f->bctx);
    tree_free(&f->gtree);
    modular_free(&f->gm);
    for (int c = 0; c < 3; c++) { free(f->lf[c]); free(f->px[c]); }
    free(f->lfidx); free(f->acs); free(f->qf); free(f->sharp); free(f->ytox); free(f->ytob);
    for (int i = 0; i < NUM_DQ; i++) free(f->dqinv[i]);
    for (int p = 0; p < MAX_PASSES; p++) {
        for (int o = 0; o < NUM_ORDERS; o++) for (int c = 0; c < 3; c++) free(f->orders[p][o][c]);
        code_free(&f->coef[p]);
    }
    memset(f, 0, sizeof(*f));
}

// row y of the colour plane c (a ring of rows when the frame is rendered as its groups are decoded)
static inline float *prow(const Frame *f, int c, int y) { return f->px[c] + (size_t)(y % f->pxh[c]) * f->pxw[c]; }

// the TOC: sizes, an optional permutation; sec[] in logical order
static int read_toc(Br *br, Frame *f) {
    FrameHeader *h = &f->h;
    int64_t n = h->npasses == 1 && h->ngroups == 1 ? 1 : 2 + (int64_t)h->nlfgroups + (int64_t)h->npasses * h->ngroups;
    if (n > (1 << 24)) return ERR_LIMIT;
    int32_t *perm = NULL;
    if (br_u(br, 1)) {
        Code c;
        Sym s;
        TRY(read_code(br, 8, 1, &c));
        sym_init(&s, &c, 0, br);
        if (!(perm = xmalloc((size_t)n, sizeof(int32_t)))) { code_free(&c); return ERR_NOMEM; }
        int e = read_permutation(br, &s, (int)n, 0, perm);
        int e2 = sym_finish(br, &s);
        code_free(&c);
        if (e || e2) { free(perm); return e ? e : e2; }
    }
    int e = br_byte_align(br);
    uint32_t *sizes = xmalloc((size_t)n, sizeof(uint32_t));
    if (!sizes) { free(perm); return ERR_NOMEM; }
    for (int64_t i = 0; i < n; i++) sizes[i] = br_u32(br, 0, 10, 1024, 14, 17408, 22, 4211712, 30);
    if (!e) e = br_byte_align(br);
    if (!e && br_overrun(br)) e = ERR_INVALID;
    const uint8_t *base = br_byte_pos(br), *end = br->end;
    f->nsec = (int)n;
    f->single = n == 1;
    if (!e && !(f->sec = xcalloc((size_t)n, sizeof(Br)))) e = ERR_NOMEM;
    uint64_t *offs = e ? NULL : xmalloc((size_t)n + 1, sizeof(uint64_t));   // in the order of the sizes
    if (!e && !offs) e = ERR_NOMEM;
    if (!e) {
        offs[0] = 0;
        for (int64_t i = 0; i < n; i++) offs[i + 1] = offs[i] + sizes[i];
        if (offs[n] > (uint64_t)(end - base)) e = ERR_INVALID;
    }
    for (int64_t i = 0; i < n && !e; i++) {
        int64_t k = perm ? perm[i] : i;
        br_init(&f->sec[i], base + offs[k], sizes[k]);
    }
    if (!e) br_init(br, base + offs[n], (size_t)(end - base - offs[n]));   // the next frame
    free(offs); free(sizes); free(perm);
    return e;
}

static inline Br *sec_lfglobal(Frame *f) { return &f->sec[0]; }
static inline Br *sec_lfgroup(Frame *f, int i) { return f->single ? &f->sec[0] : &f->sec[1 + i]; }
static inline Br *sec_hfglobal(Frame *f) { return f->single ? &f->sec[0] : &f->sec[1 + f->h.nlfgroups]; }
static inline Br *sec_pass(Frame *f, int p, int g) { return f->single ? &f->sec[0] : &f->sec[2 + f->h.nlfgroups + p * f->h.ngroups + g]; }

static int lf_global(Frame *f) {
    Br *br = sec_lfglobal(f);
    FrameHeader *h = &f->h;
    const Meta *m = f->m;
    if (h->patches || h->splines) return ERR_UNSUP;
    if (h->noise) br_u(br, 8 * 10 - 32), br_u(br, 32);   // the noise LUT, 8 x u(10): not rendered
    f->mlf[0] = 1.0f / 4096; f->mlf[1] = 1.0f / 512; f->mlf[2] = 1.0f / 256;
    if (!br_u(br, 1)) {
        for (int c = 0; c < 3; c++) { TRY(br_f16(br, &f->mlf[c])); f->mlf[c] /= 128; if (!(f->mlf[c] > 1e-8f)) return ERR_INVALID; }
    }
    f->color_factor = 84; f->base_x = 0; f->base_b = 1.0f;   // (libjxl's decoder: 1 without XYB too)
    if (!h->modular) {
        f->global_scale = (int)br_u32(br, 1, 11, 2049, 11, 4097, 12, 8193, 16);
        f->quant_lf = (int)br_u32(br, 16, 0, 1, 5, 1, 8, 1, 16);
        if (br_u(br, 1)) {   // default block context map
            static const uint8_t DEF[39] = {0, 1, 2, 2, 3, 3, 4, 5, 6, 6, 6, 6, 6, 7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 14, 14, 14,
                                            7, 8, 9, 9, 10, 11, 12, 13, 14, 14, 14, 14, 14};
            if (!(f->bctx = xmalloc(39, 1))) return ERR_NOMEM;
            memcpy(f->bctx, DEF, 39);
            f->nbctx = 15; f->ndcctx = 1;
        } else {
            f->ndcctx = 1;
            for (int c = 0; c < 3; c++) {
                f->nlfthr[c] = (int)br_u(br, 4);
                f->ndcctx *= f->nlfthr[c] + 1;
                for (int j = 0; j < f->nlfthr[c]; j++) f->lfthr[c][j] = unpack_signed(br_u32(br, 0, 4, 16, 8, 272, 16, 65808, 32));
            }
            f->nqfthr = (int)br_u(br, 4);
            for (int j = 0; j < f->nqfthr; j++) f->qfthr[j] = br_u32(br, 0, 2, 4, 3, 12, 5, 44, 8) + 1;
            if (f->ndcctx * (f->nqfthr + 1) > 64) return ERR_INVALID;
            TRY(cluster_map(br, 39 * f->ndcctx * (f->nqfthr + 1), 16, &f->nbctx, &f->bctx));
        }
        if (!br_u(br, 1)) {   // LfChannelCorrelation
            f->color_factor = (int)br_u32(br, 84, 0, 256, 0, 2, 8, 258, 16);
            TRY(br_f16(br, &f->base_x));
            TRY(br_f16(br, &f->base_b));
            f->xfac_lf = (int)br_u(br, 8) - 128;
            f->bfac_lf = (int)br_u(br, 8) - 128;
            if (fabsf(f->base_x) > 4 || fabsf(f->base_b) > 4 || !f->color_factor) return ERR_INVALID;
        }
    }
    // GlobalModular
    int is_gray = m->cspace == CS_GREY;
    f->nb_color = h->modular ? (is_gray && !h->ycbcr && !m->xyb ? 1 : 3) : 0;
    int nch = f->nb_color + m->num_ec;
    if (br_u(br, 1)) {   // global tree
        int64_t lim = 1024 + (int64_t)h->cw * h->ch * ((is_gray && !h->ycbcr && !m->xyb ? 1 : 3) + m->num_ec) / 16;
        if (lim > (1 << 22)) lim = 1 << 22;
        TRY(read_tree(br, lim, &f->gtree));
    }
    TRY(modular_alloc(&f->gm, nch, m->bits));
    for (int c = 0; c < f->nb_color; c++) {
        Chan *ch = &f->gm.ch[c];
        ch->w = h->cw; ch->h = h->ch;
        if (h->ycbcr) { ch->hshift = h->hs[c]; ch->vshift = h->vs[c]; ch->w = cdiv(h->cw, 1 << h->hs[c]); ch->h = cdiv(h->ch, 1 << h->vs[c]); }
    }
    for (int i = 0; i < m->num_ec; i++) {
        Chan *ch = &f->gm.ch[f->nb_color + i];
        int ecups = f->h.ec_upsampling[i] << m->ec[i].dim_shift;
        if (ecups < h->upsampling) return ERR_INVALID;
        ch->w = cdiv(h->w, ecups); ch->h = cdiv(h->h, ecups);
        ch->hshift = ch->vshift = ceil_log2((uint64_t)ecups) - ceil_log2((uint64_t)h->upsampling);
        if (ch->hshift) return ERR_UNSUP;   // upsampled extra channels
    }
    if (h->upsampling > 1) return ERR_UNSUP;
    if (nch) {
        TRY(modular_decode_channels(br, &f->gm, &f->gtree, 0, h->gdim));
        int c = f->gm.nmeta;
        while (c < f->gm.nchan && f->gm.ch[c].w <= h->gdim && f->gm.ch[c].h <= h->gdim) c++;
        f->gm_begin = c;
    }
    return br_overrun(br) ? ERR_INVALID : 0;
}

// a group's part of the global Modular image: the channels from gm_begin whose shift is in
// [minshift, maxshift], within the rectangle (pixels of the frame)
static int modular_group(Frame *f, Br *br, int x0, int y0, int xs, int ys, int minshift, int maxshift, uint32_t sid) {
    Modular g;
    int idx[4096], n = 0;
    for (int c = f->gm_begin; c < f->gm.nchan && n < 4096; c++) {
        Chan *fc = &f->gm.ch[c];
        int shift = imin(fc->hshift, fc->vshift);
        if (shift > maxshift || shift < minshift) continue;
        int rx = x0 >> fc->hshift, ry = y0 >> fc->vshift;
        int rw = rx > fc->w ? 0 : imin(xs >> fc->hshift, fc->w - rx), rh = ry > fc->h ? 0 : imin(ys >> fc->vshift, fc->h - ry);
        if (rw <= 0 || rh <= 0) continue;
        idx[n++] = c;
    }
    if (!n) return 0;
    TRY(modular_alloc(&g, n, f->gm.bits));
    for (int i = 0; i < n; i++) {
        Chan *fc = &f->gm.ch[idx[i]];
        int rx = x0 >> fc->hshift, ry = y0 >> fc->vshift;
        g.ch[i].w = imin(xs >> fc->hshift, fc->w - rx);
        g.ch[i].h = imin(ys >> fc->vshift, fc->h - ry);
        g.ch[i].hshift = fc->hshift; g.ch[i].vshift = fc->vshift;
    }
    int e = modular_image(br, &g, &f->gtree, sid);
    if (!e && g.nchan != n) e = ERR_INVALID;
    for (int i = 0; i < n && !e; i++) {
        Chan *fc = &f->gm.ch[idx[i]], *gc = &g.ch[i];
        if (gc->w != imin(xs >> fc->hshift, fc->w - (x0 >> fc->hshift))) { e = ERR_INVALID; break; }
        int rx = x0 >> fc->hshift, ry = y0 >> fc->vshift;
        for (int y = 0; y < gc->h; y++)
            memcpy(fc->p + (size_t)(ry + y) * fc->w + rx, gc->p + (size_t)y * gc->w, sizeof(int32_t) * (size_t)gc->w);
    }
    modular_free(&g);
    return e;
}

static int lf_group(Frame *f, int lfg) {
    FrameHeader *h = &f->h;
    Br *br = sec_lfgroup(f, lfg);
    int gx = lfg % h->lfgx, gy = lfg / h->lfgx;
    int bx0 = gx * h->gdim, by0 = gy * h->gdim;
    int bxs = imin(h->gdim, h->bw - bx0), bys = imin(h->gdim, h->bh - by0);
    uint32_t nlf = (uint32_t)h->nlfgroups;
    if (!h->modular && !h->use_lf_frame) {   // VarDCT LF
        int extra = (int)br_u(br, 2);
        Modular g;
        TRY(modular_alloc(&g, 3, f->m->bits));
        for (int c = 0; c < 3; c++) {
            Chan *ch = &g.ch[c < 2 ? c ^ 1 : c];
            ch->w = bxs >> h->hs[c]; ch->h = bys >> h->vs[c];
        }
        int e = modular_image(br, &g, &f->gtree, 1 + (uint32_t)lfg);
        if (!e && g.nchan != 3) e = ERR_INVALID;
        if (e) { modular_free(&g); return e; }
        float mul = 1.0f / (float)(1 << extra), dcf[3];
        for (int c = 0; c < 3; c++) dcf[c] = f->mlf[c] * 65536.0f / (float)f->global_scale / (float)f->quant_lf * mul;
        float kx = f->base_x + f->xfac_lf / (float)f->color_factor, kb = f->base_b + f->bfac_lf / (float)f->color_factor;
        int is444 = !h->maxhs && !h->maxvs;
        for (int c = 0; c < 3; c++) {
            const Chan *ch = &g.ch[c < 2 ? c ^ 1 : c];
            for (int y = 0; y < ch->h; y++) {
                float *row = f->lf[c] + (size_t)((by0 >> h->vs[c]) + y) * h->bw + (bx0 >> h->hs[c]);
                const int32_t *in = ch->p + (size_t)y * ch->w;
                for (int x = 0; x < ch->w; x++) row[x] = (float)in[x] * dcf[c];
            }
        }
        if (is444) {
            for (int y = 0; y < bys; y++) {
                size_t o = (size_t)(by0 + y) * h->bw + bx0;
                for (int x = 0; x < bxs; x++) {
                    float Y = f->lf[1][o + x];
                    f->lf[0][o + x] += kx * Y;
                    f->lf[2][o + x] += kb * Y;
                }
            }
        }
        for (int y = 0; y < bys; y++) {   // the LF context of each block
            uint8_t *row = f->lfidx + (size_t)(by0 + y) * h->bw + bx0;
            const int32_t *qx = g.ch[1].p + (size_t)(y >> h->vs[0]) * g.ch[1].w;
            const int32_t *qy = g.ch[0].p + (size_t)(y >> h->vs[1]) * g.ch[0].w;
            const int32_t *qb = g.ch[2].p + (size_t)(y >> h->vs[2]) * g.ch[2].w;
            for (int x = 0; x < bxs; x++) {
                int bx = 0, by = 0, bb = 0;
                for (int t = 0; t < f->nlfthr[0]; t++) bx += qx[x >> h->hs[0]] > f->lfthr[0][t];
                for (int t = 0; t < f->nlfthr[1]; t++) by += qy[x >> h->hs[1]] > f->lfthr[1][t];
                for (int t = 0; t < f->nlfthr[2]; t++) bb += qb[x >> h->hs[2]] > f->lfthr[2][t];
                row[x] = (uint8_t)((bx * (f->nlfthr[2] + 1) + bb) * (f->nlfthr[1] + 1) + by);
            }
        }
        modular_free(&g);
    }
    TRY(modular_group(f, br, gx * h->gdim * 8, gy * h->gdim * 8, h->gdim * 8, h->gdim * 8, 3, 1000, 1 + nlf + (uint32_t)lfg));
    if (!h->modular) {   // HF metadata
        uint32_t count = br_u(br, ceil_log2((uint64_t)bxs * bys)) + 1;
        Modular g;
        TRY(modular_alloc(&g, 4, f->m->bits));
        g.ch[0] = (Chan){cdiv(bxs, 8), cdiv(bys, 8), 3, 3, NULL};
        g.ch[1] = g.ch[0];
        g.ch[2] = (Chan){(int)count, 2, 0, 0, NULL};
        g.ch[3] = (Chan){bxs, bys, 0, 0, NULL};
        int e = modular_image(br, &g, &f->gtree, 1 + 2 * nlf + (uint32_t)lfg);
        if (!e && g.nchan != 4) e = ERR_INVALID;
        if (e) { modular_free(&g); return e; }
        int cfw = cdiv(h->bw, 8);
        for (int y = 0; y < g.ch[0].h; y++)
            for (int x = 0; x < g.ch[0].w; x++) {
                int32_t vx = g.ch[0].p[y * g.ch[0].w + x], vb = g.ch[1].p[y * g.ch[1].w + x];
                size_t o = (size_t)(by0 / 8 + y) * cfw + bx0 / 8 + x;
                f->ytox[o] = (int8_t)(vx < -128 ? -128 : vx > 127 ? 127 : vx);
                f->ytob[o] = (int8_t)(vb < -128 ? -128 : vb > 127 ? 127 : vb);
            }
        uint32_t num = 0, used = 0;
        int is444 = !h->maxhs && !h->maxvs;
        for (int iy = 0; iy < bys && !e; iy++) {
            int y = by0 + iy;
            for (int ix = 0; ix < bxs; ix++) {
                int x = bx0 + ix;
                size_t o = (size_t)y * h->bw + x;
                int32_t sh = g.ch[3].p[iy * bxs + ix];
                if (sh < 0 || sh > 7) { e = ERR_INVALID; break; }
                f->sharp[o] = (uint8_t)sh;
                if (f->acs[o] != 0xff) continue;
                if (num >= count) { e = ERR_INVALID; break; }
                int32_t st = g.ch[2].p[num], q = g.ch[2].p[count + num];
                if (st < 0 || st >= NUM_DCT) { e = ERR_INVALID; break; }
                const DctSelect *d = &DCT_SELECT[st];
                int cbx = 1 << (d->log_cols - 3), cby = 1 << (d->log_rows - 3);
                if ((cbx > 1 || cby > 1) && !is444) { e = ERR_INVALID; break; }
                if (x + cbx > (x / 32 + 1) * 32 || x + cbx > bx0 + bxs || y + cby > (y / 32 + 1) * 32 || y + cby > by0 + bys) { e = ERR_INVALID; break; }
                q = 1 + (q < 0 ? 0 : q > 255 ? 255 : q);
                for (int yy = 0; yy < cby; yy++)
                    for (int xx = 0; xx < cbx; xx++) {
                        size_t oo = o + (size_t)yy * h->bw + xx;
                        if (f->acs[oo] != 0xff) { e = ERR_INVALID; break; }
                        f->acs[oo] = (uint8_t)st;
                        f->qf[oo] = q;
                    }
                f->acs[o] = (uint8_t)(st | 0x80);
                used |= 1u << st;
                num++;
            }
        }
        modular_free(&g);
        if (e) return e;
        __atomic_fetch_or(&f->used_acs, used, __ATOMIC_RELAXED);
    }
    return br_overrun(br) ? ERR_INVALID : 0;
}

// AdaptiveDCSmoothing over the whole frame
static int smooth_lf(Frame *f) {
    FrameHeader *h = &f->h;
    int w = h->bw, hh = h->bh;
    if (w <= 2 || hh <= 2) return 0;
    const float w1 = 0.20345139757231578f, w2 = 0.0334829185968739f, w0 = 1.0f - 4.0f * (w1 + w2);
    float dcf[3];
    for (int c = 0; c < 3; c++) dcf[c] = f->mlf[c] * 65536.0f / (float)f->global_scale / (float)f->quant_lf;
    float *out[3];
    for (int c = 0; c < 3; c++) {
        if (!(out[c] = xmalloc((size_t)w * hh, sizeof(float)))) { for (int k = 0; k < c; k++) free(out[k]); return ERR_NOMEM; }
        memcpy(out[c], f->lf[c], sizeof(float) * (size_t)w * hh);
    }
    float **outp = out, *dcfp = dcf;
    nitro_parallel_n((size_t)(hh - 2), f->threads, ^(size_t i) {
        int y = (int)i + 1;
        float **out = outp, *dcf = dcfp;
        for (int x = 1; x < w - 1; x++) {
            float mc[3], sm[3], gap = 0.5f;
            for (int c = 0; c < 3; c++) {
                const float *t = f->lf[c] + (size_t)(y - 1) * w, *r = t + w, *b = r + w;
                mc[c] = r[x];
                sm[c] = ((t[x - 1] + t[x + 1]) + (b[x - 1] + b[x + 1])) * w2 + ((r[x - 1] + r[x + 1]) + (t[x] + b[x])) * w1 + mc[c] * w0;
                float g = fabsf((mc[c] - sm[c]) / dcf[c]);
                if (g > gap) gap = g;
            }
            float factor = 3.0f - 4.0f * gap;
            if (factor < 0) factor = 0;
            for (int c = 0; c < 3; c++) out[c][(size_t)y * w + x] = (sm[c] - mc[c]) * factor + mc[c];
        }
    });
    for (int c = 0; c < 3; c++) { free(f->lf[c]); f->lf[c] = out[c]; }
    return 0;
}

static int hf_global(Frame *f) {
    Br *br = sec_hfglobal(f);
    FrameHeader *h = &f->h;
    uint32_t nlf = (uint32_t)h->nlfgroups;
    DqEnc enc[NUM_DQ];
    memset(enc, 0, sizeof enc);
    int e = 0;
    if (!br_u(br, 1)) {   // the quantisation tables
        for (int i = 0; i < NUM_DQ && !e; i++) {
            DqEnc *d = &enc[i];
            d->mode = (int)br_u(br, 3);
            static const struct { int8_t sq, nparams, nscaled, ndct; } HOW[8] = {
                {0, 0, 0, 0}, {1, 3, 3, 0}, {1, 6, 6, 0}, {1, 2, 0, 1}, {1, 1, 0, 1}, {1, 9, 6, 2}, {0, 0, 0, 1}, {0, 0, 0, 0}};
            int rows = 1 << DCT_PARAMS[i].log_rows, cols = 1 << DCT_PARAMS[i].log_cols;
            if (d->mode == DQ_RAW) {
                float den;
                if ((e = br_f16(br, &den))) break;
                if (!(den > 1e-8f)) { e = ERR_INVALID; break; }
                Modular g;
                int rw = RAW_SIZE_X[i] * 8, rh = RAW_SIZE_Y[i] * 8;
                if ((e = modular_alloc(&g, 3, 8))) break;
                for (int c = 0; c < 3; c++) g.ch[c] = (Chan){rw, rh, 0, 0, NULL};
                e = modular_image(br, &g, &f->gtree, 1 + 3 * nlf + (uint32_t)i);
                if (!e && g.nchan != 3) e = ERR_INVALID;
                if (!e && !(d->raw = xmalloc((size_t)3 * rows * cols, sizeof(float)))) e = ERR_NOMEM;
                for (int c = 0; c < 3 && !e; c++)
                    for (int k = 0; k < rows * cols; k++) {
                        int32_t q = g.ch[c].p[k];
                        if (q <= 0) { e = ERR_INVALID; break; }
                        d->raw[c * rows * cols + k] = 1.0f / (den * (float)q);
                    }
                modular_free(&g);
            } else if (d->mode != DQ_LIBRARY) {
                if (HOW[d->mode].sq && (rows != 8 || cols != 8)) { e = ERR_INVALID; break; }
                for (int c = 0; c < 3; c++)
                    for (int j = 0; j < HOW[d->mode].nparams; j++) {
                        if ((e = br_f16(br, &d->params[j][c]))) break;
                        if (j < HOW[d->mode].nscaled) d->params[j][c] *= 64;
                    }
                int pidx = HOW[d->mode].nparams;
                for (int k = 0; k < HOW[d->mode].ndct && !e; k++) {
                    int nb = (int)br_u(br, 4) + 1;
                    if (k == 0) d->n = nb; else d->m = nb;
                    for (int c = 0; c < 3 && !e; c++)
                        for (int j = 0; j < nb; j++) {
                            if ((e = br_f16(br, &d->params[pidx + j][c]))) break;
                            if (j == 0) d->params[pidx][c] *= 64;
                        }
                    pidx += nb;
                }
            }
        }
    }
    if (e) { for (int i = 0; i < NUM_DQ; i++) free(enc[i].raw); return e; }
    // the weights of the tables the frame's varblocks use (libjxl computes no others either), in parallel
    uint32_t used_dq = 0, used_ord = 0;
    for (int t = 0; t < NUM_DCT; t++) if (f->used_acs >> t & 1) { used_dq |= 1u << DCT_SELECT[t].param; used_ord |= 1u << DCT_SELECT[t].order; }
    int list[NUM_DQ], nlist = 0, errs[NUM_DQ] = {0};
    for (int i = 0; i < NUM_DQ; i++) if (used_dq >> i & 1) list[nlist++] = i;
    int *errp = errs, *listp = list;
    DqEnc *encp = enc;
    nitro_parallel_n((size_t)nlist, f->threads, ^(size_t j) {
        int i = listp[j], size = 1 << (DCT_PARAMS[i].log_rows + DCT_PARAMS[i].log_cols);
        float *w = xmalloc((size_t)3 * size, sizeof(float));
        int er = w ? dq_weights(i, &encp[i], w) : ERR_NOMEM;
        if (!er) for (int k = 0; k < 3 * size; k++) w[k] = 1.0f / w[k];
        f->dqinv[i] = w;
        errp[i] = er;
    });
    for (int i = 0; i < NUM_DQ && !e; i++) e = errs[i];
    for (int i = 0; i < NUM_DQ; i++) free(enc[i].raw);
    if (e) return e;
    f->nhist = (int)br_u(br, ceil_log2((uint64_t)h->ngroups)) + 1;
    for (int p = 0; p < h->npasses; p++) {
        uint32_t used = br_u32(br, 0x5F, 0, 0x13, 0, 0, 0, 0, 13);
        Code c;
        Sym s;
        if (used) { TRY(read_code(br, 8, 1, &c)); sym_init(&s, &c, 0, br); }
        for (int o = 0; o < NUM_ORDERS && !e; o++) {   // (orders of varblocks the frame does not have: read, not kept)
            int lr = LOG_ORDER_SIZE[o][0], lc = LOG_ORDER_SIZE[o][1], size = 1 << (lr + lc);
            if (!(used_ord >> o & 1) && !(used >> o & 1)) continue;
            int32_t *nat = xmalloc((size_t)size, sizeof(int32_t));
            if (!nat) { e = ERR_NOMEM; break; }
            natural_order(lr, lc, nat);
            for (int ch = 0; ch < 3 && !e; ch++) {
                int32_t *ord = used_ord >> o & 1 ? xmalloc((size_t)size, sizeof(int32_t)) : NULL;
                if ((used_ord >> o & 1) && !ord) { e = ERR_NOMEM; break; }
                f->orders[p][o][ch] = ord;
                if (used >> o & 1) {
                    int32_t *perm = xmalloc((size_t)size, sizeof(int32_t));
                    if (!perm) { e = ERR_NOMEM; break; }
                    e = read_permutation(br, &s, size, size / 64, perm);
                    for (int k = 0; k < size && !e && ord; k++) ord[k] = nat[perm[k]];
                    free(perm);
                } else {
                    memcpy(ord, nat, sizeof(int32_t) * (size_t)size);
                }
            }
            free(nat);
        }
        if (used) { int e2 = sym_finish(br, &s); code_free(&c); if (!e) e = e2; }
        if (e) return e;
        TRY(read_code(br, f->nhist * f->nbctx * 495, 1, &f->coef[p]));
        uint8_t *m2 = realloc(f->coef[p].map, (size_t)f->coef[p].num_dist + 16);   // (contexts up to 15 past: cluster 0)
        if (!m2) return ERR_NOMEM;
        memset(m2 + f->coef[p].num_dist, 0, 16);
        f->coef[p].map = m2;
    }
    return br_overrun(br) ? ERR_INVALID : 0;
}

static const uint8_t COEFF_FREQ_CTX[64] = {
    0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
    15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22,
    23, 23, 23, 23, 24, 24, 24, 24, 25, 25, 25, 25, 26, 26, 26, 26,
    27, 27, 27, 27, 28, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30,
};
static const uint8_t COEFF_NUM_NONZERO_CTX[64] = {
    0,   0,   31,  62,  62,  93,  93,  93,  93,  123, 123, 123, 123,
    152, 152, 152, 152, 152, 152, 152, 152, 180, 180, 180, 180, 180,
    180, 180, 180, 180, 180, 180, 180, 206, 206, 206, 206, 206, 206,
    206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206,
    206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206, 206,
};

typedef struct {   // (a VarDCT group is at most 32 x 32 blocks)
    int32_t *coef[3];        // per channel, the group's varblocks one after the other
    int32_t *nz[3];          // per channel: non-zero counts of the 8x8 blocks
    float scratch[3 * 65536];   // a block, the dequantised Y (for CfL), the transforms' scratch
    int32_t coefmem[3][32 * 32 * 64], nzmem[3][32 * 32], offs[32 * 32];
} GroupBuf;

// the group's varblocks: coefficient offsets in raster order of their first blocks
static int group_blocks(const Frame *f, int gbx0, int gby0, int gbxs, int gbys, int32_t *offs) {
    const FrameHeader *h = &f->h;
    int off = 0;
    for (int by = 0; by < gbys; by++)
        for (int bx = 0; bx < gbxs; bx++) {
            uint8_t a = f->acs[(size_t)(gby0 + by) * h->bw + gbx0 + bx];
            if (a == 0xff) return ERR_INVALID;
            if (!(a & 0x80)) { offs[by * gbxs + bx] = -1; continue; }
            const DctSelect *d = &DCT_SELECT[a & 0x7f];
            offs[by * gbxs + bx] = off;
            off += 1 << (d->log_rows + d->log_cols);
        }
    return 0;
}

// the coefficients of a group's pass: fast = ANS without LZ77 (the bit reader and the state in locals)
static inline __attribute__((always_inline)) int hf_coefficients(Frame *f, Br *brp, Sym *sp, int fast, int p, int ctxoff, int gbx0, int gby0,
                                                                  int gbxs, int gbys, const int32_t *offs, GroupBuf *gb) {
    const FrameHeader *h = &f->h;
    const Code *code = sp->code;
    const uint8_t *map = code->map;
    const Alias *alias = code->alias;
    const HybridCfg *cfg = code->cfg;
    int la = code->log_alpha, bad = 0;
    Br lbr = *brp, *br = fast ? &lbr : brp;
    uint32_t st = sp->state;
#define HF_READ(ctx) (fast ? ans_fast(br, map, alias, la, cfg, &st, &bad, (ctx)) : sym_read(br, sp, (ctx)))
    int shift = h->pass_shift[p], e = 0;
    for (int by = 0; by < gbys && !e; by++)
        for (int bx = 0; bx < gbxs && !e; bx++) {
            size_t bo = (size_t)(gby0 + by) * h->bw + gbx0 + bx;
            uint8_t a = f->acs[bo];
            if (!(a & 0x80)) continue;
            const DctSelect *d = &DCT_SELECT[a & 0x7f];
            int log2cb = d->log_rows + d->log_cols - 6, cb = 1 << log2cb, size = 64 << log2cb;
            int cbx = 1 << (d->log_cols - 3), cby = 1 << (d->log_rows - 3);
            int qfidx = 0;
            for (int t = 0; t < f->nqfthr; t++) qfidx += (uint32_t)f->qf[bo] > f->qfthr[t];
            static const int YXB[3] = {1, 0, 2};
            for (int ci = 0; ci < 3 && !e; ci++) {
                int c = YXB[ci];
                int sbx = bx >> h->hs[c], sby = by >> h->vs[c];
                if (sbx << h->hs[c] != bx || sby << h->vs[c] != by) continue;
                int stride = gbxs;
                int32_t *nzr = gb->nz[c] + (size_t)sby * stride;
                int pred = sbx ? (sby ? (nzr[sbx - stride] + nzr[sbx - 1] + 1) >> 1 : nzr[sbx - 1]) : (sby ? nzr[sbx - stride] : 32);
                int bctx = f->bctx[(((c < 2 ? c ^ 1 : 2) * NUM_ORDERS + d->order) * (f->nqfthr + 1) + qfidx) * f->ndcctx + f->lfidx[bo]];
                int nzctx = ctxoff + (pred < 8 ? pred : 4 + pred / 2) * f->nbctx + bctx;
                uint32_t nz = HF_READ(nzctx);
                if (nz > (uint32_t)(size - cb)) { e = ERR_INVALID; break; }
                int32_t q = (int32_t)((nz + (uint32_t)cb - 1) >> log2cb);
                for (int y = 0; y < cby; y++) for (int x = 0; x < cbx; x++) nzr[sbx + x + y * stride] = q;
                int hoff = ctxoff + f->nbctx * 37 + 458 * bctx;   // (the contexts reach 15 past the last: the map has 16 more)
                int prev = nz > (uint32_t)size / 16 ? 0 : 1;
                int32_t *coef = gb->coef[c] + offs[by * gbxs + bx];
                const int32_t *order = f->orders[p][d->order][c];
                for (int k = cb; k < size && nz; k++) {
                    int ctx = hoff + (COEFF_NUM_NONZERO_CTX[(nz + (uint32_t)cb - 1) >> log2cb] + COEFF_FREQ_CTX[k >> log2cb]) * 2 + prev;
                    uint32_t u = HF_READ(ctx);
                    coef[order[k]] += (int32_t)((uint32_t)unpack_signed(u) << shift);
                    prev = u != 0;
                    nz -= (uint32_t)prev;
                }
                if (nz) e = ERR_INVALID;
            }
            if (!e && (bad || sp->bad)) e = ERR_INVALID;
        }
#undef HF_READ
    if (fast) { *brp = lbr; sp->state = st; }
    return e;
}

static int hf_pass(Frame *f, Br *br, int p, int gbx0, int gby0, int gbxs, int gbys, const int32_t *offs, GroupBuf *gb) {
    int ctxoff = (int)br_u(br, ceil_log2((uint64_t)f->nhist)) * f->nbctx * 495;
    if (ctxoff >= f->nhist * f->nbctx * 495) return ERR_INVALID;
    Sym s;
    sym_init(&s, &f->coef[p], 0, br);
    for (int c = 0; c < 3; c++) memset(gb->nz[c], 0, sizeof(int32_t) * (size_t)gbxs * gbys);
    int e = !s.code->use_prefix && !s.code->lz77 ? hf_coefficients(f, br, &s, 1, p, ctxoff, gbx0, gby0, gbxs, gbys, offs, gb)
                                                  : hf_coefficients(f, br, &s, 0, p, ctxoff, gbx0, gby0, gbxs, gbys, offs, gb);
    int e2 = sym_finish(br, &s);
    return e ? e : e2;
}

// dequantisation, chroma from luma, the LLF coefficients from the LF image, the inverse transforms
static void hf_reconstruct(Frame *f, int gbx0, int gby0, int gbxs, int gbys, const int32_t *offs, GroupBuf *gb) {
    const FrameHeader *h = &f->h;
    const Meta *m = f->m;
    static const float QM_SCALE[8] = {1.5625f, 1.25f, 1.0f, 0.8f, 0.64f, 0.512f, 0.4096f, 0.32768f};
    float *block = gb->scratch, *dqy = gb->scratch + 65536, *tmp = gb->scratch + 131072;
    int cfw = cdiv(h->bw, 8);
    for (int by = 0; by < gbys; by++)
        for (int bx = 0; bx < gbxs; bx++) {
            int off = offs[by * gbxs + bx];
            if (off < 0) continue;
            int abx = gbx0 + bx, aby = gby0 + by;
            size_t bo = (size_t)aby * h->bw + abx;
            int st = f->acs[bo] & 0x7f;
            const DctSelect *d = &DCT_SELECT[st];
            int size = 1 << (d->log_rows + d->log_cols);
            int lmin = imin(d->log_rows, d->log_cols), lmax = imax(d->log_rows, d->log_cols);
            int vh8 = 1 << (lmin - 3), vw8 = 1 << (lmax - 3);
            float muly = 65536.0f / (float)f->global_scale / (float)f->qf[bo];
            float mul[3] = {muly * QM_SCALE[h->xqm], muly, muly * QM_SCALE[h->bqm]};
            size_t tile = (size_t)(aby / 8) * cfw + abx / 8;
            float kx = f->base_x + f->ytox[tile] / (float)f->color_factor;
            float kb = f->base_b + f->ytob[tile] / (float)f->color_factor;
            const float *dq = f->dqinv[d->param];
            for (int ci = 0; ci < 3; ci++) {
                static const int YXB[3] = {1, 0, 2};
                int c = YXB[ci];
                int sbx = abx >> h->hs[c], sby = aby >> h->vs[c];
                int aligned = sbx << h->hs[c] == abx && sby << h->vs[c] == aby;
                if (!aligned && c != 1) continue;
                const int32_t *q = gb->coef[c] + off;
                float *out = c == 1 ? dqy : block;
                float bias = m->quant_bias[c], bnum = m->quant_bias_num, mc = mul[c];
                const float *dqc = dq + c * size;
                for (int k = 0; k < size; k += 8) {   // (size: a multiple of 64)
                    vi qi;
                    memcpy(&qi, q + k, sizeof qi);
                    vf qf = __builtin_convertvector(qi, vf), aq = vabs(qf);
                    vf v = aq < 1.5f ? qf * bias : qf - bnum / qf;   // (0, +-1: the biases; libjxl's AdjustQuantBias)
                    vf r = v * vload(dqc + k) * mc;
                    if (c == 0) r += kx * vload(dqy + k);
                    else if (c == 2) r += kb * vload(dqy + k);
                    memcpy(out + k, &r, sizeof r);
                }
                if (!aligned) continue;
                float *buf = c == 1 ? dqy : block;
                if (c == 1) { memcpy(block, dqy, sizeof(float) * (size_t)size); buf = block; }
                // LLF from LF
                const float *lf = f->lf[c] + (size_t)sby * h->bw + sbx;
                if (d->log_rows <= 3 && d->log_cols <= 3) {
                    buf[0] = lf[0];
                } else {
                    int cby = 1 << (d->log_rows - 3), cbx = 1 << (d->log_cols - 3);
                    float llf[1024];
                    for (int y = 0; y < cby; y++) for (int x = 0; x < cbx; x++) llf[y * cbx + x] = lf[(size_t)y * h->bw + x];
                    fdct2d_llf(llf, tmp, d->log_rows - 3, d->log_cols - 3);
                    for (int y = 0; y < vh8; y++) for (int x = 0; x < vw8; x++) buf[y * vw8 * 8 + x] = llf[y * vw8 + x];
                }
                switch (st) {
                case 1: ihornuss(buf); break;
                case 2: idct11(buf); break;
                case 3: idct22(buf); break;
                case 12: idct23(buf); break;
                case 13: idct32(buf); break;
                case 14: iafv(buf, 0, 0); break;
                case 15: iafv(buf, 1, 0); break;
                case 16: iafv(buf, 0, 1); break;
                case 17: iafv(buf, 1, 1); break;
                default: idct2d(buf, tmp, d->log_rows, d->log_cols); break;
                }
                int rows = 1 << d->log_rows, cols = 1 << d->log_cols;
                for (int y = 0; y < rows; y++) memcpy(prow(f, c, sby * 8 + y) + (size_t)sbx * 8, buf + y * cols, sizeof(float) * (size_t)cols);
            }
        }
}

// ---------------------------------------------------------------------------
// restoration filters (Gaborish, EPF: libjxl's render stages), in horizontal stripes: each stage
// works on rows [a, b) of the frame with mirrored borders, so stripes need no shared state.

enum { PAD = 4 };
#define K_INV_SIGMA_NUM (-1.1715728752538099024f)
#define K_MIN_SIGMA (-3.90524291751269967465540850526868f)

static inline int mirror(int v, int size) {
    while (v < 0 || v >= size) v = v < 0 ? -v - 1 : 2 * size - 1 - v;
    return v;
}

typedef struct {   // a tile of three planes: the valid part [x0, x1) x [y0, y1); rows indexed by image x
    int x0, x1, y0, y1;   // (what a stage has computed)
    int w, h;             // the image (for mirroring at its edges)
    int xbase, ybase, stride, nrows;   // the memory: rows ybase ..., columns xbase - PAD ...
    float *mem;
} Stripe;

static inline float *srow(const Stripe *s, int c, int y) {
    y = mirror(y, s->h);
    return s->mem + ((size_t)c * s->nrows + (size_t)(y - s->ybase)) * (size_t)s->stride + PAD - s->xbase;
}
// the columns past the image's edges, mirrored (for a row computed over [x0, x1))
static inline void mirror_x(float *row, int x0, int x1, int w) {
    if (x0 == 0) for (int i = 1; i <= PAD; i++) row[-i] = row[mirror(-i, w)];
    if (x1 == w) for (int i = 1; i <= PAD; i++) row[w - 1 + i] = row[mirror(w - 1 + i, w)];
}

static void stage_gaborish(const Frame *f, const Stripe *in, Stripe *out) {
    int x0 = out->x0, x1 = out->x1;
    for (int c = 0; c < 3; c++) {
        float w0 = 1.0f, w1 = f->h.gabw[c][0], w2 = f->h.gabw[c][1], sum = w0 + 4 * (w1 + w2);
        w0 /= sum; w1 /= sum; w2 /= sum;
        for (int y = out->y0; y < out->y1; y++) {
            const float *t = srow(in, c, y - 1), *r = srow(in, c, y), *b = srow(in, c, y + 1);
            float *o = srow(out, c, y);
            int x = x0;
            for (; x + 8 <= x1; x += 8) {
                vf v = vload(r + x) * w0 + (vload(r + x - 1) + vload(r + x + 1) + vload(t + x) + vload(b + x)) * w1 +
                       (vload(t + x - 1) + vload(t + x + 1) + vload(b + x - 1) + vload(b + x + 1)) * w2;
                vstore(o + x, v);
            }
            for (; x < x1; x++)
                o[x] = r[x] * w0 + (r[x - 1] + r[x + 1] + t[x] + b[x]) * w1 + (t[x - 1] + t[x + 1] + b[x - 1] + b[x + 1]) * w2;
            mirror_x(o, x0, x1, out->w);
        }
    }
}

// inverse sigma of the block at (bx, by) (< K_MIN_SIGMA: not filtered)
static inline float epf_inv_sigma(const Frame *f, int bx, int by) {
    if (f->h.modular) return K_INV_SIGMA_NUM / f->h.epf_smod;
    size_t o = (size_t)by * f->h.bw + bx;
    float s = f->h.epf_qmul * 65536.0f / ((float)f->global_scale * (float)f->qf[o] * K_INV_SIGMA_NUM) * f->h.epf_sharp[f->sharp[o]];
    if (s > -1e-4f) s = -1e-4f;
    return 1.0f / s;
}

// one pixel of an EPF step (the scalar path: a block's partial rows)
static void epf_pixel(const Frame *f, const float *rows7[3][7], float *o[3], int x, int y, int step, int nk, const int (*off)[2],
                      int yb, float sm, float bsm) {
    static const int PLUS[5][2] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};
    const FrameHeader *h = &f->h;
    const float *const *rows[3] = {rows7[0] + 1, rows7[1] + 1, rows7[2] + 1};   // (rows -2 .. 2)
    float is = epf_inv_sigma(f, x >> 3, y >> 3);
    if (is < K_MIN_SIGMA) {
        for (int c = 0; c < 3; c++) o[c][x] = rows[c][2][x];
        return;
    }
    float inv = is * (yb || (x & 7) == 0 || (x & 7) == 7 ? bsm : sm);
    float sw = 1.0f, sx = rows[0][2][x], sy = rows[1][2][x], sb = rows[2][2][x];
    for (int k = 0; k < nk; k++) {
        int dy = off[k][0], dx = off[k][1];
        float sad = 0;
        for (int c = 0; c < 3; c++) {
            float sc = 0;
            if (step == 2) {
                sc = fabsf(rows[c][2][x] - rows[c][2 + dy][x + dx]);
            } else {
                for (int p = 0; p < 5; p++)
                    sc += fabsf(rows7[c][3 + PLUS[p][0]][x + PLUS[p][1]] - rows7[c][3 + dy + PLUS[p][0]][x + dx + PLUS[p][1]]);
            }
            sad += sc * h->epf_cs[c];
        }
        float wt = 1.0f + sad * inv;
        if (wt <= 0) continue;
        sw += wt;
        sx += wt * rows[0][2 + dy][x + dx];
        sy += wt * rows[1][2 + dy][x + dx];
        sb += wt * rows[2][2 + dy][x + dx];
    }
    float iw = 1.0f / sw;
    o[0][x] = sx * iw; o[1][x] = sy * iw; o[2][x] = sb * iw;
}

static void stage_epf(const Frame *f, const Stripe *in, Stripe *out, int step) {
    static const int OFF12[12][2] = {{-2, 0}, {-1, -1}, {-1, 0}, {-1, 1}, {0, -2}, {0, -1}, {0, 1}, {0, 2}, {1, -1}, {1, 0}, {1, 1}, {2, 0}};
    static const int OFF4[4][2] = {{-1, 0}, {0, -1}, {0, 1}, {1, 0}};
    static const int PLUS[5][2] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};
    const FrameHeader *h = &f->h;
    int x0 = out->x0, x1 = out->x1, nk = step == 0 ? 12 : 4;
    const int (*off)[2] = step == 0 ? OFF12 : OFF4;
    float sm = (step == 0 ? h->epf_s0 : step == 2 ? h->epf_s2 : 1.0f) * 1.65f, bsm = sm * h->epf_bmul;
    for (int y = out->y0; y < out->y1; y++) {
        const float *rows7[3][7], *rows[3][5];
        int reach = step == 0 ? 3 : step == 1 ? 2 : 1;   // (the rows this step reads)
        for (int c = 0; c < 3; c++) for (int d = -3; d <= 3; d++) rows7[c][d + 3] = srow(in, c, d < -reach || d > reach ? y : y + d);
        for (int c = 0; c < 3; c++) for (int d = 0; d < 5; d++) rows[c][d] = rows7[c][d + 1];
        float *o[3] = {srow(out, 0, y), srow(out, 1, y), srow(out, 2, y)};
        int yb = (y & 7) == 0 || (y & 7) == 7;
        vf smv = yb ? (vf)bsm : (vf){bsm, sm, sm, sm, sm, sm, sm, bsm};   // (block edges: the border multiplier)
        int x = x0, xa = imin(x1, (x0 + 7) & ~7);
        for (; x < xa; x++) epf_pixel(f, rows7, o, x, y, step, nk, off, yb, sm, bsm);   // (to the first block's start)
        for (; x + 8 <= x1; x += 8) {   // one block row at a time: one sigma
            float is = epf_inv_sigma(f, x >> 3, y >> 3);
            if (is < K_MIN_SIGMA) {
                for (int c = 0; c < 3; c++) vstore(o[c] + x, vload(rows[c][2] + x));
                continue;
            }
            vf inv = is * smv, sw = 1.0f, sx = vload(rows[0][2] + x), sy = vload(rows[1][2] + x), sb = vload(rows[2][2] + x);
            for (int k = 0; k < nk; k++) {
                int dy = off[k][0], dx = off[k][1];
                vf sad = 0;
                for (int c = 0; c < 3; c++) {
                    vf sc;
                    if (step == 2) {
                        sc = vabs(vload(rows[c][2] + x) - vload(rows[c][2 + dy] + x + dx));
                    } else {
                        sc = 0;
                        for (int p = 0; p < 5; p++)
                            sc += vabs(vload(rows7[c][3 + PLUS[p][0]] + x + PLUS[p][1]) - vload(rows7[c][3 + dy + PLUS[p][0]] + x + dx + PLUS[p][1]));
                    }
                    sad += sc * h->epf_cs[c];
                }
                vf wt = vmax(1.0f + sad * inv, (vf)0);
                sw += wt;
                sx += wt * vload(rows[0][2 + dy] + x + dx);
                sy += wt * vload(rows[1][2 + dy] + x + dx);
                sb += wt * vload(rows[2][2 + dy] + x + dx);
            }
            vf iw = 1.0f / sw;
            vstore(o[0] + x, sx * iw); vstore(o[1] + x, sy * iw); vstore(o[2] + x, sb * iw);
        }
        for (; x < x1; x++) epf_pixel(f, rows7, o, x, y, step, nk, off, yb, sm, bsm);
        for (int c = 0; c < 3; c++) mirror_x(o[c], x0, x1, out->w);
    }
}

// ---------------------------------------------------------------------------
// colour: to sRGB for display (XYB, enumerated colour spaces), the ICC profile's space otherwise

typedef struct {
    int mode;            // 0: XYB, 1: pass through (ICC or sRGB), 2: enumerated -> sRGB
    float opsin[3][3], bias[3], cbrt_bias[3];
    float mat[3][3];     // mode 2: linear source -> linear sRGB
    int tf, gamma_set; float gamma;   // the source transfer (mode 2)
    int hdr; float peak;              // tone mapping: the source's peak, in units of SDR white
    int grey;
} Colour;

// the sRGB transfer function, libjxl's rational approximation (error ~5e-7); for x in [0, 1]
static inline vf vsqrt(vf x) {
#if defined(__has_builtin) && __has_builtin(__builtin_elementwise_sqrt)
    return __builtin_elementwise_sqrt(x);
#else
    vf r;
    for (int i = 0; i < 8; i++) r[i] = sqrtf(x[i]);
    return r;
#endif
}
static inline vf srgb_v(vf x) {
    vf r = vsqrt(x);
    vf p = (((7.352629620e-01f * r + 1.474205315e+00f) * r + 3.903842876e-01f) * r + 5.287254571e-03f) * r - 5.135152395e-04f;
    vf q = (((2.424867759e-02f * r + 9.258482155e-01f) * r + 1.340816930e+00f) * r + 3.036675394e-01f) * r + 1.004519624e-02f;
    return x > 0.0031308f ? p / q : x * 12.92f;
}
static float srgb_oetf(float v) {
    if (v <= 0.0031308f) return v <= 0 ? 0 : 12.92f * v;
    if (v >= 1) return 1;
    float r = sqrtf(v);
    return ((((7.352629620e-01f * r + 1.474205315e+00f) * r + 3.903842876e-01f) * r + 5.287254571e-03f) * r - 5.135152395e-04f) /
           ((((2.424867759e-02f * r + 9.258482155e-01f) * r + 1.340816930e+00f) * r + 3.036675394e-01f) * r + 1.004519624e-02f);
}
static float tf_decode(const Colour *k, float v) {   // to linear (PQ: in units of 10000 nits)
    if (k->gamma_set) return v <= 0 ? 0 : powf(v, k->gamma);
    switch (k->tf) {
    case TF_LINEAR: return v;
    case TF_709: return v < 0.081f ? v / 4.5f : powf((v + 0.099f) / 1.099f, 1 / 0.45f);
    case TF_DCI: return v <= 0 ? 0 : powf(v, 2.6f);
    case TF_PQ: {
        if (v <= 0) return 0;
        float p = powf(v, 1 / 78.84375f), n = p - 0.8359375f;
        if (n < 0) n = 0;
        return powf(n / (18.8515625f - 18.6875f * p), 1 / 0.1593017578125f);
    }
    case TF_HLG: {
        float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
        return v <= 0.5f ? v * v / 3 : (expf((v - c) / a) + b) / 12;
    }
    default: return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    }
}

static void xy_to_xyz(const float xy[2], double out[3]) { out[0] = xy[0] / xy[1]; out[1] = 1; out[2] = (1 - xy[0] - xy[1]) / xy[1]; }
static void mat_inv(const double a[3][3], double o[3][3]) {
    double d = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        int i1 = (j + 1) % 3, i2 = (j + 2) % 3, j1 = (i + 1) % 3, j2 = (i + 2) % 3;
        o[i][j] = (a[i1][j1] * a[i2][j2] - a[i1][j2] * a[i2][j1]) / d;
    }
}
static void mat_mul(const double a[3][3], const double b[3][3], double o[3][3]) {
    double t[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(o, t, sizeof t);
}
// RGB -> XYZ (D50 adapted with Bradford, as ICC profiles)
static void rgb_to_xyz(const float prim[3][2], const float white[2], double o[3][3]) {
    double p[3][3], pi[3][3], wxyz[3], s[3];
    for (int i = 0; i < 3; i++) { double v[3]; xy_to_xyz(prim[i], v); for (int k = 0; k < 3; k++) p[k][i] = v[k]; }
    xy_to_xyz(white, wxyz);
    mat_inv(p, pi);
    for (int k = 0; k < 3; k++) s[k] = pi[k][0] * wxyz[0] + pi[k][1] * wxyz[1] + pi[k][2] * wxyz[2];
    for (int k = 0; k < 3; k++) for (int i = 0; i < 3; i++) p[k][i] *= s[i];
    static const double BR[3][3] = {{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}};
    double bri[3][3], lw[3], ld[3], d50[3] = {0.96422, 1.0, 0.82521}, ad[3][3] = {{0}};
    mat_inv(BR, bri);
    for (int k = 0; k < 3; k++) { lw[k] = BR[k][0] * wxyz[0] + BR[k][1] * wxyz[1] + BR[k][2] * wxyz[2]; ld[k] = BR[k][0] * d50[0] + BR[k][1] * d50[1] + BR[k][2] * d50[2]; }
    for (int k = 0; k < 3; k++) ad[k][k] = ld[k] / lw[k];
    mat_mul(ad, BR, ad);
    mat_mul(bri, ad, ad);
    mat_mul(ad, p, o);
}

static void colour_init(const Meta *m, Colour *k) {
    memset(k, 0, sizeof(*k));
    k->grey = m->cspace == CS_GREY;
    int tf = m->gamma ? 0 : m->tf;
    float it = m->intensity_target;
    if (m->xyb) {
        k->mode = 0;
        float s = 255.0f / it;
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) k->opsin[i][j] = m->opsin_inv[i][j] * s;
        for (int i = 0; i < 3; i++) { k->bias[i] = m->opsin_bias[i]; k->cbrt_bias[i] = cbrtf(m->opsin_bias[i]); }
        if (tf == TF_PQ || tf == TF_HLG) { k->hdr = 1; k->peak = it / 203.0f; }
        // XYB is decoded to linear sRGB: wide gamut colours are clipped
        return;
    }
    if (m->want_icc || m->cspace == CS_UNKNOWN) { k->mode = 1; return; }
    static const float SRGB_W[2] = {0.3127f, 0.3290f};
    static const float SRGB_P[3][2] = {{0.639998686f, 0.330010138f}, {0.300003784f, 0.600003357f}, {0.150002046f, 0.059997204f}};
    int same_prim = k->grey || (!memcmp(m->prim, SRGB_P, sizeof SRGB_P) && !memcmp(m->white, SRGB_W, sizeof SRGB_W));
    if (same_prim && !m->gamma && m->tf == TF_SRGB) { k->mode = 1; return; }
    k->mode = 2;
    k->gamma_set = m->gamma != 0;
    k->gamma = m->gamma ? 1e7f / (float)m->gamma : 1;
    k->tf = m->tf;
    if (tf == TF_PQ) { k->hdr = 1; k->peak = 10000.0f / 203.0f; }
    if (tf == TF_HLG) { k->hdr = 1; k->peak = 1000.0f / 203.0f; }
    if (same_prim) {
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) k->mat[i][j] = i == j;
    } else {
        double src[3][3], dst[3][3], dsti[3][3], mm[3][3];
        rgb_to_xyz(m->prim, m->white, src);
        rgb_to_xyz(SRGB_P, SRGB_W, dst);
        mat_inv(dst, dsti);
        mat_mul(dsti, src, mm);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) k->mat[i][j] = (float)mm[i][j];
    }
}

// one pixel to display values 0..1
static inline void colour_px(const Colour *k, float *r, float *g, float *b) {
    float R = *r, G = *g, B = *b;
    if (k->mode == 1) return;
    if (k->mode == 0) {
        float gr = G + R - k->cbrt_bias[0], gg = G - R - k->cbrt_bias[1], gb = B - k->cbrt_bias[2];
        float mr = gr * gr * gr + k->bias[0], mg = gg * gg * gg + k->bias[1], mb = gb * gb * gb + k->bias[2];
        R = k->opsin[0][0] * mr + k->opsin[0][1] * mg + k->opsin[0][2] * mb;
        G = k->opsin[1][0] * mr + k->opsin[1][1] * mg + k->opsin[1][2] * mb;
        B = k->opsin[2][0] * mr + k->opsin[2][1] * mg + k->opsin[2][2] * mb;
        if (k->hdr) { R *= k->peak; G *= k->peak; B *= k->peak; }
    } else {
        R = tf_decode(k, R); G = tf_decode(k, G); B = tf_decode(k, B);
        if (k->tf == TF_HLG && !k->gamma_set) {   // the HLG OOTF for a 1000 nits display
            float Y = 0.2627f * R + 0.678f * G + 0.0593f * B, s = Y > 0 ? powf(Y, 0.2f) : 0;
            R *= s; G *= s; B *= s;
        }
        if (k->hdr) { R *= k->peak; G *= k->peak; B *= k->peak; }
        float r2 = k->mat[0][0] * R + k->mat[0][1] * G + k->mat[0][2] * B;
        float g2 = k->mat[1][0] * R + k->mat[1][1] * G + k->mat[1][2] * B;
        float b2 = k->mat[2][0] * R + k->mat[2][1] * G + k->mat[2][2] * B;
        R = r2; G = g2; B = b2;
    }
    if (k->hdr) {   // extended Reinhard on the largest component, SDR white at 1
        float mx = R > G ? R : G;
        if (B > mx) mx = B;
        if (mx > 0) {
            float lw = k->peak, t = mx * (1 + mx / (lw * lw)) / (1 + mx);
            R *= t / mx; G *= t / mx; B *= t / mx;
        }
    }
    *r = srgb_oetf(R); *g = srgb_oetf(G); *b = srgb_oetf(B);
}

// ---------------------------------------------------------------------------
// decoding a frame

typedef struct Output Output;
typedef struct Render Render;
static int render_rows(const Frame *f, const Output *o, int ry0, int ry1);

// one group: its passes, then (VarDCT) its pixels
static int decode_group(Frame *f, int g) {
    const FrameHeader *h = &f->h;
    int gx = g % h->gx, gy = g / h->gx, err = 0;
    int gbx0 = gx * (h->gdim >> 3), gby0 = gy * (h->gdim >> 3);
    int gbxs = imin(h->gdim >> 3, h->bw - gbx0), gbys = imin(h->gdim >> 3, h->bh - gby0);
    GroupBuf *gb = NULL;
    int32_t *offs = NULL;
    if (!h->modular) {
        if (gbxs > 32 || gbys > 32) return ERR_INVALID;
        if (!(gb = pool_get(f->pool, sizeof(GroupBuf)))) return ERR_NOMEM;
        offs = gb->offs;
        for (int c = 0; c < 3; c++) {
            gb->coef[c] = gb->coefmem[c];
            gb->nz[c] = gb->nzmem[c];
            memset(gb->coef[c], 0, sizeof(int32_t) * (size_t)gbxs * gbys * 64);
        }
        err = group_blocks(f, gbx0, gby0, gbxs, gbys, offs);
    }
    for (int p = 0; p < h->npasses && !err; p++) {
        Br *br = sec_pass(f, p, g);
        if (!h->modular) err = hf_pass(f, br, p, gbx0, gby0, gbxs, gbys, offs, gb);
        if (!err) err = modular_group(f, br, gx * h->gdim, gy * h->gdim, h->gdim, h->gdim, h->minshift[p], h->maxshift[p],
                                      1 + 3 * (uint32_t)h->nlfgroups + NUM_DQ + (uint32_t)(p * h->ngroups + g));
        if (!err && br_overrun(br)) err = ERR_INVALID;
    }
    if (!err && !h->modular) hf_reconstruct(f, gbx0, gby0, gbxs, gbys, offs, gb);
    pool_put(f->pool, gb);
    return err;
}

// a custom float (bits, exp_bits) stored in an int to a float (libjxl's int_to_float)
static float int_to_float(int32_t v, int bits, int exp_bits) {
    uint32_t f = (uint32_t)v;
    float out;
    if (bits == 32) { memcpy(&out, &f, 4); return out; }
    int exp_bias = (1 << (exp_bits - 1)) - 1, sign_shift = bits - 1, mant_bits = bits - exp_bits - 1, mant_shift = 23 - mant_bits;
    int signbit = (int)(f >> sign_shift);
    f &= (1u << sign_shift) - 1;
    if (!f) return signbit ? -0.f : 0.f;
    int exp = (int)(f >> mant_bits);
    uint32_t mant = (f & ((1u << mant_bits) - 1)) << mant_shift;
    if (exp == (1 << exp_bits) - 1) exp = 255;   // inf / nan
    else {
        if (exp == 0 && exp_bits < 8) {
            while (!(mant & 0x800000)) { mant <<= 1; exp--; }
            exp++;
            mant &= 0x7fffff;
        }
        exp += 127 - exp_bias;
        if (exp < 0) return 0;
    }
    f = (signbit ? 0x80000000u : 0) | (uint32_t)exp << 23 | mant;
    memcpy(&out, &f, 4);
    return out;
}

// the colour channel c's row y (coded frame size) as float, before the filters
static void load_row(const Frame *f, int c, int y, int x0, int x1, float *dst) {   // dst[x0 .. x1)
    const FrameHeader *h = &f->h;
    const Meta *m = f->m;
    int w = h->cw;
    if (!h->modular) {
        if (!h->hs[c] && !h->vs[c]) { memcpy(dst + x0, prow(f, c, y) + x0, sizeof(float) * (size_t)(x1 - x0)); return; }
        // chroma upsampling (libjxl's fancy upsampling: 3/4, 1/4)
        int cw = cdiv(w, 1 << h->hs[c]), chh = cdiv(h->ch, 1 << h->vs[c]);
        const float *r0, *r1;
        float a = 1.0f, b = 0.0f;
        if (h->vs[c]) {
            int ys = y >> 1, yn = mirror(ys + (y & 1 ? 1 : -1), chh);
            r0 = prow(f, c, ys); r1 = prow(f, c, yn); a = 0.75f; b = 0.25f;
        } else {
            r0 = r1 = prow(f, c, y);
        }
        if (!h->hs[c]) {
            int x = x0;
            for (; x + 8 <= x1; x += 8) vstore(dst + x, a * vload(r0 + x) + b * vload(r1 + x));
            for (; x < x1; x++) dst[x] = a * r0[x] + b * r1[x];
            return;
        }
        for (int c0 = x0; c0 < x1; c0 += 1024) {   // in chunks: the vertical pass into v, then 3/4, 1/4 across
            int c1 = x1 - c0 > 1024 ? c0 + 1024 : x1;
            int k0 = (c0 >> 1) - 1, k1 = ((c1 - 1) >> 1) + 1, k = k0;
            float vb[1024 / 2 + 16], *v = vb - k0;   // v[k0 .. k1]
            for (; k <= k1 && (k < 0 || k + 8 > cw); k++) { int i = mirror(k, cw); v[k] = a * r0[i] + b * r1[i]; }
            for (; k + 8 <= k1 + 1 && k + 8 <= cw; k += 8) vstore(v + k, a * vload(r0 + k) + b * vload(r1 + k));
            for (; k <= k1; k++) { int i = mirror(k, cw); v[k] = a * r0[i] + b * r1[i]; }
            int x = c0;
            if (x & 1) { dst[x] = 0.75f * v[x >> 1] + 0.25f * v[(x >> 1) + 1]; x++; }
            for (; x + 16 <= c1; x += 16) {
                const float *p = v + (x >> 1);
                vf m = vload(p), e = 0.75f * m + 0.25f * vload(p - 1), o = 0.75f * m + 0.25f * vload(p + 1);
                vstore(dst + x, __builtin_shufflevector(e, o, 0, 8, 1, 9, 2, 10, 3, 11));
                vstore(dst + x + 8, __builtin_shufflevector(e, o, 4, 12, 5, 13, 6, 14, 7, 15));
            }
            for (; x < c1; x++) dst[x] = 0.75f * v[x >> 1] + 0.25f * v[(x >> 1) + (x & 1 ? 1 : -1)];
        }
        return;
    }
    int ci = f->nb_color == 1 ? 0 : (m->xyb && c < 2 ? c ^ 1 : c);
    const Chan *ch = &f->gm.ch[ci];
    const int32_t *in = ch->p + (size_t)y * ch->w;
    if (m->xyb) {
        float factor = f->mlf[c];
        if (c == 2) { const int32_t *iy = f->gm.ch[0].p + (size_t)y * ch->w; for (int x = x0; x < x1; x++) dst[x] = (float)(in[x] + iy[x]) * factor; }
        else for (int x = x0; x < x1; x++) dst[x] = (float)in[x] * factor;
    } else if (m->exp_bits) {
        for (int x = x0; x < x1; x++) dst[x] = int_to_float(in[x], m->bits, m->exp_bits);
    } else {
        float factor = 1.0f / (float)((1u << m->bits) - 1);
        for (int x = x0; x < x1; x++) dst[x] = (float)in[x] * factor;
    }
}

struct Output {
    int format;   // NJXL_BGRA8, NJXL_RGBA16
    uint8_t *pixels;
    size_t pitch;
    int alpha_ec;   // -1: none
    int premul_in;  // the alpha is associated (premultiplied) already
};

static void emit_row(const Frame *f, const Colour *k, const Output *o, int y, int x0, int w, const float *r, const float *g, const float *b) {
    const Meta *m = f->m;
    const int32_t *ain = NULL;
    float afac = 0;
    int abits = 0, aexp = 0;
    if (o->alpha_ec >= 0) {
        const Chan *ac = &f->gm.ch[f->nb_color + o->alpha_ec];
        ain = ac->p + (size_t)y * ac->w;
        abits = m->ec[o->alpha_ec].bits; aexp = m->ec[o->alpha_ec].exp_bits;
        afac = 1.0f / (float)(((uint64_t)1 << abits) - 1);
    }
    int x = x0;   // (columns x0 .. w; x0 is a multiple of 8)
    if (k->mode != 2) for (; x + 8 <= w; x += 8) {   // 8 pixels at once (the enumerated colour spaces: below)
        vf R = vload(r + x), G = vload(g + x), B = vload(b + x), A = 1;
        if (f->h.ycbcr) {
            vf Yv = G + 128.0f / 255, cb = R, cr = B;
            R = Yv + 1.402f * cr;
            G = Yv + (-0.114f * 1.772f / 0.587f) * cb + (-0.299f * 1.402f / 0.587f) * cr;
            B = Yv + 1.772f * cb;
        }
        if (k->mode == 0) {   // XYB to linear sRGB
            vf gr = G + R - k->cbrt_bias[0], gg = G - R - k->cbrt_bias[1], gb = B - k->cbrt_bias[2];
            vf mr = gr * gr * gr + k->bias[0], mg = gg * gg * gg + k->bias[1], mb = gb * gb * gb + k->bias[2];
            R = k->opsin[0][0] * mr + k->opsin[0][1] * mg + k->opsin[0][2] * mb;
            G = k->opsin[1][0] * mr + k->opsin[1][1] * mg + k->opsin[1][2] * mb;
            B = k->opsin[2][0] * mr + k->opsin[2][1] * mg + k->opsin[2][2] * mb;
            if (k->hdr) {
                R *= k->peak; G *= k->peak; B *= k->peak;
                vf mx = vmax(vmax(R, G), B), lw2 = k->peak * k->peak;
                vf sc = mx > 0 ? (1 + mx / lw2) / (1 + mx) : 1;
                R *= sc; G *= sc; B *= sc;
            }
            R = srgb_v(vclamp01(R)); G = srgb_v(vclamp01(G)); B = srgb_v(vclamp01(B));
        }
        R = vclamp01(R); G = vclamp01(G); B = vclamp01(B);
        if (ain) {
            if (aexp) for (int i = 0; i < 8; i++) A[i] = int_to_float(ain[x + i], abits, aexp);
            else { vi ai; memcpy(&ai, ain + x, sizeof ai); A = __builtin_convertvector(ai, vf) * afac; }
            A = vclamp01(A);
            if (!o->premul_in) { R *= A; G *= A; B *= A; }
            else { R = vmin(R, A); G = vmin(G, A); B = vmin(B, A); }
        }
        if (o->format == NJXL_BGRA8) {
            vi bi = __builtin_convertvector(B * 255 + 0.5f, vi), gi = __builtin_convertvector(G * 255 + 0.5f, vi);
            vi ri = __builtin_convertvector(R * 255 + 0.5f, vi), ai = __builtin_convertvector(A * 255 + 0.5f, vi);
            vi px = bi | gi << 8 | ri << 16 | ai << 24;   // (each 0 .. 255)
            memcpy(o->pixels + (size_t)y * o->pitch + (size_t)x * 4, &px, sizeof px);
        } else {
            vi ri = __builtin_convertvector(R * 65535 + 0.5f, vi), gi = __builtin_convertvector(G * 65535 + 0.5f, vi);
            vi bi = __builtin_convertvector(B * 65535 + 0.5f, vi), ai = __builtin_convertvector(A * 65535 + 0.5f, vi);
            uint16_t px[32];
            for (int i = 0; i < 8; i++) { px[4 * i] = (uint16_t)ri[i]; px[4 * i + 1] = (uint16_t)gi[i]; px[4 * i + 2] = (uint16_t)bi[i]; px[4 * i + 3] = (uint16_t)ai[i]; }
            memcpy((uint16_t *)(o->pixels + (size_t)y * o->pitch) + (size_t)x * 4, px, sizeof px);
        }
    }
    for (; x < w; x++) {
        float R = r[x], G = g[x], B = b[x], A = 1;
        if (f->h.ycbcr) {
            float Yv = G + 128.0f / 255, cb = R, cr = B;
            R = Yv + 1.402f * cr;
            G = Yv + (-0.114f * 1.772f / 0.587f) * cb + (-0.299f * 1.402f / 0.587f) * cr;
            B = Yv + 1.772f * cb;
        }
        colour_px(k, &R, &G, &B);
        R = R < 0 ? 0 : R > 1 ? 1 : R; G = G < 0 ? 0 : G > 1 ? 1 : G; B = B < 0 ? 0 : B > 1 ? 1 : B;
        if (ain) {
            A = aexp ? int_to_float(ain[x], abits, aexp) : (float)ain[x] * afac;
            A = A < 0 ? 0 : A > 1 ? 1 : A;
            if (!o->premul_in) { R *= A; G *= A; B *= A; }
            else { R = R < A ? R : A; G = G < A ? G : A; B = B < A ? B : A; }
        }
        if (o->format == NJXL_BGRA8) {
            uint8_t *p = o->pixels + (size_t)y * o->pitch + (size_t)x * 4;
            p[0] = (uint8_t)(B * 255 + 0.5f); p[1] = (uint8_t)(G * 255 + 0.5f); p[2] = (uint8_t)(R * 255 + 0.5f); p[3] = (uint8_t)(A * 255 + 0.5f);
        } else {
            uint16_t *p = (uint16_t *)(o->pixels + (size_t)y * o->pitch) + (size_t)x * 4;
            p[0] = (uint16_t)(R * 65535 + 0.5f); p[1] = (uint16_t)(G * 65535 + 0.5f); p[2] = (uint16_t)(B * 65535 + 0.5f); p[3] = (uint16_t)(A * 65535 + 0.5f);
        }
    }
}

// the restoration filters' stages, the colour conversion
struct Render { Colour k; int nst, radii[4], kind[4], border; };

static void render_init(const Frame *f, Render *rd) {
    const FrameHeader *h = &f->h;
    memset(rd, 0, sizeof(*rd));
    if (h->gab) { rd->kind[rd->nst] = -1; rd->radii[rd->nst++] = 1; }
    if (h->epf_iters >= 3) { rd->kind[rd->nst] = 0; rd->radii[rd->nst++] = 3; }
    if (h->epf_iters >= 1) { rd->kind[rd->nst] = 1; rd->radii[rd->nst++] = 2; }
    if (h->epf_iters >= 2) { rd->kind[rd->nst] = 2; rd->radii[rd->nst++] = 1; }
    for (int i = 0; i < rd->nst; i++) rd->border += rd->radii[i];
    colour_init(f->m, &rd->k);
}

// filters, colour conversion and output of the tile [x0, x1) x [y0, y1)
static int render_tile(const Frame *f, const Render *rd, const Output *o, int x0, int x1, int y0, int y1) {
    int W = f->h.cw, H = f->h.ch, B = rd->border;
    int ybase = imax(0, y0 - B), xbase = imax(0, x0 - B);
    int nrows = imin(H, y1 + B) - ybase, stride = imin(W, x1 + B) - xbase + 2 * PAD;
    size_t planes = (size_t)nrows * (size_t)stride;
    float *mem = planes > SIZE_MAX / 24 ? NULL : pool_get(f->pool, planes * 6 * sizeof(float));
    if (!mem) return ERR_NOMEM;
    Stripe s[2];
    for (int i = 0; i < 2; i++)
        s[i] = (Stripe){xbase, imin(W, x1 + B), ybase, imin(H, y1 + B), W, H, xbase, ybase, stride, nrows, mem + planes * 3 * i};
    for (int c = 0; c < 3; c++)
        for (int y = s[0].y0; y < s[0].y1; y++) { float *r = srow(&s[0], c, y); load_row(f, c, y, s[0].x0, s[0].x1, r); mirror_x(r, s[0].x0, s[0].x1, W); }
    int cur = 0, rem = B;
    for (int i = 0; i < rd->nst; i++) {
        rem -= rd->radii[i];
        Stripe *in = &s[cur], *out = &s[cur ^ 1];
        out->y0 = imax(0, y0 - rem); out->y1 = imin(H, y1 + rem);
        out->x0 = imax(0, x0 - rem); out->x1 = imin(W, x1 + rem);
        if (rd->kind[i] < 0) stage_gaborish(f, in, out); else stage_epf(f, in, out, rd->kind[i]);
        cur ^= 1;
    }
    for (int y = y0; y < y1; y++) emit_row(f, &rd->k, o, y, x0, x1, srow(&s[cur], 0, y), srow(&s[cur], 1, y), srow(&s[cur], 2, y));
    pool_put(f->pool, mem);
    return 0;
}

enum { TW = 512, TH = 64 };   // render tiles (the last ones take the remainder: at least TW x TH unless the image is smaller)
static int tiles_x(int w) { return imax(1, w / TW); }
static int tiles_y(int h) { return imax(1, h / TH); }
static void tile_rect(int w, int h, int ntx, int nty, int i, int *x0, int *x1, int *y0, int *y1) {   // (rows relative)
    int tx = i % ntx, ty = i / ntx;
    *x0 = tx * TW; *x1 = tx == ntx - 1 ? w : *x0 + TW;
    *y0 = ty * TH; *y1 = ty == nty - 1 ? h : *y0 + TH;
}

// rows [ry0, ry1), tile by tile in parallel
static int render_rows(const Frame *f, const Output *o, int ry0, int ry1) {
    Render rd;
    render_init(f, &rd);
    const Render *rdp = &rd;
    int ntx = tiles_x(f->h.cw), nty = tiles_y(ry1 - ry0), n = ntx * nty, *errs = xcalloc((size_t)n, sizeof(int));
    if (!errs) return ERR_NOMEM;
    nitro_parallel_n((size_t)n, f->threads, ^(size_t i) {
        int x0, x1, y0, y1;
        tile_rect(f->h.cw, ry1 - ry0, ntx, nty, (int)i, &x0, &x1, &y0, &y1);
        errs[i] = render_tile(f, rdp, o, x0, x1, ry0 + y0, ry0 + y1);
    });
    int e = 0;
    for (int i = 0; i < n && !e; i++) e = errs[i];
    free(errs);
    return e;
}

enum { RING = 5, LAG = 2 };   // group rows of colour planes kept while a VarDCT frame is rendered as it goes

// The groups and the rendering as one loop ("waterfront"): group row r, then the band of rows that
// row r - LAG + 1 completed (band b: rows b * gdim - border ...: the filters reach border rows down). A
// band's tiles wait for the group rows around it, a group for the bands that read the ring rows it
// overwrites; each waits only for items before it, which the pool has already started.
static int decode_wave(Frame *f, const Output *o, int threads) {
    const FrameHeader *h = &f->h;
    int gy = h->gy, gx = h->gx, G = h->gdim;
    Render rd;
    render_init(f, &rd);
    int border = rd.border + 2;   // (+2: the chroma upsampling's row)
    int *band_y0 = xmalloc((size_t)gy + 1, sizeof(int)), *band_tiles = xmalloc((size_t)gy, sizeof(int));
    int *task = xmalloc((size_t)gy * gx + (size_t)gy * 64 * 64 + 1, sizeof(int)), ntask = 0;
    int *rowdone = xcalloc((size_t)gy, sizeof(int)), *banddone = xcalloc((size_t)gy, sizeof(int));
    int *failed = xcalloc(1, sizeof(int)), *err = xcalloc(1, sizeof(int));
    int e = 0;
    if (!band_y0 || !band_tiles || !task || !rowdone || !banddone || !failed || !err) e = ERR_NOMEM;
    for (int b = 0; b <= gy && !e; b++) band_y0[b] = b == gy ? h->ch : imax(0, b * G - border);
    int ntx = tiles_x(h->cw);
    for (int b = 0; b < gy && !e; b++) band_tiles[b] = o ? ntx * tiles_y(band_y0[b + 1] - band_y0[b]) : 0;
    // the order: groups of row r, then band r - LAG's tiles (when they come, the rows they wait for have
    // long been started); the last bands at the end. (task: group g, or -1 - (band << 16 | tile))
    for (int r = 0; r < gy + LAG && !e; r++) {
        if (r < gy) for (int c = 0; c < gx; c++) task[ntask++] = r * gx + c;
        if (r >= LAG) for (int t = 0; t < band_tiles[r - LAG]; t++) task[ntask++] = -1 - ((r - LAG) << 16 | t);
    }
    if (!e) {
        const Render *rdp = &rd;
        nitro_parallel_n((size_t)ntask, threads, ^(size_t i) {
            int t = task[i];
            if (t >= 0) {   // a group: the ring rows it overwrites must have been rendered
                int r = t / gx;
                for (int k = r - RING; k <= r - RING + 1; k++)
                    if (k >= 0) while (__atomic_load_n(&banddone[k], __ATOMIC_ACQUIRE) < band_tiles[k] && !__atomic_load_n(failed, __ATOMIC_RELAXED)) nitro_yield();
                int ge = __atomic_load_n(failed, __ATOMIC_RELAXED) ? ERR_INVALID : decode_group(f, t);
                if (ge) { __atomic_store_n(err, ge, __ATOMIC_RELAXED); __atomic_store_n(failed, 1, __ATOMIC_RELEASE); }
                __atomic_add_fetch(&rowdone[r], 1, __ATOMIC_RELEASE);
            } else {   // a band's tile: the group rows its rows and their border lie in
                int b = (-1 - t) >> 16, ti = (-1 - t) & 0xffff;
                for (int k = imax(0, b - 1); k <= imin(gy - 1, b + 1); k++)
                    while (__atomic_load_n(&rowdone[k], __ATOMIC_ACQUIRE) < gx && !__atomic_load_n(failed, __ATOMIC_RELAXED)) nitro_yield();
                if (!__atomic_load_n(failed, __ATOMIC_RELAXED)) {
                    int x0, x1, y0, y1;
                    tile_rect(h->cw, band_y0[b + 1] - band_y0[b], ntx, tiles_y(band_y0[b + 1] - band_y0[b]), ti, &x0, &x1, &y0, &y1);
                    int re = render_tile(f, rdp, o, x0, x1, band_y0[b] + y0, band_y0[b] + y1);
                    if (re) { __atomic_store_n(err, re, __ATOMIC_RELAXED); __atomic_store_n(failed, 1, __ATOMIC_RELEASE); }
                }
                __atomic_add_fetch(&banddone[b], 1, __ATOMIC_RELEASE);
            }
        });
        e = *err;
    }
    free(band_y0); free(band_tiles); free(task); free(rowdone); free(banddone); free(failed); free(err);
    return e;
}

// Decodes the frame and, with o, renders it into the output. A VarDCT frame whose extra channels
// need no transform at the end is rendered as it goes (decode_wave), its colour planes a ring of rows.
static int decode_frame(Frame *f, const Output *o) {
    FrameHeader *h = &f->h;
    if (!(f->pool = xcalloc(1, sizeof(Pool)))) return ERR_NOMEM;
    TRY(lf_global(f));
    if (h->use_lf_frame) return ERR_UNSUP;
    size_t nb = (size_t)h->bw * h->bh;
    int threads = f->single ? 1 : f->threads;
    int wave = !h->modular && !f->gm.ntr && RING * h->gdim < h->ch && h->gy < (1 << 15) && tiles_x(h->cw) * tiles_y(h->gdim + 64) < (1 << 16);
    if (!h->modular) {
        if ((int64_t)h->bw * h->bh > ((int64_t)1 << 26)) return ERR_LIMIT;
        for (int c = 0; c < 3; c++) {
            if (!(f->lf[c] = xcalloc(nb, sizeof(float)))) return ERR_NOMEM;
            f->pxw[c] = (h->bw >> h->hs[c]) * 8; f->pxh[c] = (h->bh >> h->vs[c]) * 8;
            if (wave) f->pxh[c] = (RING * h->gdim) >> h->vs[c];   // (a multiple of the rows a group row has)
            if (!(f->px[c] = xmalloc((size_t)f->pxw[c] * f->pxh[c], sizeof(float)))) return ERR_NOMEM;
        }
        if (!(f->lfidx = xcalloc(nb, 1)) || !(f->acs = xmalloc(nb, 1)) || !(f->qf = xcalloc(nb, sizeof(int32_t))) ||
            !(f->sharp = xcalloc(nb, 1)) || !(f->ytox = xcalloc((size_t)cdiv(h->bw, 8) * cdiv(h->bh, 8), 1)) ||
            !(f->ytob = xcalloc((size_t)cdiv(h->bw, 8) * cdiv(h->bh, 8), 1))) return ERR_NOMEM;
        memset(f->acs, 0xff, nb);
    }
    int nlf = h->nlfgroups, *errs = xcalloc((size_t)imax(nlf, h->ngroups), sizeof(int));
    if (!errs) return ERR_NOMEM;
    nitro_parallel_n((size_t)nlf, threads, ^(size_t i) { errs[i] = lf_group(f, (int)i); });
    int e = 0;
    for (int i = 0; i < nlf && !e; i++) e = errs[i];
    if (!e && !h->modular && !h->skip_smooth) e = smooth_lf(f);
    if (!e && !h->modular) e = hf_global(f);
    if (!e && wave) {
        e = decode_wave(f, o, threads);
    } else if (!e) {
        nitro_parallel_n((size_t)h->ngroups, threads, ^(size_t i) { errs[i] = decode_group(f, (int)i); });
        for (int i = 0; i < h->ngroups && !e; i++) e = errs[i];
    }
    free(errs);
    if (!e && f->gm.nchan) e = modular_inverse(&f->gm);
    if (!e && f->gm.nchan != f->nb_color + f->m->num_ec) e = ERR_INVALID;
    if (!e && o && !wave) e = render_rows(f, o, 0, h->ch);
    return e;
}

// ---------------------------------------------------------------------------
// the image

typedef struct {
    Br br;
    Meta m;
    njxl_stream stream;
} Image;

static int read_headers(const uint8_t *data, size_t len, Image *im) {
    memset(im, 0, sizeof(*im));
    TRY(njxl_unwrap(data, len, &im->stream));
    br_init(&im->br, im->stream.data, im->stream.len);
    if (br_u(&im->br, 16) != 0x0aff) return ERR_INVALID;
    TRY(size_header(&im->br, &im->m.width, &im->m.height));
    TRY(image_metadata(&im->br, &im->m));
    if ((int64_t)im->m.width * im->m.height > ((int64_t)1 << 30)) return ERR_LIMIT;
    return 0;
}

static void image_free(Image *im) {
    free(im->m.icc);
    njxl_stream_free(&im->stream);
    memset(im, 0, sizeof(*im));
}

static void fill_info(const Image *im, njxl_info *info) {
    const Meta *m = &im->m;
    memset(info, 0, sizeof(*info));
    info->width = m->width; info->height = m->height; info->orientation = m->orientation;
    info->bits = m->bits; info->is_float = m->exp_bits != 0;
    info->grey = m->cspace == CS_GREY;
    info->xyb = m->xyb;
    info->animated = m->animated;
    info->alpha = 0;
    for (int i = 0; i < m->num_ec; i++)
        if (m->ec[i].type == EC_ALPHA) { info->alpha = 1; info->premultiplied = m->ec[i].alpha_associated; break; }
}

int njxl_info_read(const uint8_t *data, size_t len, njxl_info *info) {
    Image im;
    if (!info) return NJXL_INVALID;
    memset(info, 0, sizeof(*info));
    int e = read_headers(data, len, &im);
    if (!e) {
        fill_info(&im, info);
        Colour k;
        colour_init(&im.m, &k);
        if (k.mode == 1 && im.m.want_icc && !read_icc(&im.br, &im.m)) {   // (the pixels will be in its space)
            info->icc = im.m.icc; info->icc_len = im.m.icc_len;
            im.m.icc = NULL;
        }
    }
    image_free(&im);
    return e;
}

void njxl_info_free(njxl_info *info) {
    if (info) { free(info->icc); memset(info, 0, sizeof(*info)); }
}

int njxl_decode(const uint8_t *data, size_t len, unsigned flags, int format, void *pixels, size_t pitch, njxl_info *info) {
    Image im;
    njxl_info tmp;
    if (!info) info = &tmp;
    memset(info, 0, sizeof(*info));
    if (!pixels || (format != NJXL_BGRA8 && format != NJXL_RGBA16)) return NJXL_INVALID;
    int e = read_headers(data, len, &im);
    Meta *m = &im.m;
    Br *br = &im.br;
    if (!e && m->want_icc) e = read_icc(br, m);
    if (e) { image_free(&im); return e; }
    fill_info(&im, info);
    Colour k;
    colour_init(m, &k);
    if (k.mode == 1 && m->icc) {   // the pixels stay in the profile's space
        info->icc = m->icc; info->icc_len = m->icc_len;
        m->icc = NULL;
    }
    int threads = flags & NJXL_SINGLE_THREAD ? 1 : 0;
    if (m->have_preview) {   // skip the preview frame
        Frame f;
        memset(&f, 0, sizeof f);
        f.m = m;
        e = frame_header(br, m, m->preview_w, m->preview_h, &f.h);
        if (!e) e = read_toc(br, &f);
        frame_free(&f);
    }
    int done = 0, nframes = 0;
    while (!e && !done) {
        Frame f;
        memset(&f, 0, sizeof f);
        f.m = m; f.threads = threads;
        e = frame_header(br, m, m->width, m->height, &f.h);
        if (!e) e = read_toc(br, &f);
        if (!e && ++nframes > 100000) e = ERR_LIMIT;
        if (!e && (f.h.type == FT_REGULAR || f.h.type == FT_SKIPPROG)) {
            FrameHeader *h = &f.h;
            int full = h->x0 == 0 && h->y0 == 0 && h->w == m->width && h->h == m->height && h->blend.mode == BLEND_REPLACE;
            if (!full) e = ERR_UNSUP;   // layers, cropped animation frames
            done = h->is_last || h->duration;   // (shown: the frame rendered; the others decoded only)
            Output o = {format, pixels, pitch, -1, 0};
            for (int i = 0; i < m->num_ec; i++)
                if (m->ec[i].type == EC_ALPHA) { o.alpha_ec = i; o.premul_in = m->ec[i].alpha_associated; break; }
            if (!e) e = decode_frame(&f, done ? &o : NULL);
        } else if (!e && f.h.is_last) {
            e = ERR_INVALID;
        }
        frame_free(&f);
    }
    image_free(&im);
    if (e) njxl_info_free(info);
    return e;
}

int njxl_size(const uint8_t *data, size_t len, int *width, int *height, int *orientation) {
    static const uint8_t signature[12] = {0,0,0,12,'J','X','L',' ',13,10,135,10};
    const uint8_t *cs = NULL;
    size_t cl = 0;
    if (!data) return -1;
    if (codestream_magic(data, len)) {
        cs = data; cl = len;
    } else if (len >= 12 && !memcmp(data, signature, 12)) {   // the first codestream box; it may be cut off
        size_t pos = 12;
        while (!cs && pos + 8 <= len) {
            uint64_t size = be32(data + pos), hdr = 8;
            uint32_t type = be32(data + pos + 4);
            if (size == 1) {
                if (pos + 16 > len) return -1;
                size = (uint64_t)be32(data + pos + 8) << 32 | be32(data + pos + 12);
                hdr = 16;
            }
            if (type == TAG('j','x','l','c') || type == TAG('j','x','l','p')) {
                size_t skip = (size_t)hdr + (type == TAG('j','x','l','p') ? 4 : 0);
                if (pos + skip > len) return -1;
                cs = data + pos + skip;
                cl = len - pos - skip;
                if (size && size - skip < cl) cl = (size_t)(size - skip);
                break;
            }
            if (!size || size < hdr || size > len - pos) return -1;
            pos += (size_t)size;
        }
    }
    if (!cs) return -1;
    Meta m;
    Br br;
    memset(&m, 0, sizeof m);
    br_init(&br, cs, cl);
    if (br_u(&br, 16) != 0x0aff || size_header(&br, &m.width, &m.height) || image_metadata(&br, &m)) return -1;
    *width = m.width; *height = m.height;
    if (orientation) *orientation = m.orientation;
    return 0;
}
