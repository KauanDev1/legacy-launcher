// SPDX-License-Identifier: MIT
#include "runtime/write_watch.h"

#include <execinfo.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>

#include "runtime/fault.h"

namespace lr::runtime {

namespace {

constexpr uint32_t kPage = 0x1000;
constexpr size_t kMaxRanges = 16;
constexpr size_t kMaxSitesPerRange = 64;
constexpr int kSiteDepth = 3;    // guest functions that make a call site
constexpr int kLogDepth = 6;     // guest functions printed per line
constexpr int kMaxFrames = 48;   // host frames walked
constexpr int kMaxPending = 4;   // pages one instruction may unprotect before its single step ends
constexpr greg_t kTrapFlag = 0x100;

struct Site {
  uint32_t fn[kSiteDepth];
};

// Index kMaxSitesPerRange of the per-site counters collects the writes of sites past the table.
constexpr uint32_t kOtherSite = kMaxSitesPerRange;

struct Range {
  uint32_t addr = 0;
  uint32_t len = 0;
  uint32_t first_page = 0;  // guest page index
  uint32_t end_page = 0;    // one past the last page
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> nonzero_stores{0};  // stores that left a nonzero byte (see check_store)
  uint32_t nsites = 0;  // under g_lock
  Site sites[kMaxSitesPerRange]{};
  std::atomic<uint64_t> site_count[kMaxSitesPerRange + 1]{};
  std::atomic<uint64_t> site_nonzero[kMaxSitesPerRange + 1]{};
};

Range g_ranges[kMaxRanges];
std::atomic<size_t> g_nranges{0};
std::atomic<bool> g_armed{false};
const uint8_t* g_base = nullptr;
uint64_t g_size = 0;
int g_fd = 2;
HostCodeMap g_code;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;  // serializes the site tables and the log lines

thread_local uint32_t t_pending[kMaxPending];
thread_local int t_npending = 0;

// Stores of this thread's single step whose value is checked once it has run (trap_handler).
struct PendingCheck {
  uint32_t range;
  uint32_t site;
  uint32_t addr;
};
thread_local PendingCheck t_checks[kMaxPending];
thread_local int t_nchecks = 0;

// Nonzero bytes in guest [addr, addr + len). The watched pages are always readable.
uint64_t nonzero_bytes(uint32_t addr, uint32_t len) {
  uint64_t n = 0;
  for (uint32_t i = 0; i < len; ++i) n += g_base[uint64_t{addr} + i] != 0;
  return n;
}

// After a single step: did the store at `c.addr` leave a nonzero byte? Looks at up to 8 bytes from the faulting
// address (the store's width is unknown), inside the range and the page.
void check_store(const PendingCheck& c) {
  Range& r = g_ranges[c.range];
  const uint64_t end = std::min<uint64_t>({uint64_t{c.addr} + 8, uint64_t{r.addr} + r.len,
                                           (uint64_t{c.addr} | (kPage - 1)) + 1});
  if (nonzero_bytes(c.addr, static_cast<uint32_t>(end - c.addr)) == 0) return;
  r.nonzero_stores.fetch_add(1, std::memory_order_relaxed);
  r.site_nonzero[c.site].fetch_add(1, std::memory_order_relaxed);
}

// Async-signal-safe line builder (no allocation, no stdio).
struct Line {
  char buf[512];
  size_t len = 0;
  void put(char c) {
    if (len < sizeof buf) buf[len++] = c;
  }
  void str(const char* s) {
    while (*s) put(*s++);
  }
  void hex(uint64_t v, int min_digits = 1) {
    char tmp[16];
    int n = 0;
    do {
      tmp[n++] = "0123456789ABCDEF"[v & 0xF];
      v >>= 4;
    } while (v != 0);
    while (n < min_digits) tmp[n++] = '0';
    str("0x");
    while (n > 0) put(tmp[--n]);
  }
  void dec(uint64_t v) {
    char tmp[24];
    int n = 0;
    do {
      tmp[n++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v != 0);
    while (n > 0) put(tmp[--n]);
  }
  void flush() {
    size_t off = 0;
    while (off < len) {
      const ssize_t n = ::write(g_fd, buf + off, len - off);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
  }
};

struct Lock {
  Lock() {
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
  }
  ~Lock() { g_lock.clear(std::memory_order_release); }
};

bool page_watched(uint32_t page_index) {
  const size_t n = g_nranges.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (page_index >= g_ranges[i].first_page && page_index < g_ranges[i].end_page) return true;
  return false;
}

// Guest functions on the host stack, innermost first. `pc` (the interrupted instruction, may be 0) comes first; the
// return addresses from backtrace() follow (looked up at address - 1, inside their call instruction).
int guest_frames(uintptr_t pc, uint32_t* out, int max) {
  void* frames[kMaxFrames];
  const int n = ::backtrace(frames, kMaxFrames);
  int k = 0;
  auto add = [&](uintptr_t host) {
    const auto g = g_code.guest_function(host);
    if (!g || k >= max) return;
    if (k > 0 && out[k - 1] == *g) return;  // recursion and the signal frame repeat the innermost function
    out[k++] = *g;
  };
  if (pc != 0) add(pc);
  for (int i = 0; i < n; ++i) {
    const auto a = reinterpret_cast<uintptr_t>(frames[i]);
    if (a != pc) add(a - 1);
  }
  return k;
}

// Counts one write into range `r` and logs it when its call site is new (or the count reaches a power of two from
// 1024). `what` names a host-side write (nullptr for a faulting guest store). Returns the site's counter index.
// Async-signal-safe for what == nullptr.
uint32_t record(size_t r, uint32_t guest, uint32_t len, uintptr_t pc, const char* what) {
  Range& range = g_ranges[r];
  const uint64_t n = range.count.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t fns[kLogDepth] = {};
  const int depth = guest_frames(pc, fns, kLogDepth);
  Site site{};
  for (int i = 0; i < kSiteDepth && i < depth; ++i) site.fn[i] = fns[i];

  Lock lock;
  uint32_t index = kOtherSite;
  for (uint32_t i = 0; i < range.nsites && index == kOtherSite; ++i)
    if (std::memcmp(&range.sites[i], &site, sizeof site) == 0) index = i;
  const bool fresh = index == kOtherSite && range.nsites < kMaxSitesPerRange;
  if (fresh) {
    index = range.nsites++;
    range.sites[index] = site;
  }
  range.site_count[index].fetch_add(1, std::memory_order_relaxed);
  const bool milestone = n >= 1024 && (n & (n - 1)) == 0;
  if (!fresh && !milestone) return index;

  Line l;
  l.str("[watch] ");
  if (what != nullptr) {
    l.str("host ");
    l.str(what);
    l.put(' ');
  } else {
    l.str("write ");
  }
  l.hex(guest, 8);
  if (len != 0) {
    l.put('+');
    l.hex(len);
  }
  l.str(" range ");
  l.dec(r);
  l.str(" n=");
  l.dec(n);
  l.str(" tid=");
  l.dec(static_cast<uint64_t>(::syscall(SYS_gettid)));
  if (!fresh) {
    l.str(" (count) nonzero_stores=");
    l.dec(range.nonzero_stores.load(std::memory_order_relaxed));
    l.str(" nonzero_bytes=");
    l.dec(nonzero_bytes(range.addr, range.len));
    l.put('\n');
    l.flush();
    return index;
  }
  l.str(" site ");
  l.dec(index);
  l.str(" fn");
  if (depth == 0) l.str(" (no guest frame)");
  for (int i = 0; i < depth; ++i) {
    l.str(i == 0 ? " " : " <- ");
    l.hex(fns[i], 8);
  }
  if (pc != 0) {
    l.str(" pc=");
    l.hex(pc);
  }
  l.put('\n');
  l.flush();
  return index;
}

void trap_handler(int signo, siginfo_t*, void* uctx) {
  if (t_npending == 0) {  // not a single step of ours: the default action (core dump)
    ::signal(signo, SIG_DFL);
    ::raise(signo);
    return;
  }
  for (int i = 0; i < t_nchecks; ++i) check_store(t_checks[i]);
  t_nchecks = 0;
  for (int i = 0; i < t_npending; ++i) ::mprotect(const_cast<uint8_t*>(g_base) + t_pending[i], kPage, PROT_READ);
  t_npending = 0;
  static_cast<ucontext_t*>(uctx)->uc_mcontext.gregs[REG_EFL] &= ~kTrapFlag;
}

std::optional<uint64_t> parse_number(std::string_view s) {
  if (s.empty()) return std::nullopt;
  int radix = 10;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    radix = 16;
    s.remove_prefix(2);
  }
  uint64_t v = 0;
  for (const char c : s) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (radix == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (radix == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return std::nullopt;
    v = v * static_cast<uint64_t>(radix) + static_cast<uint64_t>(d);
    if (v > 0xFFFF'FFFFull + 1) return std::nullopt;
  }
  return v;
}

}  // namespace

Result<WatchRange> parse_watch_spec(std::string_view spec) {
  const auto bad = [&] {
    return Error{ErrorCode::InvalidFormat, "--watch-guest needs <addr>:<len> (hex with 0x, or decimal), got '" +
                                               std::string(spec) + "'", ""};
  };
  const size_t colon = spec.find(':');
  if (colon == std::string_view::npos) return bad();
  const auto addr = parse_number(spec.substr(0, colon));
  const auto len = parse_number(spec.substr(colon + 1));
  if (!addr || !len || *len == 0 || *addr > 0xFFFF'FFFFull || *addr + *len > 0x1'0000'0000ull) return bad();
  return WatchRange{static_cast<uint32_t>(*addr), static_cast<uint32_t>(*len)};
}

HostCodeMap::HostCodeMap(std::vector<HostCodeEntry> entries) : entries_(std::move(entries)) {
  std::sort(entries_.begin(), entries_.end(), [](const HostCodeEntry& a, const HostCodeEntry& b) { return a.host < b.host; });
}

std::optional<uint32_t> HostCodeMap::guest_function(uintptr_t host_pc) const {
  // The last entry gets a short tail: the host code linked after the generated functions is not guest code.
  constexpr uintptr_t kLastTail = 0x1000;
  if (entries_.empty() || host_pc < entries_.front().host) return std::nullopt;
  auto it = std::upper_bound(entries_.begin(), entries_.end(), host_pc,
                             [](uintptr_t pc, const HostCodeEntry& e) { return pc < e.host; });
  --it;
  if (it + 1 == entries_.end() && host_pc - it->host >= kLastTail) return std::nullopt;
  return it->guest;
}

Result<void> install_write_watch(const GuestMemory& mem, std::span<const WatchRange> ranges, HostCodeMap code,
                                 int log_fd) {
  if (g_armed.load()) return Error{ErrorCode::InvalidFormat, "the guest write watch is already installed", ""};
  if (ranges.empty() || ranges.size() > kMaxRanges)
    return Error{ErrorCode::InvalidFormat, "--watch-guest takes 1 to 16 ranges", ""};
  for (const WatchRange& w : ranges)
    if (w.len == 0 || uint64_t{w.addr} + w.len > mem.size())
      return Error{ErrorCode::OutOfRange, "--watch-guest range runs past guest memory", ""};
  g_base = mem.base();
  g_size = mem.size();
  g_fd = log_fd;
  g_code = std::move(code);
  // backtrace() loads libgcc on its first call: do it now, outside any signal handler.
  void* warm[4];
  (void)::backtrace(warm, 4);

  struct sigaction sa{};
  sa.sa_sigaction = trap_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  if (::sigaction(SIGTRAP, &sa, nullptr) != 0)
    return Error{ErrorCode::IoError, std::string("sigaction(SIGTRAP) failed: ") + std::strerror(errno), ""};

  for (size_t i = 0; i < ranges.size(); ++i) {
    Range& r = g_ranges[i];
    r.addr = ranges[i].addr;
    r.len = ranges[i].len;
    r.first_page = r.addr / kPage;
    r.end_page = static_cast<uint32_t>((uint64_t{r.addr} + r.len + kPage - 1) / kPage);
    r.count.store(0);
    r.nsites = 0;
  }
  g_nranges.store(ranges.size(), std::memory_order_release);
  g_armed.store(true, std::memory_order_release);
  for (size_t i = 0; i < ranges.size(); ++i) {
    const Range& r = g_ranges[i];
    const uint32_t start = r.first_page * kPage;
    const uint64_t bytes = (uint64_t{r.end_page} - r.first_page) * kPage;
    if (::mprotect(mem.base() + start, bytes, PROT_READ) != 0)
      return Error{ErrorCode::IoError, std::string("mprotect for --watch-guest failed: ") + std::strerror(errno), ""};
    Line l;
    l.str("[watch] armed range ");
    l.dec(i);
    l.put(' ');
    l.hex(r.addr, 8);
    l.put('+');
    l.hex(r.len);
    l.put('\n');
    l.flush();
  }
  return {};
}

bool write_watch_handle_segv(siginfo_t* info, void* ucontext) {
  if (!g_armed.load(std::memory_order_acquire) || info == nullptr || ucontext == nullptr) return false;
  if (info->si_code != SEGV_ACCERR) return false;
  const auto g = guest_address_of(g_base, g_size, info->si_addr);
  if (!g || !page_watched(*g / kPage)) return false;
  auto* uc = static_cast<ucontext_t*>(ucontext);
  if ((uc->uc_mcontext.gregs[REG_ERR] & 2) == 0) return false;  // a read or fetch: not ours
  if (t_npending >= kMaxPending) return false;                    // cannot make progress: report it
  const auto pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
  const size_t n = g_nranges.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (*g >= g_ranges[i].addr && uint64_t{*g} < uint64_t{g_ranges[i].addr} + g_ranges[i].len) {
      const uint32_t site = record(i, *g, 0, pc, nullptr);
      if (t_nchecks < kMaxPending) t_checks[t_nchecks++] = {static_cast<uint32_t>(i), site, *g};
    }
  const uint32_t page = *g & ~(kPage - 1);
  if (::mprotect(const_cast<uint8_t*>(g_base) + page, kPage, PROT_READ | PROT_WRITE) != 0) return false;
  t_pending[t_npending++] = page;
  uc->uc_mcontext.gregs[REG_EFL] |= kTrapFlag;
  return true;
}

bool write_watch_overlaps(uint32_t addr, uint32_t len) {
  if (!g_armed.load(std::memory_order_acquire) || len == 0) return false;
  const uint32_t first = addr / kPage;
  const auto end = static_cast<uint32_t>((uint64_t{addr} + len + kPage - 1) / kPage);
  const size_t n = g_nranges.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (first < g_ranges[i].end_page && end > g_ranges[i].first_page) return true;
  return false;
}

void write_watch_note_host_write(uint32_t addr, uint32_t len, const char* what) {
  if (!g_armed.load(std::memory_order_acquire) || len == 0) return;
  const size_t n = g_nranges.load(std::memory_order_acquire);
  const uint64_t end = uint64_t{addr} + len;
  for (size_t i = 0; i < n; ++i)
    if (addr < uint64_t{g_ranges[i].addr} + g_ranges[i].len && end > g_ranges[i].addr) (void)record(i, addr, len, 0, what);
}

void write_watch_report() {
  const size_t n = g_nranges.load(std::memory_order_acquire);
  for (size_t r = 0; r < n; ++r) {
    Range& range = g_ranges[r];
    Lock lock;
    Line l;
    l.str("[watch] summary range ");
    l.dec(r);
    l.put(' ');
    l.hex(range.addr, 8);
    l.put('+');
    l.hex(range.len);
    l.str(" writes=");
    l.dec(range.count.load());
    l.str(" nonzero_stores=");
    l.dec(range.nonzero_stores.load());
    l.str(" nonzero_bytes=");
    l.dec(nonzero_bytes(range.addr, range.len));
    l.put('\n');
    l.flush();
    for (uint32_t s = 0; s <= range.nsites; ++s) {
      if (s == range.nsites && range.site_count[kOtherSite].load() == 0) break;
      const uint32_t index = s == range.nsites ? kOtherSite : s;
      Line m;
      m.str("[watch]   site ");
      if (index == kOtherSite) m.str("other");
      else m.dec(index);
      m.str(" writes=");
      m.dec(range.site_count[index].load());
      m.str(" nonzero_stores=");
      m.dec(range.site_nonzero[index].load());
      if (index != kOtherSite) {
        m.str(" fn");
        for (int i = 0; i < kSiteDepth && range.sites[index].fn[i] != 0; ++i) {
          m.str(i == 0 ? " " : " <- ");
          m.hex(range.sites[index].fn[i], 8);
        }
      }
      m.put('\n');
      m.flush();
    }
  }
}

uint64_t write_watch_count(size_t index) {
  return index < g_nranges.load(std::memory_order_acquire) ? g_ranges[index].count.load() : 0;
}

}  // namespace lr::runtime
