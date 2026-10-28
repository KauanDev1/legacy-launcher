# SPDX-License-Identifier: MIT
"""Objective statistics of a frame, so a screenshot can be judged without viewing it.

Usage (inside the dev container): python3 -I tools/frame_stats.py <png>

Prints exactly one line and nothing else:
    colors=N mean_luminance=F edge_fraction=F
- colors: distinct RGB triples (alpha ignored);
- mean_luminance: mean of (0.2126 R + 0.7152 G + 0.0722 B) / 255 over all pixels, in [0, 1];
- edge_fraction: fraction of pixels whose luminance differs by more than EDGE_THRESHOLD from the pixel to the right
  or the pixel below (pixels on the last column/row compare only with the neighbour they have).

Standard library only (zlib decode). Supports 8-bit, non-interlaced PNGs of colour type 0, 2, 4 or 6.
"""
import struct
import sys
import zlib

EDGE_THRESHOLD = 0.1
_CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}


def _fail(msg):
    sys.stderr.write("frame_stats: " + msg + "\n")
    sys.exit(2)


def read_png(path):
    """Returns (width, height, rows) with rows a list of bytes objects holding RGB triples."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        _fail("not a PNG: " + path)
    pos = 8
    ihdr = None
    idat = []
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if ihdr is None:
        _fail("no IHDR")
    width, height, depth, ctype, _comp, _filt, interlace = ihdr
    if depth != 8 or ctype not in _CHANNELS or interlace != 0:
        _fail("unsupported PNG (depth %d, colour type %d, interlace %d)" % (depth, ctype, interlace))
    bpp = _CHANNELS[ctype]
    raw = zlib.decompress(b"".join(idat))
    stride = width * bpp
    if len(raw) < height * (stride + 1):
        _fail("truncated image data")
    prev = bytearray(stride)
    rows = []
    for y in range(height):
        base = y * (stride + 1)
        ftype = raw[base]
        line = bytearray(raw[base + 1:base + 1 + stride])
        if ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        elif ftype != 0:
            _fail("bad filter type %d" % ftype)
        prev = line
        if ctype in (2, 6):
            rgb = bytearray(width * 3)
            for x in range(width):
                rgb[x * 3:x * 3 + 3] = line[x * bpp:x * bpp + 3]
        else:
            rgb = bytearray(width * 3)
            for x in range(width):
                g = line[x * bpp]
                rgb[x * 3:x * 3 + 3] = bytes((g, g, g))
        rows.append(bytes(rgb))
    return width, height, rows


def frame_stats(width, height, rows):
    colors = set()
    lum = []
    total = 0.0
    for row in rows:
        lrow = []
        for x in range(width):
            r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            colors.add((r << 16) | (g << 8) | b)
            v = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0
            lrow.append(v)
            total += v
        lum.append(lrow)
    edges = 0
    for y in range(height):
        cur = lum[y]
        below = lum[y + 1] if y + 1 < height else None
        for x in range(width):
            v = cur[x]
            if (x + 1 < width and abs(cur[x + 1] - v) > EDGE_THRESHOLD) or (
                    below is not None and abs(below[x] - v) > EDGE_THRESHOLD):
                edges += 1
    pixels = width * height
    if pixels == 0:
        return 0, 0.0, 0.0
    return len(colors), total / pixels, edges / pixels


def main(argv):
    if len(argv) != 2:
        _fail("usage: frame_stats.py <png>")
    w, h, rows = read_png(argv[1])
    colors, mean, edge = frame_stats(w, h, rows)
    print("colors=%d mean_luminance=%.4f edge_fraction=%.4f" % (colors, mean, edge))


if __name__ == "__main__":
    main(sys.argv)
