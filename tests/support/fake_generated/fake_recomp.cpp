// SPDX-License-Identifier: MIT
// Hand-written stand-ins for recompiled guest functions.
#include "ppc_recomp_shared.h"

// Calls an implemented import, then an unimplemented one: the run must stop with exit code 3.
PPC_FUNC(sub_82000100) {
  ctx.lr = 0x82000104;
  __imp__FakeImplemented(ctx, base);
  ctx.lr = 0x82000108;
  __imp__FakeMissing(ctx, base);
}

// Only calls the implemented import: the run must finish with exit code 0.
PPC_FUNC(sub_82000180) {
  ctx.lr = 0x82000184;
  __imp__FakeImplemented(ctx, base);
}
