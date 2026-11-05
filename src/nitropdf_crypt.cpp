// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitropdf_crypt: MD5 (RFC 1321), SHA-256/384/512 (FIPS 180-4), RC4 and AES (FIPS 197) in CBC
// mode, plainly written: a PDF's keys are short, and its encrypted streams are decrypted once.
#include "nitropdf_crypt.h"

#include <cstring>

namespace nitropdf {

namespace {

inline uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
inline uint32_t ror(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
inline uint64_t ror64(uint64_t x, int c) { return (x >> c) | (x << (64 - c)); }

// --- MD5 ---------------------------------------------------------------------------

const uint32_t MD5_K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
    0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
    0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
    0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
    0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const int MD5_R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

void md5Block(uint32_t h[4], const uint8_t *p) {
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

const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void sha256Block(uint32_t h[8], const uint8_t *p) {
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

const uint64_t SHA512_K[80] = {
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

void sha512Block(uint64_t h[8], const uint8_t *p) {
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

// the Merkle-Damgård padding shared by all: block (64 or 128 bytes), the length field's size and order
template <class F> void mdHash(const uint8_t *p, size_t len, size_t block, bool bigEndian, F blockFn) {
    size_t full = len / block * block;
    for (size_t o = 0; o < full; o += block) blockFn(p + o);
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
    for (size_t o = 0; o < total; o += block) blockFn(tail + o);
}

void sha512Family(const void *data, size_t len, uint64_t h[8]) {
    mdHash((const uint8_t *)data, len, 128, true, [&](const uint8_t *b) { sha512Block(h, b); });
}

// --- AES ---------------------------------------------------------------------------

uint8_t SBOX[256], INV[256];
bool aesReady = false;

inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ (x & 0x80 ? 0x1b : 0)); }
uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

void aesTables() {
    if (aesReady) return;
    uint8_t p = 1, q = 1;
    do {   // p runs through the powers of 3, q through those of its inverse
        p = p ^ xtime(p);
        q ^= q << 1;
        q ^= q << 2;
        q ^= q << 4;
        if (q & 0x80) q ^= 0x09;
        uint8_t x = q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^ (uint8_t)((q << 3) | (q >> 5)) ^
                    (uint8_t)((q << 4) | (q >> 4));
        SBOX[p] = x ^ 0x63;
    } while (p != 1);
    SBOX[0] = 0x63;
    for (int k = 0; k < 256; k++) INV[SBOX[k]] = (uint8_t)k;
    aesReady = true;
}

inline uint32_t subWord(uint32_t w) {
    return (uint32_t)SBOX[w >> 24] << 24 | (uint32_t)SBOX[(w >> 16) & 255] << 16 | (uint32_t)SBOX[(w >> 8) & 255] << 8 | SBOX[w & 255];
}

}  // namespace

void md5(const void *data, size_t len, uint8_t out[16]) {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    mdHash((const uint8_t *)data, len, 64, false, [&](const uint8_t *b) { md5Block(h, b); });
    for (int k = 0; k < 16; k++) out[k] = (uint8_t)(h[k / 4] >> (8 * (k % 4)));
}

void sha256(const void *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    mdHash((const uint8_t *)data, len, 64, true, [&](const uint8_t *b) { sha256Block(h, b); });
    for (int k = 0; k < 32; k++) out[k] = (uint8_t)(h[k / 4] >> (24 - 8 * (k % 4)));
}

void sha384(const void *data, size_t len, uint8_t out[48]) {
    uint64_t h[8] = {0xcbbb9d5dc1059ed8, 0x629a292a367cd507, 0x9159015a3070dd17, 0x152fecd8f70e5939,
                     0x67332667ffc00b31, 0x8eb44a8768581511, 0xdb0c2e0d64f98fa7, 0x47b5481dbefa4fa4};
    sha512Family(data, len, h);
    for (int k = 0; k < 48; k++) out[k] = (uint8_t)(h[k / 8] >> (56 - 8 * (k % 8)));
}

void sha512(const void *data, size_t len, uint8_t out[64]) {
    uint64_t h[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
                     0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};
    sha512Family(data, len, h);
    for (int k = 0; k < 64; k++) out[k] = (uint8_t)(h[k / 8] >> (56 - 8 * (k % 8)));
}

void rc4(const uint8_t *key, size_t keyLen, const uint8_t *in, size_t len, uint8_t *out) {
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

bool Aes::init(const uint8_t *key, size_t keyLen) {
    if (keyLen != 16 && keyLen != 24 && keyLen != 32) return false;
    aesTables();
    int nk = (int)keyLen / 4;
    rounds = nk + 6;
    int total = 4 * (rounds + 1);
    for (int k = 0; k < nk; k++) ek[k] = (uint32_t)key[4 * k] << 24 | key[4 * k + 1] << 16 | key[4 * k + 2] << 8 | key[4 * k + 3];
    uint8_t rcon = 1;
    for (int k = nk; k < total; k++) {
        uint32_t t = ek[k - 1];
        if (k % nk == 0) {
            t = subWord((t << 8) | (t >> 24)) ^ (uint32_t)rcon << 24;
            rcon = xtime(rcon);
        } else if (nk > 6 && k % nk == 4) t = subWord(t);
        ek[k] = ek[k - nk] ^ t;
    }
    for (int k = 0; k < total; k++) dk[k] = ek[k];
    return true;
}

namespace {

inline void addKey(uint8_t s[16], const uint32_t *w) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) s[4 * c + r] ^= (uint8_t)(w[c] >> (24 - 8 * r));
}

void encryptBlock(const Aes &a, uint8_t s[16]) {
    addKey(s, a.ek);
    for (int round = 1; round <= a.rounds; round++) {
        for (int k = 0; k < 16; k++) s[k] = SBOX[s[k]];
        uint8_t t[16];   // ShiftRows
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[4 * c + r] = s[4 * ((c + r) & 3) + r];
        if (round != a.rounds)
            for (int c = 0; c < 4; c++) {   // MixColumns
                uint8_t *x = t + 4 * c, a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3], all = a0 ^ a1 ^ a2 ^ a3;
                x[0] ^= all ^ xtime(a0 ^ a1);
                x[1] ^= all ^ xtime(a1 ^ a2);
                x[2] ^= all ^ xtime(a2 ^ a3);
                x[3] ^= all ^ xtime(a3 ^ a0);
            }
        memcpy(s, t, 16);
        addKey(s, a.ek + 4 * round);
    }
}

void decryptBlock(const Aes &a, uint8_t s[16]) {
    addKey(s, a.dk + 4 * a.rounds);
    for (int round = a.rounds - 1; round >= 0; round--) {
        uint8_t t[16];   // InvShiftRows, InvSubBytes
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) t[4 * ((c + r) & 3) + r] = INV[s[4 * c + r]];
        memcpy(s, t, 16);
        addKey(s, a.dk + 4 * round);
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

}  // namespace

void Aes::decryptCbc(uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out) const {
    for (size_t o = 0; o + 16 <= len; o += 16) {
        uint8_t c[16], s[16];
        memcpy(c, in + o, 16);
        memcpy(s, c, 16);
        decryptBlock(*this, s);
        for (int k = 0; k < 16; k++) out[o + k] = s[k] ^ iv[k];
        memcpy(iv, c, 16);
    }
}

void Aes::encryptCbc(uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out) const {
    for (size_t o = 0; o + 16 <= len; o += 16) {
        uint8_t s[16];
        for (int k = 0; k < 16; k++) s[k] = in[o + k] ^ iv[k];
        encryptBlock(*this, s);
        memcpy(out + o, s, 16);
        memcpy(iv, s, 16);
    }
}

}  // namespace nitropdf
