// SPDX-License-Identifier: MIT
// Xenos texture formats and the CPU texture loader (Task 19, M3 spec 5.4): the texture fetch constant, the format
// table, the 2D/3D tiled address math, the guest endian swaps, the guest memory layout of a texture (base level,
// mip levels, packed mip tails) and the decode of guest bytes into host texel data. Pure GPU core: no Vulkan types
// (the Vulkan texture and sampler caches are runtime/gpu/vk/textures.h and samplers.h).
//
// Untiling is done here, on the CPU, not in a compute shader: the cache has to read (and hash) the guest bytes on the
// CPU anyway to notice changes, every guest read is bounds-checked in one place, and the result is testable without a
// device. docs/architecture.md "Texturas (M3, Tarefa 19)" describes the layout rules in prose.
//
// Semantics (field layout, format numbers, block sizes, tiling, mip layout, packed mips, CTX1) were checked against
// Xenia's xenos.h, texture_util.{h,cc}, texture_address.h, the texture_load_ctx1 shader and the Vulkan texture cache;
// reference only, no code taken from them.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"

namespace lr::gpu {

// --- the texture fetch constant (6 dwords at SHADER_CONSTANT_FETCH_00_0 + 6 * slot) -------------------------------

enum class XenosFormat : uint32_t {
  k_1_REVERSE = 0, k_1 = 1, k_8 = 2, k_1_5_5_5 = 3, k_5_6_5 = 4, k_6_5_5 = 5, k_8_8_8_8 = 6, k_2_10_10_10 = 7,
  k_8_A = 8, k_8_B = 9, k_8_8 = 10, k_Cr_Y1_Cb_Y0_REP = 11, k_Y1_Cr_Y0_Cb_REP = 12, k_16_16_EDRAM = 13,
  k_8_8_8_8_A = 14, k_4_4_4_4 = 15, k_10_11_11 = 16, k_11_11_10 = 17, k_DXT1 = 18, k_DXT2_3 = 19, k_DXT4_5 = 20,
  k_16_16_16_16_EDRAM = 21, k_24_8 = 22, k_24_8_FLOAT = 23, k_16 = 24, k_16_16 = 25, k_16_16_16_16 = 26,
  k_16_EXPAND = 27, k_16_16_EXPAND = 28, k_16_16_16_16_EXPAND = 29, k_16_FLOAT = 30, k_16_16_FLOAT = 31,
  k_16_16_16_16_FLOAT = 32, k_32 = 33, k_32_32 = 34, k_32_32_32_32 = 35, k_32_FLOAT = 36, k_32_32_FLOAT = 37,
  k_32_32_32_32_FLOAT = 38, k_32_AS_8 = 39, k_32_AS_8_8 = 40, k_16_MPEG = 41, k_16_16_MPEG = 42,
  k_8_INTERLACED = 43, k_32_AS_8_INTERLACED = 44, k_32_AS_8_8_INTERLACED = 45, k_16_INTERLACED = 46,
  k_16_MPEG_INTERLACED = 47, k_16_16_MPEG_INTERLACED = 48, k_DXN = 49, k_8_8_8_8_AS_16_16_16_16 = 50,
  k_DXT1_AS_16_16_16_16 = 51, k_DXT2_3_AS_16_16_16_16 = 52, k_DXT4_5_AS_16_16_16_16 = 53,
  k_2_10_10_10_AS_16_16_16_16 = 54, k_10_11_11_AS_16_16_16_16 = 55, k_11_11_10_AS_16_16_16_16 = 56,
  k_32_32_32_FLOAT = 57, k_DXT3A = 58, k_DXT5A = 59, k_CTX1 = 60, k_DXT3A_AS_1_1_1_1 = 61,
  k_8_8_8_8_GAMMA_EDRAM = 62, k_2_10_10_10_FLOAT_EDRAM = 63,
};

enum class TextureDimension : uint32_t { k1D = 0, k2DOrStacked = 1, k3D = 2, kCube = 3 };
enum class TextureSign : uint32_t { kUnsigned = 0, kSigned = 1, kUnsignedBiased = 2, kGamma = 3 };
// dword 0 clamp_x/y/z
enum class ClampMode : uint32_t {
  kRepeat = 0, kMirroredRepeat = 1, kClampToEdge = 2, kMirrorClampToEdge = 3, kClampToHalfway = 4,
  kMirrorClampToHalfway = 5, kClampToBorder = 6, kMirrorClampToBorder = 7,
};
// dword 3 mag/min/mip_filter. kUseFetchConst means something only in a fetch instruction; in a fetch constant it is
// treated as kPoint.
enum class TextureFilter : uint32_t { kPoint = 0, kLinear = 1, kBaseMap = 2, kUseFetchConst = 3 };
// dword 3 aniso_filter: kMax_1_1 + n is 2^n:1; 6 and 7 (kUseFetchConst) mean disabled in a fetch constant.
enum class AnisoFilter : uint32_t { kDisabled = 0, kMax_1_1 = 1, kMax_2_1 = 2, kMax_4_1 = 3, kMax_8_1 = 4, kMax_16_1 = 5 };
// dword 5 border_color
enum class BorderColor : uint32_t { k_ABGR_Black = 0, k_ABGR_White = 1, k_ACBYCR_Black = 2, k_ACBCRY_Black = 3 };

inline constexpr uint32_t kFetchConstantTypeTexture = 2;  // dword 0 bits 0-1
inline constexpr uint32_t kMaxTextureLevels = 14;         // 8192 = 2^13: levels 0..13
inline constexpr uint32_t kMaxTexture2DSide = 8192;
// Host bytes one texture may decode to (all levels and layers). 8192x8192 RGBA8 with mips is 341 MiB.
inline constexpr uint64_t kMaxTextureHostBytes = 512ull << 20;

// The 6 dwords as the GPU reads them (host order). Field layout per Xenia xenos.h xe_gpu_texture_fetch_t (checked).
struct TextureFetch {
  std::array<uint32_t, 6> dwords{};

  static TextureFetch from_registers(const RegisterFile& regs, uint32_t slot);  // slot 0..31

  uint32_t field(uint32_t dword, uint32_t lo, uint32_t count) const {
    return (dwords[dword] >> lo) & ((count >= 32 ? 0u : (1u << count)) - 1u);
  }
  int32_t signed_field(uint32_t dword, uint32_t lo, uint32_t count) const {
    const uint32_t v = field(dword, lo, count);
    return (v & (1u << (count - 1))) ? int32_t(v) - int32_t(1u << count) : int32_t(v);
  }

  uint32_t type() const { return field(0, 0, 2); }
  TextureSign sign(uint32_t component) const { return TextureSign(field(0, 2 + 2 * component, 2)); }  // x..w
  ClampMode clamp(uint32_t axis) const { return ClampMode(field(0, 10 + 3 * axis, 3)); }             // x, y, z
  uint32_t pitch_texels() const { return field(0, 22, 9) << 5; }  // base row pitch in texels (field is >> 5)
  bool tiled() const { return field(0, 31, 1) != 0; }
  XenosFormat format() const { return XenosFormat(field(1, 0, 6)); }
  uint32_t endian() const { return field(1, 6, 2); }
  bool stacked() const { return field(1, 10, 1) != 0; }
  uint32_t base_address() const { return dwords[1] & 0xFFFFF000u; }  // physical, page aligned
  uint32_t num_format() const { return field(3, 0, 1); }
  uint32_t swizzle() const { return field(3, 1, 12); }  // 3 bits per output x, y, z, w: 0-3 = X-W, 4 = 0, 5 = 1
  int32_t exp_adjust() const { return signed_field(3, 13, 6); }
  TextureFilter mag_filter() const { return TextureFilter(field(3, 19, 2)); }
  TextureFilter min_filter() const { return TextureFilter(field(3, 21, 2)); }
  TextureFilter mip_filter() const { return TextureFilter(field(3, 23, 2)); }
  uint32_t aniso_filter() const { return field(3, 25, 3); }
  uint32_t mip_min_level() const { return field(4, 2, 4); }
  uint32_t mip_max_level() const { return field(4, 6, 4); }
  int32_t lod_bias() const { return signed_field(4, 12, 10); }  // 1/32 LOD units
  BorderColor border_color() const { return BorderColor(field(5, 0, 2)); }
  TextureDimension dimension() const { return TextureDimension(field(5, 9, 2)); }
  bool packed_mips() const { return field(5, 11, 1) != 0; }
  uint32_t mip_address() const { return dwords[5] & 0xFFFFF000u; }

  // Sizes (the fields hold size - 1). 1D: dword 2 bits 0-23; 2D/stacked/cube: 13:13 plus the stack depth (6 bits);
  // 3D: 11:11:10.
  uint32_t width() const;
  uint32_t height() const;
  uint32_t depth() const;  // 3D depth, stacked layer count, 6 for a cube, else 1

  bool operator==(const TextureFetch&) const = default;
};

// The mip levels a fetch constant makes available: Xenia's GetSubresourcesFromFetchConstant semantics. No mip
// address: level 0 only. With mips: min/max clamped to the size's level count; no base address raises min to 1;
// min > 0 drops the base.
struct MipRange {
  uint32_t min_level = 0;
  uint32_t max_level = 0;
  bool has_base = true;
  bool has_mips = false;
  bool operator==(const MipRange&) const = default;
};
MipRange mip_range(const TextureFetch& fetch);

// --- formats ------------------------------------------------------------------------------------------------------

// How the loader turns one guest block (after the endian swap) into host data.
enum class TextureConversion : uint8_t {
  kNone,      // unsupported: the loader refuses the format, naming it
  kCopy,      // host block == guest block
  k565,       // X 0-4, Y 5-10, Z 11-15 -> RGBA8 (A = 255)
  k1555,      // X 0-4, Y 5-9, Z 10-14, W 15 -> RGBA8
  k4444,      // X 0-3, Y 4-7, Z 8-11, W 12-15 -> RGBA8
  k10_11_11,  // X 0-10, Y 11-21, Z 22-31 -> RGBA16 unorm (A = 65535)
  k11_11_10,  // X 0-9, Y 10-20, Z 21-31 -> RGBA16 unorm
  kCtx1,      // 4x4 block, 8 bytes -> R8G8 texels
};

// The host texel layout a format is uploaded as (the Vulkan side maps it to a VkFormat).
enum class HostLayout : uint8_t {
  kNone, kR8, kR8G8, kR8G8B8A8, kA2B10G10R10, kR16, kR16G16, kR16G16B16A16, kR16F, kR16G16F, kR16G16B16A16F,
  kR32F, kR32G32F, kR32G32B32A32F, kBC1, kBC2, kBC3, kBC4, kBC5,
};

struct FormatInfo {
  const char* name;            // "k_8_8_8_8"
  uint8_t block_width;         // guest block in texels (4 for the compressed formats)
  uint8_t block_height;
  uint8_t bytes_per_block;     // guest bytes per block
  uint8_t components;          // 1..4: X only, XY, XYZ, XYZW; missing components read as the last present one
  TextureConversion conversion;
  HostLayout host;
  uint8_t host_block_width;    // host block in texels (4 for BC formats, 1 otherwise)
  uint8_t host_block_height;
  uint8_t host_bytes_per_block;
};

const FormatInfo& format_info(XenosFormat format);  // any value 0..63
inline bool format_supported(XenosFormat format) { return format_info(format).host != HostLayout::kNone; }

// --- tiling and endian --------------------------------------------------------------------------------------------

// Byte offset of block (x, y) in a 2D tiled surface: 32x32-block macro tiles, row-major; inside one, 8x16 micro
// tiles with the bank (y[4]) and pipe (x[4:3] ^ y[3] << 1) bits interleaved (Xenia texture_address.h semantics).
// pitch_blocks: row pitch in blocks, rounded up to 32 here. bytes_per_block: 1, 2, 4, 8 or 16.
uint32_t tiled_offset_2d(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t bytes_per_block);
// Byte offset of block (x, y, z) in a 3D tiled surface: 32x16x4 macro tiles, 8x8x4 micro tiles. pitch_blocks and
// height_blocks are rounded up to 32 here.
uint64_t tiled_offset_3d(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch_blocks, uint32_t height_blocks,
                         uint32_t bytes_per_block);
// One past the largest byte a block in [0, width) x [0, height) (blocks) can touch, for a 2D tiled surface. Exact up
// to the 32x32 tile holding the last block; never smaller than the true extent (the unit tests check it against
// every block).
uint64_t tiled_extent_2d(uint32_t width, uint32_t height, uint32_t pitch_blocks, uint32_t bytes_per_block);
uint64_t tiled_extent_3d(uint32_t width, uint32_t height, uint32_t depth, uint32_t pitch_blocks, uint32_t height_blocks,
                         uint32_t bytes_per_block);

// The Xenos endian swap of one 32-bit word as loaded little-endian from guest memory (fetch constant dword 1 bits
// 6-7): 0 none, 1 8in16 (bytes swapped in each half), 2 8in32 (byte-reversed), 3 16in32 (halves swapped).
uint32_t endian_swap32(uint32_t word, uint32_t mode);

// One CTX1 block (8 bytes, after the endian swap: word0 = the bytes 0-3 as a little-endian word, word1 = bytes 4-7)
// as 16 R8G8 texels, row-major. word0 holds the two endpoints, R in the high byte of each 16-bit half (endpoint 0 in
// bits 0-15: R bits 8-15, G bits 0-7; endpoint 1 in bits 16-31); word1 holds 2-bit codes, texel (x, y) at bit
// 2 * (4y + x): 0 = endpoint 0, 1 = endpoint 1, 2 = (2 e0 + e1) / 3, 3 = (e0 + 2 e1) / 3 (truncated).
std::array<std::array<uint8_t, 2>, 16> decode_ctx1_block(uint32_t word0, uint32_t word1);

// Offset in blocks of mip `level` inside its packed mip tail (Xenia GetPackedMipOffset semantics): false (and zero
// offsets) when the level is not in the tail. width/height/depth: of level 0, in texels; depth 1 for non-3D.
bool packed_mip_offset(uint32_t width, uint32_t height, uint32_t depth, uint32_t block_width, uint32_t block_height,
                       uint32_t level, uint32_t& x_blocks, uint32_t& y_blocks, uint32_t& z);
// The level holding the packed mip tail of a width x height texture: the first one whose shorter side is <= 16.
uint32_t packed_mip_level(uint32_t width, uint32_t height);

// --- the texture a fetch constant describes -----------------------------------------------------------------------

struct TextureDesc {
  bool null = false;  // empty or unbound fetch constant (not a texture, or no address at all): reads (0, 0, 0, 0)
  XenosFormat format = XenosFormat::k_8_8_8_8;
  TextureDimension dimension = TextureDimension::k2DOrStacked;
  bool stacked = false, tiled = false, packed_mips = false;
  uint32_t endian = 0;
  uint32_t width = 1, height = 1, depth = 1;  // texels; depth: 3D depth, stacked layers, 6 for a cube, else 1
  uint32_t pitch_texels = 0;                  // base level row pitch
  uint32_t base_phys = 0, mip_phys = 0;       // physical addresses (0: absent)
  MipRange mips;                              // host levels 0..mips.max_level
  bool gamma = false;   // some colour component has TextureSign::kGamma (an sRGB view)
  bool biased = false;  // some component is "unsigned biased" (2v - 1): not applied in M3 (logged by the cache)
  uint32_t swizzle = 0;
  bool operator==(const TextureDesc&) const = default;
};

// Decodes and validates a fetch constant. Errors (ErrorCode::InvalidFormat, message naming the fetch constant's
// problem): an unsupported format ("texture format k_1_REVERSE (0) is not supported"); a signed component ("signed
// texture formats are not supported in M3 (k_8_8_8_8, sign 1101)"); a 1D texture that is tiled or wider than 8192.
Result<TextureDesc> describe_texture(const TextureFetch& fetch);

// One host level: `layers` slices (cube faces in Xenos/Vulkan order +X -X +Y -Y +Z -Z, stacked layers or 3D depth
// slices), each `rows` rows of `row_blocks` host blocks, tightly packed.
struct HostLevel {
  uint32_t width = 0, height = 0, layers = 0;
  uint32_t row_blocks = 0, rows = 0;
  size_t offset = 0, size = 0;  // in HostTexture::bytes
};
struct HostTexture {
  std::vector<uint8_t> bytes;
  std::vector<HostLevel> levels;  // 0..max_level; levels below mips.min_level stay zero when the base is absent
};

// The guest ranges a texture reads: the base level's storage and the mip storage, each clamped to guest memory and
// to its physical allocation (MemoryAccess::allocation_end). Bytes past the clamp read as zero.
struct GuestSpan {
  uint32_t phys = 0;
  uint32_t guest = 0;
  uint32_t size = 0;  // readable bytes
};
struct TextureFootprint {
  std::optional<GuestSpan> base, mips;
  uint64_t host_bytes = 0;
};

// The guest bytes `desc` covers. Errors (InvalidFormat, naming the texture): storage whose first byte is not in guest
// memory or not in a physical allocation, a range that does not map linearly, or more than kMaxTextureHostBytes of
// host data.
Result<TextureFootprint> texture_footprint(const TextureDesc& desc, const MemoryAccess& mem);
// XXH3-64 of the footprint's guest bytes (what the cache compares to notice a change).
uint64_t hash_footprint(const TextureFootprint& footprint, const MemoryAccess& mem);
// Untiles, endian-swaps and converts the texture into host data (every read bounds-checked against the footprint).
Result<HostTexture> load_texture(const TextureDesc& desc, const MemoryAccess& mem, const TextureFootprint& footprint);

// The front buffer VdSwap presents: fetch constant 0 (its base address replaced by the physical front buffer)
// decoded as a width x height texture into RGBA8 with the fetch constant's swizzle applied and alpha 255. Formats
// whose host data is R8G8B8A8, A2B10G10R10 or R16G16B16A16F only (InvalidFormat otherwise, naming it).
Result<std::vector<uint8_t>> front_buffer_rgba8(const TextureFetch& fetch, uint32_t phys, uint32_t width,
                                                uint32_t height, const MemoryAccess& mem);

// "k_8_8_8_8 1280x720 tiled at 0x1F000000" for messages.
std::string describe(const TextureDesc& desc);

}  // namespace lr::gpu
