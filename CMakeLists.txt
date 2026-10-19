# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)
project(LegacyRecomp LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
    "LegacyRecomp requires Clang (found '${CMAKE_CXX_COMPILER_ID}'). "
    "Install it with: sudo pacman -S --needed clang, then use `cmake --preset dev`.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/LrWarnings.cmake)
include(CTest)

# XenonRecomp's bundled fmt would otherwise add its headers/library to `cmake --install`.
set(FMT_INSTALL OFF CACHE BOOL "Install fmt (bundled by XenonRecomp)" FORCE)
# XenonRecomp = upstream submodule + thirdparty/patches/xenonrecomp (cmake/LrXenonRecompPatches.cmake).
include(cmake/LrXenonRecompPatches.cmake)
add_subdirectory(${LR_XENONRECOMP_DIR} ${CMAKE_BINARY_DIR}/thirdparty/XenonRecomp)
add_subdirectory(thirdparty/Catch2)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/Catch2/extras")

# Dear ImGui (SDL2 + SDL_Renderer2 backends) for the GUI launcher. Built without our strict flags (submodule).
find_package(SDL2 QUIET)
if(TARGET SDL2::SDL2)
  set(LR_IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui)
  add_library(imgui STATIC
    ${LR_IMGUI_DIR}/imgui.cpp ${LR_IMGUI_DIR}/imgui_draw.cpp ${LR_IMGUI_DIR}/imgui_tables.cpp
    ${LR_IMGUI_DIR}/imgui_widgets.cpp
    ${LR_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp ${LR_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${LR_IMGUI_DIR} ${LR_IMGUI_DIR}/backends)
  target_link_libraries(imgui PUBLIC SDL2::SDL2)
endif()

add_subdirectory(src)
add_subdirectory(tests)

# Install layout mirrors the build tree: <prefix>/bin/legacyrecomp* next to <prefix>/share/legacyrecomp (resources:
# tools, config, runtime libs/headers/template), which lr::resources_dir finds as <exe dir>/../share/legacyrecomp.
foreach(_lr_exe legacyrecomp legacyrecomp-gui)
  if(TARGET ${_lr_exe})
    install(TARGETS ${_lr_exe} RUNTIME DESTINATION bin)
  endif()
endforeach()
install(DIRECTORY "${LR_RESOURCES_DIR}/" DESTINATION share/legacyrecomp USE_SOURCE_PERMISSIONS)
