// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitropdf_crypt: what the Standard security handler needs (internal): MD5, SHA-256/384/512,
// RC4 and AES-128/256 in CBC mode. Our own, so that every system decrypts the same way.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nitropdf {

void md5(const void *data, size_t len, uint8_t out[16]);
void sha256(const void *data, size_t len, uint8_t out[32]);
void sha384(const void *data, size_t len, uint8_t out[48]);
void sha512(const void *data, size_t len, uint8_t out[64]);

// in == out allowed
void rc4(const uint8_t *key, size_t keyLen, const uint8_t *in, size_t len, uint8_t *out);

struct Aes {
    uint32_t ek[60], dk[60];
    int rounds = 0;
    bool init(const uint8_t *key, size_t keyLen);   // 16 or 32 bytes
    // len a multiple of 16; iv updated (in == out allowed)
    void decryptCbc(uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out) const;
    void encryptCbc(uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out) const;
};

}  // namespace nitropdf
