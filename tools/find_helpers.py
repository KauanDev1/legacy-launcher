#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Finds the XenonRecomp register save/restore helpers in a loaded Xbox 360 image.

Input is the image dumped by `legacyrecomp inspect --dump-image <xex> <out.bin>` (byte 0 = base
address) plus its layout (the stdout of that command, or --base/--code/--pdata by hand).
Run with `python3 -I tools/find_helpers.py ...`. Prints addresses only, never image bytes.

For every helper the full expected instruction sequence is generated (not just the prologue),
so a "full" match means every instruction from the first register to the final blr matched.
`xrefs` counts `bl`/`b` instructions elsewhere in code that land inside the helper (callers enter
at the slot of the first register they need, so most target the middle; restores are usually
reached by a tail `b`).

    legacyrecomp inspect --dump-image work/image/default_tu1.xex /tmp/img.bin > /tmp/layout.txt
    python3 -I tools/find_helpers.py /tmp/img.bin --layout /tmp/layout.txt
    python3 -I tools/find_helpers.py /tmp/img.bin --layout /tmp/layout.txt --xrefs 0x82B63FEC
"""

import argparse
import re
import struct
import sys

BLR = 0x4E800020


def li(rd, imm):  # addi rd, 0, imm
    return (14 << 26) | (rd << 21) | (imm & 0xFFFF)


def dform(op, rs, ra, d):
    return (op << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)


def xform(rs, ra, rb, xo):
    return (31 << 26) | (rs << 21) | (ra << 16) | (rb << 11) | (xo << 1)


def vmx128_mem(vd, base):  # stvx128/lvx128 vd, r11, r12 (VD128 split: low 5 bits + 2 high bits)
    return base | ((vd & 31) << 21) | (((vd >> 5) & 3) << 2)


def seq_savegprlr():
    return [dform(62, r, 1, -0x98 + 8 * (r - 14)) for r in range(14, 32)] + [0x9181FFF8, BLR]


def seq_restgprlr():
    return [dform(58, r, 1, -0x98 + 8 * (r - 14)) for r in range(14, 32)] + [0x8181FFF8, 0x7D8803A6, BLR]


def seq_savefpr():
    return [dform(54, f, 12, -0x90 + 8 * (f - 14)) for f in range(14, 32)] + [BLR]


def seq_restfpr():
    return [dform(50, f, 12, -0x90 + 8 * (f - 14)) for f in range(14, 32)] + [BLR]


def seq_vmx14(xo):  # stvx = 231, lvx = 103
    out = []
    for v in range(14, 32):
        out += [li(11, -0x120 + 16 * (v - 14)), xform(v, 11, 12, xo)]
    return out + [BLR]


def seq_vmx64(base):  # stvx128 = 0x100B61C3, lvx128 = 0x100B60C3
    out = []
    for v in range(64, 128):
        out += [li(11, -0x400 + 16 * (v - 64)), vmx128_mem(v, base)]
    return out + [BLR]


HELPERS = [
    ("savegprlr_14_address", seq_savegprlr()),
    ("restgprlr_14_address", seq_restgprlr()),
    ("savefpr_14_address", seq_savefpr()),
    ("restfpr_14_address", seq_restfpr()),
    ("savevmx_14_address", seq_vmx14(231)),
    ("restvmx_14_address", seq_vmx14(103)),
    ("savevmx_64_address", seq_vmx64(0x100B61C3)),
    ("restvmx_64_address", seq_vmx64(0x100B60C3)),
]


def parse_int(s):
    return int(s, 0)


def parse_range(s):
    start, size = s.split(":")
    return parse_int(start), parse_int(size)


def read_layout(path):
    base, code, pdata = None, [], None
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"base=(0x[0-9A-Fa-f]+)", line)
            if m:
                base = int(m.group(1), 16)
            m = re.match(r"section name=(\S+)\s+base=(0x[0-9A-Fa-f]+) size=(0x[0-9A-Fa-f]+) flags=(\w+)", line)
            if m:
                rng = (int(m.group(2), 16), int(m.group(3), 16))
                if m.group(4) == "code":
                    code.append(rng)
                if m.group(1) == ".pdata":
                    pdata = rng
    return base, code, pdata


class Image:
    def __init__(self, data, base, code):
        self.data, self.base = data, base
        self.code = code or [(base, len(data))]

    def word(self, addr):
        off = addr - self.base
        if off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from(">I", self.data, off)[0]

    def code_words(self):
        for start, size in self.code:
            off = start - self.base
            end = min(off + size, len(self.data)) & ~3
            for o in range(off & ~3, end, 4):
                yield self.base + o, struct.unpack_from(">I", self.data, o)[0]


def matched_len(img, addr, seq):
    n = 0
    for i, w in enumerate(seq):
        if img.word(addr + 4 * i) != w:
            break
        n += 1
    return n


def branch_target(addr, w):
    """Target of an I-form branch (b/bl, not absolute) or None."""
    if (w >> 26) != 18 or (w & 2):
        return None
    li_ = w & 0x03FFFFFC
    if li_ & 0x02000000:
        li_ -= 0x04000000
    return (addr + li_) & 0xFFFFFFFF, bool(w & 1)


def collect_branches(img):
    out = []
    for addr, w in img.code_words():
        t = branch_target(addr, w)
        if t:
            out.append((addr, t[0], t[1]))
    return out


def pdata_functions(img, pdata):
    """(start, size) of every .pdata entry: BeginAddress + packed length in instructions (bits 8..29)."""
    if not pdata:
        return []
    start, size = pdata
    funcs = []
    for a in range(start, start + size - 7, 8):
        begin, info = img.word(a), img.word(a + 4)
        if not begin:
            continue
        funcs.append((begin, ((info >> 8) & 0x3FFFFF) * 4))
    return sorted(funcs)


def containing(funcs, addr):
    lo, hi = 0, len(funcs) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if funcs[mid][0] <= addr:
            best = funcs[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if best and addr < best[0] + best[1]:
        return best
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="image dumped by `legacyrecomp inspect --dump-image`")
    ap.add_argument("--layout", help="stdout of `legacyrecomp inspect --dump-image` (base + sections)")
    ap.add_argument("--base", type=parse_int, help="image base address (default: from --layout)")
    ap.add_argument("--code", type=parse_range, action="append", default=[],
                    help="executable range START:SIZE (repeatable; default: code sections from --layout)")
    ap.add_argument("--pdata", type=parse_range, help=".pdata range START:SIZE (default: from --layout)")
    ap.add_argument("--xrefs", type=parse_int, action="append", default=[],
                    help="also list b/bl instructions targeting this address (repeatable)")
    args = ap.parse_args()

    base, code, pdata = None, [], None
    if args.layout:
        base, code, pdata = read_layout(args.layout)
    if args.base is not None:
        base = args.base
    if args.code:
        code = args.code
    if args.pdata:
        pdata = args.pdata
    if base is None:
        sys.exit("error: need --base or --layout")

    with open(args.dump, "rb") as f:
        data = f.read()
    img = Image(data, base, code)
    branches = collect_branches(img)
    funcs = pdata_functions(img, pdata)

    for key, seq in HELPERS:
        cands = []
        for addr, w in img.code_words():
            if w == seq[0]:
                n = matched_len(img, addr, seq)
                if n >= 2:
                    cands.append((addr, n))
        if not cands:
            print(f"{key}: no candidate")
            continue
        for addr, n in cands:
            end = addr + 4 * len(seq)
            inside = [link for a, t, link in branches if addr <= t < end and not addr <= a < end]
            bl, b = sum(inside), len(inside) - sum(inside)
            kind = "full" if n == len(seq) else f"partial {n}/{len(seq)}"
            print(f"{key} = 0x{addr:08X}  # {kind} match, {len(seq) * 4:#x} bytes, xrefs: {bl} bl + {b} b")

    for target in args.xrefs:
        hits = [(a, link) for a, t, link in branches if t == target]
        print(f"xrefs to 0x{target:08X}: {len(hits)}")
        for a, link in hits:
            fn = containing(funcs, a)
            where = f" in function 0x{fn[0]:08X} (size {fn[1]:#x})" if fn else ""
            print(f"  {'bl' if link else 'b '} at 0x{a:08X}{where}")


if __name__ == "__main__":
    main()
