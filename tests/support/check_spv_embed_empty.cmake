# SPDX-License-Identifier: MIT
# cmake/LrSpvEmbed.cmake with an empty shader list (lr_compile_gpu_shaders called without sources) must still write
# lr_gpu_shaders.h / lr_gpu_shaders.cpp that compile, with kBuiltinShaderCount == 0.
# Inputs: CMAKE (the cmake binary), CXX, SCRIPT (cmake/LrSpvEmbed.cmake), WORK (scratch directory).
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${CMAKE}" "-DOUT_DIR=${WORK}" "-DSPVS=" "-DNAMES=" -P "${SCRIPT}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "LrSpvEmbed.cmake fails on an empty list:\n${_out}${_err}")
endif()
file(WRITE "${WORK}/check.cpp" "#include \"lr_gpu_shaders.h\"
static_assert(lr::gpu::vk::shaders::kBuiltinShaderCount == 0);
int main() { return lr::gpu::vk::shaders::kBuiltinShaders[0].words == nullptr ? 0 : 1; }
")
foreach(_src "${WORK}/lr_gpu_shaders.cpp" "${WORK}/check.cpp")
  execute_process(COMMAND "${CXX}" -std=c++20 -Wall -Wextra -Wpedantic -Werror -fsyntax-only "-I${WORK}" "${_src}"
                  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "${_src} (empty shader list) does not compile:\n${_out}${_err}")
  endif()
endforeach()
message(STATUS "ok: an empty shader list embeds and compiles")
