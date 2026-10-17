// SPDX-License-Identifier: MIT
// Kernel side of the fake game: registered by name, exactly like kernel code in lr_runtime_core.
// FakeMissing is deliberately not registered.
#include <cstdint>

#include "runtime/kernel/glue.h"
#include "runtime/kernel/registry.h"

namespace lr::kernel::exports {
// Returns its argument + 1 so the guest can check that the thunk passed r3 in and wrote r3 back.
uint32_t FakeImplemented(uint32_t x) { return x + 1; }

// Calls the recompiled guest function at `fn` with `arg` in r3 and returns its r3, like a kernel import that
// runs a guest callback (thread start routine, APC, notification callback).
uint32_t FakeCallback(uint32_t fn, uint32_t arg) {
  const uint64_t args[] = {arg};
  return static_cast<uint32_t>(call_guest(fn, args));
}
}  // namespace lr::kernel::exports

LR_KERNEL(FakeImplemented);
LR_KERNEL(FakeCallback);
LR_KERNEL_STUB(FakeStub, 0);  // the fake guest only needs the success value
