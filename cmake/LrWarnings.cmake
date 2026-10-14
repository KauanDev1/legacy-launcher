# SPDX-License-Identifier: MIT
# Strict flags for LegacyRecomp's own targets. Never call this on submodule targets.
function(lr_configure_target target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(${target} PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>)
  target_link_options(${target} PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
endfunction()
