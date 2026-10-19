#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Classify vpkd3d128 sites by (type, pack, shift).

Usage: python3 -I tools/vpkd3d_sites.py <image.bin> <address> [<address> ...]
       python3 -I tools/vpkd3d_sites.py <image.bin> --log <recompile.log>

<image.bin> is the loaded image from `legacyrecomp inspect --dump-image` (byte 0 = base 0x82000000). Addresses come
from the command line or from the "Unexpected float16_4 pack instruction at <addr>" lines of a recompile log.
Prints only the decoded VX128_4 fields of each word (never the bytes or neighbouring code), then a count per class.
Field layout as in XenonRecomp's disassembler (VD3D0/VD3D1/VD3D2) and Xenia's VX128_4: type = IMM >> 2,
pack = IMM & 3, shift = z.
"""
import re
import sys
from collections import Counter

BASE = 0x82000000
FLOAT16 = re.compile(r"^Unexpected float16_4 pack instruction at ([0-9A-Fa-f]+)\s*$")
TYPES = {0: "D3DCOLOR", 1: "NORMSHORT2", 2: "NORMPACKED32", 3: "FLOAT16_2", 4: "NORMSHORT4", 5: "FLOAT16_4",
         6: "NORMPACKED64"}


def decode(word):
    if (word >> 26) != 6 or (word & 0x730) != 0x610:
        return None
    vd = ((word >> 21) & 0x1F) | (((word >> 2) & 0x3) << 5)
    vb = ((word >> 11) & 0x1F) | ((word & 0x3) << 5)
    imm = (word >> 16) & 0x1F
    return {"vd": vd, "vb": vb, "type": imm >> 2, "pack": imm & 3, "shift": (word >> 6) & 3}


def main(argv):
    if len(argv) < 3 or argv[1] in ("-h", "--help"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    addrs = []
    if argv[2] == "--log":
        if len(argv) != 4:
            print(__doc__.strip(), file=sys.stderr)
            return 2
        with open(argv[3], encoding="utf-8", errors="replace") as f:
            for line in f:
                m = FLOAT16.match(line)
                if m:
                    addrs.append(int(m.group(1), 16))
    else:
        addrs = [int(a, 16) for a in argv[2:]]
    with open(argv[1], "rb") as f:
        image = f.read()
    classes = Counter()
    for addr in addrs:
        off = addr - BASE
        if off < 0 or off + 4 > len(image) or off % 4:
            print(f"0x{addr:08X}  outside the image")
            continue
        fields = decode(int.from_bytes(image[off:off + 4], "big"))
        if fields is None:
            print(f"0x{addr:08X}  not a vpkd3d128")
            continue
        name = TYPES.get(fields["type"], f"type{fields['type']}")
        print(f"0x{addr:08X}  {name:<12} pack={fields['pack']} shift={fields['shift']} "
              f"v{fields['vd']} <- v{fields['vb']}")
        classes[(name, fields["pack"], fields["shift"])] += 1
    print("classes:")
    for (name, pack, shift), n in sorted(classes.items()):
        print(f"  {name:<12} pack={pack} shift={shift}  {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
