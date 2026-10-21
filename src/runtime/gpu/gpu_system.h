// SPDX-License-Identifier: MIT
// The emulated GPU behind the kernel's Vd* imports (spec section 5.2): a host "GPU" thread consumes the guest's
// ring buffer through the PM4 CommandProcessor and writes the read pointer back, and a vblank pacer raises the
// graphics interrupts the game's callback expects. Semantics per Xenia graphics_system.cc / command_processor.cc
// (reference only; no code taken from them). Pure GPU core: no Vulkan types, no kernel includes (the kernel wires
// it through GpuHooks).
//
// Ring buffer. VdInitializeRingBuffer(phys, size_log2) gives a ring of 2^(size_log2 + 3) bytes (2^size_log2
// quadwords) at physical `phys`. The game writes packets and then the write pointer, a dword index, to CP_RB_WPTR in
// the MMIO window (runtime/gpu/mmio.h); the GPU thread polls it, executes [RPTR, WPTR), and after each step writes
// RPTR (dword index, big-endian) to the write-back address (VdEnableRingBufferRPtrWriteBack, physical) and to
// CP_RB_RPTR in the MMIO window.
//
// Wrap and linearisation. CommandProcessor::execute takes one contiguous span, so the reader passes [RPTR, WPTR) or,
// when the ring wraps, [RPTR, ring end). A packet that straddles the end is linearised: the tail [RPTR, end) and the
// head [0, WPTR) are copied, in order, into a scratch area of the ring's size that lives in guest memory (allocated
// through GpuHooks::alloc_guest when the ring is initialised), and the copy is executed there. Guest addresses the
// processor derives from the packet (IM_LOAD_IMMEDIATE's microcode) therefore stay valid guest addresses for the
// duration of the call; packets after the straddling one in the copied head run from the copy too, and RPTR moves
// past everything the copy consumed.
//
// Malformed rings. A step that consumes nothing although every dword up to WPTR was handed to the processor (no wrap
// cut, or the linearised copy) can never make progress: GpuHooks::fatal("... RPTR ... WPTR ..."), which the kernel
// turns into guest_fatal (exit 5). So do execute() errors (unknown opcode, malformed packet) and a WPTR outside the
// ring. A step cut short by the wrap only waits for the linearisation; nothing ever spins silently.
//
// Interrupts. The game's callback(source, user_data) (VdSetGraphicsInterruptCallback) gets source 0 for a vblank
// and source 1 for a command-processor interrupt (PM4 INTERRUPT), Xenia's convention (graphics_system.cc /
// command_processor.cc). Xenia also selects the guest CPU that runs it (vblank on CPU 2, INTERRUPT per mask bit);
// we do not model CPUs, so the CPU index is dropped (known gap). Delivery goes through GpuHooks::queue_interrupt
// (the kernel worker, call_guest), one interrupt in flight at a time and in raise order; without a callback an
// interrupt is dropped, as Xenia does.
//
// Vblank pacing (spec section 11). Realtime60Hz (window): a vblank every 1/60 s of wall time; one already waiting
// for delivery absorbs the next (a missed vblank, as on hardware). PacedBySwap (headless default): one vblank per
// processed swap, none dropped, plus a fallback vblank once paced_interval (16 ms) of wall time has passed since the
// last vblank and since the backend last worked (coalesced like the realtime ones, so a slow callback cannot
// make the queue grow). A slow backend (software rendering) therefore gets about one vblank
// per frame instead of a stream of them, and a game waiting for vblanks before its first swap still gets them.
//
// --trace-gpu (enable_trace): one line per PM4 packet ("pm4 type3 DRAW_INDX_2 dwords=3"), per draw
// ("draw prim=4 count=3"), per resolve and per swap ("swap 1 fb=0x... 1280x720"), and once per second
// "stats draws=N resolves=N textures=N swaps=N" (counts of that second). Written by the GPU thread, flushed after
// every step before RPTR is written back (so a guest that saw RPTR advance also sees the lines on disk), on stop and on
// the kernel's exit paths (flush_trace through kernel/trace.h's flush hook);
// capped at 64 MiB (a final "gpu trace capped at 64 MiB" line).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "common/result.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"

namespace lr::gpu {

enum class VblankMode { Realtime60Hz, PacedBySwap };

inline constexpr uint32_t kInterruptSourceVblank = 0;
inline constexpr uint32_t kInterruptSourceCommand = 1;

inline constexpr uint64_t kGpuTraceCapBytes = 64ull << 20;

// What GpuSystem needs from its host (the kernel; tests supply their own).
struct GpuHooks {
  // Runs guest `callback(source, user_data)` asynchronously (kernel: on the kernel worker through call_guest) and
  // then `done`, on the same thread, once the callback returned.
  std::function<void(uint32_t callback, uint32_t source, uint32_t user_data, std::function<void()> done)>
      queue_interrupt;
  // `bytes` of committed read/write guest memory (the linearisation scratch); 0 on failure. Called from the thread
  // that initialises the ring (a guest thread in VdInitializeRingBuffer).
  std::function<uint32_t(uint32_t bytes)> alloc_guest;
  // An unrecoverable guest error (kernel: guest_fatal, exit 5; never returns there). If it returns (tests), the GPU
  // thread stops.
  std::function<void(const std::string& what)> fatal;
};

class GpuSystem {
 public:
  GpuSystem(MemoryAccess mem, GpuHooks hooks);
  ~GpuSystem();  // stop()
  GpuSystem(const GpuSystem&) = delete;
  GpuSystem& operator=(const GpuSystem&) = delete;

  // VdInitializeRingBuffer: ring at physical `phys`, 2^(size_log2 + 3) bytes; RPTR restarts at 0. A ring that maps
  // to no guest memory, or a size_log2 above 20 (8 MiB), is fatal (GpuHooks::fatal).
  void initialize_ring_buffer(uint32_t phys, uint32_t size_log2);
  // VdEnableRingBufferRPtrWriteBack: RPTR is stored at physical `phys` after every step. block_log2 (the
  // write-back granularity) is accepted and ignored: we write back after every step, which is more often.
  void enable_rptr_writeback(uint32_t phys, uint32_t block_log2);
  // VdSetGraphicsInterruptCallback. fn 0 disables delivery.
  void set_interrupt_callback(uint32_t fn, uint32_t user_data);
  // The backend the command processor drives (default: NullBackend). Call it before start().
  void set_backend(std::unique_ptr<Backend> backend);
  // Starts the GPU thread and the vblank pacer. Idempotent; a no-op after stop().
  void start();
  // Stops both threads (a spinning WAIT_REG_MEM is cancelled) and flushes the trace. Idempotent.
  void stop();
  void set_vblank_mode(VblankMode mode);
  // PacedBySwap fallback interval (default 16 ms). Tests raise it to see only the per-swap vblanks.
  void set_paced_interval(std::chrono::nanoseconds interval);
  // --trace-gpu: to `file` (truncated, parent directories created) or stderr with nullopt. Before start().
  Result<void> enable_trace(const std::optional<std::filesystem::path>& file);
  // Writes the buffered trace lines (no-op without a trace). Any thread; the kernel calls it on its exit paths.
  void flush_trace();

  // Swaps the backend processed (LR_SWAP packets executed).
  uint64_t swaps() const { return swaps_.load(std::memory_order_acquire); }
  // Interrupts whose guest callback returned, per source (0 vblank, 1 command processor).
  uint64_t interrupts_delivered(uint32_t source) const;
  // The current read pointer (dword index).
  uint32_t read_pointer() const { return rptr_.load(std::memory_order_acquire); }

 private:
  class BackendProxy;
  class Trace;
  using Clock = std::chrono::steady_clock;

  void gpu_main();
  void vblank_main();
  // One step over [rptr, wptr): false when the thread must stop (fatal or stop request).
  // `generation` is ring_generation_ at the snapshot: a re-initialised ring drops this step's RPTR.
  bool step(uint32_t generation, uint32_t ring_guest, uint32_t ring_dwords, uint32_t scratch, uint32_t wptr);
  // RPTR to CP_RB_RPTR in the MMIO window and to physical `phys` (0: no write-back).
  void write_back(uint32_t rptr, uint32_t phys);
  void fail(const std::string& what);
  void raise_interrupt(uint32_t source, bool coalesce);
  void on_interrupt_done(uint32_t source);
  void on_swap();
  uint32_t read_wptr() const;

  MemoryAccess mem_;
  GpuHooks hooks_;
  RegisterFile regs_;
  std::unique_ptr<BackendProxy> proxy_;
  std::unique_ptr<Trace> trace_;
  CommandProcessor cp_;

  // Ring state, set by guest threads and read by the GPU thread.
  std::mutex ring_mutex_;
  uint32_t ring_phys_ = 0, ring_guest_ = 0, ring_dwords_ = 0, scratch_ = 0, scratch_bytes_ = 0;
  uint32_t ring_generation_ = 0;  // bumped by initialize_ring_buffer: the GPU thread restarts at RPTR 0
  uint32_t writeback_phys_ = 0;
  std::atomic<uint32_t> rptr_{0};

  // Threads.
  std::mutex thread_mutex_;
  std::condition_variable gpu_cv_;
  std::condition_variable vblank_cv_;
  bool started_ = false;
  std::atomic<bool> stop_{false};  // written under thread_mutex_ (no lost wake-ups), read anywhere
  std::thread gpu_thread_;
  std::thread vblank_thread_;

  // Interrupts (interrupt_mutex_).
  std::mutex interrupt_mutex_;
  uint32_t callback_ = 0, callback_data_ = 0;
  std::deque<uint32_t> pending_;
  bool in_flight_ = false;
  std::atomic<uint64_t> delivered_[2]{};

  // Vblank pacing (thread_mutex_ for mode/interval/owed; the atomics are written by the GPU thread).
  VblankMode mode_ = VblankMode::Realtime60Hz;
  std::chrono::nanoseconds paced_interval_ = std::chrono::milliseconds(16);
  uint64_t swap_vblanks_owed_ = 0;
  std::atomic<bool> backend_busy_{false};
  std::atomic<int64_t> backend_idle_since_ns_{0};  // Clock time of the last backend call's end

  std::atomic<uint64_t> swaps_{0};
};

}  // namespace lr::gpu
