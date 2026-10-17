// SPDX-License-Identifier: MIT
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lr::pipeline {

// Import names declared by the generated ppc_recomp_shared.h as `PPC_EXTERN_FUNC(__imp__<Name>);`
// (any PPC_EXTERN_* macro; comments are ignored). Deduplicated, sorted, `__imp__` stripped.
std::vector<std::string> parse_import_names(std::string_view shared_header);

// C++ source defining, for every name, the thunk `__imp__<Name>`: it caches lr::kernel::find_import("<Name>")
// in a function-local static, traces the call, copies r3..r10, f1..f13, r1, r13 and lr into a
// lr::kernel::GuestCall and calls the registered function (writing back r3, and f1 when it returns a float),
// or calls lr::kernel::unimplemented_import when nothing is registered. Also defines `lr_import_names[]` /
// `lr_import_count` (declared extern so other TUs can reference them). Compiled by the game template together
// with the generated headers; it is the only kernel-side code that touches PPCContext.
std::string render_import_thunks(std::span<const std::string> names);

}  // namespace lr::pipeline
