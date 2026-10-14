// SPDX-License-Identifier: GPL-3.0-or-later
#include "launcher/cli/args.h"

#include <charconv>

#include "common/paths.h"

namespace lr::cli {
namespace {

using pipeline::StageId;

Error usage_error(std::string msg, std::string hint = "run `legacyrecomp --help` for usage") {
  return Error{ErrorCode::InvalidFormat, std::move(msg), std::move(hint)};
}

constexpr StageId kAllStages[] = {StageId::Doctor,  StageId::Extract,   StageId::Verify, StageId::Patch,
                                  StageId::Analyse, StageId::Recompile, StageId::Build};

std::string valid_stage_names() {
  std::string s;
  for (StageId id : kAllStages) {
    if (!s.empty()) s += ", ";
    s += pipeline::stage_name(id);
  }
  return s;
}

}  // namespace

std::string_view usage_text() {
  return "LegacyRecomp: static recompilation of Minecraft: Xbox 360 Edition to native Linux.\n"
         "\n"
         "usage: legacyrecomp <command> [options]\n"
         "\n"
         "commands:\n"
         "  doctor                     check the toolchain, CPU and disk space\n"
         "  install --iso <file>       run the whole pipeline (extract, verify, patch, analyse, recompile, build);\n"
         "                             finished stages are skipped\n"
         "  build                      rebuild from the already extracted game (recompile + build); no --iso needed\n"
         "  run [-- <game args>]       start the built game; arguments after -- go to the game\n"
         "  inspect --iso <file>       dump the ISO tree and XEX headers\n"
         "  inspect --sections <xex>   print the sections of a loaded XEX\n"
         "  inspect --dump-image <xex> <out.bin>\n"
         "\n"
         "options:\n"
         "  --iso <file>               game ISO (install)\n"
         "  --root <dir>               data root; precedence: --root, $LEGACYRECOMP_ROOT, $XDG_DATA_HOME/legacyrecomp\n"
         "  --force <stage[,stage]>    re-run a stage and everything after it (doctor, extract, verify, patch,\n"
         "                             analyse, recompile, build); may be repeated\n"
         "  --jobs <n>                 parallel jobs (default: hardware concurrency)\n"
         "  --help                     show this text\n"
         "\n"
         "exit codes: 0 ok, 1 pipeline error, 2 usage; `run` returns the game's exit code.\n"
         "Ctrl+C during install/build cancels cleanly; completed stages stay cached.\n";
}

Result<CliArgs> parse_args(std::span<const std::string_view> args) {
  if (args.empty()) return usage_error("missing command");
  CliArgs out;
  out.command = std::string(args[0]);
  const std::string& cmd = out.command;
  if (cmd != "doctor" && cmd != "inspect" && cmd != "install" && cmd != "build" && cmd != "run")
    return usage_error("unknown command '" + cmd + "'");

  if (cmd == "inspect") {
    for (size_t i = 1; i < args.size(); ++i) out.passthrough.emplace_back(args[i]);
    return out;
  }

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string_view a = args[i];
    if (a == "--" && cmd == "run") {
      for (size_t j = i + 1; j < args.size(); ++j) out.passthrough.emplace_back(args[j]);
      break;
    }
    auto value = [&]() -> Result<std::string_view> {
      if (i + 1 >= args.size()) return usage_error("option " + std::string(a) + " needs a value");
      return args[++i];
    };
    if (a == "--iso") {
      LR_TRY_ASSIGN(v, value());
      out.iso = std::filesystem::path(std::string(v));
    } else if (a == "--root") {
      LR_TRY_ASSIGN(v, value());
      out.root = std::filesystem::path(std::string(v));
    } else if (a == "--jobs") {
      LR_TRY_ASSIGN(v, value());
      unsigned n = 0;
      auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
      if (ec != std::errc{} || p != v.data() + v.size() || n == 0)
        return usage_error("--jobs needs a positive integer, got '" + std::string(v) + "'");
      out.jobs = n;
    } else if (a == "--force") {
      LR_TRY_ASSIGN(v, value());
      while (!v.empty()) {
        size_t comma = v.find(',');
        std::string_view name = v.substr(0, comma);
        auto id = pipeline::parse_stage(name);
        if (!id)
          return usage_error("unknown stage '" + std::string(name) + "' in --force",
                             "valid stages: " + valid_stage_names());
        out.force.insert(*id);
        if (comma == std::string_view::npos) break;
        v.remove_prefix(comma + 1);
      }
    } else {
      return usage_error("unknown option '" + std::string(a) + "' for `" + cmd + "`");
    }
  }

  if (cmd == "install" && !out.iso) return usage_error("install needs --iso <file>");
  if (cmd == "build") out.force.insert(StageId::Recompile);
  return out;
}

std::filesystem::path resolve_root(const std::optional<std::filesystem::path>& flag, const char* env_root,
                                   const char* xdg_data_home, const char* home) {
  if (flag) return *flag;
  if (env_root && *env_root) return env_root;
  return default_data_root(xdg_data_home, home);
}

}  // namespace lr::cli
