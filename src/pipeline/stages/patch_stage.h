// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

#include "pipeline/stages/stages.h"

namespace lr::pipeline {

// Name of an XexPatcher::Result value (as an int, to keep XenonUtils out of this header).
std::string_view xex_patcher_result_name(int result);

}  // namespace lr::pipeline
