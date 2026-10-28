// SPDX-License-Identifier: MIT
// Test-only CPU reference of the Xenos 2D tiled surface layout (resolve destinations, Task 18; tiled textures,
// Task 19). It is deliberately written differently from the GPU code it checks (the resolve compute shader,
// src/runtime/gpu/vk/shaders/resolve_common.glsl, computes the same address as a sum of shifted terms), so a bug
// in either one shows up as a mismatch.
//
// Layout (semantics from Xenia's texture_address.h description and AMD's r800 AddrLib macro-tiled addressing; no
// code taken from either): for a surface of `pitch` blocks (rounded up to 32) and 2^bpb_log2 bytes per block, the
// byte offset of block (x, y) from the surface base is built bit by bit:
//
//   linear  = ((x/32 + (pitch/32) * (y/32)) * 64  |  ((y >> 1) & 7) * 8  |  (x & 7)) << bpb_log2
//             (32x32 macro tiles laid out row-major, 64 "slots" each; inside one, an 8x16 micro tile addressed
//             by x[2:0] and y[3:1]; y[0] and the micro tile's position are placed by the bits below)
//   bank    = y[4]
//   pipe    = { x[4] ^ y[3], x[3] }
//   offset  = linear[:8] << 12 | bank << 11 | linear[7:5] << 8 | pipe << 6 | linear[4] << 5 | y[0] << 4
//             | linear[3:0]
//
// For 4 or more bytes per block every 32x32 macro tile is a contiguous 2^(10 + bpb_log2)-byte range, tiles in
// row-major order (tile_origin()).
#pragma once

#include <cstdint>

namespace lr::testing {

inline uint32_t xenos_bits(uint32_t v, uint32_t lo, uint32_t count) { return (v >> lo) & ((1u << count) - 1u); }

// Byte offset of block (x, y) in a 2D tiled surface. pitch: in blocks (any value; rounded up to 32 here).
inline uint32_t xenos_tiled_offset_2d(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpb_log2) {
  const uint32_t macro_pitch = (pitch + 31u) / 32u;
  const uint32_t macro_index = (x / 32u) + macro_pitch * (y / 32u);
  const uint32_t micro = xenos_bits(y, 1, 3) * 8u + xenos_bits(x, 0, 3);
  const uint32_t linear = (macro_index * 64u + micro) << bpb_log2;
  const uint32_t bank = xenos_bits(y, 4, 1);
  const uint32_t pipe = ((xenos_bits(x, 4, 1) ^ xenos_bits(y, 3, 1)) << 1) | xenos_bits(x, 3, 1);
  uint32_t offset = 0;
  offset |= (linear >> 8) << 12;
  offset |= bank << 11;
  offset |= xenos_bits(linear, 5, 3) << 8;
  offset |= pipe << 6;
  offset |= xenos_bits(linear, 4, 1) << 5;
  offset |= xenos_bits(y, 0, 1) << 4;
  offset |= xenos_bits(linear, 0, 4);
  return offset;
}

// Byte offset of the 32x32 macro tile holding block (x, y), for 4 or more bytes per block.
inline uint32_t xenos_tile_origin_2d(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpb_log2) {
  const uint32_t macro_pitch = (pitch + 31u) / 32u;
  return ((x / 32u) + macro_pitch * (y / 32u)) << (10u + bpb_log2);
}

}  // namespace lr::testing
