# SPDX-License-Identifier: MIT
# Instruction harness: synthetic PPC functions -> XenonRecomp (at build time) -> generated C++ compiled into
# lr_recomp_tests, which runs each function and checks registers/memory. Separate executable so the generated
# ppc_config.h never meets tests/support/fake_generated.
set(LR_RECOMP_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(LR_RECOMP_OUT ${LR_RECOMP_GEN}/out)

add_executable(lr_gen_recomp_fixture gen_recomp_fixture.cpp)
target_include_directories(lr_gen_recomp_fixture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests)
lr_configure_target(lr_gen_recomp_fixture)

set(LR_RECOMP_GENERATED
  ${LR_RECOMP_OUT}/ppc_recomp.0.cpp ${LR_RECOMP_OUT}/ppc_func_mapping.cpp
  ${LR_RECOMP_OUT}/ppc_config.h ${LR_RECOMP_OUT}/ppc_context.h ${LR_RECOMP_OUT}/ppc_recomp_shared.h)
add_custom_command(OUTPUT ${LR_RECOMP_GENERATED}
  COMMAND lr_gen_recomp_fixture ${LR_RECOMP_GEN}
  COMMAND ${CMAKE_COMMAND} -DXENON=$<TARGET_FILE:XenonRecomp>
          -DCONTEXT_H=${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/XenonUtils/ppc_context.h -DDIR=${LR_RECOMP_GEN}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/run_xenonrecomp.cmake
  DEPENDS lr_gen_recomp_fixture XenonRecomp ${CMAKE_CURRENT_SOURCE_DIR}/run_xenonrecomp.cmake
          ${CMAKE_CURRENT_SOURCE_DIR}/cases.h
  COMMENT "Recompiling synthetic PPC test functions" VERBATIM)

add_executable(lr_recomp_tests test_recomp_instructions.cpp ${LR_RECOMP_GENERATED})
target_include_directories(lr_recomp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${LR_RECOMP_OUT}
  ${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/thirdparty/simde)
target_link_libraries(lr_recomp_tests PRIVATE lr_runtime_core lr_common Catch2::Catch2WithMain)
# Generated code gets the game template's flags (src/runtime/template/CMakeLists.txt), never -Werror/sanitizers.
set_source_files_properties(${LR_RECOMP_OUT}/ppc_recomp.0.cpp ${LR_RECOMP_OUT}/ppc_func_mapping.cpp PROPERTIES
  GENERATED TRUE COMPILE_OPTIONS
  "-fno-strict-aliasing;-ffp-model=strict;-fexceptions;$<$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x86_64>:-march=sandybridge>;-Wno-switch;-Wno-unused-function;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-unused-label;-Wno-null-arithmetic;-Wno-null-conversion;-Wno-tautological-undefined-compare;-Wno-void-pointer-to-int-cast;-Wno-int-to-void-pointer-cast;-Wno-invalid-offsetof;-Wno-c++23-extensions")
# The test source is ours: strict warnings and sanitizers. The generated sources stay uninstrumented, but the
# link needs the sanitizer runtime for the (sanitized) lr_runtime_core.
set_source_files_properties(test_recomp_instructions.cpp PROPERTIES COMPILE_OPTIONS
  "-Wall;-Wextra;-Wpedantic;-Werror;$<$<CONFIG:Debug>:-fsanitize=address,undefined>;$<$<CONFIG:Debug>:-fno-omit-frame-pointer>")
target_link_options(lr_recomp_tests PRIVATE $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
catch_discover_tests(lr_recomp_tests ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 600)
