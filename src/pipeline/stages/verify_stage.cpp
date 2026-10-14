// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline/stages/verify_stage.h"

#include "common/sha256.h"

namespace lr::pipeline {
namespace {

class VerifyStage : public Stage {
 public:
  StageId id() const override { return StageId::Verify; }
  Result<std::string> fingerprint(StageContext& ctx) override {
    LR_TRY_ASSIGN(a, sha256_file(ctx.paths.input / "package.xex"));
    LR_TRY_ASSIGN(b, sha256_file(ctx.paths.input / "tu.xexp"));
    return a + b;
  }
  std::vector<fs::path> outputs(StageContext&) const override { return {}; }
  Result<void> execute(StageContext& ctx) override {
    struct Item { const char* file; const std::string& expected; };
    const Item items[] = {{"package.xex", ctx.game.hash_package_xex}, {"tu.xexp", ctx.game.hash_tu_xexp}};
    for (auto& it : items) {
      if (it.expected.empty())
        return Error{ErrorCode::Internal, std::string("hash not configured for ") + it.file, ""};
      LR_TRY_ASSIGN(found, sha256_file(ctx.paths.input / it.file));
      if (found != it.expected)
        return Error{ErrorCode::UnsupportedVersion,
                     std::string(it.file) + " has sha256 " + found + ", expected " + it.expected,
                     "LegacyRecomp supports the Minecraft X360 disc (title 584111F7) with TU1; see docs/pipeline.md"};
    }
    return {};
  }
};

}  // namespace

std::unique_ptr<Stage> make_verify_stage() { return std::make_unique<VerifyStage>(); }

}  // namespace lr::pipeline
