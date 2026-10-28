// SPDX-License-Identifier: MIT
#pragma once

// Force-included (-include runtime/indirect_call.h) before the generated ppc_config.h / ppc_context.h in every
// translation unit that holds recompiled code: the game template and the fake game. It replaces XenonRecomp's
// PPC_CALL_INDIRECT_FUNC (ppc_context.h only defines it when it is not set yet), which calls whatever the function
// table holds and so jumps to host address 0 for a guest address with no recompiled function.
//
// Fast path: one unsigned range check, one alignment test and the table load. Anything else (outside
// [PPC_CODE_BASE, PPC_CODE_BASE + PPC_CODE_SIZE), unaligned, empty slot) ends the process through
// lr::kernel::bad_indirect_call(target, ctx.lr): the message names the guest target and the return address the
// generated bctrl sequence stored in ctx.lr (exit 4). For a tail `bctr` ctx.lr is the enclosing function's own
// return address. PPC_CODE_BASE/SIZE, PPC_LOOKUP_FUNC, ctx and base resolve at the expansion site.

#include <cstdint>

#include "runtime/kernel/glue.h"

#define PPC_CALL_INDIRECT_FUNC(x)                                                                    \
  do {                                                                                               \
    const uint32_t lr_ind_target_ = uint32_t(x);                                                     \
    PPCFunc* const lr_ind_fn_ =                                                                      \
        (uint32_t(lr_ind_target_ - uint32_t(PPC_CODE_BASE)) < uint32_t(PPC_CODE_SIZE) &&             \
         (lr_ind_target_ & 3u) == 0)                                                                 \
            ? PPC_LOOKUP_FUNC(base, lr_ind_target_)                                                  \
            : nullptr;                                                                               \
    if (__builtin_expect(lr_ind_fn_ == nullptr, 0))                                                  \
      ::lr::kernel::bad_indirect_call(lr_ind_target_, uint32_t(ctx.lr));                             \
    lr_ind_fn_(ctx, base);                                                                           \
  } while (0)
