// SPDX-License-Identifier: MIT
// The draw path (Task 20, plan B; docs/architecture.md "Desenho"): one guest draw (PM4 DRAW_INDX / DRAW_INDX_2 in
// RB_MODECONTROL edram_mode 4 or 5) becomes one Vulkan draw with the translated shaders, following the runtime
// contract of runtime/gpu/shader/contract.h:
//   - per draw, in an upload ring (host-visible, buffer device address): the VS and PS float constants, the shared
//     block (texture and sampler heap indices, bools, loops, ndc_scale/ndc_offset, alpha reference, VTE flags,
//     index offset and min/max), the vertex fetch table with a raw copy of every guest vertex range the vertex
//     shader fetches, and the index buffer (endian-swapped, primitive reset rewritten, quads expanded);
//   - the 32 bytes of push constants (four device addresses), dynamic viewport/scissor/depth bias/blend
//     constants/stencil state, and the pipeline of PipelineCache (pipelines.h) for the rest of the state.
// Every guest read (index buffer, vertex ranges) is checked against guest memory, the physical allocation
// (MemoryAccess::allocation_end) and a linear mapping; what does not fit is an InvalidFormat error naming the
// address, which VkBackend turns into a guest fatal (exit 5). Semantics checked against Xenia's draw_util.cc,
// primitive_processor.cc, vulkan_pipeline_cache.cc and vulkan_command_processor.cc (reference only, no code taken).
//
// The pure helpers below (no device) are what the tests check one by one.
//
// Thread-safety: GPU thread only.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "runtime/gpu/shader/shader_key.h"
#include "runtime/gpu/vk/device.h"
#include "runtime/gpu/vk/vk_backend.h"

namespace lr::gpu::vk {

class CommandStream;
class Edram;
class TextureCache;
class SamplerCache;
class PipelineCache;

// VGT_DRAW_INITIATOR.prim_type values the draw path knows (Xenia xenos.h PrimitiveType, hardware facts).
namespace prim {
inline constexpr uint32_t kNone = 0, kPointList = 1, kLineList = 2, kLineStrip = 3, kTriangleList = 4,
                          kTriangleFan = 5, kTriangleStrip = 6, kTriangleWithWFlags = 7, kRectangleList = 8,
                          kLineLoop = 0x0C, kQuadList = 0x0D, kQuadStrip = 0x0E, kPolygon = 0x0F;
}

// How a guest primitive type is drawn.
struct Primitive {
  uint32_t topology = 0;  // VkPrimitiveTopology
  bool list = false;      // a list topology (Vulkan 1.2 forbids primitive restart on those)
  bool polygonal = false; // triangles: culling, polygon mode, back-face stencil apply
  bool quads = false;     // quad list: indices expanded to two triangles per quad
  bool rects = false;     // rectangle list: the geometry stage adds the fourth corner
  const char* name = "";
};
// InvalidFormat "primitive type 0x0C (line loop) is not supported" for anything but point, line and triangle
// lists, strips and fans, rectangle lists and quad lists.
Result<Primitive> primitive(uint32_t prim_type);

// The host viewport and the contract's ndc_scale/ndc_offset (Xenia draw_util GetHostViewportInfo semantics):
// PA_CL_VPORT_* (each one 1 or 0 unless its PA_CL_VTE_CNTL enable bit is set), the window offset
// (PA_SC_WINDOW_OFFSET when PA_SU_SC_MODE_CNTL.vtx_window_offset_enable) and half a pixel when PA_SU_VTX_CNTL
// .pix_center is 0 (the Direct3D 9 pixel centre at integer coordinates; Vulkan's is at .5). With clipping
// (PA_CL_CLIP_CNTL.clip_disable clear) the viewport snaps to whole pixels inside [0, target) and ndc_scale/offset
// carry the rest; without, it covers the whole target. Z: PA_CL_CLIP_CNTL.dx_clip_space_def selects [0, w] (as is)
// or [-w, w] (halved), the depth range is the viewport's z range clamped to [0, 1] ([0, 1] when the pixel shader
// writes depth), halved for a D24FS8 target (stored as [0, 2) -> [0, 1)). Y is not flipped: the guest's negative
// YSCALE and Vulkan's y-down framebuffer agree. An empty viewport has width or height 0 (the draw is skipped).
struct ViewportInfo {
  float x = 0, y = 0, width = 0, height = 0, min_depth = 0, max_depth = 1;
  std::array<float, 4> ndc_scale{1, 1, 1, 1};
  std::array<float, 4> ndc_offset{0, 0, 0, 0};
};
ViewportInfo viewport_info(const RegisterFile& regs, uint32_t target_width, uint32_t target_height,
                           bool ps_writes_depth, bool depth_float24);

// The scissor (Xenia draw_util GetScissor): the window scissor, moved by the window offset unless
// PA_SC_WINDOW_SCISSOR_TL.window_offset_disable, intersected with the screen scissor, then with the target.
struct ScissorRect {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
};
ScissorRect scissor_rect(const RegisterFile& regs, uint32_t target_width, uint32_t target_height);

// The indices a draw uses, as uint32 (the 24 bits the hardware keeps; a primitive reset becomes 0xFFFFFFFF).
struct GuestIndices {
  std::vector<uint32_t> indices;
  bool restart = false;  // a reset index was found: primitiveRestartEnable
};
// Reads min(index_count, index_words) indices of index_size bytes at index_base_phys (DMA), swapped per
// index_endian (pm4::kEndian*: none, 8in16, 8in32, 16in32, applied to each 32-bit word read little-endian), masked
// to 24 bits; with PA_SU_SC_MODE_CNTL.multi_prim_ib_ena, indices equal to VGT_MULTI_PRIM_IB_RESET_INDX (24 bits;
// its low 16 for 16-bit indices) become 0xFFFFFFFF. A reset in a list topology is an InvalidFormat error, since
// Vulkan 1.2 cannot restart a list. Outside guest memory or its physical allocation: InvalidFormat naming it.
// Quad lists are expanded to (a, b, c), (a, c, d) per quad; auto-index draws of quads get generated indices.
Result<GuestIndices> read_indices(const DrawInfo& info, const RegisterFile& regs, const MemoryAccess& mem,
                                  const Primitive& primitive);
// Indices (or 0..count-1 for an auto-index draw) of a quad list as a triangle list.
std::vector<uint32_t> expand_quads(const std::vector<uint32_t>& quad_indices);

// The guest range of vertex fetch constant `slot` (0..95, registers kShaderConstantFetchBase + 2 * slot): base
// (dword 0 bits 2-31, physical) and size (dword 1 bits 2-25, in dwords; size_bytes = 4 x that, clamped to the
// physical allocation holding the base and to guest memory), endian (dword 1 bits 0-1). nullopt-like empty range
// (size 0) for a fetch constant of another type or size 0; InvalidFormat naming the slot and the address when the
// base is in no guest memory or no physical allocation.
struct VertexRange {
  uint32_t guest = 0;
  uint32_t size_bytes = 0;
  uint32_t endian = 0;
};
Result<VertexRange> vertex_range(const RegisterFile& regs, uint32_t slot, const MemoryAccess& mem);

// The 736-byte shared block (contract.h offsets) for a draw, from the registers and the heap indices.
struct HeapIndices {
  std::array<uint32_t, 32> texture_2d{}, texture_3d{}, texture_cube{}, sampler{};
};
std::array<uint8_t, 736> shared_block(const RegisterFile& regs, const HeapIndices& heaps, const ViewportInfo& vp);

class DrawPath {
 public:
  static Result<std::unique_ptr<DrawPath>> create(const Device& device, MemoryAccess mem, CommandStream& stream,
                                                  Edram& edram, TextureCache& textures, SamplerCache& samplers);
  ~DrawPath();
  DrawPath(const DrawPath&) = delete;
  DrawPath& operator=(const DrawPath&) = delete;

  // Records one draw into the stream. InvalidFormat: a guest error (named); Internal: a Vulkan failure; Cancelled:
  // the GPU is stopping. A draw that cannot produce anything (no vertex shader, prim_type 0, no bound surface, an
  // empty viewport or scissor) is skipped silently or with one warning.
  Result<void> draw(const RegisterFile& regs, const DrawInfo& info, const ActiveShaders& shaders);
  const DrawStats& stats() const;

 private:
  struct Impl;
  explicit DrawPath(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace lr::gpu::vk
