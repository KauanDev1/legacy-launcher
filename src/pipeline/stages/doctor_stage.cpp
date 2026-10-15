// SPDX-License-Identifier: MIT
#include "pipeline/stages/doctor_stage.h"

#include <string>
#include <string_view>

#include "pipeline/doctor.h"

namespace lr::pipeline {
namespace {

class DoctorStage : public Stage {
 public:
  explicit DoctorStage(DoctorProbe& probe) : probe_(probe) {}
  StageId id() const override { return StageId::Doctor; }
  // Cached after the first success: a doctor that ran on every install would mark every later stage dirty
  // (run_stages re-executes everything after an executed stage), so nothing would ever be cached. Later stages
  // still fail with ToolMissing if the toolchain disappears; `--force doctor` (or `legacyrecomp doctor`) re-checks.
  Result<std::string> fingerprint(StageContext&) override { return std::string(kFingerprint); }
  std::vector<fs::path> outputs(StageContext&) const override { return {}; }
  Result<void> execute(StageContext& ctx) override {
    LR_TRY_ASSIGN(report, run_doctor(probe_, ctx.paths.root, ctx.resources));
    for (auto& w : report.warnings) ctx.sink.on_log("warning: " + w);
    return {};
  }

 private:
  static constexpr std::string_view kFingerprint = "doctor-1";
  DoctorProbe& probe_;
};

}  // namespace

std::unique_ptr<Stage> make_doctor_stage(DoctorProbe& probe) { return std::make_unique<DoctorStage>(probe); }

}  // namespace lr::pipeline
