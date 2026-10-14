# SPDX-License-Identifier: GPL-3.0-or-later
# SDL-free core (unit-tested); the executable is only built when SDL2 is available (see top-level CMakeLists).
add_library(lr_gui_core STATIC queue_sink.cpp ui_model.cpp)
target_include_directories(lr_gui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_link_libraries(lr_gui_core PUBLIC lr_common lr_pipeline)
lr_configure_target(lr_gui_core)

if(TARGET imgui)
  add_executable(legacyrecomp-gui main.cpp app.cpp)
  target_link_libraries(legacyrecomp-gui PRIVATE lr_gui_core lr_common lr_pipeline imgui SDL2::SDL2)
  lr_configure_target(legacyrecomp-gui)
endif()
