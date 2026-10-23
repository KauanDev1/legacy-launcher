// SPDX-License-Identifier: MIT
// EDRAM resolve (Task 18): copies a rectangle of a host render target into a staging buffer that mirrors a range of
// guest memory, in the Xenos 2D tiled layout with the guest endian swap. Included by resolve_color.comp,
// resolve_color_ms.comp, resolve_depth.comp and resolve_depth_ms.comp, which define LR_RESOLVE_DEPTH (colour or
// depth/stencil source) and LR_RESOLVE_MS (multisampled source) before including it. The host side is
// src/runtime/gpu/vk/edram.cpp; tests/support/xenos_tiling.h is an independent CPU reference of the tiling.
//
// One invocation per destination pixel (x, y) of the rectangle; source pixel = destination pixel (Direct3D 9 gives
// one rectangle for both, Xenia xenos.h notes). Every write is bounds-checked against the staging range, which the
// host clamps to guest memory, so a guest-controlled base, pitch or size never writes outside it.

layout(local_size_x = 8, local_size_y = 8) in;

#if LR_RESOLVE_DEPTH
#if LR_RESOLVE_MS
layout(set = 0, binding = 0) uniform sampler2DMS u_depth;
layout(set = 0, binding = 1) uniform usampler2DMS u_stencil;
#else
layout(set = 0, binding = 0) uniform sampler2D u_depth;
layout(set = 0, binding = 1) uniform usampler2D u_stencil;
#endif
#else
#if LR_RESOLVE_MS
layout(set = 0, binding = 0) uniform sampler2DMS u_color;
#else
layout(set = 0, binding = 0) uniform sampler2D u_color;
#endif
#endif

layout(set = 0, binding = 2, std430) buffer Destination { uint words[]; } u_dst;

// Must match ResolvePush in edram.cpp.
layout(push_constant) uniform Params {
  uvec4 rect;          // x0, y0, width, height of the rectangle, in pixels
  uvec2 src_size;      // host image size; pixels past it are skipped
  uint dest_pitch;     // RB_COPY_DEST_PITCH.copy_dest_pitch rounded up to 32, in pixels
  uint pack;           // kPack* below
  uint endian;         // RB_COPY_DEST_INFO.copy_dest_endian (Endian128 0..4)
  uint swap_rb;        // RB_COPY_DEST_INFO.copy_dest_swap: exchange red and blue before packing
  int exp_bias;        // RB_COPY_DEST_INFO.copy_dest_exp_bias: colour scaled by 2^bias
  uint sample_count;   // host samples averaged (1, 2 or 4); depth always 1
  uint samples;        // host sample indices, 8 bits each, lowest first
  uint dst_lo;         // byte offset, from the destination base, of u_dst.words[0]
  uint dst_bytes;      // bytes of u_dst that mirror guest memory; writes past them are dropped
  uint depth_format;   // RB_DEPTH_INFO.depth_format: 0 D24S8 (unorm), 1 D24FS8 (20e4 float)
} u_p;

// Destination packings (u_p.pack).
const uint kPack8888 = 0u;        // k_8_8_8_8: R bits 0-7 ... A bits 24-31
const uint kPack2101010 = 1u;     // k_2_10_10_10: R 0-9, G 10-19, B 20-29, A 30-31
const uint kPack16x2Float = 2u;   // k_16_16_FLOAT
const uint kPack16x4Float = 3u;   // k_16_16_16_16_FLOAT (64bpp: R, G in the first word, B, A in the second)
const uint kPack32Float = 4u;     // k_32_FLOAT
const uint kPack32x2Float = 5u;   // k_32_32_FLOAT (64bpp)
const uint kPackDepth = 6u;       // the EDRAM depth word: depth << 8 | stencil

uint bytes_per_pixel_log2() { return (u_p.pack == kPack16x4Float || u_p.pack == kPack32x2Float) ? 3u : 2u; }

// Byte offset of pixel (x, y) in a 2D tiled surface whose pitch (in pixels) is a multiple of 32, with 2^b bytes per
// pixel. Computed as a sum of shifted fields: first an intermediate offset where the 32x32 macro tile index is
// scaled by 2^(7+b), the 8x8 micro position by 2^b with room for y[0] at bit 4 and y[3] above it; then that offset's
// bits are spread to their final places and the bank (y[4], bit 11) and pipe (x[4:3] + 2*y[3], bits 6-7) bits are
// added.
uint tiled_offset_2d(uint x, uint y, uint pitch, uint b) {
  uint macro_tile = (x >> 5u) + (y >> 5u) * (pitch >> 5u);
  uint micro = ((x & 7u) + ((y & 6u) << 2u)) << b;
  uint inter = (macro_tile << (b + 7u)) + ((micro & ~15u) << 1u) + (micro & 15u) + ((y & 8u) << (b + 3u)) +
               ((y & 1u) << 4u);
  uint pipe = ((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u;
  uint bank = (y & 16u) << 7u;
  return ((inter & ~511u) << 3u) + ((inter & 448u) << 2u) + (inter & 63u) + bank + pipe;
}

uint swap32(uint v, uint mode) {
  if (mode == 1u) return ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);   // 8in16
  if (mode == 2u || mode == 4u)                                                     // 8in32 (8in64: + word swap)
    return (v << 24u) | ((v & 0xFF00u) << 8u) | ((v >> 8u) & 0xFF00u) | (v >> 24u);
  if (mode == 3u) return (v << 16u) | (v >> 16u);                                   // 16in32
  return v;
}

uint unorm(float v, float scale) { return uint(round(clamp(v, 0.0, 1.0) * scale)); }

#if LR_RESOLVE_DEPTH
// IEEE float -> Xenos 20e4 (4-bit exponent biased by 15, 20-bit mantissa, [0, 2)). Normal values round to nearest
// even; values below the smallest normal (2^-14) are truncated to a denormal; 2 and above saturate.
uint float_to_20e4(float f) {
  if (!(f > 0.0)) return 0u;
  uint u = floatBitsToUint(f);
  if (u >= 0x40000000u) return 0xFFFFFFu;
  uint e = u >> 23u;
  uint m = u & 0x7FFFFFu;
  if (e <= 112u) {
    uint shift = 116u - e;
    return shift >= 32u ? 0u : ((m | 0x800000u) >> shift);
  }
  uint v = ((e - 112u) << 23u) | m;
  v += 3u + ((v >> 3u) & 1u);
  return min(v >> 3u, 0xFFFFFFu);
}

uvec2 source_words(ivec2 p) {
  int s = int(u_p.samples & 0xFFu);
#if LR_RESOLVE_MS
  float d = texelFetch(u_depth, p, s).r;
  uint st = texelFetch(u_stencil, p, s).r;
#else
  float d = texelFetch(u_depth, p, 0).r;
  uint st = texelFetch(u_stencil, p, 0).r;
#endif
  uint d24 = u_p.depth_format == 0u ? unorm(d, 16777215.0) : float_to_20e4(d * 2.0);
  return uvec2((d24 << 8u) | (st & 0xFFu), 0u);
}
#else
vec4 source_color(ivec2 p) {
#if LR_RESOLVE_MS
  vec4 sum = vec4(0.0);
  uint n = clamp(u_p.sample_count, 1u, 4u);
  for (uint i = 0u; i < n; ++i) sum += texelFetch(u_color, p, int((u_p.samples >> (8u * i)) & 0xFFu));
  return sum / float(n);
#else
  return texelFetch(u_color, p, 0);
#endif
}

uvec2 source_words(ivec2 p) {
  vec4 c = source_color(p) * exp2(float(u_p.exp_bias));
  if (u_p.swap_rb != 0u) c = c.bgra;
  if (u_p.pack == kPack8888)
    return uvec2(unorm(c.r, 255.0) | (unorm(c.g, 255.0) << 8u) | (unorm(c.b, 255.0) << 16u) |
                     (unorm(c.a, 255.0) << 24u), 0u);
  if (u_p.pack == kPack2101010)
    return uvec2(unorm(c.r, 1023.0) | (unorm(c.g, 1023.0) << 10u) | (unorm(c.b, 1023.0) << 20u) |
                     (unorm(c.a, 3.0) << 30u), 0u);
  if (u_p.pack == kPack16x2Float) return uvec2(packHalf2x16(c.rg), 0u);
  if (u_p.pack == kPack16x4Float) return uvec2(packHalf2x16(c.rg), packHalf2x16(c.ba));
  if (u_p.pack == kPack32Float) return uvec2(floatBitsToUint(c.r), 0u);
  return uvec2(floatBitsToUint(c.r), floatBitsToUint(c.g));
}
#endif

void main() {
  uvec2 local = gl_GlobalInvocationID.xy;
  if (local.x >= u_p.rect.z || local.y >= u_p.rect.w) return;
  uvec2 p = u_p.rect.xy + local;
  if (p.x >= u_p.src_size.x || p.y >= u_p.src_size.y) return;
  uint b = bytes_per_pixel_log2();
  uint addr = tiled_offset_2d(p.x, p.y, u_p.dest_pitch, b);
  if (addr < u_p.dst_lo) return;
  uint rel = addr - u_p.dst_lo;
  uint bytes = 1u << b;
  if (rel > u_p.dst_bytes || u_p.dst_bytes - rel < bytes) return;
  uint index = rel >> 2u;
  if (index + (bytes >> 2u) > uint(u_dst.words.length())) return;
  uvec2 w = source_words(ivec2(p));
  if (b == 2u) {
    u_dst.words[index] = swap32(w.x, u_p.endian);
  } else if (u_p.endian == 4u) {  // 8in64: the whole 8 bytes reversed
    u_dst.words[index] = swap32(w.y, 4u);
    u_dst.words[index + 1u] = swap32(w.x, 4u);
  } else {
    u_dst.words[index] = swap32(w.x, u_p.endian);
    u_dst.words[index + 1u] = swap32(w.y, u_p.endian);
  }
}
