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

add_subdirectory(thirdparty/XenonRecomp)
add_subdirectory(thirdparty/Catch2)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/Catch2/extras")

add_subdirectory(src)
add_subdirectory(tests)
