// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_xmltag: an XML text's tags one after the other, for the readers that only need to know
// what a document is made of before it goes to its real parser (nitrotype: an SVG font's glyphs;
// nitrosvg: a sprite's symbols). No tree, no validation. Header only (C++).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace nxt {

struct Tag {
    std::string name;   // without a namespace prefix ("svg:font" is "font")
    bool closing = false, empty = false;   // </name>; <name ... />
    std::vector<std::pair<std::string, std::string>> attrs;   // (the values' entities resolved, UTF-8)
    size_t begin = 0, end = 0;   // the tag in the text: its '<', and after its '>'
    const std::string *attr(const char *name_) const {
        for (const auto &a : attrs)
            if (a.first == name_) return &a.second;
        return nullptr;
    }
};

inline void utf8(std::string &o, uint32_t c) {
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) o += (char)(0xc0 | c >> 6), o += (char)(0x80 | (c & 63));
    else if (c < 0x10000) o += (char)(0xe0 | c >> 12), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
    else if (c < 0x110000) o += (char)(0xf0 | c >> 18), o += (char)(0x80 | (c >> 12 & 63)), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
}

// an attribute's value with its character references and the five named entities resolved
inline std::string unescape(const char *s, size_t n) {
    std::string o;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        size_t e = i + 1;
        while (e < n && e - i < 12 && s[e] != ';') e++;
        if (e >= n || s[e] != ';') {
            o += '&';
            continue;
        }
        std::string name(s + i + 1, e - i - 1);
        if (name.size() > 1 && name[0] == '#') {
            bool hex = name[1] == 'x' || name[1] == 'X';
            utf8(o, (uint32_t)strtoul(name.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10));
        } else if (name == "amp") o += '&';
        else if (name == "lt") o += '<';
        else if (name == "gt") o += '>';
        else if (name == "quot") o += '"';
        else if (name == "apos") o += '\'';
        else {
            o += '&';
            continue;
        }
        i = e;
    }
    return o;
}

// a text as an attribute's value or an element's content
inline std::string escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

// The next tag from `at` on (comments, processing instructions, CDATA and the DOCTYPE passed over);
// false: no more. `at` moves past it.
inline bool next(const char *s, size_t n, size_t &at, Tag &t) {
    auto find = [&](size_t from, const char *what) {
        size_t m = strlen(what);
        for (size_t i = from; i + m <= n; i++)
            if (!memcmp(s + i, what, m)) return i + m;
        return n;
    };
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    for (;;) {
        while (at < n && s[at] != '<') at++;
        if (at + 1 >= n) return false;
        if (n - at >= 4 && !memcmp(s + at, "<!--", 4)) {
            at = find(at + 4, "-->");
            continue;
        }
        if (n - at >= 9 && !memcmp(s + at, "<![CDATA[", 9)) {
            at = find(at + 9, "]]>");
            continue;
        }
        if (s[at + 1] == '?') {
            at = find(at + 2, "?>");
            continue;
        }
        if (s[at + 1] == '!') {   // (a DOCTYPE: its internal subset, in brackets, has '>' of its own)
            int depth = 0;
            for (at += 2; at < n && (s[at] != '>' || depth > 0); at++) depth += s[at] == '[' ? 1 : s[at] == ']' ? -1 : 0;
            if (at < n) at++;
            continue;
        }
        break;
    }
    t = Tag();
    t.begin = at;
    size_t i = at + 1;
    if (s[i] == '/') t.closing = true, i++;
    size_t b = i;
    while (i < n && !space(s[i]) && s[i] != '/' && s[i] != '>') i++;
    t.name.assign(s + b, i - b);
    size_t colon = t.name.rfind(':');
    if (colon != std::string::npos) t.name.erase(0, colon + 1);
    for (;;) {
        while (i < n && space(s[i])) i++;
        if (i >= n) break;
        if (s[i] == '>') {
            i++;
            break;
        }
        if (s[i] == '/') {
            t.empty = true, i++;
            continue;
        }
        b = i;
        while (i < n && !space(s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/') i++;
        std::string name(s + b, i - b);
        while (i < n && space(s[i])) i++;
        std::string value;
        if (i < n && s[i] == '=') {
            i++;
            while (i < n && space(s[i])) i++;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                b = i;
                while (i < n && s[i] != q) i++;
                value = unescape(s + b, i - b);
                if (i < n) i++;
            }
        } else if (i == b)   // (a stray character: passed over)
            i++;
        if (!name.empty()) t.attrs.emplace_back(std::move(name), std::move(value));
    }
    t.end = at = i;
    return true;
}

}   // namespace nxt
