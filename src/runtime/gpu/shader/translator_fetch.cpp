// SPDX-License-Identifier: MIT
// Fetch instructions. Vertex fetch (Task 15.8) reads the vertex data inside the shader, so pipelines have no vertex
// input state: the per-draw vertex-fetch table (contract.h, push-constant offset 24) gives each fetch constant's
// buffer device address, size in bytes and endian swap mode, and the instruction gives the format, stride and offset
// (so they are part of the microcode hash). Texture fetch is Task 15.9. Semantics per Xenia's SPIR-V translator and
// xenos.h (reference only, nothing copied):
//   - index: the source register component, floor(x + 0.5) when is_index_rounded, else floor(x); no index when the
//     stride is 0. A mini fetch reuses index * stride of the last full fetch the shader ran (a Function variable;
//     the decoder already gave it that fetch's register, slot and stride) with its own offset and format;
//   - dword offset d = index * stride + offset (signed, dwords); the element covers dwords d .. d + words - 1 of the
//     slot. When any of them is before the slot start or past size_bytes, every component is 0 (the destination
//     swizzle's 1 selector still gives 1) and nothing is read: the guest can never make the shader read outside the
//     uploaded buffer (Review Focus 7). The index is clamped (NaN to the low end) before the integer conversion so
//     index * stride + offset never overflows 32 bits; a clamped index is out of range for every slot anyway;
//   - each dword is swapped per the slot's endian mode (0 none, 1 8in16, 2 8in32, 3 16in32), then decoded: packed
//     fields from bit 0 up in component order x, y, z, w (k_8_8_8_8 8:8:8:8, k_2_10_10_10 10:10:10:2,
//     k_10_11_11 11:11:10, k_11_11_10 10:11:11, k_16_16 and k_16_16_16_16 16:16 per dword), sign- or zero-extended
//     per is_signed; k_32* integers; *_FLOAT 32-bit floats or half pairs (x in the low half);
//   - normalized (num_format 0) integer formats: unsigned v / (2^n - 1); signed with signed_rf_mode 0 v / (2^(n-1) - 1)
//     clamped to -1, with signed_rf_mode 1 (v + 0.5) / (2^(n-1) - 0.5). Integer formats (num_format 1) keep the value.
//     Float formats ignore is_signed and num_format;
//   - exp_adjust: the format's components times 2^exp_adjust;
//   - components the format does not have are (0, 0, 0, 1) (Xenia pads with 0; the Direct3D default, planner's call);
//   - the destination swizzle: x..w, 0, 1 or keep per component (selector 6 reads 0, as in Xenia);
//   - a predicated fetch is evaluated unconditionally and merged with OpSelect on p0 (and so is the mini-fetch base).
// Every float multiply and add is NoContraction (arith()).
#include <spirv/unified1/GLSL.std.450.h>

#include <array>
#include <cmath>
#include <optional>
#include <string>

#include "runtime/gpu/shader/disasm.h"
#include "runtime/gpu/shader/translator_internal.h"

namespace lr::gpu::shader::detail {

namespace {

enum class FormatKind { Packed, Half, Int32, Float32 };

struct VertexFormat {
  FormatKind kind;
  uint32_t words;       // dwords per element
  uint32_t components;  // 1..4
  // Packed only, per component: the dword, the lowest bit and the width.
  std::array<uint8_t, 4> word{};
  std::array<uint8_t, 4> offset{};
  std::array<uint8_t, 4> width{};
};

std::optional<VertexFormat> vertex_format(uint32_t format) {
  using K = FormatKind;
  switch (format) {
    case 6: return VertexFormat{K::Packed, 1, 4, {0, 0, 0, 0}, {0, 8, 16, 24}, {8, 8, 8, 8}};  // 8_8_8_8
    case 7: return VertexFormat{K::Packed, 1, 4, {0, 0, 0, 0}, {0, 10, 20, 30}, {10, 10, 10, 2}};  // 2_10_10_10
    case 16: return VertexFormat{K::Packed, 1, 3, {0, 0, 0, 0}, {0, 11, 22, 0}, {11, 11, 10, 0}};  // 10_11_11
    case 17: return VertexFormat{K::Packed, 1, 3, {0, 0, 0, 0}, {0, 10, 21, 0}, {10, 11, 11, 0}};  // 11_11_10
    case 25: return VertexFormat{K::Packed, 1, 2, {0, 0, 0, 0}, {0, 16, 0, 0}, {16, 16, 0, 0}};  // 16_16
    case 26: return VertexFormat{K::Packed, 2, 4, {0, 0, 1, 1}, {0, 16, 0, 16}, {16, 16, 16, 16}};  // 16_16_16_16
    case 31: return VertexFormat{K::Half, 1, 2, {}, {}, {}};  // 16_16_FLOAT
    case 32: return VertexFormat{K::Half, 2, 4, {}, {}, {}};  // 16_16_16_16_FLOAT
    case 33: return VertexFormat{K::Int32, 1, 1, {}, {}, {}};  // 32
    case 34: return VertexFormat{K::Int32, 2, 2, {}, {}, {}};  // 32_32
    case 35: return VertexFormat{K::Int32, 4, 4, {}, {}, {}};  // 32_32_32_32
    case 36: return VertexFormat{K::Float32, 1, 1, {}, {}, {}};  // 32_FLOAT
    case 37: return VertexFormat{K::Float32, 2, 2, {}, {}, {}};  // 32_32_FLOAT
    case 38: return VertexFormat{K::Float32, 4, 4, {}, {}, {}};  // 32_32_32_32_FLOAT
    case 57: return VertexFormat{K::Float32, 3, 3, {}, {}, {}};  // 32_32_32_FLOAT
    default: return std::nullopt;
  }
}

// |index * stride| stays at most this (plus one float rounding step), so adding the 23-bit offset never overflows an
// int32; any element that far out is past every slot (size_bytes / 4 <= 2^30) anyway.
constexpr double kIndexSpan = double(1u << 30) + double(1u << 23);

}  // namespace

Result<void> Translator::emit_fetch(uint32_t slot, const FetchInstr& f) {
  if (f.op == FetchOp::VertexFetch) return emit_vertex_fetch(slot, f);
  return unsupported(slot, "fetch", fetch_op_name(f.op));
}

void Translator::declare_fetch_contract() {
  if (!program_.vfetch_slots.any()) return;
  // { { uvec2 address; uint size_bytes; uint endian; } entries[96]; }
  const Id entry_members[] = {t_.uvec2, t_.u32, t_.u32};
  const Id entry = b_.type_struct(entry_members);
  b_.member_decorate(entry, 0, spv::DecorationOffset, {kVertexFetchEntryAddress});
  b_.member_decorate(entry, 1, spv::DecorationOffset, {kVertexFetchEntrySize});
  b_.member_decorate(entry, 2, spv::DecorationOffset, {kVertexFetchEntryEndian});
  const Id entries = b_.type_array(entry, const_u(kVertexFetchSlots));
  b_.decorate(entries, spv::DecorationArrayStride, {kVertexFetchEntryBytes});
  const Id table_members[] = {entries};
  const Id table = b_.type_struct(table_members);
  b_.decorate(table, spv::DecorationBlock);
  b_.member_decorate(table, 0, spv::DecorationOffset, {0});
  fetch_table_ptr_type_ = b_.type_pointer(spv::StorageClassPhysicalStorageBuffer, table);

  // The vertex data: { uint words[]; }.
  const Id words = b_.type_runtime_array(t_.u32);
  b_.decorate(words, spv::DecorationArrayStride, {4});
  const Id data_members[] = {words};
  const Id data = b_.type_struct(data_members);
  b_.decorate(data, spv::DecorationBlock);
  b_.member_decorate(data, 0, spv::DecorationOffset, {0});
  vertex_data_ptr_type_ = b_.type_pointer(spv::StorageClassPhysicalStorageBuffer, data);
}

void Translator::load_fetch_table_pointer() {
  if (!fetch_table_ptr_type_) return;
  const Id member = b_.op(spv::OpAccessChain, b_.type_pointer(spv::StorageClassPushConstant, t_.uvec2),
                          {push_block_, const_u(3)});
  fetch_table_ = b_.op(spv::OpBitcast, fetch_table_ptr_type_, {b_.op(spv::OpLoad, t_.uvec2, {member})});
  vfetch_base_ =
      b_.variable(spv::StorageClassFunction, b_.type_pointer(spv::StorageClassFunction, t_.i32), "vfetch_base");
  b_.op_void(spv::OpStore, {vfetch_base_, b_.constant_i32(0)});
}

Result<void> Translator::emit_vertex_fetch(uint32_t slot, const FetchInstr& f) {
  const std::optional<VertexFormat> format = vertex_format(f.format);
  if (!format) return unsupported(slot, "fetch", "vertex format " + std::to_string(f.format));
  if (!fetch_table_ || !vfetch_base_) return internal("vfetch without the vertex-fetch table");
  const VertexFormat& fmt = *format;
  const Id predicate = f.predicated ? pred_matches(f.pred_condition) : 0;

  // index * stride: computed by a full fetch, reused by a mini fetch.
  Id base = 0;
  if (f.mini) {
    base = b_.op(spv::OpLoad, t_.i32, {vfetch_base_});
  } else {
    if (f.stride == 0) {
      base = b_.constant_i32(0);
    } else {
      const Id src = b_.op(spv::OpLoad, t_.vec4, {reg_ptr(temp_index(f.src_reg, f.src_rel))});
      Id x = extract(src, f.index_component & 3);
      if (f.rounded) x = arith(spv::OpFAdd, t_.f32, {x, const_f(0.5f)});
      x = glsl(t_.f32, GLSLstd450Floor, {x});
      const auto limit = static_cast<float>(std::floor(kIndexSpan / f.stride));
      x = glsl(t_.f32, GLSLstd450NMax, {x, const_f(-limit)});  // NaN -> -limit
      x = glsl(t_.f32, GLSLstd450NMin, {x, const_f(limit)});
      base = b_.op(spv::OpIMul, t_.i32,
                   {b_.op(spv::OpConvertFToS, t_.i32, {x}), b_.constant_i32(static_cast<int32_t>(f.stride))});
    }
    Id stored = base;
    if (predicate)
      stored = b_.op(spv::OpSelect, t_.i32, {predicate, base, b_.op(spv::OpLoad, t_.i32, {vfetch_base_})});
    b_.op_void(spv::OpStore, {vfetch_base_, stored});
  }
  const Id first = b_.op(spv::OpIAdd, t_.i32, {base, b_.constant_i32(f.offset)});

  // The slot's table entry.
  const auto entry_member = [&](uint32_t m, Id type, uint32_t align) {
    const Id ptr = b_.op(spv::OpAccessChain, b_.type_pointer(spv::StorageClassPhysicalStorageBuffer, type),
                         {fetch_table_, const_u(0), const_u(f.slot), const_u(m)});
    return b_.op(spv::OpLoad, type, {ptr, spv::MemoryAccessAlignedMask, align});
  };
  const Id address = entry_member(0, t_.uvec2, 8);
  const Id size_dwords = b_.op(spv::OpShiftRightLogical, t_.u32, {entry_member(1, t_.u32, 4), const_u(2)});
  const Id endian = b_.op(spv::OpBitwiseAnd, t_.u32, {entry_member(2, t_.u32, 4), const_u(3)});

  // Lower and upper bound: 0 <= first and first + words <= size_bytes / 4 (|first| < 2^31, so the unsigned sum of a
  // non-negative first cannot wrap).
  const Id first_u = b_.op(spv::OpBitcast, t_.u32, {first});
  const Id end = b_.op(spv::OpIAdd, t_.u32, {first_u, const_u(fmt.words)});
  const Id in_range = b_.op(spv::OpLogicalAnd, t_.bool_,
                            {b_.op(spv::OpSGreaterThanEqual, t_.bool_, {first, b_.constant_i32(0)}),
                             b_.op(spv::OpULessThanEqual, t_.bool_, {end, size_dwords})});

  const Id result_var =
      b_.variable(spv::StorageClassFunction, b_.type_pointer(spv::StorageClassFunction, t_.vec4), "vfetch");
  b_.op_void(spv::OpStore, {result_var, const_vec4(0, 0, 0, 0)});
  LR_TRY(emit_if(in_range, [&]() -> Result<void> {
    const Id data = b_.op(spv::OpBitcast, vertex_data_ptr_type_, {address});
    const auto is = [&](uint32_t mode) { return b_.op(spv::OpIEqual, t_.bool_, {endian, const_u(mode)}); };
    const Id swap_bytes = b_.op(spv::OpLogicalOr, t_.bool_, {is(1), is(2)});   // 8in16, 8in32
    const Id swap_halves = b_.op(spv::OpLogicalOr, t_.bool_, {is(2), is(3)});  // 8in32, 16in32
    const auto u32_op = [&](spv::Op op, Id a, Id b) { return b_.op(op, t_.u32, {a, b}); };
    const Id low_bytes = const_u(0x00FF00FFu);
    std::array<Id, 4> w{};
    for (uint32_t k = 0; k < fmt.words; ++k) {
      const Id ptr = b_.op(spv::OpAccessChain, b_.type_pointer(spv::StorageClassPhysicalStorageBuffer, t_.u32),
                           {data, const_u(0), b_.op(spv::OpIAdd, t_.u32, {first_u, const_u(k)})});
      Id v = b_.op(spv::OpLoad, t_.u32, {ptr, spv::MemoryAccessAlignedMask, 4});
      const Id bytes = u32_op(spv::OpBitwiseOr,
                              u32_op(spv::OpShiftLeftLogical, u32_op(spv::OpBitwiseAnd, v, low_bytes), const_u(8)),
                              u32_op(spv::OpBitwiseAnd, u32_op(spv::OpShiftRightLogical, v, const_u(8)), low_bytes));
      v = b_.op(spv::OpSelect, t_.u32, {swap_bytes, bytes, v});
      const Id halves = u32_op(spv::OpBitwiseOr, u32_op(spv::OpShiftLeftLogical, v, const_u(16)),
                               u32_op(spv::OpShiftRightLogical, v, const_u(16)));
      w[k] = b_.op(spv::OpSelect, t_.u32, {swap_halves, halves, v});
    }

    // An n-bit integer (as a float) to the normalized value.
    const auto scale = [&](Id v, double s) { return arith(spv::OpFMul, t_.f32, {v, const_f(static_cast<float>(s))}); };
    const auto normalize = [&](Id v, uint32_t n, bool is_signed) {
      if (!f.normalized) return v;
      if (!is_signed) return scale(v, 1.0 / (std::ldexp(1.0, n) - 1));
      if (f.signed_rf_mode) {
        const double s = 1.0 / (std::ldexp(1.0, n - 1) - 0.5);
        return arith(spv::OpFAdd, t_.f32, {scale(v, s), const_f(static_cast<float>(0.5 * s))});
      }
      return glsl(t_.f32, GLSLstd450FMax, {scale(v, 1.0 / (std::ldexp(1.0, n - 1) - 1)), const_f(-1.0f)});
    };

    std::array<Id, 4> comp{};
    for (uint32_t i = 0; i < fmt.components; ++i) {
      switch (fmt.kind) {
        case FormatKind::Packed: {
          const Id word = w[fmt.word[i]];
          const Id offset = const_u(fmt.offset[i]);
          const Id width = const_u(fmt.width[i]);
          Id v = 0;
          if (f.is_signed) {
            const Id signed_word = b_.op(spv::OpBitcast, t_.i32, {word});
            const Id field = b_.op(spv::OpBitFieldSExtract, t_.i32, {signed_word, offset, width});
            v = b_.op(spv::OpConvertSToF, t_.f32, {field});
          } else {
            v = b_.op(spv::OpConvertUToF, t_.f32, {b_.op(spv::OpBitFieldUExtract, t_.u32, {word, offset, width})});
          }
          comp[i] = normalize(v, fmt.width[i], f.is_signed);
          break;
        }
        case FormatKind::Half:
          comp[i] = extract(glsl(t_.vec2, GLSLstd450UnpackHalf2x16, {w[i / 2]}), i % 2);
          break;
        case FormatKind::Int32: {
          const Id v = f.is_signed ? b_.op(spv::OpConvertSToF, t_.f32, {b_.op(spv::OpBitcast, t_.i32, {w[i]})})
                                   : b_.op(spv::OpConvertUToF, t_.f32, {w[i]});
          comp[i] = normalize(v, 32, f.is_signed);
          break;
        }
        case FormatKind::Float32: comp[i] = b_.op(spv::OpBitcast, t_.f32, {w[i]}); break;
      }
      if (f.exp_adjust != 0)
        comp[i] = arith(spv::OpFMul, t_.f32, {comp[i], const_f(std::ldexp(1.0f, f.exp_adjust))});
    }
    for (uint32_t i = fmt.components; i < 4; ++i) comp[i] = const_f(i == 3 ? 1.0f : 0.0f);
    b_.op_void(spv::OpStore,
               {result_var, b_.op(spv::OpCompositeConstruct, t_.vec4, {comp[0], comp[1], comp[2], comp[3]})});
    return {};
  }));
  const Id fetched = b_.op(spv::OpLoad, t_.vec4, {result_var});

  // The destination swizzle: x..w, 0 (and the invalid 6), 1, or keep.
  uint32_t mask = 0;
  std::array<uint32_t, 4> pick{};
  for (uint32_t j = 0; j < 4; ++j) {
    const uint32_t sel = (f.dst_swizzle >> (3 * j)) & 7;
    if (sel == kFetchSwizzleKeep) {
      pick[j] = j;
      continue;
    }
    mask |= 1u << j;
    pick[j] = sel < 4 ? sel : (sel == kFetchSwizzleOne ? 5 : 4);  // 4, 5: the (0, 1) constant's x, y
  }
  if (mask == 0) return {};
  const Id value = b_.op(spv::OpVectorShuffle, t_.vec4,
                         {fetched, const_vec4(0, 1, 0, 0), pick[0], pick[1], pick[2], pick[3]});
  const Id dst = temp_index(f.dst_reg, f.dst_rel);
  if (!predicate) {
    store_reg_masked_at(dst, value, mask);
    return {};
  }
  const Id ptr = reg_ptr(dst);
  const Id old = b_.op(spv::OpLoad, t_.vec4, {ptr});
  const Id merged = merge_masked(old, value, mask);
  b_.op_void(spv::OpStore, {ptr, b_.op(spv::OpSelect, t_.vec4, {predicate, merged, old})});
  return {};
}

}  // namespace lr::gpu::shader::detail
