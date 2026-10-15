# SPDX-License-Identifier: MIT
# GPL tripwire (ADR 0002). XenonRecomp's bundled PowerPC disassembler (thirdparty/XenonRecomp/thirdparty/disasm,
# from GNU binutils) is GPL-2.0-or-later. It may only live inside the XenonAnalyse/XenonRecomp tools, never in our
# MIT binaries or in the runtime libraries we stage for the game link.
# Inputs: NM (nm program), BINARIES ("|"-separated executables to scan), RUNTIME_LIB_DIR (staged runtime/lib).
# Run: cmake -DNM=... -DBINARIES=a|b -DRUNTIME_LIB_DIR=... -P check_no_gpl_disassembler.cmake
set(_marker "decode_insn_ppc")  # defined only by the binutils disassembler (ppc-dis.c)

string(REPLACE "|" ";" _bins "${BINARIES}")
foreach(_bin IN LISTS _bins)
  if(NOT EXISTS "${_bin}")
    message(FATAL_ERROR "missing binary to scan: ${_bin}")
  endif()
  execute_process(COMMAND "${NM}" "${_bin}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${NM} failed on ${_bin}: ${_err}")
  endif()
  string(FIND "${_out}" "${_marker}" _pos)
  if(NOT _pos EQUAL -1)
    message(FATAL_ERROR "${_bin} contains ${_marker}: the GPL-2.0-or-later binutils disassembler got linked in")
  endif()
  message(STATUS "ok: no ${_marker} in ${_bin}")
endforeach()

if(EXISTS "${RUNTIME_LIB_DIR}/libdisasm.a")
  message(FATAL_ERROR "${RUNTIME_LIB_DIR}/libdisasm.a is staged; the game link must not get the GPL disassembler")
endif()
message(STATUS "ok: no libdisasm.a in ${RUNTIME_LIB_DIR}")
