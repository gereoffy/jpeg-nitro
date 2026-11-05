#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# pdfparse3's view of a PDF in bench/pdfdump.cpp's format, to compare the two:
#   bench/pdfdump.py [--pdfparse3 DIR] file.pdf
import hashlib
import io
import os
import re
import sys

args = sys.argv[1:]
lib = os.path.expanduser('~/Claude.AI/pdfparse3')
if args and args[0] == '--pdfparse3':
    lib = args[1]
    args = args[2:]
sys.path.insert(0, lib)
import pdfparse3 as pp   # noqa: E402
import pdf2str           # noqa: E402

re_num = re.compile(rb'[+-]?(\d+\.?\d*|\.\d+)$')


def md5s(b):
    return hashlib.md5(bytes(b)).hexdigest()[:8]


def name(b):
    out = '/'
    for c in b:
        out += '#%02x' % c if c <= 32 or c >= 127 or chr(c) in '()<>[]{}/%#' else chr(c)
    return out


def canon(v, depth=0):
    if depth > 70: return '?'
    if v is None: return 'null'
    if type(v) == int: return str(v)
    if type(v) == float: return '%.6g' % v
    if isinstance(v, pp.PDFString):
        b = bytes(v.data)
        return '<%s>' % b.hex() if len(b) <= 32 else '<%d:%s>' % (len(b), md5s(b))
    if type(v) == tuple and len(v) == 2 and v[0] == 'R':   # (an object number beyond 18 digits: a real for nitropdf)
        return 'R%d' % v[1] if v[1] < 10 ** 18 else '%.6g' % v[1]
    if type(v) == list: return '[' + ' '.join(canon(x, depth + 1) for x in v) + ']'
    if type(v) == dict:
        return '<<' + ' '.join(name(k[1:]) + ' ' + canon(v[k], depth + 1) for k in sorted(v)) + '>>'
    if type(v) in (bytes, bytearray):
        if v.startswith(b'/'): return name(v[1:])
        if re_num.match(v): return '%.6g' % float(v)
        return v.decode('latin-1')
    return '?'


def main():
    d = open(args[0], 'rb').read()
    real = sys.stdout
    sys.stdout = io.StringIO()   # (pdfparse3 prints what it finds)
    try:
        p = pp.PDFParser(d)
        ok = p.parse()
        if not ok:
            sys.stdout = real
            print('notpdf')
            return
        ids = set(p.dom)
        for s in list(p.objstm.values()): ids |= set(s)
        out = []
        nstreams = nerr = 0
        for oid in sorted(ids):
            v, s = p.get_obj(oid)
            line = 'obj %d %s' % (oid, canon(v))
            if s is not None:
                dd = s.decode(predictor=True)
                nstreams += 1
                nerr += bool(s.error)
                line += ' stream dec=%d md5=%s err=%d' % (len(dd), md5s(dd), 1 if s.error else 0)
                for f in s.fmt or []:
                    if f in (b'/DCTDecode', b'/DCT', b'/CCITTFaxDecode', b'/CCF', b'/JBIG2Decode', b'/JPXDecode'):
                        line += ' img=' + f[1:].decode()
                        break
            out.append(line)
        pages = pdf2str.pages(p, maxn=10 ** 6)
        nops = 0
        for k, (page, res) in enumerate(pages):
            c = page.get(b'/Contents')
            cd = p.deref(c)
            data = b'\n'.join(pdf2str._stream_data(p, x) for x in (cd if type(cd) == list else [c]))
            n = sum(1 for op in pp.content_ops(data, maxops=10 ** 9))
            nops += n
            out.append('page %d ops=%d' % (k + 1, n))
    finally:
        sys.stdout = real
    print('summary objs=%d streams=%d decerr=%d pages=%d ops=%d crypt=%s' % (len(ids), nstreams, nerr, len(pages), nops,
                                                                                 'decrypted' if p.crypt else 'encrypted' if p.encrypt else 'none'))
    print('\n'.join(out))


main()
