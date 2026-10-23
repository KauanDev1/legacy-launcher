# SPDX-License-Identifier: MIT
set(LR_RUNTIME_CORE_SOURCES guest_memory.cpp fault.cpp func_table.cpp loader.cpp xex_imports.cpp kernel/registry.cpp kernel/kernel.cpp kernel/glue.cpp guest_thread.cpp memory/page_heap.cpp memory/physical_heap.cpp kernel/exports/memory.cpp kernel/objects.cpp kernel/exports/objects.cpp kernel/variables.cpp kernel/dispatcher.cpp kernel/exports/sync.cpp kernel/threads.cpp kernel/exports/threads.cpp kernel/timers.cpp kernel/exports/time.cpp kernel/exports/critsec.cpp kernel/trace.cpp kernel/deadlock.cpp kernel/exports/debug.cpp kernel/vfs.cpp kernel/exports/vfs.cpp kernel/exports/files.cpp kernel/exports/lzx.cpp kernel/guest_printf.cpp kernel/exports/rtl.cpp kernel/exports/xex.cpp kernel/exports/config.cpp kernel/exports/crypto.cpp kernel/exports/xam.cpp kernel/exports/xam_ui.cpp kernel/exports/audio.cpp kernel/exports/video.cpp kernel/exports/net.cpp kernel/exports/msg.cpp kernel/exports/nui.cpp gpu/registers.cpp gpu/null_backend.cpp gpu/pm4.cpp gpu/mmio.cpp gpu/gpu_system.cpp gpu/shader_cache.cpp gpu/shader/ir.cpp gpu/shader/disasm.cpp gpu/shader/spirv_builder.cpp gpu/shader/translator.cpp gpu/shader/translator_export.cpp gpu/shader/translator_cf.cpp gpu/shader/translator_alu.cpp gpu/shader/translator_fetch.cpp)
add_library(lr_runtime_core STATIC ${LR_RUNTIME_CORE_SOURCES})
target_include_directories(lr_runtime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_runtime_core PUBLIC lr_common lr_formats lr_spirv_headers PRIVATE XenonUtils)
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
target_link_libraries(lr_runtime_core_staged PRIVATE XenonUtils lr_spirv_headers)
target_compile_options(lr_runtime_core_staged PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)
set_target_properties(lr_runtime_core_staged PROPERTIES
  OUTPUT_NAME lr_runtime_core
  ARCHIVE_OUTPUT_DIRECTORY "${LR_RESOURCES_DIR}/runtime/lib")

# lzx.cpp includes libmspack headers (lzxd.c itself is already inside libXenonUtils.a).
set(LR_MSPACK_INCLUDE "${LR_XENONRECOMP_DIR}/thirdparty/libmspack/libmspack/mspack")
target_include_directories(lr_runtime_core PRIVATE ${LR_MSPACK_INCLUDE})
target_include_directories(lr_runtime_core_staged PRIVATE ${LR_MSPACK_INCLUDE})
# gpu/shader_cache.cpp uses xxHash header-only (XXH_INLINE_ALL), XenonRecomp's bundled copy (XXH3-64 of the microcode).
set(LR_XXHASH_INCLUDE "${LR_XENONRECOMP_DIR}/thirdparty/xxHash")
target_include_directories(lr_runtime_core SYSTEM PRIVATE ${LR_XXHASH_INCLUDE})
target_include_directories(lr_runtime_core_staged SYSTEM PRIVATE ${LR_XXHASH_INCLUDE})

# --- Vulkan backend (lr::gpu::vk): our wrapper over volk + VMA (lr_vulkan_thirdparty, root CMakeLists.txt) ---
add_library(lr_vulkan STATIC gpu/vk/device.cpp)
target_include_directories(lr_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_vulkan PUBLIC lr_common lr_vulkan_thirdparty)
if(TARGET SDL2::SDL2)
  target_compile_definitions(lr_vulkan PRIVATE LR_HAVE_SDL2)
  target_link_libraries(lr_vulkan PRIVATE SDL2::SDL2)
endif()
lr_configure_target(lr_vulkan)
