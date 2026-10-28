// SPDX-License-Identifier: MIT
// The interface the PM4 command processor drives. The Vulkan renderer implements it in a later task; NullBackend
// (null_backend.h) only counts calls. Pure GPU core: no Vulkan types.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "runtime/gpu/registers.h"

namespace lr::gpu {

enum class ShaderStage { Vertex, Pixel };

struct ActiveShaders;  // runtime/gpu/shader/shader_key.h

// One draw as the PM4 DRAW_INDX / DRAW_INDX_2 packet describes it. The index_* buffer fields are 0 unless
// `indexed`.
struct DrawInfo {
  uint32_t prim_type;           // VGT_DRAW_INITIATOR.prim_type (bits 0-5)
  uint32_t index_count;         // VGT_DRAW_INITIATOR.num_indices (bits 16-31): vertices/indices the draw uses
  uint32_t index_base_phys;     // VGT_DMA_BASE: physical address of the index buffer, aligned down to index_size
  uint32_t index_size;          // bytes per index: 4 if VGT_DRAW_INITIATOR.index_size (bit 11) is set, else 2
  bool indexed;                 // index buffer fetched by DMA (source_select 0); false for auto-index draws
  uint32_t vgt_draw_initiator;  // the raw initiator dword
  uint32_t index_endian = 0;    // VGT_DMA_SIZE.swap_mode (bits 30-31): pm4::kEndian* byte order of the indices
  uint32_t index_words = 0;     // VGT_DMA_SIZE.num_words (bits 0-23): index buffer length in indices (DMA size)
};

// All calls come from the GPU thread, in packet order.
class Backend {
 public:
  virtual ~Backend();
  // IM_LOAD / IM_LOAD_IMMEDIATE: `dwords` microcode dwords (big-endian) at guest address `guest_addr`. For
  // IM_LOAD_IMMEDIATE the address points into the command buffer itself, valid only during the call. GpuSystem has
  // already copied and decoded the microcode (runtime/gpu/shader_cache.h); draws get the translation.
  virtual void load_shader(ShaderStage stage, uint32_t guest_addr, uint32_t dwords) = 0;
  // `shaders`: the translated vertex and pixel shaders of the draw (GpuSystem fills them; the command processor alone
  // passes an empty ActiveShaders).
  virtual void draw(const RegisterFile& regs, const DrawInfo& info, const ActiveShaders& shaders) = 0;
  // A draw issued while RB_MODECONTROL.edram_mode is "copy": the RB_COPY_* registers describe an EDRAM resolve.
  virtual void resolve(const RegisterFile& regs) = 0;
  // LR_SWAP (VdSwap): present the front buffer. `regs` holds the swap's texture fetch constant 0 (format, tiling,
  // endian and pitch of the front buffer; VdSwap wrote it just before, base address = frontbuffer_phys).
  virtual void swap(const RegisterFile& regs, uint32_t frontbuffer_phys, uint32_t width, uint32_t height) = 0;
  virtual void wait_idle() = 0;
  // Host textures created so far, for the --trace-gpu per-second counters. Backends without textures report 0.
  virtual uint64_t textures_created() const { return 0; }
  // True for a backend that binds the translated shaders (the Vulkan renderer). Microcode the runtime translator
  // cannot handle is fatal (GpuHooks::fatal, exit 5) only with such a backend; with one that renders nothing
  // (NullBackend) GpuSystem only reports it (stderr, --trace-gpu, the exit summary).
  virtual bool binds_shaders() const { return false; }
  // GpuSystem installs its fatal path here before the GPU thread starts (GpuHooks::fatal: guest_fatal, exit 5). A
  // backend calls it, once, for a guest error it cannot draw past (an index or vertex buffer outside guest memory,
  // an unsupported primitive type, ...), naming it, and then does no more work. Default: ignored.
  virtual void set_fatal_handler(std::function<void(const std::string& what)> fatal) { (void)fatal; }
};

}  // namespace lr::gpu
