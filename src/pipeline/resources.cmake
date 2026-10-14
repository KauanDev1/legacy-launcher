# SPDX-License-Identifier: GPL-3.0-or-later
# Stages the runtime resources into ${CMAKE_BINARY_DIR}/share/legacyrecomp (same layout as an install
# prefix's share/legacyrecomp): tools/{XenonAnalyse,XenonRecomp,ppc_context.h}, config/minecraft-tu1/*.
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

set(_ctx_src "${_lr_src_root}/thirdparty/XenonRecomp/XenonUtils/ppc_context.h")
set(_ctx_dst "${LR_RESOURCES_DIR}/tools/ppc_context.h")
add_custom_command(OUTPUT "${_ctx_dst}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ctx_src}" "${_ctx_dst}"
  DEPENDS "${_ctx_src}"
  COMMENT "Staging tools/ppc_context.h")
list(APPEND _lr_staged "${_ctx_dst}")

add_custom_target(lr_resources ALL
  DEPENDS ${_lr_staged} XenonAnalyse XenonRecomp
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LR_RESOURCES_DIR}/tools"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:XenonAnalyse>" "${LR_RESOURCES_DIR}/tools/XenonAnalyse"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:XenonRecomp>" "${LR_RESOURCES_DIR}/tools/XenonRecomp"
  COMMENT "Staging XenonAnalyse and XenonRecomp")

# Commit of the XenonRecomp submodule: part of the analyse/recompile fingerprints.
execute_process(COMMAND git -C "${_lr_src_root}/thirdparty/XenonRecomp" rev-parse HEAD
  OUTPUT_VARIABLE LR_XENONRECOMP_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _lr_git_rc ERROR_QUIET)
if(NOT _lr_git_rc EQUAL 0 OR LR_XENONRECOMP_COMMIT STREQUAL "")
  set(LR_XENONRECOMP_COMMIT "unknown")
  message(WARNING "Could not read the XenonRecomp submodule commit (git -C thirdparty/XenonRecomp rev-parse HEAD). "
                  "The analyse/recompile stage caches will not notice a XenonRecomp update; use --force for those stages.")
endif()
