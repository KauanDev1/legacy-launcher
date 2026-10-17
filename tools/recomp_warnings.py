#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarise XenonRecomp warnings from work/logs/recompile.log.

Usage: python3 -I tools/recomp_warnings.py <recompile.log>

Prints a table of unrecognized instructions per mnemonic (count descending, then name) and every
float16_4 pack site. The float16_4 warning carries only an address (no operands), so only the
address is listed. Reads the log only, never the image.
"""
import re
import sys
from collections import Counter

UNRECOGNIZED = re.compile(r"^Unrecognized instruction at 0x([0-9A-Fa-f]+): (\S+)\s*$")
FLOAT16 = re.compile(r"^Unexpected float16_4 pack instruction at ([0-9A-Fa-f]+)\s*$")


def main(argv):
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    counts = Counter()
    packs = []
    try:
        with open(argv[1], encoding="utf-8", errors="replace") as f:
            for line in f:
                m = UNRECOGNIZED.match(line)
                if m:
                    counts[m.group(2)] += 1
                    continue
                m = FLOAT16.match(line)
                if m:
                    packs.append(m.group(1).upper())
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(f"unrecognized instructions: {sum(counts.values())}")
    for name, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"  {name:<12} {n:>8}")
    print(f"float16_4 pack sites: {len(packs)}")
    for addr in packs:
        print(f"  0x{addr}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
