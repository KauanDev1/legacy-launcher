// SPDX-License-Identifier: GPL-3.0-or-later
// Test tool: gen_import_stubs <ppc_recomp_shared.h> <out.cpp> — what the game template does with the real header.
#include <cstdio>
#include <fstream>
#include <sstream>

#include "pipeline/import_stubs.h"

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  std::ifstream in(argv[1]);
  if (!in) return 2;
  std::stringstream ss;
  ss << in.rdbuf();
  const auto names = lr::pipeline::parse_import_names(ss.str());
  std::ofstream out(argv[2]);
  out << lr::pipeline::render_import_stubs(names);
  return out ? 0 : 1;
}
