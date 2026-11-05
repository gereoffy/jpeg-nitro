#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# bench/pdfdump (nitropdf_doc) against bench/pdfdump.py (pdfparse3) on PDF files:
#   bench/pdfcompare.py [--py PYTHON] OUTDIR file.pdf ...   (the two dumps: OUTDIR/<name>.c and .py)
# (pdfparse3 under pypy3 by default, where there is one: several times faster)
# Per file: the objects missing on either side, the values and the streams that differ, the pages'
# operator counts; the differing lines into OUTDIR/<name>.diff.
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

here = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
py = next((x for x in ('/usr/local/pypy/bin/pypy3', '/usr/local/bin/pypy3') if os.path.exists(x)), sys.executable)
if args[0] == '--py':
    py = args[1]
    args = args[2:]
out = args[0]
files = args[1:]
os.makedirs(out, exist_ok=True)


def parse(txt):
    objs, pages, summary = {}, {}, ''
    for l in txt.splitlines():
        if l.startswith('obj '):
            oid, rest = l[4:].split(' ', 1)
            objs[int(oid)] = rest
        elif l.startswith('page '):
            k = l.split()[1]
            pages[int(k)] = [x for x in l.split() if x.startswith('ops=')][0]
        elif l.startswith('summary'): summary = l
    return objs, pages, summary


def strip_stream(v):   # (an empty stream pdfparse3 keeps no stream for)
    return v.replace(' stream dec=0 md5=d41d8cd9 err=0', '')


def run(f):
    base = os.path.join(out, os.path.basename(f))
    try:
        c = subprocess.run([os.path.join(here, 'pdfdump'), f], capture_output=True, timeout=300).stdout.decode('latin-1')
    except subprocess.TimeoutExpired:
        c = 'TIMEOUT'
    try:
        p = subprocess.run([py, os.path.join(here, 'pdfdump.py'), f], capture_output=True, timeout=600).stdout.decode('latin-1')
    except subprocess.TimeoutExpired:
        p = 'TIMEOUT'
    open(base + '.c', 'w').write(c)
    open(base + '.py', 'w').write(p)
    co, cp, cs = parse(c)
    po, pg, ps = parse(p)
    onlyc = sorted(set(co) - set(po))
    onlyp = sorted(set(po) - set(co))
    val = strm = 0
    diff = []
    for oid in sorted(set(co) & set(po)):
        a, b = strip_stream(co[oid]), strip_stream(po[oid])
        if a == b: continue
        av, _, ast = a.partition(' stream ')
        bv, _, bst = b.partition(' stream ')
        if av != bv: val += 1
        elif ast.split(' #')[0] != bst:
            if 'err=1' in ast and 'err=1' in bst: continue   # (both failed: what each saved may differ)
            strm += 1
        else: continue
        diff.append('C  %d %s\nPY %d %s' % (oid, a[:2000], oid, b[:2000]))
    for oid in onlyc: diff.append('C only %d %s' % (oid, co[oid][:300]))
    for oid in onlyp: diff.append('PY only %d %s' % (oid, po[oid][:300]))
    pdiff = sum(1 for k in set(cp) | set(pg) if cp.get(k) != pg.get(k))
    open(base + '.diff', 'w').write(cs + '\n' + ps + '\n' + '\n'.join(diff) + '\n')
    return os.path.basename(f), len(co), len(po), len(onlyc), len(onlyp), val, strm, len(cp), len(pg), pdiff, cs


with ThreadPoolExecutor(os.cpu_count()) as ex:
    rows = list(ex.map(run, files))
tot = [0] * 9
print('%-70s %6s %6s %5s %5s %5s %5s %6s %6s %5s' % ('file', 'objsC', 'objsPy', 'onlyC', 'onlyP', 'value', 'strm', 'pagesC', 'pagesP', 'opsdf'))
for r in rows:
    flag = '' if r[3:7] == (0, 0, 0, 0) and r[7] == r[8] and r[9] == 0 else '  <--'
    print('%-70s %6d %6d %5d %5d %5d %5d %6d %6d %5d%s' % (r[0][:70], *r[1:10], flag))
    for k in range(9): tot[k] += r[k + 1]
print('%-70s %6d %6d %5d %5d %5d %5d %6d %6d %5d' % ('TOTAL', *tot))
