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
# XenonRecomp = upstream submodule + thirdparty/patches/xenonrecomp (cmake/LrSubmodulePatches.cmake).
include(cmake/LrSubmodulePatches.cmake)
lr_apply_submodule_patches(XenonRecomp "${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp"
  "${CMAKE_SOURCE_DIR}/thirdparty/patches/xenonrecomp" LR_XENONRECOMP_DIR)
add_subdirectory(${LR_XENONRECOMP_DIR} ${CMAKE_BINARY_DIR}/thirdparty/XenonRecomp)

# XenosRecomp (Xenos shader microcode -> HLSL -> SPIR-V, host tool run at install time): upstream submodule +
# thirdparty/patches/xenosrecomp. It needs DXC (dxcapi.h and libdxcompiler.so) from the system (the
# directx-shader-compiler package; ADR 0005): we define Microsoft::DirectXShaderCompiler ourselves, so upstream's
# dxc-bin submodule (prebuilt binaries) is never initialised. fmt::fmt and xxHash::xxhash come from XenonRecomp's
# bundled copies (same commits as XenosRecomp's own submodules), so only smol-v and zstd are nested submodules we
# need. Added EXCLUDE_FROM_ALL: it builds only as a dependency of lr_resources, and none of its install rules
# (zstd's headers and libraries) reach `cmake --install`.
lr_apply_submodule_patches(XenosRecomp "${CMAKE_SOURCE_DIR}/thirdparty/XenosRecomp"
  "${CMAKE_SOURCE_DIR}/thirdparty/patches/xenosrecomp" LR_XENOSRECOMP_DIR)
option(LR_BUILD_XENOSRECOMP "Build the XenosRecomp shader translator (needs the system DXC)" ON)
set(LR_DXC_ROOT "" CACHE PATH "Prefix of a DXC install (include/dxc, lib/libdxcompiler.so); empty = system paths")
find_path(LR_DXC_INCLUDE_DIR dxcapi.h HINTS "${LR_DXC_ROOT}/include" PATH_SUFFIXES dxc)
find_library(LR_DXC_LIBRARY dxcompiler HINTS "${LR_DXC_ROOT}/lib")
if(LR_BUILD_XENOSRECOMP AND LR_DXC_INCLUDE_DIR AND LR_DXC_LIBRARY)
  add_library(Microsoft::DirectXShaderCompiler SHARED IMPORTED GLOBAL)
  set_target_properties(Microsoft::DirectXShaderCompiler PROPERTIES
    IMPORTED_LOCATION "${LR_DXC_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${LR_DXC_INCLUDE_DIR}")
  set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  add_subdirectory(${LR_XENOSRECOMP_DIR} ${CMAKE_BINARY_DIR}/thirdparty/XenosRecomp EXCLUDE_FROM_ALL)
  # LegacyRecomp mode of the fork (thirdparty/patches/xenosrecomp): the emulated-GPU shader contract, microcode
  # hashes and the shader cache format of docs/architecture.md; the Unleashed Recompiled paths are compiled out.
  target_compile_definitions(XenosRecomp PRIVATE LR_LEGACYRECOMP)
  set(LR_HAVE_XENOSRECOMP ON)
else()
  set(LR_HAVE_XENOSRECOMP OFF)
  if(LR_BUILD_XENOSRECOMP)
    message(WARNING "DXC (dxcapi.h, libdxcompiler.so) not found: XenosRecomp is not built. Install the "
                  "directx-shader-compiler package or pass -DLR_DXC_ROOT=<prefix>.")
  endif()
endif()
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

# Vulkan loader (volk), allocator (VMA) and headers: third-party, built without our strict flags. VK_NO_PROTOTYPES
# because every entry point comes from volk; lr_vulkan (src/runtime) is our wrapper on top of this.
add_library(lr_vulkan_thirdparty STATIC
  thirdparty/volk/volk.c src/runtime/gpu/vk/vma_impl.cpp)
target_include_directories(lr_vulkan_thirdparty SYSTEM PUBLIC
  thirdparty/Vulkan-Headers/include thirdparty/volk thirdparty/VulkanMemoryAllocator/include)
target_compile_definitions(lr_vulkan_thirdparty PUBLIC VK_NO_PROTOTYPES
  VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1 VMA_VULKAN_VERSION=1002000)
target_link_libraries(lr_vulkan_thirdparty PUBLIC ${CMAKE_DL_LIBS})
set_target_properties(lr_vulkan_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
