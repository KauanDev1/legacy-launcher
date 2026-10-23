// SPDX-License-Identifier: MIT
#include "runtime/gpu/null_backend.h"

namespace lr::gpu {

void NullBackend::load_shader(ShaderStage stage, uint32_t guest_addr, uint32_t dwords) {
  ++load_shader_calls;
  last_shader = ShaderLoad{stage, guest_addr, dwords};
}

void NullBackend::draw(const RegisterFile& /*regs*/, const DrawInfo& info, const ActiveShaders& shaders) {
  ++draw_calls;
  last_draw = info;
  last_shaders = shaders;
}

void NullBackend::resolve(const RegisterFile& /*regs*/) { ++resolve_calls; }

void NullBackend::swap(const RegisterFile& /*regs*/, uint32_t frontbuffer_phys, uint32_t width, uint32_t height) {
  ++swap_calls;
  last_swap = Swap{frontbuffer_phys, width, height};
}

void NullBackend::wait_idle() { ++wait_idle_calls; }

}  // namespace lr::gpu
