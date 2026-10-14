# SPDX-License-Identifier: GPL-3.0-or-later
include(Catch)
add_executable(lr_tests test_result.cpp test_sha256.cpp test_paths.cpp test_xdvdfs.cpp test_stfs.cpp)
target_include_directories(lr_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lr_tests PRIVATE lr_common lr_formats Catch2::Catch2WithMain)
lr_configure_target(lr_tests)
catch_discover_tests(lr_tests ADD_TAGS_AS_LABELS)
