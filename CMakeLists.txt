cmake_minimum_required(VERSION 3.25)
project(LegacyRecomp LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
    "LegacyRecomp requires Clang (found '${CMAKE_CXX_COMPILER_ID}'). "
    "Install it with: sudo pacman -S --needed clang lld llvm, then use `cmake --preset dev`.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/LrWarnings.cmake)
include(CTest)

# XenonRecomp's bundled fmt would otherwise add its headers/library to `cmake --install`.
set(FMT_INSTALL OFF CACHE BOOL "Install fmt (bundled by XenonRecomp)" FORCE)
add_subdirectory(thirdparty/XenonRecomp)
add_subdirectory(thirdparty/Catch2)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/Catch2/extras")

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
