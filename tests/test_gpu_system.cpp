// SPDX-License-Identifier: MIT
// The emulated GPU behind the Vd* imports (runtime/gpu/gpu_system.h): ring buffer consumption and RPTR write-back,
// the wrap linearisation, malformed rings, graphics interrupts and vblank pacing, plus the Vd* imports and GPU
// variables. "Guest" callbacks are host lambdas behind a test GuestGlue (same pattern as test_kernel_time.cpp).
// Threaded: every wait is "until a condition holds" with a generous deadline, never a sleep used as a barrier.
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "runtime/be.h"
#include "runtime/gpu/gpu_system.h"
#include "runtime/gpu/mmio.h"
#include "runtime/gpu/null_backend.h"
#include "runtime/gpu/pm4.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/variables.h"
#include "runtime/memory/physical_heap.h"
#include "support/kernel_fixture.h"
#include "support/pm4_builder.h"

using namespace lr::kernel;
using lr::gpu::GpuSystem;
using lr::runtime::be;
using lr::testing::KernelFixture;
using lr::testing::Pm4Builder;
using namespace std::chrono_literals;

namespace {

namespace pm4 = lr::gpu::pm4;

// --- Host-lambda guest functions (the graphics interrupt callback). ---
using GuestFn = std::function<uint64_t(const std::vector<uint64_t>&)>;
std::mutex g_fns_mutex;
std::map<uint32_t, GuestFn> g_fns;

uint64_t test_call(uint8_t*, uint32_t fn, const uint64_t* args, int nargs, uint32_t, uint32_t) {
  GuestFn f;
  {
    std::lock_guard<std::mutex> lock(g_fns_mutex);
    f = g_fns.at(fn);
  }
  return f(std::vector<uint64_t>(args, args + nargs));
}
bool test_has(uint32_t fn) {
  std::lock_guard<std::mutex> lock(g_fns_mutex);
  return g_fns.count(fn) != 0;
}
void def(uint32_t addr, GuestFn f) {
  set_guest_glue({test_call, test_has});
  std::lock_guard<std::mutex> lock(g_fns_mutex);
  g_fns[addr] = std::move(f);
}

constexpr uint32_t kCallback = 0x82000600;

// Polls `pred` until it holds or `limit` passes; returns its last value.
bool wait_until(const std::function<bool()>& pred, std::chrono::milliseconds limit = 10s) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > end) return pred();
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

uint32_t rd(KernelFixture& f, uint32_t addr) { return *f.mem.translate<be<uint32_t>>(addr); }
void wr(KernelFixture& f, uint32_t addr, uint32_t v) { *f.mem.translate<be<uint32_t>>(addr) = v; }

// A physical allocation: guest view address and physical address.
struct Phys {
  uint32_t guest, phys;
};
Phys phys_alloc(uint32_t bytes) {
  auto r = kernel().physical().allocate(bytes, 0x20000004 /* MEM_LARGE_PAGES | PAGE_READWRITE */, 0, 0xFFFFFFFFu, 0);
  REQUIRE(r);
  return {r.value(), lr::runtime::PhysicalHeap::physical_address(r.value())};
}

// The guest side of a ring: writes dwords at WPTR (wrapping) and publishes WPTR through the MMIO window.
struct Ring {
  KernelFixture& f;
  Phys mem;
  uint32_t dwords;
  uint32_t wptr = 0;
  void put(uint32_t v) {
    wr(f, mem.guest + wptr * 4, v);
    wptr = (wptr + 1) % dwords;
  }
  void put(const Pm4Builder& b) {
    for (uint32_t d : b.dwords()) put(d);
  }
  void submit() { wr(f, lr::gpu::mmio_register_address(lr::gpu::CP_RB_WPTR), wptr); }
};

// Records what the backend sees; load_shader also copies the microcode while the address is valid.
struct RecordingBackend final : lr::gpu::Backend {
  std::mutex m;
  std::vector<uint32_t> microcode;
  uint8_t* base = nullptr;
  void draw(const lr::gpu::RegisterFile&, const lr::gpu::DrawInfo&) override {}
  void resolve(const lr::gpu::RegisterFile&) override {}
  void swap(uint32_t, uint32_t, uint32_t) override {}
  void wait_idle() override {}
  void load_shader(lr::gpu::ShaderStage, uint32_t guest_addr, uint32_t dwords) override {
    std::lock_guard<std::mutex> lock(m);
    for (uint32_t i = 0; i < dwords; ++i) {
      uint32_t v;
      std::memcpy(&v, base + guest_addr + 4 * i, 4);
      microcode.push_back(__builtin_bswap32(v));
    }
  }
};

// A GpuSystem of its own over the fixture's memory, with recording hooks (fatal returns, so the GPU thread stops).
struct Standalone {
  std::mutex m;
  std::vector<std::string> fatals;
  std::unique_ptr<GpuSystem> gpu;
  explicit Standalone(KernelFixture& f) {
    lr::gpu::GpuHooks hooks;
    hooks.alloc_guest = [&f](uint32_t bytes) { return f.alloc(bytes); };
    hooks.fatal = [this](const std::string& what) {
      std::lock_guard<std::mutex> lock(m);
      fatals.push_back(what);
    };
    gpu = std::make_unique<GpuSystem>(kernel().gpu_memory_access(), std::move(hooks));
  }
  std::string fatal() {
    std::lock_guard<std::mutex> lock(m);
    return fatals.empty() ? std::string() : fatals.front();
  }
};

}  // namespace

TEST_CASE("ring buffer commands execute and RPTR is written back", "[unit][gpu]") {
  KernelFixture f;
  auto backend = std::make_unique<RecordingBackend>();
  RecordingBackend* rec = backend.get();
  rec->base = f.mem.base();
  kernel().gpu().set_backend(std::move(backend));

  constexpr uint32_t kLog2 = 9;  // 2^(9 + 3) = 4 KiB = 1024 dwords
  Ring ring{f, phys_alloc(0x10000), 1024};
  const Phys wb = phys_alloc(0x10000);
  const Phys fence = phys_alloc(0x10000);
  f.call("VdInitializeRingBuffer", ring.mem.phys, kLog2);
  f.call("VdEnableRingBufferRPtrWriteBack", wb.phys, 6u);
  wr(f, wb.guest, 0xFFFFFFFF);

  // MEM_WRITE of one value (8-in-32 endian, lands big-endian) and a register write.
  ring.put(Pm4Builder()
               .type3(pm4::MEM_WRITE, {fence.phys | pm4::kEndian8in32, 0x11223344})
               .type0(lr::gpu::SCRATCH_REG0, {7}));
  ring.submit();
  REQUIRE(wait_until([&] { return rd(f, wb.guest) == ring.wptr; }));
  CHECK(rd(f, fence.guest) == 0x11223344);
  CHECK(kernel().gpu().read_pointer() == ring.wptr);
  CHECK(rd(f, lr::gpu::mmio_register_address(lr::gpu::CP_RB_RPTR)) == ring.wptr);

  SECTION("a packet straddling the ring end runs from a linear copy with valid guest addresses") {
    // Type-2 NOPs up to 3 dwords before the end, then IM_LOAD_IMMEDIATE (header + 2 + 4 microcode dwords) and a
    // MEM_WRITE: the first packet straddles the end.
    while (ring.wptr != 1024 - 3) ring.put(2u << 30);
    ring.submit();
    REQUIRE(wait_until([&] { return rd(f, wb.guest) == 1024 - 3; }));
    ring.put(Pm4Builder()
                 .type3(pm4::IM_LOAD_IMMEDIATE, {0, 4, 0xA0000001, 0xA0000002, 0xA0000003, 0xA0000004})
                 .type3(pm4::MEM_WRITE, {(fence.phys + 4) | pm4::kEndian8in32, 0x55667788}));
    ring.submit();
    CHECK(ring.wptr < 1024 - 3);  // wrapped
    REQUIRE(wait_until([&] { return rd(f, wb.guest) == ring.wptr; }));
    CHECK(rd(f, fence.guest + 4) == 0x55667788);
    std::lock_guard<std::mutex> lock(rec->m);
    CHECK(rec->microcode == std::vector<uint32_t>{0xA0000001, 0xA0000002, 0xA0000003, 0xA0000004});
  }
  kernel().gpu().stop();
}

TEST_CASE("a malformed ring buffer is fatal, never a silent spin", "[unit][gpu]") {
  KernelFixture f;
  Standalone s(f);
  Ring ring{f, phys_alloc(0x10000), 1024};
  s.gpu->initialize_ring_buffer(ring.mem.phys, 9);

  SECTION("an incomplete packet with every dword up to WPTR available") {
    ring.put(Pm4Builder::type3_header(pm4::NOP, 10));  // promises 10 payload dwords
    ring.put(0);
    ring.submit();
    s.gpu->start();
    REQUIRE(wait_until([&] { return !s.fatal().empty(); }));
    CHECK(s.fatal().find("malformed GPU ring buffer") != std::string::npos);
    CHECK(s.fatal().find("RPTR 0x0000, WPTR 0x0002") != std::string::npos);
  }
  SECTION("an unknown opcode") {
    ring.put(Pm4Builder::type3_header(0x7E, 1));
    ring.put(0);
    ring.submit();
    s.gpu->start();
    REQUIRE(wait_until([&] { return !s.fatal().empty(); }));
    CHECK(s.fatal().find("unknown PM4 opcode 0x7E") != std::string::npos);
    CHECK(s.fatal().find("WPTR 0x0002") != std::string::npos);
  }
  SECTION("a WPTR outside the ring") {
    wr(f, lr::gpu::mmio_register_address(lr::gpu::CP_RB_WPTR), 5000);
    s.gpu->start();
    REQUIRE(wait_until([&] { return !s.fatal().empty(); }));
    CHECK(s.fatal().find("CP_RB_WPTR 0x00001388 lies outside the ring of 1024 dwords") != std::string::npos);
  }
  s.gpu->stop();
  wr(f, lr::gpu::mmio_register_address(lr::gpu::CP_RB_WPTR), 0);
}

TEST_CASE("stop cancels a spinning WAIT_REG_MEM", "[unit][gpu]") {
  KernelFixture f;
  Standalone s(f);
  Ring ring{f, phys_alloc(0x10000), 1024};
  const Phys poll = phys_alloc(0x10000);
  s.gpu->initialize_ring_buffer(ring.mem.phys, 9);
  // A fence, then a wait until memory == 1 (function 3 "equal", memory space): never true.
  ring.put(Pm4Builder()
               .type3(pm4::MEM_WRITE, {(poll.phys + 4) | pm4::kEndian8in32, 0xF00D})
               .type3(pm4::WAIT_REG_MEM, {0x13, poll.phys, 1, 0xFFFFFFFF, 1}));
  ring.submit();
  s.gpu->start();
  REQUIRE(wait_until([&] { return rd(f, poll.guest + 4) == 0xF00D; }));  // the GPU thread reached the wait
  s.gpu->stop();  // returns: the wait was cancelled
  CHECK(s.fatal().empty());
  CHECK(s.gpu->read_pointer() == 0);
  wr(f, lr::gpu::mmio_register_address(lr::gpu::CP_RB_WPTR), 0);
}

TEST_CASE("an unknown PM4 opcode in the ring is guest fatal (exit 5)", "[unit][gpu]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    {
      KernelFixture f;
      Ring ring{f, phys_alloc(0x10000), 1024};
      f.call("VdInitializeRingBuffer", ring.mem.phys, 9u);
      ring.put(Pm4Builder::type3_header(0x7D, 1));
      ring.put(0);
      ring.submit();
      std::this_thread::sleep_for(10s);  // the GPU thread ends the process long before
    }
    ::_exit(99);
  }
  ::close(fds[1]);
  std::string out;
  char buf[256];
  for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
  ::close(fds[0]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 5);
  CHECK(out.find("Guest fatal: GPU command processor: unknown PM4 opcode 0x7D") != std::string::npos);
}

// The callback runs on the CPU the interrupt targets (its KPCR current_cpu): vblanks on CPU 2 and a PM4 INTERRUPT on
// each CPU of its mask, as Xenia. D3D's callback clears the bit of the CPU it runs on from the mask the GPU then
// waits on (WAIT_REG_MEM for 0), so a wrong CPU deadlocks the GPU.
TEST_CASE("vblank interrupt reaches the guest callback", "[unit][gpu]") {
  // Declared before the fixture: the kernel worker may still run the callback until the fixture is gone.
  std::mutex m;
  std::vector<std::tuple<uint64_t, uint64_t, uint32_t>> calls;  // (source, user_data, KPCR current_cpu)
  KernelFixture f;  // Realtime60Hz (no vblank_paced_by_swap)
  def(kCallback, [&](const std::vector<uint64_t>& a) -> uint64_t {
    const GuestThread* t = current_thread();
    const uint32_t cpu = t ? f.mem.translate<X_KPCR>(t->pcr())->current_cpu : 0xFFu;
    std::lock_guard<std::mutex> lock(m);
    calls.emplace_back(a.at(0), a.at(1), cpu);
    return 0;
  });
  auto count = [&](uint64_t source) {
    std::lock_guard<std::mutex> lock(m);
    size_t n = 0;
    for (const auto& c : calls) n += std::get<0>(c) == source && std::get<1>(c) == 0x40055980 ? 1 : 0;
    return n;
  };
  f.call("VdSetGraphicsInterruptCallback", kCallback, 0x40055980u);
  REQUIRE(wait_until([&] { return count(lr::gpu::kInterruptSourceVblank) >= 3; }));

  // A PM4 INTERRUPT with CPU mask 0b101 raises two command-processor interrupts (source 1), on CPUs 0 and 2.
  Ring ring{f, phys_alloc(0x10000), 1024};
  f.call("VdInitializeRingBuffer", ring.mem.phys, 9u);
  ring.put(Pm4Builder().type3(pm4::INTERRUPT, {0x5}));
  ring.submit();
  REQUIRE(wait_until([&] { return count(lr::gpu::kInterruptSourceCommand) == 2; }));
  CHECK(kernel().gpu().interrupts_delivered(lr::gpu::kInterruptSourceCommand) == 2);
  kernel().gpu().stop();
  {
    std::lock_guard<std::mutex> lock(m);
    std::vector<uint32_t> command_cpus;
    for (const auto& c : calls) {
      CHECK(std::get<1>(c) == 0x40055980);
      if (std::get<0>(c) == lr::gpu::kInterruptSourceVblank) CHECK(std::get<2>(c) == 2u);
      if (std::get<0>(c) == lr::gpu::kInterruptSourceCommand) command_cpus.push_back(std::get<2>(c));
    }
    CHECK(command_cpus == std::vector<uint32_t>{0, 2});
  }
}

TEST_CASE("paced vblank follows swaps", "[unit][gpu]") {
  lr::kernel::KernelConfig cfg;
  cfg.vblank_paced_by_swap = true;
  std::atomic<uint64_t> vblanks{0};  // before the fixture: see above
  KernelFixture f(cfg);
  def(kCallback, [&](const std::vector<uint64_t>& a) -> uint64_t {
    if (a.at(0) == lr::gpu::kInterruptSourceVblank) ++vblanks;
    return 0;
  });

  SECTION("one vblank per processed swap") {
    kernel().gpu().set_paced_interval(1h);  // no wall-time fallback: only swaps make vblanks
    Ring ring{f, phys_alloc(0x10000), 1024};
    const Phys fb = phys_alloc(0x10000);
    const uint32_t args = f.alloc(0x40);
    const uint32_t fetch = args, fb_ptr = args + 0x20, width = args + 0x24, height = args + 0x28;
    wr(f, fetch + 4, (fb.guest & ~0xFFFu) | 0xABC);  // dword 1: low 12 bits kept, base address -> physical
    wr(f, fb_ptr, fb.guest);
    wr(f, width, 1280);
    wr(f, height, 720);
    f.call("VdSetGraphicsInterruptCallback", kCallback, 0u);
    f.call("VdInitializeRingBuffer", ring.mem.phys, 9u);
    for (int frame = 0; frame < 3; ++frame) {
      const uint32_t area = ring.mem.guest + ring.wptr * 4;
      f.call("VdSwap", area, fetch, 0u, 0u, 0u, fb_ptr, 0u, 0u, width, height);
      if (frame == 0) {
        // The reserved 64 dwords: fetch constant (type 0, 6 dwords), then LR_SWAP covering the rest.
        CHECK(rd(f, area) == ((5u << 16) | lr::gpu::SHADER_CONSTANT_FETCH_00_0));
        CHECK(rd(f, area + 8) == ((fb.phys & ~0xFFFu) | 0xABC));
        CHECK(rd(f, area + 28) == Pm4Builder::type3_header(pm4::LR_SWAP, 56));
        CHECK(rd(f, area + 32) == pm4::kSwapMagic);
        CHECK(rd(f, area + 36) == fb.phys);
        CHECK(rd(f, area + 40) == 1280);
        CHECK(rd(f, area + 44) == 720);
        for (uint32_t i = 12; i < 64; ++i) CHECK(rd(f, area + 4 * i) == 0x80000000);  // NOP fill as payload
      }
      for (int i = 0; i < 64; ++i) ring.put(rd(f, area + 4 * static_cast<uint32_t>(i)));
      ring.submit();
    }
    REQUIRE(wait_until([&] { return kernel().gpu().swaps() == 3; }));
    REQUIRE(wait_until([&] { return kernel().gpu().interrupts_delivered(lr::gpu::kInterruptSourceVblank) == 3; }));
    CHECK(vblanks == 3);
  }
  SECTION("without swaps the 16 ms wall-time minimum still brings vblanks") {
    f.call("VdSetGraphicsInterruptCallback", kCallback, 0u);
    REQUIRE(wait_until([&] { return vblanks >= 3; }));
  }
  kernel().gpu().stop();
}

TEST_CASE("Vd* query imports and GPU variables", "[unit][gpu]") {
  KernelFixture f;
  CHECK(f.call("VdInitializeEngines", 0x52B50000u, 0x82445B00u, 0u, 0u) == 1);
  const uint32_t p0 = f.alloc(0x100), p1 = f.alloc(4);
  wr(f, p0 + 0x90, 0x12345678);
  f.call("VdGetSystemCommandBuffer", p0, p1);
  CHECK(rd(f, p0) == 0xBEEF0000);
  CHECK(rd(f, p0 + 0x90) == 0);
  CHECK(rd(f, p1) == 0xBEEF0001);
  const uint32_t out = f.alloc(4);
  CHECK(f.call("VdPersistDisplay", 0u, out) == 1);
  CHECK(rd(f, out) >= 0xA0000000);
  const uint32_t dest = f.alloc(16);
  CHECK(f.call("VdInitializeScalerCommandBuffer", 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, dest, 3u) == 3);
  CHECK(rd(f, dest + 8) == 0x80000000);
  CHECK(rd(f, dest + 12) == 0);
  CHECK(f.call("MmMapIoSpace", 2u, 0x7FC80000u, 0x40u, 0x404u) == 0x7FC80000);
  for (const char* v : {"VdGlobalDevice", "VdGlobalXamDevice", "VdGpuClockInMHz", "VdHSIOCalibrationLock",
                        "KeCertMonitorData"})
    CHECK(variable_state(v) == ImportState::Implemented);
}

// A front buffer outside the physical views has no physical address for the GPU: guest fatal (exit 5) naming it.
TEST_CASE("VdSwap with a non-physical front buffer is guest fatal", "[unit][gpu]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::dup2(fds[1], 2);
    ::close(fds[0]);
    {
      KernelFixture f;
      const uint32_t area = f.alloc(64 * 4), args = f.alloc(0x40);
      wr(f, args + 0x20, 0x40000000);  // *frontbuffer: a virtual heap address
      wr(f, args + 0x24, 1280);
      wr(f, args + 0x28, 720);
      f.call("VdSwap", area, args, 0u, 0u, 0u, args + 0x20, 0u, 0u, args + 0x24, args + 0x28);
    }
    ::_exit(99);
  }
  ::close(fds[1]);
  std::string out;
  char buf[256];
  for (ssize_t n; (n = ::read(fds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
  ::close(fds[0]);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 5);
  CHECK(out.find("Guest fatal: VdSwap: front buffer 0x40000000 is not in a physical view") != std::string::npos);
}
