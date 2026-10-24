# SPDX-License-Identifier: MIT
# Regression guard for the game template (src/runtime/template/CMakeLists.txt): the staged main.cpp and an import
# thunk file (render_import_thunks output) must compile against the STAGED headers only, exactly as the build stage
# compiles them. A header that main.cpp or the thunks include, directly or transitively, but resources.cmake does
# not stage makes this test fail instead of the user's install.
# Include path (template order): GENERATED (fake generated dir), PPC_CONTEXT_DIR (staged tools/, stand-in for the
# ppc_context.h XenonRecomp copies into work/generated), RUNTIME_INCLUDE (staged runtime/include), its simde/, then
# EXTRA_INCLUDES ("|"-separated: SDL2's headers, and tests/ for the header-only test helpers fake_recomp.cpp uses).
# Never src/: that is the point of the test.
# The template force-includes runtime/indirect_call.h (found through RUNTIME_INCLUDE), so the check does too, and
# SOURCES includes a file that expands PPC_CALL_INDIRECT_FUNC.
# HEADERS ("|"-separated, optional): staged headers that must each compile on their own (the Vulkan backend's
# runtime/gpu/vk/*.h, which need the staged volk.h and Vulkan headers).
# LINK_SOURCES and RUNTIME_LIB (optional, Task 17): the fake game's full source set is compiled and linked like the
# template links the game (liblr_runtime_core.a --whole-archive, liblr_vulkan.a, libXenonUtils.a from RUNTIME_LIB,
# then SDL2_LIB, pthread and dl), in WORK; the result must run `--coverage` with exit 0. So the staged Vulkan library
# is complete (no undefined symbol) and links into the game.
# Inputs: CXX, SOURCES ("|"-separated), GENERATED, PPC_CONTEXT_DIR, RUNTIME_INCLUDE, EXTRA_INCLUDES, MARCH (0/1),
# HEADERS, LINK_SOURCES ("|"-separated), RUNTIME_LIB, SDL2_LIB, WORK.
# Run: cmake -DCXX=clang++ -DSOURCES=a.cpp|b.cpp -DGENERATED=... ... -P check_template_compiles.cmake
string(REPLACE "|" ";" _sources "${SOURCES}")
string(REPLACE "|" ";" _extra "${EXTRA_INCLUDES}")
string(REPLACE "|" ";" _headers "${HEADERS}")
string(REPLACE "|" ";" _link_sources "${LINK_SOURCES}")
set(_flags -std=c++20 -fno-strict-aliasing -fexceptions -w -include runtime/indirect_call.h
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
  execute_process(COMMAND "${CXX}" ${_flags} -fsyntax-only "${_src}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out
                  ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${_src} does not compile against the staged runtime headers "
                        "(is a header missing from src/pipeline/resources.cmake?):\n${_out}${_err}")
  endif()
  message(STATUS "ok: ${_src} compiles against the staged headers")
endforeach()

foreach(_h IN LISTS _headers)
  if(_h STREQUAL "")
    continue()
  endif()
  if(NOT EXISTS "${RUNTIME_INCLUDE}/${_h}")
    message(FATAL_ERROR "staged header missing: ${RUNTIME_INCLUDE}/${_h}")
  endif()
  execute_process(COMMAND "${CXX}" ${_flags} -fsyntax-only -x c++ -
                  INPUT_FILE "${RUNTIME_INCLUDE}/${_h}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "staged header ${_h} does not compile on its own against the staged headers:\n${_out}${_err}")
  endif()
  message(STATUS "ok: ${_h} compiles on its own")
endforeach()

if(_link_sources)
  file(REMOVE_RECURSE "${WORK}")
  file(MAKE_DIRECTORY "${WORK}")
  set(_objects "")
  set(_i 0)
  foreach(_src IN LISTS _link_sources)
    get_filename_component(_name "${_src}" NAME_WE)
    set(_obj "${WORK}/${_i}_${_name}.o")
    math(EXPR _i "${_i} + 1")
    execute_process(COMMAND "${CXX}" ${_flags} -O1 -c "${_src}" -o "${_obj}" RESULT_VARIABLE _rc
                    OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "${_src} does not compile against the staged runtime headers:\n${_out}${_err}")
    endif()
    list(APPEND _objects "${_obj}")
  endforeach()
  set(_game "${WORK}/legacyrecomp-game")
  execute_process(COMMAND "${CXX}" ${_objects}
                          -Wl,--whole-archive "${RUNTIME_LIB}/liblr_runtime_core.a" -Wl,--no-whole-archive
                          "${RUNTIME_LIB}/liblr_vulkan.a" "${RUNTIME_LIB}/libXenonUtils.a" "${SDL2_LIB}"
                          -lpthread -ldl -o "${_game}"
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "the game does not link with the staged runtime libraries (runtime/lib):\n${_out}${_err}")
  endif()
  execute_process(COMMAND "${_game}" --coverage RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "the game linked with the staged libraries fails `--coverage` (exit ${_rc}):\n${_out}${_err}")
  endif()
  message(STATUS "ok: the game links with liblr_runtime_core.a, liblr_vulkan.a and libXenonUtils.a and runs")
endif()
