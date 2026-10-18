# SPDX-License-Identifier: MIT
set(LR_RUNTIME_CORE_SOURCES guest_memory.cpp fault.cpp func_table.cpp loader.cpp xex_imports.cpp kernel/registry.cpp kernel/kernel.cpp kernel/glue.cpp guest_thread.cpp memory/page_heap.cpp memory/physical_heap.cpp kernel/exports/memory.cpp kernel/objects.cpp kernel/exports/objects.cpp kernel/variables.cpp kernel/dispatcher.cpp kernel/exports/sync.cpp kernel/threads.cpp kernel/exports/threads.cpp kernel/timers.cpp kernel/exports/time.cpp kernel/exports/critsec.cpp kernel/trace.cpp kernel/deadlock.cpp kernel/exports/debug.cpp kernel/vfs.cpp kernel/exports/vfs.cpp kernel/exports/files.cpp kernel/exports/lzx.cpp)
add_library(lr_runtime_core STATIC ${LR_RUNTIME_CORE_SOURCES})
target_include_directories(lr_runtime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_runtime_core PUBLIC lr_common lr_formats PRIVATE XenonUtils)
lr_configure_target(lr_runtime_core)

# --- Staging variant linked by the game template (share/legacyrecomp/runtime/lib/liblr_runtime_core.a) ---
# Same sources as lr_runtime_core plus lr_common's, but never sanitized and always -O2, whatever the launcher's
# build type: the template builds the game in Release without sanitizer runtimes (ruling R18). lr_common is folded
# into the same archive so the template links one --whole-archive library for everything we own (with lr_formats'
# xex2.cpp, the only formats code the runtime uses).
get_target_property(_lr_common_sources lr_common SOURCES)
get_target_property(_lr_common_dir lr_common SOURCE_DIR)
list(TRANSFORM _lr_common_sources PREPEND "${_lr_common_dir}/")
# formats/xex2.cpp too: the runtime reads the XEX header (loader, variable imports, main.cpp).
get_target_property(_lr_formats_dir lr_formats SOURCE_DIR)
add_library(lr_runtime_core_staged STATIC ${LR_RUNTIME_CORE_SOURCES} ${_lr_common_sources} "${_lr_formats_dir}/xex2.cpp")
target_include_directories(lr_runtime_core_staged PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_runtime_core_staged PRIVATE XenonUtils)
target_compile_options(lr_runtime_core_staged PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)
set_target_properties(lr_runtime_core_staged PROPERTIES
  OUTPUT_NAME lr_runtime_core
  ARCHIVE_OUTPUT_DIRECTORY "${LR_RESOURCES_DIR}/runtime/lib")

# lzx.cpp includes libmspack headers (lzxd.c itself is already inside libXenonUtils.a).
set(LR_MSPACK_INCLUDE "${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp/thirdparty/libmspack/libmspack/mspack")
target_include_directories(lr_runtime_core PRIVATE ${LR_MSPACK_INCLUDE})
target_include_directories(lr_runtime_core_staged PRIVATE ${LR_MSPACK_INCLUDE})
