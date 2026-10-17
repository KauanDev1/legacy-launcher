// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace lr::kernel {

// Fixed-layout snapshot of the guest registers relevant to a kernel call. Generated thunks fill it from
// XenonRecomp's PPCContext, so kernel code in lr_runtime_core never depends on the generated config.
struct GuestCall {
  uint8_t* base = nullptr;  // host address of guest address 0
  uint64_t gpr[8] = {};     // r3..r10
  double fpr[13] = {};      // f1..f13
  uint32_t r1 = 0, r13 = 0, lr = 0;
  uint64_t ret_gpr = 0;  // copied back to r3
  double ret_fpr = 0;    // copied back to f1 when has_fpr_return
  bool has_fpr_return = false;
};

using KernelFn = void (*)(GuestCall&);

enum class ImportState : uint8_t { Missing, Stub, Implemented };

}  // namespace lr::kernel
