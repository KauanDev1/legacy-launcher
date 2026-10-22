// SPDX-License-Identifier: MIT
// The plan B runtime contract (ADR 0005 M8): every number shared by the translated SPIR-V (translator*.cpp) and the
// runtime that feeds it (the Vulkan backend, Task 20). docs/architecture.md "Contrato dos shaders traduzidos (plano
// B)" describes the same numbers in prose; change both together, and bump kTranslatorVersion (translator.h) when the
// translated code changes. CPU-only, no Vulkan types.
#pragma once

#include <cstdint>

namespace lr::gpu::shader {

// --- Xenos export registers (the vector_dest of an ALU instruction with export_data) --------------------------------
inline constexpr uint32_t kExportInterpolatorCount = 16;  // VS 0..15: interpolators, linked to the PS by index
inline constexpr uint32_t kExportColorCount = 4;          // PS 0..3: oC0..oC3
inline constexpr uint32_t kExportAddress = 32;            // eA (memexport), both stages
inline constexpr uint32_t kExportData0 = 33;              // eM0..eM4 = 33..37 (memexport), both stages
inline constexpr uint32_t kExportDataCount = 5;
inline constexpr uint32_t kExportPsDepth = 61;            // PS depth (x)
inline constexpr uint32_t kExportVsPosition = 62;         // VS position
inline constexpr uint32_t kExportVsPointSize = 63;        // VS point size / edge flag / kill vertex (dropped in M3)

// --- push constants: four buffer device addresses, each read as a uvec2 (low, high) and bitcast to a
//     PhysicalStorageBuffer64 pointer; visible to the vertex and fragment stages ------------------------------------
inline constexpr uint32_t kPushConstantBytes = 32;
inline constexpr uint32_t kPushVsConstants = 0;     // 256 float4: VS float constants c0..c255
inline constexpr uint32_t kPushPsConstants = 8;     // 256 float4: PS float constants (register file 256 + i)
inline constexpr uint32_t kPushSharedBlock = 16;    // the shared block below
inline constexpr uint32_t kPushVertexFetchTable = 24;  // kVertexFetchSlots entries of kVertexFetchEntryBytes

// --- float constant buffers ---------------------------------------------------------------------------------------
inline constexpr uint32_t kFloatConstantCount = 256;
inline constexpr uint32_t kFloatConstantBytes = kFloatConstantCount * 16;  // float4, stride 16, 16-byte aligned base

// --- shared block (little-endian, 16-byte aligned base) -----------------------------------------------------------
inline constexpr uint32_t kSharedTexture2DIndex = 0;    // uint[32]: Texture2D heap index (set 0) per fetch constant
inline constexpr uint32_t kSharedTexture3DIndex = 128;  // uint[32]: Texture3D heap index (set 1)
inline constexpr uint32_t kSharedTextureCubeIndex = 256;  // uint[32]: TextureCube heap index (set 2)
inline constexpr uint32_t kSharedSamplerIndex = 384;    // uint[32]: sampler heap index (set 3)
inline constexpr uint32_t kSharedBools = 512;           // uint[8]: bool b = bit (b % 32) of word b / 32
inline constexpr uint32_t kSharedLoops = 544;           // uint[32]: loop constants (count, start, step bytes)
inline constexpr uint32_t kSharedNdcScale = 672;        // float4: applied to xyz of the position
inline constexpr uint32_t kSharedNdcOffset = 688;       // float4: xyz added as offset * w
inline constexpr uint32_t kSharedAlphaRef = 704;        // float: alpha test reference (RB_ALPHA_REF)
inline constexpr uint32_t kSharedVteFlags = 708;        // uint: kVte* bits (PA_CL_VTE_CNTL)
inline constexpr uint32_t kSharedIndexOffset = 712;     // int: VGT_INDX_OFFSET
inline constexpr uint32_t kSharedMinIndex = 716;        // uint: VGT_MIN_VTX_INDX
inline constexpr uint32_t kSharedMaxIndex = 720;        // uint: VGT_MAX_VTX_INDX
inline constexpr uint32_t kSharedReserved = 724;        // 724..735: reserved, zero
inline constexpr uint32_t kSharedBlockBytes = 736;

// VTE flags, applied by the VS to the exported position before ndc_scale/ndc_offset (Xenia's semantics):
inline constexpr uint32_t kVteXyDividedByW = 1u << 0;     // vtx_xy_fmt: xy were divided by W; multiply them back
inline constexpr uint32_t kVteZDividedByW = 1u << 1;      // vtx_z_fmt: z was divided by W; multiply it back
inline constexpr uint32_t kVteW0NotReciprocal = 1u << 2;  // vtx_w0_fmt: W is W; when clear, the VS exports 1/W

// VS r0.x = float(clamp((VertexIndex + index_offset) & kVertexIndexMask, min_index, max_index)), clamp unsigned.
inline constexpr uint32_t kVertexIndexMask = 0xFFFFFF;  // the hardware vertex index is 24 bits

// --- vertex fetch table (Task 15.8): one entry per vertex fetch constant ------------------------------------------
inline constexpr uint32_t kVertexFetchSlots = 96;
inline constexpr uint32_t kVertexFetchEntryBytes = 16;
inline constexpr uint32_t kVertexFetchEntryAddress = 0;  // uint64 (read as uvec2): buffer device address
inline constexpr uint32_t kVertexFetchEntrySize = 8;     // uint32: size in bytes
inline constexpr uint32_t kVertexFetchEntryEndian = 12;  // uint32: Xenos endian swap mode

// --- bindless descriptor sets (binding 0 each, runtime arrays, partially bound, update-after-bind) ------------------
inline constexpr uint32_t kSetTexture2D = 0;  // Texture2D heap; 1D textures are height-1 2D textures
inline constexpr uint32_t kSetTexture3D = 1;
inline constexpr uint32_t kSetTextureCube = 2;
inline constexpr uint32_t kSetSamplers = 3;
inline constexpr uint32_t kHeapBinding = 0;

// --- specialization constants -------------------------------------------------------------------------------------
inline constexpr uint32_t kSpecAlphaTest = 0;            // uint, pixel shader
inline constexpr uint32_t kAlphaTestEnable = 1u << 1;    // bit 0 unused
inline constexpr uint32_t kAlphaTestFuncShift = 2;       // bits 2..4: RB_COLORCONTROL.ALPHA_FUNC
inline constexpr uint32_t kAlphaTestFuncMask = 7;        // 0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always

// --- stage interface ----------------------------------------------------------------------------------------------
// The VS always declares kExportInterpolatorCount vec4 outputs at locations 0..15 (zero unless written); the PS
// declares an input at location r for every register r < 16 it references. Neither declares vertex inputs.
inline constexpr uint32_t kInterpolatorLocations = kExportInterpolatorCount;

static_assert(kSharedNdcScale % 16 == 0 && kSharedNdcOffset % 16 == 0);
static_assert(kSharedReserved + 12 == kSharedBlockBytes);
static_assert(kVertexFetchSlots * kVertexFetchEntryBytes == 1536);

}  // namespace lr::gpu::shader
