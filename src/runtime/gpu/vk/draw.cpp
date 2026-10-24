// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/draw.h"

#include <algorithm>
#include <bit>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "runtime/gpu/shader/contract.h"
#include "runtime/gpu/shader/translator.h"
#include "runtime/gpu/texture_format.h"
#include "runtime/gpu/vk/edram.h"
#include "runtime/gpu/vk/pipelines.h"
#include "runtime/gpu/vk/samplers.h"
#include "runtime/gpu/vk/stream.h"
#include "runtime/gpu/vk/textures.h"
#include "vk_mem_alloc.h"

namespace lr::gpu::vk {
namespace {

namespace sc = shader;

template <class T>
T make(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

uint32_t bits(uint32_t v, uint32_t lo, uint32_t count) { return (v >> lo) & ((1u << count) - 1u); }

int32_t sbits(uint32_t v, uint32_t lo, uint32_t count) {
  const uint32_t u = bits(v, lo, count);
  const uint32_t sign = 1u << (count - 1);
  return int32_t(u ^ sign) - int32_t(sign);
}

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08X", v);
  return b;
}

Error guest_error(const std::string& message) { return Error{ErrorCode::InvalidFormat, message, ""}; }

Error vk_error(const char* what, VkResult r) {
  return Error{ErrorCode::Internal, std::string("draw: ") + what + " failed (VkResult " + std::to_string(r) + ")", ""};
}

// A float register, or `fallback` when it is not finite (guest garbage never reaches the host as NaN or infinity).
float finite_float(const RegisterFile& regs, uint32_t reg, float fallback) {
  const float f = regs.read_float(reg);
  return std::isfinite(f) ? f : fallback;
}

constexpr uint64_t kPhysicalSpace = uint64_t{pm4::kPhysicalMask} + 1;

struct Mapped {
  uint32_t guest = 0;
  uint32_t bytes = 0;
};

// Where `bytes` bytes at physical `phys` are in guest memory. The range must be in guest memory, inside the
// physical allocation holding `phys` (MemoryAccess::allocation_end, when known) and map linearly. `clamp`: a range
// running past the allocation or guest memory is cut there instead of failing. The error message completes "<what>
// at physical 0x... ".
Result<Mapped> map_guest(const MemoryAccess& mem, uint32_t phys, uint64_t bytes, bool clamp) {
  phys &= pm4::kPhysicalMask;
  const std::optional<uint32_t> guest = mem.to_guest ? mem.to_guest(phys) : std::nullopt;
  if (!guest || mem.base == nullptr || *guest >= mem.size) return guest_error("is not in guest memory");
  uint64_t room = std::min<uint64_t>(mem.size - *guest, kPhysicalSpace - phys);
  if (mem.allocation_end) {
    const std::optional<uint32_t> end = mem.allocation_end(phys);
    if (!end || *end <= phys) return guest_error("is not in a physical allocation");
    room = std::min<uint64_t>(room, uint64_t{*end} - phys);
  }
  if (bytes > room) {
    if (!clamp)
      return guest_error("runs past the end of its physical allocation or of guest memory (" + std::to_string(bytes) +
                         " bytes, " + std::to_string(room) + " available)");
    bytes = room;
  }
  if (bytes > 0) {
    const std::optional<uint32_t> last = mem.to_guest(uint32_t(phys + bytes - 1));
    if (!last || uint64_t{*last} != uint64_t{*guest} + bytes - 1)
      return guest_error("does not map to one contiguous guest range");
  }
  return Mapped{*guest, uint32_t(bytes)};
}

const char* primitive_name(uint32_t t) {
  switch (t) {
    case prim::kPointList: return "point list";
    case prim::kLineList: return "line list";
    case prim::kLineStrip: return "line strip";
    case prim::kTriangleList: return "triangle list";
    case prim::kTriangleFan: return "triangle fan";
    case prim::kTriangleStrip: return "triangle strip";
    case prim::kTriangleWithWFlags: return "triangle with W flags";
    case prim::kRectangleList: return "rectangle list";
    case prim::kLineLoop: return "line loop";
    case prim::kQuadList: return "quad list";
    case prim::kQuadStrip: return "quad strip";
    case prim::kPolygon: return "polygon";
    case 0x10: return "2D copy rect list V0 / line patch";
    case 0x11: return "2D copy rect list V1 / triangle patch";
    case 0x12: return "2D copy rect list V2 / quad patch";
    case 0x13: return "2D copy rect list V3";
    case 0x14: return "2D fill rect list";
    case 0x15: return "2D line strip";
    case 0x16: return "2D triangle strip";
    default: return "unknown";
  }
}

// Xenos blend factor (RB_BLENDCONTROL, 5 bits) -> Vulkan; the unused values are zero (Xenia does the same).
VkBlendFactor blend_factor(uint32_t f) {
  switch (f) {
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ZERO;
  }
}

// Xenos blend function (3 bits) -> Vulkan; unknown values add.
VkBlendOp blend_op(uint32_t op) {
  switch (op) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

// RB_DEPTHCONTROL compare functions and stencil operations share Vulkan's numbering (never, less, equal, ...;
// keep, zero, replace, incr clamp, decr clamp, invert, incr wrap, decr wrap).
uint32_t compare_op(uint32_t xenos) { return uint32_t(VK_COMPARE_OP_NEVER) + (xenos & 7); }
uint32_t stencil_op(uint32_t xenos) { return uint32_t(VK_STENCIL_OP_KEEP) + (xenos & 7); }

constexpr VkDeviceSize kRingInitialBytes = VkDeviceSize{8} << 20;
constexpr VkDeviceSize kRingMaxBytes = VkDeviceSize{512} << 20;

constexpr uint32_t kBlendControl[kColorSlots] = {RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2,
                                                 RB_BLENDCONTROL3};

void put32(uint8_t* p, uint32_t offset, uint32_t value) { std::memcpy(p + offset, &value, 4); }
void put_float(uint8_t* p, uint32_t offset, float value) { std::memcpy(p + offset, &value, 4); }

}  // namespace

// --- pure helpers -------------------------------------------------------------------------------------------------

Result<Primitive> primitive(uint32_t prim_type) {
  Primitive p;
  p.name = primitive_name(prim_type);
  switch (prim_type) {
    case prim::kPointList:
      p.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      p.list = true;
      return p;
    case prim::kLineList:
      p.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      p.list = true;
      return p;
    case prim::kLineStrip:
      p.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      return p;
    case prim::kTriangleList:
      p.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      p.list = p.polygonal = true;
      return p;
    case prim::kTriangleFan:
      p.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
      p.polygonal = true;
      return p;
    case prim::kTriangleStrip:
      p.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      p.polygonal = true;
      return p;
    case prim::kRectangleList:
      p.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      p.list = p.polygonal = p.rects = true;
      return p;
    case prim::kQuadList:
      p.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      p.list = p.polygonal = p.quads = true;
      return p;
    default: {
      char b[96];
      std::snprintf(b, sizeof b, "primitive type 0x%02X (%s) is not supported", prim_type, primitive_name(prim_type));
      return guest_error(b);
    }
  }
}

ViewportInfo viewport_info(const RegisterFile& regs, uint32_t target_width, uint32_t target_height,
                           bool ps_writes_depth, bool depth_float24) {
  const uint32_t vte = regs.read(PA_CL_VTE_CNTL);
  const float scale[3] = {bits(vte, 0, 1) ? finite_float(regs, PA_CL_VPORT_XSCALE, 1.0f) : 1.0f,
                          bits(vte, 2, 1) ? finite_float(regs, PA_CL_VPORT_YSCALE, 1.0f) : 1.0f,
                          bits(vte, 4, 1) ? finite_float(regs, PA_CL_VPORT_ZSCALE, 1.0f) : 1.0f};
  const float offset[3] = {bits(vte, 1, 1) ? finite_float(regs, PA_CL_VPORT_XOFFSET, 0.0f) : 0.0f,
                           bits(vte, 3, 1) ? finite_float(regs, PA_CL_VPORT_YOFFSET, 0.0f) : 0.0f,
                           bits(vte, 5, 1) ? finite_float(regs, PA_CL_VPORT_ZOFFSET, 0.0f) : 0.0f};
  // Window offset and the Direct3D 9 pixel centre (integer coordinates; Vulkan's is at .5).
  float add[2] = {0.0f, 0.0f};
  if (bits(regs.read(PA_SU_SC_MODE_CNTL), 16, 1)) {
    const uint32_t off = regs.read(PA_SC_WINDOW_OFFSET);
    add[0] += float(sbits(off, 0, 15));
    add[1] += float(sbits(off, 16, 15));
  }
  if (bits(regs.read(PA_SU_VTX_CNTL), 0, 1) == 0) {
    add[0] += 0.5f;
    add[1] += 0.5f;
  }
  const float max[2] = {float(target_width), float(target_height)};
  const uint32_t clip = regs.read(PA_CL_CLIP_CNTL);
  ViewportInfo v;
  if (bits(clip, 16, 1)) {
    // Clipping disabled: the host viewport covers the whole target and the shader does all of the transform.
    for (int a = 0; a < 2; ++a) {
      const float extent = max[a];
      if (extent <= 0.0f) return ViewportInfo{0, 0, 0, 0, 0, 1, {1, 1, 1, 1}, {0, 0, 0, 0}};
      v.ndc_scale[a] = scale[a] * 2.0f / extent;
      v.ndc_offset[a] = (offset[a] + add[a] - extent * 0.5f) * 2.0f / extent;
    }
    v.width = max[0];
    v.height = max[1];
    v.ndc_scale[2] = scale[2];
    v.ndc_offset[2] = offset[2];
    v.min_depth = 0.0f;
    v.max_depth = 1.0f;
  } else {
    // The viewport snapped to whole pixels inside the target; ndc_scale/offset move the guest clip space onto it.
    float origin[2], extent[2];
    for (int a = 0; a < 2; ++a) {
      const float o = offset[a] + add[a], s = std::fabs(scale[a]);
      const float a0 = std::trunc(std::clamp(o - s, 0.0f, max[a]));
      const float a1 = std::trunc(std::clamp(o + s, 0.0f, max[a]));
      origin[a] = a0;
      extent[a] = a1 - a0;
      if (extent[a] > 0.0f) {
        v.ndc_scale[a] = scale[a] * 2.0f / extent[a];
        v.ndc_offset[a] = (o - (a0 + extent[a] * 0.5f)) * 2.0f / extent[a];
      } else {
        v.ndc_scale[a] = 1.0f;
        v.ndc_offset[a] = 0.0f;
      }
    }
    v.x = origin[0];
    v.y = origin[1];
    v.width = extent[0];
    v.height = extent[1];
    float z_offset = offset[2], z_scale = scale[2];
    if (bits(clip, 19, 1)) {  // dx_clip_space_def: z in [0, w], as is
      v.ndc_scale[2] = 1.0f;
      v.ndc_offset[2] = 0.0f;
    } else {  // z in [-w, w]: halved into [0, w]
      z_offset -= z_scale;
      z_scale *= 2.0f;
      v.ndc_scale[2] = 0.5f;
      v.ndc_offset[2] = 0.5f;
    }
    if (ps_writes_depth) {
      v.min_depth = 0.0f;
      v.max_depth = 1.0f;
    } else {
      v.min_depth = std::clamp(z_offset, 0.0f, 1.0f);
      v.max_depth = std::clamp(z_offset + z_scale, 0.0f, 1.0f);
    }
  }
  if (depth_float24) {  // D24FS8 targets store guest depth [0, 2) as host [0, 1)
    v.min_depth *= 0.5f;
    v.max_depth *= 0.5f;
  }
  v.ndc_scale[3] = 1.0f;
  v.ndc_offset[3] = 0.0f;
  return v;
}

ScissorRect scissor_rect(const RegisterFile& regs, uint32_t target_width, uint32_t target_height) {
  const uint32_t tl = regs.read(PA_SC_WINDOW_SCISSOR_TL), br = regs.read(PA_SC_WINDOW_SCISSOR_BR);
  int32_t x0 = int32_t(bits(tl, 0, 14)), y0 = int32_t(bits(tl, 16, 14));
  int32_t x1 = int32_t(bits(br, 0, 14)), y1 = int32_t(bits(br, 16, 14));
  if (bits(tl, 31, 1) == 0) {  // window_offset_disable clear
    const uint32_t off = regs.read(PA_SC_WINDOW_OFFSET);
    x0 += sbits(off, 0, 15);
    x1 += sbits(off, 0, 15);
    y0 += sbits(off, 16, 15);
    y1 += sbits(off, 16, 15);
  }
  // A screen scissor of TL = BR = (0, 0) is taken as never set (registers at their reset value), not as empty.
  const uint32_t stl = regs.read(PA_SC_SCREEN_SCISSOR_TL), sbr = regs.read(PA_SC_SCREEN_SCISSOR_BR);
  if (stl != 0 || sbr != 0) {
    x0 = std::max(x0, sbits(stl, 0, 15));
    y0 = std::max(y0, sbits(stl, 16, 15));
    x1 = std::min(x1, sbits(sbr, 0, 15));
    y1 = std::min(y1, sbits(sbr, 16, 15));
  }
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::min(x1, int32_t(target_width));
  y1 = std::min(y1, int32_t(target_height));
  ScissorRect r;
  r.x = x0;
  r.y = y0;
  r.width = x1 > x0 ? uint32_t(x1 - x0) : 0;
  r.height = y1 > y0 ? uint32_t(y1 - y0) : 0;
  return r;
}

std::vector<uint32_t> expand_quads(const std::vector<uint32_t>& q) {
  std::vector<uint32_t> out;
  out.reserve(q.size() / 4 * 6);
  for (size_t i = 0; i + 4 <= q.size(); i += 4) {
    const uint32_t a = q[i], b = q[i + 1], c = q[i + 2], d = q[i + 3];
    out.insert(out.end(), {a, b, c, a, c, d});
  }
  return out;
}

Result<GuestIndices> read_indices(const DrawInfo& info, const RegisterFile& regs, const MemoryAccess& mem,
                                  const Primitive& p) {
  GuestIndices out;
  if (!info.indexed) {
    if (p.quads) {
      std::vector<uint32_t> seq(info.index_count);
      for (uint32_t i = 0; i < info.index_count; ++i) seq[i] = i;
      out.indices = expand_quads(seq);
    }
    return out;
  }
  const uint32_t size = info.index_size == 4 ? 4 : 2;
  const uint32_t count = std::min(info.index_count, info.index_words);
  const uint32_t phys = info.index_base_phys & pm4::kPhysicalMask;
  const uint64_t bytes = uint64_t{count} * size;
  auto m = map_guest(mem, phys, bytes, false);
  if (!m) {
    return guest_error("index buffer at physical " + hex(phys) + " (" + std::to_string(count) + " indices of " +
                       std::to_string(size) + " bytes) " + m.error().message);
  }
  // Read as little-endian 32-bit words, swapped per the DMA's endian mode, as the hardware fetches them.
  std::vector<uint32_t> words((bytes + 3) / 4, 0);
  if (bytes > 0) std::memcpy(words.data(), mem.base + m.value().guest, bytes);
  const bool reset_enabled = bits(regs.read(PA_SU_SC_MODE_CNTL), 21, 1) != 0;
  uint32_t reset = regs.read(VGT_MULTI_PRIM_IB_RESET_INDX) & 0xFFFFFFu;
  const bool reset_possible = reset_enabled && (size == 4 || reset <= 0xFFFFu);
  out.indices.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v;
    if (size == 4) {
      v = pm4::gpu_swap(words[i], info.index_endian) & 0xFFFFFFu;
    } else {
      const uint32_t w = pm4::gpu_swap(words[i / 2], info.index_endian);
      v = (i & 1) ? (w >> 16) : (w & 0xFFFFu);
    }
    if (reset_possible && v == reset) {
      v = 0xFFFFFFFFu;
      out.restart = true;
    }
    out.indices[i] = v;
  }
  if (out.restart && p.list) {
    char b[64];
    std::snprintf(b, sizeof b, "0x%X", reset);
    return guest_error(std::string("primitive reset index ") + b + " in a " + p.name + " (index buffer at physical " +
                       hex(phys) + ") cannot be drawn: Vulkan 1.2 has no primitive restart for list topologies");
  }
  if (p.quads) out.indices = expand_quads(out.indices);
  return out;
}

Result<VertexRange> vertex_range(const RegisterFile& regs, uint32_t slot, const MemoryAccess& mem) {
  VertexRange r;
  const uint32_t d0 = regs.read(kShaderConstantFetchBase + 2 * slot);
  const uint32_t d1 = regs.read(kShaderConstantFetchBase + 2 * slot + 1);
  r.endian = bits(d1, 0, 2);
  if (bits(d0, 0, 2) != 3) return r;  // not a vertex fetch constant: reads zero
  const uint32_t phys = (d0 & ~3u) & pm4::kPhysicalMask;
  const uint64_t bytes = uint64_t{bits(d1, 2, 24)} * 4;
  if (bytes == 0) return r;
  auto m = map_guest(mem, phys, bytes, true);
  if (!m) {
    return guest_error("vertex fetch constant " + std::to_string(slot) + ": base physical address " + hex(phys) +
                       " (" + std::to_string(bytes) + " bytes) " + m.error().message);
  }
  r.guest = m.value().guest;
  r.size_bytes = m.value().bytes;
  return r;
}

std::array<uint8_t, 736> shared_block(const RegisterFile& regs, const HeapIndices& heaps, const ViewportInfo& vp) {
  static_assert(sc::kSharedBlockBytes == 736);
  std::array<uint8_t, 736> b{};
  uint8_t* p = b.data();
  for (uint32_t i = 0; i < 32; ++i) {
    put32(p, sc::kSharedTexture2DIndex + 4 * i, heaps.texture_2d[i]);
    put32(p, sc::kSharedTexture3DIndex + 4 * i, heaps.texture_3d[i]);
    put32(p, sc::kSharedTextureCubeIndex + 4 * i, heaps.texture_cube[i]);
    put32(p, sc::kSharedSamplerIndex + 4 * i, heaps.sampler[i]);
  }
  for (uint32_t i = 0; i < kShaderConstantBoolRegCount; ++i)
    put32(p, sc::kSharedBools + 4 * i, regs.read(kShaderConstantBoolBase + i));
  for (uint32_t i = 0; i < kShaderConstantLoopCount; ++i)
    put32(p, sc::kSharedLoops + 4 * i, regs.read(kShaderConstantLoopBase + i));
  for (uint32_t i = 0; i < 4; ++i) {
    put_float(p, sc::kSharedNdcScale + 4 * i, vp.ndc_scale[i]);
    put_float(p, sc::kSharedNdcOffset + 4 * i, vp.ndc_offset[i]);
  }
  put32(p, sc::kSharedAlphaRef, regs.read(RB_ALPHA_REF));
  put32(p, sc::kSharedVteFlags, bits(regs.read(PA_CL_VTE_CNTL), 8, 3));  // vtx_xy_fmt, vtx_z_fmt, vtx_w0_fmt
  put32(p, sc::kSharedIndexOffset, regs.read(VGT_INDX_OFFSET));
  put32(p, sc::kSharedMinIndex, regs.read(VGT_MIN_VTX_INDX) & sc::kVertexIndexMask);
  put32(p, sc::kSharedMaxIndex, regs.read(VGT_MAX_VTX_INDX) & sc::kVertexIndexMask);
  return b;
}

// --- DrawPath -----------------------------------------------------------------------------------------------------

struct DrawPath::Impl {
  const Device& device;
  MemoryAccess mem;
  CommandStream& stream;
  Edram& edram;
  TextureCache& textures;
  SamplerCache& samplers;
  std::unique_ptr<PipelineCache> pipelines;
  uint64_t flush_listener = 0;

  // The upload ring: host-visible, device addresses, reset after every completed flush (the device is idle then).
  VkBuffer ring = VK_NULL_HANDLE;
  VmaAllocation ring_alloc = nullptr;
  uint8_t* ring_map = nullptr;
  VkDeviceAddress ring_address = 0;
  VkDeviceSize ring_capacity = 0;
  VkDeviceSize ring_offset = 0;

  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    std::array<VkImageView, kColorSlots + 1> views{};
  };
  // Key: the render pass and the attachment views (as integers), then the extent.
  std::map<std::array<uint64_t, kColorSlots + 4>, Framebuffer> framebuffers;

  uint64_t bound_generation = 0;  // the stream generation whose command buffer has our descriptor sets bound
  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  DrawStats stats;
  std::set<std::string> warned;

  Impl(const Device& d, MemoryAccess m, CommandStream& s, Edram& e, TextureCache& t, SamplerCache& sm)
      : device(d), mem(std::move(m)), stream(s), edram(e), textures(t), samplers(sm) {}
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() {
    stream.remove_flush_listener(flush_listener);
    edram.set_evict_observer(nullptr);
    stream.drain();
    const VkDevice d = device.device();
    for (auto& [k, fb] : framebuffers) vkDestroyFramebuffer(d, fb.framebuffer, nullptr);
    if (ring) vmaDestroyBuffer(device.allocator(), ring, ring_alloc);
    pipelines.reset();
  }

  void warn_once(const std::string& msg) {
    if (warned.insert(msg).second) std::fprintf(stderr, "warning: [gpu] draw: %s\n", msg.c_str());
  }

  Result<void> init() {
    const std::array<VkDescriptorSetLayout, 4> sets{textures.set_layout(0), textures.set_layout(1),
                                                    textures.set_layout(2), samplers.set_layout()};
    LR_TRY_ASSIGN(p, PipelineCache::create(device, sets));
    pipelines = std::move(p);
    LR_TRY(create_ring(kRingInitialBytes));
    flush_listener = stream.add_flush_listener([this] { ring_offset = 0; });
    edram.set_evict_observer([this](const RenderTarget& rt) { forget(rt.attachment_view); });
    return {};
  }

  // A render target is going away: drop the framebuffers that use it (destroyed once the device is done with them).
  void forget(VkImageView view) {
    const VkDevice d = device.device();
    for (auto it = framebuffers.begin(); it != framebuffers.end();) {
      if (std::find(it->second.views.begin(), it->second.views.end(), view) == it->second.views.end()) {
        ++it;
        continue;
      }
      const VkFramebuffer fb = it->second.framebuffer;
      stream.defer([d, fb] { vkDestroyFramebuffer(d, fb, nullptr); });
      it = framebuffers.erase(it);
    }
  }

  Result<void> create_ring(VkDeviceSize bytes) {
    VkBufferCreateInfo bci = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (VkResult r = vmaCreateBuffer(device.allocator(), &bci, &aci, &ring, &ring_alloc, &info); r != VK_SUCCESS)
      return vk_error("vmaCreateBuffer (upload ring)", r);
    ring_map = static_cast<uint8_t*>(info.pMappedData);
    VkBufferDeviceAddressInfo bdai = make<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
    bdai.buffer = ring;
    ring_address = vkGetBufferDeviceAddress(device.device(), &bdai);
    ring_capacity = bytes;
    ring_offset = 0;
    return {};
  }

  // Room for `bytes` more (alignment included by the caller): flushes the stream (which resets the ring) when the
  // ring is full, and grows it when one draw needs more than it holds.
  Result<void> ring_reserve(VkDeviceSize bytes) {
    if (ring_offset + bytes <= ring_capacity) return {};
    if (stream.recording()) LR_TRY(stream.flush());
    ring_offset = 0;
    if (bytes <= ring_capacity) return {};
    VkDeviceSize cap = ring_capacity;
    while (cap < bytes) cap *= 2;
    if (cap > kRingMaxBytes)
      return guest_error("a draw needs " + std::to_string(bytes) + " bytes of constants, vertices and indices (limit " +
                         std::to_string(kRingMaxBytes) + ")");
    // Nothing recorded uses the old ring (the stream was just flushed).
    vmaDestroyBuffer(device.allocator(), ring, ring_alloc);
    ring = VK_NULL_HANDLE;
    return create_ring(cap);
  }

  VkDeviceSize ring_alloc_bytes(VkDeviceSize bytes, VkDeviceSize align) {
    const VkDeviceSize off = (ring_offset + align - 1) & ~(align - 1);
    ring_offset = off + bytes;
    return off;
  }

  Result<VkFramebuffer> framebuffer(VkRenderPass pass, const std::array<VkImageView, kColorSlots + 1>& views,
                                    VkExtent2D extent) {
    std::array<uint64_t, kColorSlots + 4> key{};
    key[0] = reinterpret_cast<uint64_t>(pass);
    for (uint32_t i = 0; i < views.size(); ++i) key[1 + i] = reinterpret_cast<uint64_t>(views[i]);
    key[kColorSlots + 2] = extent.width;
    key[kColorSlots + 3] = extent.height;
    if (auto it = framebuffers.find(key); it != framebuffers.end()) return it->second.framebuffer;
    std::vector<VkImageView> attachments;
    for (VkImageView v : views)
      if (v) attachments.push_back(v);
    VkFramebufferCreateInfo fbci = make<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    fbci.renderPass = pass;
    fbci.attachmentCount = uint32_t(attachments.size());
    fbci.pAttachments = attachments.data();
    fbci.width = extent.width;
    fbci.height = extent.height;
    fbci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (VkResult r = vkCreateFramebuffer(device.device(), &fbci, nullptr, &fb); r != VK_SUCCESS)
      return vk_error("vkCreateFramebuffer", r);
    framebuffers.emplace(key, Framebuffer{fb, views});
    return fb;
  }

  bool blendable(VkFormat f) const {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(device.physical_device(), f, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
  }

  Result<void> skip() {
    ++stats.skipped;
    return {};
  }

  Result<void> draw(const RegisterFile& regs, const DrawInfo& info, const ActiveShaders& shaders) {
    if (info.prim_type == prim::kNone || info.index_count == 0) return skip();
    LR_TRY_ASSIGN(p, primitive(info.prim_type));
    if (!shaders.vs) {
      warn_once("a draw without a vertex shader was skipped");
      return skip();
    }
    const uint32_t edram_mode = regs.read(RB_MODECONTROL) & 7;
    const sc::TranslatedShader* ps = edram_mode == 4 ? shaders.ps : nullptr;  // 5: depth only, no pixel shader
    if (p.rects && !device.geometry_shader())
      return guest_error("rectangle-list draws need the Vulkan geometryShader feature, which this device lacks");
    if (bits(regs.read(RB_SURFACE_INFO), 0, 14) == 0) {
      warn_once("a draw with no surface (RB_SURFACE_INFO pitch 0) was skipped");
      return skip();
    }

    // Which EDRAM targets the draw writes: colour slot i when the pixel shader writes oC<i> and RB_COLOR_MASK keeps a
    // component of it; depth/stencil when the depth or stencil test is on.
    const uint32_t color_mask = regs.read(RB_COLOR_MASK);
    std::array<bool, kColorSlots> want_color{};
    bool any_color = false;
    for (uint32_t i = 0; i < kColorSlots; ++i) {
      want_color[i] = ps && ((ps->info.colors_written >> i) & 1u) && bits(color_mask, 4 * i, 4) != 0;
      any_color = any_color || want_color[i];
    }
    const uint32_t depthcontrol = regs.read(RB_DEPTHCONTROL);
    const bool z_enable = bits(depthcontrol, 1, 1) != 0, stencil_enable = bits(depthcontrol, 0, 1) != 0;
    const bool want_depth = z_enable || stencil_enable;
    if (!any_color && !want_depth) return skip();

    // Textures and samplers first: the texture cache may flush the stream (uploads, evictions).
    HeapIndices heaps;
    std::bitset<32> tslots = shaders.vs->info.tfetch_slots;
    if (ps) tslots |= ps->info.tfetch_slots;
    for (uint32_t t = 0; t < 32; ++t) {
      if (!tslots.test(t)) continue;
      const TextureFetch fetch = TextureFetch::from_registers(regs, t);
      if (auto r = textures.bind(t, fetch); !r) {
        if (r.error().code == ErrorCode::Cancelled) return r.error();
        return guest_error("texture fetch constant " + std::to_string(t) + ": " + r.error().message);
      }
      const TextureHeapIndices idx = textures.heap_index(t);
      heaps.texture_2d[t] = idx.texture_2d;
      heaps.texture_3d[t] = idx.texture_3d;
      heaps.texture_cube[t] = idx.texture_cube;
      heaps.sampler[t] = samplers.heap_index(fetch, !idx.filter_linear);
    }

    // Render targets. At most five keys are bound here and the EDRAM keeps eight, evicting the least recently used:
    // none of these pointers is evicted by the binds that follow it.
    std::array<RenderTarget*, kColorSlots> color{};
    RenderTarget* depth = nullptr;
    for (uint32_t i = 0; i < kColorSlots; ++i) {
      if (!want_color[i]) continue;
      auto rt = edram.bind(regs, i);
      if (!rt) {
        if (rt.error().code == ErrorCode::Cancelled) return rt.error();
        return guest_error("colour render target " + std::to_string(i) + ": " + rt.error().message);
      }
      color[i] = rt.value();
    }
    if (want_depth) {
      auto rt = edram.bind(regs, edram::kDepthSlot);
      if (!rt) {
        if (rt.error().code == ErrorCode::Cancelled) return rt.error();
        return guest_error("depth render target: " + rt.error().message);
      }
      depth = rt.value();
    }
    RenderPassKey pass_key;
    const RenderTarget* first = depth;
    VkExtent2D extent{UINT32_MAX, UINT32_MAX};
    std::array<VkImageView, kColorSlots + 1> views{};
    for (uint32_t i = 0; i < kColorSlots; ++i) {
      if (!color[i]) continue;
      if (!first || first == depth) first = color[i];
      pass_key.color[i] = color[i]->format;
      views[i] = color[i]->attachment_view;
      extent.width = std::min(extent.width, color[i]->width);
      extent.height = std::min(extent.height, color[i]->height);
    }
    if (depth) {
      pass_key.depth = depth->format;
      views[kColorSlots] = depth->attachment_view;
      extent.width = std::min(extent.width, depth->width);
      extent.height = std::min(extent.height, depth->height);
    }
    pass_key.samples = first->samples;
    for (uint32_t i = 0; i < kColorSlots; ++i)
      if (color[i] && color[i]->samples != first->samples)
        return guest_error("the colour and depth targets of a draw have different host sample counts");
    if (depth && depth->samples != first->samples)
      return guest_error("the colour and depth targets of a draw have different host sample counts");

    const bool float24 = depth && depth->key.format == edram::kDepth_D24FS8;
    const ViewportInfo vp = viewport_info(regs, extent.width, extent.height, ps && ps->info.writes_depth, float24);
    const ScissorRect scissor = scissor_rect(regs, extent.width, extent.height);
    if (vp.width <= 0.0f || vp.height <= 0.0f || scissor.width == 0 || scissor.height == 0) {
      char b[320];
      std::snprintf(b, sizeof b,
                    "a draw with an empty viewport or scissor was skipped (viewport %gx%g at %g,%g; "
                    "PA_SC_WINDOW_SCISSOR_TL/BR 0x%08X/0x%08X, PA_SC_SCREEN_SCISSOR_TL/BR 0x%08X/0x%08X, "
                    "PA_SC_WINDOW_OFFSET 0x%08X, target %ux%u)",
                    double(vp.width), double(vp.height), double(vp.x), double(vp.y),
                    regs.read(PA_SC_WINDOW_SCISSOR_TL), regs.read(PA_SC_WINDOW_SCISSOR_BR),
                    regs.read(PA_SC_SCREEN_SCISSOR_TL), regs.read(PA_SC_SCREEN_SCISSOR_BR),
                    regs.read(PA_SC_WINDOW_OFFSET), extent.width, extent.height);
      warn_once(b);
      return skip();
    }

    LR_TRY_ASSIGN(indices, read_indices(info, regs, mem, p));
    if (info.indexed && info.index_count > info.index_words)
      warn_once("an index count above the index buffer size (VGT_DMA_SIZE) was cut to the buffer");
    const bool use_indices = info.indexed || p.quads;
    const uint32_t vertex_count = use_indices ? uint32_t(indices.indices.size()) : info.index_count;
    if (vertex_count == 0) return skip();

    // The pipeline.
    PipelineKey key = pipeline_key(regs, shaders, ps, p, indices.restart, pass_key, first, depth != nullptr);
    LR_TRY(pipelines->add_shader(shaders.vs_key, *shaders.vs));
    if (ps) LR_TRY(pipelines->add_shader(shaders.ps_key, *ps));
    LR_TRY_ASSIGN(pipeline, pipelines->get(key));
    LR_TRY_ASSIGN(pass, pipelines->render_pass(pass_key));
    LR_TRY_ASSIGN(fb, framebuffer(pass, views, extent));

    // The guest vertex ranges the vertex shader fetches.
    std::vector<std::pair<uint32_t, VertexRange>> ranges;
    VkDeviceSize vertex_bytes = 0;
    for (uint32_t slot = 0; slot < sc::kVertexFetchSlots; ++slot) {
      if (!shaders.vs->info.vfetch_slots.test(slot)) continue;
      const uint32_t d0 = regs.read(kShaderConstantFetchBase + 2 * slot);
      if (bits(d0, 0, 2) != 3)
        warn_once("vertex fetch constant " + std::to_string(slot) + " is not a vertex fetch constant (type " +
                  std::to_string(bits(d0, 0, 2)) + "); its fetches read zero");
      LR_TRY_ASSIGN(range, vertex_range(regs, slot, mem));
      vertex_bytes += range.size_bytes + 16;
      ranges.emplace_back(slot, range);
    }

    // Uploads: constants, shared block, fetch table, vertices, indices.
    const VkDeviceSize index_bytes = use_indices ? VkDeviceSize{indices.indices.size()} * 4 : 0;
    const VkDeviceSize total = 2 * sc::kFloatConstantBytes + sc::kSharedBlockBytes +
                               sc::kVertexFetchSlots * sc::kVertexFetchEntryBytes + vertex_bytes + index_bytes + 128;
    LR_TRY(ring_reserve(total));
    const VkDeviceSize start = ring_offset;
    const VkDeviceSize vs_constants = ring_alloc_bytes(sc::kFloatConstantBytes, 16);
    const VkDeviceSize ps_constants = ring_alloc_bytes(sc::kFloatConstantBytes, 16);
    for (uint32_t i = 0; i < sc::kFloatConstantCount * 4; ++i) {
      put32(ring_map + vs_constants, 4 * i, regs.read(kShaderConstantFloatBase + i));
      put32(ring_map + ps_constants, 4 * i, regs.read(kShaderConstantFloatBase + sc::kFloatConstantCount * 4 + i));
    }
    const VkDeviceSize shared = ring_alloc_bytes(sc::kSharedBlockBytes, 16);
    const std::array<uint8_t, 736> block = shared_block(regs, heaps, vp);
    std::memcpy(ring_map + shared, block.data(), block.size());
    const VkDeviceSize table = ring_alloc_bytes(sc::kVertexFetchSlots * sc::kVertexFetchEntryBytes, 16);
    for (uint32_t slot = 0; slot < sc::kVertexFetchSlots; ++slot) {  // unused slots: a valid address, size 0
      uint8_t* e = ring_map + table + slot * sc::kVertexFetchEntryBytes;
      std::memcpy(e + sc::kVertexFetchEntryAddress, &ring_address, 8);
      put32(e, sc::kVertexFetchEntrySize, 0);
      put32(e, sc::kVertexFetchEntryEndian, 0);
    }
    for (const auto& [slot, range] : ranges) {
      const VkDeviceSize at = ring_alloc_bytes(range.size_bytes, 16);
      if (range.size_bytes) std::memcpy(ring_map + at, mem.base + range.guest, range.size_bytes);
      const VkDeviceAddress address = ring_address + at;
      uint8_t* e = ring_map + table + slot * sc::kVertexFetchEntryBytes;
      std::memcpy(e + sc::kVertexFetchEntryAddress, &address, 8);
      put32(e, sc::kVertexFetchEntrySize, range.size_bytes);
      put32(e, sc::kVertexFetchEntryEndian, range.endian);
    }
    VkDeviceSize index_offset = 0;
    if (use_indices) {
      index_offset = ring_alloc_bytes(index_bytes, 4);
      std::memcpy(ring_map + index_offset, indices.indices.data(), index_bytes);
    }
    vmaFlushAllocation(device.allocator(), ring_alloc, start, ring_offset - start);

    // Record.
    LR_TRY_ASSIGN(cmd, stream.begin_render_pass(pass, fb, extent));
    const VkPipelineLayout layout = pipelines->layout();
    if (bound_generation != stream.generation()) {
      const std::array<VkDescriptorSet, 4> sets{textures.set(0), textures.set(1), textures.set(2), samplers.set()};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, uint32_t(sets.size()), sets.data(), 0,
                              nullptr);
      bound_generation = stream.generation();
      bound_pipeline = VK_NULL_HANDLE;
    }
    if (pipeline != bound_pipeline) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      bound_pipeline = pipeline;
    }
    std::array<VkDeviceAddress, 4> push{ring_address + vs_constants, ring_address + ps_constants,
                                        ring_address + shared, ring_address + table};
    static_assert(sizeof(push) == sc::kPushConstantBytes);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sc::kPushConstantBytes, push.data());
    const VkViewport viewport{vp.x, vp.y, vp.width, vp.height, vp.min_depth, vp.max_depth};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D rect{{scissor.x, scissor.y}, {scissor.width, scissor.height}};
    vkCmdSetScissor(cmd, 0, 1, &rect);
    set_dynamic_state(cmd, regs, p, depth);
    if (use_indices) {
      vkCmdBindIndexBuffer(cmd, ring, index_offset, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(cmd, vertex_count, 1, 0, 0, 0);
    } else {
      vkCmdDraw(cmd, vertex_count, 1, 0, 0);
    }
    ++stats.draws;
    stats.pipelines = pipelines->pipeline_count();
    stats.last_fragment_stage = pipelines->last_has_fragment_stage();
    return {};
  }

  PipelineKey pipeline_key(const RegisterFile& regs, const ActiveShaders& shaders, const sc::TranslatedShader* ps,
                           const Primitive& p, bool restart, const RenderPassKey& pass_key, const RenderTarget* first,
                           bool has_depth) {
    PipelineKey key;
    key.vs = shaders.vs_key;
    key.has_ps = ps != nullptr;
    if (ps) key.ps = shaders.ps_key;
    // Alpha test (specialization constant 0): only when the pixel shader writes oC0; "always" needs no test.
    if (ps && (ps->info.colors_written & 1u)) {
      const uint32_t cc = regs.read(RB_COLORCONTROL);
      const uint32_t func = bits(cc, 0, 3);
      if (bits(cc, 3, 1) && func != 7) key.alpha_test = sc::kAlphaTestEnable | (func << sc::kAlphaTestFuncShift);
    }
    key.pass = pass_key;
    // Every guest sample, in the host's sample order (2x on a 4x host image covers both halves).
    key.sample_mask = first->host_sample_mask(0xF);
    key.topology = p.topology;
    key.primitive_restart = restart;
    key.rect_list = p.rects;

    const uint32_t mode = regs.read(PA_SU_SC_MODE_CNTL);
    const bool cull_front = bits(mode, 0, 1) != 0, cull_back = bits(mode, 1, 1) != 0;
    key.cull_mode = VK_CULL_MODE_NONE;
    key.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    key.polygon_mode = VK_POLYGON_MODE_FILL;
    if (p.polygonal) {
      key.cull_mode =
          (cull_front ? uint32_t(VK_CULL_MODE_FRONT_BIT) : 0u) | (cull_back ? uint32_t(VK_CULL_MODE_BACK_BIT) : 0u);
      key.front_face = bits(mode, 2, 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      if (bits(mode, 3, 2) == 1) {  // dual mode: the most special type among the faces drawn (Xenia's choice)
        uint32_t type = 2;          // 0 points, 1 lines, 2 triangles
        if (!cull_front) type = std::min(type, bits(mode, 5, 3));
        if (!cull_back) type = std::min(type, bits(mode, 8, 3));
        if (cull_front && cull_back) type = 2;
        if (type < 2 && !device.fill_mode_non_solid()) {
          warn_once("point/line polygon modes need fillModeNonSolid; drawn filled");
          type = 2;
        }
        key.polygon_mode = type == 0 ? VK_POLYGON_MODE_POINT : type == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
      }
    }
    key.depth_bias = has_depth && polygon_offset(regs, p).enabled;
    if (bits(mode, 19, 1)) {
      if (device.provoking_vertex_last()) {
        key.provoking_last = true;
      } else if (ps && sc::key_options(shaders.ps_key).flat_mask != 0) {
        warn_once("flat interpolants take the first vertex: the device lacks VK_EXT_provoking_vertex "
                  "(PA_SU_SC_MODE_CNTL.provoking_vtx_last)");
      }
    }

    if (has_depth) {
      const uint32_t dc = regs.read(RB_DEPTHCONTROL);
      if (bits(dc, 1, 1)) {
        key.depth_test = true;
        key.depth_write = bits(dc, 2, 1) != 0;
        key.depth_compare = compare_op(bits(dc, 4, 3));
      } else {
        key.depth_compare = VK_COMPARE_OP_ALWAYS;
      }
      if (bits(dc, 0, 1)) {
        key.stencil_test = true;
        key.stencil_front = {stencil_op(bits(dc, 11, 3)), stencil_op(bits(dc, 14, 3)), stencil_op(bits(dc, 17, 3)),
                             compare_op(bits(dc, 8, 3))};
        key.stencil_back = key.stencil_front;
        if (p.polygonal && bits(dc, 7, 1))  // backface_enable
          key.stencil_back = {stencil_op(bits(dc, 23, 3)), stencil_op(bits(dc, 26, 3)), stencil_op(bits(dc, 29, 3)),
                              compare_op(bits(dc, 20, 3))};
      }
    }

    const uint32_t color_mask = regs.read(RB_COLOR_MASK);
    int first_color = -1;
    for (uint32_t i = 0; i < kColorSlots; ++i) {
      if (pass_key.color[i] == VK_FORMAT_UNDEFINED) continue;
      const uint32_t bc = regs.read(kBlendControl[i]);
      BlendKey b;
      b.write_mask = bits(color_mask, 4 * i, 4);  // R, G, B, A in bits 0-3, like VkColorComponentFlags
      const uint32_t src_c = bits(bc, 0, 5), op_c = bits(bc, 5, 3), dst_c = bits(bc, 8, 5);
      const uint32_t src_a = bits(bc, 16, 5), op_a = bits(bc, 21, 3), dst_a = bits(bc, 24, 5);
      const bool identity = src_c == 1 && dst_c == 0 && op_c == 0 && src_a == 1 && dst_a == 0 && op_a == 0;
      if (!identity && blendable(pass_key.color[i])) {
        b.enable = 1;
        b.src_color = blend_factor(src_c);
        b.dst_color = blend_factor(dst_c);
        b.color_op = blend_op(op_c);
        b.src_alpha = blend_factor(src_a);
        b.dst_alpha = blend_factor(dst_a);
        b.alpha_op = blend_op(op_a);
      } else if (!identity) {
        warn_once("blending is off for VkFormat " + std::to_string(int(pass_key.color[i])) +
                  " (no COLOR_ATTACHMENT_BLEND support)");
      }
      key.blend[i] = b;
      if (first_color < 0) first_color = int(i);
    }
    if (first_color >= 0 && !device.independent_blend()) {
      // Every attachment state must be equal without independentBlend: the first bound target's for all.
      for (uint32_t i = 0; i < kColorSlots; ++i) {
        if (i == uint32_t(first_color)) continue;
        if (pass_key.color[i] != VK_FORMAT_UNDEFINED && key.blend[i] != key.blend[first_color])
          warn_once("per-target blend state needs independentBlend; every target uses the first one's");
        key.blend[i] = key.blend[first_color];
      }
    }
    return key;
  }

  struct PolygonOffset {
    bool enabled = false;
    float scale = 0.0f, offset = 0.0f;
  };
  // The face whose polygon offset applies (one depth bias per draw; Xenia's preferred-face rule): the front values
  // when the front offset is enabled, the front is not culled and its scale or offset is nonzero; else the back
  // values under the same conditions; lines and points use the front values with poly_offset_para_enable.
  PolygonOffset polygon_offset(const RegisterFile& regs, const Primitive& p) const {
    const uint32_t mode = regs.read(PA_SU_SC_MODE_CNTL);
    PolygonOffset o;
    bool back = false;
    if (p.polygonal) {
      auto nonzero = [&](uint32_t scale_reg, uint32_t offset_reg) {
        return finite_float(regs, scale_reg, 0.0f) != 0.0f || finite_float(regs, offset_reg, 0.0f) != 0.0f;
      };
      const bool front_ok = bits(mode, 11, 1) && !bits(mode, 0, 1) &&
                            nonzero(PA_SU_POLY_OFFSET_FRONT_SCALE, PA_SU_POLY_OFFSET_FRONT_OFFSET);
      const bool back_ok = bits(mode, 12, 1) && !bits(mode, 1, 1) &&
                           nonzero(PA_SU_POLY_OFFSET_BACK_SCALE, PA_SU_POLY_OFFSET_BACK_OFFSET);
      if (front_ok) {
        o.enabled = true;
      } else if (back_ok) {
        o.enabled = true;
        back = true;
      }
    } else {
      o.enabled = bits(mode, 13, 1) != 0;
    }
    if (!o.enabled) return o;
    o.scale = finite_float(regs, back ? PA_SU_POLY_OFFSET_BACK_SCALE : PA_SU_POLY_OFFSET_FRONT_SCALE, 0.0f);
    o.offset = finite_float(regs, back ? PA_SU_POLY_OFFSET_BACK_OFFSET : PA_SU_POLY_OFFSET_FRONT_OFFSET, 0.0f);
    return o;
  }

  void set_dynamic_state(VkCommandBuffer cmd, const RegisterFile& regs, const Primitive& p,
                         const RenderTarget* depth) {
    // Depth bias: the guest offset is in depth units (x 2^24 for 24-bit depth, Xenia's D3D10 factors), the slope
    // scale in 1/16 subpixel units.
    const PolygonOffset o = polygon_offset(regs, p);
    float constant = 0.0f, slope = 0.0f;
    if (o.enabled && depth) {
      constant = o.offset * (depth->key.format == edram::kDepth_D24FS8 ? 16777216.0f : 16777215.0f);
      slope = o.scale * (1.0f / 16.0f);
    }
    vkCmdSetDepthBias(cmd, constant, 0.0f, slope);
    const float blend[4] = {finite_float(regs, RB_BLEND_RED, 0.0f), finite_float(regs, RB_BLEND_GREEN, 0.0f),
                            finite_float(regs, RB_BLEND_BLUE, 0.0f), finite_float(regs, RB_BLEND_ALPHA, 0.0f)};
    vkCmdSetBlendConstants(cmd, blend);
    const uint32_t front = regs.read(RB_STENCILREFMASK);
    const bool separate = p.polygonal && bits(regs.read(RB_DEPTHCONTROL), 7, 1);
    const uint32_t back = separate ? regs.read(RB_STENCILREFMASK_BF) : front;
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, bits(front, 0, 8));
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, bits(front, 8, 8));
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, bits(front, 16, 8));
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, bits(back, 0, 8));
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, bits(back, 8, 8));
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, bits(back, 16, 8));
  }
};

Result<std::unique_ptr<DrawPath>> DrawPath::create(const Device& device, MemoryAccess mem, CommandStream& stream,
                                                   Edram& edram, TextureCache& textures, SamplerCache& samplers) {
  auto impl = std::make_unique<Impl>(device, std::move(mem), stream, edram, textures, samplers);
  LR_TRY(impl->init());
  return std::unique_ptr<DrawPath>(new DrawPath(std::move(impl)));
}

DrawPath::DrawPath(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DrawPath::~DrawPath() = default;

Result<void> DrawPath::draw(const RegisterFile& regs, const DrawInfo& info, const ActiveShaders& shaders) {
  return impl_->draw(regs, info, shaders);
}

const DrawStats& DrawPath::stats() const { return impl_->stats; }

}  // namespace lr::gpu::vk
