// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string_view>

#include "common/result.h"
#include "formats/xex2.h"
#include "runtime/guest_memory.h"
#include "runtime/loader.h"

// Kernel DATA imports (variables such as KeTimeStampBundle or XexExecutableModuleHandle). The game reads them
// through its type-0 import records (IAT slots), which must hold the guest address of the variable's block.
// Variables register here (register_variable, from static initializers in variables.cpp: lr_runtime_core is linked
// --whole-archive); resolve_variable_imports allocates their blocks in the kernel data area and patches the slots.

namespace lr::kernel {

// Fills a freshly allocated, zeroed block: `host` points at it, `guest` is its guest address.
using VariableInit = void (*)(uint8_t* host, uint32_t guest);

// Registers kernel variable `name` with a block of `size` bytes, filled by `init` (may be null) when the block is
// allocated. Registering a name twice is a programming error: it prints the name and aborts. Safe to call from
// static initializers.
void register_variable(const char* name, uint32_t size, VariableInit init);

bool is_variable_registered(std::string_view name);

// Guest address of the block of registered variable `name`, or 0 when unknown or not yet allocated (blocks are
// allocated by resolve_variable_imports, for every registered variable, imported or not).
uint32_t variable_address(std::string_view name);

// Run once after load_image (and init_kernel), before guest code. Records the executable's module (LDR entry and
// header copy) in the kernel, allocates and fills every registered variable block, then rewrites every type-0
// import record slot in guest memory (found through ParseImage's byte-swapped values, see runtime/xex_imports.h)
// to a big-endian guest address:
//   - a function import (a thunk record follows it): the thunk's address;
//   - a registered variable: its block;
//   - anything else: a fresh PROT_NONE 4 KiB guard page in the kernel data area, named in fault reports as
//     "missing kernel variable <name>" (runtime/fault.h note_fault_page).
// InvalidFormat for malformed records, InsufficientResources when the kernel data area is full.
Result<void> resolve_variable_imports(runtime::GuestMemory& mem, const runtime::LoadedImage& image,
                                      const formats::Xex2File& xex);

}  // namespace lr::kernel
