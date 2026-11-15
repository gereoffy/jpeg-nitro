// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitrotype: font files shown as a specimen, for the viewers: TrueType and OpenType (.ttf, .otf),
// their collections (.ttc, .otc: face after face), Type 1 (.pfb, .pfa), WOFF (its tables
// inflated with zlib), WOFF2 (FreeType's reader, with Brotli's decoder) and Embedded OpenType (.eot: the font after its
// header; not a compressed one: that is MicroType Express). FreeType reads the outlines (the modules
// nitropdf uses), plutovg draws them like the other readers' drawings: at any scale, so a glyph's
// curves can be zoomed into. A face's first page: its name and what it is, lines of sample text in
// the languages of Central Europe and the rest of Latin, Cyrillic and Greek (most fonts cover
// Latin-1 alone: a character the font lacks is drawn in red from a system font, a line it mostly
// lacks is only named; a font with none of them, icons or symbols, gets no samples), then its glyphs in the order the font has them, each in a cell with its
// number, its character and its name, over its baseline and advance width. More glyphs: more
// pages. Not drawn: colour glyphs' layers and bitmaps (their outline, if any), variations (the
// default instance), hinting.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nitrotype nitrotype;

// NULL if it is not a font FreeType reads, or it has no outlines (a bitmap font). The data is copied.
nitrotype *nitrotype_load(const void *data, size_t len);
void nitrotype_free(nitrotype *font);

int nitrotype_pages(const nitrotype *font);
// 0 white (black text), 1 dark grey, 2 black (light text), 3 transparent. Set before rendering.
void nitrotype_set_background(nitrotype *font, int background);
// Size of a page, in its units (the same for every page; render coordinates start at its top left, y down).
void nitrotype_size(const nitrotype *font, int page, double *w, double *h);

// Renders a page into a w x h BGRA buffer (premultiplied): pixel (i, j) shows the point
// (x0 + (i + 0.5) / scale, y0 + (j + 0.5) / scale) of the page. Several renders of the same font
// may run at once (the bands of one): the first one of a page lays it out. Returns 0 on success,
// 1 if cancelled (may be NULL) returned nonzero: then the buffer is drawn in part.
int nitrotype_render(nitrotype *font, int page, uint8_t *bgra, int w, int h, size_t pitch, double scale, double x0, double y0,
                     int (*cancelled)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
