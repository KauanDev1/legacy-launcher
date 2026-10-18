# SPDX-License-Identifier: MIT
include(${CMAKE_CURRENT_SOURCE_DIR}/resources.cmake)

add_library(lr_pipeline STATIC state.cpp runner.cpp game_config.cpp types.cpp process.cpp doctor.cpp
  stages/doctor_stage.cpp stages/extract_stage.cpp stages/verify_stage.cpp
  stages/patch_stage.cpp stages/analyse_stage.cpp stages/recompile_stage.cpp stages/build_stage.cpp
  switch_scan.cpp import_stubs.cpp toolchain.cpp pipeline.cpp game_exit.cpp)
target_include_directories(lr_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_pipeline PUBLIC lr_common lr_formats tomlplusplus::tomlplusplus PRIVATE XenonUtils fmt::fmt)
target_compile_definitions(lr_pipeline PRIVATE LR_XENONRECOMP_COMMIT="${LR_XENONRECOMP_COMMIT}")
add_dependencies(lr_pipeline lr_resources)
lr_configure_target(lr_pipeline)
