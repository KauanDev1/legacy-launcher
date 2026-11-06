// SPDX-License-Identifier: MIT
#pragma once

#include <signal.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "common/result.h"
#include "runtime/guest_memory.h"

// Guest write watch (`--watch-guest <addr>:<len>[:rw]`, a diagnostic): finds who writes a guest buffer, or shows
// that nothing does; with ":rw" also who reads it (the pages are then inaccessible, and a read is let through the
// same way as a write). The 4 KiB host pages covering each watched range are made read-only. A write into one of them faults;
// the SIGSEGV handler (runtime/fault.h) asks write_watch_handle_segv first. For a watched page it counts the write
// when it lands inside a watched range and logs it, makes the page writable, and sets the trap flag so the faulting
// instruction runs once (single step); the SIGTRAP that follows makes the page read-only again. Any other fault stays
// a real fault (exit 4).
//
// A logged write names the guest functions on the host call stack (innermost first): the host code addresses are
// mapped back to recompiled guest functions through the function table (the HostCodeMap given to
// install_write_watch), so the log holds guest addresses only, never data. One line per new call site (the
// innermost three guest functions) per range, at most kMaxSitesPerRange of them, plus a count line at every power of
// two from 1024.
//
// Host-side bulk writes that would not fault go through write_watch_note_host_write: GuestMemory::zero (its hole
// punch and madvise paths never fault) and the kernel's file reads (a read(2) into a read-only page fails with
// EFAULT, so the reader bounces through a host buffer instead when write_watch_overlaps says so).
//
// Limits (a diagnostic, not a guard): while one thread single-steps its write the page is writable for every
// thread, so a concurrent write by another thread may go uncounted; GuestMemory::protect on a watched page
// (strict-memory heap commits) lifts the watch from that page. Both the watch and GuestMemory::protect act on the
// host pages of one view only: the three physical views alias one memory (0xA0000000 + p, 0xC0000000 + p and
// 0xE0000000 + p - 0x1000), so to see every access to physical page p, arm one range per view. Process-global, x86-64
// Linux only.
namespace lr::runtime {

struct WatchRange {
  uint32_t addr = 0;
  uint32_t len = 0;
  bool reads = false;  // ":rw": reads are logged too (the pages become inaccessible instead of read-only)
};

// "<addr>:<len>[:rw]", each number hex with 0x or decimal digits; len > 0 and the range must end at or below 2^32.
// With ":rw" the range's pages are PROT_NONE and reads are logged as well ("[watch] read ...").
Result<WatchRange> parse_watch_spec(std::string_view spec);

// Recompiled host code -> guest function. Entries are (host entry, guest entry); host code between one entry and
// the next belongs to the first. Addresses kLastTail (0x1000) or more past the last entry, or before the first, are
// not guest code.
struct HostCodeEntry {
  uintptr_t host = 0;
  uint32_t guest = 0;
};
class HostCodeMap {
 public:
  HostCodeMap() = default;
  explicit HostCodeMap(std::vector<HostCodeEntry> entries);  // any order; sorted here
  std::optional<uint32_t> guest_function(uintptr_t host_pc) const;
  bool empty() const { return entries_.empty(); }

 private:
  std::vector<HostCodeEntry> entries_;
};

// Arms the watch on `ranges` (at most 16) and installs the SIGTRAP handler. Call after install_fault_handlers and
// before guest code runs; once per process. Log lines go to `log_fd` with write(2).
Result<void> install_write_watch(const GuestMemory& mem, std::span<const WatchRange> ranges, HostCodeMap code,
                                 int log_fd = 2);

// For the SIGSEGV handler: true when the fault was a write into a watched page and has been let through (the
// handler must return), false for every other fault. Async-signal-safe.
bool write_watch_handle_segv(siginfo_t* info, void* ucontext);

// True when guest [addr, addr + len) touches a watched page (any thread, any time; lock-free).
bool write_watch_overlaps(uint32_t addr, uint32_t len);

// Counts and logs (always in full) a host-side write `what` into guest [addr, addr + len) when it touches a watched
// range (no-op otherwise). Not for signal handlers.
void write_watch_note_host_write(uint32_t addr, uint32_t len, const char* what);

// Logs a host-side event `what` (an allocation, a free) on guest [addr, addr + len) when it touches a watched range,
// without counting it as a write. Not for signal handlers.
void write_watch_note_host_event(uint32_t addr, uint32_t len, const char* what);

// Prints one summary line per range ("[watch] summary range N <addr>+<len> writes=N nonzero_stores=N
// nonzero_bytes=N") and one per call site ("[watch]   site K writes=N nonzero_stores=N fn ..."). A store counts as
// nonzero when, once it ran, one of the (up to) 8 bytes from its faulting address inside the range is nonzero: a
// writer of zeros shows nonzero_stores=0. For the exit report (main.cpp); no-op when nothing is watched.
void write_watch_report();

// Number of writes counted so far into watched range `index` (faults and host writes, not reads); 0 for an unknown
// index.
uint64_t write_watch_count(size_t index);

}  // namespace lr::runtime
