---
name: xbox360-re
description: Use when finding addresses or fixing recompilation errors for the Xbox 360 XEX in LegacyRecomp
---

# Xbox 360 reverse engineering for LegacyRecomp

Every address in `config/minecraft-tu1/recomp.toml` must have its evidence in
`docs/reverse-engineering/addresses.md` (name, address, how found, confirmed by). Never paste game
bytes, disassembly listings or decompiled code into the repo or docs: addresses, names, sizes,
counts and hashes only. Generated C++ and image dumps stay in the data root / a scratch dir.

## Setup

1. Run the pipeline up to `patch` (or `analyse`) so `work/image/default_tu1.xex` exists:
   `LR_STOP_AFTER=patch LEGACYRECOMP_TEST_ROOT=<root> LEGACYRECOMP_ISO=<iso> build/dev/bin/lr_tests "real ISO: pipeline*"`.
2. XEX files are encrypted/compressed: scan the **loaded image**, never the `.xex`:
   `build/dev/bin/legacyrecomp inspect --dump-image <root>/work/image/default_tu1.xex <scratch>/image.bin > <scratch>/layout.txt`
   (prints base, size, entry, sections, and every `__imp__*` import thunk; byte 0 of the dump = base).
3. Quick disassembly without Ghidra (LLVM knows PowerPC, not VMX128):
   `llvm-objcopy -I binary -O elf32-powerpc --rename-section .data=.text,code,alloc,contents image.bin image.elf`
   then `llvm-objdump -d --adjust-vma=0x82000000 --triple=powerpc64-unknown-linux-gnu --start-address=<addr-0x82000000> --stop-address=... image.elf`.
4. Interactive work: Ghidra + the XEXLoaderWV extension opens `default_tu1.xex` directly (it is a plain,
   unencrypted XEX2 after the patch stage). Use it to confirm anything the scripts flag.

## Register save/restore helpers

`python3 -I tools/find_helpers.py <scratch>/image.bin --layout <scratch>/layout.txt` generates each
helper's full expected sequence and prints candidates with xref counts. Keys use the XenonRecomp README
names; prologues (big-endian):

| Key | First instruction(s) | Pattern |
|---|---|---|
| `savegprlr_14_address` | `std r14,-0x98(r1)` | `f9 c1 ff 68` |
| `restgprlr_14_address` | `ld r14,-0x98(r1)` | `e9 c1 ff 68` |
| `savefpr_14_address` | `stfd f14,-0x90(r12)` | `d9 cc ff 70` |
| `restfpr_14_address` | `lfd f14,-0x90(r12)` | `c9 cc ff 70` |
| `savevmx_14_address` | `li r11,-0x120; stvx v14,r11,r12` | `39 60 fe e0 7d cb 61 ce` |
| `restvmx_14_address` | `li r11,-0x120; lvx v14,r11,r12` | `39 60 fe e0 7d cb 60 ce` |
| `savevmx_64_address` | `li r11,-0x400; stvx128 v64,r11,r12` | `39 60 fc 00 10 0b 61 cb` |
| `restvmx_64_address` | `li r11,-0x400; lvx128 v64,r11,r12` | `39 60 fc 00 10 0b 60 cb` |

Accept only a unique **full** match. Sanity check: the eight helpers sit together in the CRT with the
same relative offsets as the README example (restgpr = savegpr + 0x50, savevmx_64 = savevmx_14 + 0x94,
restvmx_14 = savevmx_64 + 0x204, restvmx_64 = restvmx_14 + 0x94).

`setjmp`/`longjmp`: `find_helpers.py ... --xrefs <__imp__RtlUnwind address>` lists its callers;
`longjmp` is the one that restores f14-f31, GPRs, LR, r1 and CR from a `jmp_buf` in r3 and returns r4
(0 -> 1); `setjmp` follows it and stores the same offsets, returning 0. Never guess: if not confident,
leave the keys out and say so in `addresses.md`.

## Reading XenonRecomp output (`work/logs/recompile.log`)

| Message | Meaning | Fix |
|---|---|---|
| `ERROR: __xxx address is unspecified` | helper key missing | find it (above); the recompile stage fails on this |
| no output, huge RAM, OOM-killed | gap analysis ran into data (e.g. the 8 handler bytes before EH functions) | add `invalid_instructions` entries (`{ data = <word>, size = N }`) |
| `Switch case at X is trying to jump outside function: Y` | function boundary too short (leaf function, no `.pdata`) or bogus table | the analyse stage emits `work/analysis/functions.toml` automatically; otherwise add `functions = [{ address, size }]` in `recomp.toml`, or fix/drop the table |
| `Found a switch jump table at X with no switch table entry present` | `bctr` after a `nop` without a table | add a `[[switch]]` (base = any address from the bound check to the bctr, `r` = index register, `labels`) or extend `src/pipeline/switch_scan.cpp` |
| `Unrecognized instruction at X: <name>` | XenonRecomp lacks that instruction | not a config problem; runtime/XenonRecomp work |

Jump tables: XenonAnalyse misses this compiler's patterns; `src/pipeline/switch_scan.cpp` evaluates
the code between the `cmplwi`/`bgt` guard and the `bctr`. `work/logs/analyse.log` lists unresolved,
dropped and unguarded sites; resolve them by hand in Ghidra (count the cases, check the table bounds)
and record the result in `addresses.md`.

## Iterating fast

The dev build of XenonRecomp is a Debug build (minutes). For quick iterations build a Release copy
outside the repo (`cmake -S thirdparty/XenonRecomp -B <scratch>/xr-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`,
target `XenonRecomp`; a full run takes seconds) and point a scratch copy of `work/recomp.toml` at a
scratch output dir. Confirm the final config with the real pipeline: `ctest --preset dev -L integration`.
