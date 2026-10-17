// SPDX-License-Identifier: MIT
// Debug, bugcheck, HAL and SEH-minimum imports. Semantics and argument orders follow Xenia's xboxkrnl_debug.cc,
// xboxkrnl_hal.cc and xboxkrnl_memory.cc (KeLockL2) (semantic reference only, no code copied). Everything the guest
// cannot recover from goes through guest_fatal (exit 5) so the run stops with a named reason and the import trail.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/guest_thread.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/trace.h"

namespace lr::kernel {
namespace {

constexpr uint32_t kMaxDbgPrint = 4096;
// MSVC's SetThreadName raises this code for an attached debugger and catches it itself; Xenia handles it and returns.
constexpr uint32_t kSetThreadNameException = 0x406D1388;
constexpr uint32_t kContextControl = 0x1, kContextInteger = 0x4;

std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

[[noreturn]] void bugcheck(const char* name, uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4) {
  guest_fatal(std::string(name) + " " + hex32(code) + " (" + hex32(p1) + ", " + hex32(p2) + ", " + hex32(p3) + ", " +
              hex32(p4) + ")");
}

}  // namespace

namespace exports {

// DbgPrint(format, ...): until the printf interpreter (Task 24) exists, the format string itself is printed,
// verbatim, prefixed "[guest] " (at most 4 KiB, no newline added).
uint32_t DbgPrint(GuestCall& c, uint32_t format) {
  std::string s = "[guest] ";
  if (GuestPtr<char>{format}.valid_range(1)) {
    const char* p = reinterpret_cast<const char*>(c.base + format);
    const uint64_t room = (uint64_t{1} << 32) - format;
    s.append(p, ::strnlen(p, static_cast<size_t>(room < kMaxDbgPrint ? room : kMaxDbgPrint)));
  } else {
    s += "(null)\n";
  }
  std::fputs(s.c_str(), stderr);
  return 0;  // STATUS_SUCCESS
}

// No debugger to break into: log it and continue.
void DbgBreakPoint(GuestCall& c) {
  const GuestThread* t = current_thread();
  std::fprintf(stderr, "[guest] DbgBreakPoint from 0x%08X (thread %u), continuing\n", c.lr, t ? t->id() : 0u);
}

void KeBugCheckEx(uint32_t code, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4) {
  bugcheck("KeBugCheckEx", code, p1, p2, p3, p4);
}

void KeBugCheck(uint32_t code) { bugcheck("KeBugCheck", code, 0, 0, 0, 0); }

// HalReturnToFirmware(routine): the title asked the console to reboot / return to the dashboard. Every routine ends
// the run normally (exit 0).
void HalReturnToFirmware(uint32_t routine) {
  std::fprintf(stderr, "Guest requested shutdown: HalReturnToFirmware(%u)\n", routine);
  std::fflush(nullptr);
  flush_import_trace();
  _exit(runtime::kExitOk);
}

// RtlCaptureContext(context*): only what the thunk knows is real: r1, r3..r10, r13 and lr (iar = lr, the caller's
// resume address, as on NT); every other register, the FP and the VMX sections are 0. ContextFlags = control |
// integer.
void RtlCaptureContext(GuestCall& c, uint32_t context) {
  if (!GuestPtr<X_CONTEXT>{context}.valid_range(sizeof(X_CONTEXT)))
    guest_fatal("RtlCaptureContext with an invalid CONTEXT pointer " + hex32(context));
  auto* x = reinterpret_cast<X_CONTEXT*>(c.base + context);
  std::memset(static_cast<void*>(x), 0, sizeof(X_CONTEXT));
  x->context_flags = kContextControl | kContextInteger;
  x->lr = c.lr;
  x->iar = c.lr;
  x->gpr[1] = c.r1;
  for (int i = 0; i < 8; ++i) x->gpr[3 + i] = c.gpr[i];
  x->gpr[13] = c.r13;
}

// RtlRaiseException(record*): no guest SEH unwinding exists, so every exception is fatal and named by its code,
// except SetThreadName (handled and returned from, as Xenia does).
void RtlRaiseException(GuestCall& c, uint32_t record) {
  if (!GuestPtr<X_EXCEPTION_RECORD>{record}.valid_range(sizeof(X_EXCEPTION_RECORD)))
    guest_fatal("RtlRaiseException with an invalid record pointer " + hex32(record));
  const auto* r = reinterpret_cast<const X_EXCEPTION_RECORD*>(c.base + record);
  const uint32_t code = r->code;
  if (code == kSetThreadNameException) return;
  guest_fatal("RtlRaiseException " + hex32(code) + " (flags " + hex32(r->exception_flags) + ", address " +
              hex32(r->exception_address) + ", called from " + hex32(c.lr) + ")");
}

void RtlUnwind(GuestCall& c) { guest_fatal("RtlUnwind (guest SEH unwinding is not supported), called from " + hex32(c.lr)); }

void __C_specific_handler(GuestCall& c) {
  guest_fatal("__C_specific_handler (guest SEH is not supported), called from " + hex32(c.lr));
}

}  // namespace exports
}  // namespace lr::kernel

LR_KERNEL(DbgPrint);
LR_KERNEL(DbgBreakPoint);
LR_KERNEL(KeBugCheckEx);
LR_KERNEL(KeBugCheck);
LR_KERNEL(HalReturnToFirmware);
LR_KERNEL(RtlCaptureContext);
LR_KERNEL(RtlRaiseException);
LR_KERNEL(RtlUnwind);
LR_KERNEL(__C_specific_handler);
// Event tracing (ETX) producers: no host consumer, so registration, unregistration and logging just succeed.
LR_KERNEL_STUB(EtxProducerRegister, 0);
LR_KERNEL_STUB(EtxProducerUnregister, 0);
LR_KERNEL_STUB(EtxProducerLog, 0);
// The L2 cache lock only matters for cache-locked GPU streaming on the console; Xenia also returns 0.
LR_KERNEL_STUB(KeLockL2, 0);
LR_KERNEL_STUB(KeUnlockL2, 0);
