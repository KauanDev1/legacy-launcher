// SPDX-License-Identifier: MIT
// XenosRecomp (LegacyRecomp fork) harness: the synthetic containers of cases.h were translated at build time
// (run_xenosrecomp.cmake); the generated shader_cache.cpp is compiled into this test, and the HLSL the tool dumped
// sits in LR_SHADER_HLSL_DIR. Checks the runtime contract of docs/architecture.md ("Contrato dos shaders").
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <xxhash.h>
#include <zstd.h>

#include "cases.h"
// The patched shader_common.h: its LR_* layout macros are plain #defines, visible to C++.
#include "shader_common.h"

// The generated cache's interface (same declarations as the file the tool writes).
struct lr_shader_cache_entry {
  uint64_t microcode_hash;
  uint8_t stage;
  uint32_t spirv_offset, spirv_size;
  uint32_t spec_mask;
};
struct lr_shader_vertex_input {
  uint64_t microcode_hash;
  uint32_t location;
  uint32_t fetch_slot;
  uint32_t offset_dwords;
  uint32_t stride_dwords;
  uint32_t format;
  uint32_t flags;
  int32_t exp_adjust;
};
extern "C" const lr_shader_cache_entry lr_shader_cache[];
extern "C" const size_t lr_shader_cache_count;
extern "C" const lr_shader_vertex_input lr_shader_vertex_inputs[];
extern "C" const size_t lr_shader_vertex_input_count;
extern "C" const uint8_t lr_shader_blob[];
extern "C" const size_t lr_shader_blob_size;
extern "C" const size_t lr_shader_blob_uncompressed_size;

namespace {

namespace x = lr::testing::xenos;
namespace sh = lr::testing::shaders;

uint64_t microcode_hash(const x::ShaderDesc& d) {
  const auto bytes = x::microcode_bytes(d);
  return XXH3_64bits(bytes.data(), bytes.size());
}

std::string hex(uint64_t v) {
  char buf[19];
  std::snprintf(buf, sizeof buf, "0x%016llX", static_cast<unsigned long long>(v));
  return buf;
}

const lr_shader_cache_entry* find_entry(uint64_t hash) {
  for (size_t i = 0; i < lr_shader_cache_count; ++i)
    if (lr_shader_cache[i].microcode_hash == hash) return &lr_shader_cache[i];
  return nullptr;
}

const std::vector<uint8_t>& blob() {
  static const std::vector<uint8_t> data = [] {
    std::vector<uint8_t> out(lr_shader_blob_uncompressed_size);
    const size_t n = ZSTD_decompress(out.data(), out.size(), lr_shader_blob, lr_shader_blob_size);
    REQUIRE(!ZSTD_isError(n));
    REQUIRE(n == out.size());
    return out;
  }();
  return data;
}

std::vector<uint32_t> spirv(const lr_shader_cache_entry& e) {
  REQUIRE(e.spirv_size % 4 == 0);
  REQUIRE(size_t(e.spirv_offset) + e.spirv_size <= blob().size());
  std::vector<uint32_t> words(e.spirv_size / 4);
  std::memcpy(words.data(), blob().data() + e.spirv_offset, e.spirv_size);
  return words;
}

// SPIR-V instruction walk: calls f(opcode, operands) for each instruction after the 5-word header.
template <typename F>
void for_each_instruction(const std::vector<uint32_t>& words, F&& f) {
  REQUIRE(words.size() > 5);
  REQUIRE(words[0] == 0x07230203u);
  for (size_t i = 5; i < words.size();) {
    const uint32_t count = words[i] >> 16;
    const uint32_t opcode = words[i] & 0xFFFF;
    REQUIRE(count > 0);
    REQUIRE(i + count <= words.size());
    f(opcode, std::vector<uint32_t>(words.begin() + i + 1, words.begin() + i + count));
    i += count;
  }
}

constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kDecorationSpecId = 1;
constexpr uint32_t kDecorationLocation = 30;
constexpr uint32_t kDecorationDescriptorSet = 34;

std::optional<uint32_t> execution_model(const std::vector<uint32_t>& words) {
  std::optional<uint32_t> model;
  for_each_instruction(words, [&](uint32_t op, const std::vector<uint32_t>& args) {
    if (op == kOpEntryPoint) model = args.at(0);
  });
  return model;
}

std::vector<uint32_t> decorations(const std::vector<uint32_t>& words, uint32_t decoration) {
  std::vector<uint32_t> values;
  for_each_instruction(words, [&](uint32_t op, const std::vector<uint32_t>& args) {
    if (op == kOpDecorate && args.size() >= 3 && args[1] == decoration) values.push_back(args[2]);
  });
  return values;
}

bool contains(const std::vector<uint32_t>& v, uint32_t x) {
  for (uint32_t e : v)
    if (e == x) return true;
  return false;
}

std::string read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  REQUIRE(f);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

std::string hlsl(uint64_t hash) {
  char name[32];
  std::snprintf(name, sizeof name, "%016llX.hlsl", static_cast<unsigned long long>(hash));
  return read_text(std::string(LR_SHADER_HLSL_DIR) + "/" + name);
}

bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

}  // namespace

TEST_CASE("vertex shader container translates and reports its microcode hash", "[unit][shaders]") {
  const auto desc = sh::vertex_basic();
  const uint64_t hash = microcode_hash(desc);

  const auto log = read_text(LR_SHADER_LOG);
  CHECK(has(log, "translated 8 of 8"));
  CHECK(has(log, "microcode " + hex(hash) + " vertex"));
  // The container hash is reported next to it (XXH3-64 of the whole container, as upstream keys it).
  const auto container = x::build_container(desc);
  CHECK(has(log, "container " + hex(XXH3_64bits(container.data(), container.size())) + " microcode " + hex(hash)));

  const auto* e = find_entry(hash);
  REQUIRE(e != nullptr);
  CHECK(e->stage == 0);
  const auto words = spirv(*e);
  CHECK(execution_model(words) == 0u);  // Vertex

  // Vertex input: location 0 <- vertex element 0, fed by vertex fetch constant slot 0 (offset 0, stride 4 dwords).
  bool found = false;
  for (size_t i = 0; i < lr_shader_vertex_input_count; ++i) {
    const auto& v = lr_shader_vertex_inputs[i];
    if (v.microcode_hash != hash) continue;
    found = true;
    CHECK(v.location == 0);
    CHECK(v.fetch_slot == 0);
    CHECK(v.offset_dwords == 0);
    CHECK(v.stride_dwords == 4);
    CHECK(v.format == x::kFormat32_32_32_32Float);
  }
  CHECK(found);
  CHECK(contains(decorations(words, kDecorationLocation), 0));

  const auto text = hlsl(hash);
  CHECK(has(text, "[[vk::location(0)]] in float4 iFetch0"));
  CHECK(has(text, "g_HalfPixelOffset"));
  // Unleashed-only paths are compiled out (the dumped HLSL is the shader body; the prelude is an #include).
  CHECK(text.rfind("#include \"shader_common.h\"", 0) == 0);
  CHECK_FALSE(has(text, "g_SwappedTexcoords"));
  CHECK_FALSE(has(text, "tfetchR11G11B10"));
  CHECK_FALSE(has(text, "UNLEASHED"));
}

TEST_CASE("pixel shader with alpha test greater-equal", "[unit][shaders]") {
  const uint64_t hash = microcode_hash(sh::pixel_alpha_test());
  const auto* e = find_entry(hash);
  REQUIRE(e != nullptr);
  CHECK(e->stage == 1);
  CHECK((e->spec_mask & SPEC_CONSTANT_ALPHA_TEST) != 0);
  CHECK((e->spec_mask & SPEC_CONSTANT_ALPHA_FUNC_MASK) == SPEC_CONSTANT_ALPHA_FUNC_MASK);
  // The alpha function sits in the spec constant: 6 = GEQUAL (Xenos RB_COLORCONTROL.ALPHA_FUNC).
  STATIC_REQUIRE(SPEC_CONSTANT_ALPHA_FUNC_SHIFT == 2);
  STATIC_REQUIRE(SPEC_CONSTANT_ALPHA_FUNC_MASK == (7 << 2));

  const auto words = spirv(*e);
  CHECK(execution_model(words) == 4u);  // Fragment
  CHECK(contains(decorations(words, kDecorationSpecId), 0));
  // Bindless texture (set 0) and sampler (set 3) heaps.
  const auto sets = decorations(words, kDecorationDescriptorSet);
  CHECK(contains(sets, 0));
  CHECK(contains(sets, 3));

  // The compare function table lives in the prelude (shader_common.h).
  CHECK(has(read_text(LR_SHADER_COMMON_PATH), "case 6: return alpha >= reference;"));
  const auto text = hlsl(hash);
  CHECK(has(text, "alphaTestPasses(oC0.w, g_AlphaThreshold"));
  CHECK(has(text, "g_Texture2DIndex(0)"));
  CHECK(has(text, "g_PSConstant(0)"));
}

TEST_CASE("shader using bool constant 100", "[unit][shaders]") {
  const uint64_t hash = microcode_hash(sh::vertex_bool_100());
  REQUIRE(find_entry(hash) != nullptr);
  STATIC_REQUIRE(LR_SHARED_BOOLEANS_OFFSET == 512);
  STATIC_REQUIRE(LR_BOOLEAN_COUNT == 256);
  const auto text = hlsl(hash);
  CHECK(has(text, "if (g_Bool(100) == 1)"));
}

TEST_CASE("shader with a loop constant", "[unit][shaders]") {
  const uint64_t hash = microcode_hash(sh::vertex_loop());
  REQUIRE(find_entry(hash) != nullptr);
  STATIC_REQUIRE(LR_SHARED_LOOPS_OFFSET == 544);
  STATIC_REQUIRE(LR_LOOP_CONSTANT_COUNT == 32);
  const auto text = hlsl(hash);
  CHECK(has(text, "g_LoopConstant(5)"));
  CHECK(has(text, "g_VSConstant(0 + aL)"));
}

TEST_CASE("shared constants layout matches the documented contract", "[unit][shaders]") {
  STATIC_REQUIRE(LR_SHARED_TEXTURE_2D_OFFSET == 0);
  STATIC_REQUIRE(LR_SHARED_TEXTURE_3D_OFFSET == 128);
  STATIC_REQUIRE(LR_SHARED_TEXTURE_CUBE_OFFSET == 256);
  STATIC_REQUIRE(LR_SHARED_SAMPLER_OFFSET == 384);
  STATIC_REQUIRE(LR_SHARED_HALF_PIXEL_OFFSET == 672);
  STATIC_REQUIRE(LR_SHARED_ALPHA_REFERENCE_OFFSET == 680);
  STATIC_REQUIRE(LR_SHARED_CONSTANTS_SIZE == 688);
  STATIC_REQUIRE(LR_FLOAT_CONSTANT_COUNT == 256);
  STATIC_REQUIRE(LR_FETCH_CONSTANT_COUNT == 32);
}

TEST_CASE("pixel shader bool constant 130 indexes the shared bool file directly", "[unit][shaders]") {
  // The 8-bit bool address is an index into the 256-bit bool file, the same in both stages.
  const uint64_t hash = microcode_hash(sh::pixel_bool_130());
  REQUIRE(find_entry(hash) != nullptr);
  CHECK(has(hlsl(hash), "if (g_Bool(130) == 1)"));
}

TEST_CASE("second constant operand uses its own relative addressing bit", "[unit][shaders]") {
  const uint64_t hash = microcode_hash(sh::vertex_constant_pair());
  REQUIRE(find_entry(hash) != nullptr);
  CHECK(has(hlsl(hash), "g_VSConstant(0).xyzw + g_VSConstant(1 + aL).xyzw"));
}

TEST_CASE("repeat loop keeps the current aL", "[unit][shaders]") {
  const uint64_t hash = microcode_hash(sh::vertex_loop_repeat());
  REQUIRE(find_entry(hash) != nullptr);
  const auto text = hlsl(hash);
  CHECK(has(text, "g_LoopConstant(3)"));
  CHECK(has(text, "int lrSavedAL0 = aL;"));
  CHECK(has(text, "aL = lrSavedAL0;"));
  CHECK_FALSE(has(text, "aL = lrLoopStart("));
  // The non-repeat loop does load the start value.
  CHECK(has(hlsl(microcode_hash(sh::vertex_loop())), "aL = lrLoopStart(lrLoop0);"));
}

TEST_CASE("pixel shader reading only COLOR0 gets input location 16", "[unit][shaders]") {
  const uint64_t hash = microcode_hash(sh::pixel_color0_only());
  const auto* e = find_entry(hash);
  REQUIRE(e != nullptr);
  const auto locations = decorations(spirv(*e), kDecorationLocation);
  CHECK(contains(locations, 16));  // COLOR0 input
  CHECK(contains(locations, 0));   // SV_Target0
  CHECK(locations.size() == 2);
}

TEST_CASE("the tool reports loop ids taken from definition literals", "[unit][shaders]") {
  const auto log = read_text(LR_SHADER_LOG);
  CHECK(has(log, "microcode " + hex(microcode_hash(sh::vertex_loop())) + " vertex literal-loops 0"));
}
