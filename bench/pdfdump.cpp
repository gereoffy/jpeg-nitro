// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// pdfdump: nitropdf's view of a PDF, to compare with pdfparse3's (bench/pdfdump.py writes the
// same format): every object found, its value written canonically, its stream's decoded length
// and MD5; the pages, their boxes and their content's operator count.
//
//   c++ -std=c++17 -O2 -Isrc bench/pdfdump.cpp src/nitropdf.cpp src/nitrocrypt.cpp -lz -o bench/pdfdump
//   bench/pdfdump [-q] file.pdf      (-q: the summary only)
#include "nitrocrypt.h"
#include "nitropdf.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace nitropdf;

static std::string hexs(const uint8_t *p, size_t n) {
    std::string s;
    char b[3];
    for (size_t k = 0; k < n; k++) snprintf(b, sizeof b, "%02x", p[k]), s += b;
    return s;
}

static std::string md5s(const void *p, size_t n) {
    uint8_t h[16];
    md5(p, n, h);
    return hexs(h, 4);
}

static std::string name(std::string_view s) {
    std::string out = "/";
    for (unsigned char c : s) {
        if (c <= 32 || c >= 127 || strchr("()<>[]{}/%#", c)) {
            char b[4];
            snprintf(b, sizeof b, "#%02x", c);
            out += b;
        } else out += (char)c;
    }
    return out;
}

static std::string fmtReal(double r) {
    char b[64];
    snprintf(b, sizeof b, "%.6g", r);
    return b;
}

static std::string canon(Val v, int depth = 0) {
    if (depth > 70) return "?";
    switch (v.t) {
    case NUL: return "null";
    case BOOL: return v.i ? "true" : "false";
    case INT: return std::to_string(v.i);
    case REAL: return fmtReal(v.r);
    case STR: return v.n <= 32 ? "<" + hexs((const uint8_t *)v.s, v.n) + ">" : "<" + std::to_string(v.n) + ":" + md5s(v.s, v.n) + ">";
    case NAME: return name(v.str());
    case KW: return std::string(v.str());
    case REF: return "R" + std::to_string(v.oid());
    case ARRAY: {
        std::string s = "[";
        for (size_t k = 0; k < v.n; k++) s += (k ? " " : "") + canon(v.at(k), depth + 1);
        return s + "]";
    }
    case DICT: {
        std::map<std::string, std::string> m;   // (sorted, the last of the same keys)
        for (size_t k = 0; k < v.n; k++) m[std::string(v.v[2 * k].str())] = canon(v.v[2 * k + 1], depth + 1);
        std::string s = "<<";
        bool first = true;
        for (auto &kv : m) s += (first ? "" : " ") + name(kv.first) + " " + kv.second, first = false;
        return s + ">>";
    }
    }
    return "?";
}

int main(int argc, char **argv) {
    bool quiet = false;
    int a = 1;
    if (a < argc && !strcmp(argv[a], "-q")) quiet = true, a++;
    if (a >= argc) {
        fprintf(stderr, "usage: pdfdump [-q] file.pdf\n");
        return 1;
    }
    FILE *f = fopen(argv[a], "rb");
    if (!f) return 1;
    std::string data;
    char buf[1 << 16];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, r);
    fclose(f);
    auto t0 = std::chrono::steady_clock::now();
    Doc doc;
    if (!doc.open(data.data(), data.size())) {
        printf("notpdf\n");
        return 0;
    }
    std::vector<uint32_t> ids = doc.objectIds(true);
    size_t nstreams = 0, nerr = 0;
    std::string out;
    for (uint32_t oid : ids) {
        Val v = doc.object(oid);
        std::string line = "obj " + std::to_string(oid) + " " + canon(v);
        if (const Stream *s = doc.stream(oid)) {
            Decoded dec;
            bool ok = doc.decode(*s, dec);
            nstreams++, nerr += !ok;
            line += " stream dec=" + std::to_string(dec.data.size()) + " md5=" + md5s(dec.data.data(), dec.data.size()) + " err=" +
                    (ok ? "0" : "1");
            if (!dec.image.empty()) line += " img=" + dec.image;
            if (!ok && !quiet) line += " # " + dec.error;
        }
        out += line + "\n";
    }
    size_t np = doc.pageCount();
    size_t ops = 0;
    for (size_t k = 0; k < np; k++) {
        Page p;
        doc.page(k, p);
        std::string c;
        doc.contents(p, c);
        ContentLexer lx(c.data(), c.size());
        Op op;
        size_t n = 0;
        while (lx.next(op)) n++;
        ops += n;
        char b[256];
        snprintf(b, sizeof b, "page %zu oid=%u box=%s,%s,%s,%s rotate=%d ops=%zu\n", k + 1, p.oid, fmtReal(p.crop[0]).c_str(), fmtReal(p.crop[1]).c_str(),
                 fmtReal(p.crop[2]).c_str(), fmtReal(p.crop[3]).c_str(), p.rotate, n);
        out += b;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("summary objs=%zu streams=%zu decerr=%zu pages=%zu ops=%zu mode=%s crypt=%s time=%.0fms\n", ids.size(), nstreams, nerr, np, ops,
           doc.repaired ? "repair" : "xref", doc.crypt.empty() ? "none" : doc.crypt.c_str(), ms);
    if (!quiet) {
        for (auto &n : doc.notes) printf("note %s\n", n.c_str());
        fputs(out.c_str(), stdout);
    }
    return 0;
}
