# SPDX-License-Identifier: MIT
# XenonRecomp with the LegacyRecomp patches applied (docs/adr/0004-m2-plan-deviations.md, docs/development.md).
#
# thirdparty/XenonRecomp stays pinned to upstream hedge-dev/XenonRecomp; our recompiler changes live as
# `git format-patch` files in thirdparty/patches/xenonrecomp/. At configure time the submodule is mirrored into
# ${CMAKE_BINARY_DIR}/_deps/XenonRecomp-patched and the patches are applied to that copy, so the submodule checkout
# is never modified. Files no patch touches are kept in sync on every configure (file(COPY) skips files whose
# timestamp already matches); the files the patches touch are restored and re-patched only when a patch or one of
# those originals changes (stamp = hash of both), so an unchanged tree never recompiles.
#
# Sets:
#   LR_XENONRECOMP_DIR         source dir to use for XenonRecomp files (patched copy, or the submodule when off)
#   LR_XENONRECOMP_PATCH_HASH  hash of the patch set and the originals it touches ("none" when nothing is applied)
#
# Developers editing the recompiler on the submodule's `legacyrecomp` branch configure with
# -DLR_XENONRECOMP_APPLY_PATCHES=OFF (the patches are already in that checkout), then regenerate the patch files.
option(LR_XENONRECOMP_APPLY_PATCHES "Build XenonRecomp from a copy of the submodule with thirdparty/patches/xenonrecomp applied" ON)

set(_lr_xr_src "${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp")
set(_lr_xr_patch_dir "${CMAKE_SOURCE_DIR}/thirdparty/patches/xenonrecomp")
file(GLOB _lr_xr_patches CONFIGURE_DEPENDS "${_lr_xr_patch_dir}/*.patch")
list(SORT _lr_xr_patches)

if(NOT EXISTS "${_lr_xr_src}/CMakeLists.txt")
  message(FATAL_ERROR "thirdparty/XenonRecomp is empty: run `git submodule update --init --recursive`.")
endif()

if(NOT LR_XENONRECOMP_APPLY_PATCHES OR NOT _lr_xr_patches)
  set(LR_XENONRECOMP_DIR "${_lr_xr_src}")
  set(LR_XENONRECOMP_PATCH_HASH "none")
  if(NOT LR_XENONRECOMP_APPLY_PATCHES AND _lr_xr_patches)
    message(STATUS "XenonRecomp: building the submodule as checked out (LR_XENONRECOMP_APPLY_PATCHES=OFF)")
  endif()
  return()
endif()

find_package(Git REQUIRED)

# Files touched by the patches ("diff --git a/<path> b/<path>"), and the fingerprint of patches + originals.
set(_lr_xr_touched "")
set(_lr_xr_fingerprint "")
foreach(_p IN LISTS _lr_xr_patches)
  file(SHA256 "${_p}" _h)
  get_filename_component(_n "${_p}" NAME)
  string(APPEND _lr_xr_fingerprint "patch ${_n} ${_h}\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_p}")
  file(STRINGS "${_p}" _diffs REGEX "^diff --git a/")
  foreach(_d IN LISTS _diffs)
    string(REGEX REPLACE "^diff --git a/(.+) b/.+$" "\\1" _f "${_d}")
    if(_f MATCHES "(^|/)\\.\\.(/|$)" OR IS_ABSOLUTE "${_f}")
      message(FATAL_ERROR "${_n}: refusing to patch a path outside XenonRecomp: ${_f}")
    endif()
    list(APPEND _lr_xr_touched "${_f}")
  endforeach()
endforeach()
list(REMOVE_DUPLICATES _lr_xr_touched)
list(SORT _lr_xr_touched)
# The patches are made from the LF blobs; a Windows checkout (core.autocrlf) has CRLF files, so the originals are
# normalized to LF before patching and hashing (same result and fingerprint on every host).
foreach(_f IN LISTS _lr_xr_touched)
  if(EXISTS "${_lr_xr_src}/${_f}")
    file(READ "${_lr_xr_src}/${_f}" _content)
    string(REPLACE "\r\n" "\n" _content "${_content}")
    string(SHA256 _h "${_content}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_lr_xr_src}/${_f}")
  else()
    set(_h "absent")
  endif()
  string(APPEND _lr_xr_fingerprint "file ${_f} ${_h}\n")
endforeach()
string(SHA256 LR_XENONRECOMP_PATCH_HASH "${_lr_xr_fingerprint}")

set(LR_XENONRECOMP_DIR "${CMAKE_BINARY_DIR}/_deps/XenonRecomp-patched")
set(_lr_xr_stamp "${CMAKE_BINARY_DIR}/_deps/XenonRecomp-patched.stamp")

# Mirror everything except the submodules' .git files and the files the patches own.
set(_lr_xr_excludes PATTERN ".git" EXCLUDE)
foreach(_f IN LISTS _lr_xr_touched)
  if(NOT _f MATCHES "^[A-Za-z0-9_./-]+$")
    message(FATAL_ERROR "XenonRecomp patches: unsupported characters in path ${_f}")
  endif()
  string(REPLACE "." "\\." _re "${_f}")
  list(APPEND _lr_xr_excludes REGEX "/${_re}$" EXCLUDE)
endforeach()
file(MAKE_DIRECTORY "${LR_XENONRECOMP_DIR}")
file(COPY "${_lr_xr_src}/" DESTINATION "${LR_XENONRECOMP_DIR}" ${_lr_xr_excludes})

set(_lr_xr_old "")
if(EXISTS "${_lr_xr_stamp}")
  file(READ "${_lr_xr_stamp}" _lr_xr_old)
endif()
# The file(COPY) above skips the patched files, so one deleted from the copy would never come back: re-patch then.
set(_lr_xr_missing "")
if(_lr_xr_old STREQUAL LR_XENONRECOMP_PATCH_HASH)
  foreach(_f IN LISTS _lr_xr_touched)
    if(EXISTS "${_lr_xr_src}/${_f}" AND NOT EXISTS "${LR_XENONRECOMP_DIR}/${_f}")
      list(APPEND _lr_xr_missing "${_f}")
    endif()
  endforeach()
endif()
if(_lr_xr_missing)
  message(STATUS "XenonRecomp: patched file(s) missing from the copy, re-patching: ${_lr_xr_missing}")
endif()
if(NOT _lr_xr_old STREQUAL LR_XENONRECOMP_PATCH_HASH OR _lr_xr_missing)
  file(REMOVE "${_lr_xr_stamp}")
  foreach(_f IN LISTS _lr_xr_touched)
    if(EXISTS "${_lr_xr_src}/${_f}")
      get_filename_component(_dir "${LR_XENONRECOMP_DIR}/${_f}" DIRECTORY)
      file(MAKE_DIRECTORY "${_dir}")
      file(READ "${_lr_xr_src}/${_f}" _content)
      string(REPLACE "\r\n" "\n" _content "${_content}")
      file(WRITE "${LR_XENONRECOMP_DIR}/${_f}" "${_content}")
    else()
      file(REMOVE "${LR_XENONRECOMP_DIR}/${_f}")
    endif()
  endforeach()
  # The copy lives inside the build tree, which may sit inside our own work tree: the ceiling keeps git from
  # discovering that repository, so `git apply` works on plain files relative to the copy. Inside a repository
  # `git apply` run from a subdirectory would skip the paths outside it and still exit 0, so every patch must also
  # report each file it touches as applied (`-v`: "Applied patch <path> cleanly."), or configure stops.
  get_filename_component(_lr_xr_parent "${LR_XENONRECOMP_DIR}" DIRECTORY)
  foreach(_p IN LISTS _lr_xr_patches)
    get_filename_component(_n "${_p}" NAME)
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES=${_lr_xr_parent}"
              "${GIT_EXECUTABLE}" apply -v --whitespace=nowarn "${_p}"
      WORKING_DIRECTORY "${LR_XENONRECOMP_DIR}"
      RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR
        "Could not apply ${_n} to thirdparty/XenonRecomp:\n${_out}${_err}\n"
        "The submodule must be at the commit the patches were made against (git submodule update). "
        "When editing the recompiler on its legacyrecomp branch, configure with -DLR_XENONRECOMP_APPLY_PATCHES=OFF "
        "(docs/development.md).")
    endif()
    file(STRINGS "${_p}" _diffs REGEX "^diff --git a/")
    foreach(_d IN LISTS _diffs)
      string(REGEX REPLACE "^diff --git a/(.+) b/.+$" "\\1" _f "${_d}")
      string(FIND "${_out}${_err}" "Applied patch ${_f} cleanly." _pos)
      if(_pos EQUAL -1)
        file(REMOVE "${_lr_xr_stamp}")
        message(FATAL_ERROR
          "${_n}: git apply did not report ${_f} as applied in ${LR_XENONRECOMP_DIR}; refusing an unpatched "
          "XenonRecomp. git said:\n${_out}${_err}")
      endif()
    endforeach()
    string(STRIP "${_err}" _err)
    message(STATUS "XenonRecomp: ${_n}: ${_err}")
  endforeach()
  file(WRITE "${_lr_xr_stamp}" "${LR_XENONRECOMP_PATCH_HASH}")
  list(LENGTH _lr_xr_patches _lr_xr_count)
  message(STATUS "XenonRecomp: applied ${_lr_xr_count} LegacyRecomp patch(es) in ${LR_XENONRECOMP_DIR}")
endif()
