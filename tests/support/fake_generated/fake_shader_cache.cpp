// SPDX-License-Identifier: MIT
// The fake game's stand-in for the shaders stage output (work/shaders/shader_cache.cpp): the same symbols, no
// shaders. Every array has one zero row because C++ has no empty arrays; the counts say 0.
#include <cstddef>
#include <cstdint>

#include "runtime/gpu/shader_cache.h"

struct lr_shader_vertex_input {
  uint64_t microcode_hash;
  uint32_t location, fetch_slot, offset_dwords, stride_dwords, format, flags;
  int32_t exp_adjust;
};

extern "C" const lr_shader_cache_entry lr_shader_cache[] = {{0, 0, 0, 0, 0}};
extern "C" const size_t lr_shader_cache_count = 0;
extern "C" const lr_shader_vertex_input lr_shader_vertex_inputs[] = {{0, 0, 0, 0, 0, 0, 0, 0}};
extern "C" const size_t lr_shader_vertex_input_count = 0;
extern "C" const uint8_t lr_shader_blob[] = {0};
extern "C" const size_t lr_shader_blob_size = 0;
extern "C" const size_t lr_shader_blob_uncompressed_size = 0;
