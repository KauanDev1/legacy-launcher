// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "pipeline/import_stubs.h"

using namespace lr::pipeline;

TEST_CASE("parse_import_names extracts, dedups and sorts import names", "[unit]") {
  // Shape copied from the generated ppc_recomp_shared.h: imports are PPC_EXTERN_FUNC(__imp__Name);
  const std::string header =
      "#pragma once\n#include \"ppc_config.h\"\n#include \"ppc_context.h\"\n"
      "PPC_EXTERN_FUNC(sub_82000000);\n"
      "PPC_EXTERN_FUNC(__imp__XamInputGetState);\n"
      "PPC_EXTERN_FUNC(__imp__NtCreateFile);\n"
      "PPC_EXTERN_FUNC(__imp__NtCreateFile);\n"
      "  PPC_EXTERN_FUNC( __imp__RtlUnwind ) ;\n"
      "// PPC_EXTERN_FUNC(__imp__NotADeclaration) is only mentioned in prose\n"
      "static int __imp__outside = 0;\n";
  const std::vector<std::string> expect = {"NtCreateFile", "RtlUnwind", "XamInputGetState"};
  CHECK(parse_import_names(header) == expect);
}

TEST_CASE("parse_import_names on an empty header", "[unit]") { CHECK(parse_import_names("").empty()); }

TEST_CASE("render_import_thunks emits a cached lookup per import", "[unit]") {
  const std::vector<std::string> names = {"A", "B"};
  const std::string src = render_import_thunks(names);
  for (const std::string n : {"A", "B"}) {
    CHECK(src.find("PPC_FUNC(__imp__" + n + ")") != std::string::npos);
    CHECK(src.find("static const lr::kernel::ImportEntry e = lr::kernel::find_import(\"" + n + "\");") !=
          std::string::npos);
    CHECK(src.find("lr_call_import(\"" + n + "\", e, ctx, base);") != std::string::npos);
  }
  // The register copy lives once in a shared helper, not in every thunk.
  CHECK(src.find("lr::kernel::trace(name)") != std::string::npos);
  CHECK(src.find("unimplemented_import(name, uint32_t(ctx.lr))") != std::string::npos);
  CHECK(src.find("c.gpr[7] = ctx.r10.u64;") != std::string::npos);
  CHECK(src.find("c.fpr[12] = ctx.f13.f64;") != std::string::npos);
  CHECK(src.find("ctx.r3.u64 = c.ret_gpr;") != std::string::npos);
  CHECK(src.find("if (c.has_fpr_return) ctx.f1.f64 = c.ret_fpr;") != std::string::npos);
  CHECK(src.find("c.gpr[0]") == src.rfind("c.gpr[0]"));
  CHECK(src.find("weak") == std::string::npos);
  CHECK(src.find("lr_import_count = 2") != std::string::npos);
  CHECK(src.find("lr_import_names") != std::string::npos);
  CHECK(src.find("  \"A\",\n") != std::string::npos);
}

TEST_CASE("render_import_thunks with no imports still defines a valid table", "[unit]") {
  const std::string src = render_import_thunks({});
  CHECK(src.find("lr_import_count = 0") != std::string::npos);
}
