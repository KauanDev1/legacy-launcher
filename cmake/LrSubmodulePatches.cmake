# SPDX-License-Identifier: MIT
# Submodules with the LegacyRecomp patches applied (thirdparty/XenonRecomp, thirdparty/XenosRecomp;
# docs/adr/0004-m2-plan-deviations.md, docs/development.md).
#
# Each submodule stays pinned to its upstream; our changes live as `git format-patch` files in
# thirdparty/patches/<name in lower case>/. At configure time the submodule is mirrored into
# ${CMAKE_BINARY_DIR}/_deps/<name>-patched and the patches are applied to that copy, so the submodule checkout
# is never modified. Files no patch touches are kept in sync on every configure (file(COPY) skips files whose
# timestamp already matches); the files the patches touch are restored and re-patched only when a patch or one of
# those originals changes (stamp = hash of both), so an unchanged tree never recompiles.
#
#   lr_apply_submodule_patches(<name> <submodule dir> <patches dir> <out var copy dir>)
#
# Sets in the caller's scope:
#   <out var copy dir>        source dir to use (patched copy, or the submodule when off / no patches)
#   LR_<NAME>_PATCH_HASH      hash of the patch set and the originals it touches ("none" when nothing is applied)
# <NAME> is <name> in upper case. A missing or empty patches dir means "no patches" (the submodule is used as is).
#
# Developers editing a submodule on its `legacyrecomp` branch configure with -DLR_<NAME>_APPLY_PATCHES=OFF (the
# patches are already in that checkout), then regenerate the patch files.
function(lr_apply_submodule_patches name src patch_dir out_dir_var)
  string(TOUPPER "${name}" _up)
  option(LR_${_up}_APPLY_PATCHES "Build ${name} from a copy of the submodule with its LegacyRecomp patches applied" ON)
  set(_hash_var "LR_${_up}_PATCH_HASH")

  if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB _patches "${patch_dir}/*.patch")
  else()
    file(GLOB _patches CONFIGURE_DEPENDS "${patch_dir}/*.patch")
  endif()
  list(SORT _patches)

  if(NOT EXISTS "${src}/CMakeLists.txt")
    message(FATAL_ERROR "thirdparty/${name} is empty: run `git submodule update --init` (docs/development.md).")
  endif()

  if(NOT LR_${_up}_APPLY_PATCHES OR NOT _patches)
    set(${out_dir_var} "${src}" PARENT_SCOPE)
    set(${_hash_var} "none" PARENT_SCOPE)
    if(NOT LR_${_up}_APPLY_PATCHES AND _patches)
      message(STATUS "${name}: building the submodule as checked out (LR_${_up}_APPLY_PATCHES=OFF)")
    endif()
    return()
  endif()

  find_package(Git REQUIRED)

  # Files touched by the patches ("diff --git a/<path> b/<path>"), and the fingerprint of patches + originals.
  set(_touched "")
  set(_fingerprint "")
  foreach(_p IN LISTS _patches)
    file(SHA256 "${_p}" _h)
    get_filename_component(_n "${_p}" NAME)
    string(APPEND _fingerprint "patch ${_n} ${_h}\n")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_p}")
    endif()
    file(STRINGS "${_p}" _diffs REGEX "^diff --git a/")
    foreach(_d IN LISTS _diffs)
      string(REGEX REPLACE "^diff --git a/(.+) b/.+$" "\\1" _f "${_d}")
      if(_f MATCHES "(^|/)\\.\\.(/|$)" OR IS_ABSOLUTE "${_f}")
        message(FATAL_ERROR "${_n}: refusing to patch a path outside ${name}: ${_f}")
      endif()
      list(APPEND _touched "${_f}")
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES _touched)
  list(SORT _touched)
  # The patches are made from the LF blobs; a Windows checkout (core.autocrlf) has CRLF files, so the originals are
  # normalized to LF before patching and hashing (same result and fingerprint on every host).
  foreach(_f IN LISTS _touched)
    if(EXISTS "${src}/${_f}")
      file(READ "${src}/${_f}" _content)
      string(REPLACE "\r\n" "\n" _content "${_content}")
      string(SHA256 _h "${_content}")
      if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}/${_f}")
      endif()
    else()
      set(_h "absent")
    endif()
    string(APPEND _fingerprint "file ${_f} ${_h}\n")
  endforeach()
  string(SHA256 _patch_hash "${_fingerprint}")

  set(_copy "${CMAKE_BINARY_DIR}/_deps/${name}-patched")
  set(_stamp "${CMAKE_BINARY_DIR}/_deps/${name}-patched.stamp")

  # Mirror everything except the submodules' .git files and the files the patches own.
  set(_excludes PATTERN ".git" EXCLUDE)
  foreach(_f IN LISTS _touched)
    if(NOT _f MATCHES "^[A-Za-z0-9_./-]+$")
      message(FATAL_ERROR "${name} patches: unsupported characters in path ${_f}")
    endif()
    string(REPLACE "." "\\." _re "${_f}")
    list(APPEND _excludes REGEX "/${_re}$" EXCLUDE)
  endforeach()
  file(MAKE_DIRECTORY "${_copy}")
  file(COPY "${src}/" DESTINATION "${_copy}" ${_excludes})

  set(_old "")
  if(EXISTS "${_stamp}")
    file(READ "${_stamp}" _old)
  endif()
  # The file(COPY) above skips the patched files, so one deleted from the copy would never come back: re-patch then.
  set(_missing "")
  if(_old STREQUAL _patch_hash)
    foreach(_f IN LISTS _touched)
      if(EXISTS "${src}/${_f}" AND NOT EXISTS "${_copy}/${_f}")
        list(APPEND _missing "${_f}")
      endif()
    endforeach()
  endif()
  if(_missing)
    message(STATUS "${name}: patched file(s) missing from the copy, re-patching: ${_missing}")
  endif()
  if(NOT _old STREQUAL _patch_hash OR _missing)
    file(REMOVE "${_stamp}")
    foreach(_f IN LISTS _touched)
      if(EXISTS "${src}/${_f}")
        get_filename_component(_dir "${_copy}/${_f}" DIRECTORY)
        file(MAKE_DIRECTORY "${_dir}")
        file(READ "${src}/${_f}" _content)
        string(REPLACE "\r\n" "\n" _content "${_content}")
        file(WRITE "${_copy}/${_f}" "${_content}")
      else()
        file(REMOVE "${_copy}/${_f}")
      endif()
    endforeach()
    # The copy lives inside the build tree, which may sit inside our own work tree: the ceiling keeps git from
    # discovering that repository, so `git apply` works on plain files relative to the copy. Inside a repository
    # `git apply` run from a subdirectory would skip the paths outside it and still exit 0, so every patch must also
    # report each file it touches as applied (`-v`: "Applied patch <path> cleanly."), or configure stops.
    get_filename_component(_parent "${_copy}" DIRECTORY)
    foreach(_p IN LISTS _patches)
      get_filename_component(_n "${_p}" NAME)
      execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES=${_parent}"
                "${GIT_EXECUTABLE}" apply -v --whitespace=nowarn "${_p}"
        WORKING_DIRECTORY "${_copy}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
      if(NOT _rc EQUAL 0)
        message(FATAL_ERROR
          "Could not apply ${_n} to thirdparty/${name}:\n${_out}${_err}\n"
          "The submodule must be at the commit the patches were made against (git submodule update). "
          "When editing it on its legacyrecomp branch, configure with -DLR_${_up}_APPLY_PATCHES=OFF "
          "(docs/development.md).")
      endif()
      file(STRINGS "${_p}" _diffs REGEX "^diff --git a/")
      foreach(_d IN LISTS _diffs)
        string(REGEX REPLACE "^diff --git a/(.+) b/.+$" "\\1" _f "${_d}")
        string(FIND "${_out}${_err}" "Applied patch ${_f} cleanly." _pos)
        if(_pos EQUAL -1)
          file(REMOVE "${_stamp}")
          message(FATAL_ERROR
            "${_n}: git apply did not report ${_f} as applied in ${_copy}; refusing an unpatched "
            "${name}. git said:\n${_out}${_err}")
        endif()
      endforeach()
      string(STRIP "${_err}" _err)
      message(STATUS "${name}: ${_n}: ${_err}")
    endforeach()
    file(WRITE "${_stamp}" "${_patch_hash}")
    list(LENGTH _patches _count)
    message(STATUS "${name}: applied ${_count} LegacyRecomp patch(es) in ${_copy}")
  endif()

  set(${out_dir_var} "${_copy}" PARENT_SCOPE)
  set(${_hash_var} "${_patch_hash}" PARENT_SCOPE)
endfunction()
