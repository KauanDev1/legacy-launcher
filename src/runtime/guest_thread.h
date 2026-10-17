// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "runtime/guest_memory.h"

namespace lr::runtime {

inline constexpr uint32_t kGuestStackBase = 0x70000000;
inline constexpr uint32_t kGuestStackSize = 0x100000;
inline constexpr uint32_t kGuestStackTopSlack = 0x80;  // r1 = top - slack
inline constexpr uint32_t kGuestPcrAddress = 0x7FFF0000;  // r13
inline constexpr uint32_t kGuestTlsAddress = 0x7FFE0000;
inline constexpr uint32_t kGuestThreadAddress = 0x7FFD0000;  // zeroed fake KTHREAD block
inline constexpr unsigned long kHostStackSize = 16ul * 1024 * 1024;

inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitUnimplementedImport = 3;
inline constexpr int kExitFault = 4;
inline constexpr int kExitGuestFatal = 5;  // lr::kernel::guest_fatal
inline constexpr int kExitDeadlock = 6;

// Writes the Xbox 360 KPCR (big-endian guest layout, NOT PPCContext) at kGuestPcrAddress, following Xenia's
// X_KPCR (xenia-project/xenia, src/xenia/kernel/xthread.h): tls_ptr (+0x00) = kGuestTlsAddress,
// pcr_ptr (+0x30) = kGuestPcrAddress, stack_base_ptr (+0x70) = stack top, stack_end_ptr (+0x74) = stack bottom,
// current_thread (+0x100) = kGuestThreadAddress (a zeroed block), current_cpu (+0x10C, u8) = 0.
// (Xenia canary widens pcr_ptr to 64 bits; the master layout used here has a 32-bit field at +0x30.)
// Full TLS slot / KTHREAD emulation is M2 work. Touches no PPCContext, so the prebuilt runtime stays independent of
// the game's generated ppc_config.h layout options.
void init_guest_pcr(GuestMemory& mem);

// Layout-agnostic guest runner: `body(mem.base(), user)` runs on a fresh host thread with a 16 MiB stack after
// install_thread_altstack(). The caller (main.cpp, compiled with the real generated ppc_config.h) builds the
// PPCContext inside `body` (r1 = kGuestStackBase + kGuestStackSize - kGuestStackTopSlack, r13 = kGuestPcrAddress).
// Returns kExitOk on normal return; on lr::kernel::UnimplementedImport prints the report to stderr and returns
// kExitUnimplementedImport; any other escaping exception or a setup failure is reported and returns kExitFault.
// (A real fault is handled by the signal handler, which _exit(4)s.)
using GuestBody = void(uint8_t* base, void* user);
int run_guest(GuestMemory& mem, GuestBody* body, void* user);

}  // namespace lr::runtime
