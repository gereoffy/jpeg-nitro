// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
// The tolerance of pdfparse3 (MIT, https://github.com/gereoffy/pdfparse3), ported.
//
// nitropdf: see nitropdf.h. The lexer and the object reader keep pdfparse3's tolerance:
// unterminated strings closed at the end of their line (without quadratic time on garbage full of
// '('), a missing endobj ended by the next "N G obj", a stream's end from its /Length only where
// endstream follows it, otherwise from the endstream found, the referenced /Length resolved, junk
// before the header (offsets relative to it), a startxref pointing nowhere, misnumbered or shifted
// xref entries (the object then found where it is in the file), a text-mode transfer's line
// endings reverted, zlib streams without their checksum, with a wrong one or without their end.
#include "nitropdf.h"
#include "nitrocrypt.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace nitropdf {

// --- values ------------------------------------------------------------------------

bool Val::is(const char *name) const {
    size_t l = strlen(name);
    return (t == NAME || t == KW) && n == l && memcmp(s, name, l) == 0;
}

Val Val::get(const char *key) const {
    if (t != DICT) return Val();
    size_t l = strlen(key);
    for (size_t k = n; k-- > 0;) {   // (the last of the same keys counts)
        const Val &kv = v[2 * k];
        if (kv.n == l && memcmp(kv.s, key, l) == 0) return v[2 * k + 1];
    }
    return Val();
}

// Chunks that never move: the values point into them. A mark / release pair frees what was
// allocated between them (the repair scan's objects, the content lexer's operands).
struct Arena {
    struct Chunk {
        std::unique_ptr<char[]> mem;
        size_t size, used;
    };
    std::vector<Chunk> chunks;
    struct Mark {
        size_t chunk, used;
    };
    void *alloc(size_t len) {
        len = (len + 7) & ~(size_t)7;
        if (chunks.empty() || chunks.back().size - chunks.back().used < len) {
            size_t size = std::max(len, (size_t)64 << 10);
            chunks.push_back({std::unique_ptr<char[]>(new char[size]), size, 0});
        }
        Chunk &c = chunks.back();
        void *p = c.mem.get() + c.used;
        c.used += len;
        return p;
    }
    const char *copy(const void *p, size_t len) {
        char *q = (char *)alloc(len ? len : 1);
        if (len) memcpy(q, p, len);
        return q;
    }
    Mark mark() const { return {chunks.size(), chunks.empty() ? 0 : chunks.back().used}; }
    void reset() {   // (all freed, the first chunk kept)
        if (chunks.size() > 1) chunks.resize(1);
        if (!chunks.empty()) chunks[0].used = 0;
    }
    void release(Mark m) {
        while (chunks.size() > m.chunk) chunks.pop_back();
        if (!chunks.empty() && m.chunk == chunks.size()) chunks.back().used = m.used;
    }
};

namespace {

inline bool isWs(uint8_t c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
inline bool isDelim(uint8_t c) {   // ( ) < > [ ] { } / %
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}
inline int hexDigit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
inline bool isDigit(uint8_t c) { return c >= '0' && c <= '9'; }

bool startsWith(const uint8_t *d, size_t n, size_t p, const char *s) {
    size_t l = strlen(s);
    return p + l <= n && memcmp(d + p, s, l) == 0;
}

size_t find(const uint8_t *d, size_t from, size_t to, const char *s) {
    size_t l = strlen(s);
    if (to < l || from > to - l) return SIZE_MAX;
    const void *q = memmem(d + from, to - from, s, l);
    return q ? (size_t)((const uint8_t *)q - d) : SIZE_MAX;
}

size_t rfind(const uint8_t *d, size_t from, size_t to, const char *s) {
    size_t l = strlen(s);
    if (to < l) return SIZE_MAX;
    for (size_t p = to - l + 1; p-- > from;)
        if (d[p] == (uint8_t)s[0] && memcmp(d + p, s, l) == 0) return p;
    return SIZE_MAX;
}

// --- the lexer ---------------------------------------------------------------------

enum TK : uint8_t { T_END, T_DOPEN, T_DCLOSE, T_AOPEN, T_ACLOSE, T_STRAY, T_STR, T_NAME, T_INT, T_REAL, T_KW, T_EOFMARK };

struct Tok {
    TK k = T_END;
    uint32_t n = 0;
    const char *s = nullptr;
    int64_t i = 0;
    double r = 0;
    size_t pos = 0, end = 0;
    bool kw(const char *w) const { return k == T_KW && n == strlen(w) && memcmp(s, w, n) == 0; }
};

// A token that is a number: an integer of up to 18 digits (with its sign), otherwise a real as
// [+-]?(\d+\.?\d*|\.\d+) (no exponent); anything else is a keyword.
bool parseNumber(const char *s, size_t n, Tok &t) {
    size_t p = 0;
    bool neg = false;
    if (p < n && (s[p] == '+' || s[p] == '-')) neg = s[p] == '-', p++;
    size_t d0 = p;
    while (p < n && isDigit(s[p])) p++;
    size_t intDigits = p - d0;
    if (p == n && intDigits) {
        if (intDigits <= 18) {
            int64_t v = 0;
            for (size_t k = d0; k < n; k++) v = v * 10 + (s[k] - '0');
            t.k = T_INT, t.i = neg ? -v : v;
            return true;
        }
    } else if (p < n && s[p] == '.') {
        p++;
        size_t f0 = p;
        while (p < n && isDigit(s[p])) p++;
        if (p != n || (!intDigits && p == f0)) return false;
    } else return false;
    // (without strtod: no locale) the digits as one integer over a power of ten, rounded once
    static const double P10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};
    uint64_t mant = 0;
    int digits = 0, scale = 0, extra = 0;
    for (size_t k = d0; k < n; k++) {
        if (s[k] == '.') {
            scale = -1;
            continue;
        }
        if (digits < 18) {
            if (mant || s[k] != '0') digits++;
            mant = mant * 10 + (s[k] - '0');
            if (scale < 0) scale--;
        } else if (scale == 0) extra++;   // (digits beyond 18 before the point still count)
    }
    int frac = scale < 0 ? -scale - 1 : 0;
    double v = (double)mant;
    for (; frac > 0; frac -= 18) v /= P10[std::min(frac, 18)];
    for (; extra > 0; extra--) v *= 10;
    t.k = T_REAL, t.r = neg ? -v : v;
    return true;
}

// Where an unterminated (string ends: the parentheses' depths of the last scan kept, so that the
// many '(' of binary junk after it don't each scan to the end again (pdfparse3's _string_end).
struct StrEnd {
    const uint8_t *d = nullptr;
    size_t pend = 0;
    std::vector<size_t> pos;
    std::vector<int> dep, smin;
};

struct Lexer {
    const uint8_t *d;
    size_t p, pend;
    Arena *arena;
    StrEnd *cache;
    bool bad = false;   // an unterminated string or hex string, an invalid character in one

    Lexer(const uint8_t *data, size_t from, size_t to, Arena *a, StrEnd *c) : d(data), p(from), pend(to), arena(a), cache(c) {}

    // the position after the ')' closing the string whose '(' is at p - 1, SIZE_MAX if none before pend
    size_t stringEnd(size_t p) {
        StrEnd &c = *cache;
        if (c.d == d && c.pend == pend && !c.pos.empty() && p - 1 >= c.pos[0]) {
            size_t k = std::upper_bound(c.pos.begin(), c.pos.end(), p - 1) - c.pos.begin();
            if (k >= c.pos.size() || c.smin[k] >= c.dep[k - 1]) return SIZE_MAX;
        }
        int depth = 1;
        std::vector<size_t> pos{p - 1};
        std::vector<int> dep{1};
        while (p < pend) {
            uint8_t ch = d[p];
            if (ch == '\\') {
                p += 2;
                continue;
            }
            p++;
            if (ch != '(' && ch != ')') continue;
            depth += ch == '(' ? 1 : -1;
            if (depth <= 0) return p;
            pos.push_back(p - 1);
            dep.push_back(depth);
        }
        std::vector<int> smin = dep;
        for (size_t k = smin.size() - 1; k-- > 0;)
            if (smin[k + 1] < smin[k]) smin[k] = smin[k + 1];
        c.d = d, c.pend = pend, c.pos.swap(pos), c.dep.swap(dep), c.smin.swap(smin);
        return SIZE_MAX;
    }

    void literal(Tok &t) {
        size_t s0 = p;
        size_t e = stringEnd(p);
        bool eol = false;
        if (e == SIZE_MAX)
            for (size_t q = s0; q < pend && !eol; q++) eol = d[q] == 10 || d[q] == 13;
        std::string out;
        int depth = 1;
        if (!(e == SIZE_MAX && eol)) {
            size_t send = e == SIZE_MAX ? pend : e;
            while (p < send) {
                uint8_t c = d[p++];
                if (c == '(') depth++;
                else if (c == ')') {
                    if (--depth <= 0) break;
                } else if (c == 13) {   // CR, CRLF -> LF
                    if (p < pend && d[p] == 10) p++;
                    c = 10;
                } else if (c == '\\' && p < pend) {
                    uint8_t x = d[p++];
                    if (x == 'n') c = 10;
                    else if (x == 'r') c = 13;
                    else if (x == 't') c = 9;
                    else if (x == 'b') c = 8;
                    else if (x == 'f') c = 12;
                    else if (x == 10 || x == 13) {   // a line continued: neither is part of the string
                        if (x == 13 && p < pend && d[p] == 10) p++;
                        continue;
                    } else if (x >= '0' && x <= '7') {
                        int v = x - '0';
                        for (int k = 1; k < 3 && p < pend && d[p] >= '0' && d[p] <= '7'; k++) v = v * 8 + d[p++] - '0';
                        c = (uint8_t)v;
                    } else c = x;   // \( \) \\ and the unknown ones: the backslash dropped
                }
                out.push_back((char)c);
            }
        } else p = pend;
        if (depth > 0) {
            // unterminated (e.g. "/CreationDate ("): closed at the end of its line, not swallowing the file
            bad = true;
            size_t q = s0;
            while (q < pend && d[q] != 10 && d[q] != 13) q++;
            if (q < pend) {
                p = q;
                out.assign((const char *)d + s0, q - s0);
            }
        }
        t.k = T_STR, t.s = arena->copy(out.data(), out.size()), t.n = (uint32_t)out.size();
    }

    void hexString(Tok &t) {
        std::string out;
        int nib = -1;
        bool closed = false;
        while (p < pend) {
            uint8_t c = d[p++];
            if (c <= 0x20) continue;
            if (c == '>') {
                closed = true;
                break;
            }
            int x = hexDigit(c);
            if (x < 0) {
                bad = true;
                p--;
                break;
            }
            if (nib < 0) nib = x;
            else out.push_back((char)(nib << 4 | x)), nib = -1;
        }
        if (nib >= 0) out.push_back((char)(nib << 4));   // (the last nibble may be missing)
        if (!closed && p >= pend) bad = true;
        t.k = T_STR, t.s = arena->copy(out.data(), out.size()), t.n = (uint32_t)out.size();
    }

    Tok next() {
        Tok t;
        while (p < pend) {
            if (p + 6 <= pend && memcmp(d + p, "endobj", 6) == 0) {   // (not always followed by a separator)
                t.pos = p, p += 6;
                t.k = T_KW, t.s = (const char *)d + t.pos, t.n = 6, t.end = p;
                return t;
            }
            uint8_t c = d[p];
            t.pos = p++;
            if (c <= 32) continue;
            if ((c == '<' || c == '>') && p < pend && d[p] == c) {
                p++;
                t.k = c == '<' ? T_DOPEN : T_DCLOSE, t.end = p;
                return t;
            }
            if (c == '>') {   // (a single '>' closes a dictionary, as in pdfparse3)
                t.k = T_DCLOSE, t.end = p;
                return t;
            }
            if (c == '[' || c == ']') {
                t.k = c == '[' ? T_AOPEN : T_ACLOSE, t.end = p;
                return t;
            }
            if (c == ')' || c == '{' || c == '}') {
                t.k = T_STRAY, t.s = (const char *)d + t.pos, t.n = 1, t.end = p;
                return t;
            }
            if (c == '%') {
                if (p + 4 <= pend && memcmp(d + p, "%EOF", 4) == 0) {
                    p += 4;
                    t.k = T_EOFMARK, t.end = p;
                    return t;
                }
                while (p < pend && d[p] != 10 && d[p] != 13) p++;
                continue;
            }
            if (c == '<') {
                hexString(t);
                t.end = p;
                return t;
            }
            if (c == '(') {
                literal(t);
                t.end = p;
                return t;
            }
            if (c == '/') {
                size_t s = p;
                bool esc = false;
                while (p < pend && d[p] > 0x20 && !isDelim(d[p])) esc |= d[p++] == '#';
                if (!esc) t.s = (const char *)d + s, t.n = (uint32_t)(p - s);
                else {
                    std::string out;
                    for (size_t q = s; q < p; q++) {
                        int x, y;
                        if (d[q] == '#' && q + 2 < pend && (x = hexDigit(d[q + 1])) >= 0 && (y = hexDigit(d[q + 2])) >= 0) {
                            out.push_back((char)(x << 4 | y));
                            q += 2;
                        } else out.push_back((char)d[q]);
                    }
                    t.s = arena->copy(out.data(), out.size()), t.n = (uint32_t)out.size();
                }
                t.k = T_NAME, t.end = p;
                return t;
            }
            while (p < pend && d[p] > 0x20 && !isDelim(d[p])) p++;
            t.s = (const char *)d + t.pos, t.n = (uint32_t)(p - t.pos), t.end = p;
            if (!parseNumber(t.s, t.n, t)) t.k = T_KW;
            return t;
        }
        t.k = T_END, t.pos = t.end = pend;
        return t;
    }
};

// --- values from tokens --------------------------------------------------------------

const size_t MAX_ITEMS = 16u << 20;

struct Parser {
    Lexer lx;
    bool content;        // a content stream's operands: no references, and a keyword ends them
    Tok la[3];
    int nla = 0;
    bool header = false;  // stopped at a "N G obj": the object before it had no endobj

    Parser(const uint8_t *d, size_t from, size_t to, Arena *a, StrEnd *c, bool contentMode)
        : lx(d, from, to, a, c), content(contentMode) {}

    Tok &peek(int k) {
        while (nla <= k) la[nla++] = lx.next();
        return la[k];
    }
    Tok take() {
        Tok t = peek(0);
        la[0] = la[1], la[1] = la[2], nla--;
        return t;
    }
    size_t pos() { return peek(0).pos; }
    bool objHeader() {   // "N G obj" next
        return !content && peek(0).k == T_INT && peek(1).k == T_INT && peek(2).kw("obj");
    }
    // the end of a structure the object can't continue past
    bool stop() {
        if (objHeader()) return header = true;
        Tok &t = peek(0);
        if (t.k == T_END || t.k == T_EOFMARK) return true;
        if (t.k != T_KW) return false;
        if (content) return !(t.kw("true") || t.kw("false") || t.kw("null"));
        return t.kw("endobj") || t.kw("stream") || t.kw("endstream") || t.kw("xref") || t.kw("trailer") || t.kw("startxref");
    }

    Val scalar(const Tok &t) {
        Val v;
        switch (t.k) {
        case T_INT: v.t = INT, v.i = t.i; break;
        case T_REAL: v.t = REAL, v.r = t.r; break;
        case T_STR: v.t = STR, v.s = t.s, v.n = t.n; break;
        case T_NAME: v.t = NAME, v.s = t.s, v.n = t.n; break;
        case T_KW:
            if (t.kw("true") || t.kw("false")) v.t = BOOL, v.i = t.kw("true");
            else if (!t.kw("null")) v.t = KW, v.s = t.s, v.n = t.n;
            break;
        default: break;
        }
        return v;
    }

    Val list(std::vector<Val> &items, Type type) {
        Val v;
        v.t = type;
        v.n = (uint32_t)(type == DICT ? items.size() / 2 : items.size());
        Val *mem = (Val *)lx.arena->alloc(sizeof(Val) * (items.empty() ? 1 : items.size()));
        if (!items.empty()) memcpy((void *)mem, items.data(), sizeof(Val) * items.size());
        v.v = mem;
        return v;
    }

    Val value(int depth) {
        if (stop()) return Val();
        Tok t = take();
        if (t.k == T_INT && !content && peek(0).k == T_INT && peek(1).kw("R") && peek(0).i >= 0 && peek(0).i <= 65535 && t.i >= 0 &&
            t.i <= 0x7fffffff) {
            Val v;
            v.t = REF, v.i = t.i, v.n = (uint32_t)peek(0).i;
            take(), take();
            return v;
        }
        if (t.k == T_AOPEN || t.k == T_DOPEN) {
            bool dict = t.k == T_DOPEN;
            if (depth >= 64) {
                // too deep: read past, kept as null. Without recursing (a hundred thousand '[' of a
                // damaged or hostile file went as deep on the stack), closed as below: a dictionary's
                // ">>" ends the arrays open in it, a stray ']' in a dictionary is skipped
                std::vector<char> open{(char)dict};
                while (!open.empty() && !stop()) {
                    TK k = peek(0).k;
                    if (k == T_DCLOSE) {
                        while (!open.empty() && !open.back()) open.pop_back();
                        if (open.empty()) break;   // (the ">>" of a dictionary around it: left there)
                        open.pop_back();
                    } else if (k == T_ACLOSE) {
                        if (!open.back()) open.pop_back();
                    } else if (k == T_AOPEN || k == T_DOPEN) open.push_back(k == T_DOPEN);
                    take();
                }
                return Val();
            }
            std::vector<Val> items;
            while (!stop()) {
                Tok &c = peek(0);
                if (c.k == (dict ? T_DCLOSE : T_ACLOSE)) {
                    take();
                    break;
                }
                if (dict && c.k == T_ACLOSE) {
                    take();
                    continue;
                }
                if (!dict && c.k == T_DCLOSE) break;   // ("<< /A [1 2 >>": the array ends with its dictionary)
                if (c.k == T_STRAY) {
                    take();
                    continue;
                }
                if (dict) {
                    if (c.k != T_NAME) {   // (a key that is no name: skipped, a structure with its value)
                        if (c.k == T_AOPEN || c.k == T_DOPEN) value(depth + 1);
                        else take();
                        continue;
                    }
                    Tok key = take();
                    if (peek(0).k == T_DCLOSE) {
                        take();
                        if (items.size() < 2 * MAX_ITEMS) items.push_back(scalar(key)), items.push_back(Val());
                        break;
                    }
                    Val v = value(depth + 1);
                    if (items.size() < 2 * MAX_ITEMS) items.push_back(scalar(key)), items.push_back(v);
                } else {
                    Val v = value(depth + 1);
                    if (items.size() < MAX_ITEMS) items.push_back(v);
                }
            }
            return list(items, dict ? DICT : ARRAY);
        }
        if (t.k == T_DCLOSE || t.k == T_ACLOSE || t.k == T_STRAY) return Val();
        return scalar(t);
    }
};

// --- filters -----------------------------------------------------------------------

std::string adlerNote(const char *what, size_t n) {
    char b[160];
    snprintf(b, sizeof b, "ZLIB: %s (decoded %zu bytes)", what, n);
    return b;
}

// zlib's inflate over the whole input: false on a data error. eof: the deflate data's end reached;
// used: the input consumed.
bool zinflate(const uint8_t *d, size_t n, bool raw, std::string &out, size_t limit, bool &eof, size_t &used, std::string *err) {
    z_stream z;
    memset(&z, 0, sizeof z);
    eof = false;
    used = 0;
    if (inflateInit2(&z, raw ? -15 : 15) != Z_OK) return false;
    z.next_in = (Bytef *)d;
    z.avail_in = (uInt)std::min(n, (size_t)UINT32_MAX);
    char buf[65536];
    bool ok = true;
    for (;;) {
        z.next_out = (Bytef *)buf;
        z.avail_out = sizeof buf;
        int r = inflate(&z, Z_NO_FLUSH);
        size_t got = sizeof buf - z.avail_out;
        if (out.size() + got > limit) {
            out.append(buf, limit - out.size());
            if (err) *err = "too large";
            ok = false;
            break;
        }
        out.append(buf, got);
        if (r == Z_STREAM_END) {
            eof = true;
            break;
        }
        if (r == Z_BUF_ERROR && z.avail_in == 0) break;   // (ran out of input)
        if (r != Z_OK) {
            if (err) *err = z.msg ? z.msg : "data error";
            ok = false;
            break;
        }
        if (z.avail_in == 0 && got == 0) break;
    }
    used = n - z.avail_in;
    inflateEnd(&z);
    return ok;
}

bool allWs(const uint8_t *d, size_t n) {
    for (size_t k = 0; k < n; k++)
        if (!isWs(d[k])) return false;
    return true;
}

// pdfparse3's inflate: with the zlib header, then raw deflate as readers do (the checksum checked by
// hand: missing or a writer's bug only noted), and what can be saved of a damaged one.
void flate(const uint8_t *d, size_t n, std::string &out, size_t limit, std::string &err, std::string &note) {
    out.clear();
    if (allWs(d, n)) return;   // (an empty stream: only its line ending before endstream)
    bool eof;
    size_t used;
    std::string zerr;
    bool ok = zinflate(d, n, false, out, limit, eof, used, &zerr);
    if (ok && eof) return;
    if (zerr == "too large") {
        err = "ZLIB: too large";
        return;
    }
    bool hdrOk = n >= 2 && (d[0] & 15) == 8 && (d[0] * 256 + d[1]) % 31 == 0;
    for (int pass = hdrOk ? 0 : 1; pass < 2; pass++) {
        size_t skip = pass == 0 ? 2 : 0;
        std::string rd;
        bool reof;
        size_t rused;
        zinflate(d + skip, n - skip, true, rd, limit, reof, rused, nullptr);
        if (!reof) continue;
        if (skip == 0 && hdrOk) continue;
        if (skip == 0) {
            out.swap(rd);
            note = adlerNote("raw deflate without zlib header", out.size());
            return;
        }
        const uint8_t *tail = d + skip + rused;
        size_t tl = n - skip - rused;
        size_t tw = tl;
        while (tw && isWs(tail[tw - 1])) tw--;
        if (tw < 4) {
            out.swap(rd);
            note = adlerNote("missing adler32 checksum", out.size());
            return;
        }
        uint32_t a = (uint32_t)adler32(1, (const Bytef *)rd.data(), (uInt)rd.size());
        uint32_t st = (uint32_t)tail[0] << 24 | tail[1] << 16 | tail[2] << 8 | tail[3];
        out.swap(rd);
        if (a != st) {
            // the upper half (B) sums the lower (A) over all the data: right only if the data is
            if (a >> 16 == st >> 16) err = adlerNote("adler32 checksum bug of the writer (only the low 16 bits differ), data OK", out.size());
            else err = adlerNote("adler32 checksum mismatch, deflate data complete", out.size());
            return;
        }
        note = adlerNote(zerr.find("too far back") != std::string::npos ? "window size in zlib header too small, OK with 32K window"
                                                                          : "not finished, but raw deflate and checksum OK",
                         out.size());
        return;
    }
    size_t e = n;
    while (e && isWs(d[e - 1])) e--;
    if (ok && e >= 4 && memcmp(d + e - 4, "\0\0\xff\xff", 4) == 0) {
        note = adlerNote("stream ends with sync flush, no final block", out.size());
        return;
    }
    for (int pass = hdrOk ? 0 : 1; pass < 2; pass++) {   // what can be saved
        std::string rd;
        bool reof;
        size_t rused;
        zinflate(d + (pass == 0 ? 2 : 0), n - (pass == 0 ? 2 : 0), true, rd, limit, reof, rused, nullptr);
        if (rd.size() > out.size()) out.swap(rd);
    }
    err = adlerNote(!ok ? zerr.c_str() : "truncated stream?", out.size());
}

// LZW (as pypdf; early: /EarlyChange)
void lzw(const uint8_t *d, size_t n, int early, bool exact, std::string &out, size_t limit, std::string &err, std::string &note) {
    if (early != 0 && early != 1) early = 1;
    std::vector<uint16_t> prefix(4096);
    std::vector<uint8_t> suffix(4096), first(4096);
    std::vector<uint16_t> length(4096);
    for (int k = 0; k < 256; k++) suffix[k] = first[k] = (uint8_t)k, length[k] = 1;
    int dictLen = 258, bits = 9;
    size_t bytePos = 0;
    int bitPos = 0;
    auto nextCode = [&]() -> int {
        int fill = bits, v = 0;
        while (fill > 0) {
            if (bytePos >= n) return -1;
            int from = std::min(8 - bitPos, fill);
            v |= ((d[bytePos] >> (8 - bitPos - from)) & (0xff >> (8 - from))) << (fill - from);
            fill -= from;
            bitPos += from;
            if (bitPos >= 8) bitPos = 0, bytePos++;
        }
        return v;
    };
    auto emit = [&](int code) {
        size_t l = length[code], at = out.size();
        out.resize(at + l);
        for (int c = code; l--; c = prefix[c]) out[at + l] = (char)suffix[c];
    };
    int cw = 256;
    bool noEod = false;
    for (;;) {
        int pw = cw;
        cw = nextCode();
        if (cw == -1) {
            noEod = true;
            break;
        }
        if (cw == 257) break;
        if (cw == 256) {
            dictLen = 258, bits = 9;
            continue;
        }
        if (pw == 256) {
            if (cw >= dictLen) {
                err = "LZW: invalid code after clear";
                break;
            }
            emit(cw);
        } else {
            if (dictLen >= 4096) {
                err = "LZW: dictionary overflow (missing clear code)";
                break;
            }
            if (cw < dictLen) {
                emit(cw);
                prefix[dictLen] = (uint16_t)pw, suffix[dictLen] = first[cw], first[dictLen] = first[pw];
            } else if (cw == dictLen) {
                prefix[dictLen] = (uint16_t)pw, suffix[dictLen] = first[pw], first[dictLen] = first[pw];
            } else {
                err = "LZW: invalid code";
                break;
            }
            length[dictLen] = (uint16_t)std::min(length[pw] + 1, 65535);
            dictLen++;
            if (cw == dictLen - 1) emit(cw);
            if (dictLen + early >= (1 << bits) && bits < 12) bits++;
        }
        if (out.size() > limit) {
            out.resize(limit);
            err = "LZW: too large";
            break;
        }
    }
    // a missing EOD is tolerated where /Length surely gave the end (some writers omit it)
    if (err.empty() && noEod && !exact) err = "LZW: no EOD code and the stream length is uncertain";
    if (err.empty() && !noEod && n - bytePos > 3) note = "LZW: bytes left after the EOD code";
}

void asciiHex(const uint8_t *d, size_t n, std::string &out, std::string &err) {
    size_t p = 0;
    while (p < n) {
        if (d[p] == '>') break;
        if (d[p] <= 32) {
            p++;
            continue;
        }
        int x = hexDigit(d[p++]);
        if (x < 0) {
            if (err.empty()) err = "ASCIIHex: invalid char";
            continue;
        }
        while (p < n && d[p] <= 32) p++;
        int y = p < n ? hexDigit(d[p]) : -1;
        if (y < 0) y = 0;
        else p++;
        out.push_back((char)(x << 4 | y));
    }
    if (p >= n && err.empty()) err = "ASCIIHex: missing EOD '>'";
}

void ascii85(const uint8_t *d, size_t n, std::string &out, std::string &note) {
    size_t p = 0;
    if (n >= 2 && d[0] == '<' && d[1] == '~') p = 2;
    size_t e = find(d, p, n, "~>");
    if (e != SIZE_MAX) n = e;
    uint32_t grp[5];
    int g = 0;
    bool overflow = false, y = false, invalid = false;
    for (; p < n; p++) {
        uint8_t c = d[p];
        if (c >= 33 && c <= 117) {
            grp[g++] = c - 33;
            if (g == 5) {
                uint64_t v = (((((uint64_t)grp[0] * 85 + grp[1]) * 85 + grp[2]) * 85 + grp[3]) * 85 + grp[4]);
                if (v > 0xffffffffu) overflow = true;
                uint32_t w = (uint32_t)v;
                char b[4] = {(char)(w >> 24), (char)(w >> 16), (char)(w >> 8), (char)w};
                out.append(b, 4);
                g = 0;
            }
        } else if (c == 'z' && !g) out.append(4, '\0');
        else if (c == 'y' && !g) out.append(4, ' '), y = true;
        else if (!isWs(c)) invalid = true;
    }
    if (g == 1) note = "ASCII85: 1 char in the last group";
    else if (g) {
        for (int k = g; k < 5; k++) grp[k] = 84;
        uint32_t w = (uint32_t)((((((uint64_t)grp[0] * 85 + grp[1]) * 85 + grp[2]) * 85 + grp[3]) * 85 + grp[4]));
        char b[4] = {(char)(w >> 24), (char)(w >> 16), (char)(w >> 8), (char)w};
        out.append(b, g - 1);
    }
    if (overflow || y || invalid) {
        std::string s = "ASCII85:";
        if (overflow) s += " overflow";
        if (y) s += " y";
        if (invalid) s += " invalid char";
        note = note.empty() ? s : s + ", " + note;
    }
}

void runLength(const uint8_t *d, size_t n, std::string &out, size_t limit) {
    size_t p = 0;
    while (p < n && out.size() < limit) {
        int c = d[p++];
        if (c == 128) break;
        if (c < 128) {
            size_t l = std::min((size_t)c + 1, n - p);
            out.append((const char *)d + p, l);
            p += c + 1;
        } else {
            if (p < n) out.append(257 - c, (char)d[p]);
            p++;
        }
    }
}

// the PNG (10-15) and TIFF (2) predictors
bool unpredict(std::string &data, int pred, int colors, int bpc, int cols, std::string &err) {
    if (pred <= 1) return true;
    if (colors < 1 || colors > 32 || cols < 1 || cols > (1 << 24) || (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16)) {
        err = "Predictor: invalid parameters";
        return false;
    }
    size_t bpp = std::max(1, (colors * bpc + 7) / 8);
    size_t rowLen = ((size_t)colors * bpc * cols + 7) / 8;
    std::string out;
    if (pred == 2) {
        out = data;
        uint8_t *o = (uint8_t *)&out[0];
        for (size_t r = 0; r < out.size(); r += rowLen) {
            size_t l = std::min(rowLen, out.size() - r);
            uint8_t *row = o + r;
            if (bpc == 8)
                for (size_t k = bpp; k < l; k++) row[k] = (uint8_t)(row[k] + row[k - bpp]);
            else if (bpc == 16)
                for (size_t k = 2 * colors; k + 1 < l; k += 2) {
                    unsigned v = ((row[k] << 8) | row[k + 1]) + ((row[k - 2 * colors] << 8) | row[k + 1 - 2 * colors]);
                    row[k] = (uint8_t)(v >> 8), row[k + 1] = (uint8_t)v;
                }
            else {
                size_t samples = (size_t)colors * cols, mask = (1u << bpc) - 1;
                auto getS = [&](size_t s) { return (row[s * bpc / 8] >> (8 - bpc - s * bpc % 8)) & mask; };
                for (size_t s = colors; s < samples && (s * bpc) / 8 < l; s++) {
                    size_t v = (getS(s) + getS(s - colors)) & mask;
                    size_t sh = 8 - bpc - s * bpc % 8;
                    row[s * bpc / 8] = (uint8_t)((row[s * bpc / 8] & ~(mask << sh)) | v << sh);
                }
            }
        }
        data.swap(out);
        return true;
    }
    if (pred < 10) {
        err = "Predictor: unknown predictor";
        return false;
    }
    std::vector<uint8_t> prev(rowLen, 0);
    out.reserve(data.size());
    const uint8_t *d = (const uint8_t *)data.data();
    for (size_t r = 0; r < data.size(); r += rowLen + 1) {
        int ft = d[r];
        size_t l = std::min(rowLen, data.size() - r - 1);
        if (l < rowLen) {
            err = "Predictor: PNG predictor: truncated row";
            if (!l) break;
        }
        std::vector<uint8_t> row(d + r + 1, d + r + 1 + l);
        row.resize(rowLen, 0);
        if (ft == 1)
            for (size_t k = bpp; k < rowLen; k++) row[k] = (uint8_t)(row[k] + row[k - bpp]);
        else if (ft == 2)
            for (size_t k = 0; k < rowLen; k++) row[k] = (uint8_t)(row[k] + prev[k]);
        else if (ft == 3)
            for (size_t k = 0; k < rowLen; k++) row[k] = (uint8_t)(row[k] + (((k >= bpp ? row[k - bpp] : 0) + prev[k]) >> 1));
        else if (ft == 4)
            for (size_t k = 0; k < rowLen; k++) {
                int a = k >= bpp ? row[k - bpp] : 0, b = prev[k], c = k >= bpp ? prev[k - bpp] : 0;
                int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
                row[k] = (uint8_t)(row[k] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c));
            }
        else if (ft != 0) {
            err = "Predictor: PNG predictor: invalid filter type";
            break;
        }
        out.append((const char *)row.data(), l);
        prev = row;
        if (l < rowLen) break;
    }
    data.swap(out);
    return err.empty();
}

const uint8_t PDF_PAD[32] = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
                             0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};

// ISO 32000-2 7.6.4.3.4, algorithm 2.B: the R6 password hash (an empty password, no user key)
void hash2b(const uint8_t *salt, uint8_t out[32]) {
    uint8_t K[64];
    size_t kl = 32;
    nc_sha256(salt, 8, K);
    std::vector<uint8_t> K1, E;
    for (int round = 0;; round++) {
        K1.clear();
        for (int k = 0; k < 64; k++) K1.insert(K1.end(), K, K + kl);
        E.resize(K1.size());
        nc_aes aes;
        nc_aes_init(&aes, K, 16);
        uint8_t iv[16];
        memcpy(iv, K + 16, 16);
        nc_aes_encrypt_cbc(&aes, iv, K1.data(), K1.size(), E.data());
        int sum = 0;
        for (int k = 0; k < 16; k++) sum += E[k];
        int h = sum % 3;
        if (h == 0) nc_sha256(E.data(), E.size(), K), kl = 32;
        else if (h == 1) nc_sha384(E.data(), E.size(), K), kl = 48;
        else nc_sha512(E.data(), E.size(), K), kl = 64;
        if (round >= 63 && E.back() <= round + 1 - 32) break;
    }
    memcpy(out, K, 32);
}

}  // namespace

// --- the document ------------------------------------------------------------------

struct Doc::Impl {
    Doc *doc;
    const uint8_t *d = nullptr;
    size_t len = 0, pend = 0, headEnd = 0, base = 0;
    std::string fixed;   // the data with its line endings reverted (eol_repair)
    Arena arena;
    StrEnd strEnd;

    struct Entry {
        uint8_t type;   // 0 free, 1 in the file, 2 in an object stream, 3 "0000000000 n"
        bool bad;
        uint32_t gen;   // 2: the index in the stream
        uint64_t off;   // 2: the stream's object number
    };
    std::unordered_map<uint32_t, Entry> xref;
    Val trailer;          // (the newest section's)
    Val encryptRef;
    std::string fileId;
    bool hasId = false;
    bool truncated = false, xrefDamaged = false;

    // the objects' headers in the file: "N G obj" (oid, gen) -> positions, in order
    bool startsDone = false;
    std::unordered_map<uint64_t, std::vector<size_t>> starts;
    std::vector<std::pair<size_t, uint64_t>> startList;
    std::unordered_map<uint32_t, size_t> lastStart;   // (any generation)

    // the repair scan's result: oid -> position (the last in the file), the object streams found
    bool scanned = false;
    std::unordered_map<uint32_t, size_t> found;
    std::vector<uint32_t> objStms;
    uint32_t catalog = 0;

    struct Cached {
        Val v;
        int stream = -1;
        bool busy = false;
    };
    std::unordered_map<uint32_t, Cached> cache;
    std::deque<Stream> streams;   // references survive loading further streams during resolve/decode

    struct ObjStm {
        bool loaded = false;
        std::string data;
        size_t first = 0;
        std::vector<std::pair<uint32_t, std::pair<size_t, size_t>>> members;   // oid -> [start, end) in data
    };
    std::unordered_map<uint32_t, ObjStm> objstm;

    // the security handler
    bool encrypted = false, decrypting = false, encMeta = true, stmAes = false, strAes = false, stmNone = false, strNone = false;
    int cryptR = 0;
    uint32_t encOid = 0;
    std::vector<uint8_t> key;

    void note(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        char b[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        if (doc->notes.size() < 200 && (doc->notes.empty() || doc->notes.back() != b)) doc->notes.push_back(b);
    }

    // --- object headers ---

    // "N G obj" at p (up to 32 whitespace bytes between, the numbers up to 10 digits)
    bool headerAt(size_t p, uint32_t *oid, uint32_t *gen, size_t *after = nullptr) {
        uint64_t v[2];
        for (int k = 0; k < 2; k++) {
            size_t s = p;
            uint64_t x = 0;
            while (p < len && isDigit(d[p]) && p - s < 10) x = x * 10 + (d[p++] - '0');
            if (p == s || (p < len && isDigit(d[p]))) return false;
            size_t w = p;
            while (p < len && isWs(d[p]) && p - w < 32) p++;
            if (p == w) return false;
            v[k] = x;
        }
        if (!startsWith(d, len, p, "obj")) return false;
        if (v[0] > 0xffffffffu || v[1] > 0xffffffffu) return false;
        if (oid) *oid = (uint32_t)v[0];
        if (gen) *gen = (uint32_t)v[1];
        if (after) *after = p + 3;
        return true;
    }

    void findStarts() {
        if (startsDone) return;
        startsDone = true;
        for (size_t q = find(d, 0, len, "obj"); q != SIZE_MAX; q = find(d, q + 1, len, "obj")) {
            // back over whitespace, the generation, whitespace and the number
            size_t p = q;
            size_t w = 0;
            while (p > 0 && isWs(d[p - 1]) && w < 32) p--, w++;
            if (!w) continue;
            size_t g = 0;
            while (p > 0 && isDigit(d[p - 1]) && g < 10) p--, g++;
            if (!g) continue;
            w = 0;
            while (p > 0 && isWs(d[p - 1]) && w < 32) p--, w++;
            if (!w) continue;
            size_t o = 0;
            while (p > 0 && isDigit(d[p - 1]) && o < 10) p--, o++;
            if (!o || (p > 0 && isDigit(d[p - 1]))) continue;   // (more than 10 digits: no header)
            uint32_t oid, gen;
            if (!headerAt(p, &oid, &gen)) continue;
            uint64_t k = (uint64_t)oid << 32 | gen;
            starts[k].push_back(p);
            startList.push_back({p, k});
        }
        std::sort(startList.begin(), startList.end());
        for (auto &s : startList) lastStart[(uint32_t)(s.second >> 32)] = s.first;
    }

    size_t lastStartOf(uint32_t oid, uint32_t gen, bool anyGen) {
        findStarts();
        if (anyGen) {
            auto it = lastStart.find(oid);
            return it != lastStart.end() && it->second < pend ? it->second : SIZE_MAX;
        }
        auto it = starts.find((uint64_t)oid << 32 | gen);
        if (it == starts.end()) return SIZE_MAX;
        for (size_t k = it->second.size(); k-- > 0;)
            if (it->second[k] < pend) return it->second[k];
        return SIZE_MAX;
    }

    // --- indirect objects ---

    struct Obj {
        uint32_t oid = 0, gen = 0;
        Val v;
        bool hasStream = false, endobj = false;
        Stream st;
        size_t end = 0;
    };

    // a referenced /Length: "N G obj <number> endobj" where the xref says, or the last in the file
    int64_t lengthOf(uint32_t oid, uint32_t gen) {
        size_t cands[2] = {SIZE_MAX, SIZE_MAX};
        auto it = xref.find(oid);
        if (it != xref.end() && it->second.type == 1 && it->second.gen == gen && it->second.off < len) cands[0] = (size_t)it->second.off;
        else if (it != xref.end() && it->second.type == 2) {
            Val v = getObject(oid);
            return v.t == INT ? v.i : -1;
        }
        for (int c = 0; c < 2; c++) {
            size_t p = cands[c];
            if (c == 1) p = lastStartOf(oid, gen, false);
            if (p == SIZE_MAX) continue;
            size_t q = p;
            while (q < len && q < p + 4 && isWs(d[q])) q++;
            uint32_t o, g;
            size_t after;
            if (!headerAt(q, &o, &g, &after) || o != oid) continue;
            Arena tmp;
            Lexer lx(d, after, len, &tmp, &strEnd);
            Tok t = lx.next();
            Tok e = lx.next();
            if (t.k == T_INT && e.kw("endobj")) return t.i;
        }
        return -1;
    }

    // an "endobj" and an "N G obj" header after it in [from, to)
    bool objectsIn(size_t from, size_t to) {
        size_t e = find(d, from, to, "endobj");
        if (e == SIZE_MAX) return false;
        for (size_t p = e + 6; p < to; p++) {
            if (!isDigit(d[p]) || (p > 0 && isDigit(d[p - 1]))) continue;
            if (headerAt(p, nullptr, nullptr)) return true;
        }
        return false;
    }

    // the indirect object at p, its stream found as pdfparse3 does
    bool parseAt(size_t p, Obj &o, Arena *a) {
        Parser ps(d, p, len, a, &strEnd, false);
        if (!(ps.peek(0).k == T_INT && ps.peek(1).k == T_INT && ps.peek(2).kw("obj"))) return false;
        if (ps.peek(0).i < 0 || ps.peek(0).i > 0xffffffffLL || ps.peek(1).i < 0 || ps.peek(1).i > 0xffffffffLL) return false;
        o.oid = (uint32_t)ps.take().i;
        o.gen = (uint32_t)ps.take().i;
        ps.take();
        o.v = ps.value(0);
        for (;;) {
            if (ps.objHeader()) break;
            Tok &t = ps.peek(0);
            if (t.k == T_END || t.k == T_EOFMARK || t.kw("startxref") || t.kw("xref") || t.kw("trailer")) break;
            if (t.kw("endobj")) {
                ps.take();
                o.endobj = true;
                break;
            }
            if (t.kw("stream")) {
                size_t q = ps.take().end;
                ps.nla = 0;
                if (q < len && (d[q] == 9 || d[q] == 32)) q++;
                if (q < len && (d[q] == 13 || d[q] == 10)) {
                    if (d[q] == 13 && q + 1 < len && d[q + 1] == 10) q++;
                    q++;
                }
                int64_t sl = -1;
                Val L = o.v.get("Length");
                if (L.t == INT) sl = L.i;
                else if (L.t == REF) sl = lengthOf(L.oid(), L.n);
                bool lengthKnown = sl >= 0;
                if (sl < 0) sl = 0;
                size_t next = q;
                bool got = false;
                if (lengthKnown && (uint64_t)sl <= len - q) {
                    size_t e = q + sl;
                    while (e < len && e < q + sl + 8 && isWs(d[e])) e++;
                    if (startsWith(d, len, e, "endstream")) {
                        o.st.raw = d + q, o.st.len = sl, o.st.exact = true;
                        got = true;
                        next = e;
                    }
                }
                if (!got) {
                    size_t e = find(d, q, len, "endstream");
                    if (e == SIZE_MAX) {
                        if (sl > 0 && (uint64_t)sl <= len - q) {
                            o.st.raw = d + q, o.st.len = sl;
                            got = true;
                            next = q + sl;
                        }
                    } else {
                        size_t span = e - q;
                        if (sl > 0 && (uint64_t)sl + 3 < span && objectsIn(q + sl, e)) {
                            // its endstream missing: objects follow /Length, the endstream found is another's
                            o.st.raw = d + q, o.st.len = sl;
                            got = true;
                            next = q + sl;
                        } else {
                            if (!lengthKnown || (uint64_t)sl > span) sl = span;
                            if ((uint64_t)sl + 3 < span) sl = span;
                            o.st.raw = d + q, o.st.len = sl;
                            got = true;
                            next = e;
                        }
                    }
                }
                if (got) {
                    o.hasStream = true;
                    o.st.dict = o.v, o.st.oid = o.oid, o.st.gen = o.gen;
                }
                ps.lx.p = next;
                continue;
            }
            ps.take();
        }
        o.end = ps.nla ? ps.peek(0).pos : ps.lx.p;
        if (ps.header) o.end = ps.peek(0).pos;
        return true;
    }

    // the object streams' members
    ObjStm *loadObjStm(uint32_t soid) {
        ObjStm &s = objstm[soid];
        if (s.loaded) return &s;
        s.loaded = true;
        if (encrypted && !decrypting) return &s;
        getObject(soid);
        const Stream *st = streamOf(soid);
        if (!st) return &s;
        Decoded dec;
        doc->decode(*st, dec);
        s.data.swap(dec.data);
        Val first = resolve(st->dict.get("First"));
        if (first.t != INT || first.i < 0 || (size_t)first.i > s.data.size()) return &s;
        s.first = (size_t)first.i;
        std::vector<std::pair<uint32_t, size_t>> pairs;
        {
            Arena tmp;
            Lexer lx((const uint8_t *)s.data.data(), 0, s.first, &tmp, &strEnd);
            for (;;) {
                Tok a = lx.next(), b = lx.next();
                if (a.k != T_INT || b.k != T_INT) break;
                if (a.i >= 0 && a.i <= 0xffffffffLL && b.i >= 0) pairs.push_back({(uint32_t)a.i, (size_t)b.i});
            }
        }
        std::vector<size_t> offs;
        for (auto &pr : pairs) offs.push_back(pr.second);
        std::sort(offs.begin(), offs.end());
        offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
        for (auto &pr : pairs) {
            size_t k = std::lower_bound(offs.begin(), offs.end(), pr.second) - offs.begin();
            size_t e = k + 1 < offs.size() ? offs[k + 1] : s.data.size() - s.first;
            size_t a = s.first + pr.second, b = std::min(s.first + e, s.data.size());   // (offsets beyond the data)
            if (a > s.data.size() || b < a) continue;
            s.members.push_back({pr.first, {a, b}});
        }
        return &s;
    }

    bool fromObjStm(uint32_t soid, uint32_t oid, Val &v) {
        ObjStm *s = loadObjStm(soid);
        for (auto &m : s->members)
            if (m.first == oid) {
                Parser ps((const uint8_t *)s->data.data(), m.second.first, m.second.second, &arena, &strEnd, false);
                v = ps.value(0);
                return true;
            }
        return false;
    }

    // a file object's strings decrypted (not a signature's /Contents)
    void decryptStrings(Val &v, uint32_t oid, uint32_t gen, int depth) {
        if (depth > 64) return;
        if (v.t == STR) {
            std::string out = decrypt(oid, gen, (const uint8_t *)v.s, v.n, false);
            v.s = arena.copy(out.data(), out.size()), v.n = (uint32_t)out.size();
        } else if (v.t == ARRAY) {
            for (uint32_t k = 0; k < v.n; k++) decryptStrings(const_cast<Val &>(v.v[k]), oid, gen, depth + 1);
        } else if (v.t == DICT) {
            bool sig = v.get("ByteRange").t != NUL;
            for (uint32_t k = 0; k < v.n; k++) {
                if (sig && v.v[2 * k].n == 8 && memcmp(v.v[2 * k].s, "Contents", 8) == 0) continue;
                decryptStrings(const_cast<Val &>(v.v[2 * k + 1]), oid, gen, depth + 1);
            }
        }
    }

    bool loadFromFile(size_t p, uint32_t oid, Cached &c) {
        Obj o;
        if (!parseAt(p, o, &arena) || o.oid != oid) return false;
        if (decrypting && oid != encOid && !resolve(o.v.get("Type")).is("XRef")) decryptStrings(o.v, oid, o.gen, 0);
        c.v = o.v;
        if (o.hasStream) {
            o.st.dict = o.v;
            c.stream = (int)streams.size();
            streams.push_back(o.st);
        }
        return true;
    }

    Val getObject(uint32_t oid) {
        auto it = cache.find(oid);
        if (it != cache.end()) return it->second.v;
        Cached &c = cache[oid];
        c.busy = true;
        bool ok = false;
        if (repairedMode()) {
            auto f = found.find(oid);
            if (f != found.end()) ok = loadFromFile(f->second, oid, c);
        }
        auto x = xref.find(oid);
        if (!ok && x != xref.end()) {
            const Entry &e = x->second;
            if (e.type == 1) {
                size_t p = SIZE_MAX;
                if (!e.bad) {
                    p = (size_t)e.off;
                    while (p < len && p < e.off + 4 && isWs(d[p])) p++;
                } else if ((p = lastStartOf(oid, e.gen, false)) == SIZE_MAX) p = lastStartOf(oid, 0, true);
                if (p != SIZE_MAX) ok = loadFromFile(p, oid, c);
                if (!ok && !e.bad && (p = lastStartOf(oid, e.gen, false)) != SIZE_MAX) ok = loadFromFile(p, oid, c);
            } else if (e.type == 2 && e.off <= 0xffffffffu && e.off != oid) {
                Val v;
                if (fromObjStm((uint32_t)e.off, oid, v)) cache[oid].v = v, ok = true;
            } else if (e.type == 0 && !repairedMode()) ok = true;   // (free: null)
            // (3, "0000000000 n": unused numbers, as macOS writes them, or an offset lost: looked for below)
        }
        if (!ok) {
            // not in the xref, or not where it says: where it is in the file, or in an object stream
            size_t p = lastStartOf(oid, 0, true);
            if (p != SIZE_MAX) ok = loadFromFile(p, oid, cache[oid]);
            if (!ok) {
                for (uint32_t s : allObjStms(repairedMode())) {
                    Val v;
                    if (s != oid && fromObjStm(s, oid, v)) {
                        cache[oid].v = v;
                        break;
                    }
                }
            }
        }
        cache[oid].busy = false;
        return cache[oid].v;
    }

    const Stream *streamOf(uint32_t oid) {
        getObject(oid);
        auto it = cache.find(oid);
        return it != cache.end() && it->second.stream >= 0 ? &streams[it->second.stream] : nullptr;
    }

    std::vector<uint32_t> allObjStms(bool withScan) {
        std::vector<uint32_t> out;
        std::unordered_set<uint32_t> seen;
        for (auto &e : xref)
            if (e.second.type == 2 && e.second.off <= 0xffffffffu && seen.insert((uint32_t)e.second.off).second) out.push_back((uint32_t)e.second.off);
        if (withScan) scan();
        for (uint32_t s : objStms)
            if (seen.insert(s).second) out.push_back(s);
        return out;
    }

    Val resolve(Val v) {
        for (int k = 0; k < 32 && v.t == REF; k++) {
            auto it = cache.find(v.oid());
            if (it != cache.end() && it->second.busy) return Val();   // (a reference cycle)
            v = getObject(v.oid());
        }
        return v.t == REF ? Val() : v;
    }

    bool repairedMode() const { return doc->repaired; }

    // --- reading the whole file (repair) ---

    void scan() {
        if (scanned) return;
        scanned = true;
        findStarts();
        size_t cursor = headEnd;
        for (auto &s : startList) {
            if (s.first < cursor || s.first >= pend) continue;
            Arena::Mark mk = arena.mark();
            Obj o;
            if (parseAt(s.first, o, &arena)) {
                found[o.oid] = s.first;
                Val type = o.v.get("Type");
                if (type.is("ObjStm") && o.hasStream) objStms.push_back(o.oid);
                if (type.is("Catalog")) catalog = o.oid;
                cursor = std::max(o.end, s.first + 1);
            }
            arena.release(mk);
        }
    }

    // --- the xref sections ---

    bool sectionAt(size_t o) {
        if (o >= len) return false;
        while (o < len && isWs(d[o])) o++;
        return startsWith(d, len, o, "xref") || headerAt(o, nullptr, nullptr);
    }

    // the last "xref" keyword (no letters around it) in [from, to)
    size_t rfindXref(size_t from, size_t to) {
        for (size_t p = rfind(d, from, to, "xref"); p != SIZE_MAX; p = p > from ? rfind(d, from, p + 3, "xref") : SIZE_MAX) {
            bool before = p > 0 && isalpha(d[p - 1]), after = p + 4 < len && isalpha(d[p + 4]);
            if (!before && !after) return p;
            if (p == 0) break;
        }
        return SIZE_MAX;
    }

    // A hybrid file's stream (/XRefStm) gives the objects its table hides as free: it replaces those
    // entries of its own table (and the ones without an offset; an object in use there stays: the
    // table is read first, then its stream), never a newer revision's.
    std::unordered_map<uint32_t, size_t> xrefRevision;
    size_t currentRevision = 0;
    bool hybridSection = false;
    void setEntry(uint32_t oid, uint8_t type, uint64_t off, uint32_t gen) {
        auto it = xref.find(oid);
        if (it != xref.end() && !(hybridSection && (it->second.type == 0 || it->second.type == 3) && xrefRevision[oid] == currentRevision)) return;
        xrefRevision[oid] = currentRevision;
        xref[oid] = {type, false, gen, off};
    }

    void trailerKeys(Val t) {
        if (trailer.t == NUL) trailer = t;
        Val enc = t.get("Encrypt");
        if (!encrypted && enc.t != NUL) {
            encrypted = true;
            encryptRef = enc;
            Val id = t.get("ID");
            if (id.t == ARRAY && id.at(0).t == STR) fileId.assign(id.at(0).s, id.at(0).n), hasId = true;   // (may be empty)
        }
        if (doc->rootDict.t == NUL && t.get("Root").t == REF) doc->rootDict = t.get("Root");   // (resolved later)
    }

    void parseTable(Parser &ps) {
        ps.take();   // xref
        for (bool firstSub = true;; firstSub = false) {
            if (ps.peek(0).kw("trailer")) {
                ps.take();
                Val t = ps.value(0);
                trailerKeys(t);
                sectionLinks.push_back(t);
                return;
            }
            if (ps.peek(0).k != T_INT || ps.peek(1).k != T_INT) {
                note("XREF: invalid subsection header");
                return;
            }
            int64_t first = ps.take().i, count = ps.take().i;
            if (first < 0 || count < 0) {
                note("XREF: invalid subsection header");
                return;
            }
            for (int64_t j = 0; j < count; j++) {
                Tok a = ps.take(), b = ps.take(), c = ps.take();
                if (a.k == T_END || c.k == T_END) {
                    note("XREF: end of data before trailer");
                    return;
                }
                if (a.k != T_INT || b.k != T_INT) {
                    note("XREF: invalid entry");
                    continue;
                }
                uint32_t oid = (uint32_t)(first + j);
                if (c.kw("n")) {
                    if (a.i != 0) setEntry(oid, 1, (uint64_t)a.i + base, (uint32_t)b.i);
                    else setEntry(oid, 3, 0, 0);
                } else if (c.kw("f")) {
                    if (firstSub && first == 1 && j == 0 && b.i == 65535) first = 0;   // (the free head numbered 1, not 0)
                    else setEntry(oid, 0, 0, 0);
                } else note("XREF: invalid entry type");
            }
        }
    }

    void parseXrefStream(const Obj &o) {
        Val t = o.v;
        Val W = resolve(t.get("W")), size = resolve(t.get("Size")), index = resolve(t.get("Index"));
        int w[3];
        bool okW = W.t == ARRAY && W.n == 3;
        for (int k = 0; okW && k < 3; k++) {
            Val x = resolve(W.at(k));
            okW = x.t == INT && x.i >= 0 && x.i <= 8;
            w[k] = okW ? (int)x.i : 0;
        }
        if (!okW || size.t != INT || (index.t == ARRAY && index.n % 2)) {
            note("XREF: invalid binary xref header");
            return;
        }
        trailerKeys(t);
        sectionLinks.push_back(t);
        Decoded dec;
        doc->decode(o.st, dec, 64u << 20);
        if (!dec.error.empty()) {
            note("XREF: binary xref decoding error: %s", dec.error.c_str());
            xrefDamaged = true;   // (its entries can't be trusted: damaged data, e.g. a checksum mismatch)
        }
        int rl = w[0] + w[1] + w[2];
        if (!rl) return;
        std::vector<int64_t> idx;
        if (index.t == ARRAY)
            for (uint32_t k = 0; k < index.n; k++) idx.push_back(resolve(index.at(k)).num(0) > 0 ? (int64_t)resolve(index.at(k)).num(0) : 0);
        else idx = {0, size.i};
        const uint8_t *dd = (const uint8_t *)dec.data.data();
        size_t pos = 0;
        auto field = [&](const uint8_t *row, int a, int n, uint64_t def) {
            if (!n) return def;
            uint64_t v = 0;
            for (int k = 0; k < n; k++) v = v << 8 | row[a + k];
            return v;
        };
        for (size_t k = 0; k + 1 < idx.size(); k += 2)
            for (int64_t j = 0; j < idx[k + 1]; j++) {
                if (pos + rl > dec.data.size()) {
                    note("XREF: binary xref table too short");
                    return;
                }
                const uint8_t *row = dd + pos;
                pos += rl;
                uint64_t ty = field(row, 0, w[0], 1), f2 = field(row, w[0], w[1], 0), f3 = field(row, w[0] + w[1], w[2], 0);
                uint32_t oid = (uint32_t)(idx[k] + j);
                if (ty == 1 && f2 == 0) setEntry(oid, 3, 0, 0);
                else if (ty == 1) setEntry(oid, 1, f2 + base, (uint32_t)f3);
                else if (ty == 2) setEntry(oid, 2, f2, (uint32_t)f3);
                else if (ty == 0) setEntry(oid, 0, 0, 0);
            }
    }

    std::vector<Val> sectionLinks;

    // the sections from o on, following /XRefStm (a hybrid file's stream first, as the standard
    // says) and /Prev
    void parseXref(size_t o, size_t oend) {
        struct Work { size_t at, end, revision = 0; bool hybrid = false; };
        std::vector<Work> todo{{o, oend}};
        std::unordered_set<size_t> seen;
        while (!todo.empty()) {
            auto [at, end, revision, hybrid] = todo.front();
            currentRevision = revision;
            hybridSection = hybrid;
            todo.erase(todo.begin());
            if (!seen.insert(at).second) continue;
            if (seen.size() > 4096) break;
            size_t q = at;
            while (q < len && isWs(d[q])) q++;
            sectionLinks.clear();
            if (startsWith(d, len, q, "xref")) {
                Parser ps(d, q, end, &arena, &strEnd, false);
                parseTable(ps);
            } else {
                Obj ob;
                if (parseAt(q, ob, &arena) && ob.hasStream && resolve(ob.v.get("Type")).is("XRef")) parseXrefStream(ob);
                else {
                    note("XREF: invalid xref section at %zu", at);
                    xrefDamaged = true;   // (a /Prev or startxref pointing nowhere: sections missing)
                    if (seen.size() == 1) {
                        size_t o2 = rfindXref(0, oend);
                        if (o2 != SIZE_MAX && !seen.count(o2)) todo.push_back({o2, end});
                    }
                    continue;
                }
            }
            std::vector<Val> links = sectionLinks;
            for (Val &t : links)
                for (const char *name : {"XRefStm", "Prev"}) {
                    Val v = t.get(name);
                    if (v.t != INT) continue;
                    if (v.i == 0 && !strcmp(name, "Prev")) continue;   // (written instead of omitted)
                    if (v.i < 0 || (uint64_t)v.i + base >= pend) {
                        note("XREF: invalid /%s offset", name);
                        xrefDamaged = true;
                        continue;
                    }
                    // (the stream of a hybrid file before the older sections)
                    if (!strcmp(name, "XRefStm")) todo.insert(todo.begin(), {(size_t)v.i + base, pend, revision, true});
                    else todo.push_back({(size_t)v.i + base, pend, revision + 1, false});
                }
        }
    }

    // the xref entries pointing where their object's header is not
    size_t verifyXref() {
        size_t bad = 0, n = 0;
        for (auto &e : xref) {
            if (e.second.type != 1) continue;
            n++;
            size_t pos = (size_t)e.second.off;
            bool ok = false;
            if (pos >= headEnd && pos + 5 < pend) {
                size_t q = pos;
                while (q < pend && q < pos + 4 && isWs(d[q])) q++;
                uint32_t oid, gen;
                // (the number right; a generation differing from the xref's is tolerated, as readers do)
                ok = !(q > 0 && isDigit(d[q - 1])) && headerAt(q, &oid, &gen) && oid == e.first;
            }
            if (!ok) e.second.bad = true, bad++;
        }
        if (bad) note("XREF: %zu/%zu entries point to wrong positions", bad, n);
        return n && bad * 2 > n ? SIZE_MAX : bad;
    }

    // a text-mode transfer's damage: all of it text, its line endings converted, so that startxref
    // points nowhere; reverted if that makes it right
    bool eolRepair(size_t hdr) {
        size_t q = rfind(d, hdr, len, "startxref");
        if (q == SIZE_MAX) return false;
        size_t p = q + 9, w = p;
        while (p < len && isWs(d[p])) p++;
        if (p == w) return false;
        size_t s = p;
        uint64_t o = 0;
        while (p < len && isDigit(d[p]) && p - s < 10) o = o * 10 + (d[p++] - '0');
        if (p == s) return false;
        if (sectionAt(hdr + o) || sectionAt(o)) return false;
        size_t e = p;
        while (e < len && e < p + 32 && (d[e] == 9 || d[e] == 10 || d[e] == 13 || d[e] == 32)) e++;
        if (startsWith(d, len, e, "%%EOF")) e += 5;
        if (e < len && d[e] == 13) e++;
        if (e < len && d[e] == 10) e++;
        size_t cr = 0, lf = 0, crlf = 0;
        for (size_t k = hdr; k < e; k++) {
            uint8_t c = d[k];
            if (c < 32 && c != 9 && c != 10 && c != 13) return false;   // binary: 0D 0A may have been there
            cr += c == 13, lf += c == 10;
            crlf += c == 13 && k + 1 < e && d[k + 1] == 10;
        }
        std::string body;
        if (cr == 0 && lf) {   // CRLF -> LF
            for (size_t k = hdr; k < e; k++) {
                if (d[k] == 10) body.push_back(13);
                body.push_back((char)d[k]);
            }
        } else if (cr == lf && crlf == cr && cr) {   // LF -> CRLF
            for (size_t k = hdr; k < e; k++)
                if (!(d[k] == 13 && k + 1 < e && d[k + 1] == 10)) body.push_back((char)d[k]);
        } else return false;
        std::string all((const char *)d, hdr);
        all += body;
        all.append((const char *)d + e, len - e);
        const uint8_t *od = d;
        size_t ol = len;
        d = (const uint8_t *)all.data(), len = all.size();
        bool ok = sectionAt(hdr + o) || sectionAt(o);
        if (!ok) {
            d = od, len = ol;
            return false;
        }
        fixed.swap(all);
        d = (const uint8_t *)fixed.data(), len = fixed.size();
        note("TRANSFER: %s line ending conversion damage reverted", cr == 0 ? "CRLF -> LF" : "LF -> CRLF");
        return true;
    }

    // "startxref N" (and a part of "%%EOF") at the very end: complete, only the %%EOF missing
    bool completeTail(size_t e) {
        size_t from = e > 64 ? e - 64 : 0;
        size_t q = rfind(d, from, e, "startxref");
        if (q == SIZE_MAX) return false;
        size_t p = q + 9, w = p;
        while (p < e && isWs(d[p])) p++;
        if (p == w) return false;
        size_t s = p;
        while (p < e && isDigit(d[p])) p++;
        if (p == s) return false;
        while (p < e && isWs(d[p])) p++;
        for (const char *t = "%%EOF"; *t && p < e && d[p] == (uint8_t)*t; t++) p++;
        while (p < e && isWs(d[p])) p++;
        return p == e;
    }

    // pdf structure after the last %%EOF (an interrupted download), or no %%EOF at all
    void checkTruncated() {
        size_t e = len;
        while (e && d[e - 1] == 0) e--;
        if (completeTail(e)) return;
        size_t last = rfind(d, 0, e, "%%EOF");
        bool after = false;
        if (last != SIZE_MAX) {
            size_t from = last + 5;
            after = find(d, from, e, "endobj") != SIZE_MAX || find(d, from, e, "endstream") != SIZE_MAX ||
                    find(d, from, e, "trailer") != SIZE_MAX;
            if (!after) {
                findStarts();
                for (auto &s : startList)
                    if (s.first >= from && s.first < e) after = true;
            }
            if (after) {
                size_t t = from;
                while (t < e && isWs(d[t])) t++;
                bool boundary = headerAt(t, nullptr, nullptr) || startsWith(d, e, t, "xref");
                bool xrefOk = !xref.empty();
                for (auto &x : xref) xrefOk &= !x.second.bad;
                if (!boundary && xrefOk) return;   // an older file's leftover after a complete one
            }
        }
        if (after || last == SIZE_MAX) {
            truncated = true;
            note("TRUNCATED: incomplete file");
        }
    }

    // --- the security handler ---

    void setupCrypt() {
        Val enc = encryptRef;
        if (enc.t == REF) {
            encOid = enc.oid();
            enc = getObject(encOid);
        }
        if (enc.t != DICT) {
            doc->crypt = "encrypted, its dictionary not found";
            return;
        }
        auto str = [&](const char *k) {
            Val v = resolve(enc.get(k));
            return v.t == STR ? std::string(v.s, v.n) : std::string();
        };
        std::string O = str("O"), U = str("U"), UE = str("UE");
        int V = (int)resolve(enc.get("V")).num(0), R = (int)resolve(enc.get("R")).num(0);
        int64_t P = (int64_t)resolve(enc.get("P")).num(0);
        Val Lv = resolve(enc.get("Length"));
        int L = Lv.t == INT ? (int)Lv.i : 40;
        Val em = resolve(enc.get("EncryptMetadata"));
        encMeta = !(em.t == BOOL && !em.i);
        cryptR = R;
        // V4+: the crypt filters /StmF and /StrF (none: /Identity), each with its /CFM and /Length
        Val CF = resolve(enc.get("CF"));
        int cfl = 0;
        bool aesv2 = false;
        auto filterKeyLength = [&](Val cf) {
            aesv2 |= resolve(cf.get("CFM")).is("AESV2");
            Val l = resolve(cf.get("Length"));
            if (l.t == INT && l.i > 0) cfl = (int)(l.i <= 32 ? l.i : l.i / 8);
        };
        auto method = [&](const char *key, bool &aes, bool &none) {
            Val f = resolve(enc.get(key));
            none = V >= 4 && (f.t == NUL || f.is("Identity"));
            aes = false;
            if (V < 4 || none) return;
            Val cf = resolve(CF.get(std::string(f.str()).c_str()));
            Val cfm = resolve(cf.get("CFM"));
            none = cfm.t == NUL || cfm.is("None");
            aes = cfm.is("AESV2") || cfm.is("AESV3");
            filterKeyLength(cf);
        };
        method("StmF", stmAes, stmNone);
        method("StrF", strAes, strNone);
        // With Identity defaults, encryption may be used only by explicit /Crypt filters.
        if (V == 4 && stmNone && strNone && CF.t == DICT)
            for (uint32_t k = 0; k < CF.n; k++) {
                if (CF.v[2 * k].is("Identity")) continue;
                Val cf = resolve(CF.v[2 * k + 1]);
                Val cfm = resolve(cf.get("CFM"));
                if (cfm.is("V2") || cfm.is("AESV2")) filterKeyLength(cf);
            }
        if (!resolve(enc.get("Filter")).is("Standard") || !R) {
            doc->crypt = "encrypted with an unsupported security handler";
            return;
        }
        char desc[96];
        bool ok = false;
        size_t n = 32;
        if (R <= 4) {
            n = R == 2 ? 5 : (size_t)std::max(5, std::min(16, L >= 40 ? L / 8 : L));
            if (cfl) n = (size_t)std::max(5, std::min(16, cfl));
            if (aesv2) n = 16;
            if (!hasId) {
                doc->crypt = "encrypted, no /ID";
                return;
            }
            std::string b((const char *)PDF_PAD, 32);
            b += O.substr(0, 32);
            uint32_t p32 = (uint32_t)P;
            for (int k = 0; k < 4; k++) b.push_back((char)(p32 >> (8 * k)));
            b += fileId;
            if (R >= 4 && !encMeta) b += "\xff\xff\xff\xff";
            uint8_t k16[16];
            nc_md5(b.data(), b.size(), k16);
            if (R >= 3)
                for (int k = 0; k < 50; k++) nc_md5(k16, n, k16);
            std::vector<uint8_t> k(k16, k16 + n);
            if (R == 2) {
                uint8_t c[32];
                nc_rc4(k.data(), n, PDF_PAD, 32, c);
                ok = U.size() >= 32 && memcmp(c, U.data(), 32) == 0;
            } else {
                std::string h((const char *)PDF_PAD, 32);
                h += fileId;
                uint8_t c[16];
                nc_md5(h.data(), h.size(), c);
                nc_rc4(k.data(), n, c, 16, c);
                for (int i = 1; i < 20; i++) {
                    std::vector<uint8_t> ki(k);
                    for (auto &x : ki) x ^= (uint8_t)i;
                    nc_rc4(ki.data(), n, c, 16, c);
                }
                ok = U.size() >= 16 && memcmp(c, U.data(), 16) == 0;
            }
            if (ok) key = k;
        } else if ((R == 5 || R == 6) && U.size() >= 48 && UE.size() >= 32) {
            auto h = [&](const uint8_t *salt, uint8_t out[32]) {
                if (R == 5) nc_sha256(salt, 8, out);
                else hash2b(salt, out);
            };
            uint8_t hv[32];
            h((const uint8_t *)U.data() + 32, hv);
            if (memcmp(hv, U.data(), 32) == 0) {
                uint8_t ik[32], iv[16] = {0};
                h((const uint8_t *)U.data() + 40, ik);
                nc_aes aes;
                nc_aes_init(&aes, ik, 32);
                key.resize(32);
                nc_aes_decrypt_cbc(&aes, iv, (const uint8_t *)UE.data(), 32, key.data());
                ok = true;
            }
            stmAes = !stmNone, strAes = !strNone;
        }
        snprintf(desc, sizeof desc, "V%d R%d %s %d bit", V, R, stmAes || strAes ? "AES" : "RC4", (int)(8 * n));
        if (!ok) {
            doc->crypt = std::string(desc) + ", user password needed: not decrypted";
            key.clear();
            return;
        }
        doc->crypt = std::string(desc) + ", empty user password: decrypted";
        decrypting = true;
    }

    std::string decrypt(uint32_t oid, uint32_t gen, const uint8_t *data, size_t n, bool stream, int method = -1) {
        bool none = stream ? stmNone : strNone, aes = stream ? stmAes : strAes;
        if (method >= 0) none = method == 0, aes = method == 2;
        if (none || !decrypting) return std::string((const char *)data, n);
        std::vector<uint8_t> k;
        if (cryptR >= 5) k = key;
        else {
            std::string b((const char *)key.data(), key.size());
            for (int s = 0; s < 3; s++) b.push_back((char)(oid >> (8 * s)));
            for (int s = 0; s < 2; s++) b.push_back((char)(gen >> (8 * s)));
            if (aes) b += "sAlT";
            uint8_t h[16];
            nc_md5(b.data(), b.size(), h);
            k.assign(h, h + std::min(key.size() + 5, (size_t)16));
        }
        std::string out(n, '\0');
        if (!aes) {
            nc_rc4(k.data(), k.size(), data, n, (uint8_t *)&out[0]);
            return out;
        }
        if (n < 32) return std::string();   // (the IV and at least a block: an empty string is 32 bytes)
        nc_aes a;
        if (!nc_aes_init(&a, k.data(), k.size())) return std::string();
        uint8_t iv[16];
        memcpy(iv, data, 16);
        size_t bl = (n - 16) / 16 * 16;
        out.resize(bl);
        nc_aes_decrypt_cbc(&a, iv, data + 16, bl, (uint8_t *)&out[0]);
        int pad = bl ? (uint8_t)out[bl - 1] : 0;
        if (pad >= 1 && pad <= 16 && (size_t)pad <= bl) {
            bool okPad = true;
            for (int k2 = 0; k2 < pad; k2++) okPad &= (uint8_t)out[bl - 1 - k2] == pad;
            if (okPad) out.resize(bl - pad);
        }
        return out;
    }

    // --- opening ---

    bool open(const uint8_t *data, size_t n) {
        d = data, len = n;
        size_t hdr = find(d, 0, std::min(len, (size_t)1024), "%PDF-");
        if (hdr == SIZE_MAX) hdr = find(d, 0, std::min(len, (size_t)1024), "%FDF-");
        if (hdr == SIZE_MAX && len >= 4 && memcmp(d, "\x78\x9f\x3e\x22", 4) == 0) {
            // Outlook's TNEF (winmail.dat): the PDF's length from its attribute's header
            size_t m = find(d, 0, len, "\x02\x0f\x80\x06");
            if (m != SIZE_MAX && m + 10 <= len && d[m + 4] == 0 && startsWith(d, len, m + 9, "%PDF-")) {
                uint32_t L = d[m + 5] | d[m + 6] << 8 | d[m + 7] << 16 | (uint32_t)d[m + 8] << 24;
                if (m + 9 + (size_t)L <= len) len = m + 9 + L;
            }
        }
        if (hdr == SIZE_MAX) {
            hdr = find(d, 0, len, "%PDF-");   // (beyond the first 1024 bytes: opened by some readers)
            if (hdr == SIZE_MAX) return false;
            note("JUNK: header beyond the first 1024 bytes");
        }
        size_t p = hdr + 5;
        while (p < len && (isDigit(d[p]) || d[p] == '.')) p++;
        headEnd = p;
        eolRepair(hdr);
        pend = len;
        size_t q = rfind(d, headEnd, len, "startxref");
        size_t o = SIZE_MAX;
        if (q != SIZE_MAX) {
            size_t oend = q;
            q += 9;
            while (q < pend && d[q] <= 32) q++;
            size_t s = q;
            uint64_t v = 0;
            while (q < pend && isDigit(d[q]) && q - s < 15) v = v * 10 + (d[q++] - '0');
            size_t r = q;
            while (r < pend && (d[r] == 32 || d[r] == 9)) r++;
            bool num = q > s && (r >= pend || d[r] == 10 || d[r] == 13);
            if (num) o = (size_t)v;
            else note("XREF: invalid startxref value");
            if (!num) q = s;
            while (q < pend && d[q] <= 32) q++;
            if (startsWith(d, pend, q, "%%EOF")) {
                bool structure = false;
                size_t from = q + 5;
                if (pend > q + 7) {
                    structure = find(d, from, pend, "endobj") != SIZE_MAX || find(d, from, pend, "endstream") != SIZE_MAX ||
                                find(d, from, pend, " obj") != SIZE_MAX || find(d, from, pend, "trailer") != SIZE_MAX;
                }
                if (!structure) pend = q + 5;   // (junk after %%EOF: not read)
            } else note("XREF: missing EOF");
            // junk before the header: the offsets relative to it, if they point right that way
            if (hdr > 0 && o != SIZE_MAX && !sectionAt(o) && sectionAt(o + hdr)) {
                base = hdr;
                o += hdr;
                note("JUNK: %zu bytes before the %%PDF header, offsets relative to it", hdr);
            }
            if (o != SIZE_MAX && (o < headEnd || o >= oend)) {
                size_t o2 = rfindXref(headEnd, oend);
                note("XREF: invalid startxref offset");
                o = o2;
            }
            if (o != SIZE_MAX) parseXref(o, oend);
        } else note("XREF: no startxref");
        size_t bad = verifyXref();
        checkTruncated();
        if (encrypted) {
            setupCrypt();
            cache.clear();   // (what was read before the key was known)
            streams.clear();
        }
        bool useXref = !xref.empty() && bad != SIZE_MAX && !truncated && !xrefDamaged;
        if (!useXref) {
            doc->repaired = true;
            scan();
            note("REPAIR: objects found by reading the whole file (%zu)", found.size());
        }
        Val root = resolve(doc->rootDict);
        if (root.t != DICT || !(resolve(root.get("Pages")).t == DICT)) {
            if (!scanned) scan();
            if (!catalog)   // (in an object stream)
                for (uint32_t so : objStms) {
                    for (auto &mm : loadObjStm(so)->members)
                        if (resolve(getObject(mm.first).get("Type")).is("Catalog")) catalog = mm.first;
                    if (catalog) break;
                }
            if (catalog) {
                doc->repaired = true;
                root = getObject(catalog);
                note("REPAIR: the catalog object used (no /Root)");
            }
        }
        doc->rootDict = root.t == DICT ? root : Val();
        return true;
    }
};

// --- the document's interface ------------------------------------------------------

Doc::Doc() : m(new Impl) { m->doc = this; }
Doc::~Doc() { delete m; }

bool Doc::open(const void *data, size_t len) {
    Impl *fresh = new Impl;
    fresh->doc = this;
    delete m;
    m = fresh;
    rootDict = Val();
    pagesDone = false;
    pages.clear();
    repaired = false;
    crypt.clear();
    notes.clear();
    return m->open((const uint8_t *)data, len);
}
Val Doc::object(uint32_t oid) { return m->getObject(oid); }
Val Doc::resolve(Val v) { return m->resolve(v); }
const Stream *Doc::stream(uint32_t oid) { return m->streamOf(oid); }

bool Doc::decode(const Stream &s, Decoded &out, size_t limit) {
    out = Decoded();
    Val dict = s.dict;
    std::string data;
    Val filter = resolve(dict.get("Filter")), parms = resolve(dict.get("DecodeParms"));
    if (parms.t == NUL) parms = resolve(dict.get("DP"));
    std::vector<Val> filters;
    if (filter.t == ARRAY)
        for (uint32_t k = 0; k < filter.n; k++) filters.push_back(resolve(filter.at(k)));
    else if (filter.t == NAME) filters.push_back(filter);
    bool noCrypt = false;
    for (Val &f : filters) noCrypt |= f.is("Crypt");
    Val type = resolve(dict.get("Type"));
    if (m->decrypting && s.oid != m->encOid && !type.is("XRef") && !(type.is("Metadata") && !m->encMeta) && !noCrypt)
        data = m->decrypt(s.oid, s.gen, s.raw, s.len, true);
    else data.assign((const char *)s.raw, s.len);
    for (size_t k = 0; k < filters.size(); k++) {
        Val f = filters[k];
        Val p = parms.t == ARRAY ? resolve(parms.at(k)) : parms.t == DICT && (filters.size() == 1 || f.is("FlateDecode") || f.is("Fl") ||
                                                                               f.is("LZWDecode") || f.is("LZW")) ? parms : Val();
        std::string next, err, note;
        bool predict = false;
        if (f.is("FlateDecode") || f.is("Fl")) flate((const uint8_t *)data.data(), data.size(), next, limit, err, note), predict = true;
        else if (f.is("LZWDecode") || f.is("LZW")) {
            Val e = resolve(p.get("EarlyChange"));
            lzw((const uint8_t *)data.data(), data.size(), e.t == INT ? (int)e.i : 1, s.exact, next, limit, err, note);
            predict = true;
        } else if (f.is("ASCII85Decode") || f.is("A85")) ascii85((const uint8_t *)data.data(), data.size(), next, note);
        else if (f.is("ASCIIHexDecode") || f.is("AHx")) asciiHex((const uint8_t *)data.data(), data.size(), next, err);
        else if (f.is("RunLengthDecode") || f.is("RL")) runLength((const uint8_t *)data.data(), data.size(), next, limit);
        else if (f.is("Crypt")) {
            Val name = resolve(p.get("Name"));
            if (name.t == NUL || name.is("Identity")) continue;
            Val enc = resolve(m->encryptRef);
            Val cf = get(enc, "CF");
            Val spec = resolve(cf.get(std::string(name.str()).c_str()));
            Val cfm = resolve(spec.get("CFM"));
            if (spec.t != DICT || !(cfm.t == NUL || cfm.is("None") || cfm.is("V2") || cfm.is("AESV2") || cfm.is("AESV3"))) {
                if (out.error.empty()) out.error = "Crypt: unsupported crypt filter " + std::string(name.str());
                break;
            }
            if (cfm.t == NUL || cfm.is("None")) continue;
            if (!m->decrypting) {
                if (out.error.empty()) out.error = "Crypt: decryption key unavailable";
                break;
            }
            // An inferred stream end can include a line ending after the ciphertext.
            if (!cfm.is("V2") && (data.size() < 32 || (s.exact && data.size() % 16))) {
                if (out.error.empty()) out.error = "Crypt: invalid AES stream length";
                break;
            }
            next = m->decrypt(s.oid, s.gen, (const uint8_t *)data.data(), data.size(), true, cfm.is("V2") ? 1 : 2);
        }
        else if (f.is("DCTDecode") || f.is("DCT") || f.is("CCITTFaxDecode") || f.is("CCF") || f.is("JBIG2Decode") || f.is("JPXDecode")) {
            out.image = std::string(f.str());
            out.imageParms = p;
            break;
        } else {
            if (out.error.empty()) out.error = "unsupported filter " + std::string(f.str());
            break;
        }
        data.swap(next);
        if (!err.empty() && out.error.empty()) out.error = err;
        if (!note.empty()) out.note = out.note.empty() ? note : out.note + "; " + note;
        if (predict && p.t == DICT) {
            Val pr = resolve(p.get("Predictor"));
            if (pr.t == INT && pr.i > 1) {
                auto iv = [&](const char *key, int def) {
                    Val v = resolve(p.get(key));
                    return v.t == INT ? (int)std::max<int64_t>(-1, std::min<int64_t>(v.i, 1 << 25)) : def;
                };
                std::string perr;
                unpredict(data, (int)pr.i, iv("Colors", 1), iv("BitsPerComponent", 8), iv("Columns", 1), perr);
                if (!perr.empty() && out.error.empty()) out.error = perr;
            }
        }
        if (data.size() > limit) {
            data.resize(limit);
            if (out.error.empty()) out.error = "too large";
            break;
        }
    }
    out.data.swap(data);
    return out.error.empty();
}

std::vector<uint32_t> Doc::objectIds(bool scan) {
    std::unordered_set<uint32_t> ids;
    if (scan) m->scan();
    for (auto &e : m->xref)
        if (e.second.type == 1 || e.second.type == 2) ids.insert(e.first);
    for (auto &f : m->found) ids.insert(f.first);
    for (uint32_t s : m->allObjStms(scan || repaired)) {
        Impl::ObjStm *o = m->loadObjStm(s);
        for (auto &mm : o->members) ids.insert(mm.first);
    }
    std::vector<uint32_t> out(ids.begin(), ids.end());
    std::sort(out.begin(), out.end());
    return out;
}

// --- pages -------------------------------------------------------------------------

namespace {
bool rect(Doc &doc, Val v, double r[4]) {
    v = doc.resolve(v);
    if (v.t != ARRAY || v.n < 4) return false;
    double x[4];
    for (int k = 0; k < 4; k++) {
        Val e = doc.resolve(v.at(k));
        if (!e.isNum()) return false;
        x[k] = e.num();
    }
    r[0] = std::min(x[0], x[2]), r[1] = std::min(x[1], x[3]), r[2] = std::max(x[0], x[2]), r[3] = std::max(x[1], x[3]);
    return r[2] > r[0] && r[3] > r[1];
}
}  // namespace

void Doc::collectPages() {
    if (pagesDone) return;
    pagesDone = true;
    struct Inh {
        Val res, media, crop, rotate;
    };
    std::unordered_set<uint32_t> seen;
    size_t nodes = 0;
    struct Walker {
        Doc &doc;
        std::unordered_set<uint32_t> &seen;
        size_t &nodes;
        void walk(Val ref, Inh inh, int depth) {
            if (depth > 64 || nodes > 2000000 || doc.pages.size() >= 1000000) return;
            uint32_t oid = ref.t == REF ? ref.oid() : 0;
            if (oid && !seen.insert(oid).second) return;   // (a loop, or a node referenced twice)
            Val node = doc.resolve(ref);
            if (node.t != DICT) return;
            nodes++;
            for (auto [key, slot] : {std::pair<const char *, Val *>{"Resources", &inh.res}, {"MediaBox", &inh.media}, {"CropBox", &inh.crop},
                                     {"Rotate", &inh.rotate}}) {
                Val v = node.get(key);
                if (v.t != NUL) *slot = v;
            }
            Val kids = doc.resolve(node.get("Kids"));
            if (!doc.resolve(node.get("Type")).is("Page") && node.get("Kids").t != NUL) {
                if (kids.t != ARRAY) return;
                for (uint32_t k = 0; k < kids.n; k++) walk(kids.at(k), inh, depth + 1);
                return;
            }
            Page p;
            p.dict = node, p.oid = oid;
            p.resources = doc.resolve(inh.res);
            double r[4];
            if (rect(doc, inh.media, r)) memcpy(p.media, r, sizeof r);
            memcpy(p.crop, p.media, sizeof r);
            if (rect(doc, inh.crop, r)) {
                double c[4] = {std::max(r[0], p.media[0]), std::max(r[1], p.media[1]), std::min(r[2], p.media[2]), std::min(r[3], p.media[3])};
                if (c[2] > c[0] && c[3] > c[1]) memcpy(p.crop, c, sizeof c);
            }
            Val rot = doc.resolve(inh.rotate);
            if (rot.isNum()) p.rotate = (((int)rot.num() / 90) % 4 + 4) % 4 * 90;
            Val uu = doc.resolve(node.get("UserUnit"));
            if (uu.isNum() && uu.num() > 0) p.userUnit = uu.num();
            doc.pages.push_back(p);
        }
    } w{*this, seen, nodes};
    Val pagesRoot = rootDict.get("Pages");
    w.walk(pagesRoot, Inh(), 0);
    if (!pages.empty()) return;
    // no page tree (e.g. a truncated file without its end): the pages left in the file, in its order,
    // each with what its parents still give
    m->scan();
    std::vector<std::pair<size_t, uint32_t>> order;
    for (auto &f : m->found) order.push_back({f.second, f.first});
    std::sort(order.begin(), order.end());
    for (auto &o : order) {
        Val v = object(o.second);
        if (v.t != DICT || !resolve(v.get("Type")).is("Page")) continue;
        Inh inh;
        Val node = v;
        for (int k = 0; k < 64 && node.t == DICT; k++, node = resolve(node.get("Parent")))
            for (auto [key, slot] : {std::pair<const char *, Val *>{"Resources", &inh.res}, {"MediaBox", &inh.media}, {"CropBox", &inh.crop},
                                     {"Rotate", &inh.rotate}})
                if (slot->t == NUL) *slot = node.get(key);
        Val ref;
        ref.t = REF, ref.i = o.second;
        seen.erase(o.second);
        w.walk(ref, inh, 0);
    }
    if (!pages.empty()) notes.push_back("REPAIR: no page tree, the page objects found used");
}

size_t Doc::pageCount() {
    collectPages();
    return pages.size();
}

bool Doc::page(size_t k, Page &p) {
    collectPages();
    if (k >= pages.size()) return false;
    p = pages[k];
    return true;
}

bool Doc::contents(const Page &p, std::string &out, std::string *err) {
    out.clear();
    Val c = p.dict.get("Contents");
    std::vector<Val> parts;
    Val r = resolve(c);
    if (r.t == ARRAY)
        for (uint32_t k = 0; k < r.n; k++) parts.push_back(r.at(k));
    else parts.push_back(c);
    bool ok = true;
    for (size_t k = 0; k < parts.size(); k++) {
        if (parts[k].t != REF) continue;
        const Stream *s = stream(parts[k].oid());
        if (!s) continue;
        Decoded dec;
        if (!decode(*s, dec)) {
            ok = false;
            if (err && err->empty()) *err = dec.error;
        }
        if (k) out.push_back('\n');
        out += dec.data;
    }
    return ok;
}

// --- the content stream lexer ------------------------------------------------------

struct ContentLexer::State {
    Arena arena;
    StrEnd strEnd;
};

ContentLexer::ContentLexer(const char *data, size_t len) : d(data), n(len), st(new State) {}
ContentLexer::~ContentLexer() { delete st; }

bool ContentLexer::next(Op &op) {
    st->arena.reset();
    args.clear();
    while (p < n) {
        Parser ps((const uint8_t *)d, p, n, &st->arena, &st->strEnd, true);
        Tok &t = ps.peek(0);
        if (t.k == T_END) {
            p = n;
            break;
        }
        if (t.k == T_KW && !(t.kw("true") || t.kw("false") || t.kw("null"))) {
            Tok o = ps.take();
            p = o.end;
            if (o.kw("BI")) {   // (its dictionary follows as operands of ID)
                args.clear();
                continue;
            }
            op.image = Val(), op.imageData = std::string_view();
            if (o.kw("ID")) {
                // the data after one whitespace byte, up to an "EI" between whitespace (or the end); PDF 2.0: /L
                std::vector<Val> pairs;
                for (size_t k = 0; k + 1 < args.size(); k += 2)
                    if (args[k].t == NAME) pairs.push_back(args[k]), pairs.push_back(args[k + 1]);
                Val dict;
                dict.t = DICT;
                Val *mem = (Val *)st->arena.alloc(sizeof(Val) * (pairs.empty() ? 1 : pairs.size()));
                if (!pairs.empty()) memcpy((void *)mem, pairs.data(), sizeof(Val) * pairs.size());
                dict.v = mem, dict.n = (uint32_t)(pairs.size() / 2);
                size_t q = p < n && isWs((uint8_t)d[p]) ? p + 1 : p;
                Val L = dict.get("L");
                if (L.t == NUL) L = dict.get("Length");
                bool lengthKnown = L.t == INT && L.i >= 0 && (uint64_t)L.i <= n - q;
                size_t from = q + (lengthKnown ? (size_t)L.i : 0);
                size_t imageSize = lengthKnown ? (size_t)L.i : 0;
                // Unfiltered inline images have an exact size. Their pixels may contain " EI ".
                auto param = [&](const char *shortKey, const char *longKey) {
                    Val v = dict.get(shortKey);
                    return v.t == NUL ? dict.get(longKey) : v;
                };
                Val filter = param("F", "Filter");
                if (!lengthKnown && (filter.t == NUL || (filter.t == ARRAY && !filter.n))) {
                    Val w = param("W", "Width"), h = param("H", "Height");
                    Val b = param("BPC", "BitsPerComponent"), cs = param("CS", "ColorSpace");
                    Val mask = param("IM", "ImageMask");
                    int colors = cs.is("G") || cs.is("DeviceGray") || cs.is("I") || cs.is("Indexed") ? 1 :
                                 cs.is("RGB") || cs.is("DeviceRGB") ? 3 : cs.is("CMYK") || cs.is("DeviceCMYK") ? 4 : 0;
                    if (cs.t == ARRAY && (cs.at(0).is("I") || cs.at(0).is("Indexed"))) colors = 1;
                    int bits = b.t == INT && (b.i == 1 || b.i == 2 || b.i == 4 || b.i == 8 || b.i == 16) ? (int)b.i : 0;
                    if (mask.t == BOOL && mask.i) colors = 1, bits = 1;
                    if (w.t == INT && h.t == INT && w.i > 0 && h.i > 0 && colors && bits &&
                        (uint64_t)w.i <= (UINT64_MAX - 7) / (colors * bits)) {
                        uint64_t row = ((uint64_t)w.i * colors * bits + 7) / 8;
                        if (row && (uint64_t)h.i <= (n - q) / row) {
                            imageSize = (size_t)(row * h.i);
                            lengthKnown = true;
                        }
                    }
                }
                size_t e = n, after = n;
                if (lengthKnown) {
                    size_t expected = q + imageSize, end = expected;
                    while (end < n && isWs((uint8_t)d[end])) end++;
                    if (end > expected && n - end >= 2 && d[end] == 'E' && d[end + 1] == 'I' &&
                        (end + 2 == n || isWs((uint8_t)d[end + 2]) || isDelim((uint8_t)d[end + 2]))) {
                        e = expected, after = end + 2;
                    }
                }
                // Without a confirmed length, delimiters such as '%' can still be image data.
                if (e == n)
                    for (size_t s = from; s + 2 < n; s++)
                        if (isWs((uint8_t)d[s]) && d[s + 1] == 'E' && d[s + 2] == 'I' && (s + 3 == n || isWs((uint8_t)d[s + 3]))) {
                            e = s, after = s + 3;
                            break;
                        }
                op.name = "BI";
                op.args = nullptr, op.nargs = 0;
                op.image = dict;
                op.imageData = std::string_view(d + q, e > q ? e - q : 0);
                p = after;
                op.pos = p;
                return true;
            }
            op.name = std::string_view(o.s, o.n);
            op.args = args.data(), op.nargs = args.size();
            op.pos = p;
            return true;
        }
        if (t.k == T_ACLOSE || t.k == T_DCLOSE || t.k == T_STRAY || t.k == T_EOFMARK) {
            p = ps.take().end;
            continue;
        }
        Val v = ps.value(0);
        p = ps.nla ? ps.peek(0).pos : ps.lx.p;
        if (args.size() < 1u << 20) args.push_back(v);
    }
    return false;
}

}  // namespace nitropdf
