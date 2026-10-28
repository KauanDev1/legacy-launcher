// SPDX-License-Identifier: MIT
// Hand-written stand-ins for recompiled guest functions.
#include "ppc_recomp_shared.h"

#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "runtime/gpu/texture_format.h"
#include "support/xenos_shader_builder.h"
#include "support/xenos_tiling.h"

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

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
// The front buffer: its own physical block holding kFbWidth x kFbHeight 32-bit pixels in the Xenos 2D tiled layout
// (rows allocated up to a multiple of 32, as D3D does), described by the fetch constant VdSwap gets: k_8_8_8_8,
// endian 8in32, tiled, pitch 1280, swizzle ARGB (r = Z, g = Y, b = X, a = W: D3D's A8R8G8B8). Pixel (x, y) is the
// big-endian word 0xFF000000 | (x & 0xFF) << 16 | (y & 0xFF) << 8 | 0xC0, so a screenshot shows R = x & 0xFF,
// G = y & 0xFF, B = 0xC0 only when the front buffer is untiled correctly (Task 19).
constexpr uint32_t kFbWidth = 1280, kFbHeight = 720, kFbAllocHeight = 736;
constexpr uint32_t front_pixel(uint32_t x, uint32_t y) { return 0xFF000000u | ((x & 0xFFu) << 16) | ((y & 0xFFu) << 8) | 0xC0u; }
// Fetch constant dwords 0, 2, 3 and 5 (dword 1 = format | endian << 6 | base address, written with the address).
constexpr uint32_t kFetch0 = 2u | ((kFbWidth / 32u) << 22) | (1u << 31);  // texture, pitch 1280, tiled
constexpr uint32_t kFetch1Format = 6u | (2u << 6);                          // k_8_8_8_8, 8in32
constexpr uint32_t kFetch2 = (kFbWidth - 1) | ((kFbHeight - 1) << 13);
constexpr uint32_t kFetch3 = (2u | (1u << 3) | (0u << 6) | (3u << 9)) << 1;  // swizzle ZYXW
constexpr uint32_t kFetch5 = 1u << 9;                                        // 2D
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

// Entry point: VdInitializeEngines (must be 1); a 32 KiB ring, a data block and the front buffer (filled with the
// tiled front_pixel pattern) from MmAllocatePhysicalMemoryEx and
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
  const auto [fb, fb_phys] = alloc_phys(kFbWidth * kFbAllocHeight * 4, 0x82000430);
  for (uint32_t y = 0; y < kFbHeight; ++y)
    for (uint32_t x = 0; x < kFbWidth; ++x)
      PPC_STORE_U32(fb + lr::gpu::tiled_offset_2d(x, y, kFbWidth, 4), front_pixel(x, y));

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

  PPC_STORE_U32(d + kFetch + 0, kFetch0);
  PPC_STORE_U32(d + kFetch + 4, fb | kFetch1Format);  // base address = the front buffer (no warning)
  PPC_STORE_U32(d + kFetch + 8, kFetch2);
  PPC_STORE_U32(d + kFetch + 12, kFetch3);
  PPC_STORE_U32(d + kFetch + 16, 0);
  PPC_STORE_U32(d + kFetch + 20, kFetch5);
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

// --- Fake textured quad (sub_820004A0, Task 21): the whole renderer end to end, driven the way D3D drives it. ---
namespace textured_quad {
namespace x = lr::testing::xenos;
namespace g = lr::gpu;
using gpu_boot::kRingDwords;
using gpu_boot::kRingLog2;
using gpu_boot::kWptrMmio;
// The frame: a kW x kH k_8_8_8_8 render target at EDRAM tile 0 and its tiled A8R8G8B8 front buffer.
constexpr uint32_t kW = 512, kH = 320;
constexpr uint32_t kClear = 0x40404040;  // grey: the same in every channel order
// The texture: kTexW x kTexH DXT1 (4x4 blocks of 8 bytes), tiled, 8in16, drawn 1:1 with its top-left texel at pixel
// (kQuadX0, kQuadY0). Block rows are allocated up to a multiple of 32 (the tiled layout's macro tile).
constexpr uint32_t kTexW = 256, kTexH = 160, kTexBlocksW = kTexW / 4, kTexBlocksH = kTexH / 4, kTexAllocRows = 64;
constexpr uint32_t kQuadX0 = 128, kQuadY0 = 80;
constexpr uint32_t kSlot = 3;  // the quad's vertex fetch constant (registers SHADER_CONSTANT_FETCH_00_0 + 6, + 7)
// Offsets in the data block D (physical memory).
constexpr uint32_t kWriteBack = 0x00, kVblanks = 0x10, kSwapFetch = 0x40, kFrontPtr = 0x60, kWidth = 0x64,
                   kHeight = 0x68, kQuad = 0x100, kRect = 0x200, kVs = 0x400, kPs = 0x800;

// Block (bx, by) is one solid colour, unique per block: R5 = bx & 31, G6 = by, B5 = (bx >> 5) << 4 | 8 (never 0).
constexpr uint16_t block_color(uint32_t bx, uint32_t by) {
  return uint16_t(((bx & 31u) << 11) | ((by & 63u) << 5) | (((bx >> 5) << 4) | 8u));
}

x::Alu mov(uint32_t dest, uint32_t reg) {
  x::Alu a;
  a.vector_op = x::kVectorMax;
  a.vector_dest = dest;
  a.export_data = true;
  a.src1 = {reg, true, 0};
  a.src2 = {reg, true, 0};
  return a;
}

// VS: r1 = float4 position, r2 = float4 texture coordinates from vertex fetch constant kSlot (8 dwords per vertex,
// index r0.x); exports the position and interpolator 0 = r2.
std::vector<uint32_t> vertex_shader() {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 4, 0b0101), x::cf_nop()));
  x::emit(code, x::vfetch(1, 0, kSlot, 0, 8, x::kFormat32_32_32_32Float));
  x::emit(code, x::vfetch_mini(2, 4, x::kFormat32_32_32_32Float));
  x::emit(code, x::alu(mov(x::kExportVsPosition, 1)));
  x::emit(code, x::alu(mov(x::kExportVsInterpolator0, 2)));
  return code;
}

// PS: r1 = tfetch 2D of texture fetch constant 0 at r0 (interpolator 0); oC0 = r1.
std::vector<uint32_t> pixel_shader() {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 2, 0b0001), x::cf_nop()));
  x::emit(code, x::tfetch(1, 0, 0, x::kTextureDim2D));
  x::emit(code, x::alu(mov(x::kExportPsColor0, 1)));
  return code;
}

uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }

static void run(PPCContext& ctx, uint8_t* base) {
  ctx.r1.u64 -= gpu_boot::kFrame;
  const uint32_t l = uint32_t(ctx.r1.u64) + 0x60;  // locals: delay interval (be int64)
  bool failed = false;
  const auto fail = [&] {
    if (failed) return;
    failed = true;
    ctx.lr = 0x820004A4;
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
  ctx.lr = 0x820004A8;
  __imp__VdInitializeEngines(ctx, base);
  if (ctx.r3.u32 != 1) fail();

  const auto [ring, ring_phys] = alloc_phys(kRingDwords * 4, 0x820004AC);
  const auto [d, d_phys] = alloc_phys(0x10000, 0x820004B4);
  const auto [fb, fb_phys] = alloc_phys(kW * kH * 4, 0x820004BC);  // kH is a multiple of 32
  const auto [tex, tex_phys] = alloc_phys(kTexBlocksW * kTexAllocRows * 8, 0x820004E4);

  // The texture, as the guest CPU writes a DXT1 surface for an 8in16 fetch constant: each 16-bit half of a block
  // big-endian (colour 0, colour 1 = 0, the 32 index bits all 0: every texel is colour 0), each block at its Xenos
  // 2D tiled offset (pitch in blocks, 8 bytes per block) from the test reference, not the runtime's tiling.
  for (uint32_t by = 0; by < kTexBlocksH; ++by) {
    for (uint32_t bx = 0; bx < kTexBlocksW; ++bx) {
      const uint32_t at = tex + lr::testing::xenos_tiled_offset_2d(bx, by, kTexBlocksW, 3);
      PPC_STORE_U16(at + 0, block_color(bx, by));
      PPC_STORE_U16(at + 2, 0);
      PPC_STORE_U16(at + 4, 0);
      PPC_STORE_U16(at + 6, 0);
    }
  }
  // The quad (clip space: the kTexW x kTexH rectangle at (kQuadX0, kQuadY0) of the frame) as a quad list TL, TR, BR,
  // BL: float4 position, float4 texture coordinates (u right, v down), big-endian.
  const float qx0 = float(kQuadX0) / (kW / 2.0f) - 1.0f, qx1 = float(kQuadX0 + kTexW) / (kW / 2.0f) - 1.0f;
  const float qy0 = 1.0f - float(kQuadY0) / (kH / 2.0f), qy1 = 1.0f - float(kQuadY0 + kTexH) / (kH / 2.0f);
  const float quad[4][8] = {{qx0, qy0, 0, 1, 0, 0, 0, 0},
                            {qx1, qy0, 0, 1, 1, 0, 0, 0},
                            {qx1, qy1, 0, 1, 1, 1, 0, 0},
                            {qx0, qy1, 0, 1, 0, 1, 0, 0}};
  for (uint32_t v = 0; v < 4; ++v)
    for (uint32_t c = 0; c < 8; ++c) PPC_STORE_U32(d + kQuad + 4 * (8 * v + c), f2u(quad[v][c]));
  // The resolve rectangle, as D3D draws it: three k_32_32_FLOAT vertices in pixels, in vertex fetch constant 0.
  const float rect[6] = {0, 0, float(kW), 0, float(kW), float(kH)};
  for (uint32_t i = 0; i < 6; ++i) PPC_STORE_U32(d + kRect + 4 * i, f2u(rect[i]));
  // The microcode, big-endian, for IM_LOAD.
  const std::vector<uint32_t> vs = vertex_shader(), ps = pixel_shader();
  for (size_t i = 0; i < vs.size(); ++i) PPC_STORE_U32(d + kVs + uint32_t(4 * i), vs[i]);
  for (size_t i = 0; i < ps.size(); ++i) PPC_STORE_U32(d + kPs + uint32_t(4 * i), ps[i]);

  ctx.r3.u64 = 0x82000480;
  ctx.r4.u64 = d + kVblanks;
  ctx.lr = 0x820004EC;
  __imp__VdSetGraphicsInterruptCallback(ctx, base);
  ctx.r3.u64 = ring_phys;
  ctx.r4.u64 = kRingLog2;
  ctx.lr = 0x820004F0;
  __imp__VdInitializeRingBuffer(ctx, base);
  ctx.r3.u64 = d_phys + kWriteBack;
  ctx.r4.u64 = 6;
  ctx.lr = 0x820004F4;
  __imp__VdEnableRingBufferRPtrWriteBack(ctx, base);

  uint32_t wptr = 0;
  const auto put = [&](uint32_t v) {
    PPC_STORE_U32(ring + wptr * 4, v);
    wptr = (wptr + 1) % kRingDwords;
  };
  const auto type0 = [&](uint32_t reg, std::initializer_list<uint32_t> values) {
    put((uint32_t(values.size() - 1) << 16) | reg);
    for (uint32_t v : values) put(v);
  };
  const auto type3 = [&](uint32_t opcode, std::initializer_list<uint32_t> payload) {
    put((3u << 30) | (uint32_t(payload.size() - 1) << 16) | (opcode << 8));
    for (uint32_t v : payload) put(v);
  };
  // SET_CONSTANT type 1 (fetch constants), starting at register SHADER_CONSTANT_FETCH_00_0 + offset.
  const auto set_fetch = [&](uint32_t offset, std::initializer_list<uint32_t> values) {
    put((3u << 30) | (uint32_t(values.size()) << 16) | (uint32_t(g::pm4::SET_CONSTANT) << 8));
    put((1u << 16) | offset);
    for (uint32_t v : values) put(v);
  };
  const uint32_t rect_fetch0 = (d_phys + kRect) | 3u, rect_fetch1 = g::pm4::kEndian8in32 | (6u << 2);
  const uint32_t rect_draw = 8u | (2u << 6) | (3u << 16);  // rect list, auto index, 3 vertices

  // Render state: the colour target, no depth test, no blending, the D3D viewport mapping clip [-1, 1] onto the
  // frame (y down) with pixel centres at .5 (pix_center 1), the scissors on the whole frame, every index in range.
  type0(g::RB_SURFACE_INFO, {kW, 0u /* RB_COLOR_INFO: tile 0, k_8_8_8_8 */});
  type0(g::RB_COLOR_MASK, {0xF});
  type0(g::RB_DEPTHCONTROL, {0, 0x00010001u /* RB_BLENDCONTROL0: one, zero, add */, 0 /* RB_COLORCONTROL */});
  type0(g::PA_SC_WINDOW_SCISSOR_TL, {1u << 31, kW | (kH << 16)});
  type0(g::PA_SC_SCREEN_SCISSOR_TL, {0, kW | (kH << 16)});
  type0(g::PA_CL_VTE_CNTL, {0x3Fu | (1u << 10)});
  type0(g::PA_CL_VPORT_XSCALE,
        {f2u(kW / 2.0f), f2u(kW / 2.0f), f2u(-(kH / 2.0f)), f2u(kH / 2.0f), f2u(1.0f), f2u(0.0f)});
  type0(g::PA_CL_CLIP_CNTL, {1u << 19});
  type0(g::PA_SU_VTX_CNTL, {1});
  type0(g::PA_SU_SC_MODE_CNTL, {0});
  type0(g::VGT_MAX_VTX_INDX, {0xFFFFFF, 0, 0});
  // Clear the colour target to kClear: a resolve that only clears (copy_command 3, color_clear_enable).
  set_fetch(0, {rect_fetch0, rect_fetch1});
  type0(g::RB_COPY_CONTROL, {(1u << 8) | (3u << 20)});
  type0(g::RB_COLOR_CLEAR, {kClear});
  type0(g::RB_MODECONTROL, {6});
  type3(g::pm4::DRAW_INDX, {0 /* viz query token */, rect_draw});
  // The shaders, from physical memory.
  type3(g::pm4::IM_LOAD, {(d_phys + kVs) | 0u /* vertex */, uint32_t(vs.size())});
  type3(g::pm4::IM_LOAD, {(d_phys + kPs) | 1u /* pixel */, uint32_t(ps.size())});
  // Texture fetch constant 0: DXT1, 8in16, tiled, pitch kTexW, kTexW x kTexH, swizzle XYZW, point filters, clamp to
  // edge, 2D. Then vertex fetch constant kSlot: the 32 quad dwords, 8in32.
  set_fetch(0, {2u | (2u << 10) | (2u << 13) | (2u << 16) | ((kTexW / 32u) << 22) | (1u << 31),
                uint32_t(g::XenosFormat::k_DXT1) | (g::pm4::kEndian8in16 << 6) | tex_phys,
                (kTexW - 1) | ((kTexH - 1) << 13), (0u | (1u << 3) | (2u << 6) | (3u << 9)) << 1, 0, 1u << 9});
  set_fetch(2 * kSlot, {(d_phys + kQuad) | 3u, g::pm4::kEndian8in32 | (32u << 2)});
  // The quad: colour+depth, quad list, auto index, 4 vertices.
  type0(g::RB_MODECONTROL, {4});
  type3(g::pm4::DRAW_INDX_2, {0x0Du | (2u << 6) | (4u << 16)});
  // Resolve colour target 0 into the front buffer as D3D's A8R8G8B8: k_8_8_8_8 with red/blue swapped, 8in32 (the
  // rectangle's fetch constant overwrites the texture's first two dwords, as in D3D).
  set_fetch(0, {rect_fetch0, rect_fetch1});
  type0(g::RB_COPY_CONTROL, {0, fb_phys, kW | (kH << 16), g::pm4::kEndian8in32 | (6u << 7) | (1u << 24)});
  type0(g::RB_MODECONTROL, {6});
  type3(g::pm4::DRAW_INDX, {0, rect_draw});

  // VdSwap of the front buffer: fetch constant k_8_8_8_8, 8in32, tiled, pitch kW, swizzle ZYXW (A8R8G8B8), 2D.
  PPC_STORE_U32(d + kSwapFetch + 0, 2u | ((kW / 32u) << 22) | (1u << 31));
  PPC_STORE_U32(d + kSwapFetch + 4, fb | gpu_boot::kFetch1Format);
  PPC_STORE_U32(d + kSwapFetch + 8, (kW - 1) | ((kH - 1) << 13));
  PPC_STORE_U32(d + kSwapFetch + 12, gpu_boot::kFetch3);
  PPC_STORE_U32(d + kSwapFetch + 16, 0);
  PPC_STORE_U32(d + kSwapFetch + 20, gpu_boot::kFetch5);
  PPC_STORE_U32(d + kFrontPtr, fb);
  PPC_STORE_U32(d + kWidth, kW);
  PPC_STORE_U32(d + kHeight, kH);
  ctx.r3.u64 = ring + wptr * 4;
  ctx.r4.u64 = d + kSwapFetch;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = d + kFrontPtr;
  ctx.r9.u64 = 0;
  ctx.r10.u64 = 0;
  const uint32_t params = uint32_t(ctx.r1.u64) + 0x50;  // ninth and tenth arguments
  PPC_STORE_U32(params + 0x4, d + kWidth);
  PPC_STORE_U32(params + 0xC, d + kHeight);
  ctx.lr = 0x820004F8;
  __imp__VdSwap(ctx, base);
  wptr = (wptr + 64) % kRingDwords;  // VdSwap filled the 64 reserved dwords
  PPC_STORE_U32(kWptrMmio, wptr);

  // --screenshot-after 1 ends the run (exit 0) once the frame is presented. Returning first would race the orderly
  // exit against it, so wait (1 ms at a time, at most 60 s) and fail if it never comes.
  PPC_STORE_U64(l, uint64_t(-10000));
  for (int i = 0; i < 60000 && !failed; ++i) {
    ctx.r3.u64 = 1;  // UserMode
    ctx.r4.u64 = 0;  // not alertable
    ctx.r5.u64 = l;
    ctx.lr = 0x820004FC;
    __imp__KeDelayExecutionThread(ctx, base);
  }
  fail();
  ctx.r1.u64 += gpu_boot::kFrame;
}
}  // namespace textured_quad

// Entry point: the textured quad. Meaningful only with --gpu vulkan --screenshot-after 1 (which ends the run with
// exit 0); otherwise exit 3 after 60 s. A failed import calls FakeMissing from 0x820004A4.
PPC_FUNC(sub_820004A0) { textured_quad::run(ctx, base); }
