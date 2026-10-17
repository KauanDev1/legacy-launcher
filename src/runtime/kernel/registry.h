// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/kernel/abi.h"
#include "runtime/kernel/marshal.h"

// Registry of kernel imports: name -> (state, KernelFn). Every import the game references gets a generated
// thunk (lr::pipeline::render_import_thunks, compiled with the game) that looks its name up here once, copies
// the guest registers into a GuestCall and calls the registered function, or calls unimplemented_import() when
// nothing is registered. Kernel code registers with LR_KERNEL / LR_KERNEL_STUB and never sees PPCContext, so it
// can live in the prebuilt lr_runtime_core (linked --whole-archive so the registration statics are kept).

namespace lr::kernel {

// Thrown by a thunk whose import is missing; caught by run_guest. The only exception that crosses runtime internals.
struct UnimplementedImport {
  std::string name;
  uint32_t caller;                 // guest LR at the call
  std::vector<std::string> trail;  // last traced imports, oldest first
};

struct ImportEntry {
  ImportState state = ImportState::Missing;
  KernelFn fn = nullptr;
};

// Registers `fn` for import `name` (state Stub or Implemented, fn non-null). Registering a name twice is a
// programming error: it prints the name and aborts. Safe to call from static initializers.
void register_import(const char* name, ImportState state, KernelFn fn);

// The registered entry, or {Missing, nullptr}.
ImportEntry find_import(std::string_view name);

// Throws UnimplementedImport carrying the calling thread's current trail.
[[noreturn]] void unimplemented_import(const char* name, uint32_t caller_lr);

// Records an import call in a per-thread ring buffer (last 32). Every thunk calls it before dispatching.
void trace(const char* name);

// The calling thread's traced imports (last 32), oldest first.
std::vector<std::string> current_trail();

// "Trail (last N): a -> b\n" — the trail line of the exit-3 report, shared with guest_fatal (exit 5).
std::string format_trail(const std::vector<std::string>& trail);

// "Unimplemented import: <name>\nCalled from: 0x<caller>\n" + format_trail(e.trail)
std::string format_report(const UnimplementedImport& e);

namespace detail {
// KernelFn of LR_KERNEL_STUB: returns the constant V in r3 with marshal's return rules.
template <auto V>
void stub_return(GuestCall& c) {
  write_ret(c, V);
}
}  // namespace detail

}  // namespace lr::kernel

// Registers lr::kernel::exports::Name (a typed function, see marshal.h) as the Implemented import `Name`.
// Use at global scope in a .cpp, after the function is declared:
//   namespace lr::kernel::exports { uint32_t KeGetCurrentProcessType() { return 1; } }
//   LR_KERNEL(KeGetCurrentProcessType);
#define LR_KERNEL(Name)                                                                             \
  [[maybe_unused]] static const bool lr_kernel_registered_##Name =                                  \
      (::lr::kernel::register_import(#Name, ::lr::kernel::ImportState::Implemented,                 \
                                     &::lr::kernel::marshal<&::lr::kernel::exports::Name>),         \
       true)

// Registers import `Name` as a Stub that only returns the constant `value` in r3 (<= 32-bit values are
// sign-extended, as marshal does). Put a comment next to every use saying why a constant is enough, e.g.
//   LR_KERNEL_STUB(XamEnableInactivityProcessing, 0);  // no screen saver to suppress on the host
#define LR_KERNEL_STUB(Name, value)                                                                 \
  [[maybe_unused]] static const bool lr_kernel_registered_##Name =                                  \
      (::lr::kernel::register_import(#Name, ::lr::kernel::ImportState::Stub,                        \
                                     &::lr::kernel::detail::stub_return<(value)>),                  \
       true)
