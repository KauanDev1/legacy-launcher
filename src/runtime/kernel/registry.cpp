// SPDX-License-Identifier: MIT
#include "runtime/kernel/registry.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace lr::kernel {

namespace {
constexpr size_t kTrailSize = 32;

struct Trail {
  std::array<std::string, kTrailSize> names{};  // copies: callers may pass non-literal names
  size_t count = 0;  // total traced; slot = count % kTrailSize
};
thread_local Trail t_trail;

// Function-local statics: LR_KERNEL / LR_KERNEL_STUB register from static initializers in other TUs.
std::mutex& table_mutex() {
  static std::mutex m;
  return m;
}
std::map<std::string, ImportEntry, std::less<>>& table() {
  static std::map<std::string, ImportEntry, std::less<>> t;
  return t;
}
}  // namespace

void register_import(const char* name, ImportState state, KernelFn fn) {
  if (state == ImportState::Missing || fn == nullptr) {
    std::fprintf(stderr, "fatal: kernel import %s registered without an implementation\n", name);
    std::abort();
  }
  std::lock_guard<std::mutex> lock(table_mutex());
  if (!table().emplace(name, ImportEntry{state, fn}).second) {
    std::fprintf(stderr, "fatal: kernel import %s registered twice\n", name);
    std::abort();
  }
}

ImportEntry find_import(std::string_view name) {
  std::lock_guard<std::mutex> lock(table_mutex());
  const auto it = table().find(name);
  return it == table().end() ? ImportEntry{} : it->second;
}

void trace(const char* name) {
  t_trail.names[t_trail.count % kTrailSize].assign(name);
  ++t_trail.count;
}

void unimplemented_import(const char* name, uint32_t caller_lr) {
  UnimplementedImport e{name, caller_lr, {}};
  const size_t n = t_trail.count < kTrailSize ? t_trail.count : kTrailSize;
  for (size_t i = t_trail.count - n; i < t_trail.count; ++i) e.trail.emplace_back(t_trail.names[i % kTrailSize]);
  throw e;
}

std::string format_report(const UnimplementedImport& e) {
  char caller[16];
  std::snprintf(caller, sizeof caller, "%08X", e.caller);
  std::string s = "Unimplemented import: " + e.name + "\nCalled from: 0x" + caller + "\nTrail (last " +
                  std::to_string(e.trail.size()) + "): ";
  for (size_t i = 0; i < e.trail.size(); ++i) {
    if (i) s += " -> ";
    s += e.trail[i];
  }
  s += "\n";
  return s;
}

}  // namespace lr::kernel
