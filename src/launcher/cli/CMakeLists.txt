# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(legacyrecomp main.cpp)
target_link_libraries(legacyrecomp PRIVATE lr_common lr_formats)
lr_configure_target(legacyrecomp)
