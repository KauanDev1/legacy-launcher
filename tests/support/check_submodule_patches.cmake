# SPDX-License-Identifier: MIT
# Tests cmake/LrSubmodulePatches.cmake (lr_apply_submodule_patches) for both patched submodules, XenonRecomp and
# XenosRecomp: against the real submodule and its real patch dir (missing or empty dir = no patches), and against a
# synthetic submodule with a good and a bad patch (the "Applied patch ... cleanly" check must stop an unapplied one).
# Usage: cmake -DREPO=<source dir> -DWORK=<scratch dir> -P check_submodule_patches.cmake
#        (internal: -DCASE=bad -DNAME=<submodule> re-runs this script to observe a configure failure)
cmake_minimum_required(VERSION 3.25)
include("${REPO}/cmake/LrSubmodulePatches.cmake")

# cond is a condition for if(); embedded double quotes (written to keep the call sites readable) are dropped, so
# the rest is split at spaces, so values with spaces are compared through variables.
function(check cond what)
  string(REPLACE "\"" "" cond "${cond}")
  string(REPLACE " " ";" cond "${cond}")
  if(NOT (${cond}))
    message(FATAL_ERROR "FAILED: ${what}")
  endif()
endfunction()

# A synthetic submodule: CMakeLists.txt + a.txt; patch_dir gets a patch changing a.txt.
function(make_fake root name)
  file(REMOVE_RECURSE "${root}")
  file(MAKE_DIRECTORY "${root}/${name}" "${root}/patches")
  file(WRITE "${root}/${name}/CMakeLists.txt" "# fake\n")
  file(WRITE "${root}/${name}/a.txt" "hello\n")
  file(WRITE "${root}/patches/0001-good.patch"
"diff --git a/a.txt b/a.txt
--- a/a.txt
+++ b/a.txt
@@ -1 +1 @@
-hello
+hello patched
")
endfunction()

if(CASE STREQUAL "bad")
  # A patch whose context does not match the submodule: configure must fail.
  set(CMAKE_BINARY_DIR "${WORK}/bad-bin")
  make_fake("${WORK}/bad" ${NAME})
  file(WRITE "${WORK}/bad/${NAME}/a.txt" "something else\n")
  lr_apply_submodule_patches(${NAME} "${WORK}/bad/${NAME}" "${WORK}/bad/patches" OUT)
  return()
endif()

foreach(name XenonRecomp XenosRecomp)
  string(TOUPPER "${name}" up)
  string(TOLOWER "${name}" low)

  # --- the real submodule and its real patch dir ---
  set(CMAKE_BINARY_DIR "${WORK}/real-${low}")
  file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}")
  set(real_src "${REPO}/thirdparty/${name}")
  set(real_patches "${REPO}/thirdparty/patches/${low}")
  file(GLOB real_list "${real_patches}/*.patch")
  lr_apply_submodule_patches(${name} "${real_src}" "${real_patches}" OUT)
  if(real_list)
    check("OUT STREQUAL \"${CMAKE_BINARY_DIR}/_deps/${name}-patched\"" "${name}: patched copy is used")
    check("NOT LR_${up}_PATCH_HASH STREQUAL \"none\"" "${name}: patch hash is set")
    check("EXISTS \"${CMAKE_BINARY_DIR}/_deps/${name}-patched.stamp\"" "${name}: stamp written")
    # The same configure again is a no-op with the same hash.
    set(first_hash "${LR_${up}_PATCH_HASH}")
    lr_apply_submodule_patches(${name} "${real_src}" "${real_patches}" OUT)
    check("LR_${up}_PATCH_HASH STREQUAL first_hash" "${name}: hash is stable")
  else()
    check("OUT STREQUAL real_src" "${name}: no patches -> the submodule itself")
    check("LR_${up}_PATCH_HASH STREQUAL \"none\"" "${name}: no patches -> hash none")
  endif()

  # --- a missing patches dir is accepted ---
  lr_apply_submodule_patches(${name} "${real_src}" "${WORK}/does-not-exist" OUT)
  check("OUT STREQUAL real_src" "${name}: missing patch dir -> the submodule itself")
  check("LR_${up}_PATCH_HASH STREQUAL \"none\"" "${name}: missing patch dir -> hash none")

  # --- a synthetic submodule: the patch applies, only to the copy ---
  set(fake "${WORK}/fake-${low}")
  set(CMAKE_BINARY_DIR "${fake}-bin")
  make_fake("${fake}" ${name})
  file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}")
  lr_apply_submodule_patches(${name} "${fake}/${name}" "${fake}/patches" OUT)
  file(READ "${OUT}/a.txt" patched)
  file(READ "${fake}/${name}/a.txt" original)
  set(want_patched "hello patched
")
  check("patched STREQUAL want_patched" "${name}: patch applied to the copy")
  set(want_original "hello
")
  check("original STREQUAL want_original" "${name}: the submodule checkout is untouched")
  check("NOT LR_${up}_PATCH_HASH STREQUAL \"none\"" "${name}: synthetic patch hash is set")

  # --- APPLY_PATCHES=OFF uses the checkout as is ---
  set(LR_${up}_APPLY_PATCHES OFF CACHE BOOL "" FORCE)
  lr_apply_submodule_patches(${name} "${fake}/${name}" "${fake}/patches" OUT)
  check("OUT STREQUAL \"${fake}/${name}\"" "${name}: APPLY_PATCHES=OFF uses the submodule")
  check("LR_${up}_PATCH_HASH STREQUAL \"none\"" "${name}: APPLY_PATCHES=OFF -> hash none")
  set(LR_${up}_APPLY_PATCHES ON CACHE BOOL "" FORCE)

  # --- a patch that does not apply stops configure ---
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -DREPO=${REPO} -DWORK=${WORK}/bad-${low} -DCASE=bad -DNAME=${name} -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
  check("NOT rc EQUAL 0" "${name}: a patch that does not apply must fail configure")
  set(want_err "Could not apply 0001-good.patch")
  string(FIND "${err}" "${want_err}" err_pos)
  check("NOT err_pos EQUAL -1" "${name}: the failure names the patch")
endforeach()
message(STATUS "submodule patch mechanism: all checks passed")
