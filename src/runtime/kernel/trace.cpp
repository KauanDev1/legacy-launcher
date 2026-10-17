// SPDX-License-Identifier: MIT
#include "runtime/kernel/trace.h"

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "runtime/kernel/threads.h"

namespace lr::kernel {

namespace {

constexpr size_t kBufferSize = 64 * 1024;

// All guarded by g_lock (a spinlock, so the fault handler can try it without blocking).
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
int g_fd = -1;
bool g_owns_fd = false;
char g_buf[kBufferSize];
size_t g_len = 0;

std::atomic<bool> g_enabled{false};

struct SpinLock {
  SpinLock() {
    while (g_lock.test_and_set(std::memory_order_acquire)) ::sched_yield();
  }
  ~SpinLock() { g_lock.clear(std::memory_order_release); }
  SpinLock(const SpinLock&) = delete;
  SpinLock& operator=(const SpinLock&) = delete;
};

// Async-signal-safe (write(2) only). Runs with g_lock held.
void write_all_locked() {
  size_t off = 0;
  while (off < g_len && g_fd >= 0) {
    const ssize_t n = ::write(g_fd, g_buf + off, g_len - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;  // nothing sensible to do on a failing trace file: drop the lines
    off += static_cast<size_t>(n);
  }
  g_len = 0;
}

void append(const char* line, size_t n) {
  SpinLock lock;
  if (g_fd < 0) return;
  if (g_len + n > kBufferSize) write_all_locked();
  if (n > kBufferSize) n = kBufferSize;
  std::memcpy(g_buf + g_len, line, n);
  g_len += n;
}

void log_line(const char* name, const GuestCall& c, const char* ret) {
  const GuestThread* t = current_thread();
  char line[512];
  int n = std::snprintf(line, sizeof line, "[%u] %s r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X -> %s\n",
                        t != nullptr ? t->id() : 0u, name, static_cast<uint32_t>(c.gpr[0]),
                        static_cast<uint32_t>(c.gpr[1]), static_cast<uint32_t>(c.gpr[2]),
                        static_cast<uint32_t>(c.gpr[3]), ret);
  if (n < 0) return;
  if (static_cast<size_t>(n) >= sizeof line) {  // absurdly long name: keep the line well formed
    n = sizeof line - 1;
    line[n - 1] = '\n';
  }
  append(line, static_cast<size_t>(n));
}

void close_locked() {
  write_all_locked();
  if (g_owns_fd && g_fd >= 0) ::close(g_fd);
  g_fd = -1;
  g_owns_fd = false;
}

}  // namespace

Result<void> enable_import_trace(const std::optional<std::filesystem::path>& file) {
  int fd = 2;
  if (file) {
    std::error_code ec;
    if (file->has_parent_path()) std::filesystem::create_directories(file->parent_path(), ec);
    if (ec)
      return Error{ErrorCode::IoError, "cannot create " + file->parent_path().string() + ": " + ec.message(), ""};
    fd = ::open(file->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
      return Error{ErrorCode::IoError, "cannot open the import trace " + file->string() + ": " + std::strerror(errno),
                   ""};
  }
  {
    SpinLock lock;
    close_locked();
    g_fd = fd;
    g_owns_fd = file.has_value();
  }
  static const bool registered = [] { return std::atexit(flush_import_trace) == 0; }();
  (void)registered;
  g_enabled.store(true, std::memory_order_release);
  return {};
}

void disable_import_trace() {
  g_enabled.store(false, std::memory_order_release);
  SpinLock lock;
  close_locked();
}

bool import_trace_enabled() { return g_enabled.load(std::memory_order_acquire); }

void trace_import_return(const char* name, const GuestCall& c) {
  if (!g_enabled.load(std::memory_order_acquire)) return;
  char ret[16];
  std::snprintf(ret, sizeof ret, "0x%08X", static_cast<uint32_t>(c.ret_gpr));
  log_line(name, c, ret);
}

void trace_import_missing(const char* name, const GuestCall& c) {
  if (!g_enabled.load(std::memory_order_acquire)) return;
  log_line(name, c, "missing");
}

void flush_import_trace() {
  if (!g_enabled.load(std::memory_order_acquire)) return;
  SpinLock lock;
  write_all_locked();
}

void flush_import_trace_from_signal() {
  if (!g_enabled.load(std::memory_order_acquire)) return;
  if (g_lock.test_and_set(std::memory_order_acquire)) return;  // busy (possibly this very thread): give up
  write_all_locked();
  g_lock.clear(std::memory_order_release);
}

}  // namespace lr::kernel
