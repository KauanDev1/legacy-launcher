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

// Guest callback run by FakeCallback through call_guest: returns r3 + 1 when it runs on a frame call_guest built
// for it on the main guest thread (r1 16-aligned inside that thread's stack, between the KPCR's stack_end +0x74
// and stack_base +0x70; r13 = the thread's KPCR, whose pcr_ptr +0x30 points at itself), otherwise 0.
PPC_FUNC(sub_82000200) {
  const uint32_t r1 = uint32_t(ctx.r1.u64), r13 = uint32_t(ctx.r13.u64);
  const bool frame_ok = r13 != 0 && PPC_LOAD_U32(r13 + 0x30) == r13 && r1 % 16 == 0 &&
                        r1 < PPC_LOAD_U32(r13 + 0x70) && r1 > PPC_LOAD_U32(r13 + 0x74);
  ctx.r3.u64 = frame_ok ? ctx.r3.u64 + 1 : 0;
}

// Entry point: FakeCallback(sub_82000200, 41) must return 42; otherwise calls the missing import (exit 3).
PPC_FUNC(sub_82000280) {
  ctx.r3.u64 = 0x82000200;
  ctx.r4.u64 = 41;
  ctx.lr = 0x82000284;
  __imp__FakeCallback(ctx, base);
  if (ctx.r3.u64 != 42) {
    ctx.lr = 0x82000288;
    __imp__FakeMissing(ctx, base);
  }
}

// Entry point: FakeCallback on an address inside the code range with no recompiled function (guest fatal, exit 5).
PPC_FUNC(sub_820002C0) {
  ctx.r3.u64 = 0x82000240;
  ctx.r4.u64 = 0;
  ctx.lr = 0x820002C4;
  __imp__FakeCallback(ctx, base);
}

// Entry point: FakeCallback on an address outside the code range (guest fatal, exit 5; the function table must not
// be read for it).
PPC_FUNC(sub_820002E0) {
  ctx.r3.u64 = 0x12345678;
  ctx.r4.u64 = 0;
  ctx.lr = 0x820002E4;
  __imp__FakeCallback(ctx, base);
}

// Guest callback that turns on flush-to-zero like recompiled float code does (fpscr.enableFlushMode) and then does
// inexact / denormal float math. Its fpscr must start from the host FP environment (all SSE exceptions masked): a
// zeroed csr would unmask them and the first inexact result would raise SIGFPE (exit 4). Returns 1 in r3.
PPC_FUNC(sub_82000220) {
  (void)base;
  ctx.fpscr.enableFlushMode();
  volatile float tiny = 1e-40f;
  volatile float one = 1.0f, three = 3.0f;
  volatile float third = one / three;  // inexact at run time
  volatile float r = tiny * third;
  (void)r;
  ctx.r3.u64 = 1;
}

// Entry point: FakeCallback(sub_82000220) must return 1 without SIGFPE, and the host MXCSR this caller had before
// call_guest (no flush-to-zero) must be back afterwards; otherwise calls the missing import (exit 3).
PPC_FUNC(sub_820002A0) {
  const uint32_t before = ctx.fpscr.getcsr();
  ctx.r3.u64 = 0x82000220;
  ctx.r4.u64 = 0;
  ctx.lr = 0x820002A4;
  __imp__FakeCallback(ctx, base);
  if (ctx.r3.u64 != 1 || ctx.fpscr.getcsr() != before) {
    ctx.lr = 0x820002A8;
    __imp__FakeMissing(ctx, base);
  }
}

// ExCreateThread(handle*, 0, id*, 0, start, context, 0) followed by NtWaitForSingleObjectEx(handle) on the result.
// The handle and id slots live in this guest frame (below r1). Returns the wait status in r3, or calls nothing
// more when the creation failed (r3 = its status).
static void create_and_wait(PPCContext& ctx, uint8_t* base, uint32_t start, uint32_t context, uint32_t lr) {
  const uint32_t slots = uint32_t(ctx.r1.u64) - 0x40;
  ctx.r3.u64 = slots;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = slots + 4;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = start;
  ctx.r8.u64 = context;
  ctx.r9.u64 = 0;
  ctx.lr = lr;
  __imp__ExCreateThread(ctx, base);
  if (ctx.r3.u32 != 0) return;
  ctx.r3.u64 = PPC_LOAD_U32(slots);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  ctx.lr = lr + 4;
  __imp__NtWaitForSingleObjectEx(ctx, base);
}

// Secondary thread body: calls the missing import (exit 3, report names thread 2).
PPC_FUNC(sub_82000160) {
  ctx.lr = 0x82000164;
  __imp__FakeMissing(ctx, base);
}

// Secondary thread body: stores context + 1 at guest 0x82000008 and returns.
PPC_FUNC(sub_82000170) {
  PPC_STORE_U32(0x82000008, ctx.r3.u32 + 1);
  ctx.r3.u64 = 0;
}

// Entry point: runs sub_82000160 on a new guest thread and waits for it (never returns: exit 3 from that thread).
PPC_FUNC(sub_820001C0) { create_and_wait(ctx, base, 0x82000160, 0, 0x820001C4); }

// Entry point: runs sub_82000170(41) on a new guest thread, waits, and checks the value it stored (42); a mismatch
// or a failed create/wait calls the missing import (exit 3).
PPC_FUNC(sub_820001E0) {
  create_and_wait(ctx, base, 0x82000170, 41, 0x820001E4);
  if (ctx.r3.u32 != 0 || PPC_LOAD_U32(0x82000008) != 42) {
    ctx.lr = 0x820001EC;
    __imp__FakeMissing(ctx, base);
  }
}
