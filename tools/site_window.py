#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode a few instructions around a code address (jump-table / function-boundary diagnosis).

Usage: python3 -I tools/site_window.py <image.bin> <address> [before] [after]

<image.bin> is the loaded image from `legacyrecomp inspect --dump-image` (byte 0 = base 0x82000000).
Prints at most 64 decoded instructions (default 24 before, 4 after) as mnemonics with operands; never raw
bytes. Only the forms the analyse scanner reasons about are named; anything else prints as its opcode
numbers ("op31/xo") so the window shape stays visible without reproducing the code.
"""
import sys

BASE = 0x82000000
MAX_WINDOW = 64


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def decode(w, at):
    op = w >> 26
    rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    imm = w & 0xFFFF
    xo = (w >> 1) & 0x3FF
    if w == 0:
        return "(zero word)"
    if w == 0x60000000:
        return "nop"
    if op == 14:
        return f"li r{rd},{s16(imm)}" if ra == 0 else f"addi r{rd},r{ra},{s16(imm)}"
    if op == 15:
        return f"lis r{rd},0x{imm:X}" if ra == 0 else f"addis r{rd},r{ra},0x{imm:X}"
    if op == 10:
        return f"cmplwi cr{rd >> 2},r{ra},{imm}" if not (rd & 1) else f"cmpldi cr{rd >> 2},r{ra},{imm}"
    if op == 11:
        return f"cmpwi cr{rd >> 2},r{ra},{s16(imm)}" if not (rd & 1) else f"cmpdi cr{rd >> 2},r{ra},{s16(imm)}"
    if op == 21:
        return f"rlwinm r{ra},r{rd},{rb},{(w >> 6) & 31},{(w >> 1) & 31}{'.' if w & 1 else ''}"
    if op in (24, 25, 26, 27, 28, 29):
        names = {24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis."}
        return f"{names[op]} r{ra},r{rd},0x{imm:X}"
    if op in (32, 34, 40, 42, 36, 38, 44, 37):
        names = {32: "lwz", 34: "lbz", 40: "lhz", 42: "lha", 36: "stw", 38: "stb", 44: "sth", 37: "stwu"}
        return f"{names[op]} r{rd},{s16(imm)}(r{ra})"
    if op == 18:
        d = w & 0x03FFFFFC
        if d & 0x02000000:
            d -= 0x04000000
        t = (at + d) & 0xFFFFFFFF if not (w & 2) else d & 0xFFFFFFFF
        return f"b{'l' if w & 1 else ''}{'a' if w & 2 else ''} 0x{t:08X}"
    if op == 16:
        t = (at + s16(w & 0xFFFC)) & 0xFFFFFFFF
        return f"bc{'l' if w & 1 else ''} bo={rd},bi={ra} (cr{ra >> 2}.{['lt', 'gt', 'eq', 'so'][ra & 3]}) 0x{t:08X}"
    if op == 19:
        if xo == 16:
            return "blr" if (rd & 0x14) == 0x14 else f"bclr bo={rd},bi={ra}"
        if xo == 528:
            return "bctr" if (rd & 0x14) == 0x14 else f"bcctr bo={rd},bi={ra}"
        return f"op19/{xo}"
    if op == 31:
        names = {23: "lwzx", 87: "lbzx", 279: "lhzx", 266: "add", 40: "subf", 444: "or", 28: "and",
                 24: "slw", 536: "srw", 824: "srawi", 0: "cmpw", 32: "cmplw", 151: "stwx"}
        if xo == 467 and ((w >> 11) & 0x3FF) == (9 << 5):
            return f"mtctr r{rd}"
        if xo == 339:
            return f"mfspr r{rd}"
        if xo in (0, 32):
            return f"{names[xo]} cr{rd >> 2},r{ra},r{rb}"
        if xo == 444 and rd == rb:
            return f"mr r{ra},r{rd}"
        if xo in (444, 28, 24, 536):
            return f"{names[xo]} r{ra},r{rd},r{rb}"
        if xo == 824:
            return f"srawi r{ra},r{rd},{rb}"
        if xo in names:
            return f"{names[xo]} r{rd},r{ra},r{rb}"
        x9 = xo & 0x1FF
        if x9 in names:
            return f"{names[x9]} r{rd},r{ra},r{rb}"
        return f"op31/{xo}"
    return f"op{op}"


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    addr = int(argv[2], 16)
    before = int(argv[3]) if len(argv) > 3 else 24
    after = int(argv[4]) if len(argv) > 4 else 4
    if before + after + 1 > MAX_WINDOW:
        print(f"window larger than {MAX_WINDOW} instructions", file=sys.stderr)
        return 2
    with open(argv[1], "rb") as f:
        image = f.read()
    for a in range(addr - 4 * before, addr + 4 * after + 4, 4):
        off = a - BASE
        if off < 0 or off + 4 > len(image):
            continue
        w = int.from_bytes(image[off:off + 4], "big")
        print(f"{'>' if a == addr else ' '}0x{a:08X}  {decode(w, a)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
