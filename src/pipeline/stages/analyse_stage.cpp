// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline/stages/analyse_stage.h"

#include <system_error>

#include "common/sha256.h"
#include "pipeline/process.h"

#ifndef LR_XENONRECOMP_COMMIT
#define LR_XENONRECOMP_COMMIT "unknown"
#endif

namespace lr::pipeline {
namespace {

class AnalyseStage : public Stage {
 public:
  StageId id() const override { return StageId::Analyse; }

  Result<std::string> fingerprint(StageContext& ctx) override {
    LR_TRY_ASSIGN(img, sha256_file(ctx.paths.image / "default_tu1.xex"));
    return img + "|" + LR_XENONRECOMP_COMMIT;
  }

  std::vector<fs::path> outputs(StageContext& ctx) const override {
    return {ctx.paths.analysis / "switch_tables.toml"};
  }

  Result<void> execute(StageContext& ctx) override {
    std::error_code ec;
    fs::create_directories(ctx.paths.analysis, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create " + ctx.paths.analysis.string() + ": " + ec.message(), ""};
    const fs::path out = ctx.paths.analysis / "switch_tables.toml";
    const fs::path tmp = ctx.paths.analysis / "switch_tables.toml.tmp";
    fs::remove(tmp, ec);

    // XenonAnalyse writes its output unconditionally and always exits 0: write to a temp file and
    // only publish it if it is non-empty.
    ToolInvocation inv;
    inv.program = ctx.resources / "tools" / "XenonAnalyse";
    inv.args = {(ctx.paths.image / "default_tu1.xex").string(), tmp.string()};
    inv.log_file = ctx.paths.logs / "analyse.log";
    inv.cancelled = [&] { return ctx.sink.cancelled(); };
    inv.on_line = [&](std::string_view line) { ctx.sink.on_log(line); };
    ctx.sink.on_progress({StageId::Analyse, 0.05f, "scanning for jump tables"});
    LR_TRY(run_tool(inv));
    auto size = fs::file_size(tmp, ec);
    if (ec || size == 0)
      return Error{ErrorCode::ToolFailed, "XenonAnalyse produced no output", "see " + inv.log_file.string()};
    fs::rename(tmp, out, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot rename to " + out.string() + ": " + ec.message(), ""};
    return {};
  }
};

}  // namespace

std::unique_ptr<Stage> make_analyse_stage() { return std::make_unique<AnalyseStage>(); }

}  // namespace lr::pipeline
