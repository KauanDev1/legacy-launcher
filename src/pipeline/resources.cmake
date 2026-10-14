# SPDX-License-Identifier: GPL-3.0-or-later
# Stages the runtime resources into ${CMAKE_BINARY_DIR}/share/legacyrecomp (same layout as an install
# prefix's share/legacyrecomp): tools/{XenonAnalyse,XenonRecomp,ppc_context.h}, config/minecraft-tu1/*, and the
# game template inputs under runtime/:
#   runtime/template/CMakeLists.txt   the CMake project the build stage configures
#   runtime/src/main.cpp              game entry point, compiled by the template with the generated headers
#   runtime/include/{runtime,common}/ our headers (same include layout as src/)
#   runtime/include/simde/            SIMD Everywhere headers (ppc_context.h includes <x86/avx.h>)
#   runtime/lib/                      liblr_runtime_core.a (sanitizer-free staged variant, see src/runtime),
#                                     libXenonUtils.a, libdisasm.a
# and the GUI launcher's fonts (assets/fonts: Inter, JetBrains Mono and their OFL licence texts) under fonts/.
# ppc_context.h itself is not staged under runtime/: XenonRecomp copies it into work/generated.
set(LR_RESOURCES_DIR "${CMAKE_BINARY_DIR}/share/legacyrecomp" CACHE INTERNAL "Staged resources dir (build tree)")
set(_lr_src_root "${CMAKE_SOURCE_DIR}")

set(_lr_staged "")
foreach(_cfg game.toml recomp.toml)
  set(_dst "${LR_RESOURCES_DIR}/config/minecraft-tu1/${_cfg}")
  add_custom_command(OUTPUT "${_dst}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_lr_src_root}/config/minecraft-tu1/${_cfg}" "${_dst}"
    DEPENDS "${_lr_src_root}/config/minecraft-tu1/${_cfg}"
    COMMENT "Staging config/minecraft-tu1/${_cfg}")
  list(APPEND _lr_staged "${_dst}")
endforeach()

foreach(_font Inter-Regular.ttf Inter-SemiBold.ttf Inter-Bold.ttf JetBrainsMono-Regular.ttf
              LICENSE-Inter.txt LICENSE-JetBrainsMono.txt)
  set(_dst "${LR_RESOURCES_DIR}/fonts/${_font}")
  add_custom_command(OUTPUT "${_dst}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_lr_src_root}/assets/fonts/${_font}" "${_dst}"
    DEPENDS "${_lr_src_root}/assets/fonts/${_font}"
    COMMENT "Staging fonts/${_font}")
  list(APPEND _lr_staged "${_dst}")
endforeach()

set(_ctx_src "${_lr_src_root}/thirdparty/XenonRecomp/XenonUtils/ppc_context.h")
set(_ctx_dst "${LR_RESOURCES_DIR}/tools/ppc_context.h")
add_custom_command(OUTPUT "${_ctx_dst}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ctx_src}" "${_ctx_dst}"
  DEPENDS "${_ctx_src}"
  COMMENT "Staging tools/ppc_context.h")
list(APPEND _lr_staged "${_ctx_dst}")

# Runtime headers and the game template: one copy_if_different per file so unchanged files keep their mtime
# (the template's incremental build would otherwise recompile everything after every launcher build).
set(_lr_runtime_dir "${LR_RESOURCES_DIR}/runtime")
file(GLOB _lr_runtime_headers CONFIGURE_DEPENDS RELATIVE "${_lr_src_root}/src"
  "${_lr_src_root}/src/runtime/*.h" "${_lr_src_root}/src/runtime/kernel/*.h" "${_lr_src_root}/src/common/*.h")
set(_lr_runtime_files "")
foreach(_h IN LISTS _lr_runtime_headers)
  list(APPEND _lr_runtime_files "src/${_h}=include/${_h}")
endforeach()
list(APPEND _lr_runtime_files "src/runtime/main.cpp=src/main.cpp" "src/runtime/template/CMakeLists.txt=template/CMakeLists.txt")
foreach(_pair IN LISTS _lr_runtime_files)
  string(REPLACE "=" ";" _pair "${_pair}")
  list(GET _pair 0 _from)
  list(GET _pair 1 _to)
  add_custom_command(OUTPUT "${_lr_runtime_dir}/${_to}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_lr_src_root}/${_from}" "${_lr_runtime_dir}/${_to}"
    DEPENDS "${_lr_src_root}/${_from}"
    COMMENT "Staging runtime/${_to}")
  list(APPEND _lr_staged "${_lr_runtime_dir}/${_to}")
endforeach()

# simde: all headers plus its license, one copy_if_different per directory (CMake 3.25 lacks
# copy_directory_if_different); re-run only when one of its files changes, and unchanged files keep their mtime.
set(_lr_simde_src "${_lr_src_root}/thirdparty/XenonRecomp/thirdparty/simde")
file(GLOB_RECURSE _lr_simde_files CONFIGURE_DEPENDS RELATIVE "${_lr_simde_src}" "${_lr_simde_src}/*.h")
list(APPEND _lr_simde_files COPYING)
set(_lr_simde_dirs "")
foreach(_f IN LISTS _lr_simde_files)
  get_filename_component(_d "${_f}" DIRECTORY)
  list(APPEND _lr_simde_dirs "${_d}")
endforeach()
list(REMOVE_DUPLICATES _lr_simde_dirs)
set(_lr_simde_cmds "")
set(_lr_simde_deps "")
foreach(_d IN LISTS _lr_simde_dirs)
  set(_srcs "")
  foreach(_f IN LISTS _lr_simde_files)
    get_filename_component(_fd "${_f}" DIRECTORY)
    if(_fd STREQUAL _d)
      list(APPEND _srcs "${_lr_simde_src}/${_f}")
      list(APPEND _lr_simde_deps "${_lr_simde_src}/${_f}")
    endif()
  endforeach()
  list(APPEND _lr_simde_cmds COMMAND ${CMAKE_COMMAND} -E make_directory "${_lr_runtime_dir}/include/simde/${_d}"
                             COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_srcs} "${_lr_runtime_dir}/include/simde/${_d}")
endforeach()
set(_lr_simde_stamp "${CMAKE_CURRENT_BINARY_DIR}/simde.stamp")
add_custom_command(OUTPUT "${_lr_simde_stamp}"
  ${_lr_simde_cmds}
  COMMAND ${CMAKE_COMMAND} -E touch "${_lr_simde_stamp}"
  DEPENDS ${_lr_simde_deps}
  COMMENT "Staging runtime/include/simde")
list(APPEND _lr_staged "${_lr_simde_stamp}")

add_custom_target(lr_resources ALL
  DEPENDS ${_lr_staged} XenonAnalyse XenonRecomp lr_runtime_core_staged XenonUtils disasm
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_lr_runtime_dir}/lib"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:XenonUtils>" "${_lr_runtime_dir}/lib/libXenonUtils.a"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:disasm>" "${_lr_runtime_dir}/lib/libdisasm.a"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LR_RESOURCES_DIR}/tools"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:XenonAnalyse>" "${LR_RESOURCES_DIR}/tools/XenonAnalyse"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:XenonRecomp>" "${LR_RESOURCES_DIR}/tools/XenonRecomp"
  COMMENT "Staging XenonAnalyse, XenonRecomp and the runtime libraries")

# Commit of the XenonRecomp submodule: part of the analyse/recompile fingerprints.
execute_process(COMMAND git -C "${_lr_src_root}/thirdparty/XenonRecomp" rev-parse HEAD
  OUTPUT_VARIABLE LR_XENONRECOMP_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _lr_git_rc ERROR_QUIET)
if(NOT _lr_git_rc EQUAL 0 OR LR_XENONRECOMP_COMMIT STREQUAL "")
  set(LR_XENONRECOMP_COMMIT "unknown")
  message(WARNING "Could not read the XenonRecomp submodule commit (git -C thirdparty/XenonRecomp rev-parse HEAD). "
                  "The analyse/recompile stage caches will not notice a XenonRecomp update; use --force for those stages.")
endif()
