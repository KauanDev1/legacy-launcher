# SPDX-License-Identifier: MIT
# Test: packaging/appimage/build.sh refuses to start when glslangValidator is not on PATH. Runs the real script with a
# PATH that holds only the few programs the script needs before its tool check (no glslangValidator), so the AppImage
# build never starts. Inputs: BASH, SCRIPT.
set(bin "${WORK}/bin")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${bin}" "${WORK}/out")
foreach(tool dirname nproc mkdir)
  find_program(path_${tool} ${tool})
  if(NOT path_${tool})
    message(FATAL_ERROR "${tool} not found")
  endif()
  file(CREATE_LINK "${path_${tool}}" "${bin}/${tool}" SYMBOLIC)
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "PATH=${bin}" "${BASH}" "${SCRIPT}" --out "${WORK}/out"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0)
  message(FATAL_ERROR "build.sh succeeded without glslangValidator")
endif()
if(NOT err MATCHES "glslangValidator is required" OR NOT err MATCHES "pacman -S --needed glslang")
  message(FATAL_ERROR "build.sh did not refuse with the glslangValidator install hint (exit ${rc}):\n${err}")
endif()
