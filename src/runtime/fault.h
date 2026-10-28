// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "common/result.h"
#include "runtime/guest_memory.h"

namespace lr::runtime {

// Guest address of `host` if it lies inside [base, base+size) and fits in 32 bits.
std::optional<uint32_t> guest_address_of(const uint8_t* base, uint64_t size, const void* host);

// Names the 4 KiB guest page containing `page` in fault reports: a fault inside it gets " (<what>)" appended, e.g.
// "SIGSEGV at guest 0x80003004 (missing kernel variable Foo)". `what` is copied (truncated to 95 chars) into a
// fixed-size table that format_fault reads without locking or allocating. Fill it before guest code runs (it is not
// meant to change while a fault can be reported). Returns false when the table (512 entries) is full.
bool note_fault_page(uint32_t page, std::string_view what);

// Empties the note_fault_page table.
void clear_fault_page_notes();

// Async-signal-safe: no allocation, no stdio. Writes a NUL-terminated message into buf
// (truncating to n-1 chars) and returns the number of chars written (excluding the NUL).
size_t format_fault(char* buf, size_t n, int signo, const uint8_t* base, uint64_t size, const void* host);

// "SIGSEGV at guest 0x82001234" or "SIGSEGV at host 0x... (outside guest memory)". A SIGILL outside guest memory
// gets a second line hinting at a host CPU without AVX (the game is compiled with -march=sandybridge).
std::string describe_fault(int signo, const uint8_t* base, uint64_t size, const void* host_addr);

// Installs SIGSEGV/SIGBUS/SIGILL handlers (SA_SIGINFO|SA_ONSTACK, own sigaltstack) that print
// the decoded fault to stderr with write(2) and _exit(4). `mem` must outlive the process's
// guest execution; its base/size are captured. Process-global: call once.
// Also calls install_thread_altstack() for the calling thread. Returns IoError on failure.
Result<void> install_fault_handlers(const GuestMemory& mem);

// sigaltstack is per-thread: EVERY guest thread must call this at start, otherwise a stack
// overflow on that thread kills the process without a report. Idempotent per thread; the
// stack is freed (and unregistered) at thread exit.
Result<void> install_thread_altstack();

}  // namespace lr::runtime
