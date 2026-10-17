// SPDX-License-Identifier: MIT
#include "runtime/kernel/registry.h"

#include <array>
#include <atomic>
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

std::atomic<void (*)()> g_entry_hook{nullptr};

// Function-local statics: LR_KERNEL / LR_KERNEL_STUB register from static initializers in other TUs. Both are
// intentionally leaked (never destroyed), so a thread still calling a thunk while the process exits never touches
// destroyed storage.
std::mutex& table_mutex() {
  static auto& m = *new std::mutex;
  return m;
}
std::map<std::string, ImportEntry, std::less<>>& table() {
  static auto& t = *new std::map<std::string, ImportEntry, std::less<>>;
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
  if (auto* hook = g_entry_hook.load(std::memory_order_acquire)) hook();
}

void set_import_entry_hook(void (*hook)()) { g_entry_hook.store(hook, std::memory_order_release); }

std::vector<std::string> current_trail() {
  std::vector<std::string> out;
  const size_t n = t_trail.count < kTrailSize ? t_trail.count : kTrailSize;
  for (size_t i = t_trail.count - n; i < t_trail.count; ++i) out.emplace_back(t_trail.names[i % kTrailSize]);
  return out;
}

void unimplemented_import(const char* name, uint32_t caller_lr) {
  throw UnimplementedImport{name, caller_lr, current_trail()};
}

std::string format_trail(const std::vector<std::string>& trail) {
  std::string s = "Trail (last " + std::to_string(trail.size()) + "): ";
  for (size_t i = 0; i < trail.size(); ++i) {
    if (i) s += " -> ";
    s += trail[i];
  }
  s += "\n";
  return s;
}

std::string format_report(const UnimplementedImport& e) {
  char caller[16];
  std::snprintf(caller, sizeof caller, "%08X", e.caller);
  return "Unimplemented import: " + e.name + "\nCalled from: 0x" + caller + "\n" + format_trail(e.trail);
}

std::string format_report(const UnimplementedImport& e, uint32_t thread_id) {
  char caller[16];
  std::snprintf(caller, sizeof caller, "%08X", e.caller);
  return "Unimplemented import: " + e.name + "\nCalled from: 0x" + caller + "\nThread: " + std::to_string(thread_id) +
         "\n" + format_trail(e.trail);
}

}  // namespace lr::kernel
