#!/usr/bin/env python3
"""Crops the display area out of a MIMIPS1_DUMP_FRAMES VRAM capture and writes a PNG.

usage: vram2png.py <vram_frame_N.ppm> <out.png> <x> <y> <w> <h> [scale=1] [bpp=15|24]
In 24-bit mode w is the display width in pixels; each pixel occupies 1.5 VRAM halfwords.
No third-party dependencies (PNG is written with zlib).
"""
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    x0, y0, w, h = (int(v) for v in sys.argv[3:7])
    scale = int(sys.argv[7]) if len(sys.argv) > 7 else 1
    bpp = int(sys.argv[8]) if len(sys.argv) > 8 else 15

    data = open(src, 'rb').read()
    # PPM header: P6\n1024 512\n255\n
    hdr_end = 0
    nl = 0
    while nl < 3:
        if data[hdr_end] == 10:
            nl += 1
        hdr_end += 1
    px = data[hdr_end:]
    stride = 1024 * 3

    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            if bpp == 24:
                # VRAM stores the 24-bit image as packed bytes across 16-bit words; the dump
                # expanded each word to RGB555-ish bytes, so rebuild the raw bytes first.
                bx = x0 * 2 + x * 3
                vals = []
                for k in range(3):
                    word = (bx + k) // 2
                    o = (y0 + y) * stride + word * 3
                    r, g, b = px[o] >> 3, px[o + 1] >> 3, px[o + 2] >> 3
                    v16 = r | (g << 5) | (b << 10)
                    vals.append((v16 >> 8) if (bx + k) & 1 else (v16 & 0xFF))
                rgb = bytes(vals)
            else:
                o = (y0 + y) * stride + (x0 + x) * 3
                rgb = px[o:o + 3]
            row += rgb * scale
        for _ in range(scale):
            rows.append(b'\x00' + bytes(row))
    raw = b''.join(rows)
    W, H = w * scale, h * scale

    def chunk(tag, body):
        c = struct.pack('>I', len(body)) + tag + body
        return c + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)


main()
