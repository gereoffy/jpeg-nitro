// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrocrypt: MD5 (RFC 1321), SHA-256/384/512 (FIPS 180-4), RC4 and AES (FIPS 197) in CBC
// mode, plainly written: keys are short, and what they encrypt is decrypted once.
#include "nitrocrypt.h"

#include <string.h>

static inline uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
static inline uint32_t ror(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
static inline uint64_t ror64(uint64_t x, int c) { return (x >> c) | (x << (64 - c)); }

// --- MD5 ---------------------------------------------------------------------------

static const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
    0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
    0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
    0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
    0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const int MD5_R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5Block(uint32_t h[4], const uint8_t *p) {
    uint32_t w[16];
    for (int k = 0; k < 16; k++) w[k] = p[4 * k] | p[4 * k + 1] << 8 | p[4 * k + 2] << 16 | (uint32_t)p[4 * k + 3] << 24;
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int k = 0; k < 64; k++) {
        uint32_t f;
        int g;
        if (k < 16) f = (b & c) | (~b & d), g = k;
        else if (k < 32) f = (d & b) | (~d & c), g = (5 * k + 1) & 15;
        else if (k < 48) f = b ^ c ^ d, g = (3 * k + 5) & 15;
        else f = c ^ (b | ~d), g = (7 * k) & 15;
        uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + MD5_K[k] + w[g], MD5_R[k]);
        a = t;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d;
}

// --- SHA-2 -------------------------------------------------------------------------

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256Block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int k = 0; k < 16; k++) w[k] = (uint32_t)p[4 * k] << 24 | p[4 * k + 1] << 16 | p[4 * k + 2] << 8 | p[4 * k + 3];
    for (int k = 16; k < 64; k++) {
        uint32_t s0 = ror(w[k - 15], 7) ^ ror(w[k - 15], 18) ^ (w[k - 15] >> 3);
        uint32_t s1 = ror(w[k - 2], 17) ^ ror(w[k - 2], 19) ^ (w[k - 2] >> 10);
        w[k] = w[k - 16] + s0 + w[k - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int k = 0; k < 64; k++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + SHA256_K[k] + w[k];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

static const uint64_t SHA512_K[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019,
    0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3,
    0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725,
    0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001,
    0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb,
    0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207,
    0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a,
    0x5fcb6fab3ad6faec, 0x6c44198c4a475817};

static void sha512Block(uint64_t h[8], const uint8_t *p) {
    uint64_t w[80];
    for (int k = 0; k < 16; k++) {
        w[k] = 0;
        for (int b = 0; b < 8; b++) w[k] = w[k] << 8 | p[8 * k + b];
    }
    for (int k = 16; k < 80; k++) {
        uint64_t s0 = ror64(w[k - 15], 1) ^ ror64(w[k - 15], 8) ^ (w[k - 15] >> 7);
        uint64_t s1 = ror64(w[k - 2], 19) ^ ror64(w[k - 2], 61) ^ (w[k - 2] >> 6);
        w[k] = w[k - 16] + s0 + w[k - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int k = 0; k < 80; k++) {
        uint64_t t1 = hh + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + SHA512_K[k] + w[k];
        uint64_t t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

// the Merkle-Damgård padding shared by all: block (64 or 128 bytes), the length field's size and
// order; blockFn(state, block) for each block
static void mdHash(const uint8_t *p, size_t len, size_t block, int bigEndian, void (*blockFn)(void *, const uint8_t *), void *state) {
    size_t full = len / block * block;
    for (size_t o = 0; o < full; o += block) blockFn(state, p + o);
    uint8_t tail[256] = {0};
    size_t r = len - full;
    memcpy(tail, p + full, r);
    tail[r] = 0x80;
    size_t lenBytes = block == 64 ? 8 : 16;
    size_t total = r + 1 + lenBytes <= block ? block : 2 * block;
    uint64_t bits = (uint64_t)len * 8;
    for (int b = 0; b < 8; b++) {
        if (bigEndian) tail[total - 1 - b] = (uint8_t)(bits >> (8 * b));
        else tail[total - lenBytes + b] = (uint8_t)(bits >> (8 * b));
    }
    for (size_t o = 0; o < total; o += block) blockFn(state, tail + o);
}

static void md5Fn(void *h, const uint8_t *b) { md5Block((uint32_t *)h, b); }
static void sha256Fn(void *h, const uint8_t *b) { sha256Block((uint32_t *)h, b); }
static void sha512Fn(void *h, const uint8_t *b) { sha512Block((uint64_t *)h, b); }

void nc_md5(const void *data, size_t len, uint8_t out[16]) {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    mdHash((const uint8_t *)data, len, 64, 0, md5Fn, h);
    for (int k = 0; k < 16; k++) out[k] = (uint8_t)(h[k / 4] >> (8 * (k % 4)));
}

void nc_sha256(const void *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    mdHash((const uint8_t *)data, len, 64, 1, sha256Fn, h);
    for (int k = 0; k < 32; k++) out[k] = (uint8_t)(h[k / 4] >> (24 - 8 * (k % 4)));
}

void nc_sha384(const void *data, size_t len, uint8_t out[48]) {
    uint64_t h[8] = {0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
                     0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4};
    mdHash((const uint8_t *)data, len, 128, 1, sha512Fn, h);
    for (int k = 0; k < 48; k++) out[k] = (uint8_t)(h[k / 8] >> (56 - 8 * (k % 8)));
}

void nc_sha512(const void *data, size_t len, uint8_t out[64]) {
    uint64_t h[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                     0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};
    mdHash((const uint8_t *)data, len, 128, 1, sha512Fn, h);
    for (int k = 0; k < 64; k++) out[k] = (uint8_t)(h[k / 8] >> (56 - 8 * (k % 8)));
}

void nc_rc4(const uint8_t *key, size_t keyLen, const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t S[256];
    for (int k = 0; k < 256; k++) S[k] = (uint8_t)k;
    if (keyLen == 0) keyLen = 1, key = (const uint8_t *)"";
    for (int k = 0, j = 0; k < 256; k++) {
        j = (j + S[k] + key[k % keyLen]) & 255;
        uint8_t t = S[k];
        S[k] = S[j], S[j] = t;
    }
    int i = 0, j = 0;
    for (size_t k = 0; k < len; k++) {
        i = (i + 1) & 255;
        j = (j + S[i]) & 255;
        uint8_t t = S[i];
        S[i] = S[j], S[j] = t;
        out[k] = in[k] ^ S[(S[i] + S[j]) & 255];
    }
}

// --- AES ---------------------------------------------------------------------------

// (constant: no first-use initialization to race over)
static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

static const uint8_t INV[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

static inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ (x & 0x80 ? 0x1b : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

static inline uint32_t subWord(uint32_t w) {
    return (uint32_t)SBOX[w >> 24] << 24 | (uint32_t)SBOX[(w >> 16) & 255] << 16 | (uint32_t)SBOX[(w >> 8) & 255] << 8 | SBOX[w & 255];
}

int nc_aes_init(nc_aes *a, const uint8_t *key, size_t keyLen) {
    if (keyLen != 16 && keyLen != 24 && keyLen != 32) return 0;
    int nk = (int)keyLen / 4;
    a->rounds = nk + 6;
    int total = 4 * (a->rounds + 1);
    for (int k = 0; k < nk; k++) a->ek[k] = (uint32_t)key[4 * k] << 24 | key[4 * k + 1] << 16 | key[4 * k + 2] << 8 | key[4 * k + 3];
    uint8_t rcon = 1;
    for (int k = nk; k < total; k++) {
        uint32_t t = a->ek[k - 1];
        if (k % nk == 0) {
            t = subWord((t << 8) | (t >> 24)) ^ (uint32_t)rcon << 24;
            rcon = xtime(rcon);
        } else if (nk > 6 && k % nk == 4) t = subWord(t);
        a->ek[k] = a->ek[k - nk] ^ t;
    }
    return 1;
}

static inline void addKey(uint8_t s[16], const uint32_t *w) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) s[4 * c + r] ^= (uint8_t)(w[c] >> (24 - 8 * r));
}

static void encryptBlock(const nc_aes *a, uint8_t s[16]) {
    addKey(s, a->ek);
    for (int round = 1; round <= a->rounds; round++) {
        for (int k = 0; k < 16; k++) s[k] = SBOX[s[k]];
        uint8_t t[16];   // ShiftRows
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) & 3) + r];
        if (round != a->rounds)
            for (int c = 0; c < 4; c++) {   // MixColumns
                uint8_t *x = t + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3], all = a0 ^ a1 ^ a2 ^ a3;
                x[0] ^= all ^ xtime(a0 ^ a1);
                x[1] ^= all ^ xtime(a1 ^ a2);
                x[2] ^= all ^ xtime(a2 ^ a3);
                x[3] ^= all ^ xtime(a3 ^ a0);
            }
        memcpy(s, t, 16);
        addKey(s, a->ek + 4 * round);
    }
}

static void decryptBlock(const nc_aes *a, uint8_t s[16]) {
    addKey(s, a->ek + 4 * a->rounds);
    for (int round = a->rounds - 1; round >= 0; round--) {
        uint8_t t[16];   // InvShiftRows, InvSubBytes
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[4 * ((c + r) & 3) + r] = INV[s[4 * c + r]];
        memcpy(s, t, 16);
        addKey(s, a->ek + 4 * round);
        if (round)
            for (int c = 0; c < 4; c++) {   // InvMixColumns
                uint8_t *x = s + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
                x[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                x[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                x[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                x[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
    }
}

void nc_aes_decrypt_cbc(const nc_aes *a, uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out) {
    for (size_t o = 0; o + 16 <= len; o += 16) {
        uint8_t c[16], s[16];
        memcpy(c, in + o, 16);
        memcpy(s, c, 16);
        decryptBlock(a, s);
        for (int k = 0; k < 16; k++) out[o + k] = s[k] ^ iv[k];
        memcpy(iv, c, 16);
    }
}

void nc_aes_encrypt_cbc(const nc_aes *a, uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out) {
    for (size_t o = 0; o + 16 <= len; o += 16) {
        uint8_t s[16];
        for (int k = 0; k < 16; k++) s[k] = in[o + k] ^ iv[k];
        encryptBlock(a, s);
        memcpy(out + o, s, 16);
        memcpy(iv, s, 16);
    }
}
