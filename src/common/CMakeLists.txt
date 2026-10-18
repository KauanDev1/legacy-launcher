# SPDX-License-Identifier: MIT
add_library(lr_common STATIC error.cpp log.cpp sha256.cpp sha1.cpp paths.cpp fs_util.cpp)
# src/ is the include root: #include "common/result.h"
target_include_directories(lr_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
lr_configure_target(lr_common)
