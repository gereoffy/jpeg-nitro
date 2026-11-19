#!/usr/bin/env python3
# src/nitrodio_stencils.h from draw.io's stencil bundle (src/main/webapp/js/stencils.min.js of github.com/jgraph/drawio:
# each library its XML in draw.io's compact form, deflated, base64) and its viewer (viewer-static.min.js: the shapes
# it draws by code, left out). The general-purpose libraries only (no cloud, vendor or brand icon sets):
#   python3 -I scripts/gen-drawio-stencils.py stencils.min.js viewer-static.min.js src/nitrodio_stencils.h
import base64, re, struct, sys, zlib
from xml.sax.saxutils import quoteattr

LIBRARIES = ('basic', 'arrows', 'flowchart', 'bpmn', 'lean_mapping', 'eip', 'floorplan', 'cabinets', 'sitemap',
             'networks', 'electrical', 'pid', 'fluid_power', 'mockup')   # (a file's name up to its first '/' or '.xml')
DRAW = {'move', 'line', 'quad', 'curve', 'arc', 'rect', 'roundrect', 'ellipse', 'text', 'image', 'include-shape'}
XS, YS = {'x', 'x1', 'x2', 'x3'}, {'y', 'y1', 'y2', 'y3'}


def decode(t):   # (the bundle's decode(): five streams - element ops, x, y and other numbers, strings)
    d = zlib.decompress(base64.b64decode(t), -15)
    o, s = 20, []
    for i in range(5):
        n = struct.unpack('>I', d[4 * i:4 * i + 4])[0]
        s.append(d[o:o + n])
        o += n
    ops, strs = s[0], s[4].decode().split('\0')
    p, si, names, attrs, out, stack = [0] * 4, 0, [], [], [], []
    px = py = 0
    pn = {}

    def read(k):   # (a zigzag varint)
        n, m = 0, 1
        while True:
            c = s[k][p[k]]
            p[k] += 1
            n += (c & 127) * m
            m *= 128
            if not c & 128:
                break
        return -(n + 1) // 2 if n % 2 else n // 2

    def num(v):
        return ('%d' % (v // 1000)) if v % 1000 == 0 else repr(v / 1000)

    while p[0] < len(ops):
        op = ops[p[0]]
        p[0] += 1
        if op == 255:
            out.append('</%s>' % stack.pop())
            continue
        if op >> 1 == len(names):
            names.append(strs[si])
            si += 1
        name = names[op >> 1]
        count = ops[p[0]]
        p[0] += 1
        coords = name in DRAW
        if name == 'shape':
            px = py = 0
            pn = {}
        at = []
        for i in range(count):
            a = ops[p[0]]
            p[0] += 1
            if a == len(attrs):
                attrs.append(strs[si])
                si += 1
            a = attrs[a]
            kind = ops[p[0]]
            p[0] += 1
            if kind == 0:
                v = strs[si]
                si += 1
            elif coords and a in XS:
                px += read(1)
                v = num(px)
            elif coords and a in YS:
                py += read(2)
                v = num(py)
            else:
                k = name + ' ' + a
                pn[k] = pn.get(k, 0) + read(3)
                v = num(pn[k])
            at.append(' %s=%s' % (a, quoteattr(v, {'\n': '&#10;'})))
        if op & 1:
            out.append('<%s%s/>' % (name, ''.join(at)))
        else:
            out.append('<%s%s>' % (name, ''.join(at)))
            stack.append(name)
    return ''.join(out)


bundle = open(sys.argv[1], encoding='utf-8').read()
code = set(m.lower() for m in re.findall(r'"(mxgraph\.[a-zA-Z0-9_.]+)"', open(sys.argv[2], encoding='utf-8').read()))
lines, shapes, used = [], 0, []
for file, data in re.findall(r"f\['([^']+)'\] = '([^']*)'", bundle):
    if re.split(r'[/.]', file)[0] not in LIBRARIES:
        continue
    x = decode(data)
    pkg = re.search(r'<shapes name="([^"]*)"', x).group(1)
    lines.append('@' + pkg)
    used.append(file)
    for sh in re.findall(r'<shape .*?</shape>', x, re.S):
        n = re.search(r' name="([^"]*)"', sh).group(1)
        if (pkg + '.' + n.replace(' ', '_')).lower() in code:
            continue
        lines.append(re.sub(r'<connections>.*?</connections>', '', sh, flags=re.S))
        shapes += 1
text = '\n'.join(lines).encode()
z = zlib.compressobj(9, zlib.DEFLATED, -15)
packed = base64.b64encode(z.compress(text) + z.flush()).decode()
with open(sys.argv[3], 'w') as f:
    f.write('// draw.io\'s general-purpose stencil libraries (stencils.min.js of github.com/jgraph/drawio, Apache License 2.0;\n'
            '// JGraph\'s stencil terms: not for use in Atlassian products), made by scripts/gen-drawio-stencils.py:\n// ' +
            ', '.join(LIBRARIES) + '.\n'
            '// Their shapes without connection points, those draw.io draws by code left out (%d shapes, %d KB of XML):\n'
            '// "@<package>" starts a library, then a <shape> a line; its name as the style\'s, spaces as _, any case.\n'
            '// Base64 of the raw deflated text.\n#pragma once\nstatic const char kLibraryStencils[] =\n' % (shapes, len(text) // 1024))
    for i in range(0, len(packed), 120):
        f.write('    "%s"\n' % packed[i:i + 120])
    f.write('    ;\n')
print(len(used), 'libraries,', shapes, 'shapes,', len(text), 'bytes of XML,', len(packed), 'of base64', file=sys.stderr)
