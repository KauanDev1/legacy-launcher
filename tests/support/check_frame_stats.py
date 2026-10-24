# SPDX-License-Identifier: MIT
# ctest "frame_stats on a synthetic PNG": writes a 64x48 RGBA checkerboard (8x8 black/white cells, every PNG filter
# type used across the rows), runs tools/frame_stats.py on it and checks the one printed line against numbers
# computed here from the pattern. Usage: python3 -I check_frame_stats.py <frame_stats.py> <scratch dir>
import os
import struct
import subprocess
import sys
import zlib

W, H, CELL = 64, 48, 8


def pixel(x, y):
    v = 255 if ((x // CELL) + (y // CELL)) % 2 else 0
    return bytes((v, v, v, 255))


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def encode_row(ftype, line, prev, bpp=4):
    out = bytearray([ftype])
    for i, v in enumerate(line):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        pred = (0, a, b, (a + b) >> 1, paeth(a, b, c))[ftype]
        out.append((v - pred) & 0xFF)
    return bytes(out)


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)


def write_png(path):
    raw = bytearray()
    prev = bytes(W * 4)
    for y in range(H):
        line = b"".join(pixel(x, y) for x in range(W))
        raw += encode_row(y % 5, line, prev)
        prev = line
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    script, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    png = os.path.join(work, "checkerboard.png")
    write_png(png)
    out = subprocess.run([sys.executable, "-I", script, png], capture_output=True, text=True, check=False)
    if out.returncode != 0:
        print("frame_stats failed (%d): %s" % (out.returncode, out.stderr))
        return 1
    # Edges: the last column of each cell except the image's last (7 columns x 48 rows), the last row of each cell
    # except the image's last (5 rows x 64 columns), minus the 7x5 pixels counted twice.
    edges = 7 * H + 5 * W - 7 * 5
    expected = "colors=2 mean_luminance=0.5000 edge_fraction=%.4f\n" % (edges / (W * H))
    if out.stdout != expected:
        print("got      %r\nexpected %r" % (out.stdout, expected))
        return 1
    print("ok: " + expected.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
