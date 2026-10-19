// SPDX-License-Identifier: MIT
// Physical -> guest translation (PhysicalHeap::guest_from_physical), Kernel::gpu_memory_access and the GPU MMIO
// window.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <cstring>

#include "runtime/gpu/mmio.h"
#include "runtime/gpu/null_backend.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "runtime/memory/physical_heap.h"
#include "runtime/memory_map.h"
#include "support/kernel_fixture.h"
#include "support/pm4_builder.h"

using namespace lr;
using lr::runtime::PhysicalHeap;

namespace {
constexpr uint32_t kRW = 0x04;
uint32_t be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
}  // namespace

TEST_CASE("physical address of a C0 allocation maps back to its view address", "[unit][gpu]") {
  PhysicalHeap heap;
  auto g = heap.allocate(0x1000000, PhysicalHeap::kMem16MbPages | kRW, 0, 0xFFFFFFFF, 0);
  REQUIRE(g);
  CHECK(g.value() >= runtime::kPhysicalViewC0);
  const uint32_t phys = PhysicalHeap::physical_address(g.value());
  CHECK(heap.guest_from_physical(phys) == g.value());
  CHECK(heap.guest_from_physical(phys + 0x123456) == g.value() + 0x123456);  // linear inside the allocation
  CHECK(heap.guest_from_physical(phys + 0x1000000 - 1) == g.value() + 0xFFFFFF);
  REQUIRE(heap.free(g.value()));
  CHECK_FALSE(heap.guest_from_physical(phys).has_value());
}

TEST_CASE("A0 view maps back too", "[unit][gpu]") {
  PhysicalHeap heap;
  auto g = heap.allocate(0x20000, PhysicalHeap::kMemLargePages | kRW, 0, 0xFFFFFFFF, 0);
  REQUIRE(g);
  CHECK(g.value() >= runtime::kPhysicalViewA0);
  CHECK(g.value() < runtime::kPhysicalViewC0);
  CHECK(heap.guest_from_physical(PhysicalHeap::physical_address(g.value()) + 0x10004) == g.value() + 0x10004);
}

TEST_CASE("E0 view round trip honours +0x1000", "[unit][gpu]") {
  PhysicalHeap heap;
  auto g = heap.allocate(0x3000, kRW, 0, 0xFFFFFFFF, 0);
  REQUIRE(g);
  CHECK(g.value() >= runtime::kPhysicalViewE0);
  const uint32_t phys = PhysicalHeap::physical_address(g.value());
  CHECK(phys == (g.value() & 0x1FFFFFFF) + 0x1000);
  CHECK(heap.guest_from_physical(phys) == g.value());
  CHECK(heap.guest_from_physical(phys + 0x2FFC) == g.value() + 0x2FFC);
  CHECK_FALSE(heap.guest_from_physical(phys + 0x3000).has_value());
  CHECK_FALSE(heap.guest_from_physical(0x0).has_value());  // physical 0 is unreachable through E0
  // An allocation at the very bottom of the usable E0 range.
  auto low = heap.allocate(0x1000, kRW, 0x1000, 0x1FFF, 0);
  REQUIRE(low);
  CHECK(low.value() == runtime::kPhysicalViewE0);
  CHECK(heap.guest_from_physical(0x1000) == runtime::kPhysicalViewE0);
}

TEST_CASE("physical addresses outside memory or unallocated are not translated", "[unit][gpu]") {
  PhysicalHeap heap;
  CHECK_FALSE(heap.guest_from_physical(0x10000000).has_value());
  CHECK_FALSE(heap.guest_from_physical(runtime::kPhysicalSize).has_value());
  CHECK_FALSE(heap.guest_from_physical(0xFFFFFFFF).has_value());
}

TEST_CASE("untranslatable physical address is reported", "[unit][gpu]") {
  lr::testing::KernelFixture f;
  auto& k = kernel::kernel();
  gpu::MemoryAccess mem = k.gpu_memory_access();
  CHECK(mem.base == f.mem.base());
  CHECK(mem.size == f.mem.size());
  CHECK_FALSE(mem.to_guest(0x0ABC0000).has_value());

  gpu::RegisterFile regs;
  gpu::NullBackend backend;
  gpu::CommandProcessor cp{mem, regs, backend, [](uint32_t) {}};

  // A real allocation translates and MEM_WRITE lands in it (big-endian dword).
  auto g = k.physical().allocate(0x1000, kRW, 0, 0xFFFFFFFF, 0);
  REQUIRE(g);
  const uint32_t phys = PhysicalHeap::physical_address(g.value());
  const uint32_t ring = lr::testing::KernelFixture::kHeapBase;
  testing::Pm4Builder ok;
  ok.type3(gpu::pm4::MEM_WRITE, {phys | gpu::pm4::kEndian8in32, 0xCAFEF00D});
  ok.store(f.mem.base() + ring);
  auto r = cp.execute(ring, ok.size());
  REQUIRE(r);
  CHECK(be32(f.mem.base() + g.value()) == 0xCAFEF00D);

  // An unallocated physical address is a fatal naming that address.
  testing::Pm4Builder bad;
  bad.type3(gpu::pm4::MEM_WRITE, {0x0ABC0000u | gpu::pm4::kEndian8in32, 1});
  bad.store(f.mem.base() + ring);
  auto e = cp.execute(ring, bad.size());
  REQUIRE_FALSE(e);
  CHECK_THAT(e.error().message, Catch::Matchers::ContainsSubstring("0x0ABC0000"));
}

TEST_CASE("the GPU MMIO window is committed with idle values", "[unit][gpu]") {
  lr::testing::KernelFixture f;
  const uint8_t* base = f.mem.base();
  CHECK(be32(base + gpu::mmio_register_address(gpu::kRegRbbmStatus)) == gpu::kRbbmStatusIdle);
  CHECK((gpu::kRbbmStatusIdle & 0x80000000u) == 0);  // "GUI active" clear
  CHECK(be32(base + gpu::mmio_register_address(gpu::CP_RB_WPTR)) == 0);
  CHECK(gpu::mmio_register_address(gpu::CP_RB_WPTR) == 0x7FC80000 + 0x1C5 * 4);
  // The whole window is writable and readable back, first and last dword.
  auto* first = f.mem.translate<uint32_t>(gpu::kMmioWindowBase);
  auto* last = f.mem.translate<uint32_t>(gpu::kMmioWindowEnd - 4);
  *first = 0x11223344;
  *last = 0x55667788;
  CHECK(*first == 0x11223344);
  CHECK(*last == 0x55667788);
}
