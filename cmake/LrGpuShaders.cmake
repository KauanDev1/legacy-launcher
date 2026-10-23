# SPDX-License-Identifier: MIT
# lr_compile_gpu_shaders(<target> <shader sources...>)
#
# Compiles the renderer's own GLSL (src/runtime/gpu/vk/shaders) with glslangValidator at launcher build time and
# embeds the SPIR-V in <target> as uint32_t arrays (gpu_shaders.h / gpu_shaders.cpp, generated in the build tree,
# never committed). glslangValidator is a build tool only; an installed game compiles no shader. Each external
# command is an argv list (no shell string).
set(_LR_GPU_SHADERS_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(lr_compile_gpu_shaders target)
  find_program(LR_GLSLANG_VALIDATOR glslangValidator)
  if(NOT LR_GLSLANG_VALIDATOR)
    message(FATAL_ERROR
      "glslangValidator was not found. It compiles the renderer's own GLSL shaders at build time. "
      "Install it with: sudo pacman -S --needed glslang (see AGENTS.md).")
  endif()

  set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/gpu_shaders")
  file(MAKE_DIRECTORY "${out_dir}")
  set(spvs "")
  set(names "")
  foreach(src IN LISTS ARGN)
    get_filename_component(name "${src}" NAME)
    set(spv "${out_dir}/${name}.spv")
    add_custom_command(OUTPUT "${spv}"
      COMMAND "${LR_GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 -o "${spv}" "${src}"
      DEPENDS "${src}"
      COMMENT "glslangValidator ${name}"
      VERBATIM)
    list(APPEND spvs "${spv}")
    list(APPEND names "${name}")
  endforeach()

  list(JOIN spvs "|" spvs_arg)
  list(JOIN names "|" names_arg)
  add_custom_command(OUTPUT "${out_dir}/gpu_shaders.h" "${out_dir}/gpu_shaders.cpp"
    COMMAND "${CMAKE_COMMAND}" "-DOUT_DIR=${out_dir}" "-DSPVS=${spvs_arg}" "-DNAMES=${names_arg}"
            -P "${_LR_GPU_SHADERS_DIR}/LrSpvEmbed.cmake"
    DEPENDS ${spvs} "${_LR_GPU_SHADERS_DIR}/LrSpvEmbed.cmake"
    COMMENT "embedding the built-in shaders"
    VERBATIM)
  target_sources(${target} PRIVATE "${out_dir}/gpu_shaders.cpp" "${out_dir}/gpu_shaders.h")
  target_include_directories(${target} PUBLIC "${out_dir}")
endfunction()
