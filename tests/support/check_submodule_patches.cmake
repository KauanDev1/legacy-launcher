# SPDX-License-Identifier: MIT
# Tests cmake/LrSubmodulePatches.cmake (lr_apply_submodule_patches) for both patched submodules, XenonRecomp and
# XenosRecomp: against the real submodule and its real patch dir (missing or empty dir = no patches), and against a
# synthetic submodule: a good patch, re-configure detection (changed patch and source, a patched file deleted from the
# copy), a patch that does not apply, and a patch that git applies with exit 0 but does not report as "Applied patch
# <file> cleanly." (the guard against an unpatched build).
# Usage: cmake -DREPO=<source dir> -DWORK=<scratch dir> -P check_submodule_patches.cmake
#        (internal: -DCASE=bad|unreported -DNAME=<submodule> re-runs this script to observe a configure failure)
cmake_minimum_required(VERSION 3.25)
include("${REPO}/cmake/LrSubmodulePatches.cmake")

# check(<what> <condition...>): the condition is if() syntax made of variable NAMES and keywords, never of expanded
# values, so paths with spaces or semicolons cannot break it.
function(check what)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAILED: ${what}")
  endif()
endfunction()

function(write_patch file from to)
  file(WRITE "${file}"
"diff --git a/a.txt b/a.txt
--- a/a.txt
+++ b/a.txt
@@ -1 +1 @@
-${from}
+${to}
")
endfunction()

# A synthetic submodule: <root>/<name>/{CMakeLists.txt,a.txt} and <root>/patches/0001-good.patch (hello -> patched).
function(make_fake root name)
  file(REMOVE_RECURSE "${root}")
  file(MAKE_DIRECTORY "${root}/${name}" "${root}/patches")
  file(WRITE "${root}/${name}/CMakeLists.txt" "# fake\n")
  file(WRITE "${root}/${name}/a.txt" "hello\n")
  write_patch("${root}/patches/0001-good.patch" "hello" "hello patched")
endfunction()

if(CASE STREQUAL "bad")
  # A patch whose context does not match the submodule: configure must fail.
  set(CMAKE_BINARY_DIR "${WORK}/bin")
  make_fake("${WORK}/src" ${NAME})
  file(WRITE "${WORK}/src/${NAME}/a.txt" "something else\n")
  lr_apply_submodule_patches(${NAME} "${WORK}/src/${NAME}" "${WORK}/src/patches" OUT)
  return()
elseif(CASE STREQUAL "unreported")
  # A rename: git apply exits 0 and reports "a.txt => c.txt", not "Applied patch a.txt cleanly.": the guard stops it.
  set(CMAKE_BINARY_DIR "${WORK}/bin")
  make_fake("${WORK}/src" ${NAME})
  file(WRITE "${WORK}/src/patches/0001-good.patch"
"diff --git a/a.txt b/c.txt
similarity index 100%
rename from a.txt
rename to c.txt
")
  lr_apply_submodule_patches(${NAME} "${WORK}/src/${NAME}" "${WORK}/src/patches" OUT)
  return()
endif()

foreach(name XenonRecomp XenosRecomp)
  string(TOUPPER "${name}" up)
  string(TOLOWER "${name}" low)
  set(hash_var LR_${up}_PATCH_HASH)

  # --- the real submodule and its real patch dir ---
  set(CMAKE_BINARY_DIR "${WORK}/real-${low}")
  file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}")
  set(real_src "${REPO}/thirdparty/${name}")
  set(real_patches "${REPO}/thirdparty/patches/${low}")
  set(real_copy "${CMAKE_BINARY_DIR}/_deps/${name}-patched")
  set(real_stamp "${CMAKE_BINARY_DIR}/_deps/${name}-patched.stamp")
  file(GLOB real_list "${real_patches}/*.patch")
  lr_apply_submodule_patches(${name} "${real_src}" "${real_patches}" OUT)
  if(real_list)
    check("${name}: patched copy is used" OUT STREQUAL real_copy)
    check("${name}: patch hash is set" NOT ${hash_var} STREQUAL none)
    check("${name}: stamp written" EXISTS "${real_stamp}")
    set(first_hash "${${hash_var}}")
    lr_apply_submodule_patches(${name} "${real_src}" "${real_patches}" OUT)  # the same configure again
    check("${name}: hash is stable" ${hash_var} STREQUAL first_hash)
  else()
    check("${name}: no patches -> the submodule itself" OUT STREQUAL real_src)
    check("${name}: no patches -> hash none" ${hash_var} STREQUAL none)
  endif()

  # --- a missing patches dir is accepted ---
  lr_apply_submodule_patches(${name} "${real_src}" "${WORK}/does-not-exist" OUT)
  check("${name}: missing patch dir -> the submodule itself" OUT STREQUAL real_src)
  check("${name}: missing patch dir -> hash none" ${hash_var} STREQUAL none)

  # --- a synthetic submodule: the patch applies, only to the copy ---
  set(fake "${WORK}/fake-${low}")
  set(fake_src "${fake}/${name}")
  set(fake_patches "${fake}/patches")
  set(CMAKE_BINARY_DIR "${fake}-bin")
  file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}")
  make_fake("${fake}" ${name})
  lr_apply_submodule_patches(${name} "${fake_src}" "${fake_patches}" OUT)
  set(copy "${OUT}")
  file(READ "${copy}/a.txt" patched)
  file(READ "${fake_src}/a.txt" original)
  set(want_patched "hello patched\n")
  set(want_original "hello\n")
  check("${name}: patch applied to the copy" patched STREQUAL want_patched)
  check("${name}: the submodule checkout is untouched" original STREQUAL want_original)
  check("${name}: synthetic patch hash is set" NOT ${hash_var} STREQUAL none)
  set(hash1 "${${hash_var}}")

  # --- re-configure: unchanged input is stable ---
  lr_apply_submodule_patches(${name} "${fake_src}" "${fake_patches}" OUT)
  check("${name}: unchanged patch set keeps the hash" ${hash_var} STREQUAL hash1)

  # --- re-configure: a rewritten patch and source change the hash and the content ---
  file(WRITE "${fake_src}/a.txt" "world\n")
  write_patch("${fake_patches}/0001-good.patch" "world" "world patched")
  lr_apply_submodule_patches(${name} "${fake_src}" "${fake_patches}" OUT)
  check("${name}: changed patch and source change the hash" NOT ${hash_var} STREQUAL hash1)
  file(READ "${copy}/a.txt" patched)
  set(want_patched "world patched\n")
  check("${name}: the copy is re-patched" patched STREQUAL want_patched)
  set(hash2 "${${hash_var}}")

  # --- re-configure: a patched file deleted from the copy is restored ---
  file(REMOVE "${copy}/a.txt")
  lr_apply_submodule_patches(${name} "${fake_src}" "${fake_patches}" OUT)
  check("${name}: the deleted patched file is restored" EXISTS "${copy}/a.txt")
  file(READ "${copy}/a.txt" patched)
  check("${name}: the restored file is patched" patched STREQUAL want_patched)
  check("${name}: restoring keeps the hash" ${hash_var} STREQUAL hash2)

  # --- APPLY_PATCHES=OFF uses the checkout as is ---
  set(LR_${up}_APPLY_PATCHES OFF CACHE BOOL "" FORCE)
  lr_apply_submodule_patches(${name} "${fake_src}" "${fake_patches}" OUT)
  check("${name}: APPLY_PATCHES=OFF uses the submodule" OUT STREQUAL fake_src)
  check("${name}: APPLY_PATCHES=OFF -> hash none" ${hash_var} STREQUAL none)
  set(LR_${up}_APPLY_PATCHES ON CACHE BOOL "" FORCE)

  # --- failures stop configure (the script runs again to observe them) ---
  foreach(case bad unreported)
    execute_process(
      COMMAND "${CMAKE_COMMAND}" "-DREPO=${REPO}" "-DWORK=${WORK}/${case}-${low}" "-DCASE=${case}" "-DNAME=${name}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    check("${name}: case ${case} must fail configure" NOT rc EQUAL 0)
    if(case STREQUAL "bad")
      string(FIND "${err}" "Could not apply 0001-good.patch" pos)
    else()
      string(FIND "${err}" "git apply did not report a.txt as applied" pos)
    endif()
    check("${name}: case ${case} fails with the expected error" NOT pos EQUAL -1)
  endforeach()
endforeach()
message(STATUS "submodule patch mechanism: all checks passed")
