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

TEST_CASE("render_import_stubs emits weak stubs and the name table", "[unit]") {
  const std::vector<std::string> names = {"A", "B"};
  const std::string src = render_import_stubs(names);
  CHECK(src.find("PPC_FUNC(__imp__A)") != std::string::npos);
  CHECK(src.find("PPC_FUNC(__imp__B)") != std::string::npos);
  CHECK(src.find("weak") != std::string::npos);
  CHECK(src.find("unimplemented_import(\"B\"") != std::string::npos);
  CHECK(src.find("lr_import_count = 2") != std::string::npos);
  CHECK(src.find("lr_import_names") != std::string::npos);
  CHECK(src.find("\"A\"") != std::string::npos);
}

TEST_CASE("render_import_stubs with no imports still defines a valid table", "[unit]") {
  const std::string src = render_import_stubs({});
  CHECK(src.find("lr_import_count = 0") != std::string::npos);
}
