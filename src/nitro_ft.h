// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_ft: a FreeType glyph's outline as a plutovg path, for the readers that read font programs
// themselves (nitropdf: the fonts a PDF embeds; nitrotype: a font file's glyphs). Header only.
#pragma once
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <plutovg.h>

namespace nft {

struct Decompose {
    plutovg_path_t *p;
    float k;
    bool open;
};
inline int moveTo(const FT_Vector *to, void *u) {
    Decompose *d = (Decompose *)u;
    if (d->open) plutovg_path_close(d->p);
    plutovg_path_move_to(d->p, to->x * d->k, to->y * d->k);
    d->open = true;
    return 0;
}
inline int lineTo(const FT_Vector *to, void *u) {
    Decompose *d = (Decompose *)u;
    plutovg_path_line_to(d->p, to->x * d->k, to->y * d->k);
    return 0;
}
inline int conicTo(const FT_Vector *c, const FT_Vector *to, void *u) {
    Decompose *d = (Decompose *)u;
    plutovg_path_quad_to(d->p, c->x * d->k, c->y * d->k, to->x * d->k, to->y * d->k);
    return 0;
}
inline int cubicTo(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *u) {
    Decompose *d = (Decompose *)u;
    plutovg_path_cubic_to(d->p, c1->x * d->k, c1->y * d->k, c2->x * d->k, c2->y * d->k, to->x * d->k, to->y * d->k);
    return 0;
}

// The outline appended to path, each of its contours closed, its coordinates times k (1 / 64 for
// a glyph loaded at a size: 26.6 fixed point; 1 for one loaded with FT_LOAD_NO_SCALE: font units),
// y up as the font has it.
inline void outlinePath(FT_Outline *outline, plutovg_path_t *path, float k) {
    Decompose d{path, k, false};
    FT_Outline_Funcs fn = {moveTo, lineTo, conicTo, cubicTo, 0, 0};
    FT_Outline_Decompose(outline, &fn, &d);
    if (d.open) plutovg_path_close(path);
}

}   // namespace nft
