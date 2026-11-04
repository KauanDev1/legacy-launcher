# SPDX-License-Identifier: MIT
# ctest "func_profile on a synthetic image": writes a tiny image (base 0x82000000) holding a byte-copy loop and a
# mapping file that ends the function at the next entry, runs tools/func_profile.py and checks its counts.
# Usage: python3 -I check_func_profile.py <func_profile.py> <scratch dir>
import os
import subprocess
import sys

BASE = 0x82000000

# A byte copy loop at 0x82000010 (lbz/stb with update, bdnz), a call to 0x82000100 and blr; the next function
# entry is 0x82000030. Words are big-endian PowerPC encodings.
WORDS = {
    0x82000010: 0x8D240001,  # lbzu r9,1(r4)
    0x82000014: 0x9D230001,  # stbu r9,1(r3)
    0x82000018: 0x4200FFF8,  # bdnz -8
    0x8200001C: 0x480000E5,  # bl 0x82000100
    0x82000020: 0x7C0018AC,  # dcbf 0,r3
    0x82000024: 0x4E800020,  # blr
    0x82000028: 0x60000000,  # nop
    0x8200002C: 0x60000000,  # nop
    0x82000030: 0x4E800020,  # blr (next function: not profiled)
}


def main():
    tool, scratch = sys.argv[1], sys.argv[2]
    os.makedirs(scratch, exist_ok=True)
    image = bytearray(0x200)
    for addr, w in WORDS.items():
        image[addr - BASE:addr - BASE + 4] = w.to_bytes(4, "big")
    img = os.path.join(scratch, "image.bin")
    with open(img, "wb") as f:
        f.write(image)
    mapping = os.path.join(scratch, "ppc_func_mapping.cpp")
    with open(mapping, "w") as f:
        f.write("{ 0x82000010, sub_82000010 },\n{ 0x82000030, sub_82000030 },\n{ 0x82000100, sub_82000100 },\n")
    out = subprocess.run([sys.executable, "-I", tool, img, "0x82000010", "--mapping", mapping],
                         capture_output=True, text=True, check=True).stdout
    lines = out.splitlines()
    expect = [
        "function 0x82000010..0x82000030 instructions=8",
        "groups: branch=3 cache=1 load=1 store=1",
    ]
    ok = lines[:2] == expect
    ok = ok and "load.b(u)=1" in lines[2] and "store.b(u)=1" in lines[2] and "cache.dcbf=1" in lines[2]
    ok = ok and "branch.cond=1" in lines[2] and "branch.call=1" in lines[2] and "branch.lr=1" in lines[2]
    ok = ok and lines[3] == "calls: 0x82000100x1"
    # An explicit end wins over the mapping.
    out2 = subprocess.run([sys.executable, "-I", tool, img, "0x82000010", "0x82000018"],
                          capture_output=True, text=True, check=True).stdout
    ok = ok and out2.splitlines()[0] == "function 0x82000010..0x82000018 instructions=2"
    if not ok:
        print("unexpected output:\n" + out + out2)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
