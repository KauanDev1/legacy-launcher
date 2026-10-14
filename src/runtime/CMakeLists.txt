# SPDX-License-Identifier: GPL-3.0-or-later
set(LR_RUNTIME_CORE_SOURCES guest_memory.cpp fault.cpp func_table.cpp loader.cpp kernel/registry.cpp guest_thread.cpp)
add_library(lr_runtime_core STATIC ${LR_RUNTIME_CORE_SOURCES})
target_include_directories(lr_runtime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_runtime_core PUBLIC lr_common PRIVATE XenonUtils)
lr_configure_target(lr_runtime_core)

# --- Staging variant linked by the game template (share/legacyrecomp/runtime/lib/liblr_runtime_core.a) ---
# Same sources as lr_runtime_core plus lr_common's, but never sanitized and always -O2, whatever the launcher's
# build type: the template builds the game in Release without sanitizer runtimes (ruling R18). lr_common is folded
# into the same archive so the template links one --whole-archive library for everything we own.
get_target_property(_lr_common_sources lr_common SOURCES)
get_target_property(_lr_common_dir lr_common SOURCE_DIR)
list(TRANSFORM _lr_common_sources PREPEND "${_lr_common_dir}/")
add_library(lr_runtime_core_staged STATIC ${LR_RUNTIME_CORE_SOURCES} ${_lr_common_sources})
target_include_directories(lr_runtime_core_staged PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_runtime_core_staged PRIVATE XenonUtils)
target_compile_options(lr_runtime_core_staged PRIVATE -Wall -Wextra -Wpedantic -Werror -O2)
set_target_properties(lr_runtime_core_staged PROPERTIES
  OUTPUT_NAME lr_runtime_core
  ARCHIVE_OUTPUT_DIRECTORY "${LR_RESOURCES_DIR}/runtime/lib")
