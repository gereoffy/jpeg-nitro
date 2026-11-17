#!/usr/bin/env python3
# Compares jxldec's PAM (16-bit RGBA, premultiplied) with a djxl reference (PPM / PAM, 8 or 16 bit).
# Usage: jxlcompare.py ours.pam ref.p[pa]m  -> max and mean absolute difference in 8-bit units
import sys
import numpy as np

def read_pnm(path):
    data = open(path, 'rb').read()
    if data[:2] in (b'PF', b'Pf'):
        f = data.split(b'\n', 3)
        w, h = map(int, f[1].split()); scale = float(f[2]); d = 3 if data[:2] == b'PF' else 1
        a = np.frombuffer(f[3], dtype='<f4' if scale < 0 else '>f4', count=w * h * d).reshape(h, w, d)[::-1].astype(np.float64)
        if '--linear' in sys.argv:
            a = np.where(a <= 0.0031308, 12.92 * a, 1.055 * np.power(np.maximum(a, 0), 1 / 2.4) - 0.055)
        return np.clip(a, 0, 1)
    if data[:2] == b'P7':
        end = data.index(b'ENDHDR\n') + 7
        hdr = dict(l.split(b' ', 1) for l in data[3:end - 7].split(b'\n') if b' ' in l)
        w, h, d, mx = int(hdr[b'WIDTH']), int(hdr[b'HEIGHT']), int(hdr[b'DEPTH']), int(hdr[b'MAXVAL'])
        body = data[end:]
    else:
        parts, pos = [], 2
        while len(parts) < 3:
            while data[pos:pos + 1].isspace(): pos += 1
            if data[pos:pos + 1] == b'#':
                pos = data.index(b'\n', pos); continue
            s = pos
            while not data[pos:pos + 1].isspace(): pos += 1
            parts.append(int(data[s:pos]))
        pos += 1
        w, h, mx = parts
        d = 3 if data[:2] == b'P6' else 1
        body = data[pos:]
    dt = np.dtype('>u2') if mx > 255 else np.uint8
    a = np.frombuffer(body, dtype=dt, count=w * h * d).reshape(h, w, d).astype(np.float64) / mx
    return a

ours, ref = read_pnm(sys.argv[1]), read_pnm(sys.argv[2])
if ref.shape[2] == 1: ref = np.repeat(ref, 3, axis=2)
if ref.shape[2] == 2: ref = np.concatenate([np.repeat(ref[:, :, :1], 3, axis=2), ref[:, :, 1:]], axis=2)
if ref.shape[2] == 4:
    ref = np.concatenate([ref[:, :, :3] * ref[:, :, 3:], ref[:, :, 3:]], axis=2)
else:   # (no alpha in the reference: ours is premultiplied)
    ref = ref * ours[:, :, 3:]
    ours = ours[:, :, :3]
if ours.shape[:2] != ref.shape[:2]:
    print('size differs', ours.shape, ref.shape); sys.exit(1)
d = np.abs(ours - ref) * 255
print('max %.2f mean %.4f  (8-bit units)' % (d.max(), d.mean()))
if '--where' in sys.argv:
    m = d.max(axis=2)
    ys, xs = np.nonzero(m > 1.0)
    blocks = sorted(set(zip((ys // 8).tolist(), (xs // 8).tolist())))
    print('blocks (by, bx) with errors > 1:', len(blocks), blocks[:40])
