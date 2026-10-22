// SPDX-License-Identifier: MIT
// The runtime binds loaded microcode to the fork's translations: the shader cache generated from the synthetic
// containers of cases.h, registered with lr::gpu::ShaderCache exactly as the game's main.cpp registers the real one,
// finds every shader by the hash the runtime computes over the microcode as the game uploads it with IM_LOAD
// (big-endian dwords in guest memory, no container header).
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "cases.h"
#include "runtime/gpu/shader_cache.h"

extern "C" const lr_shader_cache_entry lr_shader_cache[];
extern "C" const size_t lr_shader_cache_count;
extern "C" const uint8_t lr_shader_blob[];
extern "C" const size_t lr_shader_blob_size;
extern "C" const size_t lr_shader_blob_uncompressed_size;

namespace x = lr::testing::xenos;
namespace sh = lr::testing::shaders;

TEST_CASE("hash of loaded microcode matches the fork's hash", "[unit][shaders]") {
  lr::gpu::ShaderCache cache;
  REQUIRE(cache.register_entries(std::span(lr_shader_cache, lr_shader_cache_count),
                                 std::span(lr_shader_blob, lr_shader_blob_size), lr_shader_blob_uncompressed_size));
  REQUIRE(cache.size() == lr_shader_cache_count);
  for (const sh::Case& c : sh::good_cases()) {
    INFO(c.file);
    // What IM_LOAD hands the backend: `dwords` microcode dwords at a guest address, stored big-endian. The
    // container's microcode starts at virtualSize + physicalOffset and is `size` bytes: exactly these bytes.
    const std::vector<uint8_t> container = x::build_container(c.desc);
    const std::vector<uint8_t> microcode = x::microcode_bytes(c.desc);
    REQUIRE(container.size() >= microcode.size());
    std::vector<uint8_t> guest(0x100, 0xCD);  // some guest memory around the upload
    const size_t at = 0x40;
    guest.insert(guest.begin() + at, container.end() - std::ptrdiff_t(microcode.size()), container.end());
    const uint32_t dwords = uint32_t(c.desc.microcode.size());
    const uint64_t hash = lr::gpu::microcode_hash(guest.data() + at, dwords);

    const lr::gpu::ShaderEntry* e = cache.find(hash);
    REQUIRE(e != nullptr);
    CHECK(e->stage == (c.desc.vertex ? lr::gpu::ShaderStage::Vertex : lr::gpu::ShaderStage::Pixel));
    auto spirv = cache.spirv(*e);
    REQUIRE(spirv);
    REQUIRE(spirv.value().size() > 5);
    CHECK(spirv.value()[0] == 0x07230203u);
  }
  CHECK(cache.find(lr::gpu::microcode_hash(nullptr, 0)) == nullptr);
}
