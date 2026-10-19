// SPDX-License-Identifier: MIT
// The interface the PM4 command processor drives. The Vulkan renderer implements it in a later task; NullBackend
// (null_backend.h) only counts calls. Pure GPU core: no Vulkan types.
#pragma once

#include <cstdint>

#include "runtime/gpu/registers.h"

namespace lr::gpu {

enum class ShaderStage { Vertex, Pixel };

struct DrawInfo {
  uint32_t prim_type;         // VGT_DRAW_INITIATOR.prim_type (bits 0-5)
  uint32_t index_count;       // VGT_DRAW_INITIATOR.num_indices (bits 16-31)
  uint32_t index_base_phys;   // physical address of the index buffer, aligned to index_size; 0 when !indexed
  uint32_t index_size;        // bytes per index (2 or 4); 0 when !indexed
  bool indexed;               // index buffer fetched by DMA (source_select 0); false for auto-index draws
  uint32_t vgt_draw_initiator;  // the raw initiator dword
};

// All calls come from the GPU thread, in packet order.
class Backend {
 public:
  virtual ~Backend();
  // IM_LOAD / IM_LOAD_IMMEDIATE: `dwords` microcode dwords (big-endian) at guest address `guest_addr`. For
  // IM_LOAD_IMMEDIATE the address points into the command buffer itself, valid only during the call.
  virtual void load_shader(ShaderStage stage, uint32_t guest_addr, uint32_t dwords) = 0;
  virtual void draw(const RegisterFile& regs, const DrawInfo& info) = 0;
  // A draw issued while RB_MODECONTROL.edram_mode is "copy": the RB_COPY_* registers describe an EDRAM resolve.
  virtual void resolve(const RegisterFile& regs) = 0;
  virtual void swap(uint32_t frontbuffer_phys, uint32_t width, uint32_t height) = 0;
  virtual void wait_idle() = 0;
};

}  // namespace lr::gpu
