// SPDX-License-Identifier: MIT
// Semantics per Xenia graphics_system.cc / command_processor.cc (reference only; no code taken from them): ring size
// 2^(size_log2 + 3) bytes, WPTR/RPTR as dword indices, RPTR written back big-endian, interrupt sources 0 (vblank)
// and 1 (PM4 INTERRUPT). The design notes are in gpu_system.h.
#include "runtime/gpu/gpu_system.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <utility>

#include "runtime/gpu/mmio.h"
#include "runtime/gpu/null_backend.h"

namespace lr::gpu {

namespace {

constexpr uint32_t kMaxRingSizeLog2 = 20;  // 8 MiB: far above anything a title uses
constexpr std::chrono::nanoseconds kRealtimePeriod{16'666'667};
constexpr uint32_t kIdleSpins = 200;

std::string hex(uint32_t v, int digits = 8) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%0*X", digits, v);
  return b;
}

int64_t to_ns(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

}  // namespace

// --- Trace ------------------------------------------------------------------------------------------------------

class GpuSystem::Trace {
 public:
  Trace(FILE* f, bool owns) : f_(f), owns_(owns), next_stats_(Clock::now() + std::chrono::seconds(1)) {
    if (owns_) std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
  }
  ~Trace() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fflush(f_);
    if (owns_) std::fclose(f_);
  }

  __attribute__((format(printf, 2, 3))) void line(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<size_t>(n) >= sizeof buf) n = sizeof buf - 1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (capped_) return;
    if (bytes_ + static_cast<uint64_t>(n) > kGpuTraceCapBytes) {
      capped_ = true;
      std::fputs("gpu trace capped at 64 MiB\n", f_);
      std::fflush(f_);
      return;
    }
    bytes_ += static_cast<uint64_t>(n);
    std::fwrite(buf, 1, static_cast<size_t>(n), f_);
  }

  void flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fflush(f_);
  }

  // Once per second of wall time: the counts of that second.
  void maybe_stats(Clock::time_point now, uint64_t draws, uint64_t resolves, uint64_t textures, uint64_t swaps) {
    if (now < next_stats_) return;
    next_stats_ = now + std::chrono::seconds(1);
    line("stats draws=%llu resolves=%llu textures=%llu swaps=%llu\n",
         static_cast<unsigned long long>(draws - last_[0]), static_cast<unsigned long long>(resolves - last_[1]),
         static_cast<unsigned long long>(textures - last_[2]), static_cast<unsigned long long>(swaps - last_[3]));
    last_[0] = draws;
    last_[1] = resolves;
    last_[2] = textures;
    last_[3] = swaps;
  }

 private:
  std::mutex mutex_;
  FILE* f_;
  bool owns_;
  uint64_t bytes_ = 0;
  bool capped_ = false;
  Clock::time_point next_stats_;  // GPU thread only
  uint64_t last_[4] = {};
};

// --- Backend proxy: counts, traces and times every backend call for the vblank pacer --------------------------

class GpuSystem::BackendProxy final : public Backend {
 public:
  explicit BackendProxy(GpuSystem& owner) : owner_(owner), inner_(std::make_unique<NullBackend>()) {}

  void set_inner(std::unique_ptr<Backend> b) { inner_ = std::move(b); }

  void load_shader(ShaderStage stage, uint32_t guest_addr, uint32_t dwords) override {
    Busy b(owner_);
    inner_->load_shader(stage, guest_addr, dwords);
  }
  void draw(const RegisterFile& regs, const DrawInfo& info) override {
    ++draws;
    if (owner_.trace_) owner_.trace_->line("draw prim=%u count=%u\n", info.prim_type, info.index_count);
    Busy b(owner_);
    inner_->draw(regs, info);
  }
  void resolve(const RegisterFile& regs) override {
    ++resolves;
    if (owner_.trace_) owner_.trace_->line("resolve\n");
    Busy b(owner_);
    inner_->resolve(regs);
  }
  void swap(uint32_t frontbuffer_phys, uint32_t width, uint32_t height) override {
    {
      Busy b(owner_);
      inner_->swap(frontbuffer_phys, width, height);
    }
    const uint64_t n = owner_.swaps_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (owner_.trace_) {
      owner_.trace_->line("swap %llu fb=0x%08X %ux%u\n", static_cast<unsigned long long>(n), frontbuffer_phys, width,
                          height);
    }
    owner_.on_swap(n);
  }
  void wait_idle() override {
    Busy b(owner_);
    inner_->wait_idle();
  }
  uint64_t textures_created() const override { return inner_->textures_created(); }

  uint64_t draws = 0, resolves = 0;  // GPU thread only

 private:
  // Marks the backend busy for the pacer; on exit records when it went idle.
  struct Busy {
    GpuSystem& o;
    explicit Busy(GpuSystem& owner) : o(owner) { o.backend_busy_.store(true, std::memory_order_release); }
    ~Busy() {
      o.backend_idle_since_ns_.store(to_ns(Clock::now()), std::memory_order_release);
      o.backend_busy_.store(false, std::memory_order_release);
    }
  };

  GpuSystem& owner_;
  std::unique_ptr<Backend> inner_;
};

// --- GpuSystem --------------------------------------------------------------------------------------------------

GpuSystem::GpuSystem(MemoryAccess mem, GpuHooks hooks)
    : mem_(mem),
      hooks_(std::move(hooks)),
      proxy_(std::make_unique<BackendProxy>(*this)),
      cp_(std::move(mem), regs_, *proxy_, [this](uint32_t source, uint32_t cpu) { raise_interrupt(source, cpu, false); }) {}

GpuSystem::~GpuSystem() { stop(); }

void GpuSystem::fail(const std::string& what) {
  if (trace_) {
    trace_->line("fatal: %s\n", what.c_str());
    trace_->flush();
  }
  if (hooks_.fatal) hooks_.fatal(what);
}

void GpuSystem::initialize_ring_buffer(uint32_t phys, uint32_t size_log2) {
  if (size_log2 > kMaxRingSizeLog2) {
    fail("VdInitializeRingBuffer: size_log2 " + std::to_string(size_log2) + " exceeds " +
         std::to_string(kMaxRingSizeLog2));
    return;
  }
  const uint32_t bytes = 8u << size_log2;
  const uint32_t masked = phys & pm4::kPhysicalMask;
  std::optional<uint32_t> guest = mem_.to_guest ? mem_.to_guest(masked) : std::nullopt;
  std::optional<uint32_t> last = mem_.to_guest ? mem_.to_guest(masked + bytes - 1) : std::nullopt;
  if (!guest || !last || *last != *guest + bytes - 1 || (*guest & 3) != 0 ||
      uint64_t{*guest} + bytes > mem_.size) {
    fail("VdInitializeRingBuffer: the ring at physical " + hex(phys) + " (" + std::to_string(bytes) +
         " bytes) maps to no contiguous guest memory");
    return;
  }
  uint32_t scratch = 0;
  {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (scratch_bytes_ >= bytes) scratch = scratch_;
  }
  if (scratch == 0) {
    scratch = hooks_.alloc_guest ? hooks_.alloc_guest(bytes) : 0;
    if (scratch == 0) {
      fail("VdInitializeRingBuffer: cannot allocate the " + std::to_string(bytes) + "-byte ring linearisation scratch");
      return;
    }
  }
  std::lock_guard<std::mutex> lock(ring_mutex_);
  if (scratch != scratch_) {
    scratch_ = scratch;
    scratch_bytes_ = bytes;
  }
  ring_phys_ = phys;
  ring_guest_ = *guest;
  ring_dwords_ = bytes / 4;
  ++ring_generation_;
  rptr_.store(0, std::memory_order_release);
}

void GpuSystem::enable_rptr_writeback(uint32_t phys, uint32_t /*block_log2*/) {
  std::lock_guard<std::mutex> lock(ring_mutex_);
  writeback_phys_ = phys;
}

void GpuSystem::set_interrupt_callback(uint32_t fn, uint32_t user_data) {
  std::lock_guard<std::mutex> lock(interrupt_mutex_);
  callback_ = fn;
  callback_data_ = user_data;
}

void GpuSystem::set_backend(std::unique_ptr<Backend> backend) {
  if (backend) proxy_->set_inner(std::move(backend));
}

void GpuSystem::set_vblank_mode(VblankMode mode) {
  std::lock_guard<std::mutex> lock(thread_mutex_);
  mode_ = mode;
  vblank_cv_.notify_all();
}

void GpuSystem::set_paced_interval(std::chrono::nanoseconds interval) {
  std::lock_guard<std::mutex> lock(thread_mutex_);
  paced_interval_ = interval;
  vblank_cv_.notify_all();
}

Result<void> GpuSystem::enable_trace(const std::optional<std::filesystem::path>& file) {
  FILE* f = stderr;
  if (file) {
    std::error_code ec;
    if (file->has_parent_path()) std::filesystem::create_directories(file->parent_path(), ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create " + file->parent_path().string() + ": " + ec.message(), ""};
    f = std::fopen(file->c_str(), "we");
    if (f == nullptr)
      return Error{ErrorCode::IoError, "cannot open the GPU trace " + file->string() + ": " + std::strerror(errno), ""};
  }
  trace_ = std::make_unique<Trace>(f, file.has_value());
  cp_.set_packet_observer([this](uint32_t type, uint32_t opcode, uint32_t dwords) {
    if (type != pm4::kType3) {
      trace_->line("pm4 type%u dwords=%u\n", type, dwords);
      return;
    }
    const char* name = pm4::opcode_name(opcode);
    if (name != nullptr) trace_->line("pm4 type3 %s dwords=%u\n", name, dwords);
    else trace_->line("pm4 type3 0x%02X dwords=%u\n", opcode, dwords);
  });
  return {};
}

void GpuSystem::start() {
  std::lock_guard<std::mutex> lock(thread_mutex_);
  if (started_ || stop_) return;
  started_ = true;
  try {
    gpu_thread_ = std::thread([this] { gpu_main(); });
    vblank_thread_ = std::thread([this] { vblank_main(); });
  } catch (const std::system_error& e) {
    if (hooks_.fatal) hooks_.fatal(std::string("cannot start the GPU threads: ") + e.what());
  }
}

void GpuSystem::stop() {
  {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    stop_.store(true, std::memory_order_release);
    gpu_cv_.notify_all();
    vblank_cv_.notify_all();
  }
  cp_.request_stop();
  if (gpu_thread_.joinable()) gpu_thread_.join();
  if (vblank_thread_.joinable()) vblank_thread_.join();
  if (trace_) trace_->flush();
}

void GpuSystem::flush_trace() {
  if (trace_) trace_->flush();
}

uint64_t GpuSystem::interrupts_delivered(uint32_t source) const {
  return source < 2 ? delivered_[source].load(std::memory_order_acquire) : 0;
}

uint32_t GpuSystem::read_wptr() const {
  // Safe without a fence on the guest side: the recompiled code stores the packets and then WPTR with plain stores,
  // which x86 (TSO) keeps in program order as long as the compiler does not reorder them; this acquire load then
  // sees every packet dword written before the WPTR value it reads.
  const uint32_t addr = mmio_register_address(CP_RB_WPTR);
  if (mem_.base == nullptr || uint64_t{addr} + 4 > mem_.size) return 0;
  const uint32_t raw =
      std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(mem_.base + addr)).load(std::memory_order_acquire);
  return __builtin_bswap32(raw);
}

void GpuSystem::write_back(uint32_t rptr, uint32_t phys) {
  const uint32_t be = __builtin_bswap32(rptr);
  const uint32_t mmio = mmio_register_address(CP_RB_RPTR);
  if (mem_.base != nullptr && uint64_t{mmio} + 4 <= mem_.size) {
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(mem_.base + mmio)).store(be, std::memory_order_release);
  }
  if (phys == 0) return;
  std::optional<uint32_t> guest = mem_.to_guest ? mem_.to_guest(phys & pm4::kPhysicalMask) : std::nullopt;
  if (!guest || (*guest & 3) != 0 || uint64_t{*guest} + 4 > mem_.size) {
    fail("RPTR write-back address (physical " + hex(phys) + ") maps to no aligned guest memory");
    return;
  }
  std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(mem_.base + *guest)).store(be, std::memory_order_release);
}

bool GpuSystem::step(uint32_t generation, uint32_t ring_guest, uint32_t ring_dwords, uint32_t scratch,
                     uint32_t wptr) {
  const uint32_t rptr = rptr_.load(std::memory_order_acquire);
  const bool wraps = wptr < rptr;
  const uint32_t span = wraps ? ring_dwords - rptr + wptr : wptr - rptr;
  const uint32_t window = wraps ? ring_dwords - rptr : span;
  auto where = [&] {
    return "RPTR " + hex(rptr, 4) + ", WPTR " + hex(wptr, 4) + ", ring of " + std::to_string(ring_dwords) + " dwords";
  };
  auto exec = [&](uint32_t addr, uint32_t dwords) -> std::optional<uint32_t> {
    Result<uint32_t> r = cp_.execute(addr, dwords);
    if (r) return r.value();
    if (r.error().code == ErrorCode::Cancelled && stop_.load(std::memory_order_acquire)) return std::nullopt;
    fail("GPU command processor: " + r.error().message + " (" + where() + ")");
    return std::nullopt;
  };
  auto consumed = exec(ring_guest + rptr * 4, window);
  if (!consumed) return false;
  if (*consumed == 0) {
    if (window == span) {
      fail("malformed GPU ring buffer: no complete PM4 packet at RPTR although every dword up to WPTR is available (" +
           where() + ")");
      return false;
    }
    // The packet at RPTR straddles the ring end: run a linear copy of [RPTR, end) + [0, WPTR) in the scratch.
    std::memcpy(mem_.base + scratch, mem_.base + ring_guest + rptr * 4, size_t{window} * 4);
    std::memcpy(mem_.base + scratch + window * 4, mem_.base + ring_guest, size_t{wptr} * 4);
    consumed = exec(scratch, span);
    if (!consumed) return false;
    if (*consumed == 0) {
      fail("malformed GPU ring buffer: no complete PM4 packet across the ring end although every dword up to WPTR "
           "is available (" + where() + ")");
      return false;
    }
  }
  const uint32_t next = (rptr + *consumed) % ring_dwords;
  uint32_t writeback_phys;
  {
    // VdInitializeRingBuffer ran meanwhile: this step belonged to the old ring, so its RPTR is dropped (the new
    // ring starts at the RPTR 0 initialize_ring_buffer stored).
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (ring_generation_ != generation) return true;
    rptr_.store(next, std::memory_order_release);
    writeback_phys = writeback_phys_;
  }
  if (trace_) trace_->flush();  // before the guest can see RPTR move
  write_back(next, writeback_phys);
  return true;
}

void GpuSystem::gpu_main() {
  uint32_t spins = 0;
  while (!stop_.load(std::memory_order_acquire)) {
    if (trace_) {
      trace_->maybe_stats(Clock::now(), proxy_->draws, proxy_->resolves, proxy_->textures_created(), swaps());
    }
    uint32_t generation, ring_guest, ring_dwords, scratch;
    {
      std::lock_guard<std::mutex> lock(ring_mutex_);
      generation = ring_generation_;  // step() drops its result if the ring is re-initialised meanwhile
      ring_guest = ring_guest_;
      ring_dwords = ring_dwords_;
      scratch = scratch_;
    }
    bool idle = ring_dwords == 0;
    if (!idle) {
      const uint32_t wptr = read_wptr();
      if (wptr >= ring_dwords) {
        fail("GPU ring buffer: CP_RB_WPTR " + hex(wptr) + " lies outside the ring of " + std::to_string(ring_dwords) +
             " dwords (RPTR " + hex(rptr_.load(), 4) + ")");
        return;
      }
      if (wptr != rptr_.load(std::memory_order_acquire)) {
        if (!step(generation, ring_guest, ring_dwords, scratch, wptr)) return;
        spins = 0;
        continue;
      }
      idle = true;
    }
    if (spins < kIdleSpins) {
      ++spins;
      std::this_thread::yield();
    } else {
      std::unique_lock<std::mutex> lock(thread_mutex_);
      gpu_cv_.wait_for(lock, std::chrono::microseconds(500), [this] { return stop_.load(); });
    }
  }
}

void GpuSystem::on_swap(uint64_t swaps) {
  {
    std::lock_guard<std::mutex> lock(thread_mutex_);
    ++swap_vblanks_owed_;
    vblank_cv_.notify_all();
  }
  if (hooks_.on_swap) hooks_.on_swap(swaps);
}

void GpuSystem::vblank_main() {
  std::unique_lock<std::mutex> lock(thread_mutex_);
  Clock::time_point last = Clock::now();
  while (!stop_.load(std::memory_order_acquire)) {
    const Clock::time_point now = Clock::now();
    if (mode_ == VblankMode::Realtime60Hz) {
      swap_vblanks_owed_ = 0;
      const Clock::time_point due = last + kRealtimePeriod;
      if (now < due) {
        vblank_cv_.wait_until(lock, due);
        continue;
      }
      last = now - due > kRealtimePeriod ? now : due;  // missed periods are skipped
      lock.unlock();
      raise_interrupt(kInterruptSourceVblank, kVblankCpu, true);
      lock.lock();
      continue;
    }
    // PacedBySwap.
    if (swap_vblanks_owed_ > 0) {
      --swap_vblanks_owed_;
      last = now;
      lock.unlock();
      raise_interrupt(kInterruptSourceVblank, kVblankCpu, false);
      lock.lock();
      continue;
    }
    Clock::time_point due;
    if (backend_busy_.load(std::memory_order_acquire)) {
      due = now + paced_interval_;
    } else {
      const Clock::time_point idle_since{
          std::chrono::nanoseconds(backend_idle_since_ns_.load(std::memory_order_acquire))};
      due = (idle_since > last ? idle_since : last) + paced_interval_;
    }
    if (now < due) {
      vblank_cv_.wait_until(lock, due);
      continue;
    }
    last = now;
    lock.unlock();
    raise_interrupt(kInterruptSourceVblank, kVblankCpu, true);
    lock.lock();
  }
}

void GpuSystem::raise_interrupt(uint32_t source, uint32_t cpu, bool coalesce) {
  uint32_t fn, data;
  {
    std::lock_guard<std::mutex> lock(interrupt_mutex_);
    if (callback_ == 0 || !hooks_.queue_interrupt) return;
    if (in_flight_) {
      if (coalesce) {
        for (const auto& p : pending_)
          if (p.first == source && p.second == cpu) return;
      }
      pending_.emplace_back(source, cpu);
      return;
    }
    in_flight_ = true;
    fn = callback_;
    data = callback_data_;
  }
  hooks_.queue_interrupt(fn, source, data, cpu, [this, source] { on_interrupt_done(source); });
}

void GpuSystem::on_interrupt_done(uint32_t source) {
  if (source < 2) delivered_[source].fetch_add(1, std::memory_order_acq_rel);
  std::pair<uint32_t, uint32_t> next;
  uint32_t fn, data;
  {
    std::lock_guard<std::mutex> lock(interrupt_mutex_);
    if (pending_.empty() || callback_ == 0) {
      pending_.clear();
      in_flight_ = false;
      return;
    }
    next = pending_.front();
    pending_.pop_front();
    fn = callback_;
    data = callback_data_;
  }
  hooks_.queue_interrupt(fn, next.first, data, next.second, [this, s = next.first] { on_interrupt_done(s); });
}

}  // namespace lr::gpu
