// SPDX-License-Identifier: MIT
// Hand-written stand-ins for recompiled guest functions.
#include "ppc_recomp_shared.h"

// Calls an implemented import, a stub, then an unimplemented one: the run must stop with exit code 3.
PPC_FUNC(sub_82000100) {
  ctx.lr = 0x82000104;
  __imp__FakeImplemented(ctx, base);
  ctx.lr = 0x82000108;
  __imp__FakeStub(ctx, base);
  ctx.lr = 0x8200010C;
  __imp__FakeMissing(ctx, base);
}

// Calls the implemented import and the stub, and checks what their thunks wrote to r3. Returns normally (exit
// code 0) only when both values are right; otherwise it calls the missing import (exit code 3).
PPC_FUNC(sub_82000180) {
  ctx.r3.u64 = 41;
  ctx.lr = 0x82000184;
  __imp__FakeImplemented(ctx, base);
  const bool impl_ok = ctx.r3.u64 == 42;
  ctx.r3.u64 = 7;
  ctx.lr = 0x82000188;
  __imp__FakeStub(ctx, base);
  const bool stub_ok = ctx.r3.u64 == 0;
  if (!impl_ok || !stub_ok) {
    ctx.lr = 0x8200018C;
    __imp__FakeMissing(ctx, base);
  }
}
