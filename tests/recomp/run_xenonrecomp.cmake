# SPDX-License-Identifier: MIT
# Runs XenonRecomp on <DIR>/recomp.toml and fails on any "ERROR:" line: XenonRecomp keeps going after errors
# (unknown instruction, missing helper address) and still exits 0.
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
if("${_filtered}" MATCHES "ERROR:")
  message(FATAL_ERROR "XenonRecomp reported errors (see output above)")
endif()
