// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "runtime/guest_memory.h"

namespace lr::runtime {

// Guest address of `host` if it lies inside [base, base+size) and fits in 32 bits.
std::optional<uint32_t> guest_address_of(const uint8_t* base, uint64_t size, const void* host);

// Async-signal-safe: no allocation, no stdio. Writes a NUL-terminated message into buf
// (truncating to n-1 chars) and returns the number of chars written (excluding the NUL).
size_t format_fault(char* buf, size_t n, int signo, const uint8_t* base, uint64_t size, const void* host);

// "SIGSEGV at guest 0x82001234" or "SIGSEGV at host 0x... (outside guest memory)".
std::string describe_fault(int signo, const uint8_t* base, uint64_t size, const void* host_addr);

// Installs SIGSEGV/SIGBUS/SIGILL handlers (SA_SIGINFO|SA_ONSTACK, own sigaltstack) that print
// the decoded fault to stderr with write(2) and _exit(4). `mem` must outlive the process's
// guest execution; its base/size are captured. Process-global: call once.
void install_fault_handlers(const GuestMemory& mem);

}  // namespace lr::runtime
