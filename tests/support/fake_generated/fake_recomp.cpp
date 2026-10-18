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

// Second thread of the deadlock pair: KeWaitForSingleObject(main thread's KTHREAD, 0, 0, non-alertable, no
// timeout), i.e. an infinite wait on the thread that is itself waiting for this one.
PPC_FUNC(sub_82000360) {
  ctx.r3.u64 = PPC_LOAD_U32(0x82000010);
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = 0;
  ctx.lr = 0x82000364;
  __imp__KeWaitForSingleObject(ctx, base);
}

// Entry point: a two-thread deadlock. Stores this thread's KTHREAD (KPCR +0x100) at guest 0x82000010, starts
// sub_82000360 (which waits on that KTHREAD forever) and waits for it forever: the deadlock detector ends the run
// with exit 6.
PPC_FUNC(sub_82000340) {
  PPC_STORE_U32(0x82000010, PPC_LOAD_U32(uint32_t(ctx.r13.u64) + 0x100));
  create_and_wait(ctx, base, 0x82000360, 0, 0x82000344);
}

// --- Fake game boot (sub_82000300 + worker sub_82000380): threads, files and waits through the thunks only. ---
// Offsets inside the scratch block sub_82000300 allocates and hands to the worker as its start context.
namespace boot {
constexpr uint32_t kCritSec = 0x00;  // RTL_CRITICAL_SECTION (0x1C bytes)
constexpr uint32_t kEvent = 0x20;    // event handle (written by thread 1)
constexpr uint32_t kData = 0x24;     // the 4 bytes read from game:\data.bin
constexpr uint32_t kFile = 0x30;     // file handle
constexpr uint32_t kIosb = 0x38;     // X_IO_STATUS_BLOCK
constexpr uint32_t kAttrs = 0x40;    // X_OBJECT_ATTRIBUTES
constexpr uint32_t kName = 0x50;     // X_ANSI_STRING
constexpr uint32_t kPath = 0x60;     // path characters
constexpr uint32_t kOffset = 0x80;   // byte offset (be int64)
constexpr uint32_t kFrame = 0x100;   // guest stack frame of each function; locals at r1 + 0x60
}  // namespace boot

// Worker (thread 2, r3 = scratch block): NtCreateFile(game:\data.bin) with its ninth argument (create options) in
// the caller's parameter area at r1 + 0x54, NtReadFile of 4 bytes at offset 0 into the block, NtSetEvent on the
// event thread 1 waits on. A failed status (or a short read) calls FakeMissing from 0x820003A0.
PPC_FUNC(sub_82000380) {
  using namespace boot;
  const uint32_t s = ctx.r3.u32;
  ctx.r1.u64 -= kFrame;
  const auto fail = [&] {
    ctx.lr = 0x820003A0;
    __imp__FakeMissing(ctx, base);
  };
  static constexpr char kPathText[] = "game:\\data.bin";
  constexpr uint16_t kPathLen = sizeof(kPathText) - 1;
  for (uint32_t i = 0; i < kPathLen; ++i) PPC_STORE_U8(s + kPath + i, uint8_t(kPathText[i]));
  PPC_STORE_U16(s + kName + 0, kPathLen);      // length
  PPC_STORE_U16(s + kName + 2, kPathLen + 1);  // maximum_length
  PPC_STORE_U32(s + kName + 4, s + kPath);     // buffer
  PPC_STORE_U32(s + kAttrs + 0, 0);            // root_directory
  PPC_STORE_U32(s + kAttrs + 4, s + kName);    // name_ptr
  PPC_STORE_U32(s + kAttrs + 8, 0x40);         // OBJ_CASE_INSENSITIVE

  ctx.r3.u64 = s + kFile;
  ctx.r4.u64 = 0x80100000;  // GENERIC_READ | SYNCHRONIZE
  ctx.r5.u64 = s + kAttrs;
  ctx.r6.u64 = s + kIosb;
  ctx.r7.u64 = 0;           // allocation_size*
  ctx.r8.u64 = 0x80;        // FILE_ATTRIBUTE_NORMAL
  ctx.r9.u64 = 1;           // FILE_SHARE_READ
  ctx.r10.u64 = 1;          // FILE_OPEN
  // Ninth argument: the low half of slot 0 of the parameter area, r1 + 0x54. The neighbouring words (high half at
  // +0x50, the next slot's low half at +0x5C) hold FILE_DIRECTORY_FILE, which fails on a file with
  // STATUS_NOT_A_DIRECTORY: a thunk reading the wrong word cannot pass.
  const uint32_t params = uint32_t(ctx.r1.u64) + 0x50;
  PPC_STORE_U32(params + 0x0, 0x01);  // FILE_DIRECTORY_FILE (sentinel)
  PPC_STORE_U32(params + 0x4, 0x60);  // FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE
  PPC_STORE_U32(params + 0x8, 0x01);  // sentinel
  PPC_STORE_U32(params + 0xC, 0x01);  // sentinel
  ctx.lr = 0x82000384;
  __imp__NtCreateFile(ctx, base);
  if (ctx.r3.u32 != 0) fail();

  PPC_STORE_U64(s + kOffset, 0);
  ctx.r3.u64 = PPC_LOAD_U32(s + kFile);
  ctx.r4.u64 = 0;  // event
  ctx.r5.u64 = 0;  // apc_routine
  ctx.r6.u64 = 0;  // apc_context
  ctx.r7.u64 = s + kIosb;
  ctx.r8.u64 = s + kData;
  ctx.r9.u64 = 4;
  ctx.r10.u64 = s + kOffset;
  ctx.lr = 0x82000388;
  __imp__NtReadFile(ctx, base);
  if (ctx.r3.u32 != 0 || PPC_LOAD_U32(s + kIosb + 4) != 4) fail();

  ctx.r3.u64 = PPC_LOAD_U32(s + kEvent);
  ctx.r4.u64 = 0;  // previous_state*
  ctx.lr = 0x8200038C;
  __imp__NtSetEvent(ctx, base);
  if (ctx.r3.u32 != 0) fail();

  ctx.r1.u64 += kFrame;
  ctx.r3.u64 = 0;
}

// Entry point: boots like a game. NtAllocateVirtualMemory(64 KiB scratch block), RtlInitializeCriticalSection in
// it, NtCreateEvent (notification, not signaled), ExCreateThread(sub_82000380, block), ObReferenceObjectByHandle
// on the event (any type) and KeWaitForSingleObject on the object it returns, NtWaitForSingleObjectEx on the worker
// thread, then VdInitializeEngines with the 4 bytes the worker read in r3 (missing: exit 3 from 0x82000320). A
// failed status calls FakeMissing from 0x82000324.
PPC_FUNC(sub_82000300) {
  using namespace boot;
  ctx.r1.u64 -= kFrame;
  const uint32_t l = uint32_t(ctx.r1.u64) + 0x60;  // locals: base, size, event, thread, thread id, event object
  const auto fail = [&] {
    ctx.lr = 0x82000324;
    __imp__FakeMissing(ctx, base);
  };
  PPC_STORE_U32(l + 0x00, 0);  // anywhere
  PPC_STORE_U32(l + 0x04, 0x10000);
  ctx.r3.u64 = l + 0x00;
  ctx.r4.u64 = l + 0x04;
  ctx.r5.u64 = 0x3000;  // MEM_COMMIT | MEM_RESERVE
  ctx.r6.u64 = 0x04;    // PAGE_READWRITE
  ctx.r7.u64 = 0;
  ctx.lr = 0x82000304;
  __imp__NtAllocateVirtualMemory(ctx, base);
  if (ctx.r3.u32 != 0) fail();
  const uint32_t s = PPC_LOAD_U32(l + 0x00);

  ctx.r3.u64 = s + kCritSec;
  ctx.lr = 0x82000308;
  __imp__RtlInitializeCriticalSection(ctx, base);

  ctx.r3.u64 = l + 0x08;
  ctx.r4.u64 = 0;  // object_attributes
  ctx.r5.u64 = 0;  // NotificationEvent
  ctx.r6.u64 = 0;  // not signaled
  ctx.lr = 0x8200030C;
  __imp__NtCreateEvent(ctx, base);
  if (ctx.r3.u32 != 0) fail();
  PPC_STORE_U32(s + kEvent, PPC_LOAD_U32(l + 0x08));

  ctx.r3.u64 = l + 0x0C;
  ctx.r4.u64 = 0;  // default stack size
  ctx.r5.u64 = l + 0x10;
  ctx.r6.u64 = 0;  // xapi_startup
  ctx.r7.u64 = 0x82000380;
  ctx.r8.u64 = s;
  ctx.r9.u64 = 0;  // flags: start now
  ctx.lr = 0x82000310;
  __imp__ExCreateThread(ctx, base);
  if (ctx.r3.u32 != 0) fail();

  ctx.r3.u64 = PPC_LOAD_U32(l + 0x08);
  ctx.r4.u64 = 0;  // any object type
  ctx.r5.u64 = l + 0x14;
  ctx.lr = 0x82000314;
  __imp__ObReferenceObjectByHandle(ctx, base);
  if (ctx.r3.u32 != 0) fail();

  ctx.r3.u64 = PPC_LOAD_U32(l + 0x14);
  ctx.r4.u64 = 0;  // Executive
  ctx.r5.u64 = 0;  // KernelMode
  ctx.r6.u64 = 0;  // not alertable
  ctx.r7.u64 = 0;  // no timeout
  ctx.lr = 0x82000318;
  __imp__KeWaitForSingleObject(ctx, base);
  if (ctx.r3.u32 != 0) fail();

  // Then the worker thread itself: once it has ended, every import it called has been traced, so the trace is the
  // same on every run.
  ctx.r3.u64 = PPC_LOAD_U32(l + 0x0C);
  ctx.r4.u64 = 0;  // KernelMode
  ctx.r5.u64 = 0;  // not alertable
  ctx.r6.u64 = 0;  // no timeout
  ctx.lr = 0x8200031C;
  __imp__NtWaitForSingleObjectEx(ctx, base);
  if (ctx.r3.u32 != 0) fail();

  ctx.r3.u64 = PPC_LOAD_U32(s + kData);
  ctx.lr = 0x82000320;
  __imp__VdInitializeEngines(ctx, base);
  ctx.r1.u64 += kFrame;
}
