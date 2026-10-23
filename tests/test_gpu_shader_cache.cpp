// SPDX-License-Identifier: MIT
// The runtime side of the plan B translator (Task 15.10): ShaderKey and its modifiers (runtime/gpu/shader/
// shader_key.h), the disk cache (shader/disk_cache.h), the ShaderCache (runtime/gpu/shader_cache.h), GpuSystem's
// translation at draw time with its trace, statistics and fatal/report policy (gpu_system.h), and the golden
// fingerprint that ties the translator's output to kTranslatorVersion. The microcode is built by hand with the test
// builder (tests/support/xenos_shader_builder.h).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/be.h"
#include "runtime/gpu/gpu_system.h"
#include "runtime/gpu/mmio.h"
#include "runtime/gpu/null_backend.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "runtime/gpu/shader/disk_cache.h"
#include "runtime/gpu/shader/ir.h"
#include "runtime/gpu/shader/shader_key.h"
#include "runtime/gpu/shader/translator.h"
#include "runtime/gpu/shader_cache.h"
#include "runtime/kernel/kernel.h"
#include "runtime/memory/physical_heap.h"
#include "support/kernel_fixture.h"
#include "support/pm4_builder.h"
#include "support/temp_dir.h"
#include "support/xenos_shader_builder.h"

namespace fs = std::filesystem;
namespace sh = lr::gpu::shader;
using lr::gpu::ActiveShaders;
using lr::gpu::GpuSystem;
using lr::gpu::ShaderCache;
using lr::gpu::ShaderLoadStats;
using lr::gpu::ShaderStage;
using lr::runtime::be;
using lr::testing::KernelFixture;
using lr::testing::Pm4Builder;
using lr::testing::TempDir;
using namespace std::chrono_literals;

namespace {

namespace pm4 = lr::gpu::pm4;
namespace x = lr::testing::xenos;

std::string hex64(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%016llX", static_cast<unsigned long long>(v));
  return b;
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// --- microcode ----------------------------------------------------------------------------------------------------

x::AluSrc r(uint32_t reg, uint32_t swizzle = 0) { return {reg, true, swizzle}; }
x::AluSrc c(uint32_t reg, uint32_t swizzle = 0) { return {reg, false, swizzle}; }

x::Alu vop(uint32_t op, uint32_t dest, x::AluSrc a, x::AluSrc b, bool to_export = false) {
  x::Alu alu;
  alu.vector_op = op;
  alu.vector_dest = dest;
  alu.export_data = to_export;
  alu.src1 = a;
  alu.src2 = b;
  return alu;
}
x::Alu mov(uint32_t dest, x::AluSrc s, bool to_export = false) { return vop(x::kVectorMax, dest, s, s, to_export); }

// One exec_end clause of ALU slots.
std::vector<uint32_t> alu_only(const std::vector<x::Alu>& slots) {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(true, 1, uint32_t(slots.size()), 0), x::cf_nop()));
  for (const x::Alu& a : slots) x::emit(code, x::alu(a));
  return code;
}

// VS: position = c0. PS: colour 0 = r0 (interpolator 0). Both translate.
std::vector<uint32_t> vertex_shader() { return alu_only({mov(x::kExportVsPosition, c(0), true)}); }
std::vector<uint32_t> pixel_shader() { return alu_only({mov(x::kExportPsColor0, r(0), true)}); }
// PS that writes eA: memory export, which the translator refuses.
std::vector<uint32_t> memexport_pixel_shader() { return alu_only({mov(x::kExportAddress, r(0), true)}); }

// VS reading vertex r0.x of fetch slot 0 with vertex format `format` into r1 and exporting it as the position: what
// D3D patches at run time is that vfetch's format/stride dword.
std::vector<uint32_t> vfetch_vertex_shader(uint32_t format) {
  std::vector<uint32_t> code;
  x::emit(code, x::cf_pair(x::cf_exec(false, 1, 1, 1 /*fetch*/), x::cf_exec(true, 2, 1, 0)));
  x::emit(code, x::vfetch(1, 0, 0, 0, 4, format));
  x::emit(code, x::alu(mov(x::kExportVsPosition, r(1), true)));
  return code;
}

uint64_t hash_of(const std::vector<uint32_t>& microcode) {
  const std::vector<uint8_t> bytes = x::microcode_bytes(microcode);
  return sh::microcode_hash(bytes.data(), uint32_t(microcode.size()));
}

sh::ShaderKey key_of(ShaderStage stage, const std::vector<uint32_t>& microcode, uint64_t modifiers = 0) {
  return {stage, hash_of(microcode), uint32_t(microcode.size()), modifiers};
}

// Loads `microcode` into `cache` (as the guest's big-endian bytes) and returns its key with `modifiers`.
sh::ShaderKey load(ShaderCache& cache, ShaderStage stage, const std::vector<uint32_t>& microcode,
                   uint64_t modifiers = 0) {
  const std::vector<uint8_t> bytes = x::microcode_bytes(microcode);
  const uint64_t hash = cache.load(stage, bytes.data(), uint32_t(microcode.size()));
  CHECK(hash == hash_of(microcode));
  return {stage, hash, uint32_t(microcode.size()), modifiers};
}

std::vector<uint32_t> translate_direct(ShaderStage stage, const std::vector<uint32_t>& microcode,
                                       const sh::TranslateOptions& options = {}) {
  auto program = sh::decode(stage, microcode);
  REQUIRE(program);
  auto t = sh::translate(program.value(), options);
  if (!t) FAIL("translate: " << t.error().message);
  return t.value().spirv;
}

std::vector<fs::path> files_in(const fs::path& dir) {
  std::vector<fs::path> out;
  for (const auto& e : fs::directory_iterator(dir)) out.push_back(e.path());
  return out;
}

// --- GpuSystem fixture ---------------------------------------------------------------------------------------------

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
  auto res = lr::kernel::kernel().physical().allocate(bytes, 0x20000004 /* MEM_LARGE_PAGES | PAGE_READWRITE */, 0,
                                                      0xFFFFFFFFu, 0);
  REQUIRE(res);
  return {res.value(), lr::runtime::PhysicalHeap::physical_address(res.value())};
}

// Counts calls and keeps the last draw's shaders; may claim to bind shaders.
struct CountingBackend final : lr::gpu::Backend {
  explicit CountingBackend(bool binds) : binds_(binds) {}
  std::atomic<uint32_t> loads{0}, draws{0};
  std::mutex m;
  ActiveShaders last;
  void load_shader(ShaderStage, uint32_t, uint32_t) override { ++loads; }
  void draw(const lr::gpu::RegisterFile&, const lr::gpu::DrawInfo&, const ActiveShaders& s) override {
    std::lock_guard<std::mutex> lock(m);
    last = s;
    ++draws;
  }
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
  // Writes the packets at WPTR and publishes WPTR; returns the ring dword index of the first one.
  uint32_t submit(const Pm4Builder& b) {
    const uint32_t first = wptr;
    for (uint32_t d : b.dwords()) *f.mem.translate<be<uint32_t>>(ring.guest + 4 * wptr++) = d;
    *f.mem.translate<be<uint32_t>>(lr::gpu::mmio_register_address(lr::gpu::CP_RB_WPTR)) = wptr;
    return first;
  }
  bool drained() { return gpu->read_pointer() == wptr; }
  void run(const Pm4Builder& b) {
    submit(b);
    gpu->start();
    REQUIRE(wait_until([&] { return drained() || !fatal().empty(); }));
  }
};

Pm4Builder& im_load_immediate(Pm4Builder& b, ShaderStage stage, const std::vector<uint32_t>& microcode) {
  std::vector<uint32_t> payload = {stage == ShaderStage::Vertex ? 0u : 1u, uint32_t(microcode.size())};
  payload.insert(payload.end(), microcode.begin(), microcode.end());
  return b.type3(pm4::IM_LOAD_IMMEDIATE, payload);
}

// RB_MODECONTROL colour+depth, then one auto-index triangle list (DRAW_INDX_2).
Pm4Builder& draw_triangle(Pm4Builder& b) {
  b.type0(lr::gpu::RB_MODECONTROL, {4});
  return b.type3(pm4::DRAW_INDX_2, {4u | (2u << 6) | (3u << 16)});
}

}  // namespace

// --- key ----------------------------------------------------------------------------------------------------------

TEST_CASE("microcode hash is XXH3-64 of the big-endian bytes", "[unit][gpu]") {
  // XXH3-64 reference value (seed 0) of the empty input; the hash depends on byte order.
  CHECK(sh::microcode_hash(nullptr, 0) == 0x2D06800538D394C2ull);
  const std::vector<uint32_t> code = vertex_shader();
  std::vector<uint8_t> le(code.size() * 4);
  std::memcpy(le.data(), code.data(), le.size());
  CHECK(hash_of(code) != sh::microcode_hash(le.data(), uint32_t(code.size())));
}

TEST_CASE("same microcode and modifiers give the same key", "[unit][gpu]") {
  lr::gpu::RegisterFile regs;
  regs.write(lr::gpu::SQ_INTERPOLATOR_CNTL, 0x00010002);
  const lr::gpu::DrawInfo draw{4, 3, 0, 0, false, 0};
  const std::vector<uint32_t> ps = pixel_shader();
  const std::vector<uint32_t> copy = ps;
  const sh::ShaderKey a = key_of(ShaderStage::Pixel, ps, sh::key_modifiers(ShaderStage::Pixel, regs, draw));
  const sh::ShaderKey b = key_of(ShaderStage::Pixel, copy, sh::key_modifiers(ShaderStage::Pixel, regs, draw));
  CHECK(a == b);
  CHECK(sh::cache_file_name(a) == sh::cache_file_name(b));
  char want[96];
  std::snprintf(want, sizeof want, "v%u-ps-%016llx-6-00000001000200ff.spv", sh::kTranslatorVersion,
                static_cast<unsigned long long>(a.microcode_hash));
  CHECK(sh::cache_file_name(a) == want);
  // The stage is part of the key and of the name.
  const sh::ShaderKey v = key_of(ShaderStage::Vertex, ps, 0);
  CHECK(v != a);
  CHECK(sh::cache_file_name(v).find("-vs-") != std::string::npos);
  // ShaderCache: one translation, then memory.
  ShaderCache cache;
  const sh::ShaderKey k = load(cache, ShaderStage::Pixel, ps, a.modifiers);
  auto first = cache.translate(k);
  REQUIRE(first);
  CHECK(cache.last_origin() == ShaderCache::Origin::Translator);
  load(cache, ShaderStage::Pixel, copy);
  auto again = cache.translate(k);
  REQUIRE(again);
  CHECK(cache.last_origin() == ShaderCache::Origin::Memory);
  CHECK(again.value() == first.value());
}

TEST_CASE("the fake guest's shader words match the builder", "[unit][gpu]") {
  // tests/support/fake_generated/fake_recomp.cpp hard-codes these (it compiles against the staged headers only).
  CHECK(vertex_shader() == std::vector<uint32_t>{0x00001001, 0x00002000, 0, 0xC80F803E, 0, 0x22000000});
  CHECK(pixel_shader() == std::vector<uint32_t>{0x00001001, 0x00002000, 0, 0xC80F8000, 0, 0xE2000000});
}

TEST_CASE("D3D-patched fetch dwords give a different key that translates", "[unit][gpu]") {
  // Two vertex shaders that differ only in the vfetch format (D3D patches it per vertex declaration).
  const std::vector<uint32_t> float4 = vfetch_vertex_shader(x::kFormat32_32_32_32Float);
  const std::vector<uint32_t> float2 = vfetch_vertex_shader(37 /* 32_32_FLOAT */);
  REQUIRE(float4.size() == float2.size());
  size_t differing = 0;
  for (size_t i = 0; i < float4.size(); ++i) differing += float4[i] != float2[i] ? 1 : 0;
  CHECK(differing == 1);
  ShaderCache cache;
  const sh::ShaderKey a = load(cache, ShaderStage::Vertex, float4);
  const sh::ShaderKey b = load(cache, ShaderStage::Vertex, float2);
  CHECK(a != b);
  CHECK(sh::cache_file_name(a) != sh::cache_file_name(b));
  auto ta = cache.translate(a);
  auto tb = cache.translate(b);
  REQUIRE(ta);
  REQUIRE(tb);
  CHECK(ta.value()->spirv != tb.value()->spirv);
  CHECK(ta.value()->info.vfetch_slots.test(0));
  CHECK(tb.value()->info.vfetch_slots.test(0));
}

TEST_CASE("param_gen and the point flag change the pixel shader key", "[unit][gpu]") {
  lr::gpu::RegisterFile regs;
  const lr::gpu::DrawInfo triangles{4, 3, 0, 0, false, 0};
  const lr::gpu::DrawInfo points{sh::kPrimPointList, 3, 0, 0, false, 0};
  // No param_gen: 0xFF, and a point draw changes nothing.
  CHECK(sh::key_modifiers(ShaderStage::Pixel, regs, triangles) == 0xFF);
  CHECK(sh::key_modifiers(ShaderStage::Pixel, regs, points) == 0xFF);
  // A param_gen position without SQ_PROGRAM_CNTL.param_gen is ignored.
  regs.write(lr::gpu::SQ_CONTEXT_MISC, 5u << 8);
  CHECK(sh::key_modifiers(ShaderStage::Pixel, regs, triangles) == 0xFF);
  // param_gen on (bit 18): the register is SQ_CONTEXT_MISC.param_gen_pos; points add bit 8.
  regs.write(lr::gpu::SQ_PROGRAM_CNTL, 1u << 18);
  const uint64_t tri = sh::key_modifiers(ShaderStage::Pixel, regs, triangles);
  const uint64_t pts = sh::key_modifiers(ShaderStage::Pixel, regs, points);
  CHECK(tri == 5);
  CHECK(pts == (5 | sh::kModPointBit));
  // A position past the interpolators means none.
  regs.write(lr::gpu::SQ_CONTEXT_MISC, 16u << 8);
  CHECK(sh::key_modifiers(ShaderStage::Pixel, regs, points) == 0xFF);
  // The vertex shader has no modifiers.
  CHECK(sh::key_modifiers(ShaderStage::Vertex, regs, points) == 0);
  // Different modifiers, different keys and files.
  const std::vector<uint32_t> ps = pixel_shader();
  CHECK(key_of(ShaderStage::Pixel, ps, tri) != key_of(ShaderStage::Pixel, ps, pts));
  CHECK(sh::cache_file_name(key_of(ShaderStage::Pixel, ps, tri)) !=
        sh::cache_file_name(key_of(ShaderStage::Pixel, ps, pts)));
}

TEST_CASE("interpolator flat and centroid masks change the pixel shader key", "[unit][gpu]") {
  lr::gpu::RegisterFile regs;
  const lr::gpu::DrawInfo draw{4, 3, 0, 0, false, 0};
  regs.write(lr::gpu::SQ_INTERPOLATOR_CNTL, 0x0003);  // param_shade: r0, r1 flat
  const uint64_t flat = sh::key_modifiers(ShaderStage::Pixel, regs, draw);
  CHECK(flat == (0xFF | (0x0003ull << 16)));
  regs.write(lr::gpu::SQ_INTERPOLATOR_CNTL, 0x00050000);  // sampling_pattern: r0, r2 centroid
  const uint64_t centroid = sh::key_modifiers(ShaderStage::Pixel, regs, draw);
  CHECK(centroid == (0xFF | (0x0005ull << 32)));
  regs.write(lr::gpu::SQ_INTERPOLATOR_CNTL, 0xFFFFFFFF);
  CHECK(sh::key_modifiers(ShaderStage::Pixel, regs, draw) == (0xFF | (0xFFFFull << 16) | (0xFFFFull << 32)));
  CHECK(flat != centroid);
  CHECK(sh::key_modifiers(ShaderStage::Vertex, regs, draw) == 0);  // VS outputs carry no interpolation decorations
}

TEST_CASE("key options decode the modifiers", "[unit][gpu]") {
  const uint64_t mod = 3 | sh::kModPointBit | (0x1234ull << 16) | (0xABCDull << 32);
  const sh::TranslateOptions o = sh::key_options({ShaderStage::Pixel, 1, 6, mod});
  CHECK(o.ps_param_gen == 3);
  CHECK(o.ps_point);
  CHECK(o.flat_mask == 0x1234);
  CHECK(o.centroid_mask == 0xABCD);
  const sh::TranslateOptions none = sh::key_options({ShaderStage::Pixel, 1, 6, 0xFF});
  CHECK(none.ps_param_gen == sh::kNoParamGen);
  CHECK_FALSE(none.ps_point);
  CHECK(none.flat_mask == 0);
  CHECK(none.centroid_mask == 0);
  // The vertex shader decodes to the defaults whatever the bits.
  const sh::TranslateOptions vs = sh::key_options({ShaderStage::Vertex, 1, 6, mod});
  CHECK(vs.ps_param_gen == sh::kNoParamGen);
  CHECK_FALSE(vs.ps_point);
  CHECK(vs.flat_mask == 0);
  CHECK(vs.centroid_mask == 0);
  // Round trip through the registers: the options the cache translates with are the draw's.
  lr::gpu::RegisterFile regs;
  regs.write(lr::gpu::SQ_PROGRAM_CNTL, 1u << 18);
  regs.write(lr::gpu::SQ_CONTEXT_MISC, 7u << 8);
  regs.write(lr::gpu::SQ_INTERPOLATOR_CNTL, 0x00F0000F);
  const sh::TranslateOptions rt = sh::key_options(
      {ShaderStage::Pixel, 1, 6,
       sh::key_modifiers(ShaderStage::Pixel, regs, lr::gpu::DrawInfo{sh::kPrimPointList, 1, 0, 0, false, 0})});
  CHECK(rt.ps_param_gen == 7);
  CHECK(rt.ps_point);
  CHECK(rt.flat_mask == 0x000F);
  CHECK(rt.centroid_mask == 0x00F0);
  // A flat mask is a different module (the PS reads r0: interpolator 0 becomes Flat).
  CHECK(translate_direct(ShaderStage::Pixel, pixel_shader(), sh::key_options({ShaderStage::Pixel, 1, 6, 0xFF})) !=
        translate_direct(ShaderStage::Pixel, pixel_shader(),
                         sh::key_options({ShaderStage::Pixel, 1, 6, 0xFF | (1ull << 16)})));
}

// --- disk cache ---------------------------------------------------------------------------------------------------

TEST_CASE("translated module is written to and read from the disk cache", "[unit][gpu]") {
  TempDir td("lr_shader_disk");
  const fs::path dir = td.path / "work" / "shaders" / "runtime";  // created by set_disk_dir
  const std::vector<uint32_t> ps = pixel_shader();
  std::vector<uint32_t> spirv;
  lr::gpu::shader::ShaderInfo info;
  {
    ShaderCache cache;
    cache.set_disk_dir(dir);
    REQUIRE(cache.disk_enabled());
    const sh::ShaderKey k = load(cache, ShaderStage::Pixel, ps, 0xFF);
    auto t = cache.translate(k);
    REQUIRE(t);
    CHECK(cache.last_origin() == ShaderCache::Origin::Translator);
    spirv = t.value()->spirv;
    info = t.value()->info;
    CHECK(fs::is_regular_file(dir / sh::cache_file_name(k)));
    CHECK(files_in(dir).size() == 1);  // the temporary file was renamed, nothing else is left
  }
  // A new process (cache) with the same directory reads the module back, with the same ShaderInfo.
  ShaderCache cache;
  cache.set_disk_dir(dir);
  const sh::ShaderKey k = load(cache, ShaderStage::Pixel, ps, 0xFF);
  auto t = cache.translate(k);
  REQUIRE(t);
  CHECK(cache.last_origin() == ShaderCache::Origin::Disk);
  CHECK(t.value()->spirv == spirv);
  CHECK(t.value()->info.interpolators_read == info.interpolators_read);
  CHECK(t.value()->info.interpolators_read == 1);
  CHECK(t.value()->info.colors_written == info.colors_written);
  // The file layout: magic, version, then the key.
  const std::string bytes = read_text(dir / sh::cache_file_name(k));
  REQUIRE(bytes.size() > sh::kDiskCacheHeaderBytes);
  CHECK(bytes.substr(0, 4) == "LRSV");
  uint32_t version;
  std::memcpy(&version, bytes.data() + 4, 4);
  CHECK(version == sh::kTranslatorVersion);
  // shader_info() is what translate() reports.
  auto program = sh::decode(ShaderStage::Pixel, ps);
  REQUIRE(program);
  CHECK(sh::shader_info(program.value(), sh::key_options(k)).interpolators_read == info.interpolators_read);
}

TEST_CASE("cache file from another translator version is ignored", "[unit][gpu]") {
  TempDir td("lr_shader_version");
  const std::vector<uint32_t> ps = pixel_shader();
  const sh::ShaderKey k = key_of(ShaderStage::Pixel, ps, 0xFF);
  const std::vector<uint32_t> fresh = translate_direct(ShaderStage::Pixel, ps);
  // A file under the current name whose header says another version (and a decoy SPIR-V).
  sh::DiskCache disk(td.path);
  std::vector<uint32_t> decoy = fresh;
  decoy.push_back(0);  // still a SPIR-V header, different words
  REQUIRE(disk.store(k, ps, decoy));
  const fs::path file = td.path / sh::cache_file_name(k);
  {
    std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
    const uint32_t old = sh::kTranslatorVersion - 1;
    f.seekp(4);
    f.write(reinterpret_cast<const char*>(&old), 4);
  }
  CHECK_FALSE(disk.load(k, ps).has_value());
  // The cache retranslates and overwrites it.
  ShaderCache cache;
  cache.set_disk_dir(td.path);
  load(cache, ShaderStage::Pixel, ps, 0xFF);
  auto t = cache.translate(k);
  REQUIRE(t);
  CHECK(cache.last_origin() == ShaderCache::Origin::Translator);
  CHECK(t.value()->spirv == fresh);
  auto back = disk.load(k, ps);
  REQUIRE(back.has_value());
  CHECK(*back == fresh);
  // The names carry the version: an older translator's file is never even opened.
  CHECK(sh::cache_file_name(k).starts_with("v" + std::to_string(sh::kTranslatorVersion) + "-"));
}

TEST_CASE("corrupt cache file is retranslated", "[unit][gpu]") {
  TempDir td("lr_shader_corrupt");
  const std::vector<uint32_t> ps = pixel_shader();
  const sh::ShaderKey k = key_of(ShaderStage::Pixel, ps, 0xFF);
  const std::vector<uint32_t> fresh = translate_direct(ShaderStage::Pixel, ps);
  sh::DiskCache disk(td.path);
  REQUIRE(disk.store(k, ps, fresh));
  const fs::path file = td.path / sh::cache_file_name(k);
  std::string bytes = read_text(file);
  const auto rewrite = [&](const std::string& content) {
    std::ofstream(file, std::ios::binary | std::ios::trunc).write(content.data(), std::streamsize(content.size()));
  };
  SECTION("a flipped SPIR-V byte (checksum)") {
    bytes[bytes.size() - 5] ^= 0x40;
    rewrite(bytes);
  }
  SECTION("a flipped microcode byte") {
    bytes[sh::kDiskCacheHeaderBytes + 1] ^= 0x01;
    rewrite(bytes);
  }
  SECTION("truncated") { rewrite(bytes.substr(0, bytes.size() - 8)); }
  SECTION("header only") { rewrite(bytes.substr(0, sh::kDiskCacheHeaderBytes)); }
  SECTION("empty") { rewrite(""); }
  SECTION("not ours") { rewrite(std::string(bytes.size(), 'x')); }
  SECTION("a header naming another key") {
    bytes[24] ^= 0x01;  // modifiers
    rewrite(bytes);
  }
  CHECK_FALSE(disk.load(k, ps).has_value());
  ShaderCache cache;
  cache.set_disk_dir(td.path);
  load(cache, ShaderStage::Pixel, ps, 0xFF);
  auto t = cache.translate(k);
  REQUIRE(t);
  CHECK(cache.last_origin() == ShaderCache::Origin::Translator);
  CHECK(t.value()->spirv == fresh);
  auto back = disk.load(k, ps);  // rewritten whole
  REQUIRE(back.has_value());
  CHECK(*back == fresh);
}

TEST_CASE("hash collision with different microcode is retranslated", "[unit][gpu]") {
  TempDir td("lr_shader_collision");
  const std::vector<uint32_t> ps = pixel_shader();
  const std::vector<uint32_t> other = alu_only({mov(x::kExportPsColor0, c(3), true)});
  REQUIRE(other.size() == ps.size());
  const sh::ShaderKey k = key_of(ShaderStage::Pixel, ps, 0xFF);
  // A file under ps's key that holds another microcode (and its module): what a 64-bit hash collision looks like.
  sh::DiskCache disk(td.path);
  REQUIRE(disk.store(k, other, translate_direct(ShaderStage::Pixel, other)));
  CHECK(disk.load(k, other).has_value());
  CHECK_FALSE(disk.load(k, ps).has_value());
  ShaderCache cache;
  cache.set_disk_dir(td.path);
  load(cache, ShaderStage::Pixel, ps, 0xFF);
  auto t = cache.translate(k);
  REQUIRE(t);
  CHECK(cache.last_origin() == ShaderCache::Origin::Translator);
  CHECK(t.value()->spirv == translate_direct(ShaderStage::Pixel, ps));
}

TEST_CASE("an unusable cache directory keeps the shaders in memory", "[unit][gpu]") {
  TempDir td("lr_shader_nodir");
  const fs::path not_a_dir = td.path / "file";
  std::ofstream(not_a_dir) << "x";
  ShaderCache cache;
  cache.set_disk_dir(not_a_dir / "runtime");  // its parent is a file: cannot be created
  CHECK_FALSE(cache.disk_enabled());
  const sh::ShaderKey k = load(cache, ShaderStage::Vertex, vertex_shader());
  REQUIRE(cache.translate(k));
  CHECK(cache.last_origin() == ShaderCache::Origin::Translator);
}

TEST_CASE("shader dump (--dump-shaders) writes the disassembly next to each cache file", "[unit][gpu]") {
  TempDir td("lr_shader_dump");
  ShaderCache cache;
  cache.set_disk_dir(td.path);
  cache.set_dump(true);
  const sh::ShaderKey ok = load(cache, ShaderStage::Vertex, vertex_shader());
  const sh::ShaderKey bad = load(cache, ShaderStage::Pixel, memexport_pixel_shader(), 0xFF);
  REQUIRE(cache.translate(ok));
  REQUIRE_FALSE(cache.translate(bad));
  const auto txt = [&](const sh::ShaderKey& k) {
    return fs::path(td.path / sh::cache_file_name(k)).replace_extension(".txt");
  };
  REQUIRE(fs::is_regular_file(txt(ok)));
  CHECK_THAT(read_text(txt(ok)), Catch::Matchers::ContainsSubstring("exec_end"));
  REQUIRE(fs::is_regular_file(txt(bad)));
  CHECK_THAT(read_text(txt(bad)), Catch::Matchers::ContainsSubstring("cannot be translated: "));
  CHECK_FALSE(fs::exists(td.path / sh::cache_file_name(bad)));  // nothing to cache for a failure
}

// --- golden fingerprint -------------------------------------------------------------------------------------------

TEST_CASE("translator output fingerprint matches kTranslatorVersion", "[unit][gpu]") {
  // Three reference shaders that between them reach vertex fetch, the ALU, texture fetch (offsets, an unnormalized
  // cube), control flow, predication, kill and every pixel shader modifier. Their SPIR-V, concatenated, hashed: if
  // this fingerprint changes, the translator's output changed, so cached modules of the old output are stale. Bump
  // kTranslatorVersion (translator.h) and replace the entry below with the new version and the printed value.
  constexpr uint32_t kVersion = 9;
  constexpr uint64_t kFingerprint = 0x4260C796DECC1553ull;

  // 1. VS: a full and a mini vfetch, a mad, position and interpolator 0.
  std::vector<uint32_t> vs;
  {
    x::emit(vs, x::cf_pair(x::cf_exec(false, 1, 2, 0x5 /*fetch, fetch*/), x::cf_exec(true, 3, 3, 0)));
    x::emit(vs, x::vfetch(1, 0, 0, 0, 8, x::kFormat32_32_32_32Float));
    x::emit(vs, x::vfetch_mini(2, 4, x::kFormat32_32_32_32Float));
    x::Alu mad = vop(x::kVectorMulAdd, 3, r(1), c(1));
    mad.src3 = c(2);
    x::emit(vs, x::alu(mad));
    x::emit(vs, x::alu(mov(x::kExportVsPosition, r(3), true)));
    x::emit(vs, x::alu(mov(0, r(2), true)));
  }
  // 2. PS: a 2D tfetch with x/y offsets and an unnormalized cube tfetch, added into colour 0.
  std::vector<uint32_t> tex;
  {
    x::emit(tex, x::cf_pair(x::cf_exec(false, 1, 2, 0x5), x::cf_exec(true, 3, 1, 0)));
    x::TFetch t2;
    t2.dst = 1;
    t2.src = 0;
    t2.const_index = 0;
    t2.dimension = x::kTextureDim2D;
    t2.offset_x = 1;
    t2.offset_y = -3;
    x::emit(tex, x::tfetch(t2));
    x::TFetch cube;
    cube.dst = 2;
    cube.src = 0;
    cube.const_index = 1;
    cube.dimension = x::kTextureDimCube;
    cube.unnormalized = true;
    x::emit(tex, x::tfetch(cube));
    x::emit(tex, x::alu(vop(x::kVectorAdd, x::kExportPsColor0, r(1), r(2), true)));
  }
  // 3. PS: a predicate set, a predicated kill, a cond_exec on a bool, and colour 0 = r1 * r3 (r3: param_gen).
  std::vector<uint32_t> flow;
  {
    x::emit(flow, x::cf_pair(x::cf_exec(false, 1, 2, 0), x::cf_cond_exec(true, 3, 1, 0, 5, true)));
    x::Alu pred;
    pred.vector_mask = 0;
    pred.scalar_op = 27;  // PRED_SETEs
    pred.scalar_mask = 0;
    pred.src3 = r(0);  // operand a: r0.w
    x::emit(flow, x::alu(pred));
    x::Alu kill = vop(x::kVectorKillGt, 2, r(0), c(0));
    kill.predicated = true;
    kill.pred_condition = true;
    x::emit(flow, x::alu(kill));
    x::emit(flow, x::alu(vop(x::kVectorMul, x::kExportPsColor0, r(1), r(3), true)));
  }
  sh::TranslateOptions flow_options;
  flow_options.ps_param_gen = 3;
  flow_options.flat_mask = 0x2;
  flow_options.centroid_mask = 0x1;

  std::vector<uint32_t> all = translate_direct(ShaderStage::Vertex, vs);
  for (const auto& m :
       {translate_direct(ShaderStage::Pixel, tex), translate_direct(ShaderStage::Pixel, flow, flow_options)})
    all.insert(all.end(), m.begin(), m.end());
  const uint64_t fingerprint = sh::microcode_hash(reinterpret_cast<const uint8_t*>(all.data()), uint32_t(all.size()));
  INFO("kTranslatorVersion " << sh::kTranslatorVersion << ", fingerprint " << hex64(fingerprint));
  CHECK(sh::kTranslatorVersion == kVersion);
  CHECK(fingerprint == kFingerprint);
}

// --- GpuSystem ----------------------------------------------------------------------------------------------------

TEST_CASE("IM_LOAD_IMMEDIATE microcode is copied before the packet ends", "[unit][gpu]") {
  KernelFixture f;
  Gpu g(f);
  auto backend = std::make_unique<CountingBackend>(true);
  CountingBackend* counting = backend.get();
  g.gpu->set_backend(std::move(backend));
  const std::vector<uint32_t> vs = vertex_shader(), ps = pixel_shader();
  Pm4Builder loads;
  im_load_immediate(loads, ShaderStage::Vertex, vs);
  im_load_immediate(loads, ShaderStage::Pixel, ps);
  const uint32_t at = g.submit(loads);
  g.gpu->start();
  REQUIRE(wait_until([&] { return g.drained(); }));
  // The ring space is reused: the inline microcode is gone before any draw needs it.
  for (uint32_t i = 0; i < loads.size(); ++i) *f.mem.translate<be<uint32_t>>(g.ring.guest + 4 * (at + i)) = 0xDEADBEEF;
  Pm4Builder draw;
  draw_triangle(draw);
  g.submit(draw);
  REQUIRE(wait_until([&] { return g.drained() && counting->draws == 1; }));
  g.gpu->stop();
  CHECK(g.fatal().empty());
  CHECK(counting->loads == 2);
  std::lock_guard<std::mutex> lock(counting->m);
  REQUIRE(counting->last.vs != nullptr);
  REQUIRE(counting->last.ps != nullptr);
  CHECK(counting->last.vs_key == sh::ShaderKey{ShaderStage::Vertex, hash_of(vs), uint32_t(vs.size()), 0});
  CHECK(counting->last.ps_key == sh::ShaderKey{ShaderStage::Pixel, hash_of(ps), uint32_t(ps.size()), 0xFF});
  CHECK(counting->last.vs->spirv == translate_direct(ShaderStage::Vertex, vs));
  CHECK(counting->last.ps->spirv == translate_direct(ShaderStage::Pixel, ps));
  const ShaderLoadStats stats = g.gpu->shader_stats();
  CHECK(stats.loads == 2);
  REQUIRE(stats.keys.size() == 2);
  CHECK(stats.keys[0].status == ShaderLoadStats::Status::Translated);
  CHECK(stats.keys[1].status == ShaderLoadStats::Status::Translated);
}

TEST_CASE("untranslatable shader is fatal with a backend that binds shaders", "[unit][gpu]") {
  KernelFixture f;
  Gpu g(f);
  auto backend = std::make_unique<CountingBackend>(true);
  CountingBackend* counting = backend.get();
  g.gpu->set_backend(std::move(backend));
  const std::vector<uint32_t> ps = memexport_pixel_shader();
  Pm4Builder b;
  im_load_immediate(b, ShaderStage::Vertex, vertex_shader());
  im_load_immediate(b, ShaderStage::Pixel, ps);
  draw_triangle(b);
  draw_triangle(b);
  g.submit(b);
  g.gpu->start();
  REQUIRE(wait_until([&] { return !g.fatal().empty(); }));
  g.gpu->stop();
  const std::string fatal = g.fatal();
  CHECK_THAT(fatal, Catch::Matchers::StartsWith("shader microcode " + hex64(hash_of(ps)) + " (pixel, " +
                                                std::to_string(ps.size()) + " dwords) cannot be translated: "));
  CHECK_THAT(fatal, Catch::Matchers::ContainsSubstring("memexport"));
  CHECK(counting->draws == 0);  // the draw never reaches the backend
  const ShaderLoadStats stats = g.gpu->shader_stats();
  REQUIRE(stats.keys.size() == 2);
  CHECK(stats.keys[1].status == ShaderLoadStats::Status::Failed);
  CHECK_THAT(stats.keys[1].reason, Catch::Matchers::ContainsSubstring("memexport"));
}

TEST_CASE("untranslatable shader is only reported with the NullBackend", "[unit][gpu]") {
  KernelFixture f;
  Gpu g(f);
  auto backend = std::make_unique<lr::gpu::NullBackend>();
  lr::gpu::NullBackend* null = backend.get();
  g.gpu->set_backend(std::move(backend));
  const std::vector<uint32_t> ps = memexport_pixel_shader();
  Pm4Builder b;
  im_load_immediate(b, ShaderStage::Vertex, vertex_shader());
  im_load_immediate(b, ShaderStage::Pixel, ps);
  draw_triangle(b);
  draw_triangle(b);
  g.run(b);
  g.gpu->stop();
  CHECK(g.fatal().empty());
  CHECK(null->draw_calls == 2);  // the draws go on
  REQUIRE(null->last_shaders.has_value());
  CHECK(null->last_shaders->vs != nullptr);
  CHECK(null->last_shaders->ps == nullptr);  // failed: no module
  CHECK(null->last_shaders->ps_key.microcode_hash == hash_of(ps));
  const ShaderLoadStats stats = g.gpu->shader_stats();
  REQUIRE(stats.keys.size() == 2);  // one translation per key, not per draw
  CHECK(stats.keys[0].status == ShaderLoadStats::Status::Translated);
  CHECK(stats.keys[1].status == ShaderLoadStats::Status::Failed);
  CHECK_THAT(stats.keys[1].reason, Catch::Matchers::ContainsSubstring("memexport"));
}

TEST_CASE("GPU trace (--trace-gpu) names translated and failed shaders", "[unit][gpu]") {
  KernelFixture f;
  TempDir td("lr_shader_trace");
  const fs::path cache_dir = td.path / "cache";
  const std::vector<uint32_t> vs = vertex_shader(), ps = memexport_pixel_shader();
  const auto run = [&](const fs::path& trace) {
    Gpu g(f);
    g.gpu->set_shader_cache_dir(cache_dir);
    REQUIRE(g.gpu->enable_trace(trace));
    Pm4Builder b;
    im_load_immediate(b, ShaderStage::Vertex, vs);
    im_load_immediate(b, ShaderStage::Pixel, ps);
    im_load_immediate(b, ShaderStage::Pixel, ps);  // a second load of the same microcode: no new line
    draw_triangle(b);
    draw_triangle(b);  // same keys: no new line
    g.run(b);
    g.gpu->stop();
    CHECK(g.fatal().empty());
    return read_text(trace);
  };
  const auto count = [](const std::string& text, const std::string& line) {
    size_t n = 0;
    for (size_t at = text.find(line); at != std::string::npos; at = text.find(line, at + 1)) ++n;
    return n;
  };
  const std::string vs_id = hex64(hash_of(vs)) + " dwords=" + std::to_string(vs.size());
  const std::string ps_id = hex64(hash_of(ps)) + " dwords=" + std::to_string(ps.size());

  const std::string first = run(td.path / "first.log");
  INFO(first);
  CHECK(count(first, "shader load vs " + vs_id + "\n") == 1);
  CHECK(count(first, "shader load ps " + ps_id + "\n") == 1);
  CHECK(count(first, "shader vs " + vs_id + " mod=0x0 translated\n") == 1);
  CHECK(count(first, "shader ps " + ps_id + " mod=0xFF failed: ") == 1);
  CHECK_THAT(first, Catch::Matchers::ContainsSubstring("memexport"));

  // Next run, same cache: the vertex shader comes from disk; the failure is retried (failures are not cached).
  const std::string second = run(td.path / "second.log");
  INFO(second);
  CHECK(count(second, "shader vs " + vs_id + " mod=0x0 disk\n") == 1);
  CHECK(count(second, "shader ps " + ps_id + " mod=0xFF failed: ") == 1);
}

TEST_CASE("shader summary line at exit counts keys by status", "[unit][gpu]") {
  KernelFixture f;
  TempDir td("lr_shader_summary");
  const std::vector<uint32_t> vs = vertex_shader(), ps = pixel_shader(), bad = memexport_pixel_shader();
  const auto run = [&](const std::vector<std::vector<uint32_t>>& pixel_shaders) {
    Gpu g(f);
    g.gpu->set_shader_cache_dir(td.path);
    Pm4Builder b;
    im_load_immediate(b, ShaderStage::Vertex, vs);
    for (const auto& p : pixel_shaders) {
      im_load_immediate(b, ShaderStage::Pixel, p);
      draw_triangle(b);
    }
    g.run(b);
    g.gpu->stop();
    return g.gpu->shader_stats();
  };
  // Nothing drawn yet.
  CHECK(lr::gpu::format_shader_summary({}) ==
        "[gpu] shaders: keys=0 vs=0 ps=0 translated=0 disk=0 failed=0 new_last_100_swaps=0");
  // First run: everything translated (one failure).
  const ShaderLoadStats first = run({ps, bad});
  CHECK(first.loads == 3);
  CHECK(lr::gpu::format_shader_summary(first) ==
        "[gpu] shaders: keys=3 vs=1 ps=2 translated=2 disk=0 failed=1 new_last_100_swaps=3");
  for (const auto& k : first.keys) CHECK(k.first_swap == 0);
  // Second run: the translated ones come from disk.
  const ShaderLoadStats second = run({ps, bad});
  CHECK(lr::gpu::format_shader_summary(second) ==
        "[gpu] shaders: keys=3 vs=1 ps=2 translated=0 disk=2 failed=1 new_last_100_swaps=3");
  // new_last_100_swaps counts the keys first drawn within the last 100 swaps.
  ShaderLoadStats aged = second;
  aged.new_keys_last_100_swaps = 1;
  CHECK_THAT(lr::gpu::format_shader_summary(aged), Catch::Matchers::EndsWith(" new_last_100_swaps=1"));
}
