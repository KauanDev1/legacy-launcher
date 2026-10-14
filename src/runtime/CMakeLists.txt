# SPDX-License-Identifier: GPL-3.0-or-later
add_library(lr_runtime_core STATIC guest_memory.cpp fault.cpp func_table.cpp loader.cpp kernel/registry.cpp guest_thread.cpp)
target_include_directories(lr_runtime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_runtime_core PUBLIC lr_common PRIVATE XenonUtils)
lr_configure_target(lr_runtime_core)
