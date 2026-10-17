// SPDX-License-Identifier: MIT
// Kernel side of the fake game: registered by name, exactly like kernel code in lr_runtime_core.
// FakeMissing is deliberately not registered.
#include "runtime/kernel/registry.h"

namespace lr::kernel::exports {
// Returns its argument + 1 so the guest can check that the thunk passed r3 in and wrote r3 back.
uint32_t FakeImplemented(uint32_t x) { return x + 1; }
}  // namespace lr::kernel::exports

LR_KERNEL(FakeImplemented);
LR_KERNEL_STUB(FakeStub, 0);  // the fake guest only needs the success value
