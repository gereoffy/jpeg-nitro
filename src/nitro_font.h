// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_font: the fonts of the vector renderers (nitrosvg through lunasvg, nitrodxf, nitrocdr).
// One index of the installed font files (the system's and the user's font directories, read once
// on first use), their faces loaded once with plutovg and shared by the three renderers, and the
// one lock the index and the loading take (lunasvg renders under it too; nitrodxf and nitrocdr
// draw without it: plutovg's glyph caches take several threads at once, our lunasvg fork).
#pragma once
#include <plutovg.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Held around loading a drawing with text and around lunasvg's calls, and by the callers of the
// functions below.
std::mutex &nf_lock();

// The closest face of a family (its name as the font file gives it; the case does not matter) by
// weight (100..900, OS/2 usWeightClass: 400 regular, 700 bold) and slant: the slant first, then a
// normal width (not a condensed or expanded face), then the weight; of two equally close weights
// the one on the requested side (a light weight: the lighter). NULL if the family is not
// installed. The face stays loaded for the process; it is not to be destroyed.
plutovg_font_face_t *nf_face(const char *family, int weight, bool italic);
// The file (and the index in a collection) a face of nf_face's was loaded from: for a reader that needs
// the font's own glyph numbers (a PDF's font not embedded, its text by them). false if not one of ours.
bool nf_face_file(plutovg_font_face_t *face, std::string &file, int &ttc);
// Whether the family has an upright (italic false) or an italic face at all.
bool nf_has_style(const char *family, bool italic);
// Whether a face of nf_face's has a glyph for the character (else its "missing glyph" box shows).
bool nf_has_glyph(plutovg_font_face_t *face, uint32_t codepoint);
// The first installed family of a NULL-terminated list (empty names skipped), as nf_face gives it;
// NULL if none is.
plutovg_font_face_t *nf_first_face(const char *const *families, int weight, bool italic);
// A face that has a glyph for the character: of the families given first (NULL-terminated, or NULL),
// then of the common fonts (Arial, Helvetica, Lucida Grande, Arial Unicode MS, Liberation Sans,
// DejaVu Sans, then the CJK ones Noto Sans, PingFang SC, Hiragino Sans, MS Gothic, Noto Sans CJK JP);
// NULL if none has it. For a character the text's own font lacks.
plutovg_font_face_t *nf_glyph_face(const char *const *first, uint32_t codepoint, int weight, bool italic);
// A face for a CSS font-family: a family name, a generic one (sans-serif, serif, monospace,
// cursive, fantasy) or "" for the default font; what lunasvg asks for (nitrosvg sets it as its
// font resolver). A family not installed: its substitute (nf_substitute, and that one's), else, a
// fixed-width or serif one by its name (nf_generic_of), the generic family's font; else NULL.
plutovg_font_face_t *nf_css_face(const char *family, bool bold, bool italic);
// The family that stands in for one this machine may not have (case-insensitive): old CorelDRAW
// families macOS lacks (AvantGarde: Avenir ...), and the Windows / macOS core fonts that Linux has as
// their metric-compatible Liberation fonts (Arial: Liberation Sans ...); NULL if none.
const char *nf_substitute(const char *family);
// The generic family a family name says it is: "monospace" (Courier, Consolas, Lucida Console, Menlo,
// ... Mono, ... Code), "serif" (Times, Georgia, Garamond, ... Roman, ... Serif) or NULL (sans-serif or
// not known).
const char *nf_generic_of(const char *family);
// What a format says of a font's kind (a WMF font's pitch and family, a PDF font's flags), for
// nf_find_face: NF_ANY, nothing said.
enum NfKind { NF_ANY = 0, NF_SANS, NF_SERIF, NF_MONO };
// A font name as a drawing gives it, taken apart: its family, and the weight / slant the name adds to
// weight / italic. It takes: a PDF subset tag ("ABCDEF+"), a PostScript name ("TimesNewRomanPS-BoldMT",
// "Arial,Bold": "Times New Roman" bold), style words at its end ("Times New Roman Bold", "Segoe UI
// Semibold", "Arial CE"), Bitstream's abbreviations ("Futura Lt BT": Futura light), and a font file's
// name ("arialbd.ttf": Arial bold; an AutoCAD .shx stroke font: "", none).
std::string nf_font_family(const char *name, int &weight, bool &italic);
// The closest installed face for a font name as a drawing gives it (any format): the name itself, its
// family (nf_font_family; Bitstream's also as Linotype's "... LT"), the family's substitute
// (nf_substitute) and that one's; then, by kind (or, NF_ANY, by its name: nf_generic_of), a serif or
// fixed-width core font; else the sans-serif ones (Arial, Helvetica, Liberation Sans, DejaVu Sans; a
// light, medium or semibold weight from Helvetica Neue). Upright text not from a family that has only
// italic faces. NULL only if no font at all. Caller holds nf_lock.
plutovg_font_face_t *nf_find_face(const char *name, int weight, bool italic, NfKind kind = NF_ANY);
// A face that has a glyph for a character the font lacks: its family's substitute, then the common
// fonts (nf_glyph_face). Caller holds nf_lock.
plutovg_font_face_t *nf_find_glyph_face(const char *name, int weight, bool italic, uint32_t codepoint);

// UTF-8: the next code point of s from i (invalid bytes as Latin-1), and a code point appended.
uint32_t nf_utf8_next(const std::string &s, size_t &i);
void nf_utf8_put(std::string &o, uint32_t cp);
// Text in a Windows code page (1250, 932, ...; 10000 Mac Roman; 2859x ISO 8859-x), its code
// points appended to out: iconv (as in libcdr), on Windows its own converter. A byte the code
// page does not define goes through as Latin-1 (in 1252 its undefined 0x80..0x9f as '?'), as
// does all of it without a converter for the code page. Any thread.
void nf_codepage(const uint8_t *s, size_t n, unsigned cp, std::vector<uint32_t> &out);
