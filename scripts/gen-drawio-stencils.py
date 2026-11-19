#!/usr/bin/env python3
# third_party/drawio/nitrodio_stencils.h: the draw.io stencil libraries nitrodio draws (shape=mxgraph.<library>.<name>),
# from draw.io's stencil bundle (src/main/webapp/js/stencils.min.js of github.com/jgraph/drawio, Apache License 2.0;
# JGraph's stencil terms: not for use in Atlassian products). Run by scripts/get-deps.sh:
#   python3 scripts/gen-drawio-stencils.py [LIBRARIES]
# LIBRARIES: comma-separated names as the bundle's files start (basic, electrical = all of electrical/*, signs, aws4,
# ...), "all" for every one; by default the general-purpose ones below. The bundle is downloaded once into
# third_party/drawio/ (a copy already there with the right checksum is used: the Flatpak build puts it there).
import base64, hashlib, os, re, struct, sys, urllib.request, zlib
from xml.sax.saxutils import quoteattr

DEFAULT = 'basic,arrows,flowchart,bpmn,lean_mapping,eip,floorplan,cabinets,networks,electrical,pid,mockup'
COMMIT = 'f411a11f4f6b8994d869bdbc45f4d4d9e2284f34'   # (2026-11-17; also in packaging/flatpak/*.json, with the sha256)
SHA256 = '989d72d18bbf02e61c74937d7f51f048d198bb559c3ede4ae66c08e52544b983'
URL = 'https://raw.githubusercontent.com/jgraph/drawio/%s/src/main/webapp/js/stencils.min.js' % COMMIT
DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'third_party', 'drawio')
BUNDLE, OUT = os.path.join(DIR, 'stencils.min.js'), os.path.join(DIR, 'nitrodio_stencils.h')

DRAW = {'move', 'line', 'quad', 'curve', 'arc', 'rect', 'roundrect', 'ellipse', 'text', 'image', 'include-shape'}
XS, YS = {'x', 'x1', 'x2', 'x3'}, {'y', 'y1', 'y2', 'y3'}


def decode(t):   # (the bundle's decode(): five streams - element ops, x, y and other numbers, strings - back to XML)
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

    def num(v):   # (thousandths)
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


def bundle():
    if os.path.exists(BUNDLE) and hashlib.sha256(open(BUNDLE, 'rb').read()).hexdigest() == SHA256:
        return open(BUNDLE, encoding='utf-8').read()
    data = urllib.request.urlopen(URL, timeout=120).read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        sys.exit('gen-drawio-stencils.py: %s: checksum mismatch' % URL)
    os.makedirs(DIR, exist_ok=True)
    open(BUNDLE, 'wb').write(data)
    return data.decode('utf-8')


arg = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] else DEFAULT
wanted = None if arg == 'all' else set(l.strip() for l in arg.split(',') if l.strip())
files = re.findall(r"f\['([^']+)'\] = '([^']*)'", bundle())
known = set(re.split(r'[/.]', f)[0] for f, _ in files)
if wanted and wanted - known:
    sys.exit('gen-drawio-stencils.py: no such library: %s' % ', '.join(sorted(wanted - known)))
lines, shapes, used = [], 0, []
for file, data in files:
    if wanted is not None and re.split(r'[/.]', file)[0] not in wanted:
        continue
    x = decode(data)
    lines.append('@' + re.search(r'<shapes name="([^"]*)"', x).group(1))
    used.append(file)
    for sh in re.findall(r'<shape .*?</shape>', x, re.S):   # (a shape a line, without its connection points)
        lines.append(re.sub(r'<connections>.*?</connections>', '', sh, flags=re.S))
        shapes += 1
text = '\n'.join(lines).encode()
z = zlib.compressobj(9, zlib.DEFLATED, -15)
packed = z.compress(text) + z.flush()
h = ['// Made by scripts/gen-drawio-stencils.py from draw.io %s (Apache License 2.0; JGraph\'s stencil terms: not for use\n'
     '// in Atlassian products): %s - %d libraries, %d shapes, %d KB of XML.\n'
     '// "@<package>" starts a library, then a <shape> a line, without its connection points; raw deflated.\n'
     '#pragma once\nstatic const unsigned char kLibraryStencils[] = {\n' % (COMMIT[:12], arg, len(used), shapes, len(text) // 1024)]
for i in range(0, len(packed), 24):
    h.append('    ' + ','.join('0x%02x' % c for c in packed[i:i + 24]) + ',\n')
h.append('};\n')
h = ''.join(h)
if not os.path.exists(OUT) or open(OUT).read() != h:   # (rewritten only when it changes: no rebuild otherwise)
    os.makedirs(DIR, exist_ok=True)
    open(OUT, 'w').write(h)
print('gen-drawio-stencils.py: %d libraries, %d shapes, %d KB deflated' % (len(used), shapes, len(packed) // 1024))
