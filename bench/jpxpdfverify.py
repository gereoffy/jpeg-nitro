#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 A'rpi - part of jpeg-nitro (https://github.com/gereoffy/jpeg-nitro)
# PDF integration regressions: requires Pillow with JPEG 2000, and make examples/nitrodraw.
# Uses temporary PDFs; checks actual rendered pixels, including JP2/PDF colour precedence.
import io
import struct
import subprocess
import tempfile
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def box(kind, data):
    return struct.pack('>I4s', 8 + len(data), kind) + data


def jpx(mode, color, raw=False):
    if mode == '1':
        # Pillow cannot encode mode 1. Encode the same centred coefficient (-1 or 0)
        # in L, then declare one-bit precision in SIZ; QCD may retain wider exponents.
        data = bytearray(jpx('L', 127 + bool(color), True))
        data[data.index(b'\xff\x51') + 4 + 36] = 0
        return bytes(data)
    im = Image.new(mode, (8, 8), color)
    out = io.BytesIO()
    im.save(out, 'JPEG2000', no_jp2=raw, num_resolutions=1)
    return out.getvalue()


def palette_image(mapping=True):
    # A JP2 with one index component and two RGB palette entries; index 1 is red.
    ihdr = box(b'ihdr', struct.pack('>IIHBBBB', 8, 8, 1, 7, 7, 0, 0))
    colr = box(b'colr', struct.pack('>BBBI', 1, 0, 0, 16))
    pclr = box(b'pclr', struct.pack('>HB3B', 2, 3, 7, 7, 7) + bytes([0, 0, 255, 224, 64, 32]))
    cmap = box(b'cmap', b''.join(struct.pack('>HBB', 0, 1, c) for c in range(3)))
    if not mapping:
        cmap = b''
    return (box(b'jP  ', b'\r\n\x87\n') + box(b'ftyp', b'jp2 \0\0\0\0jp2 ') +
            box(b'jp2h', ihdr + colr + pclr + cmap) + box(b'jp2c', jpx('L', 1, True)))


def color_image(color, colorspace=16, definitions=b''):
    ihdr = box(b'ihdr', struct.pack('>IIHBBBB', 8, 8, 3, 7, 7, 0, 0))
    colr = box(b'colr', struct.pack('>BBBI', 1, 0, 0, colorspace))
    cdef = box(b'cdef', definitions) if definitions else b''
    return (box(b'jP  ', b'\r\n\x87\n') + box(b'ftyp', b'jp2 \0\0\0\0jp2 ') +
            box(b'jp2h', ihdr + colr + cdef) + box(b'jp2c', jpx('RGB', color, True)))


def pdf(path, image, extra=b'', mask=None, fill=b''):
    objects = {
        1: b'<< /Type /Catalog /Pages 2 0 R >>',
        2: b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        3: b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 64 64] /Contents 4 0 R /Resources << /XObject << /Im 5 0 R >> >> >>',
        4: (b'', fill + b' q 64 0 0 64 0 0 cm /Im Do Q'),
        5: (b'/Type /XObject /Subtype /Image /Width 8 /Height 8 /Filter /JPXDecode ' + extra, image),
    }
    if mask is not None:
        objects[6] = (b'/Type /XObject /Subtype /Image /Width 8 /Height 8 /ColorSpace /DeviceGray /Filter /JPXDecode', mask)
    data = bytearray(b'%PDF-1.7\n')
    offsets = [0]
    for oid, value in objects.items():
        offsets.append(len(data))
        if isinstance(value, tuple):
            d, stream = value
            value = b'<< ' + d + b' /Length %d >>\nstream\n' % len(stream) + stream + b'\nendstream'
        data += b'%d 0 obj\n' % oid + value + b'\nendobj\n'
    start = len(data)
    data += b'xref\n0 %d\n0000000000 65535 f \n' % len(offsets)
    data += b''.join(b'%010d 00000 n \n' % off for off in offsets[1:])
    data += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(offsets), start)
    path.write_bytes(data)


def main():
    rgb = jpx('RGB', (224, 64, 32))
    rgba = jpx('RGBA', (224, 64, 32, 128))
    pal = palette_image()
    bgr = color_image((32, 64, 224), definitions=struct.pack('>10H', 3, 0, 0, 3, 1, 0, 2, 2, 0, 1))
    cases = [
        ('rgb-embedded', rgb, b'', None, b'', (224, 64, 32)),
        ('rgb-raw', jpx('RGB', (224, 64, 32), True), b'/ColorSpace /DeviceRGB', None, b'', (224, 64, 32)),
        ('decode-ignored', rgb, b'/Decode [1 0 1 0 1 0]', None, b'', (224, 64, 32)),
        ('gray-16bit', jpx('I;16', 32768), b'', None, b'', (128, 128, 128)),
        ('palette-embedded', pal, b'', None, b'', (224, 64, 32)),
        ('palette-pdf-override', pal, b'/ColorSpace [/Indexed /DeviceRGB 1 <00000020c040>]', None, b'', (32, 192, 64)),
        ('palette-missing-cmap', palette_image(False), b'', None, b'', (224, 64, 32)),
        ('palette-missing-cmap-override', palette_image(False), b'/ColorSpace [/Indexed /DeviceRGB 1 <00000020c040>]', None, b'', (32, 192, 64)),
        ('sycc-embedded', color_image((128, 64, 192), 18), b'', None, b'', (218, 104, 15)),
        ('channel-order', bgr, b'', None, b'', (224, 64, 32)),
        ('channel-order-pdf-override', bgr, b'/ColorSpace /DeviceRGB', None, b'', (32, 64, 224)),
        ('alpha-ignored', rgba, b'/SMaskInData 0', None, b'', (224, 64, 32)),
        ('alpha-straight', rgba, b'/SMaskInData 1', None, b'', (239, 159, 143)),
        ('alpha-premultiplied', jpx('RGBA', (112, 32, 16, 128)), b'/SMaskInData 2', None, b'', (239, 159, 143)),
        ('soft-mask', rgb, b'/SMask 6 0 R', jpx('L', 128), b'', (239, 159, 143)),
        ('color-key', rgb, b'/Mask [224 224 64 64 32 32]', None, b'', (255, 255, 255)),
        ('stencil', jpx('1', 0), b'/ImageMask true /Decode [0 1]', None, b'0 1 0 rg', (0, 255, 0)),
        ('stencil-inverted', jpx('1', 0), b'/ImageMask true /Decode [1 0]', None, b'0 1 0 rg', (255, 255, 255)),
    ]
    failed = 0
    with tempfile.TemporaryDirectory(prefix='nitro-jpx-pdf-') as work:
        for name, image, extra, mask, fill, expected in cases:
            path = Path(work) / (name + '.pdf')
            bitmap = path.with_suffix('.bmp')
            pdf(path, image, extra, mask, fill)
            subprocess.run([str(ROOT / 'examples/nitrodraw'), '-s', '64', str(path), str(bitmap)], check=True, stdout=subprocess.DEVNULL)
            with Image.open(bitmap) as rendered:
                got = rendered.convert('RGB').getpixel((32, 32))
            bad = max(abs(a - b) for a, b in zip(got, expected)) > 1
            failed += bad
            print(('FAIL' if bad else 'OK'), name, got, 'expected', expected)
    print(f'{len(cases)} PDF cases, {failed} failures')
    return bool(failed)


if __name__ == '__main__':
    raise SystemExit(main())
