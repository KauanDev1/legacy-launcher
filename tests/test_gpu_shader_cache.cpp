// SPDX-License-Identifier: MIT
// The microcode hash and the shader loads GpuSystem hashes, records and traces (runtime/gpu/shader_cache.h,
// gpu_system.h). The microcode is built by hand with the test builder (tests/support/xenos_shader_builder.h).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "runtime/be.h"
#include "runtime/gpu/gpu_system.h"
#include "runtime/gpu/mmio.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/shader_cache.h"
#include "runtime/kernel/kernel.h"
#include "runtime/memory/physical_heap.h"
#include "support/kernel_fixture.h"
#include "support/pm4_builder.h"
#include "support/temp_dir.h"
#include "support/xenos_shader_builder.h"

using lr::gpu::GpuSystem;
using lr::gpu::ShaderStage;
using lr::runtime::be;
using lr::testing::KernelFixture;
using lr::testing::Pm4Builder;
using namespace std::chrono_literals;

namespace {

namespace pm4 = lr::gpu::pm4;
namespace x = lr::testing::xenos;

std::vector<uint8_t> words_le(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> out(words.size() * 4);
  std::memcpy(out.data(), words.data(), out.size());
  return out;
}

std::string hex64(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%016llX", static_cast<unsigned long long>(v));
  return b;
}

// The microcode of a synthetic vertex shader (Task 13 builder), as dwords.
std::vector<uint32_t> vertex_shader() {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, 1, 0), x::cf_nop()));
  x::Alu pos;
  pos.export_data = true;
  pos.vector_dest = x::kExportVsPosition;
  pos.src1 = {0, false};
  pos.src2 = {0, false};
  x::emit(code, x::alu(pos));
  return code;
}

uint64_t hash_of(const std::vector<uint32_t>& microcode) {
  const std::vector<uint8_t> bytes = x::microcode_bytes(microcode);
  return lr::gpu::microcode_hash(bytes.data(), uint32_t(microcode.size()));
}

bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds limit = 10s) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return pred();
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

struct Phys {
  uint32_t guest, phys;
};
Phys phys_alloc(uint32_t bytes) {
  auto r = lr::kernel::kernel().physical().allocate(bytes, 0x20000004 /* MEM_LARGE_PAGES | PAGE_READWRITE */, 0,
                                                    0xFFFFFFFFu, 0);
  REQUIRE(r);
  return {r.value(), lr::runtime::PhysicalHeap::physical_address(r.value())};
}

// A NullBackend-like backend that may claim to bind shaders.
struct CountingBackend final : lr::gpu::Backend {
  explicit CountingBackend(bool binds) : binds_(binds) {}
  std::atomic<uint32_t> loads{0};
  void load_shader(ShaderStage, uint32_t, uint32_t) override { ++loads; }
  void draw(const lr::gpu::RegisterFile&, const lr::gpu::DrawInfo&) override {}
  void resolve(const lr::gpu::RegisterFile&) override {}
  void swap(uint32_t, uint32_t, uint32_t) override {}
  void wait_idle() override {}
  bool binds_shaders() const override { return binds_; }

 private:
  bool binds_;
};

// A GpuSystem over the fixture's memory with a ring, recording fatals (they return, so the GPU thread stops).
struct Gpu {
  KernelFixture& f;
  std::mutex m;
  std::vector<std::string> fatals;
  std::unique_ptr<GpuSystem> gpu;
  Phys ring = phys_alloc(0x10000);
  uint32_t wptr = 0;

  explicit Gpu(KernelFixture& fx) : f(fx) {
    lr::gpu::GpuHooks hooks;
    hooks.alloc_guest = [this](uint32_t bytes) { return f.alloc(bytes); };
    hooks.fatal = [this](const std::string& what) {
      std::lock_guard<std::mutex> lock(m);
      fatals.push_back(what);
    };
    gpu = std::make_unique<GpuSystem>(lr::kernel::kernel().gpu_memory_access(), std::move(hooks));
    gpu->initialize_ring_buffer(ring.phys, 9);  // 1024 dwords
  }
  ~Gpu() {
    gpu->stop();
    *f.mem.translate<be<uint32_t>>(lr::gpu::mmio_register_address(lr::gpu::CP_RB_WPTR)) = 0;
  }
  std::string fatal() {
    std::lock_guard<std::mutex> lock(m);
    return fatals.empty() ? std::string() : fatals.front();
  }
  void submit(const Pm4Builder& b) {
    for (uint32_t d : b.dwords()) *f.mem.translate<be<uint32_t>>(ring.guest + 4 * wptr++) = d;
    *f.mem.translate<be<uint32_t>>(lr::gpu::mmio_register_address(lr::gpu::CP_RB_WPTR)) = wptr;
  }
  bool drained() { return gpu->read_pointer() == wptr; }
};

Pm4Builder& im_load_immediate(Pm4Builder& b, ShaderStage stage, const std::vector<uint32_t>& microcode) {
  std::vector<uint32_t> payload = {stage == ShaderStage::Vertex ? 0u : 1u, uint32_t(microcode.size())};
  payload.insert(payload.end(), microcode.begin(), microcode.end());
  return b.type3(pm4::IM_LOAD_IMMEDIATE, payload);
}

}  // namespace

TEST_CASE("microcode hash is XXH3-64 of the big-endian bytes", "[unit][gpu]") {
  // XXH3-64 reference values (seed 0): the empty input, and the hash depends on byte order.
  CHECK(lr::gpu::microcode_hash(nullptr, 0) == 0x2D06800538D394C2ull);
  const std::vector<uint32_t> code = vertex_shader();
  std::vector<uint8_t> le = words_le(code);
  CHECK(hash_of(code) != lr::gpu::microcode_hash(le.data(), uint32_t(code.size())));
}

TEST_CASE("shader loads are traced by hash", "[unit][gpu]") {
  KernelFixture f;
  lr::testing::TempDir td("lr_shader_trace");
  const std::vector<uint32_t> vs = vertex_shader();
  const std::vector<uint32_t> ps = {0xC0FFEE01, 0x00000002, 0x00000003};
  const uint64_t vs_hash = hash_of(vs), ps_hash = hash_of(ps);

  {
    const bool binds = GENERATE(false, true);  // no shader is refused at load time: the backend translates it
    Gpu g(f);
    auto backend = std::make_unique<CountingBackend>(binds);
    CountingBackend* counting = backend.get();
    g.gpu->set_backend(std::move(backend));
    const std::filesystem::path trace = td.path / (binds ? "binds.log" : "null.log");
    REQUIRE(g.gpu->enable_trace(trace));
    // The first shader through IM_LOAD from physical memory (the size covers the microcode only), the second one
    // twice through IM_LOAD_IMMEDIATE (one distinct sighting).
    const Phys code = phys_alloc(0x10000);
    for (size_t i = 0; i < vs.size(); ++i) *f.mem.translate<be<uint32_t>>(code.guest + 4 * uint32_t(i)) = vs[i];
    Pm4Builder b;
    b.type3(pm4::IM_LOAD, {code.phys | 0 /* vertex */, uint32_t(vs.size())});
    im_load_immediate(b, ShaderStage::Pixel, ps);
    im_load_immediate(b, ShaderStage::Pixel, ps);
    g.submit(b);
    g.gpu->start();
    REQUIRE(wait_until([&] { return g.drained(); }));
    g.gpu->stop();
    CHECK(g.fatal().empty());
    CHECK(counting->loads == 3);  // every load reaches the backend

    const lr::gpu::ShaderLoadStats stats = g.gpu->shader_stats();
    CHECK(stats.loads == 3);
    REQUIRE(stats.distinct.size() == 2);
    CHECK(stats.distinct[0].hash == vs_hash);
    CHECK(stats.distinct[0].stage == ShaderStage::Vertex);
    CHECK(stats.distinct[0].dwords == vs.size());
    CHECK(stats.distinct[1].hash == ps_hash);
    CHECK(stats.distinct[1].stage == ShaderStage::Pixel);
    CHECK(stats.distinct[1].dwords == 3);

    std::ifstream in(trace);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("shader vertex " + hex64(vs_hash) + " dwords=" + std::to_string(vs.size()) + "\n") !=
          std::string::npos);
    const std::string pixel = "shader pixel " + hex64(ps_hash) + " dwords=3\n";
    REQUIRE(text.find(pixel) != std::string::npos);
    CHECK(text.find(pixel, text.find(pixel) + 1) == std::string::npos);  // once per distinct shader
    CHECK(text.find("missing") == std::string::npos);
  }
}
