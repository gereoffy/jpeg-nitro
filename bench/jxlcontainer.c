// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// Container tests only; these do not verify JPEG XL pixel decoding.
#include "../src/nitrojxl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static const uint8_t stream[] = {255,10,1,2,3,4,5,6};
static const uint8_t header[] = {
    0,0,0,12,'J','X','L',' ',13,10,135,10,
    0,0,0,20,'f','t','y','p','j','x','l',' ',0,0,0,0,'j','x','l',' '
};
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static size_t box(uint8_t *p, const char *type, const uint8_t *d, size_t n, int extended) {
    size_t h = extended ? 16 : 8;
    put32(p, extended ? 1 : (uint32_t)(n + h)); memcpy(p + 4, type, 4);
    if (extended) { put32(p + 8, 0); put32(p + 12, (uint32_t)(n + h)); }
    if (n) memcpy(p + h, d, n);
    return h + n;
}
static size_t part(uint8_t *p, uint32_t index, const uint8_t *d, size_t n) {
    uint8_t temp[64]; CHECK(n + 4 <= sizeof temp);
    put32(temp, index); if (n) memcpy(temp + 4, d, n);
    return box(p, "jxlp", temp, n + 4, 0);
}
static void expect(const uint8_t *d, size_t n, int rc, int level, int owned) {
    njxl_stream s; memset(&s, 0x55, sizeof s);
    CHECK(njxl_unwrap(d, n, &s) == rc);
    if (!rc) {
        CHECK(s.len == sizeof stream && !memcmp(s.data, stream, sizeof stream));
        CHECK(s.level == level && !!s.storage == owned);
    } else CHECK(!s.data && !s.len && !s.storage && !s.level);
    njxl_stream_free(&s); njxl_stream_free(&s);
    CHECK(!s.data && !s.len && !s.storage && !s.level);
}
static void probe(const uint8_t *d, size_t n) {
    njxl_stream s;
    int rc = njxl_unwrap(d, n, &s);
    if (!rc) CHECK(s.data && s.len >= 2 && s.data[0] == 255 && s.data[1] == 10);
    else CHECK(!s.data && !s.storage && !s.len && !s.level);
    njxl_stream_free(&s);
}
static void fixtures(void) {
    uint8_t d[512], copy[512], level = 10;
    size_t n;
    expect(NULL, 0, NJXL_INVALID, 0, 0);
    CHECK(njxl_unwrap(stream, sizeof stream, NULL) == NJXL_INVALID);
    njxl_stream_free(NULL);
    expect(stream, sizeof stream, 0, 0, 0);
    for (int ext = 0; ext < 2; ext++) {
        memcpy(d, header, sizeof header); n = sizeof header;
        n += box(d + n, "jxlc", stream, sizeof stream, ext);
        expect(d, n, 0, 5, 0);
        for (size_t i = 0; i < n; i++) expect(d, i, NJXL_INVALID, 0, 0);
        if (ext) { put32(d + sizeof header + 8, 0xffffffff); expect(d, n, NJXL_INVALID, 0, 0); }
        else { put32(d + sizeof header, 0); expect(d, n, 0, 5, 0); }
    }
    memcpy(d, header, sizeof header); n = sizeof header;
    n += box(d + n, "jxll", &level, 1, 0);
    size_t first = n;
    n += part(d + n, 0, stream, 1); // magic split between parts
    n += box(d + n, "Exif", (const uint8_t *)"metadata", 8, 1);
    n += part(d + n, 1, NULL, 0);   // empty middle part
    size_t final = n;
    n += part(d + n, 0x80000002u, stream + 1, sizeof stream - 1);
    expect(d, n, 0, 10, 1);
    for (size_t i = 0; i < n; i++) expect(d, i, NJXL_INVALID, 0, 0);
    size_t complete = n;
    n += box(d + n, "xml ", (const uint8_t *)"xmp", 3, 0);
    n += box(d + n, "abcd", NULL, 0, 0);
    expect(d, n, 0, 10, 1);
    memcpy(copy, d, n);
    put32(d + final + 8, 2); expect(d, n, NJXL_INVALID, 0, 0); // missing last marker
    memcpy(d, copy, n);
    put32(d + final + 8, 0x80000001u); expect(d, n, NJXL_INVALID, 0, 0); // duplicate
    memcpy(d, copy, n);
    put32(d + first + 8, 1); expect(d, n, NJXL_INVALID, 0, 0); // missing part zero
    memcpy(d, copy, n);
    put32(d + first + 8, 0x80000000u); expect(d, n, NJXL_INVALID, 0, 0); // early last
    memcpy(d, copy, n);
    d[24] = 1; expect(d, n, NJXL_UNSUPPORTED, 0, 0); // container version
    memcpy(d, copy, n);
    d[40] = 11; expect(d, n, NJXL_UNSUPPORTED, 0, 0); // future level
    memcpy(d, copy, n);
    size_t more = part(d + n, 0x80000003u, NULL, 0);
    expect(d, n + more, NJXL_INVALID, 0, 0);
    more = box(d + n, "jxlc", stream, sizeof stream, 0);
    expect(d, n + more, NJXL_INVALID, 0, 0); // mixed codestream types
    // Length, type, index and flag mutations: failure must leave a releasable empty result.
    for (size_t i = 0; i < n; i++) for (unsigned bit = 0; bit < 8; bit++) {
        memcpy(d, copy, n); d[i] ^= (uint8_t)(1u << bit); probe(d, n);
    }
    memcpy(d, copy, n); put32(d + complete, 7); expect(d, n, NJXL_INVALID, 0, 0);
    memcpy(d, header, sizeof header); n = sizeof header;
    n += box(d + n, "jxlc", stream, sizeof stream, 0);
    more = box(d + n, "jxlc", stream, sizeof stream, 0);
    expect(d, n + more, NJXL_INVALID, 0, 0);
    puts("Synthetic framing, truncation and mutation tests passed.");
}
int main(int argc, char **argv) {
    fixtures();
    int failures = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        CHECK(f && !fseek(f, 0, SEEK_END));
        long length = ftell(f); CHECK(length > 0); rewind(f);
        size_t n = (size_t)length;
        uint8_t *data = malloc(n); CHECK(data && fread(data, 1, n, f) == n); fclose(f);
        njxl_stream s;
        int rc = njxl_unwrap(data, n, &s);
        if (rc) { fprintf(stderr, "%s: container error %d\n", argv[a], rc); failures++; }
        else {
            njxl_stream raw;
            CHECK(!njxl_unwrap(s.data, s.len, &raw));
            CHECK(raw.data == s.data && raw.len == s.len && !raw.storage);
            njxl_stream_free(&raw);
            printf("%s: %zu codestream bytes, level %d, %s\n", argv[a], s.len,
                   s.level, s.storage ? "joined" : "borrowed");
        }
        njxl_stream_free(&s); free(data);
    }
    printf("%d/%d input containers passed (pixel decoding not tested).\n", argc - 1 - failures, argc - 1);
    return failures != 0;
}
