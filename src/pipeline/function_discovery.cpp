// SPDX-License-Identifier: MIT
#include "pipeline/function_discovery.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <optional>
#include <set>

namespace lr::pipeline {
namespace {

constexpr uint32_t kBctr = 0x4E800420;
constexpr uint32_t kMaxFunctionSize = 0x10000;  // longer "functions" in a gap are not pointer-only leaves
constexpr int kLisWindow = 4;                   // instructions after a lis searched for its addi
constexpr int kSwitchWindow = 16;               // instructions before a bctr searched for an indexed load

uint32_t op(uint32_t i) { return i >> 26; }
uint32_t rd(uint32_t i) { return (i >> 21) & 31; }
uint32_t ra(uint32_t i) { return (i >> 16) & 31; }
uint32_t xo(uint32_t i) { return (i >> 1) & 0x3FF; }

class Reader {
 public:
  Reader(std::span<const uint8_t> image, uint32_t base) : image_(image), base_(base) {}
  std::optional<uint32_t> word(uint32_t addr) const {
    if (addr < base_) return std::nullopt;
    const uint64_t off = uint64_t{addr} - base_;
    if (off + 4 > image_.size()) return std::nullopt;
    const auto o = static_cast<size_t>(off);
    return (uint32_t{image_[o]} << 24) | (uint32_t{image_[o + 1]} << 16) | (uint32_t{image_[o + 2]} << 8) |
           uint32_t{image_[o + 3]};
  }

 private:
  std::span<const uint8_t> image_;
  uint32_t base_;
};

// Relative b/bc (with or without link): its target. Absolute forms and bclr/bcctr are not handled here.
std::optional<uint32_t> relative_target(uint32_t i, uint32_t at) {
  if ((op(i) != 18 && op(i) != 16) || (i & 2)) return std::nullopt;
  int32_t disp;
  if (op(i) == 18) {
    disp = static_cast<int32_t>(i & 0x03FFFFFC);
    if (disp & 0x02000000) disp -= 0x04000000;
  } else {
    disp = static_cast<int16_t>(i & 0xFFFC);
  }
  return at + static_cast<uint32_t>(disp);
}

// BO with both "ignore CTR" and "ignore condition" bits: always taken.
bool always(uint32_t i) { return (rd(i) & 0x14) == 0x14; }

// Unconditional, non-linking control transfer: execution never continues at the next word.
bool is_unconditional(uint32_t i) {
  if (i & 1) return false;  // link: a call returns here
  if (op(i) == 18) return true;
  if (op(i) == 16) return always(i);
  if (op(i) == 19 && (xo(i) == 16 || xo(i) == 528)) return always(i);  // blr / bctr
  return false;
}

bool is_bl(uint32_t i) { return op(i) == 18 && (i & 3) == 1; }

bool in_code(const std::vector<CodeRange>& code, uint32_t a) {
  for (auto [s, n] : code)
    if (a >= s && uint64_t{a} < uint64_t{s} + n) return true;
  return false;
}

// The code range containing `a` as [start, end).
std::pair<uint32_t, uint64_t> code_range_of(const std::vector<CodeRange>& code, uint32_t a) {
  for (auto [s, n] : code)
    if (a >= s && uint64_t{a} < uint64_t{s} + n) return {s, uint64_t{s} + n};
  return {0, 0};
}

// True if the straight-line code before the bctr at `at` contains an indexed load (lwzx/lhzx/lbzx):
// a jump table we could not resolve, whose case labels follow the bctr.
bool switch_like(const Reader& mem, uint32_t at, uint32_t start) {
  for (int k = 1; k <= kSwitchWindow && at - 4u * static_cast<uint32_t>(k) >= start; ++k) {
    auto w = mem.word(at - 4u * static_cast<uint32_t>(k));
    if (!w || op(*w) == 16 || op(*w) == 18 || op(*w) == 19) return false;
    if (op(*w) == 31 && (xo(*w) == 23 || xo(*w) == 87 || xo(*w) == 279)) return true;
  }
  return false;
}

}  // namespace

std::vector<FunctionRange> discover_pointer_functions(std::span<const uint8_t> image, uint32_t base,
                                                      const std::vector<CodeRange>& code,
                                                      const std::vector<FunctionRange>& known) {
  const Reader mem(image, base);

  // Known ranges, each owning its handler words, sorted by their effective start.
  struct Span {
    uint32_t lo;
    uint64_t hi;
  };
  std::vector<Span> spans;
  std::set<uint32_t> known_starts;
  for (const auto& f : known) {
    const uint32_t lo = f.has_handler && f.start >= 8 ? f.start - 8 : f.start;
    spans.push_back({lo, uint64_t{f.start} + f.size});
    known_starts.insert(f.start);
  }
  std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.lo < b.lo; });
  {  // merge: hand-written ranges may overlap .pdata ones
    std::vector<Span> merged;
    for (const auto& s : spans) {
      if (!merged.empty() && s.lo <= merged.back().hi)
        merged.back().hi = std::max(merged.back().hi, s.hi);
      else
        merged.push_back(s);
    }
    spans = std::move(merged);
  }
  auto inside_known = [&](uint32_t a) {
    auto it = std::upper_bound(spans.begin(), spans.end(), a, [](uint32_t x, const Span& s) { return x < s.lo; });
    return it != spans.begin() && a < std::prev(it)->hi;
  };
  // End of the gap holding `a`: the next known effective start.
  auto gap_end = [&](uint32_t a) -> uint64_t {
    auto it = std::upper_bound(spans.begin(), spans.end(), a, [](uint32_t x, const Span& s) { return x < s.lo; });
    return it == spans.end() ? uint64_t{0xFFFFFFFF} + 1 : it->lo;
  };

  // Every relative branch in code: target -> source (for the interior check), and the bl targets.
  std::multimap<uint32_t, uint32_t> branches;
  std::set<uint32_t> bl_targets;
  std::set<uint32_t> raw;  // candidate addresses from every source
  for (auto [s, n] : code) {
    for (uint32_t a = (s + 3) & ~3u; uint64_t{a} + 4 <= uint64_t{s} + n; a += 4) {
      auto w = mem.word(a);
      if (!w) break;
      if (auto t = relative_target(*w, a)) {
        branches.emplace(*t, a);
        if (is_bl(*w)) bl_targets.insert(*t);
      }
      // lis rX, hi ; ... ; addi rY, rX, lo
      if (op(*w) == 15 && ra(*w) == 0) {
        const uint32_t r = rd(*w);
        for (int k = 1; k <= kLisWindow; ++k) {
          auto n2 = mem.word(a + 4u * static_cast<uint32_t>(k));
          if (!n2 || op(*n2) == 16 || op(*n2) == 18 || op(*n2) == 19) break;
          if (op(*n2) == 14 && ra(*n2) == r) {
            raw.insert((*w << 16) + static_cast<uint32_t>(static_cast<int16_t>(*n2 & 0xFFFF)));
            break;
          }
          if (rd(*n2) == r || ra(*n2) == r) break;  // r rewritten (or used in a way we do not follow)
        }
      }
    }
  }
  raw.insert(bl_targets.begin(), bl_targets.end());
  // Code pointers stored in data (everything outside the code ranges).
  for (uint64_t off = 0; off + 4 <= image.size(); off += 4) {
    const uint32_t a = base + static_cast<uint32_t>(off);
    if (in_code(code, a)) continue;
    const uint32_t v = *mem.word(a);
    if (in_code(code, v)) raw.insert(v);
  }

  // Plausible starts: aligned, in code, in a gap, non-zero, and not reachable by falling through.
  std::set<uint32_t> cands;
  for (uint32_t c : raw) {
    if ((c & 3) || !in_code(code, c) || inside_known(c) || known_starts.contains(c)) continue;
    auto w = mem.word(c);
    if (!w || *w == 0) continue;
    const uint32_t rs = code_range_of(code, c).first;
    if (c != rs) {
      auto prev = mem.word(c - 4);
      if (!prev) continue;
      const bool ok = *prev == 0 || is_unconditional(*prev) || (is_bl(*prev) && bl_targets.contains(c));
      if (!ok) continue;
    }
    cands.insert(c);
  }

  // Walk each candidate to its end.
  std::set<uint32_t> starts = known_starts;
  starts.insert(cands.begin(), cands.end());
  std::map<uint32_t, uint32_t> ranges;  // start -> end
  std::set<uint32_t> rejected;
  for (uint32_t c : cands) {
    const uint64_t limit = std::min(gap_end(c), code_range_of(code, c).second);
    uint32_t reach = c;  // furthest forward branch target inside the function so far
    std::optional<uint32_t> end;
    for (uint32_t a = c; uint64_t{a} + 4 <= limit && a - c < kMaxFunctionSize; a += 4) {
      if (a != c && starts.contains(a)) {
        rejected.insert(a);  // fell through into another function: neither boundary can be trusted
        break;
      }
      const uint32_t i = mem.word(a).value_or(0);
      if (i == 0) break;  // padding before the function ended
      if (auto t = relative_target(i, a); t && !(i & 1)) {
        if (starts.contains(*t) && *t != c) {
          // (conditional) tail call
        } else if (*t >= c && uint64_t{*t} < limit) {
          if (*t > a) reach = std::max(reach, *t);
        } else {
          break;  // branch into code that belongs to no known function
        }
      }
      if (is_unconditional(i) && reach <= a) {
        if (i == kBctr && switch_like(mem, a, c)) break;
        end = a + 4;
        break;
      }
    }
    if (end)
      ranges.emplace(c, *end);
    else
      rejected.insert(c);
  }

  std::vector<FunctionRange> out;
  for (auto [s, e] : ranges) {
    if (rejected.contains(s)) continue;
    bool ok = true;
    for (auto it = branches.upper_bound(s); ok && it != branches.end() && it->first < e; ++it)
      ok = it->second >= s && it->second < e;  // only the function itself may branch into its interior
    if (ok) out.push_back({s, e - s, false});
  }
  return out;
}

}  // namespace lr::pipeline
