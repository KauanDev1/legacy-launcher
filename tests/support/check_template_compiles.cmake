# SPDX-License-Identifier: MIT
# Regression guard for the game template (src/runtime/template/CMakeLists.txt): the staged main.cpp and an import
# thunk file (render_import_thunks output) must compile against the STAGED headers only, exactly as the build stage
# compiles them. A header that main.cpp or the thunks include, directly or transitively, but resources.cmake does
# not stage makes this test fail instead of the user's install.
# Include path (template order): GENERATED (fake generated dir), PPC_CONTEXT_DIR (staged tools/, stand-in for the
# ppc_context.h XenonRecomp copies into work/generated), RUNTIME_INCLUDE (staged runtime/include), its simde/, then
# EXTRA_INCLUDES ("|"-separated, SDL2's headers). Never src/: that is the point of the test.
# Inputs: CXX, SOURCES ("|"-separated), GENERATED, PPC_CONTEXT_DIR, RUNTIME_INCLUDE, EXTRA_INCLUDES, MARCH (0/1).
# Run: cmake -DCXX=clang++ -DSOURCES=a.cpp|b.cpp -DGENERATED=... ... -P check_template_compiles.cmake
string(REPLACE "|" ";" _sources "${SOURCES}")
string(REPLACE "|" ";" _extra "${EXTRA_INCLUDES}")
set(_flags -std=c++20 -fsyntax-only -fno-strict-aliasing -fexceptions -w
  "-I${GENERATED}" "-I${PPC_CONTEXT_DIR}" "-I${RUNTIME_INCLUDE}" "-I${RUNTIME_INCLUDE}/simde")
foreach(_dir IN LISTS _extra)
  if(NOT _dir STREQUAL "")
    list(APPEND _flags "-I${_dir}")
  endif()
endforeach()
if(MARCH)
  list(APPEND _flags -march=sandybridge)
endif()

foreach(_src IN LISTS _sources)
  if(NOT EXISTS "${_src}")
    message(FATAL_ERROR "missing source to check: ${_src}")
  endif()
  execute_process(COMMAND "${CXX}" ${_flags} "${_src}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${_src} does not compile against the staged runtime headers "
                        "(is a header missing from src/pipeline/resources.cmake?):\n${_out}${_err}")
  endif()
  message(STATUS "ok: ${_src} compiles against the staged headers")
endforeach()
