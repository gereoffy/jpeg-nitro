#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
# Writes test PSD / PSB files (merged image only, no layers) from an image, with
# every compression the format has: 0 raw, 1 RLE (PackBits), 2 ZIP, 3 ZIP with
# prediction; 8 and 16 bit; RGB, RGBA and grayscale.
#   mkpsd.py input_image output_dir
import struct, sys, zlib, os
import numpy as np
from PIL import Image

def packbits(row):
    """PackBits (as Photoshop): runs of 3+ equal bytes become repeats."""
    out = bytearray(); i = 0; n = len(row)
    while i < n:
        j = i
        while j + 1 < n and row[j + 1] == row[i] and j - i < 127: j += 1
        if j - i >= 2:
            out += bytes([257 - (j - i + 1), row[i]]); i = j + 1; continue
        j = i
        while j < n and j - i < 128 and not (j + 2 < n and row[j] == row[j + 1] == row[j + 2]): j += 1
        out += bytes([j - i - 1]) + bytes(row[i:j]); i = j
    return bytes(out)

def write_psd(path, planes, depth, mode, comp, psb=False):
    """planes: list of 2-D arrays (uint8 or uint16), all the same size."""
    h, w = planes[0].shape
    hdr = b'8BPS' + struct.pack('>HHL', 2 if psb else 1, 0, 0) + struct.pack('>HLLHH', len(planes), h, w, depth, mode)
    body = struct.pack('>L', 0) + struct.pack('>L', 0) + (struct.pack('>Q', 0) if psb else struct.pack('>L', 0))
    rows = []
    for p in planes:
        a = p.astype('>u2') if depth == 16 else p.astype(np.uint8)
        for r in a: rows.append(r.tobytes())
    if comp == 0:
        data = struct.pack('>H', 0) + b''.join(rows)
    elif comp == 1:
        enc = [packbits(r) for r in rows]
        cnt = b''.join(struct.pack('>L' if psb else '>H', len(e)) for e in enc)
        data = struct.pack('>H', 1) + cnt + b''.join(enc)
    else:
        if comp == 3:   # prediction: per-row differences of samples (8 or 16 bit)
            pr = []
            for p in planes:
                a = p.astype(np.int64)
                d = a.copy(); d[:, 1:] = a[:, 1:] - a[:, :-1]
                d &= (0xFFFF if depth == 16 else 0xFF)
                d = d.astype('>u2') if depth == 16 else d.astype(np.uint8)
                for r in d: pr.append(r.tobytes())
            raw = b''.join(pr)
        else:
            raw = b''.join(rows)
        data = struct.pack('>H', comp) + zlib.compress(raw, 6)
    open(path, 'wb').write(hdr + body + data)

def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    im = Image.open(src).convert('RGB')
    a = np.array(im)
    rgb8 = [a[:, :, i] for i in range(3)]
    alpha = np.tile(np.linspace(0, 255, a.shape[1]).astype(np.uint8), (a.shape[0], 1))
    rgb16 = [(c.astype(np.uint16) * 257 + (np.arange(a.shape[1]) % 7).astype(np.uint16)) for c in rgb8]
    gray8 = [np.array(im.convert('L'))]
    for comp, name in [(0, 'raw'), (1, 'rle'), (2, 'zip'), (3, 'zippred')]:
        write_psd(f'{out}/rgb8_{name}.psd', rgb8, 8, 3, comp)
        write_psd(f'{out}/rgb16_{name}.psd', rgb16, 16, 3, comp)
        write_psd(f'{out}/gray8_{name}.psd', gray8, 8, 1, comp)
    write_psd(f'{out}/rgba8_rle.psd', rgb8 + [alpha], 8, 3, 1)
    write_psd(f'{out}/rgb8_rle_psb.psb', rgb8, 8, 3, 1, psb=True)
    write_psd(f'{out}/rgb8_zip_psb.psb', rgb8, 8, 3, 2, psb=True)
    print('written to', out)

if __name__ == '__main__':
    main()
