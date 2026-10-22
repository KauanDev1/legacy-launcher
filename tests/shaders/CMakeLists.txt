# SPDX-License-Identifier: MIT
# XenosRecomp harness: synthetic shader containers (cases.h) -> patched XenosRecomp + DXC (at build time) ->
# shader_cache.cpp compiled into lr_shader_tests, which checks hashes, SPIR-V and the dumped HLSL, and that the
# runtime ShaderCache finds every shader by the hash of its loaded microcode (test_shader_binding.cpp). Broken
# containers run as plain ctest commands that expect the tool to fail. Only built when XenosRecomp is.
set(LR_SHADER_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(LR_SHADER_CACHE ${LR_SHADER_GEN}/out/shader_cache.cpp)
set(LR_SHADER_HLSL ${LR_SHADER_GEN}/out/hlsl)
set(LR_SHADER_LOG ${LR_SHADER_GEN}/out/xenosrecomp.log)
set(LR_SHADER_COMMON ${LR_XENOSRECOMP_DIR}/XenosRecomp/shader_common.h)

add_executable(lr_gen_shader_fixture gen_shader_fixture.cpp)
target_include_directories(lr_gen_shader_fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests)
lr_configure_target(lr_gen_shader_fixture)

add_custom_command(OUTPUT ${LR_SHADER_CACHE} ${LR_SHADER_LOG}
  COMMAND lr_gen_shader_fixture ${LR_SHADER_GEN}
  COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:XenosRecomp> -DINCLUDE=${LR_SHADER_COMMON}
          -DIN=${LR_SHADER_GEN}/in -DOUT=${LR_SHADER_CACHE} -DHLSL=${LR_SHADER_HLSL} -DLOG=${LR_SHADER_LOG}
          -DEXPECT=success -P ${CMAKE_CURRENT_SOURCE_DIR}/run_xenosrecomp.cmake
  DEPENDS lr_gen_shader_fixture XenosRecomp ${LR_SHADER_COMMON} ${CMAKE_CURRENT_SOURCE_DIR}/run_xenosrecomp.cmake
          ${CMAKE_CURRENT_SOURCE_DIR}/cases.h ${CMAKE_SOURCE_DIR}/tests/support/xenos_shader_builder.h
  COMMENT "Translating synthetic Xenos shaders with XenosRecomp" VERBATIM)

add_executable(lr_shader_tests test_xenosrecomp.cpp test_shader_binding.cpp ${LR_SHADER_CACHE})
target_include_directories(lr_shader_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests
  ${LR_XENOSRECOMP_DIR}/XenosRecomp)
# lr_runtime_core: the runtime ShaderCache (test_shader_binding.cpp); lr_zstd also serves the harness' own decompression.
target_link_libraries(lr_shader_tests PRIVATE Catch2::Catch2WithMain xxHash::xxhash lr_runtime_core lr_zstd)
# LR_LEGACYRECOMP: the LegacyRecomp layout macros of shader_common.h.
target_compile_definitions(lr_shader_tests PRIVATE LR_LEGACYRECOMP LR_SHADER_HLSL_DIR="${LR_SHADER_HLSL}"
  LR_SHADER_LOG="${LR_SHADER_LOG}" LR_SHADER_COMMON_PATH="${LR_SHADER_COMMON}")
# The test source is ours: strict warnings and sanitizers. The generated cache is a data table; it stays plain.
set_source_files_properties(test_xenosrecomp.cpp test_shader_binding.cpp PROPERTIES COMPILE_OPTIONS
  "-Wall;-Wextra;-Wpedantic;-Werror;$<$<CONFIG:Debug>:-fsanitize=address,undefined>;$<$<CONFIG:Debug>:-fno-omit-frame-pointer>")
set_source_files_properties(${LR_SHADER_CACHE} PROPERTIES GENERATED TRUE)
target_link_options(lr_shader_tests PRIVATE $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
catch_discover_tests(lr_shader_tests ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 120)

# Broken containers: the tool must exit non-zero, report that nothing was translated and name the failure. The
# fixture generator writes them; the command needs the tool and the generated inputs, which the harness above builds.
set(_lr_broken_bounds_text "outside the container")
set(_lr_broken_dxc_text "DXC failed")
foreach(_case bounds dxc)
  add_test(NAME "broken container makes the tool fail (${_case})"
    COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:XenosRecomp> -DINCLUDE=${LR_SHADER_COMMON}
            -DIN=${LR_SHADER_GEN}/broken-${_case} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/broken-${_case}.cpp
            -DEXPECT=failure "-DEXPECT_TEXT=translated 0 of 1|${_lr_broken_${_case}_text}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_xenosrecomp.cmake)
  set_tests_properties("broken container makes the tool fail (${_case})" PROPERTIES LABELS "unit;shaders")
endforeach()
