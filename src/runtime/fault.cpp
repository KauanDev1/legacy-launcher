// SPDX-License-Identifier: MIT
#include "runtime/fault.h"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/kernel/trace.h"

namespace lr::runtime {

namespace {

constexpr size_t kFaultBufSize = 256;

struct Out {
  char* p;
  size_t cap;  // usable chars (excluding NUL)
  size_t len = 0;
  void put(char c) {
    if (len < cap) p[len++] = c;
  }
  void str(const char* s) {
    while (*s) put(*s++);
  }
  void hex(uint64_t v, int min_digits) {
    char tmp[16];
    int n = 0;
    do {
      tmp[n++] = "0123456789abcdef"[v & 0xF];
      v >>= 4;
    } while (v != 0);
    while (n < min_digits) tmp[n++] = '0';
    str("0x");
    while (n > 0) put(tmp[--n]);
  }
  void dec(int v) {
    char tmp[12];
    int n = 0;
    unsigned u = v < 0 ? 0u - static_cast<unsigned>(v) : static_cast<unsigned>(v);
    do {
      tmp[n++] = static_cast<char>('0' + u % 10);
      u /= 10;
    } while (u != 0);
    if (v < 0) put('-');
    while (n > 0) put(tmp[--n]);
  }
};

const char* signal_name(int signo) {
  switch (signo) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    default: return nullptr;
  }
}

// note_fault_page table. Entries [0, g_page_note_count) are complete: a writer fills an entry, then publishes it by
// bumping the count with release order; format_fault reads the count with acquire order.
struct PageNote {
  uint32_t page;
  char what[96];
};
constexpr size_t kMaxPageNotes = 512;
PageNote g_page_notes[kMaxPageNotes];
std::atomic<size_t> g_page_note_count{0};

const char* page_note(uint32_t guest) {
  const size_t n = g_page_note_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    if (g_page_notes[i].page == (guest & ~0xFFFu)) return g_page_notes[i].what;
  }
  return nullptr;
}

struct Installed {
  const uint8_t* base = nullptr;
  uint64_t size = 0;
};
Installed g_installed;

void handler(int signo, siginfo_t* info, void*) {
  char buf[kFaultBufSize];
  size_t n = format_fault(buf, sizeof buf - 1, signo, g_installed.base, g_installed.size, info ? info->si_addr : nullptr);
  buf[n++] = '\n';
  lr::kernel::flush_import_trace_from_signal();  // async-signal-safe: never blocks, may skip (kernel/trace.h)
  ssize_t ignored = ::write(2, buf, n);
  (void)ignored;
  ::_exit(4);
}

}  // namespace

std::optional<uint32_t> guest_address_of(const uint8_t* base, uint64_t size, const void* host) {
  const auto b = reinterpret_cast<uintptr_t>(base);
  const auto h = reinterpret_cast<uintptr_t>(host);
  if (base == nullptr || h < b) return std::nullopt;
  const uint64_t off = h - b;
  if (off >= size || off > 0xFFFF'FFFFull) return std::nullopt;
  return static_cast<uint32_t>(off);
}

size_t format_fault(char* buf, size_t n, int signo, const uint8_t* base, uint64_t size, const void* host) {
  if (buf == nullptr || n == 0) return 0;
  Out o{buf, n - 1};
  if (const char* name = signal_name(signo)) {
    o.str(name);
  } else {
    o.str("signal ");
    o.dec(signo);
  }
  if (auto g = guest_address_of(base, size, host)) {
    o.str(" at guest ");
    o.hex(*g, 8);
    if (const char* what = page_note(*g)) {
      o.str(" (");
      o.str(what);
      o.put(')');
    }
  } else {
    o.str(" at host ");
    o.hex(reinterpret_cast<uintptr_t>(host), 1);
    o.str(" (outside guest memory)");
    // For SIGILL si_addr is the faulting host PC: an instruction the host CPU lacks, most likely AVX
    // (the game template compiles with -march=sandybridge).
    if (signo == SIGILL)
      o.str("\nhint: illegal host instruction; the game is built with -march=sandybridge and needs a CPU with AVX");
  }
  buf[o.len] = '\0';
  return o.len;
}

bool note_fault_page(uint32_t page, std::string_view what) {
  static std::mutex m;  // serializes writers; readers (format_fault) only use the published count
  std::lock_guard<std::mutex> lock(m);
  const size_t n = g_page_note_count.load(std::memory_order_relaxed);
  if (n >= kMaxPageNotes) return false;
  PageNote& e = g_page_notes[n];
  e.page = page & ~0xFFFu;
  const size_t len = what.size() < sizeof e.what - 1 ? what.size() : sizeof e.what - 1;
  std::memcpy(e.what, what.data(), len);
  e.what[len] = '\0';
  g_page_note_count.store(n + 1, std::memory_order_release);
  return true;
}

void clear_fault_page_notes() { g_page_note_count.store(0, std::memory_order_release); }

std::string describe_fault(int signo, const uint8_t* base, uint64_t size, const void* host_addr) {
  char buf[kFaultBufSize];
  size_t n = format_fault(buf, sizeof buf, signo, base, size, host_addr);
  return std::string(buf, n);
}

namespace {

Error io_error(const char* what) {
  return Error{ErrorCode::IoError, std::string(what) + ": " + std::strerror(errno), ""};
}

constexpr size_t kAltStackSize = 64 * 1024;

// Owns the calling thread's alt stack; unregisters and frees it at thread exit.
struct ThreadAltStack {
  char* mem = nullptr;
  ~ThreadAltStack() {
    if (mem == nullptr) return;
    stack_t ss{};
    ss.ss_flags = SS_DISABLE;
    ::sigaltstack(&ss, nullptr);
    std::free(mem);
  }
};
thread_local ThreadAltStack t_altstack;

}  // namespace

Result<void> install_thread_altstack() {
  if (t_altstack.mem != nullptr) return {};
  char* mem = static_cast<char*>(std::malloc(kAltStackSize));
  if (mem == nullptr) return Error{ErrorCode::InsufficientResources, "cannot allocate alternate signal stack", ""};
  stack_t ss{};
  ss.ss_sp = mem;
  ss.ss_size = kAltStackSize;
  if (::sigaltstack(&ss, nullptr) != 0) {
    Error e = io_error("sigaltstack failed");
    std::free(mem);
    return e;
  }
  t_altstack.mem = mem;
  return {};
}

Result<void> install_fault_handlers(const GuestMemory& mem) {
  LR_TRY(install_thread_altstack());
  g_installed = {mem.base(), mem.size()};
  struct sigaction sa{};
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  if (::sigemptyset(&sa.sa_mask) != 0) return io_error("sigemptyset failed");
  for (int s : {SIGSEGV, SIGBUS, SIGILL}) {
    if (::sigaction(s, &sa, nullptr) != 0) return io_error("sigaction failed");
  }
  return {};
}

}  // namespace lr::runtime
