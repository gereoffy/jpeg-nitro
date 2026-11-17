// SPDX-License-Identifier: MIT
// Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
//
// nitro_help.h: the help the macOS, Windows and SDL viewers print (-h; header-only): the options a
// user needs (not the developers' --selftest) and the keys worth knowing (the README has them all,
// and the mouse). One text for the three, and what differs between them is here too, each next to
// the lines it belongs to. (The GNOME viewer has its three options from GLib and no text of its own.)
#pragma once
#include <stdio.h>
#include <string.h>

enum { NV_HELP_MAC, NV_HELP_WIN, NV_HELP_SDL };

// argv0: the program as it was started (its name is printed without the folders).
static void nv_help(int viewer, const char *argv0) {
    const char *name = argv0 && *argv0 ? argv0 : "nitroview";
    for (const char *p = name; *p; p++)
        if ((*p == '/' || *p == '\\') && p[1]) name = p + 1;
    FILE *f = stderr;
    fprintf(f,
        "usage: %s [options] file|directory ...\n"
        "\n"
        "options:\n"
        "  -f, --fullscreen      start in full screen\n"
        "  -s, --slideshow MS    slideshow: next image after MS milliseconds on screen\n"
        "  -j N                  use N threads to decode and draw (default: all logical CPUs; progressive JPEG: 4)\n"
        "  --bench               load all files and print timings, no window is shown\n",
        name);
    if (viewer == NV_HELP_WIN)
        fputs("  -n, --new-window      don't pass the files to a running nitroview\n"
              "  --register            offer nitroview for JPEG, PNG, PSD ... in Explorer (current user)\n"
              "  --unregister          remove that again\n", f);
    if (viewer == NV_HELP_SDL)
        fputs("  --register            add nitroview to the desktop's applications (\"Open with\"), current user\n"
              "  --unregister          remove that again\n", f);
    fprintf(f,
        "  -h, --help            this help\n"
        "\n"
        "keys (all of them, and the mouse: see the README):\n"
        "  PgDn / Space          next image\n"
        "  PgUp / Backspace      previous image\n"
        "  Home / End            first / last image\n"
        "  [ / ]                 previous / next page of a document\n"
        "  + / - / 0..8          zoom in / out;  0: fit to the screen,  1 .. 8: 100%% .. 800%%\n"
        "  arrows                move a zoomed image\n"
        "  W                     back to the image's own size\n"
        "  F / F11               full screen\n"
        "  E / Shift+E           exposure (DNG, DICOM) / background color (drawings) / DjVu smoothing\n"
        "  R / Shift+R           CW/CCW turn the image by 90 degrees\n"
        "  O / Shift+O           open files / a folder\n"
        "  P                     pause / resume the slideshow\n"
        "  Del / U               delete / undo: the file is moved to the .delete folder next to it\n"
        "  Q / Esc               quit (Esc in full screen: back to a window)\n");
}
