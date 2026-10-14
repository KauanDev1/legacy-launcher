// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline/switch_scan.h"

#include <algorithm>
#include <array>
#include <optional>
#include <set>

#include <fmt/format.h>

namespace lr::pipeline {
namespace {

constexpr uint32_t kBctr = 0x4E800420;
constexpr int kMaxWindow = 48;     // instructions between the bound check and bctr
constexpr int kMaxCmpDistance = 12; // instructions between cmplwi and the bgt
constexpr uint32_t kMaxCases = 4096;

uint32_t op(uint32_t i) { return i >> 26; }
uint32_t rd(uint32_t i) { return (i >> 21) & 31; }
uint32_t ra(uint32_t i) { return (i >> 16) & 31; }
uint32_t rb(uint32_t i) { return (i >> 11) & 31; }
uint32_t xo(uint32_t i) { return (i >> 1) & 0x3FF; }
int32_t simm(uint32_t i) { return static_cast<int16_t>(i & 0xFFFF); }
uint32_t uimm(uint32_t i) { return i & 0xFFFF; }

bool is_branch(uint32_t i) {
  return op(i) == 16 || op(i) == 18 || (op(i) == 19 && (xo(i) == 16 || xo(i) == 528));
}

constexpr int kNoGpr = -1;

// GPR written by an instruction the evaluator does not model: a register number, kNoGpr (writes no
// GPR: stores, compares, FPU, VMX, CR logic) or nullopt (unknown instruction: give up).
std::optional<int> gpr_written(uint32_t i) {
  const uint32_t o = op(i);
  switch (o) {
    case 4: case 5: case 6: case 10: case 11: case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    case 59: case 63: case 36: case 38: case 44: case 47: case 62:
      // VMX/VMX128 (4-6), cmpli/cmpi, FPU loads/arith, stores (update forms below), stmw
      if (o == 49 || o == 51 || o == 53 || o == 55) return static_cast<int>(ra(i));  // lfsu/lfdu/stfsu/stfdu
      return kNoGpr;
    case 37: case 39: case 45:  // stwu/stbu/sthu
      return static_cast<int>(ra(i));
    case 7: case 8: case 12: case 13: case 14: case 15: case 32: case 33: case 34: case 35: case 40: case 41:
    case 42: case 43: case 58:  // mulli, subfic, addic(.), addi(s), loads (+update writes rA too: give up)
      if (o == 33 || o == 35 || o == 41 || o == 43) return std::nullopt;
      return static_cast<int>(rd(i));
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30:
      return static_cast<int>(ra(i));  // rlwimi/rlwinm/rlwnm, ori/oris/xori/xoris/andi./andis., rld*
    case 19:
      return xo(i) == 16 || xo(i) == 528 ? std::nullopt : std::optional<int>(kNoGpr);  // CR logic, isync
    case 31: {
      const uint32_t x = xo(i);
      const uint32_t x9 = x & 0x1FF;  // XO-form arithmetic (OE bit stripped)
      switch (x) {
        case 0: case 32:                                    // cmp, cmpl
        case 151: case 215: case 407: case 149: case 663: case 727: case 983:  // stores (x-form)
        case 86: case 54: case 278: case 246: case 1014: case 982: case 598:  // cache ops, sync
        case 231: case 103: case 7: case 39: case 71: case 199: case 391: case 455: case 359:  // VMX x-form
          return kNoGpr;
        case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476:  // logical
        case 24: case 536: case 792: case 824: case 922: case 954: case 986: case 26:  // shifts, ext, cntlzw
        case 27: case 539: case 794: case 58:                                         // 64-bit shifts, cntlzd
          return static_cast<int>(ra(i));
        case 23: case 87: case 279: case 343: case 21: case 341: case 339: case 19: case 535: case 599:
          return static_cast<int>(rd(i));  // indexed loads, mfspr, mfcr
        default:
          break;
      }
      switch (x9) {
        case 8: case 10: case 40: case 104: case 136: case 138: case 200: case 202: case 232: case 234: case 266:
        case 11: case 75: case 235: case 459: case 491: case 9: case 73: case 233: case 457: case 489:
          return static_cast<int>(rd(i));  // subf/add/neg/adde/subfe/mul/div families
        default:
          return std::nullopt;
      }
    }
    default:
      return std::nullopt;
  }
}

class Memory {
 public:
  Memory(std::span<const uint8_t> image, uint32_t base) : image_(image), base_(base) {}
  std::optional<uint32_t> read(uint32_t addr, uint32_t size) const {
    if (addr < base_) return std::nullopt;
    uint64_t off = static_cast<uint64_t>(addr) - base_;
    if (off + size > image_.size()) return std::nullopt;
    uint32_t v = 0;
    for (uint32_t k = 0; k < size; ++k) v = (v << 8) | image_[static_cast<size_t>(off + k)];
    return v;
  }

 private:
  std::span<const uint8_t> image_;
  uint32_t base_;
};

uint32_t rotl32(uint32_t v, uint32_t n) { return n == 0 ? v : (v << n) | (v >> (32 - n)); }

uint32_t mask32(uint32_t mb, uint32_t me) {
  uint32_t m = 0;
  for (uint32_t b = mb;; b = (b + 1) & 31) {
    m |= 0x80000000u >> b;
    if (b == me) break;
  }
  return m;
}

// Evaluates the straight-line instructions in `window` with r[index] = value. Returns CTR, or nullopt
// if anything is unsupported, unknown, or overwrites the index register.
std::optional<uint32_t> evaluate(const Memory& mem, std::span<const uint32_t> window, uint32_t index,
                                 uint32_t aliases, uint32_t value) {
  std::array<std::optional<uint32_t>, 32> r{};
  r[index] = value;
  for (uint32_t n = 0; n < 32; ++n)
    if (aliases & (1u << n)) r[n] = value;
  std::optional<uint32_t> ctr;
  auto get = [&](uint32_t n) { return r[n]; };
  auto get0 = [&](uint32_t n) -> std::optional<uint32_t> { return n == 0 ? std::optional<uint32_t>(0) : r[n]; };
  auto set = [&](uint32_t n, std::optional<uint32_t> v) -> bool {
    if (n == index) return false;
    r[n] = v;
    return true;
  };
  auto bin = [](std::optional<uint32_t> a, std::optional<uint32_t> b, auto f) -> std::optional<uint32_t> {
    if (!a || !b) return std::nullopt;
    return f(*a, *b);
  };
  auto load = [&](std::optional<uint32_t> addr, uint32_t size) -> std::optional<uint32_t> {
    return addr ? mem.read(*addr, size) : std::nullopt;
  };
  // Unmodelled instruction: forget the register it writes (fails if that is the index).
  auto clobber = [&](uint32_t i) -> bool {
    auto w = gpr_written(i);
    if (!w) return false;
    return *w == kNoGpr || set(static_cast<uint32_t>(*w), std::nullopt);
  };

  for (uint32_t i : window) {
    switch (op(i)) {
      case 14:  // addi / li
        if (!set(rd(i), bin(get0(ra(i)), uint32_t{0}, [&](uint32_t a, uint32_t) { return a + static_cast<uint32_t>(simm(i)); })))
          return std::nullopt;
        break;
      case 15:  // addis / lis
        if (!set(rd(i), bin(get0(ra(i)), uint32_t{0}, [&](uint32_t a, uint32_t) { return a + (uimm(i) << 16); })))
          return std::nullopt;
        break;
      case 24:  // ori (nop = ori r0,r0,0)
        if (i == 0x60000000) break;
        if (!set(ra(i), bin(get(rd(i)), uint32_t{0}, [&](uint32_t a, uint32_t) { return a | uimm(i); }))) return std::nullopt;
        break;
      case 25:  // oris
        if (!set(ra(i), bin(get(rd(i)), uint32_t{0}, [&](uint32_t a, uint32_t) { return a | (uimm(i) << 16); })))
          return std::nullopt;
        break;
      case 21: {  // rlwinm
        uint32_t sh = rb(i), mb = (i >> 6) & 31, me = (i >> 1) & 31;
        if (!set(ra(i), bin(get(rd(i)), uint32_t{0}, [&](uint32_t a, uint32_t) { return rotl32(a, sh) & mask32(mb, me); })))
          return std::nullopt;
        break;
      }
      case 32: case 34: case 40: {  // lwz / lbz / lhz
        uint32_t size = op(i) == 32 ? 4 : op(i) == 34 ? 1 : 2;
        auto addr = bin(get0(ra(i)), uint32_t{0}, [&](uint32_t a, uint32_t) { return a + static_cast<uint32_t>(simm(i)); });
        if (!set(rd(i), load(addr, size))) return std::nullopt;
        break;
      }
      case 36: case 38: case 44: case 52: case 54: case 62:  // stw/stb/sth/stfs/stfd/std: no register writes
        break;
      case 37: case 39: case 45: case 53: case 55:  // update forms write rA
        if (!set(ra(i), std::nullopt)) return std::nullopt;
        break;
      case 31:
        switch (xo(i)) {
          case 444:  // or (mr)
            if (!set(ra(i), bin(get(rd(i)), get(rb(i)), [](uint32_t a, uint32_t b) { return a | b; }))) return std::nullopt;
            break;
          case 151: case 215: case 407: case 149:  // stwx/stbx/sthx/stdx
            break;
          case 266:  // add (Rc/OE only touch CR0/XER)
            if (!set(rd(i), bin(get(ra(i)), get(rb(i)), [](uint32_t a, uint32_t b) { return a + b; }))) return std::nullopt;
            break;
          case 23: case 87: case 279: {  // lwzx / lbzx / lhzx
            uint32_t size = xo(i) == 23 ? 4 : xo(i) == 87 ? 1 : 2;
            auto addr = bin(get0(ra(i)), get(rb(i)), [](uint32_t a, uint32_t b) { return a + b; });
            if (!set(rd(i), load(addr, size))) return std::nullopt;
            break;
          }
          case 467:  // mtspr
            if (((i >> 16) & 31) != 9 || ((i >> 11) & 31) != 0) return std::nullopt;  // only mtctr
            ctr = get(rd(i));
            break;
          default:
            if (!clobber(i)) return std::nullopt;
            break;
        }
        break;
      default:
        if (!clobber(i)) return std::nullopt;
        break;
    }
  }
  return ctr;
}

struct Guard {
  uint32_t window_start = 0;   // first address evaluated (after cmplwi)
  uint32_t branch_addr = 0;    // the bgt / bgtlr
  uint32_t index = 0;
  uint32_t aliases = 0;        // bit n: rN holds a copy of the index (mr before the cmplwi)
  uint32_t max = 0;
  uint32_t default_label = 0;
};

// rB is only a register field in X/XO-form (primary opcode 31) instructions.
bool mentions(uint32_t i, uint32_t reg) { return rd(i) == reg || ra(i) == reg || (op(i) == 31 && rb(i) == reg); }

// Registers copied from/to the index by `mr` in the straight-line code just before the cmplwi at
// `cmp_addr`, provided neither side is touched again before the compare (conservative: any field).
uint32_t find_aliases(const Memory& mem, uint32_t cmp_addr, uint32_t index) {
  uint32_t aliases = 0;
  for (int k = 1; k <= kMaxWindow; ++k) {
    auto w = mem.read(cmp_addr - 4u * static_cast<uint32_t>(k), 4);
    if (!w || is_branch(*w)) break;
    const uint32_t i = *w;
    const bool is_mr = op(i) == 31 && xo(i) == 444 && rd(i) == rb(i) && !(i & 1);  // or rA,rS,rS
    if (!is_mr || (ra(i) != index && rd(i) != index)) continue;
    const uint32_t other = ra(i) == index ? rd(i) : ra(i);
    bool clean = true;
    for (int j = k - 1; j >= 1 && clean; --j) {
      uint32_t between = *mem.read(cmp_addr - 4u * static_cast<uint32_t>(j), 4);
      clean = !mentions(between, other) && rd(between) != index && ra(between) != index;
    }
    if (clean) aliases |= 1u << other;
  }
  return aliases;
}

enum class GuardStatus { None, Incomplete, Found };

// Looks backwards from the bctr at `addr` for "cmplwi crN, rI, max ... bgt crN" with only
// straight-line code in between. None: the code before the bctr is not a bounded switch
// (e.g. a virtual call). Incomplete: a bgt-style guard without a recognisable compare.
std::optional<Guard> find_guard(const Memory& mem, uint32_t addr, GuardStatus& status) {
  status = GuardStatus::None;
  for (int k = 1; k <= kMaxWindow; ++k) {
    auto w = mem.read(addr - 4u * static_cast<uint32_t>(k), 4);
    if (!w) return std::nullopt;
    uint32_t i = *w;
    if (!is_branch(i)) continue;

    const uint32_t baddr = addr - 4u * static_cast<uint32_t>(k);
    const uint32_t bo = rd(i), bi = ra(i);
    const bool cond_true = (bo & 0x1C) == 0x0C;  // branch if CR bit set, CTR ignored
    if (!cond_true || (bi & 3) != 1 || (i & 1)) return std::nullopt;  // must be bgt / bgtlr, no link
    Guard g;
    g.branch_addr = baddr;
    if (op(i) == 16) {
      if (i & 2) return std::nullopt;  // absolute
      g.default_label = baddr + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(i & 0xFFFC)));
    } else if (!(op(i) == 19 && xo(i) == 16)) {
      return std::nullopt;  // bgtctr: not a bound check
    }
    status = GuardStatus::Incomplete;
    const uint32_t crf = bi >> 2;
    for (int j = 1; j <= kMaxCmpDistance; ++j) {
      auto c = mem.read(baddr - 4u * static_cast<uint32_t>(j), 4);
      if (!c || is_branch(*c)) return std::nullopt;
      if (op(*c) == 10 && ((*c >> 23) & 7) == crf && ((*c >> 21) & 1) == 0) {  // cmplwi crf, rI, max
        g.index = ra(*c);
        g.max = uimm(*c);
        g.window_start = baddr - 4u * static_cast<uint32_t>(j) + 4;
        g.aliases = find_aliases(mem, g.window_start - 4, g.index);
        status = GuardStatus::Found;
        return g;
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
}

// True if an indexed load (lwzx/lbzx/lhzx) precedes the bctr in its straight-line block.
bool has_indexed_load(const Memory& mem, uint32_t addr) {
  for (int k = 1; k <= kMaxWindow; ++k) {
    auto w = mem.read(addr - 4u * static_cast<uint32_t>(k), 4);
    if (!w || is_branch(*w)) return false;
    if (op(*w) == 31 && (xo(*w) == 23 || xo(*w) == 87 || xo(*w) == 279)) return true;
  }
  return false;
}

bool has_lis(const Memory& mem, uint32_t addr) {
  for (int k = 1; k <= kMaxWindow; ++k) {
    auto w = mem.read(addr - 4u * static_cast<uint32_t>(k), 4);
    if (!w || is_branch(*w)) return false;
    if (op(*w) == 15 && ra(*w) == 0) return true;
  }
  return false;
}

// Unguarded switch whose index is bounded by a mask in the same block (`clrlwi rX, rY, 26`,
// `rlwinm rX, rY, sh, mb, 31`, `andi. rX, rY, 2^k-1`): the table has exactly mask+1 entries.
std::optional<SwitchTable> masked_table(const Memory& mem, uint32_t addr, uint32_t start, uint32_t size) {
  std::vector<uint32_t> block;  // instructions from the block start up to (excluding) the bctr
  uint32_t first = addr;
  for (int k = 1; k <= kMaxWindow; ++k) {
    auto w = mem.read(addr - 4u * static_cast<uint32_t>(k), 4);
    if (!w || is_branch(*w) || addr - 4u * static_cast<uint32_t>(k) < start) break;
    first = addr - 4u * static_cast<uint32_t>(k);
  }
  for (uint32_t a = first; a < addr; a += 4) block.push_back(*mem.read(a, 4));
  if (!has_indexed_load(mem, addr)) return std::nullopt;

  for (size_t n = block.size(); n-- > 0;) {
    const uint32_t i = block[n];
    uint32_t count = 0;
    if (op(i) == 21 && ((i >> 1) & 31) == 31) {
      uint32_t mb = (i >> 6) & 31;
      if (mb >= 24) count = 1u << (32 - mb);
    } else if (op(i) == 28) {
      uint32_t m = uimm(i);
      if (m && m <= 0xFF && (m & (m + 1)) == 0) count = m + 1;
    }
    if (!count) continue;
    const uint32_t index = ra(i);
    std::span<const uint32_t> window(block.data() + n + 1, block.size() - n - 1);
    SwitchTable t;
    t.base = addr;
    t.r = index;
    bool ok = true;
    for (uint32_t v = 0; v < count && ok; ++v) {
      auto target = evaluate(mem, window, index, 0, v);
      ok = target && (*target & 3) == 0 && *target >= start && *target < start + size;
      if (ok) t.labels.push_back(*target);
    }
    if (ok) return t;
  }
  return std::nullopt;
}

}  // namespace

SwitchScan scan_switch_tables(std::span<const uint8_t> image, uint32_t image_base,
                              std::span<const std::pair<uint32_t, uint32_t>> code) {
  SwitchScan out;
  const Memory mem(image, image_base);
  for (auto [start, size] : code) {
    for (uint32_t addr = (start + 3) & ~3u; addr + 4 <= start + size; addr += 4) {
      auto w = mem.read(addr, 4);
      if (!w) break;
      if (*w != kBctr) continue;

      GuardStatus status;
      auto guard = find_guard(mem, addr, status);
      if (!guard) {
        if (auto t = masked_table(mem, addr, start, size)) {
          out.tables.push_back(std::move(*t));
        } else if (status == GuardStatus::Incomplete && has_indexed_load(mem, addr)) {
          out.unresolved.push_back(addr);
        } else if (has_indexed_load(mem, addr) && has_lis(mem, addr)) {
          out.unbounded.push_back(addr);
        }
        continue;
      }
      if (guard->max >= kMaxCases) {
        out.unresolved.push_back(addr);
        continue;
      }
      std::vector<uint32_t> window;
      for (uint32_t a = guard->window_start; a < addr; a += 4)
        if (a != guard->branch_addr) window.push_back(*mem.read(a, 4));

      SwitchTable t;
      t.base = addr;
      t.r = guard->index;
      t.default_label = guard->default_label;
      bool ok = true;
      for (uint32_t v = 0; v <= guard->max && ok; ++v) {
        auto target = evaluate(mem, window, guard->index, guard->aliases, v);
        ok = target && (*target & 3) == 0 && *target >= start && *target < start + size;
        if (ok) t.labels.push_back(*target);
      }
      if (ok)
        out.tables.push_back(std::move(t));
      else
        out.unresolved.push_back(addr);
    }
  }
  return out;
}

std::vector<FunctionRange> parse_pdata(std::span<const uint8_t> image, uint32_t image_base, uint32_t pdata,
                                       uint32_t size) {
  const Memory mem(image, image_base);
  std::vector<FunctionRange> out;
  for (uint32_t a = pdata; a + 8 <= pdata + size; a += 8) {
    auto begin = mem.read(a, 4), info = mem.read(a + 4, 4);
    if (!begin || !info) break;
    if (*begin == 0) continue;
    out.push_back({*begin, ((*info >> 8) & 0x3FFFFF) * 4, ((*info >> 31) & 1) != 0});
  }
  std::sort(out.begin(), out.end(), [](const FunctionRange& a, const FunctionRange& b) { return a.start < b.start; });
  return out;
}

SwitchFit fit_switch_functions(std::span<const uint8_t> image, uint32_t image_base,
                               std::span<const std::pair<uint32_t, uint32_t>> code,
                               std::span<const FunctionRange> pdata, std::vector<SwitchTable> tables) {
  const Memory mem(image, image_base);
  std::set<uint32_t> bl_targets;
  for (auto [start, size] : code) {
    for (uint32_t a = (start + 3) & ~3u; a + 4 <= start + size; a += 4) {
      auto w = mem.read(a, 4);
      if (!w) break;
      if (op(*w) == 18 && (*w & 3) == 1) {  // bl (relative, link)
        int32_t li = static_cast<int32_t>(*w & 0x03FFFFFC);
        if (li & 0x02000000) li -= 0x04000000;
        bl_targets.insert(a + static_cast<uint32_t>(li));
      }
    }
  }

  SwitchFit out;
  for (auto& t : tables) {
    const uint32_t lo = *std::min_element(t.labels.begin(), t.labels.end());
    const uint32_t hi = *std::max_element(t.labels.begin(), t.labels.end());
    auto next = std::upper_bound(pdata.begin(), pdata.end(), t.base,
                                 [](uint32_t a, const FunctionRange& f) { return a < f.start; });
    const FunctionRange* prev = next == pdata.begin() ? nullptr : &*(next - 1);
    if (prev && t.base < prev->start + prev->size) {  // inside a .pdata function
      if (lo >= prev->start && hi < prev->start + prev->size)
        out.tables.push_back(std::move(t));
      else
        out.dropped.push_back(t.base);
      continue;
    }

    // Leaf function in the gap [gap_lo, gap_hi).
    uint32_t gap_lo = prev ? prev->start + prev->size : 0;
    uint32_t gap_hi = next != pdata.end() ? next->start : 0xFFFFFFFFu;
    if (next != pdata.end() && next->has_handler) gap_hi -= 8;
    uint32_t start = gap_lo;
    while (start < t.base && mem.read(start, 4).value_or(1) == 0) start += 4;  // padding
    auto bl_below = bl_targets.upper_bound(t.base);
    if (bl_below != bl_targets.begin() && *std::prev(bl_below) >= start) start = *std::prev(bl_below);
    uint32_t end = gap_hi;
    if (auto bl_above = bl_targets.upper_bound(hi); bl_above != bl_targets.end() && *bl_above < end) end = *bl_above;
    while (end > hi + 4 && mem.read(end - 4, 4).value_or(1) == 0) end -= 4;  // trailing padding
    if (lo < start || hi >= end || t.base < start) {
      out.dropped.push_back(t.base);
      continue;
    }
    FunctionRange f{start, end - start, false};
    if (std::find(out.functions.begin(), out.functions.end(), f) == out.functions.end()) out.functions.push_back(f);
    out.tables.push_back(std::move(t));
  }
  return out;
}

std::string format_switch_tables(std::span<const SwitchTable> tables) {
  std::string s;
  for (const auto& t : tables) {
    s += fmt::format("[[switch]]\nbase = 0x{:X}\nr = {}\ndefault = 0x{:X}\nlabels = [\n", t.base, t.r, t.default_label);
    for (uint32_t l : t.labels) s += fmt::format("    0x{:X},\n", l);
    s += "]\n\n";
  }
  return s;
}

}  // namespace lr::pipeline
