// SPDX-License-Identifier: MIT
// Text disassembly of a decoded Program (debugging, --dump-shaders, test failure messages) and the mnemonic of every
// opcode. The mnemonics are the conventional Xenos ones (exec, cond_jmp, mad, dp4, muls, rcp, vfetch, tfetch ...).
#pragma once

#include <string>
#include <string_view>

#include "runtime/gpu/shader/ir.h"

namespace lr::gpu::shader {

std::string_view cf_op_name(CfOp op);
std::string_view vector_op_name(VectorOp op);
std::string_view scalar_op_name(ScalarOp op);  // empty for the Invalid41 placeholder
std::string_view fetch_op_name(FetchOp op);

// One line per CF instruction; each exec clause is followed by its ALU/fetch slots, indented. Starts with a summary
// (stage, sizes, used constants and fetch slots, exports).
std::string disassemble(const Program& program);

}  // namespace lr::gpu::shader
