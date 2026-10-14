# SPDX-License-Identifier: GPL-3.0-or-later
add_library(lr_common STATIC error.cpp log.cpp)
# src/ is the include root: #include "common/result.h"
target_include_directories(lr_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
lr_configure_target(lr_common)
