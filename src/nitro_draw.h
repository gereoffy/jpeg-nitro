// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_draw: what the display-list renderers (nitrodxf, nitrometa, nitrocdr, nitrodio; nitrosvg
// for the background) share: the backgrounds the viewers choose from, a render's canvas over the
// caller's BGRA buffer, the "stop now" callback, an item's box against the view (and against
// plutovg's fixed point), the boxes of paths and of turned rectangles, the union of boxes a page's
// area is made of, a colour on a canvas. Header only (C++), included by every reader of libnitrovec.
#pragma once
#include <plutovg.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#ifndef M_PI   // (not defined by mingw's math.h; the readers draw arcs with it)
#define M_PI 3.14159265358979323846
#endif

namespace nd {

// The backgrounds (xxx_set_background; nitro_vector's): 0 white, 1 AutoCAD's dark model space, 2
// black; 3 none (transparent: a picture inside another drawing). Opaque BGRA.
inline uint32_t background(int b) {
    static const uint32_t bgs[4] = {0xffffffff, 0xff212830, 0xff000000, 0};
    return bgs[b >= 0 && b <= 3 ? b : 0];
}

// a render's buffer, as every renderer checks it
inline bool renderArgs(int w, int h, size_t pitch, double scale) { return w > 0 && h > 0 && pitch >= (size_t)w * 4 && scale > 0; }

// the buffer filled with one colour
inline void fill(uint8_t *bgra, int w, int h, size_t pitch, uint32_t bg) {
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(bgra + (size_t)y * pitch);
        for (int x = 0; x < w; x++) row[x] = bg;
    }
}

// A canvas over the caller's buffer, which is filled with the background first; both go when it does.
struct Target {
    plutovg_surface_t *surface;
    plutovg_canvas_t *canvas;
    Target(uint8_t *bgra, int w, int h, size_t pitch, uint32_t bg) {
        fill(bgra, w, h, pitch, bg);
        surface = plutovg_surface_create_for_data(bgra, w, h, (int)pitch);
        canvas = plutovg_canvas_create(surface);
    }
    ~Target() {
        plutovg_canvas_destroy(canvas);
        plutovg_surface_destroy(surface);
    }
    Target(const Target &) = delete;
    Target &operator=(const Target &) = delete;
};

// a render's "stop now" (the viewers' cancelled callback, asked now and then while drawing)
struct Cancel {
    int (*fn)(void *);
    void *ctx;
    bool operator()() const { return fn && fn(ctx); }
};

// plutovg's fixed point overflows this many pixels out of the view (a damaged file's far-off strays)
constexpr double kFar = 1 << 24;
// whether a box (in the view's units) reaches the view, margin around it
inline bool touches(const double view[4], double bx0, double by0, double bx1, double by1, double margin = 0) {
    return !(bx1 + margin < view[0] || bx0 - margin > view[2] || by1 + margin < view[1] || by0 - margin > view[3]);
}
// ... and lies within kFar pixels of it (scale: pixels per unit)
inline bool within(const double view[4], double bx0, double by0, double bx1, double by1, double scale) {
    return std::max(std::max(view[0] - bx0, bx1 - view[2]), std::max(view[1] - by0, by1 - view[3])) * scale <= kFar;
}
// an item to draw: both
inline bool inView(const double view[4], double bx0, double by0, double bx1, double by1, double scale, double margin = 0) {
    return touches(view, bx0, by0, bx1, by1, margin) && within(view, bx0, by0, bx1, by1, scale);
}

struct Box {
    float x0, y0, x1, y1;
};
// a path's extents, margin around (a stroke's width, a miter's or an arrowhead's reach)
inline Box pathBox(const plutovg_path_t *path, float margin) {
    plutovg_rect_t r;
    plutovg_path_extents(path, &r, false);
    return {r.x - margin, r.y - margin, r.x + r.w + margin, r.y + r.h + margin};
}
// the box of the rectangle x0, y0 - x1, y1 turned by rad around cx, cy: its corners turned
inline Box turnedBox(float cx, float cy, float x0, float y0, float x1, float y1, float rad) {
    float c = cosf(rad), s = sinf(rad);
    Box b{1e30f, 1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 4; k++) {
        float dx = (k == 1 || k == 2 ? x1 : x0) - cx, dy = (k >= 2 ? y1 : y0) - cy;
        float x = cx + dx * c - dy * s, y = cy + dx * s + dy * c;
        b.x0 = std::min(b.x0, x);
        b.y0 = std::min(b.y0, y);
        b.x1 = std::max(b.x1, x);
        b.y1 = std::max(b.y1, y);
    }
    return b;
}

// The union of boxes: a page's items', the area it is shown as (with a margin around: page)
struct Union {
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    void add(double bx0, double by0, double bx1, double by1) {
        x0 = std::min(x0, bx0);
        y0 = std::min(y0, by0);
        x1 = std::max(x1, bx1);
        y1 = std::max(y1, by1);
    }
    bool valid() const { return x1 > x0 && y1 > y0; }   // (something was added, with a size)
    template <class T> void page(double margin, T &x, T &y, T &w, T &h) const {
        x = x0 - margin;
        y = y0 - margin;
        w = x1 - x0 + 2 * margin;
        h = y1 - y0 + 2 * margin;
    }
};

// a colour on the canvas: 0xAARRGGBB (times alpha), or 0xRRGGBB and an alpha
inline void setRgba(plutovg_canvas_t *c, uint32_t rgba, float alpha = 1) {
    plutovg_canvas_set_rgba(c, (rgba >> 16 & 255) / 255.0f, (rgba >> 8 & 255) / 255.0f, (rgba & 255) / 255.0f, (rgba >> 24) / 255.0f * alpha);
}
inline void setRgb(plutovg_canvas_t *c, uint32_t rgb, float a) {
    plutovg_canvas_set_rgba(c, (rgb >> 16 & 255) / 255.0f, (rgb >> 8 & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}

// A picture's halved copies (2 x 2 pixels averaged, premultiplied), each made the first time it is
// asked for, by any of the threads drawing it: drawn far smaller than its pixels, plutovg's sampling
// would skip most of them (a scanned page's thin letters fell apart). The picture stays the caller's.
class Mips {
    plutovg_surface_t *base_;
    std::vector<plutovg_surface_t *> levels_;
    std::mutex lock_;
    static plutovg_surface_t *halve(plutovg_surface_t *s) {
        int w = plutovg_surface_get_width(s), h = plutovg_surface_get_height(s), hw = (w + 1) / 2, hh = (h + 1) / 2;
        plutovg_surface_t *d = plutovg_surface_create(hw, hh);
        if (!d) return nullptr;
        const uint8_t *sp = plutovg_surface_get_data(s);
        uint8_t *dp = plutovg_surface_get_data(d);
        int ss = plutovg_surface_get_stride(s), ds = plutovg_surface_get_stride(d);
        for (int y = 0; y < hh; y++) {
            const uint8_t *r0 = sp + (size_t)(2 * y) * ss, *r1 = sp + (size_t)std::min(2 * y + 1, h - 1) * ss;
            uint8_t *o = dp + (size_t)y * ds;
            for (int x = 0; x < hw; x++) {
                int x0 = 4 * 2 * x, x1 = 4 * std::min(2 * x + 1, w - 1);
                for (int c = 0; c < 4; c++) o[4 * x + c] = (uint8_t)((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) / 4);
            }
        }
        return d;
    }

public:
    explicit Mips(plutovg_surface_t *base) : base_(base) {}
    ~Mips() {
        for (plutovg_surface_t *s : levels_) plutovg_surface_destroy(s);
    }
    Mips(const Mips &) = delete;
    Mips &operator=(const Mips &) = delete;
    // The copy to draw with at k device pixels per pixel of the picture (a reference of the
    // caller's) and how many of the picture's pixels one of its pixels is across (1, 2, 4 ...).
    plutovg_surface_t *forScale(float k, int *factor) {
        int level = 0;
        while (k > 0 && k * (1 << (level + 1)) <= 1 && level < 16) level++;   // (1 to 2 of its pixels per pixel: plutovg interpolates those)
        std::lock_guard<std::mutex> g(lock_);
        while ((int)levels_.size() < level) {
            plutovg_surface_t *from = levels_.empty() ? base_ : levels_.back();
            if (plutovg_surface_get_width(from) <= 1 && plutovg_surface_get_height(from) <= 1) break;
            plutovg_surface_t *h = halve(from);
            if (!h) break;
            levels_.push_back(h);
        }
        level = std::min(level, (int)levels_.size());
        *factor = 1 << level;
        return plutovg_surface_reference(level ? levels_[level - 1] : base_);
    }
};

}   // namespace nd
