#!/usr/bin/env python3
# src/nitropdf_glyphs.h from fontTools 4.47.2's data (its encodings and the Adobe Glyph List; the files
# are read as data, not imported). FONTTOOLS_DIR: the wheel unpacked (pip download fonttools==4.47.2):
#   python3 -I scripts/gen-pdf-glyphs.py FONTTOOLS_DIR src/nitropdf_glyphs.h
import ast, os, re, sys

D = sys.argv[1]


def literal(path, name):
    src = open(os.path.join(D, path), encoding='utf-8').read()
    m = re.search(r'^' + name + r'\s*=\s*(\[.*?\])', src, re.S | re.M)
    return ast.literal_eval(m.group(1))


def agl_text(var):
    src = open(os.path.join(D, 'fontTools/agl.py'), encoding='utf-8').read()
    start = src.index(var + ' = """') + len(var + ' = """')
    return src[start:src.index('"""', start)]


StandardEncoding = literal('fontTools/encodings/StandardEncoding.py', 'StandardEncoding')
MacRoman = literal('fontTools/encodings/MacRoman.py', 'MacRoman')
LEGACY, AGLFN, UV2AGL = {}, {}, {}
for line in agl_text('_aglText').splitlines():   # name;XXXX[ XXXX]
    line = line.strip('\\')
    if not line or line.startswith('#') or ';' not in line: continue
    name, uvs = line.split(';')[:2]
    u = [int(x, 16) for x in uvs.split()]
    if len(u) == 1: LEGACY[name] = u[0]
for line in agl_text('_aglfnText').splitlines():   # XXXX;name;description
    line = line.strip('\\')
    if not line or line.startswith('#') or ';' not in line: continue
    uv, name = line.split(';')[:2]
    AGLFN[name] = int(uv, 16)
    UV2AGL[int(uv, 16)] = name


def norm(lst):
    return [None if (n is None or n == '.notdef') else n for n in lst]


std = norm(StandardEncoding)
mac = norm(MacRoman)
assert len(std) == 256 and len(mac) == 256
# PDF's MacRomanEncoding (ISO 32000 D.2) lacks Apple's own: 0xDB is currency, 0xF0 (apple) undefined
mac[0xF0] = None
mac[0xDB] = 'currency'
# WinAnsiEncoding: cp1252 by the glyph list's names; 0x7F and cp1252's undefined codes above it:
# bullet; 0xA0 space, 0xAD hyphen (D.2's notes)
win = [None] * 256
for c in range(32, 256):
    try:
        u = bytes([c]).decode('cp1252')
    except UnicodeDecodeError:
        u = None
    if c == 0x7F or u is None:
        win[c] = 'bullet' if c >= 0x7F else None
    elif c == 0xA0:
        win[c] = 'space'
    elif c == 0xAD:
        win[c] = 'hyphen'
    else:
        win[c] = UV2AGL.get(ord(u)) or min((n for n, v in LEGACY.items() if v == ord(u)), default=None)
        assert win[c], hex(c)

pairs = dict(LEGACY)   # (the legacy list: old fonts' afii... names)
for name, uv in AGLFN.items(): pairs.setdefault(name, uv)
pairs = {n: u for n, u in pairs.items() if u < 0x10000}
names = sorted(pairs, key=lambda n: n.encode())

out = ['// SPDX-License-Identifier: MIT',
       '// Generated from fontTools 4.47.2 (MIT): its StandardEncoding and MacRoman, cp1252 named by the',
       "// Adobe Glyph List (BSD-3-Clause, Adobe), PDF's changes to them (ISO 32000-1 annex D), and the list's",
       '// names of single BMP characters. Data only, for nitropdf.cpp.',
       '#pragma once', '#include <stdint.h>', '']


def table(cname, lst):
    out.append('static const char *const %s[256] = {' % cname)
    row = []
    for n in lst:
        row.append('"%s"' % n if n else '0')
        if len(row) == 8:
            out.append('    ' + ', '.join(row) + ',')
            row = []
    out.append('};')
    out.append('')


table('kStandardEncoding', std)
table('kWinAnsiEncoding', win)
table('kMacRomanEncoding', mac)
out += ['// sorted by name (strcmp): bsearch', 'struct GlyphName {', '    const char *name;', '    uint16_t cp;', '};',
        'static const GlyphName kGlyphNames[%d] = {' % len(names)]
row = []
for n in names:
    row.append('{"%s", 0x%04X}' % (n, pairs[n]))
    if len(row) == 6:
        out.append('    ' + ', '.join(row) + ',')
        row = []
if row: out.append('    ' + ', '.join(row) + ',')
out.append('};')
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
print(len(names), 'names')
