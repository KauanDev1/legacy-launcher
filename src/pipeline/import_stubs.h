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

// C++ source defining, for every name, a weak `__imp__<Name>` stub that throws lr::kernel::UnimplementedImport,
// plus `lr_import_names[]` / `lr_import_count` (declared extern so other TUs can reference them).
// Compiled by the game template together with the generated headers.
std::string render_import_stubs(std::span<const std::string> names);

}  // namespace lr::pipeline
