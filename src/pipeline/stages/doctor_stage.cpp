// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline/stages/doctor_stage.h"

#include <chrono>

#include "pipeline/doctor.h"

namespace lr::pipeline {
namespace {

class DoctorStage : public Stage {
 public:
  explicit DoctorStage(DoctorProbe& probe) : probe_(probe) {}
  StageId id() const override { return StageId::Doctor; }
  // Environment can change between runs: never cached.
  Result<std::string> fingerprint(StageContext&) override {
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "/" +
           std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
  }
  std::vector<fs::path> outputs(StageContext&) const override { return {}; }
  Result<void> execute(StageContext& ctx) override {
    LR_TRY_ASSIGN(report, run_doctor(probe_, ctx.paths.root));
    for (auto& w : report.warnings) ctx.sink.on_log("warning: " + w);
    return {};
  }

 private:
  DoctorProbe& probe_;
};

}  // namespace

std::unique_ptr<Stage> make_doctor_stage(DoctorProbe& probe) { return std::make_unique<DoctorStage>(probe); }

}  // namespace lr::pipeline
