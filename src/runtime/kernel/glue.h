// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <span>

// Calls from the kernel into recompiled guest code (thread start routines, APCs, DPCs, notification callbacks).
// The prebuilt runtime never sees PPCContext (its layout depends on the game's generated ppc_config.h), so the
// game's main.cpp installs a GuestGlue at startup that builds a fresh PPCContext and calls the function through
// PPC_LOOKUP_FUNC. call_guest runs the callee below the calling thread's current guest frame, which every
// generated import thunk records with set_current_frame before the import runs.

namespace lr::kernel {

struct GuestGlue {
  // Runs the recompiled function at guest address `fn` on a fresh context: r3.. = args[0..nargs), r1 and r13 as
  // given; returns the callee's r3.
  uint64_t (*call)(uint8_t* base, uint32_t fn, const uint64_t* args, int nargs, uint32_t r1, uint32_t r13);
  // True when `fn` is a recompiled function entry (inside the code range, 4-aligned, function-table slot set).
  bool (*has_function)(uint32_t fn);
};

// Installs the glue (main.cpp, before any guest code runs).
void set_guest_glue(const GuestGlue& g);

// Records the calling thread's current guest frame (r1, r13). Called by every generated thunk (lr_call_import)
// before the import runs, and by code that starts guest code on a new thread.
// t_frame (glue.cpp) is the frame of the most recent import on this thread.
void set_current_frame(uint32_t r1, uint32_t r13);

// Calls the recompiled function at `fn` with up to 8 integer args (r3..r10) on the calling thread and returns its
// r3. The callee runs with r1 = (current r1 - 0x100) & ~0xF and the current r13, using kernel().memory() as the
// guest base; the caller's frame is restored afterwards, also when the callee unwinds (a thunk inside the callee
// moves it). The glue's `call` gives the callee the host FP environment and restores it on return. guest_fatal (exit 5)
// when no glue is installed, the thread has no current frame, there are more than 8 args, or !has_function(fn)
// (the message names the guest address: never a jump to host address 0).
uint64_t call_guest(uint32_t fn, std::span<const uint64_t> args);

}  // namespace lr::kernel
