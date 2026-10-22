// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include "common/paths.h"
#include "common/result.h"
#include "pipeline/game_config.h"
#include "pipeline/types.h"

namespace lr::pipeline {

class StateStore;

struct StageContext {
  const Options& options;
  DataPaths paths;
  fs::path resources;
  const GameConfig& game;
  ProgressSink& sink;
  // Set by run(); lets extract reuse its stored fingerprint when no ISO is given. Null in unit tests.
  const StateStore* state = nullptr;
};

struct Stage {
  virtual ~Stage() = default;
  virtual StageId id() const = 0;
  virtual Result<std::string> fingerprint(StageContext& ctx) = 0;
  virtual std::vector<fs::path> outputs(StageContext& ctx) const = 0;
  virtual Result<void> execute(StageContext& ctx) = 0;
  // An independent stage does not depend on the stages before it (only on its own fingerprint inputs): the runner
  // skips it when complete, fingerprint-equal and with its outputs present even after an earlier stage ran, and
  // it does not mark the following stages dirty. --force still re-runs it.
  virtual bool independent() const { return false; }
};

}  // namespace lr::pipeline
