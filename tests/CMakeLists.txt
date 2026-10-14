# SPDX-License-Identifier: GPL-3.0-or-later
include(Catch)
add_executable(lr_tests test_result.cpp)
target_link_libraries(lr_tests PRIVATE lr_common Catch2::Catch2WithMain)
lr_configure_target(lr_tests)
catch_discover_tests(lr_tests ADD_TAGS_AS_LABELS)
