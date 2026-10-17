// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>

#include "common/result.h"
#include "runtime/kernel/abi.h"

// Import trace (spec section 11, `legacyrecomp-game --trace-imports [--trace-file <path>]`). Every generated thunk
// calls trace_import_return after its import returns (trace_import_missing before reporting a missing one), which
// appends one line per call when the trace is enabled:
//
//   [<tid>] <Name> r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X -> 0x%08X      (or "-> missing")
//
// with the low 32 bits of r3..r6 at entry and of r3 at return; tid is the guest thread id (0 on a non-guest thread).
// An import that never returns (KeBugCheckEx, ExTerminateThread, HalReturnToFirmware, ...) has no line.
//
// Buffering: lines go to a 64 KiB process-wide buffer behind a spinlock (an atomic flag) and are written to the file
// with write(2) when it fills. It is flushed by flush_import_trace on every exit path that is not a signal handler
// (normal exit through atexit and main's _exit, exit 3 from the guest thread, guest_fatal's exit 5, the deadlock
// detector's exit 6, HalReturnToFirmware), and by flush_import_trace_from_signal in the fault handler (exit 4), which
// only takes the spinlock if it is free and never blocks: if the faulting thread was itself appending a line, the
// buffered lines are lost. Disabled (the default), each thunk pays one atomic load.

namespace lr::kernel {

// Starts tracing to `file` (truncated; its parent directories are created) or, with nullopt, to stderr. Replaces a
// previous trace (flushed and closed first). IoError when the file cannot be opened.
Result<void> enable_import_trace(const std::optional<std::filesystem::path>& file);
// Flushes and closes the trace; later calls log nothing (reset_kernel_for_tests, tests).
void disable_import_trace();
bool import_trace_enabled();

// Thunk side: `c` holds r3..r6 at entry (gpr[0..3]) and the return value (ret_gpr).
void trace_import_return(const char* name, const GuestCall& c);
void trace_import_missing(const char* name, const GuestCall& c);

// Writes the buffered lines. Not async-signal-safe.
void flush_import_trace();
// Async-signal-safe variant for the fault handler: skips the flush if the buffer is busy.
void flush_import_trace_from_signal();

}  // namespace lr::kernel
