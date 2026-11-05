// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrocrypt: hashes and ciphers for the readers (nitropdf's security handler uses them): MD5,
// SHA-256/384/512, RC4 and AES-128/192/256 in CBC mode. Our own, so that every system computes
// the same.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void nc_md5(const void *data, size_t len, uint8_t out[16]);
void nc_sha256(const void *data, size_t len, uint8_t out[32]);
void nc_sha384(const void *data, size_t len, uint8_t out[48]);
void nc_sha512(const void *data, size_t len, uint8_t out[64]);

// in == out allowed
void nc_rc4(const uint8_t *key, size_t keyLen, const uint8_t *in, size_t len, uint8_t *out);

typedef struct nc_aes {
    uint32_t ek[60];   // the expanded key
    int rounds;
} nc_aes;

int nc_aes_init(nc_aes *a, const uint8_t *key, size_t keyLen);   // key: 16, 24 or 32 bytes; 0 if not
// len a multiple of 16; iv updated (in == out allowed)
void nc_aes_decrypt_cbc(const nc_aes *a, uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out);
void nc_aes_encrypt_cbc(const nc_aes *a, uint8_t iv[16], const uint8_t *in, size_t len, uint8_t *out);

#ifdef __cplusplus
}
#endif
