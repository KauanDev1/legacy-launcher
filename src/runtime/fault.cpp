// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/fault.h"

#include <signal.h>
#include <unistd.h>

#include <cstdlib>

namespace lr::runtime {

namespace {

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

struct Installed {
  const uint8_t* base = nullptr;
  uint64_t size = 0;
};
Installed g_installed;

void handler(int signo, siginfo_t* info, void*) {
  char buf[160];
  size_t n = format_fault(buf, sizeof buf - 1, signo, g_installed.base, g_installed.size, info ? info->si_addr : nullptr);
  buf[n++] = '\n';
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
  } else {
    o.str(" at host ");
    o.hex(reinterpret_cast<uintptr_t>(host), 1);
    o.str(" (outside guest memory)");
  }
  buf[o.len] = '\0';
  return o.len;
}

std::string describe_fault(int signo, const uint8_t* base, uint64_t size, const void* host_addr) {
  char buf[160];
  size_t n = format_fault(buf, sizeof buf, signo, base, size, host_addr);
  return std::string(buf, n);
}

void install_fault_handlers(const GuestMemory& mem) {
  g_installed = {mem.base(), mem.size()};

  static char* alt_stack = nullptr;  // intentionally leaked: lives for the process
  if (alt_stack == nullptr) {
    const size_t sz = 64 * 1024;
    alt_stack = static_cast<char*>(std::malloc(sz));
    if (alt_stack != nullptr) {
      stack_t ss{};
      ss.ss_sp = alt_stack;
      ss.ss_size = sz;
      ::sigaltstack(&ss, nullptr);
    }
  }

  struct sigaction sa{};
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  ::sigemptyset(&sa.sa_mask);
  for (int s : {SIGSEGV, SIGBUS, SIGILL}) ::sigaction(s, &sa, nullptr);
}

}  // namespace lr::runtime
