// SPDX-License-Identifier: MIT
#include "pipeline/runner.h"

namespace lr::pipeline {

Result<void> run_stages(std::span<Stage* const> stages, StageContext& ctx, StateStore& state) {
  bool dirty = false;  // a previous stage was forced or executed
  for (Stage* stage : stages) {
    const StageId id = stage->id();
    if (ctx.sink.cancelled()) return Error{ErrorCode::Cancelled, "cancelled", ""};
    const bool forced = ctx.options.force.count(id) != 0;
    if (forced) dirty = true;

    LR_TRY_ASSIGN(fp, stage->fingerprint(ctx));
    bool skip = (!forced && (!dirty || stage->independent())) && state.is_complete(id, fp);
    if (skip) {
      std::error_code ec;
      for (const auto& out : stage->outputs(ctx)) {
        if (!fs::exists(out, ec)) {
          skip = false;
          break;
        }
      }
    }
    if (skip) {
      ctx.sink.on_progress({id, 1.0f, "cached"});
      continue;
    }

    if (!stage->independent() || forced) dirty = true;
    state.mark_started(id);
    LR_TRY(state.save());
    ctx.sink.on_progress({id, 0.0f, "start"});
    LR_TRY(stage->execute(ctx));
    state.mark_complete(id, std::move(fp));
    LR_TRY(state.save());
    ctx.sink.on_progress({id, 1.0f, "done"});
  }
  return {};
}

}  // namespace lr::pipeline
