# SPDX-License-Identifier: GPL-3.0-or-later
# Game template: builds legacyrecomp-game from the XenonRecomp output and the prebuilt runtime.
# Configured by the pipeline's build stage (src/pipeline/stages/build_stage.cpp), never by the main project:
#   cmake -S <resources>/runtime/template -B <root>/work/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#         -DCMAKE_CXX_COMPILER=clang++ -DLR_RESOURCES=<resources> -DLR_GENERATED=<root>/work/generated
# Inputs: LR_GENERATED holds ppc_config.h, ppc_context.h (copied there by XenonRecomp), ppc_recomp_shared.h,
# ppc_recomp.*.cpp, ppc_func_mapping.cpp and import_stubs.cpp (written by the build stage).
cmake_minimum_required(VERSION 3.25)
project(LegacyRecompGame LANGUAGES CXX)

foreach(_var LR_RESOURCES LR_GENERATED)
  if(NOT ${_var} OR NOT IS_DIRECTORY "${${_var}}")
    message(FATAL_ERROR "${_var} must name an existing directory (got '${${_var}}')")
  endif()
endforeach()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "The game must be built with Clang (XenonRecomp output relies on Clang extensions); "
                      "found '${CMAKE_CXX_COMPILER_ID}'.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)

set(_rt "${LR_RESOURCES}/runtime")
file(GLOB _lr_recomp_sources CONFIGURE_DEPENDS "${LR_GENERATED}/ppc_recomp.*.cpp")
if(NOT _lr_recomp_sources)
  message(FATAL_ERROR "no ppc_recomp.*.cpp in ${LR_GENERATED}: run the recompile stage first")
endif()

add_executable(legacyrecomp-game
  "${_rt}/src/main.cpp"
  ${_lr_recomp_sources}
  "${LR_GENERATED}/ppc_func_mapping.cpp"
  "${LR_GENERATED}/import_stubs.cpp")
# Generated dir first: ppc_config.h / ppc_context.h must be the game's own.
target_include_directories(legacyrecomp-game PRIVATE "${LR_GENERATED}" "${_rt}/include" "${_rt}/include/simde")

# Flags (XenonRecomp's README asks only for Clang; the rest follows UnleashedRecomp, the reference XenonRecomp game):
#  -O2                      Release; the generated code is far too slow and large at -O0.
#  -fno-strict-aliasing     generated code type-puns PPCRegister / guest memory (UnleashedRecomp uses it too).
#  -ffp-model=strict        UnleashedRecomp's setting for the generated code: no FMA contraction, and the compiler
#                           respects the FP environment the code changes (rounding mode / flush-to-zero via MXCSR).
#  -fexceptions             default, kept explicit: lr::kernel::UnimplementedImport unwinds through generated frames.
#  -march=sandybridge       x86-64 baseline UnleashedRecomp targets; lets simde map ppc_context.h's VMX helpers to
#                           real SSE4.1/AVX instructions instead of scalar fallbacks. REQUIREMENT: the host CPU
#                           must support AVX (2011+); the doctor stage refuses CPUs without it, and the runtime's
#                           fault handler adds a hint when a SIGILL happens in host code.
#  -Wno-*                   XenonRecomp output trips these by design; the template never uses -Werror.
target_compile_options(legacyrecomp-game PRIVATE
  -O2 -fno-strict-aliasing -ffp-model=strict -fexceptions
  $<$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x86_64>:-march=sandybridge>
  -Wno-switch -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label
  -Wno-null-arithmetic -Wno-null-conversion -Wno-tautological-undefined-compare -Wno-void-pointer-to-int-cast
  -Wno-int-to-void-pointer-cast -Wno-invalid-offsetof -Wno-c++23-extensions)

# --whole-archive: every strong LR_KERNEL_EXPORT in the runtime must win over the weak stubs in import_stubs.cpp,
# even when nothing else references its object file.
target_link_libraries(legacyrecomp-game PRIVATE
  "-Wl,--whole-archive" "${_rt}/lib/liblr_runtime_core.a" "-Wl,--no-whole-archive"
  "${_rt}/lib/libXenonUtils.a" "${_rt}/lib/libdisasm.a"
  SDL2::SDL2 Threads::Threads)
set_target_properties(legacyrecomp-game PROPERTIES LINK_DEPENDS
  "${_rt}/lib/liblr_runtime_core.a;${_rt}/lib/libXenonUtils.a;${_rt}/lib/libdisasm.a")
