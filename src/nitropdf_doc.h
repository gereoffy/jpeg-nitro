// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// The tolerance of pdfparse3 (MIT, https://github.com/gereoffy/pdfparse3), ported.
//
// nitropdf_doc: a PDF file's objects for nitropdf (internal, C++): the lexer, the indirect objects
// found through the cross-reference sections (tables and streams, the /Prev and /XRefStm chain) or,
// when those are missing or wrong, by reading the whole file (the repair mode), the object streams,
// the stream filters (Flate, LZW, ASCII85, ASCIIHex, RunLength with the PNG and TIFF predictors;
// the image ones are left to the caller), the Standard security handler with an empty user
// password (RC4, AES-128, AES-256), the page tree with its inherited attributes, and the content
// stream lexer. Objects are read when they are first asked for, and kept.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

namespace nitropdf {

enum Type : uint8_t { NUL, BOOL, INT, REAL, STR, NAME, ARRAY, DICT, REF, KW };

// A value. Strings, names and keywords point to their bytes (a name without its '/'), arrays to
// their items and dictionaries to their key / value pairs, all in the document's memory (or the
// content lexer's), which they share the life of.
struct Val {
    Type t = NUL;
    uint32_t n = 0;   // STR / NAME / KW: bytes, ARRAY: items, DICT: pairs, REF: the generation
    union {
        int64_t i;      // BOOL, INT; REF: the object number
        double r;       // REAL
        const char *s;  // STR / NAME / KW
        const Val *v;   // ARRAY: n items, DICT: n keys (NAME) each followed by its value
    };
    Val() : i(0) {}
    bool isNum() const { return t == INT || t == REAL; }
    double num(double def = 0) const { return t == INT ? (double)i : t == REAL ? r : def; }
    std::string_view str() const { return t == STR || t == NAME || t == KW ? std::string_view(s, n) : std::string_view(); }
    bool is(const char *name) const;   // the name (or keyword) name
    size_t size() const { return t == ARRAY || t == DICT ? n : 0; }
    Val at(size_t k) const { return t == ARRAY && k < n ? v[k] : Val(); }
    Val get(const char *key) const;    // a dictionary's value, unresolved (null if none)
    uint32_t oid() const { return t == REF ? (uint32_t)i : 0; }
};

// An indirect object's stream: its bytes in the file (still encrypted, filtered) and its dictionary.
struct Stream {
    const uint8_t *raw = nullptr;
    size_t len = 0;
    Val dict;
    uint32_t oid = 0, gen = 0;
    bool exact = false;   // its end confirmed: endstream right after /Length bytes
};

struct Decoded {
    std::string data;
    std::string image;    // the image filter left undone (DCTDecode, CCITTFaxDecode, JBIG2Decode, JPXDecode), if any
    Val imageParms;       // and its /DecodeParms
    std::string error;    // the first decoding error (the data is what came before it)
    std::string note;     // tolerated irregularities
};

struct Page {
    Val dict, resources;
    uint32_t oid = 0;
    double media[4] = {0, 0, 612, 792}, crop[4] = {0, 0, 612, 792};   // (the crop box within the media box)
    int rotate = 0;       // 0, 90, 180, 270
    double userUnit = 1;
};

class Doc {
public:
    Doc();
    ~Doc();
    // false if it is not a PDF. The data is not copied: it must outlive the document.
    bool open(const void *data, size_t len);

    Val object(uint32_t oid);                         // an indirect object's value (null if there is none)
    Val resolve(Val v);                               // references followed
    Val get(Val dict, const char *key) { return resolve(resolve(dict).get(key)); }
    const Stream *stream(uint32_t oid);               // an indirect object's stream (nullptr if none)
    // A stream's filters applied, all but the image ones, the predictors too. Nothing beyond limit bytes.
    bool decode(const Stream &s, Decoded &out, size_t limit = 256u << 20);

    Val root() { return rootDict; }
    size_t pageCount();
    bool page(size_t k, Page &p);
    // A page's content streams decoded, joined by newlines (as one stream, which they are)
    bool contents(const Page &p, std::string &out, std::string *err = nullptr);

    // diagnostics, for the tests
    bool repaired = false;           // the objects found by reading the whole file
    std::string crypt;               // the encryption ("" if none), and whether it is decrypted
    std::vector<std::string> notes;  // what was wrong and tolerated
    // all objects known: from the xref and its object streams, with scan those found reading the whole file too
    std::vector<uint32_t> objectIds(bool scan = false);

    struct Impl;
private:
    Impl *m;
    Val rootDict;
    bool pagesDone = false;
    std::vector<Page> pages;
    void collectPages();
};

// The operators of a content stream with their operands, in order. An inline image is one operator
// "BI" with its dictionary (abbreviated keys as written) and its data.
struct Op {
    std::string_view name;
    const Val *args = nullptr;
    size_t nargs = 0;
    Val image;                       // BI: the dictionary
    std::string_view imageData;      // BI: the data between ID and EI
    size_t pos = 0;                  // where it ends in the stream
};

class ContentLexer {
public:
    ContentLexer(const char *data, size_t len);
    ~ContentLexer();
    bool next(Op &op);   // false at the end; the operands live until the next call
private:
    const char *d;
    size_t n, p = 0;
    struct State;
    State *st;
    std::vector<Val> args;
};

}  // namespace nitropdf
