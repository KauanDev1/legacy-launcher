// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "runtime/guest_memory.h"

struct PPCContext;  // XenonUtils ppc_context.h

namespace lr::runtime {

inline constexpr uint32_t kGuestStackBase = 0x70000000;
inline constexpr uint32_t kGuestStackSize = 0x100000;
inline constexpr uint32_t kGuestStackTopSlack = 0x80;  // r1 = top - slack
inline constexpr uint32_t kGuestPcrAddress = 0x7FFF0000;  // r13
inline constexpr uint32_t kGuestTlsAddress = 0x7FFE0000;
inline constexpr unsigned long kHostStackSize = 16ul * 1024 * 1024;

// Same type as XenonRecomp's PPCFunc.
using GuestEntryFn = void(PPCContext&, uint8_t*);

inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitUnimplementedImport = 3;
inline constexpr int kExitFault = 4;

// Runs `entry_fn` on a fresh host thread with a 16 MiB stack: installs the per-thread sigaltstack, builds the
// PPCContext (r1 = stack top - 0x80, r13 = PCR), calls entry_fn(ctx, mem.base()) and joins.
// Returns kExitOk on normal return; on lr::kernel::UnimplementedImport prints the report to stderr and returns
// kExitUnimplementedImport; any other escaping exception or a setup failure is reported and returns kExitFault.
// (A real fault is handled by the signal handler, which _exit(4)s.) `entry` is the guest address, for messages only.
int run_guest(GuestMemory& mem, uint32_t entry, GuestEntryFn* entry_fn);

}  // namespace lr::runtime
