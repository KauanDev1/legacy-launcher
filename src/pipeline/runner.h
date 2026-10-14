// SPDX-License-Identifier: MIT
#pragma once

#include <span>

#include "common/result.h"
#include "pipeline/stage.h"
#include "pipeline/state.h"

namespace lr::pipeline {

// Runs stages in order, skipping cached ones. Once a stage executes (or is forced), all later
// stages execute too. Returns the first error unchanged.
Result<void> run_stages(std::span<Stage* const> stages, StageContext& ctx, StateStore& state);

}  // namespace lr::pipeline
