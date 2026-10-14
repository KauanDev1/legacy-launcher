# SPDX-License-Identifier: GPL-3.0-or-later
add_library(lr_formats STATIC reader.cpp xdvdfs.cpp stfs.cpp)
target_include_directories(lr_formats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(lr_formats PUBLIC lr_common)
lr_configure_target(lr_formats)
