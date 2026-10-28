// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "common/result.h"

// Host side of guest threads: exit codes of the game process and the host thread every guest thread runs on.
// The guest side (PCR, KTHREAD, TLS, guest stack, exit paths) lives in runtime/kernel/threads.h.
namespace lr::runtime {

inline constexpr uint32_t kGuestStackTopSlack = 0x80;  // a new guest thread starts with r1 = stack top - slack
inline constexpr unsigned long kHostStackSize = 16ul * 1024 * 1024;

inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitUnimplementedImport = 3;
inline constexpr int kExitFault = 4;
inline constexpr int kExitGuestFatal = 5;  // lr::kernel::guest_fatal
inline constexpr int kExitDeadlock = 6;

// Starts `fn(arg)` on a new detached host thread with a kHostStackSize stack. The thread must install its own
// signal altstack (install_thread_altstack) before running guest code. IoError when pthread_create fails.
Result<void> start_host_thread(void* (*fn)(void*), void* arg);

}  // namespace lr::runtime
