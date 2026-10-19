# SPDX-License-Identifier: MIT
include(Catch)
add_executable(lr_tests test_result.cpp test_sha256.cpp test_paths.cpp test_xdvdfs.cpp test_stfs.cpp test_xex2.cpp test_integration_formats.cpp test_runner.cpp test_game_config.cpp test_process.cpp test_doctor.cpp test_toolchain.cpp test_extract_verify.cpp test_recomp_config.cpp test_integration_pipeline.cpp test_switch_scan.cpp test_function_discovery.cpp test_runtime_core.cpp test_loader.cpp test_kernel_registry.cpp test_kernel_abi.cpp test_kernel_glue.cpp test_kernel_memory.cpp test_kernel_variables.cpp test_kernel_objects.cpp test_kernel_sync.cpp test_kernel_threads.cpp test_kernel_time.cpp test_kernel_critsec.cpp test_kernel_debug.cpp test_kernel_vfs.cpp test_kernel_files.cpp test_kernel_lzx.cpp test_kernel_rtl.cpp test_kernel_xex_config.cpp test_kernel_xam.cpp test_sha1.cpp test_import_stubs.cpp test_guest_thread.cpp test_page_heap.cpp test_physical_heap.cpp test_fake_game.cpp test_build_stage.cpp test_integration_e2e.cpp test_cli_args.cpp test_progress_bar.cpp test_queue_sink.cpp test_gui_model.cpp test_game_exit.cpp test_gpu_vk_device.cpp)
target_include_directories(lr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/support)
# lr_runtime_core is linked --whole-archive, as the game template does (see lr_fake_game below).
target_link_libraries(lr_tests PRIVATE lr_common lr_formats lr_pipeline lr_cli lr_gui_core lr_vulkan "$<LINK_LIBRARY:WHOLE_ARCHIVE,lr_runtime_core>" XenonUtils tomlplusplus::tomlplusplus Catch2::Catch2WithMain)
lr_configure_target(lr_tests)
catch_discover_tests(lr_tests ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 3600)
target_compile_definitions(lr_tests PRIVATE LR_SOURCE_DIR="${CMAKE_SOURCE_DIR}" LR_RESOURCES_DIR="${LR_RESOURCES_DIR}"
  LR_CLI_PATH="$<TARGET_FILE:legacyrecomp>")
add_dependencies(lr_tests legacyrecomp)
add_dependencies(lr_tests lr_resources)

# --- fake game: main.cpp + hand-written "generated" code + stubs rendered by render_import_stubs ---
find_package(SDL2 QUIET)
if(TARGET SDL2::SDL2)
  set(LR_FAKE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/support/fake_generated)
  add_executable(lr_gen_import_stubs support/gen_import_stubs.cpp)
  target_link_libraries(lr_gen_import_stubs PRIVATE lr_pipeline lr_common)
  lr_configure_target(lr_gen_import_stubs)
  set(LR_FAKE_STUBS ${CMAKE_CURRENT_BINARY_DIR}/fake_import_stubs.cpp)
  add_custom_command(OUTPUT ${LR_FAKE_STUBS}
    COMMAND lr_gen_import_stubs ${LR_FAKE_DIR}/ppc_recomp_shared.h ${LR_FAKE_STUBS}
    DEPENDS lr_gen_import_stubs ${LR_FAKE_DIR}/ppc_recomp_shared.h VERBATIM)
  add_executable(lr_fake_game ${CMAKE_SOURCE_DIR}/src/runtime/main.cpp ${LR_FAKE_DIR}/ppc_func_mapping.cpp
    ${LR_FAKE_DIR}/fake_recomp.cpp ${LR_FAKE_DIR}/fake_kernel.cpp ${LR_FAKE_STUBS})
  target_include_directories(lr_fake_game PRIVATE ${LR_FAKE_DIR})
  # --whole-archive like the game template, so LR_KERNEL registrations inside lr_runtime_core are linked exactly as
  # in the real game (an object nothing references is otherwise dropped with its registration).
  target_link_libraries(lr_fake_game PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,lr_runtime_core>" XenonUtils SDL2::SDL2)
  # The same forced include as the game template, so the fake game exercises the real PPC_CALL_INDIRECT_FUNC.
  target_compile_options(lr_fake_game PRIVATE -include runtime/indirect_call.h)
  lr_configure_target(lr_fake_game)
  add_dependencies(lr_tests lr_fake_game)
  target_compile_definitions(lr_tests PRIVATE LR_FAKE_GAME_PATH="$<TARGET_FILE:lr_fake_game>")

  # --- template guard: the STAGED main.cpp and rendered thunks compile with the staged headers only ---
  # lr_fake_game above compiles src/runtime/main.cpp with lr_runtime_core's include dirs (src/), so it cannot see
  # a header resources.cmake forgot to stage; this test uses the game template's include path instead.
  set(_lr_x86 0)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(_lr_x86 1)
  endif()
  add_test(NAME template_compiles_with_staged_headers
    COMMAND ${CMAKE_COMMAND} "-DCXX=${CMAKE_CXX_COMPILER}"
            "-DSOURCES=${LR_RESOURCES_DIR}/runtime/src/main.cpp|${LR_FAKE_STUBS}|${LR_FAKE_DIR}/fake_recomp.cpp"
            "-DGENERATED=${LR_FAKE_DIR}" "-DPPC_CONTEXT_DIR=${LR_RESOURCES_DIR}/tools"
            "-DRUNTIME_INCLUDE=${LR_RESOURCES_DIR}/runtime/include"
            "-DEXTRA_INCLUDES=$<JOIN:$<TARGET_PROPERTY:SDL2::SDL2,INTERFACE_INCLUDE_DIRECTORIES>,|>"
            "-DMARCH=${_lr_x86}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/support/check_template_compiles.cmake)
  set_tests_properties(template_compiles_with_staged_headers PROPERTIES LABELS unit)
endif()

# --- GPL tripwire: the binutils disassembler (GPL-2.0-or-later) must stay out of our binaries (ADR 0002) ---
set(_lr_tripwire_bins "$<TARGET_FILE:legacyrecomp>")
foreach(_t legacyrecomp-gui lr_fake_game)
  if(TARGET ${_t})
    string(APPEND _lr_tripwire_bins "|$<TARGET_FILE:${_t}>")
  endif()
endforeach()
add_test(NAME no_gpl_disassembler_in_binaries
  COMMAND ${CMAKE_COMMAND} "-DNM=${CMAKE_NM}" "-DBINARIES=${_lr_tripwire_bins}"
          "-DRUNTIME_LIB_DIR=${LR_RESOURCES_DIR}/runtime/lib"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/support/check_no_gpl_disassembler.cmake)
set_tests_properties(no_gpl_disassembler_in_binaries PROPERTIES LABELS unit)
add_subdirectory(recomp)
