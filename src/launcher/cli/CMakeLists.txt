# SPDX-License-Identifier: MIT
add_library(lr_cli STATIC args.cpp progress_bar.cpp)
target_include_directories(lr_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(lr_cli PUBLIC lr_common lr_pipeline)
lr_configure_target(lr_cli)

add_executable(legacyrecomp main.cpp)
target_link_libraries(legacyrecomp PRIVATE lr_cli lr_common lr_formats lr_pipeline XenonUtils)
lr_configure_target(legacyrecomp)
