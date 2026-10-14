// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline/runner.h"

namespace lr::pipeline {

Result<void> run_stages(std::span<Stage* const> stages, StageContext& ctx, StateStore& state) {
  bool dirty = false;  // a previous stage was forced or executed
  for (Stage* stage : stages) {
    const StageId id = stage->id();
    if (ctx.sink.cancelled()) return Error{ErrorCode::Cancelled, "cancelled", ""};
    if (ctx.options.force.count(id)) dirty = true;

    LR_TRY_ASSIGN(fp, stage->fingerprint(ctx));
    bool skip = !dirty && state.is_complete(id, fp);
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

    dirty = true;
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
