// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/result.h"
#include "pipeline/doctor.h"
#include "pipeline/stage.h"

namespace lr::pipeline {

// doctor, extract, verify, patch, analyse, recompile, build (StageId order).
std::vector<std::unique_ptr<Stage>> default_stages(DoctorProbe& probe);

// Resources dir: $LEGACYRECOMP_RESOURCES, else <dir of this executable>/../share/legacyrecomp.
Result<fs::path> find_resources();

// Data root: options.root, else $XDG_DATA_HOME/legacyrecomp (default ~/.local/share/legacyrecomp).
fs::path data_root(const Options& options);

// Full install: resolves resources, loads config/minecraft-tu1/game.toml, opens <root>/work/state.toml and runs
// default_stages() with the system doctor probe. Cached stages are skipped (see run_stages).
Result<void> run(const Options& options, ProgressSink& sink);

// Spawns <root>/bin/legacyrecomp-game --image <root>/work/image/default_tu1.xex --data <root>/game [extra_args...]
// with inherited stdio (posix_spawn, no shell) and waits. Returns its exit code (128 + signal if it was killed).
// NotFound if the game was not built yet.
Result<int> launch(const Options& options, std::span<const std::string> extra_args);

}  // namespace lr::pipeline
