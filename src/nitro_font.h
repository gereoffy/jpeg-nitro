// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_font: the fonts of the vector renderers (nitrosvg through lunasvg, nitrodxf, nitrocdr).
// One index of the installed font files (the system's and the user's font directories, read once
// on first use), their faces loaded once with plutovg and shared by the three renderers, and the
// one lock they load and draw under: plutovg's faces have glyph caches without a lock of their own.
#pragma once
#include <plutovg.h>

#include <cstdint>
#include <mutex>
#include <string>

// Held around every plutovg or lunasvg call that may touch a face (loading a drawing with text,
// rendering it), and by the callers of the functions below.
std::mutex &nf_lock();

// The closest face of a family (its name as the font file gives it; the case does not matter) by
// weight (100..900, OS/2 usWeightClass: 400 regular, 700 bold) and slant: the slant first, then a
// normal width (not a condensed or expanded face), then the weight; of two equally close weights
// the one on the requested side (a light weight: the lighter). NULL if the family is not
// installed. The face stays loaded for the process; it is not to be destroyed.
plutovg_font_face_t *nf_face(const char *family, int weight, bool italic);
// Whether the family has an upright (italic false) or an italic face at all.
bool nf_has_style(const char *family, bool italic);
// Whether a face of nf_face's has a glyph for the character (else its "missing glyph" box shows).
bool nf_has_glyph(plutovg_font_face_t *face, uint32_t codepoint);
// A face for a CSS font-family: a family name, a generic one (sans-serif, serif, monospace,
// cursive, fantasy) or "" for the default font; what lunasvg asks for (nitrosvg sets it as its
// font resolver).
plutovg_font_face_t *nf_css_face(const char *family, bool bold, bool italic);

// UTF-8: the next code point of s from i (invalid bytes as Latin-1), and a code point appended.
uint32_t nf_utf8_next(const std::string &s, size_t &i);
void nf_utf8_put(std::string &o, uint32_t cp);
