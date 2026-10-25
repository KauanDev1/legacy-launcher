#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Profile a guest function by instruction class, without printing its code.

Usage: python3 -I tools/func_profile.py <image.bin> <start> [end] [--mapping <ppc_func_mapping.cpp>]

<image.bin> is the loaded image from `legacyrecomp inspect --dump-image` (byte 0 = base 0x82000000). The function
is [start, end); without `end`, it ends at the next function entry listed in the mapping file (the generated
ppc_func_mapping.cpp: only its addresses are read), or after 4096 instructions. Prints one summary line with the
size, then counts per instruction class (loads, stores and their widths, cache ops, vector, float, branches) and the
direct call targets (`bl`) with their counts. Never prints instruction words or operands, so it is safe to run on
the game image; it is meant to tell a copy loop (loads and stores of the same width) from a fill (stores only), a
rasterizer (float/vector math), and so on.
"""
import re
import sys

BASE = 0x82000000
MAX_INSNS = 4096

# Primary opcodes -> class name.
PRIMARY = {
    2: "tdi", 3: "twi", 7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.",
    14: "addi", 15: "addis", 16: "bc", 17: "sc", 18: "b", 20: "rlwimi", 21: "rlwinm", 23: "rlwnm",
    24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.", 30: "rld*",
    32: "load.w", 33: "load.w(u)", 34: "load.b", 35: "load.b(u)", 36: "store.w", 37: "store.w(u)",
    38: "store.b", 39: "store.b(u)", 40: "load.h", 41: "load.h(u)", 42: "load.ha", 43: "load.ha(u)",
    44: "store.h", 45: "store.h(u)", 46: "lmw", 47: "stmw", 48: "load.fs", 49: "load.fs(u)", 50: "load.fd",
    51: "load.fd(u)", 52: "store.fs", 53: "store.fs(u)", 54: "store.fd", 55: "store.fd(u)", 58: "load.d",
    62: "store.d", 59: "float.single", 63: "float.double", 4: "vmx", 5: "vmx128", 6: "vmx128",
}

# Opcode 31 extended opcodes (10-bit field) -> class name.
X31 = {
    23: "load.w(x)", 55: "load.w(ux)", 87: "load.b(x)", 119: "load.b(ux)", 279: "load.h(x)", 343: "load.ha(x)",
    21: "load.d(x)", 53: "load.d(ux)", 151: "store.w(x)", 183: "store.w(ux)", 215: "store.b(x)",
    247: "store.b(ux)", 407: "store.h(x)", 439: "store.h(ux)", 149: "store.d(x)", 181: "store.d(ux)",
    535: "load.fs(x)", 599: "load.fd(x)", 663: "store.fs(x)", 727: "store.fd(x)", 534: "load.w(brx)",
    662: "store.w(brx)", 790: "load.h(brx)", 918: "store.h(brx)", 532: "load.d(brx)", 660: "store.d(brx)",
    103: "load.v(lvx)", 359: "load.v(lvxl)", 231: "store.v(stvx)", 487: "store.v(stvxl)",
    519: "load.v(lvlx)", 551: "load.v(lvrx)", 647: "store.v(stvlx)", 679: "store.v(stvrx)",
    7: "load.v(lvebx)", 39: "load.v(lvehx)", 71: "load.v(lvewx)", 135: "store.v(stvebx)",
    167: "store.v(stvehx)", 199: "store.v(stvewx)", 6: "lvsl", 38: "lvsr",
    1014: "cache.dcbz", 278: "cache.dcbt", 246: "cache.dcbtst", 86: "cache.dcbf", 54: "cache.dcbst",
    982: "cache.icbi", 598: "sync", 854: "eieio", 20: "lwarx", 150: "stwcx.", 84: "ldarx", 214: "stdcx.",
    339: "mfspr", 467: "mtspr", 19: "mfcr", 144: "mtcrf", 0: "cmp", 32: "cmpl", 266: "add", 40: "subf",
    235: "mullw", 491: "divw", 459: "divwu", 28: "and", 444: "or", 316: "xor", 124: "nor", 60: "andc",
    24: "slw", 536: "srw", 792: "sraw", 824: "srawi", 26: "cntlzw", 954: "extsb", 922: "extsh", 986: "extsw",
    104: "neg", 10: "addc", 138: "adde", 8: "subfc", 136: "subfe", 202: "addze", 200: "subfze", 4: "tw",
    27: "sld", 539: "srd", 794: "srad", 413: "sradi", 233: "mulld", 73: "mulhd", 75: "mulhw", 11: "mulhwu",
}


def classify(w):
    op = w >> 26
    if w == 0:
        return "zero-word"
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo not in X31 and ((w >> 1) & 0x1FF) in (266, 40, 235, 491, 459, 104, 10, 138, 8, 136, 202, 200, 233, 73):
            xo = (w >> 1) & 0x1FF  # XO-form with OE
        if xo == 413 or ((w >> 2) & 0x1FF) == 413:
            return "sradi"
        return X31.get(xo, f"op31/{xo}")
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo == 16:
            return "branch.lr" + (" (link)" if w & 1 else "")
        if xo == 528:
            return "branch.ctr" + (" (link)" if w & 1 else "")
        return "cr-op"
    if op == 18:
        return "branch.call" if w & 1 else "branch"
    if op == 16:
        return "branch.cond"
    return PRIMARY.get(op, f"op{op}")


def next_entry(mapping, start):
    best = None
    with open(mapping, encoding="utf-8", errors="replace") as f:
        for m in re.finditer(r"0x([0-9A-Fa-f]{8})", f.read()):
            a = int(m.group(1), 16)
            if a > start and (best is None or a < best):
                best = a
    return best


def main(argv):
    args = [a for a in argv[1:]]
    mapping = None
    if "--mapping" in args:
        i = args.index("--mapping")
        mapping = args[i + 1]
        del args[i:i + 2]
    if len(args) < 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    image = open(args[0], "rb").read()
    start = int(args[1], 0)
    end = int(args[2], 0) if len(args) > 2 else (next_entry(mapping, start) if mapping else None)
    if end is None or end - start > 4 * MAX_INSNS:
        end = start + 4 * MAX_INSNS
    if start < BASE or end <= start or end - BASE > len(image):
        print("error: range outside the image", file=sys.stderr)
        return 2
    counts = {}
    calls = {}
    for a in range(start, end, 4):
        o = a - BASE
        w = int.from_bytes(image[o:o + 4], "big")
        c = classify(w)
        counts[c] = counts.get(c, 0) + 1
        if c == "branch.call" and not (w & 2):
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = (a + li) & 0xFFFFFFFF
            calls[t] = calls.get(t, 0) + 1
    print(f"function 0x{start:08X}..0x{end:08X} instructions={(end - start) // 4}")
    groups = {}
    for c, n in counts.items():
        g = c.split(".")[0] if c.split(".")[0] in ("load", "store", "cache", "branch", "float") else None
        if g:
            groups[g] = groups.get(g, 0) + n
    print("groups: " + " ".join(f"{g}={n}" for g, n in sorted(groups.items())))
    print("classes: " + " ".join(f"{c}={n}" for c, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))))
    print("calls: " + (" ".join(f"0x{t:08X}x{n}" for t, n in sorted(calls.items())) or "none"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
