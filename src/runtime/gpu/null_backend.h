// SPDX-License-Identifier: MIT
// A Backend that renders nothing and counts calls: tests and headless runs.
#pragma once

#include <cstdint>
#include <optional>

#include "runtime/gpu/backend.h"
#include "runtime/gpu/shader/shader_key.h"

namespace lr::gpu {

class NullBackend final : public Backend {
 public:
  struct ShaderLoad {
    ShaderStage stage;
    uint32_t guest_addr;
    uint32_t dwords;
  };
  struct Swap {
    uint32_t frontbuffer_phys, width, height;
  };

  void load_shader(ShaderStage stage, uint32_t guest_addr, uint32_t dwords) override;
  void draw(const RegisterFile& regs, const DrawInfo& info, const ActiveShaders& shaders) override;
  void resolve(const RegisterFile& regs) override;
  void swap(const RegisterFile& regs, uint32_t frontbuffer_phys, uint32_t width, uint32_t height) override;
  void wait_idle() override;

  uint64_t load_shader_calls = 0;
  uint64_t draw_calls = 0;
  uint64_t resolve_calls = 0;
  uint64_t swap_calls = 0;
  uint64_t wait_idle_calls = 0;
  std::optional<ShaderLoad> last_shader;
  std::optional<DrawInfo> last_draw;
  std::optional<ActiveShaders> last_shaders;  // of the last draw
  std::optional<Swap> last_swap;
};

}  // namespace lr::gpu
