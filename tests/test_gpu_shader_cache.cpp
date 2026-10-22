// SPDX-License-Identifier: MIT
// The runtime shader cache (runtime/gpu/shader_cache.h) and the shader loads GpuSystem hashes against it. The
// cross-check with the hashes the XenosRecomp fork emits lives in tests/shaders ("hash of loaded microcode matches
// the fork's hash"), which has a generated cache; these tests build their tables by hand.
#include <catch2/catch_test_macros.hpp>

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
using lr::gpu::ShaderCache;
using lr::gpu::ShaderStage;
using lr::runtime::be;
using lr::testing::KernelFixture;
using lr::testing::Pm4Builder;
using namespace std::chrono_literals;

namespace {

namespace pm4 = lr::gpu::pm4;
namespace x = lr::testing::xenos;

// A one-frame zstd stream holding `data` (< 256 bytes) in one raw block: single segment, 1-byte content size, no
// checksum (RFC 8878 section 3.1.1).
std::vector<uint8_t> zstd_raw_frame(const std::vector<uint8_t>& data) {
  REQUIRE(data.size() < 256);
  std::vector<uint8_t> out = {0x28, 0xB5, 0x2F, 0xFD, 0x20, uint8_t(data.size())};
  const uint32_t block = 1u | (uint32_t(data.size()) << 3);  // last block, raw
  out.push_back(uint8_t(block));
  out.push_back(uint8_t(block >> 8));
  out.push_back(uint8_t(block >> 16));
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

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

// The microcode of a synthetic vertex shader (Task 13 builder), as big-endian bytes and as dwords.
x::ShaderDesc vertex_shader() {
  x::ShaderDesc d;
  d.vertex = true;
  x::emit(d.microcode, x::cf_pair(x::cf_exec(true, 1, 1, 0), x::cf_nop()));
  x::Alu pos;
  pos.export_data = true;
  pos.vector_dest = x::kExportVsPosition;
  pos.src1 = {0, false};
  pos.src2 = {0, false};
  x::emit(d.microcode, x::alu(pos));
  return d;
}

uint64_t hash_of(const std::vector<uint32_t>& microcode) {
  x::ShaderDesc d;
  d.microcode = microcode;
  const std::vector<uint8_t> bytes = x::microcode_bytes(d);
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

TEST_CASE("shader cache registers the generated tables and returns SPIR-V", "[unit][gpu]") {
  const std::vector<uint32_t> a = {0x07230203, 1, 2, 3, 4, 5}, b = {0x07230203, 9, 8, 7, 6, 5, 4};
  std::vector<uint32_t> all = a;
  all.insert(all.end(), b.begin(), b.end());
  const std::vector<uint8_t> blob = zstd_raw_frame(words_le(all));
  // Out of hash order on purpose: the cache sorts.
  const lr_shader_cache_entry entries[] = {{0x20, 1, uint32_t(a.size() * 4), uint32_t(b.size() * 4), 0x1E},
                                           {0x10, 0, 0, uint32_t(a.size() * 4), 0}};
  ShaderCache cache;
  REQUIRE(cache.register_entries(entries, blob, all.size() * 4));
  CHECK(cache.size() == 2);
  const lr::gpu::ShaderEntry* e = cache.find(0x20);
  REQUIRE(e != nullptr);
  CHECK(e->stage == ShaderStage::Pixel);
  CHECK(e->spec_mask == 0x1E);
  auto words = cache.spirv(*e);
  REQUIRE(words);
  CHECK(words.value() == b);
  REQUIRE(cache.find(0x10) != nullptr);
  CHECK(cache.find(0x10)->stage == ShaderStage::Vertex);
  CHECK(cache.spirv(*cache.find(0x10)).value() == a);

  SECTION("a corrupt table is refused and leaves the cache as it was") {
    const lr_shader_cache_entry bad_stage[] = {{0x30, 2, 0, 4, 0}};
    const lr_shader_cache_entry outside[] = {{0x30, 0, 0, uint32_t(all.size() * 4 + 4), 0}};
    const lr_shader_cache_entry unaligned[] = {{0x30, 0, 2, 4, 0}};
    const lr_shader_cache_entry twice[] = {{0x30, 0, 0, 4, 0}, {0x30, 1, 4, 4, 0}};
    for (auto table : {std::span<const lr_shader_cache_entry>(bad_stage), std::span<const lr_shader_cache_entry>(outside),
                       std::span<const lr_shader_cache_entry>(unaligned), std::span<const lr_shader_cache_entry>(twice)}) {
      auto r = cache.register_entries(table, blob, all.size() * 4);
      REQUIRE_FALSE(r);
      CHECK(r.error().code == lr::ErrorCode::InvalidFormat);
      CHECK(r.error().message.find("0x0000000000000030") != std::string::npos);
    }
    std::vector<uint8_t> corrupt = blob;
    corrupt[0] = 0;  // bad magic
    CHECK_FALSE(cache.register_entries(entries, corrupt, all.size() * 4));
    CHECK_FALSE(cache.register_entries(entries, blob, all.size() * 4 + 4));  // size mismatch
    CHECK(cache.size() == 2);
    CHECK(cache.find(0x20) != nullptr);
  }
  SECTION("an empty cache (no shaders) registers") {
    CHECK(cache.register_entries({}, {}, 0));
    CHECK(cache.size() == 0);
    CHECK(cache.find(0x20) == nullptr);
  }
}

TEST_CASE("microcode hash is XXH3-64 of the big-endian bytes", "[unit][gpu]") {
  // XXH3-64 reference values (seed 0): the empty input, and the hash depends on byte order.
  CHECK(lr::gpu::microcode_hash(nullptr, 0) == 0x2D06800538D394C2ull);
  const std::vector<uint32_t> code = vertex_shader().microcode;
  std::vector<uint8_t> le = words_le(code);
  CHECK(hash_of(code) != lr::gpu::microcode_hash(le.data(), uint32_t(code.size())));
}

TEST_CASE("unknown hash is reported", "[unit][gpu]") {
  KernelFixture f;
  lr::testing::TempDir td("lr_shader_trace");
  const std::vector<uint32_t> known = vertex_shader().microcode;
  const std::vector<uint32_t> unknown = {0xC0FFEE01, 0x00000002, 0x00000003};
  const uint64_t known_hash = hash_of(known), unknown_hash = hash_of(unknown);
  const std::vector<uint32_t> spirv = {0x07230203, 0, 0, 0, 0};
  const std::vector<uint8_t> blob = zstd_raw_frame(words_le(spirv));
  const lr_shader_cache_entry entries[] = {{known_hash, 0, 0, uint32_t(spirv.size() * 4), 0}};

  SECTION("a backend that renders nothing only reports it (the shader gate)") {
    Gpu g(f);
    REQUIRE(g.gpu->shader_cache().register_entries(entries, blob, spirv.size() * 4));
    auto backend = std::make_unique<CountingBackend>(false);
    CountingBackend* counting = backend.get();
    g.gpu->set_backend(std::move(backend));
    const std::filesystem::path trace = td.path / "gpu-trace.log";
    REQUIRE(g.gpu->enable_trace(trace));
    // The known shader through IM_LOAD from physical memory (the size covers the microcode only, no container
    // header), the unknown one twice through IM_LOAD_IMMEDIATE (one distinct sighting).
    const Phys code = phys_alloc(0x10000);
    for (size_t i = 0; i < known.size(); ++i) *f.mem.translate<be<uint32_t>>(code.guest + 4 * uint32_t(i)) = known[i];
    Pm4Builder b;
    b.type3(pm4::IM_LOAD, {code.phys | 0 /* vertex */, uint32_t(known.size())});
    im_load_immediate(b, ShaderStage::Pixel, unknown);
    im_load_immediate(b, ShaderStage::Pixel, unknown);
    g.submit(b);
    g.gpu->start();
    REQUIRE(wait_until([&] { return g.drained(); }));
    g.gpu->stop();
    CHECK(g.fatal().empty());
    CHECK(counting->loads == 3);  // every load still reaches the backend

    const lr::gpu::ShaderLoadStats stats = g.gpu->shader_stats();
    CHECK(stats.loads == 3);
    REQUIRE(stats.distinct.size() == 2);
    CHECK(stats.distinct[0].hash == known_hash);
    CHECK(stats.distinct[0].stage == ShaderStage::Vertex);
    CHECK(stats.distinct[0].cached);
    CHECK(stats.distinct[1].hash == unknown_hash);
    CHECK(stats.distinct[1].stage == ShaderStage::Pixel);
    CHECK_FALSE(stats.distinct[1].cached);

    std::ifstream in(trace);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    CHECK(text.find("shader vertex " + hex64(known_hash) + " dwords=" + std::to_string(known.size()) + " cached\n") !=
          std::string::npos);
    const std::string missing = "shader pixel " + hex64(unknown_hash) + " dwords=3 missing\n";
    REQUIRE(text.find(missing) != std::string::npos);
    CHECK(text.find(missing, text.find(missing) + 1) == std::string::npos);  // once per distinct shader
  }
  SECTION("a backend that binds shaders gets a fatal naming the hash") {
    Gpu g(f);
    REQUIRE(g.gpu->shader_cache().register_entries(entries, blob, spirv.size() * 4));
    auto backend = std::make_unique<CountingBackend>(true);
    CountingBackend* counting = backend.get();
    g.gpu->set_backend(std::move(backend));
    Pm4Builder b;
    im_load_immediate(b, ShaderStage::Vertex, known);
    im_load_immediate(b, ShaderStage::Pixel, unknown);
    g.submit(b);
    g.gpu->start();
    REQUIRE(wait_until([&] { return !g.fatal().empty(); }));
    CHECK(g.fatal() == "shader microcode " + hex64(unknown_hash) + " (pixel, 3 dwords) is not in the shader cache");
    g.gpu->stop();
    CHECK(counting->loads == 1);  // the missing shader never reaches the backend
  }
}
