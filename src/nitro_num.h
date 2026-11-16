// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_num: a file's numbers read and written with '.' as the decimal point whatever the process's
// locale is. The C library's strtod / printf follow LC_NUMERIC, and a program that takes the user's
// locale (GTK does it for every application) has ',' there in many languages: "1.5" is read as 1 and
// 1.5 is written as "1,5". Header only (C and C++).
#ifndef NITRO_NUM_H
#define NITRO_NUM_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// the locale's decimal point (into p, at most 7 bytes and the 0), its length returned
static inline size_t nn_point(char p[8]) {
    char b[16];
    int n = snprintf(b, sizeof b, "%.1f", 0.5);   // "0", the decimal point, "5"
    if (n < 3 || n > 9) return p[0] = '.', p[1] = 0, 1;
    memcpy(p, b + 1, (size_t)n - 2);
    p[n - 2] = 0;
    return (size_t)n - 2;
}

static inline int nn_digit(char c, int hex) {
    return (c >= '0' && c <= '9') || (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
}

// strtod reading the number as the C locale does (the same value, the same end)
static inline double nn_strtod(const char *s, char **end) {
    char *e = NULL;
    double v = strtod(s, &e);
    // what the C locale takes for the number: [sign] digits [. digits] [e [sign] digits], or the
    // hexadecimal one: 0x, hexadecimal digits and p for the exponent
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    const char *b = p;
    if (*p == '+' || *p == '-') p++;
    if (*p == 'i' || *p == 'I' || *p == 'n' || *p == 'N') {   // (inf, nan: no decimal point in them)
        if (end) *end = e;
        return v;
    }
    int hex = p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && (nn_digit(p[2], 1) || (p[2] == '.' && nn_digit(p[3], 1)));
    if (hex) p += 2;
    const char *d = p, *dot = NULL;
    while (nn_digit(*p, hex)) p++;
    if (*p == '.') {
        dot = p++;
        while (nn_digit(*p, hex)) p++;
    }
    if (p - d == (dot ? 1 : 0)) {   // not a number (the locale's point may have made one of it)
        if (end) *end = (char *)s;
        return 0;
    }
    if (*p == (hex ? 'p' : 'e') || *p == (hex ? 'P' : 'E')) {
        const char *q = p + 1;
        if (*q == '+' || *q == '-') q++;
        if (nn_digit(*q, 0)) {
            while (nn_digit(*q, 0)) q++;
            p = q;
        }
    }
    if (e == p) {   // strtod read just that
        if (end) *end = e;
        return v;
    }
    // the locale reads it another way: its decimal point put in the place of the '.', read again
    char point[8], small[128];
    size_t pl = dot ? nn_point(point) : 0, n = (size_t)(p - b), at = dot ? (size_t)(dot - b) : n;
    char *t = n + pl < sizeof small ? small : (char *)malloc(n + pl + 1), *o = t, *te = NULL;
    if (!t) {
        if (end) *end = (char *)s;
        return 0;
    }
    for (const char *c = b; c < p; c++) {
        if (c == dot) memcpy(o, point, pl), o += pl;
        else *o++ = *c;
    }
    *o = 0;
    v = strtod(t, &te);
    size_t used = (size_t)(te - t);
    if (used > at) used -= pl - 1;   // (in the file's text the point is one character)
    if (t != small) free(t);
    if (end) *end = used ? (char *)b + used : (char *)s;
    return v;
}

// a text printf has just written numbers into: the locale's decimal point made '.' (the text must have
// none of its own; in place: it does not get longer)
static inline void nn_cpoint(char *b) {
    char point[8];
    size_t pl = nn_point(point);
    if (pl == 1 && point[0] == '.') return;
    char *o = b;
    for (const char *c = b; *c;) {
        if (!strncmp(c, point, pl)) *o++ = '.', c += pl;
        else *o++ = *c++;
    }
    *o = 0;
}

#endif
