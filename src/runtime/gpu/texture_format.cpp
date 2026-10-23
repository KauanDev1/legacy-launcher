// SPDX-License-Identifier: MIT
#include "runtime/gpu/texture_format.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace lr::gpu {
namespace {

using C = TextureConversion;
using H = HostLayout;

constexpr uint64_t kPhysicalSpace = uint64_t{pm4::kPhysicalMask} + 1;
constexpr uint32_t kTileSide = 32;                 // 32x32(x4) blocks per macro tile, also the stride alignment
constexpr uint32_t kLinearRowAlignment = 256;      // linear mip rows are 256-byte aligned
constexpr uint32_t kSubresourceAlignment = 4096;   // array slice / level strides

// name, block w, h, bytes, components, conversion, host layout, host block w, h, bytes
#define LR_FMT(name, bw, bh, bpb, comp, conv, host, hbw, hbh, hbpb) \
  FormatInfo{name, bw, bh, bpb, comp, conv, host, hbw, hbh, hbpb}
#define LR_NONE(name, bw, bh, bpb) LR_FMT(name, bw, bh, bpb, 1, C::kNone, H::kNone, 1, 1, 0)
const std::array<FormatInfo, 64> kFormats = {
    LR_NONE("k_1_REVERSE", 1, 1, 1),
    LR_NONE("k_1", 1, 1, 1),
    LR_FMT("k_8", 1, 1, 1, 1, C::kCopy, H::kR8, 1, 1, 1),
    LR_FMT("k_1_5_5_5", 1, 1, 2, 4, C::k1555, H::kR8G8B8A8, 1, 1, 4),
    LR_FMT("k_5_6_5", 1, 1, 2, 3, C::k565, H::kR8G8B8A8, 1, 1, 4),
    LR_NONE("k_6_5_5", 1, 1, 2),
    LR_FMT("k_8_8_8_8", 1, 1, 4, 4, C::kCopy, H::kR8G8B8A8, 1, 1, 4),
    LR_FMT("k_2_10_10_10", 1, 1, 4, 4, C::kCopy, H::kA2B10G10R10, 1, 1, 4),
    LR_FMT("k_8_A", 1, 1, 1, 1, C::kCopy, H::kR8, 1, 1, 1),
    LR_NONE("k_8_B", 1, 1, 1),
    LR_FMT("k_8_8", 1, 1, 2, 2, C::kCopy, H::kR8G8, 1, 1, 2),
    LR_NONE("k_Cr_Y1_Cb_Y0_REP", 2, 1, 4),
    LR_NONE("k_Y1_Cr_Y0_Cb_REP", 2, 1, 4),
    LR_NONE("k_16_16_EDRAM", 1, 1, 4),
    LR_FMT("k_8_8_8_8_A", 1, 1, 4, 4, C::kCopy, H::kR8G8B8A8, 1, 1, 4),
    LR_FMT("k_4_4_4_4", 1, 1, 2, 4, C::k4444, H::kR8G8B8A8, 1, 1, 4),
    LR_FMT("k_10_11_11", 1, 1, 4, 3, C::k10_11_11, H::kR16G16B16A16, 1, 1, 8),
    LR_FMT("k_11_11_10", 1, 1, 4, 3, C::k11_11_10, H::kR16G16B16A16, 1, 1, 8),
    LR_FMT("k_DXT1", 4, 4, 8, 4, C::kCopy, H::kBC1, 4, 4, 8),
    LR_FMT("k_DXT2_3", 4, 4, 16, 4, C::kCopy, H::kBC2, 4, 4, 16),
    LR_FMT("k_DXT4_5", 4, 4, 16, 4, C::kCopy, H::kBC3, 4, 4, 16),
    LR_NONE("k_16_16_16_16_EDRAM", 1, 1, 8),
    LR_NONE("k_24_8", 1, 1, 4),
    LR_NONE("k_24_8_FLOAT", 1, 1, 4),
    LR_FMT("k_16", 1, 1, 2, 1, C::kCopy, H::kR16, 1, 1, 2),
    LR_FMT("k_16_16", 1, 1, 4, 2, C::kCopy, H::kR16G16, 1, 1, 4),
    LR_FMT("k_16_16_16_16", 1, 1, 8, 4, C::kCopy, H::kR16G16B16A16, 1, 1, 8),
    LR_FMT("k_16_EXPAND", 1, 1, 2, 1, C::kCopy, H::kR16F, 1, 1, 2),
    LR_FMT("k_16_16_EXPAND", 1, 1, 4, 2, C::kCopy, H::kR16G16F, 1, 1, 4),
    LR_FMT("k_16_16_16_16_EXPAND", 1, 1, 8, 4, C::kCopy, H::kR16G16B16A16F, 1, 1, 8),
    LR_FMT("k_16_FLOAT", 1, 1, 2, 1, C::kCopy, H::kR16F, 1, 1, 2),
    LR_FMT("k_16_16_FLOAT", 1, 1, 4, 2, C::kCopy, H::kR16G16F, 1, 1, 4),
    LR_FMT("k_16_16_16_16_FLOAT", 1, 1, 8, 4, C::kCopy, H::kR16G16B16A16F, 1, 1, 8),
    LR_NONE("k_32", 1, 1, 4),
    LR_NONE("k_32_32", 1, 1, 8),
    LR_NONE("k_32_32_32_32", 1, 1, 16),
    LR_FMT("k_32_FLOAT", 1, 1, 4, 1, C::kCopy, H::kR32F, 1, 1, 4),
    LR_FMT("k_32_32_FLOAT", 1, 1, 8, 2, C::kCopy, H::kR32G32F, 1, 1, 8),
    LR_FMT("k_32_32_32_32_FLOAT", 1, 1, 16, 4, C::kCopy, H::kR32G32B32A32F, 1, 1, 16),
    LR_NONE("k_32_AS_8", 1, 1, 4),
    LR_NONE("k_32_AS_8_8", 1, 1, 4),
    LR_NONE("k_16_MPEG", 1, 1, 2),
    LR_NONE("k_16_16_MPEG", 1, 1, 4),
    LR_NONE("k_8_INTERLACED", 1, 1, 1),
    LR_NONE("k_32_AS_8_INTERLACED", 1, 1, 4),
    LR_NONE("k_32_AS_8_8_INTERLACED", 1, 1, 4),
    LR_NONE("k_16_INTERLACED", 1, 1, 2),
    LR_NONE("k_16_MPEG_INTERLACED", 1, 1, 2),
    LR_NONE("k_16_16_MPEG_INTERLACED", 1, 1, 4),
    LR_FMT("k_DXN", 4, 4, 16, 2, C::kCopy, H::kBC5, 4, 4, 16),
    LR_FMT("k_8_8_8_8_AS_16_16_16_16", 1, 1, 4, 4, C::kCopy, H::kR8G8B8A8, 1, 1, 4),
    LR_FMT("k_DXT1_AS_16_16_16_16", 4, 4, 8, 4, C::kCopy, H::kBC1, 4, 4, 8),
    LR_FMT("k_DXT2_3_AS_16_16_16_16", 4, 4, 16, 4, C::kCopy, H::kBC2, 4, 4, 16),
    LR_FMT("k_DXT4_5_AS_16_16_16_16", 4, 4, 16, 4, C::kCopy, H::kBC3, 4, 4, 16),
    LR_FMT("k_2_10_10_10_AS_16_16_16_16", 1, 1, 4, 4, C::kCopy, H::kA2B10G10R10, 1, 1, 4),
    LR_FMT("k_10_11_11_AS_16_16_16_16", 1, 1, 4, 3, C::k10_11_11, H::kR16G16B16A16, 1, 1, 8),
    LR_FMT("k_11_11_10_AS_16_16_16_16", 1, 1, 4, 3, C::k11_11_10, H::kR16G16B16A16, 1, 1, 8),
    LR_NONE("k_32_32_32_FLOAT", 1, 1, 12),
    LR_NONE("k_DXT3A", 4, 4, 8),
    LR_FMT("k_DXT5A", 4, 4, 8, 1, C::kCopy, H::kBC4, 4, 4, 8),
    LR_FMT("k_CTX1", 4, 4, 8, 2, C::kCtx1, H::kR8G8, 1, 1, 2),
    LR_NONE("k_DXT3A_AS_1_1_1_1", 4, 4, 8),
    LR_NONE("k_8_8_8_8_GAMMA_EDRAM", 1, 1, 4),
    LR_NONE("k_2_10_10_10_FLOAT_EDRAM", 1, 1, 4),
};
#undef LR_NONE
#undef LR_FMT

uint32_t bits(uint32_t v, uint32_t lo, uint32_t count) { return (v >> lo) & ((1u << count) - 1u); }
uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint64_t align_up64(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
uint32_t div_up(uint32_t v, uint32_t d) { return (v + d - 1) / d; }
uint32_t log2_ceil(uint32_t v) { return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1)); }
uint32_t log2_floor(uint32_t v) { return v == 0 ? 0 : 31 - uint32_t(std::countl_zero(v)); }
uint32_t next_pow2(uint32_t v) { return 1u << log2_ceil(v); }
uint32_t bpb_log2(uint32_t bytes_per_block) { return log2_floor(bytes_per_block); }

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

// Places the "linear" tile-relative offset `o` (macro tile index and micro position, already scaled to bytes) and
// the bank / pipe / y[0] bits at their address bits: o[3:0] -> 3:0, y[0] -> 4, o[4] -> 5, pipe -> 7:6, o[7:5] ->
// 10:8, bank -> 11, o[31:8] -> 31:12.
uint64_t spread(uint64_t o, uint32_t bank, uint32_t pipe, uint32_t y_lsb) {
  return (o & 0xFu) | (uint64_t{y_lsb} << 4) | (((o >> 4) & 1u) << 5) | (uint64_t{pipe} << 6) |
         (((o >> 5) & 7u) << 8) | (uint64_t{bank} << 11) | ((o >> 8) << 12);
}

uint8_t expand(uint32_t v, uint32_t width) {  // unorm n-bit -> 8-bit, rounded
  const uint32_t max = (1u << width) - 1u;
  return uint8_t((v * 255u + max / 2u) / max);
}
uint16_t expand16(uint32_t v, uint32_t width) {  // unorm n-bit -> 16-bit, rounded
  const uint32_t max = (1u << width) - 1u;
  return uint16_t((uint64_t{v} * 65535u + max / 2u) / max);
}

// The texel layout a texture occupies in guest memory, per storage (the base level, or one mip storage level).
struct Storage {
  uint32_t phys = 0;
  uint32_t pitch_blocks = 0;      // row pitch, tile aligned
  uint32_t row_pitch_bytes = 0;   // linear row pitch
  uint32_t rows_blocks = 1;       // block rows per depth slice, tile aligned (1 for 1D)
  uint64_t z_stride = 0;          // bytes per 3D depth slice (linear)
  uint64_t slice_stride = 0;      // bytes per array slice / cube face (a whole volume for 3D)
  uint32_t x_ext = 0, y_ext = 0, z_ext = 0;  // blocks touched, over every level stored here
  uint64_t offset = 0;            // from the storage base address (mips: from mip_phys)
  uint64_t extent = 0;            // bytes touched from `offset`
};

struct LevelSource {
  int storage = -1;  // index into Layout::storages; -1: nothing to load (absent base)
  uint32_t ox = 0, oy = 0, oz = 0;
  uint32_t width = 1, height = 1, depth = 1;  // texels (depth: 3D)
  uint32_t layers = 1;                        // host slices
};

struct Layout {
  std::vector<Storage> storages;  // [0] base when present, then mip storages
  int base_storage = -1;
  std::vector<LevelSource> levels;
  uint32_t array_size = 1;
  bool is_3d = false;
  uint64_t mips_extent = 0;
};

bool is_3d_storage(const TextureDesc& d) { return d.dimension == TextureDimension::k3D; }

Layout make_layout(const TextureDesc& d) {
  const FormatInfo& f = format_info(d.format);
  const bool one_d = d.dimension == TextureDimension::k1D;
  Layout L;
  L.is_3d = is_3d_storage(d);
  const uint32_t depth3 = L.is_3d ? d.depth : 1;
  L.array_size = L.is_3d ? 1 : d.depth;  // stacked layers / cube faces / 1
  const uint32_t packed = d.packed_mips ? packed_mip_level(d.width, d.height) : UINT32_MAX;

  auto make_storage = [&](bool base, uint32_t level) {
    Storage s;
    uint32_t pitch_texels, rows_texels;
    if (base) {
      pitch_texels = d.pitch_texels != 0 ? d.pitch_texels : align_up(d.width, kTileSide);
      rows_texels = d.height;
    } else {
      pitch_texels = std::max(next_pow2(d.width) >> level, 1u);
      rows_texels = std::max(next_pow2(d.height) >> level, 1u);
    }
    s.pitch_blocks = align_up(div_up(pitch_texels, f.block_width), kTileSide);
    s.row_pitch_bytes = s.pitch_blocks * f.bytes_per_block;
    if (!d.tiled && !base) s.row_pitch_bytes = align_up(s.row_pitch_bytes, kLinearRowAlignment);
    s.rows_blocks = one_d ? 1 : align_up(div_up(rows_texels, f.block_height), kTileSide);
    s.z_stride = uint64_t{s.row_pitch_bytes} * s.rows_blocks;
    s.slice_stride = s.z_stride;
    if (L.is_3d) s.slice_stride *= align_up(depth3, 4);
    s.slice_stride = align_up64(s.slice_stride, kSubresourceAlignment);
    return s;
  };

  // Storages: the base (when present); the mips either one storage per level 1..min(max, packed), or, when the
  // whole chain is packed (packed level 0), one storage laid out like level 0 of a power-of-two texture.
  if (d.mips.has_base) {
    L.base_storage = 0;
    Storage b = make_storage(true, 0);
    b.phys = d.base_phys;
    L.storages.push_back(b);
  }
  std::vector<int> mip_storage(kMaxTextureLevels, -1);  // by storage level
  if (d.mips.has_mips) {
    uint64_t offset = 0;
    const uint32_t last = std::min(d.mips.max_level, packed);
    const uint32_t first = packed == 0 ? 0 : 1;
    for (uint32_t s_level = first; s_level <= std::max(last, first); ++s_level) {
      Storage s = make_storage(false, s_level);
      s.phys = d.mip_phys;
      s.offset = offset;
      offset += s.slice_stride * L.array_size;
      mip_storage[s_level] = int(L.storages.size());
      L.storages.push_back(s);
      if (packed == 0) break;
    }
  }

  for (uint32_t level = 0; level <= d.mips.max_level; ++level) {
    LevelSource src;
    src.width = std::max(d.width >> level, 1u);
    src.height = one_d ? 1 : std::max(d.height >> level, 1u);
    src.depth = std::max(depth3 >> level, 1u);
    src.layers = L.is_3d ? src.depth
                 : (d.dimension == TextureDimension::k2DOrStacked && d.stacked) ? std::max(d.depth >> level, 1u)
                                                                                 : d.depth;
    if (level == 0) {
      src.storage = d.mips.has_base ? L.base_storage : -1;
    } else {
      const uint32_t s_level = std::min(level, packed == 0 ? 0u : packed);
      src.storage = mip_storage[std::min(s_level, kMaxTextureLevels - 1)];
    }
    if (src.storage >= 0 && level >= packed)
      packed_mip_offset(d.width, d.height, depth3, f.block_width, f.block_height, level, src.ox, src.oy, src.oz);
    if (src.storage >= 0) {
      Storage& s = L.storages[size_t(src.storage)];
      s.x_ext = std::max(s.x_ext, src.ox + div_up(src.width, f.block_width));
      s.y_ext = std::max(s.y_ext, src.oy + div_up(src.height, f.block_height));
      s.z_ext = std::max(s.z_ext, src.oz + src.depth);
    }
    L.levels.push_back(src);
  }

  for (Storage& s : L.storages) {
    if (s.x_ext == 0) continue;
    uint64_t data;
    if (d.tiled) {
      data = L.is_3d ? tiled_extent_3d(s.x_ext, s.y_ext, s.z_ext, s.pitch_blocks, s.rows_blocks, f.bytes_per_block)
                     : tiled_extent_2d(s.x_ext, s.y_ext, s.pitch_blocks, f.bytes_per_block);
    } else {
      data = s.z_stride * (s.z_ext - 1) + uint64_t{s.row_pitch_bytes} * (s.y_ext - 1) +
             uint64_t{f.bytes_per_block} * s.x_ext;
    }
    s.extent = s.slice_stride * (L.array_size - 1) + data;
  }
  for (size_t i = 0; i < L.storages.size(); ++i)
    if (int(i) != L.base_storage) L.mips_extent = std::max(L.mips_extent, L.storages[i].offset + L.storages[i].extent);
  return L;
}

uint64_t host_size(const TextureDesc& d, const Layout& L) {
  const FormatInfo& f = format_info(d.format);
  uint64_t total = 0;
  for (const LevelSource& l : L.levels)
    total += uint64_t{div_up(l.width, f.host_block_width)} * div_up(l.height, f.host_block_height) * l.layers *
             f.host_bytes_per_block;
  return total;
}

Error texture_error(const TextureDesc& d, const std::string& what) {
  return Error{ErrorCode::InvalidFormat, "texture " + describe(d) + ": " + what, ""};
}

// Maps [phys, phys + wanted) to guest memory: the first byte must be in guest memory and in a physical allocation;
// the readable size is clamped to guest memory, the 512 MiB physical space and the allocation's end.
Result<GuestSpan> map_span(const TextureDesc& d, const MemoryAccess& mem, uint32_t phys, uint64_t wanted,
                           const char* what) {
  GuestSpan s;
  s.phys = phys;
  const std::optional<uint32_t> guest = mem.to_guest ? mem.to_guest(phys) : std::nullopt;
  if (!guest || *guest >= mem.size || !mem.base)
    return texture_error(d, std::string(what) + " at " + hex(phys) + " is not in guest memory");
  uint64_t room = std::min(mem.size - *guest, kPhysicalSpace - phys);
  if (mem.allocation_end) {
    const std::optional<uint32_t> end = mem.allocation_end(phys);
    if (!end || *end <= phys)
      return texture_error(d, std::string(what) + " at " + hex(phys) + " is not in a physical allocation");
    room = std::min(room, uint64_t{*end} - phys);
  }
  const uint64_t readable = std::min(wanted, room);
  if (readable > 0) {
    const std::optional<uint32_t> last = mem.to_guest(uint32_t(phys + readable - 1));
    if (!last || uint64_t{*last} != *guest + readable - 1)
      return texture_error(d, std::string(what) + " at " + hex(phys) + " does not map to one guest range");
  }
  s.guest = *guest;
  s.size = uint32_t(readable);
  return s;
}

// Reads one guest block (bytes_per_block bytes) at `rel` from the span start, endian-swapped per 32-bit word; bytes
// past the span read as zero. Blocks of 1 or 2 bytes take their bytes from the swapped word holding them.
void read_block(const uint8_t* span, uint32_t span_size, uint64_t rel, uint32_t bpb, uint32_t endian, uint8_t* out) {
  if (bpb >= 4) {
    for (uint32_t i = 0; i < bpb; i += 4) {
      uint32_t w = 0;
      if (rel + i + 4 <= span_size) std::memcpy(&w, span + rel + i, 4);
      w = endian_swap32(w, endian);
      std::memcpy(out + i, &w, 4);
    }
    return;
  }
  const uint64_t word_at = rel & ~uint64_t{3};
  uint32_t w = 0;  // the span may end inside the word (a 1 or 2-byte texel at its end): the rest reads zero
  if (word_at < span_size) std::memcpy(&w, span + word_at, size_t(std::min<uint64_t>(4, span_size - word_at)));
  w = endian_swap32(w, endian);
  uint8_t b[4];
  std::memcpy(b, &w, 4);
  std::memcpy(out, b + (rel & 3), bpb);
}

uint16_t load16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}
uint32_t load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// One guest block -> host bytes at `dst` (pixel formats: one texel).
void convert_texel(TextureConversion c, const uint8_t* in, uint8_t* dst) {
  switch (c) {
    case C::k565: {
      const uint32_t v = load16(in);
      const uint8_t px[4] = {expand(bits(v, 0, 5), 5), expand(bits(v, 5, 6), 6), expand(bits(v, 11, 5), 5), 255};
      std::memcpy(dst, px, 4);
      break;
    }
    case C::k1555: {
      const uint32_t v = load16(in);
      const uint8_t px[4] = {expand(bits(v, 0, 5), 5), expand(bits(v, 5, 5), 5), expand(bits(v, 10, 5), 5),
                             uint8_t(bits(v, 15, 1) ? 255 : 0)};
      std::memcpy(dst, px, 4);
      break;
    }
    case C::k4444: {
      const uint32_t v = load16(in);
      const uint8_t px[4] = {expand(bits(v, 0, 4), 4), expand(bits(v, 4, 4), 4), expand(bits(v, 8, 4), 4),
                             expand(bits(v, 12, 4), 4)};
      std::memcpy(dst, px, 4);
      break;
    }
    case C::k10_11_11:
    case C::k11_11_10: {
      const uint32_t v = load32(in);
      const bool x11 = c == C::k10_11_11;
      const uint32_t wx = x11 ? 11 : 10, wy = 11, wz = x11 ? 10 : 11;
      const uint16_t px[4] = {expand16(bits(v, 0, wx), wx), expand16(bits(v, wx, wy), wy),
                              expand16(bits(v, wx + wy, wz), wz), 0xFFFF};
      std::memcpy(dst, px, 8);
      break;
    }
    default:
      break;
  }
}

}  // namespace

// --- TextureFetch ---------------------------------------------------------------------------------------------------

TextureFetch TextureFetch::from_registers(const RegisterFile& regs, uint32_t slot) {
  TextureFetch f;
  const uint32_t base = kShaderConstantFetchBase + kShaderConstantFetchRegsPerConstant * (slot % kShaderConstantFetchCount);
  for (uint32_t i = 0; i < 6; ++i) f.dwords[i] = regs.read(base + i);
  return f;
}

uint32_t TextureFetch::width() const {
  switch (dimension()) {
    case TextureDimension::k1D: return field(2, 0, 24) + 1;
    case TextureDimension::k3D: return field(2, 0, 11) + 1;
    default: return field(2, 0, 13) + 1;
  }
}

uint32_t TextureFetch::height() const {
  switch (dimension()) {
    case TextureDimension::k1D: return 1;
    case TextureDimension::k3D: return field(2, 11, 11) + 1;
    default: return field(2, 13, 13) + 1;
  }
}

uint32_t TextureFetch::depth() const {
  switch (dimension()) {
    case TextureDimension::k3D: return field(2, 22, 10) + 1;
    case TextureDimension::kCube: return 6;
    case TextureDimension::k2DOrStacked: return stacked() ? field(2, 26, 6) + 1 : 1;
    default: return 1;
  }
}

MipRange mip_range(const TextureFetch& f) {
  MipRange r;
  uint32_t longest = std::max(f.width(), f.height());
  if (f.dimension() == TextureDimension::k3D) longest = std::max(longest, f.depth());
  const uint32_t size_levels = log2_floor(longest);
  bool has_base = (f.base_address() & pm4::kPhysicalMask) != 0;
  bool has_mips = (f.mip_address() & pm4::kPhysicalMask) != 0;
  if (has_mips) {
    r.min_level = std::min(f.mip_min_level(), size_levels);
    r.max_level = std::max(std::min(f.mip_max_level(), size_levels), r.min_level);
  }
  if (r.max_level != 0) {
    if (!has_base) r.min_level = std::max(r.min_level, 1u);
    if (r.min_level != 0) has_base = false;
  } else {
    has_mips = false;
  }
  r.has_base = has_base;
  r.has_mips = has_mips;
  return r;
}

const FormatInfo& format_info(XenosFormat format) { return kFormats[uint32_t(format) & 63u]; }

// --- tiling ---------------------------------------------------------------------------------------------------------

uint32_t tiled_offset_2d(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t bytes_per_block) {
  const uint32_t macro_pitch = (pitch_blocks + 31) >> 5;
  const uint32_t macro = (y >> 5) * macro_pitch + (x >> 5);   // 32x32 macro tile, row-major
  const uint32_t micro = (bits(y, 1, 3) << 3) | bits(x, 0, 3);  // 8 columns x 8 row pairs
  const uint32_t o = ((macro << 6) | micro) << bpb_log2(bytes_per_block);
  const uint32_t bank = bits(y, 4, 1);
  const uint32_t pipe = bits(x, 3, 2) ^ (bits(y, 3, 1) << 1);
  return uint32_t(spread(o, bank, pipe, y & 1u));
}

uint64_t tiled_offset_3d(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch_blocks, uint32_t height_blocks,
                         uint32_t bytes_per_block) {
  // 64-bit: a 2048x2048x1024 volume of 16-byte blocks addresses past 4 GiB (the loader's host-size limit refuses it
  // later, but the address math itself must not wrap).
  const uint64_t macro_pitch = (uint64_t{pitch_blocks} + 31) >> 5;
  const uint64_t macro_rows = ((uint64_t{height_blocks} + 31) >> 5) * 2;  // 16-row macro tiles
  const uint64_t macro = ((uint64_t{z} >> 2) * macro_rows + (y >> 4)) * macro_pitch + (x >> 5);
  const uint64_t micro = (bits(z, 0, 2) << 5) | (bits(y, 1, 2) << 3) | bits(x, 0, 3);
  const uint64_t o = ((macro << 7) | micro) << bpb_log2(bytes_per_block);
  const uint32_t bank = (bits(y, 3, 1) ^ bits(z, 2, 1));
  const uint32_t pipe = bits(x, 3, 2) ^ (bank << 1);
  return spread(o, bank, pipe, y & 1u);
}

uint64_t tiled_extent_2d(uint32_t width, uint32_t height, uint32_t pitch_blocks, uint32_t bytes_per_block) {
  if (width == 0 || height == 0) return 0;
  // The last 32x32 tile has the largest macro index; within it, every offset lies below its origin plus: the whole
  // tile (1024 blocks) for 4+ bytes per block, where a tile is contiguous; for 1 and 2 bytes, the highest bits the
  // spread can set inside one tile (bank 0x800, o[7:5] up to 0x100 or 0x300, pipe 0xC0, 0x3F below).
  const uint64_t origin = tiled_offset_2d((width - 1) & ~31u, (height - 1) & ~31u, pitch_blocks, bytes_per_block);
  const uint64_t tile = bytes_per_block == 1 ? 0xA00 : bytes_per_block == 2 ? 0xC00 : 1024ull * bytes_per_block;
  return origin + tile;
}

uint64_t tiled_extent_3d(uint32_t width, uint32_t height, uint32_t depth, uint32_t pitch_blocks, uint32_t height_blocks,
                         uint32_t bytes_per_block) {
  if (width == 0 || height == 0 || depth == 0) return 0;
  // Origin of the 32x32x4 region holding the last block, plus what that region spans: with 16-row macro tiles two
  // of them stack per 32 rows, and the bank (y[3] ^ z[2]) can reach into the next row of macro tiles. The unit tests
  // compare this bound with every block.
  const uint32_t pitch = align_up(pitch_blocks, 32);
  const uint64_t origin = tiled_offset_3d((width - 1) & ~31u, (height - 1) & ~31u, (depth - 1) & ~3u, pitch,
                                          height_blocks, bytes_per_block);
  uint64_t span;
  if (bytes_per_block == 1)
    span = (uint64_t{pitch >> 6} << 12) + 0xC00 + (uint64_t{pitch & 32} << 5);
  else
    span = ((uint64_t{pitch} << 6) + 0x800) << bpb_log2(bytes_per_block);
  return origin + span;
}

uint32_t endian_swap32(uint32_t v, uint32_t mode) {
  switch (mode & 3u) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);  // 8in16
    case 2: return __builtin_bswap32(v);                                 // 8in32
    case 3: return (v << 16) | (v >> 16);                                 // 16in32
    default: return v;
  }
}

std::array<std::array<uint8_t, 2>, 16> decode_ctx1_block(uint32_t word0, uint32_t word1) {
  const uint32_t r0 = bits(word0, 8, 8), g0 = bits(word0, 0, 8);
  const uint32_t r1 = bits(word0, 24, 8), g1 = bits(word0, 16, 8);
  std::array<std::array<uint8_t, 2>, 16> out{};
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t code = bits(word1, 2 * i, 2);
    // Weights of (endpoint 0, endpoint 1) in thirds.
    static constexpr uint32_t kW0[4] = {3, 0, 2, 1};
    const uint32_t w0 = kW0[code], w1 = 3 - w0;
    out[i] = {uint8_t((r0 * w0 + r1 * w1) / 3), uint8_t((g0 * w0 + g1 * w1) / 3)};
  }
  return out;
}

uint32_t packed_mip_level(uint32_t width, uint32_t height) {
  const uint32_t l = log2_ceil(std::min(width, height));
  return l > 4 ? l - 4 : 0;
}

bool packed_mip_offset(uint32_t width, uint32_t height, uint32_t depth, uint32_t block_width, uint32_t block_height,
                       uint32_t level, uint32_t& x_blocks, uint32_t& y_blocks, uint32_t& z) {
  x_blocks = y_blocks = z = 0;
  const uint32_t lw = log2_ceil(width), lh = log2_ceil(height);
  const uint32_t lsize = std::min(lw, lh);
  if (lsize > 4 + level) return false;  // the shorter side is still above 16 texels
  const uint32_t base = lsize > 4 ? lsize - 4 : 0;
  const uint32_t p = level - base;  // 0: the 16-texel slot, 1: 8, 2: 4, 3: 2, 4: 1
  uint32_t x = 0, y = 0;
  const bool wide = lw > lh;
  if (p < 3) {
    // 16, 8 and 4-texel levels side by side along the long axis, at 16 >> p.
    (wide ? y : x) = 16u >> p;
  } else {
    // 2 and 1-texel levels along the other axis, past the 4-texel one.
    const uint32_t offset = (1u << ((wide ? lw : lh) - base)) >> (p - 2);
    (wide ? x : y) = offset;
    if (offset < 4) {
      const uint32_t ld = log2_ceil(depth);
      z = ld > 1 + p ? (ld - p) * 4 : 4;
    }
  }
  x_blocks = x / block_width;
  y_blocks = y / block_height;
  return true;
}

// --- describing and loading -----------------------------------------------------------------------------------------

std::string describe(const TextureDesc& d) {
  if (d.null) return "null";
  static constexpr const char* kDims[4] = {"1D", "2D", "3D", "cube"};
  std::string s = std::string(format_info(d.format).name) + " " + std::to_string(d.width);
  if (d.dimension != TextureDimension::k1D) s += "x" + std::to_string(d.height);
  if (d.depth > 1) s += "x" + std::to_string(d.depth);
  s += std::string(" ") + kDims[uint32_t(d.dimension) & 3] + (d.stacked ? " stacked" : "") +
       (d.tiled ? " tiled" : " linear");
  s += " at " + hex(d.mips.has_base ? d.base_phys : d.mip_phys);
  return s;
}

Result<TextureDesc> describe_texture(const TextureFetch& fetch) {
  TextureDesc d;
  if (fetch.type() != kFetchConstantTypeTexture) {
    d.null = true;
    return d;
  }
  d.format = fetch.format();
  d.dimension = fetch.dimension();
  d.stacked = d.dimension == TextureDimension::k2DOrStacked && fetch.stacked();
  d.tiled = fetch.tiled();
  d.packed_mips = fetch.packed_mips();
  d.endian = fetch.endian();
  d.width = fetch.width();
  d.height = fetch.height();
  d.depth = fetch.depth();
  d.pitch_texels = fetch.pitch_texels();
  d.swizzle = fetch.swizzle();
  d.mips = mip_range(fetch);
  d.base_phys = d.mips.has_base ? fetch.base_address() & pm4::kPhysicalMask : 0;
  d.mip_phys = d.mips.has_mips ? fetch.mip_address() & pm4::kPhysicalMask : 0;
  if (!d.mips.has_base && !d.mips.has_mips) {
    TextureDesc n;
    n.null = true;
    return n;
  }
  const FormatInfo& f = format_info(d.format);
  if (f.host == HostLayout::kNone)
    return Error{ErrorCode::InvalidFormat,
                 std::string("texture format ") + f.name + " (" + std::to_string(uint32_t(d.format)) +
                     ") is not supported",
                 ""};
  std::string signs;
  for (uint32_t c = 0; c < 4; ++c) signs += char('0' + uint32_t(fetch.sign(c)));
  for (uint32_t c = 0; c < f.components; ++c) {
    const TextureSign s = fetch.sign(c);
    if (s == TextureSign::kSigned)
      return Error{ErrorCode::InvalidFormat,
                   std::string("signed texture formats are not supported in M3 (") + f.name + ", sign xyzw " + signs +
                       ")",
                   ""};
    if (s == TextureSign::kUnsignedBiased) d.biased = true;
    if (s == TextureSign::kGamma && c < 3) d.gamma = true;
  }
  if (d.dimension == TextureDimension::k1D) {
    if (d.tiled) return texture_error(d, "a tiled 1D texture is not supported");
    if (d.width > kMaxTexture2DSide) return texture_error(d, "1D textures wider than 8192 are not supported");
    d.packed_mips = false;
  }
  return d;
}

Result<TextureFootprint> texture_footprint(const TextureDesc& d, const MemoryAccess& mem) {
  TextureFootprint fp;
  if (d.null) return fp;
  const Layout L = make_layout(d);
  fp.host_bytes = host_size(d, L);
  if (fp.host_bytes > kMaxTextureHostBytes)
    return texture_error(d, "needs " + std::to_string(fp.host_bytes >> 20) + " MiB of host memory (limit " +
                                std::to_string(kMaxTextureHostBytes >> 20) + " MiB)");
  if (L.base_storage >= 0) {
    LR_TRY_ASSIGN(span, map_span(d, mem, d.base_phys, L.storages[size_t(L.base_storage)].extent, "base level"));
    fp.base = span;
  }
  if (d.mips.has_mips) {
    LR_TRY_ASSIGN(span, map_span(d, mem, d.mip_phys, L.mips_extent, "mip levels"));
    fp.mips = span;
  }
  return fp;
}

uint64_t hash_footprint(const TextureFootprint& fp, const MemoryAccess& mem) {
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  for (const std::optional<GuestSpan>& s : {fp.base, fp.mips}) {
    if (!s) continue;
    XXH3_64bits_update(&state, &s->phys, sizeof s->phys);
    XXH3_64bits_update(&state, &s->size, sizeof s->size);
    if (s->size) XXH3_64bits_update(&state, mem.base + s->guest, s->size);
  }
  return XXH3_64bits_digest(&state);
}

Result<HostTexture> load_texture(const TextureDesc& d, const MemoryAccess& mem, const TextureFootprint& fp) {
  HostTexture out;
  if (d.null) return out;
  const FormatInfo& f = format_info(d.format);
  if (f.host == HostLayout::kNone) return texture_error(d, "format is not supported");
  const Layout L = make_layout(d);
  const uint64_t total = host_size(d, L);
  if (total > kMaxTextureHostBytes) return texture_error(d, "too large for the host");
  out.bytes.assign(size_t(total), 0);

  const uint32_t bpb = f.bytes_per_block;
  size_t offset = 0;
  for (const LevelSource& src : L.levels) {
    HostLevel hl;
    hl.width = src.width;
    hl.height = src.height;
    hl.layers = src.layers;
    hl.row_blocks = div_up(src.width, f.host_block_width);
    hl.rows = div_up(src.height, f.host_block_height);
    hl.offset = offset;
    hl.size = size_t{hl.row_blocks} * hl.rows * hl.layers * f.host_bytes_per_block;
    offset += hl.size;
    out.levels.push_back(hl);
    if (src.storage < 0) continue;

    const Storage& s = L.storages[size_t(src.storage)];
    const std::optional<GuestSpan>& span = src.storage == L.base_storage ? fp.base : fp.mips;
    if (!span) continue;
    const uint8_t* g = mem.base + span->guest;
    const uint64_t storage_rel = s.offset;  // mips: from mip_phys; base: 0
    const uint32_t bx_count = div_up(src.width, f.block_width), by_count = div_up(src.height, f.block_height);
    const size_t host_row_bytes = size_t{hl.row_blocks} * f.host_bytes_per_block;
    const size_t host_slice_bytes = host_row_bytes * hl.rows;
    uint8_t block[16];
    for (uint32_t layer = 0; layer < src.layers; ++layer) {
      uint8_t* host_slice = out.bytes.data() + hl.offset + host_slice_bytes * layer;
      for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
          const uint32_t x = bx + src.ox, y = by + src.oy;
          uint64_t rel;
          if (L.is_3d) {
            const uint32_t z = layer + src.oz;
            rel = d.tiled ? tiled_offset_3d(x, y, z, s.pitch_blocks, s.rows_blocks, bpb)
                          : s.z_stride * z + uint64_t{s.row_pitch_bytes} * y + uint64_t{bpb} * x;
          } else {
            rel = s.slice_stride * layer + (d.tiled ? uint64_t{tiled_offset_2d(x, y, s.pitch_blocks, bpb)}
                                                    : uint64_t{s.row_pitch_bytes} * y + uint64_t{bpb} * x);
          }
          rel += storage_rel;
          if (rel >= span->size) {
            std::memset(block, 0, bpb);
          } else {
            read_block(g, span->size, rel, bpb, d.endian, block);
          }
          if (f.conversion == C::kCopy) {
            std::memcpy(host_slice + host_row_bytes * by + size_t{bx} * bpb, block, bpb);
          } else if (f.conversion == C::kCtx1) {
            const auto texels = decode_ctx1_block(load32(block), load32(block + 4));
            for (uint32_t ty = 0; ty < 4; ++ty) {
              const uint32_t py = by * 4 + ty;
              if (py >= src.height) break;
              for (uint32_t tx = 0; tx < 4; ++tx) {
                const uint32_t px = bx * 4 + tx;
                if (px >= src.width) break;
                std::memcpy(host_slice + host_row_bytes * py + size_t{px} * 2, texels[ty * 4 + tx].data(), 2);
              }
            }
          } else {
            convert_texel(f.conversion, block, host_slice + host_row_bytes * by + size_t{bx} * f.host_bytes_per_block);
          }
        }
      }
    }
  }
  return out;
}

namespace {

float half_to_float(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 31u, man = h & 1023u;
  float v;
  if (exp == 0)
    v = std::ldexp(float(man), -24);
  else if (exp == 31)
    v = man ? 0.0f : 65504.0f;
  else
    v = std::ldexp(float(man | 1024u), int(exp) - 25);
  return sign ? -v : v;
}

uint8_t unorm8(float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

}  // namespace

Result<std::vector<uint8_t>> front_buffer_rgba8(const TextureFetch& fetch, uint32_t phys, uint32_t width,
                                                uint32_t height, const MemoryAccess& mem) {
  TextureFetch f = fetch;
  f.dwords[1] = (f.dwords[1] & 0xFFFu) | (phys & 0xFFFFF000u);
  // A 2D texture of the swap's size, base level only (the fetch constant's own size and mips are ignored).
  f.dwords[5] = (f.dwords[5] & ~((3u << 9) | (1u << 11) | 0xFFFFF000u)) | (uint32_t(TextureDimension::k2DOrStacked) << 9);
  f.dwords[1] &= ~(1u << 10);  // not stacked
  if (width == 0 || height == 0 || width > kMaxTexture2DSide || height > kMaxTexture2DSide)
    return Error{ErrorCode::InvalidFormat,
                 "front buffer size " + std::to_string(width) + "x" + std::to_string(height) + " is not supported", ""};
  f.dwords[2] = (width - 1) | ((height - 1) << 13);
  if (f.type() != kFetchConstantTypeTexture)
    return Error{ErrorCode::InvalidFormat, "front buffer fetch constant is not a texture (type " +
                                               std::to_string(f.type()) + ")", ""};
  LR_TRY_ASSIGN(desc, describe_texture(f));
  if (desc.null) return Error{ErrorCode::InvalidFormat, "front buffer has no address", ""};
  const FormatInfo& info = format_info(desc.format);
  if (info.host != HostLayout::kR8G8B8A8 && info.host != HostLayout::kA2B10G10R10 &&
      info.host != HostLayout::kR16G16B16A16F)
    return Error{ErrorCode::InvalidFormat, std::string("front buffer format ") + info.name + " is not supported", ""};
  LR_TRY_ASSIGN(fp, texture_footprint(desc, mem));
  LR_TRY_ASSIGN(host, load_texture(desc, mem, fp));
  std::vector<uint8_t> rgba(size_t{width} * height * 4);
  const uint8_t* src = host.bytes.data() + host.levels.at(0).offset;
  for (size_t i = 0; i < size_t{width} * height; ++i) {
    float c[4];
    if (info.host == HostLayout::kR8G8B8A8) {
      for (int k = 0; k < 4; ++k) c[k] = float(src[i * 4 + size_t(k)]) / 255.0f;
    } else if (info.host == HostLayout::kA2B10G10R10) {
      const uint32_t v = load32(src + i * 4);
      c[0] = float(bits(v, 0, 10)) / 1023.0f;
      c[1] = float(bits(v, 10, 10)) / 1023.0f;
      c[2] = float(bits(v, 20, 10)) / 1023.0f;
      c[3] = float(bits(v, 30, 2)) / 3.0f;
    } else {
      for (int k = 0; k < 4; ++k) c[k] = half_to_float(load16(src + i * 8 + size_t(k) * 2));
    }
    for (uint32_t k = 0; k < 3; ++k) {
      const uint32_t sel = bits(desc.swizzle, 3 * k, 3);
      const float v = sel < 4 ? c[sel] : sel == 5 ? 1.0f : 0.0f;
      rgba[i * 4 + k] = unorm8(v);
    }
    rgba[i * 4 + 3] = 255;
  }
  return rgba;
}

}  // namespace lr::gpu
