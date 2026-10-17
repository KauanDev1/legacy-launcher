# SPDX-License-Identifier: MIT
# Runs XenonRecomp on <DIR>/recomp.toml and fails when it reports a problem. XenonRecomp keeps going after
# errors and still exits 0, and several of its diagnostics (dropped or undecodable instructions) have no
# "ERROR:" prefix, so those are matched explicitly (see XenonRecomp/recompiler.cpp).
# Usage: cmake -DXENON=<exe> -DCONTEXT_H=<ppc_context.h> -DDIR=<dir> -P run_xenonrecomp.cmake
execute_process(COMMAND "${XENON}" recomp.toml "${CONTEXT_H}"
  WORKING_DIRECTORY "${DIR}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
message(STATUS "${_out}${_err}")
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "XenonRecomp exited with ${_rc}")
endif()
# The helper addresses (__savegprlr_14 etc.) are left unset on purpose: setting them makes XenonRecomp emit
# functions at those addresses, which must exist in the image. Its "address is unspecified" notices are expected.
string(REGEX REPLACE "ERROR: __[a-z]+_(14|64) address is unspecified" "" _filtered "${_out}${_err}")
set(_patterns "ERROR:" "Unrecognized instruction" "Unable to decode instruction" "with no switch table entry"
  "has RC bit enabled but no comparison" "Unexpected D3D color pack" "Unexpected float16_4 pack")
foreach(_p IN LISTS _patterns)
  string(FIND "${_filtered}" "${_p}" _pos)
  if(NOT _pos EQUAL -1)
    string(SUBSTRING "${_filtered}" ${_pos} 120 _ctx)
    string(REGEX REPLACE "\n.*" "" _ctx "${_ctx}")
    message(FATAL_ERROR "XenonRecomp dropped or rejected code: ${_ctx}")
  endif()
endforeach()
