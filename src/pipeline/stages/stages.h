// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

#include "pipeline/doctor.h"
#include "pipeline/stage.h"

namespace lr::pipeline {

std::unique_ptr<Stage> make_doctor_stage(DoctorProbe& probe);
std::unique_ptr<Stage> make_extract_stage();
std::unique_ptr<Stage> make_verify_stage();
std::unique_ptr<Stage> make_patch_stage();     // work/input -> work/image/default_tu1.xex (XenonUtils XexPatcher)
std::unique_ptr<Stage> make_analyse_stage();   // XenonAnalyse -> work/analysis/switch_tables.toml
std::unique_ptr<Stage> make_recompile_stage(); // XenonRecomp -> work/generated/
std::unique_ptr<Stage> make_build_stage();     // runtime/template + work/generated -> bin/legacyrecomp-game

}  // namespace lr::pipeline
