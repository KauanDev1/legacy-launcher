// SPDX-License-Identifier: MIT
// Hand-written stand-ins for recompiled guest functions.
#include "ppc_recomp_shared.h"

#include <cstdint>
#include <utility>

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
// thread, then VdInitializeEngines with the 4 bytes the worker read in r3 (must return 1) and FakeMissing (exit 3
// from 0x82000328). A failed status calls FakeMissing from 0x82000324.
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
  if (ctx.r3.u32 != 1) fail();
  ctx.lr = 0x82000328;
  __imp__FakeMissing(ctx, base);
  ctx.r1.u64 += kFrame;
}

// Entry point: bctrl to an address inside the code range with no recompiled function (a hole in the function
// table): the forced-include PPC_CALL_INDIRECT_FUNC reports it with exit 4 instead of jumping to host address 0.
PPC_FUNC(sub_820003A0) {
  ctx.ctr.u64 = 0x82000240;
  ctx.lr = 0x820003A4;
  PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

// Entry point: bctrl to an address outside the code range (the function table must not be read for it).
PPC_FUNC(sub_820003B0) {
  ctx.ctr.u64 = 0x12345678;
  ctx.lr = 0x820003B4;
  PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

// Entry point: bctrl to a function that exists (sub_82000180 returns normally) must still reach it: exit 0.
PPC_FUNC(sub_820003A8) {
  ctx.ctr.u64 = 0x82000180;
  ctx.lr = 0x820003AC;
  PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}

// --- Fake GPU boot (sub_82000400 + interrupt callback sub_82000480): ring buffer, write-back, interrupts, VdSwap. ---
namespace gpu_boot {
constexpr uint32_t kFrame = 0x100;
constexpr uint32_t kRingLog2 = 12;                     // 2^(12 + 3) = 32 KiB, like the real game
constexpr uint32_t kRingDwords = 1u << (kRingLog2 + 1);
constexpr uint32_t kWptrMmio = 0x7FC80000 + 4 * 0x1C5;  // CP_RB_WPTR in the MMIO window
constexpr uint32_t kFrames = 3;
// Offsets inside the data block D (physical memory): write-back, interrupt counters, fence, VdSwap arguments.
constexpr uint32_t kWriteBack = 0x00, kVblanks = 0x10, kCpInterrupts = 0x14, kSwapFence = 0x20, kFetch = 0x40,
                   kFrontPtr = 0x60, kWidth = 0x64, kHeight = 0x68;
// The front buffer: its own physical block, kWidth x kHeight linear 32-bit pixels, every one kFrontColor (big-endian
// A8R8G8B8: R 0x20, G 0x40, B 0xC0). A solid colour, so the Vulkan backend's linear copy (Task 17; the tiled
// layout comes with Task 19) shows exactly this colour.
constexpr uint32_t kFbWidth = 1280, kFbHeight = 720, kFrontColor = 0xFF2040C0;
// Synthetic Xenos microcode (host-order dwords, written to the ring big-endian by PPC_STORE_U32): exec_end of one
// ALU slot, `max` exporting c0 to the position (VS) or r0 to colour 0 (PS). The same words the test builder
// (tests/support/xenos_shader_builder.h) assembles; test_gpu_shader_cache.cpp checks they still match.
constexpr uint32_t kShaderDwords = 6;
constexpr uint32_t kVertexShader[kShaderDwords] = {0x00001001, 0x00002000, 0x00000000,
                                                   0xC80F803E, 0x00000000, 0x22000000};
constexpr uint32_t kPixelShader[kShaderDwords] = {0x00001001, 0x00002000, 0x00000000,
                                                  0xC80F8000, 0x00000000, 0xE2000000};
}  // namespace gpu_boot

// Graphics interrupt callback(source, user_data = D + kVblanks): counts vblanks (source 0) at user_data and
// command-processor interrupts (source 1) at user_data + 4. Runs on the kernel worker.
PPC_FUNC(sub_82000480) {
  const uint32_t counter = ctx.r4.u32 + (ctx.r3.u32 == 0 ? 0 : 4);
  PPC_STORE_U32(counter, PPC_LOAD_U32(counter) + 1);
  ctx.r3.u64 = 0;
}

// Entry point: VdInitializeEngines (must be 1); a 32 KiB ring, a data block and the front buffer (filled with
// kFrontColor) from MmAllocatePhysicalMemoryEx and
// their physical addresses (MmGetPhysicalAddress); VdSetGraphicsInterruptCallback(sub_82000480, D + kVblanks),
// VdInitializeRingBuffer(ring, 12), VdEnableRingBufferRPtrWriteBack(D, 6). Then kFrames frames, each with (frame 0
// only) an INTERRUPT packet, two shader loads and a draw, then the 64 dwords VdSwap fills (10 arguments, the last
// two on the guest stack), WPTR published through the MMIO window after each; then EVENT_WRITE_SHD with initiator bit 31 (stores the swap
// counter at D + kSwapFence). It waits (KeDelayExecutionThread 1 ms, at most 5000 times) until the write-back RPTR
// equals WPTR, kFrames vblanks and one CP interrupt arrived, and checks the fence holds kFrames: exit 0. Any
// failure calls FakeMissing from 0x82000420 (exit 3).
namespace gpu_boot {
// The body of sub_82000400 (frames = kFrames) and sub_820004E0 (frames = 0: forever, until the process is ended).
static void run(PPCContext& ctx, uint8_t* base, uint32_t frames) {
  ctx.r1.u64 -= kFrame;
  const uint32_t l = uint32_t(ctx.r1.u64) + 0x60;  // locals: delay interval (be int64)
  bool failed = false;
  const auto fail = [&] {
    if (failed) return;
    failed = true;
    ctx.lr = 0x82000420;
    __imp__FakeMissing(ctx, base);
  };
  const auto alloc_phys = [&](uint32_t size, uint32_t lr) {
    ctx.r3.u64 = 0;
    ctx.r4.u64 = size;
    ctx.r5.u64 = 0x20000004;  // MEM_LARGE_PAGES | PAGE_READWRITE
    ctx.r6.u64 = 0;
    ctx.r7.u64 = 0xFFFFFFFF;
    ctx.r8.u64 = 0;
    ctx.lr = lr;
    __imp__MmAllocatePhysicalMemoryEx(ctx, base);
    const uint32_t guest = ctx.r3.u32;
    if (guest == 0) fail();
    ctx.r3.u64 = guest;
    ctx.lr = lr + 4;
    __imp__MmGetPhysicalAddress(ctx, base);
    return std::pair<uint32_t, uint32_t>{guest, ctx.r3.u32};
  };

  ctx.r3.u64 = 0;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  ctx.lr = 0x82000404;
  __imp__VdInitializeEngines(ctx, base);
  if (ctx.r3.u32 != 1) fail();

  const auto [ring, ring_phys] = alloc_phys(kRingDwords * 4, 0x82000408);
  const auto [d, d_phys] = alloc_phys(0x10000, 0x82000410);
  const auto [fb, fb_phys] = alloc_phys(kFbWidth * kFbHeight * 4, 0x82000430);
  for (uint32_t i = 0; i < kFbWidth * kFbHeight; ++i) PPC_STORE_U32(fb + i * 4, kFrontColor);

  ctx.r3.u64 = 0x82000480;
  ctx.r4.u64 = d + kVblanks;
  ctx.lr = 0x82000418;
  __imp__VdSetGraphicsInterruptCallback(ctx, base);
  ctx.r3.u64 = ring_phys;
  ctx.r4.u64 = kRingLog2;
  ctx.lr = 0x8200041C;
  __imp__VdInitializeRingBuffer(ctx, base);
  ctx.r3.u64 = d_phys + kWriteBack;
  ctx.r4.u64 = 6;
  ctx.lr = 0x82000424;
  __imp__VdEnableRingBufferRPtrWriteBack(ctx, base);

  PPC_STORE_U32(d + kFetch + 4, fb);  // fetch constant base address = the front buffer (no warning)
  PPC_STORE_U32(d + kFrontPtr, fb);
  PPC_STORE_U32(d + kWidth, kFbWidth);
  PPC_STORE_U32(d + kHeight, kFbHeight);
  uint32_t wptr = 0;
  const auto put = [&](uint32_t v) {
    PPC_STORE_U32(ring + wptr * 4, v);
    wptr = (wptr + 1) % kRingDwords;
  };
  PPC_STORE_U64(l, uint64_t(-10000));  // 1 ms, relative
  const auto delay = [&](uint32_t lr) {
    ctx.r3.u64 = 1;  // UserMode
    ctx.r4.u64 = 0;  // not alertable
    ctx.r5.u64 = l;
    ctx.lr = lr;
    __imp__KeDelayExecutionThread(ctx, base);
  };
  for (uint32_t frame = 0; frames == 0 || frame < frames; ++frame) {
    // Endless mode: the ring holds one frame at a time, so wait until the GPU consumed the previous one.
    while (frames == 0 && PPC_LOAD_U32(d + kWriteBack) != wptr) delay(0x82000434);
    if (frame == 0) {
      put((3u << 30) | (0u << 16) | (0x54u << 8));  // INTERRUPT, 1 payload dword
      put(1);                                       // CPU mask
      // A vertex and a pixel shader through IM_LOAD_IMMEDIATE, then one auto-index triangle draw (edram_mode 4):
      // the runtime translates both, so the exit summary counts 2 keys.
      for (uint32_t stage = 0; stage < 2; ++stage) {
        put((3u << 30) | ((2u + kShaderDwords - 1) << 16) | (0x2Bu << 8));  // IM_LOAD_IMMEDIATE
        put(stage);                                                          // 0 vertex, 1 pixel
        put(kShaderDwords);                                                  // start 0, size
        for (uint32_t w : stage == 0 ? kVertexShader : kPixelShader) put(w);
      }
      put(0x2208);  // type 0, one register: RB_MODECONTROL
      put(4);       // edram_mode colour+depth
      put((3u << 30) | (0u << 16) | (0x36u << 8));  // DRAW_INDX_2, 1 payload dword
      put(4u | (2u << 6) | (3u << 16));              // triangle list, auto index, 3 vertices
    }
    const uint32_t area = ring + wptr * 4;
    ctx.r3.u64 = area;
    ctx.r4.u64 = d + kFetch;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = 0;
    ctx.r7.u64 = 0;
    ctx.r8.u64 = d + kFrontPtr;
    ctx.r9.u64 = 0;
    ctx.r10.u64 = 0;
    // Ninth and tenth arguments: low halves of the parameter-area slots at r1 + 0x50 and r1 + 0x58.
    const uint32_t params = uint32_t(ctx.r1.u64) + 0x50;
    PPC_STORE_U32(params + 0x4, d + kWidth);
    PPC_STORE_U32(params + 0xC, d + kHeight);
    ctx.lr = 0x82000428;
    __imp__VdSwap(ctx, base);
    wptr = (wptr + 64) % kRingDwords;  // VdSwap filled the 64 reserved dwords
    PPC_STORE_U32(kWptrMmio, wptr);
  }
  put((3u << 30) | (2u << 16) | (0x58u << 8));  // EVENT_WRITE_SHD, 3 payload dwords
  put(0x80000000u);                              // initiator bit 31: write the swap counter
  put((d_phys + kSwapFence) | 2);                // 8-in-32 endian: lands big-endian
  put(0);
  PPC_STORE_U32(kWptrMmio, wptr);

  for (int i = 0; i < 5000; ++i) {
    if (PPC_LOAD_U32(d + kWriteBack) == wptr && PPC_LOAD_U32(d + kVblanks) >= kFrames &&
        PPC_LOAD_U32(d + kCpInterrupts) >= 1)
      break;
    delay(0x8200042C);
  }
  if (PPC_LOAD_U32(d + kWriteBack) != wptr || PPC_LOAD_U32(d + kVblanks) < kFrames ||
      PPC_LOAD_U32(d + kCpInterrupts) < 1 || PPC_LOAD_U32(d + kSwapFence) != kFrames)
    fail();
  ctx.r1.u64 += kFrame;
}
}  // namespace gpu_boot

PPC_FUNC(sub_82000400) { gpu_boot::run(ctx, base, gpu_boot::kFrames); }

// The same GPU boot, swapping the solid front buffer forever (the SIGTERM test ends it).
PPC_FUNC(sub_820004E0) { gpu_boot::run(ctx, base, 0); }
