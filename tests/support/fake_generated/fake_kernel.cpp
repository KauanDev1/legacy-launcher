// SPDX-License-Identifier: GPL-3.0-or-later
#include "ppc_recomp_shared.h"
#include "runtime/kernel/registry.h"

LR_KERNEL_EXPORT(FakeImplemented) {
  (void)base;
  lr::kernel::trace("FakeImplemented");
  ctx.r3.u32 = 0;
}
