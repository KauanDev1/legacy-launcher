// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

#include "pipeline/doctor.h"
#include "pipeline/stage.h"

namespace lr::pipeline {

std::unique_ptr<Stage> make_doctor_stage(DoctorProbe& probe);
std::unique_ptr<Stage> make_extract_stage();
std::unique_ptr<Stage> make_verify_stage();

}  // namespace lr::pipeline
