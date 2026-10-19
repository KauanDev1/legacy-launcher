// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "pipeline/switch_scan.h"

namespace lr::pipeline {

// An executable range of the image as (start, size).
using CodeRange = std::pair<uint32_t, uint32_t>;

// XenonRecomp only recompiles the functions it knows (.pdata + the config's `functions`). Functions
// without a .pdata entry that are only reached through a pointer (vtables, callback tables, a qsort
// comparator) are missed. This finds them in the gaps between `known` functions (.pdata, derived and
// hand-written ranges; a function with `has_handler` also owns the 8 bytes before its start).
//
// Candidates: (a) 4-aligned big-endian words outside `code` whose value is an address in `code`;
// (b) `bl` targets; (c) `lis rX, hi` + `addi rY, rX, lo` (within 4 straight-line instructions, rX not
// rewritten in between) whose value is an address in `code`. A candidate must lie in a gap (not inside
// a known function or its handler words), be a non-zero word, and follow a word that cannot fall
// through into it: `blr`, `bctr`, an unconditional `b`, zero padding, the end of a code range, or (only
// for `bl` targets: the call before may be noreturn) a `bl`.
//
// End: the first unconditional branch (`blr`, `bctr`, `b`) with no forward branch of the function
// pending past it. A branch to a known or candidate start is a (conditional) tail call; any other
// branch must stay inside the gap. The candidate is rejected if its code runs into a known or another
// candidate's start, padding or the end of the gap before it ends (the other candidate is then rejected
// too), if it ends in a `bctr` fed by an indexed load (an unresolved switch), or if any branch or call
// from outside lands in its interior (the `fit_switch_functions` rule). Result is sorted by start.
// Never reads outside `image` (byte 0 = `base`, big-endian).
std::vector<FunctionRange> discover_pointer_functions(std::span<const uint8_t> image, uint32_t base,
                                                      const std::vector<CodeRange>& code,
                                                      const std::vector<FunctionRange>& known);

}  // namespace lr::pipeline
