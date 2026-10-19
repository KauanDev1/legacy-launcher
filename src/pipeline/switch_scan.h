// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lr::pipeline {

// A jump table in XenonAnalyse's switch_tables.toml format. `base` is any instruction address in the
// function at or before the bctr (XenonRecomp attaches the table to the next bctr); `r` is the index
// register XenonRecomp switches on.
struct SwitchTable {
  uint32_t base = 0;
  uint32_t r = 0;
  uint32_t default_label = 0;  // 0 = conditional return (bgtlr)
  std::vector<uint32_t> labels;
};

struct SwitchScan {
  std::vector<SwitchTable> tables;
  std::vector<uint32_t> unresolved;  // bctr sites guarded by a bound check that could not be evaluated
  // Unguarded bctr sites fed by an indexed load from a lis-built address: either a switch whose
  // bound the compiler dropped (e.g. switch on an enum) or a call through a function-pointer array.
  // Needs a human look; XenonRecomp emits them as indirect calls.
  std::vector<uint32_t> unbounded;
};

// Finds `bctr` jump tables by evaluating the straight-line code between the bound check
// (`cmplwi crN, rI, max` + `bgt crN` / `bgtlr crN`) and the `bctr` for every index 0..max.
// Independent of instruction order and of `nop` padding, so it covers the patterns XenonAnalyse
// misses in this compiler's output. Evaluation starts at the earliest straight-line instruction
// before the cmplwi that leaves the index (and its `mr` copies) untouched and is not preceded by a
// branch target (another path could bring other register values), so table addresses and
// index scaling hoisted above the bound check are seen. Besides absolute word tables (`lwzx`) this
// resolves relative tables: `lhzx rT, rTable, rI` (rI = index << 1, by `rlwinm` or `add rI, rX, rX`)
// or `lbzx rT, rTable, rIdx`, optionally `rlwinm rT, rT, s, ...`, then `add rT, rT, rBase` (rBase the
// table itself or another `lis`/`addi` address) -> `mtctr rT` -> `bctr`. Every label must be 4-aligned
// inside its code range, at most 4096 cases. `image` is the loaded image (byte 0 = `image_base`, big-endian);
// `code` lists executable ranges as (start, size). Never reads outside `image`.
SwitchScan scan_switch_tables(std::span<const uint8_t> image, uint32_t image_base,
                              std::span<const std::pair<uint32_t, uint32_t>> code);

struct FunctionRange {
  uint32_t start = 0;
  uint32_t size = 0;
  bool has_handler = false;  // .pdata ExceptionFlag: 8 bytes of handler data precede the function
  bool operator==(const FunctionRange&) const = default;
};

// Reads the .pdata table (BeginAddress + packed PrologLength:8 / FunctionLength:22 / flags) at
// [pdata, pdata + size). Entries with BeginAddress 0 are skipped; result is sorted by start.
std::vector<FunctionRange> parse_pdata(std::span<const uint8_t> image, uint32_t image_base, uint32_t pdata,
                                       uint32_t size);

struct SwitchFit {
  std::vector<SwitchTable> tables;       // tables whose labels stay inside their function
  std::vector<FunctionRange> functions;  // `functions` overrides for leaf functions (no .pdata)
  std::vector<uint32_t> dropped;         // tables dropped: labels leave their function, or its range was rejected
  std::vector<FunctionRange> dropped_functions;  // derived ranges rejected by the safety checks
};

// XenonRecomp only knows function extents from .pdata and from its own analysis, which stops at
// a bctr. Leaf functions have no .pdata entry, so a switch inside one gets cut off. For every
// table outside .pdata coverage this derives the leaf function's extent: start = the closest
// `bl` target (or the first non-padding word after the previous .pdata function), end = the
// first `bl` target after the last label, else the next .pdata function (minus its handler
// words and padding). Tables inside a .pdata function must keep all labels inside it.
// A derived range is rejected (and its tables dropped) if it overlaps another derived range or one
// of `manual` (hand-written `functions` in recomp.toml), or if any call (`bl`/`bcl`) targets its
// interior, or any branch from outside it lands in its interior.
SwitchFit fit_switch_functions(std::span<const uint8_t> image, uint32_t image_base,
                               std::span<const std::pair<uint32_t, uint32_t>> code,
                               std::span<const FunctionRange> pdata, std::vector<SwitchTable> tables,
                               std::span<const FunctionRange> manual = {});

// Serialises tables in XenonAnalyse's format ([[switch]] base/r/default/labels).
std::string format_switch_tables(std::span<const SwitchTable> tables);

}  // namespace lr::pipeline
