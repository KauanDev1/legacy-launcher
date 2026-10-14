# SPDX-License-Identifier: GPL-3.0-or-later
add_library(lr_pipeline STATIC state.cpp runner.cpp game_config.cpp types.cpp process.cpp doctor.cpp stages/doctor_stage.cpp stages/extract_stage.cpp stages/verify_stage.cpp)
target_include_directories(lr_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_pipeline PUBLIC lr_common lr_formats tomlplusplus::tomlplusplus)
lr_configure_target(lr_pipeline)
