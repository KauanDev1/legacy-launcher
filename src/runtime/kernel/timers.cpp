// SPDX-License-Identifier: MIT
// Timer semantics follow Xenia's XTimer (xtimer.cc; semantic reference only, no code copied): negative due time =
// relative 100 ns, otherwise an absolute FILETIME; the APC goes to the thread that set the timer, called with
// (context, system time low, system time high). DPC objects follow Xenia's XDPC (xboxkrnl_threading.cc).
#include "runtime/kernel/timers.h"

#include <time.h>
#include <x86intrin.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "runtime/guest_thread.h"
#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/threads.h"

namespace lr::kernel {

namespace {

constexpr int64_t kUnixEpochFiletime = 116444736000000000;  // FILETIME of 1970-01-01
constexpr int64_t kTicksPerDay = 864000000000;              // 100 ns ticks
constexpr uint32_t kWorkerStackSize = 64 * 1024;
constexpr uint8_t kDispatchLevel = 2;

thread_local uint8_t t_irql = 0;

int64_t monotonic_ns() {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t{ts.tv_sec} * 1'000'000'000 + ts.tv_nsec;
}

// Days since 1970-01-01 of a proleptic Gregorian date (era-based: 400-year eras of 146097 days, March-based years).
int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int32_t& y, int32_t& m, int32_t& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int32_t>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int32_t>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int32_t>(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

bool is_leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int32_t days_in_month(int32_t y, int32_t m) {
  static constexpr int32_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && is_leap(y) ? 29 : kDays[m - 1];
}

int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0 && (a < 0) != (b < 0)) ? 1 : 0); }

std::atomic_ref<uint32_t> guest_word(uint8_t* base, uint32_t addr) {
  return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + addr));
}

void store_be64(uint8_t* p, uint64_t v) {
  std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(p)).store(__builtin_bswap64(v), std::memory_order_relaxed);
}
void store_be32(uint8_t* p, uint32_t v) {
  std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(p)).store(__builtin_bswap32(v), std::memory_order_relaxed);
}

}  // namespace

// --- Clocks and calendar ----------------------------------------------------------------------------------------

namespace {

// One CLOCK_MONOTONIC read bracketed by two TSC reads: the TSC value at the clock read is taken as the bracket's
// midpoint, and its width bounds the error.
struct Stamp {
  uint64_t tsc, width;
  int64_t ns;
};
Stamp stamp() {
  const uint64_t a = __rdtsc();
  const int64_t ns = monotonic_ns();
  const uint64_t b = __rdtsc();
  return {a + (b - a) / 2, b - a, ns};
}

}  // namespace

uint64_t calibrate_tsc_hz() {
  constexpr int kSamples = 3;
  constexpr long kSampleNs = 50'000'000 / kSamples;  // the three samples share the 50 ms budget
  uint64_t best_hz = 0, best_width = UINT64_MAX;
  for (int i = 0; i < kSamples; ++i) {
    const Stamp s0 = stamp();
    timespec req{0, kSampleNs};
    while (::nanosleep(&req, &req) != 0) {
    }
    const Stamp s1 = stamp();
    const int64_t ns = s1.ns > s0.ns ? s1.ns - s0.ns : 1;
    const uint64_t width = s0.width + s1.width;
    if (width < best_width) {
      best_width = width;
      best_hz = static_cast<uint64_t>(static_cast<unsigned __int128>(s1.tsc - s0.tsc) * 1'000'000'000u /
                                      static_cast<uint64_t>(ns));
    }
  }
  return best_hz;
}

uint64_t process_tsc_hz() {
  static const uint64_t hz = calibrate_tsc_hz();
  return hz;
}

int64_t host_filetime_now() {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return int64_t{ts.tv_sec} * 10'000'000 + ts.tv_nsec / 100 + kUnixEpochFiletime;
}

TimeFields filetime_to_fields(int64_t filetime) {
  const int64_t days1601 = floor_div(filetime, kTicksPerDay);
  int64_t rem = filetime - days1601 * kTicksPerDay;  // [0, kTicksPerDay)
  const int64_t unix_days = days1601 - kUnixEpochFiletime / kTicksPerDay;
  TimeFields f{};
  civil_from_days(unix_days, f.year, f.month, f.day);
  f.hour = static_cast<int32_t>(rem / 36'000'000'000);
  rem %= 36'000'000'000;
  f.minute = static_cast<int32_t>(rem / 600'000'000);
  rem %= 600'000'000;
  f.second = static_cast<int32_t>(rem / 10'000'000);
  rem %= 10'000'000;
  f.milliseconds = static_cast<int32_t>(rem / 10'000);
  f.weekday = static_cast<int32_t>(((unix_days % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday
  return f;
}

std::optional<int64_t> fields_to_filetime(const TimeFields& f) {
  if (f.year < 1601 || f.year > 30827 || f.month < 1 || f.month > 12 || f.day < 1 ||
      f.day > days_in_month(f.year, f.month) || f.hour < 0 || f.hour > 23 || f.minute < 0 || f.minute > 59 ||
      f.second < 0 || f.second > 59 || f.milliseconds < 0 || f.milliseconds > 999)
    return std::nullopt;
  const int64_t days = days_from_civil(f.year, f.month, f.day) + kUnixEpochFiletime / kTicksPerDay;
  const int64_t secs = int64_t{f.hour} * 3600 + int64_t{f.minute} * 60 + f.second;
  return days * kTicksPerDay + secs * 10'000'000 + int64_t{f.milliseconds} * 10'000;
}

uint8_t current_irql() { return t_irql; }
void set_current_irql(uint8_t irql) { t_irql = irql; }

// --- Timers -----------------------------------------------------------------------------------------------------

Timers::Timers(Kernel& k) : kernel_(k), start_(Clock::now()) {}

Timers::~Timers() { shutdown(); }

int32_t Timers::set_timer(uint32_t header, Clock::time_point due, uint32_t period_ms, uint32_t apc_routine,
                          uint32_t apc_context, std::weak_ptr<GuestThread> apc_thread) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto word = guest_word(kernel_.memory().base(), header + 4);
  const int32_t prev = static_cast<int32_t>(__builtin_bswap32(word.exchange(0, std::memory_order_seq_cst)));
  timers_[header] = Armed{due, period_ms, apc_routine, apc_context, std::move(apc_thread)};
  ensure_worker_locked();
  cv_.notify_all();
  return prev;
}

bool Timers::cancel_timer(uint32_t header) {
  std::lock_guard<std::mutex> lock(mutex_);
  return timers_.erase(header) != 0;
}

bool Timers::queue_dpc(uint32_t dpc, uint32_t arg1, uint32_t arg2) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!queued_dpcs_.insert(dpc).second) return false;
  auto* d = reinterpret_cast<X_KDPC*>(kernel_.memory().base() + dpc);
  d->arg1 = arg1;
  d->arg2 = arg2;
  dpcs_.push_back(dpc);
  ensure_worker_locked();
  cv_.notify_all();
  return true;
}

void Timers::enable_time_stamp_bundle(uint32_t addr) {
  std::lock_guard<std::mutex> lock(mutex_);
  fill_time_stamp_bundle(kernel_.memory().base() + addr);
  bundle_ = addr;
  ensure_worker_locked();
  cv_.notify_all();
}

void Timers::fill_time_stamp_bundle(uint8_t* host) const {
  const auto up = Clock::now() - start_;
  const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(up).count();
  store_be64(host + offsetof(X_TIME_STAMP_BUNDLE, interrupt_time), static_cast<uint64_t>(ns / 100));
  store_be64(host + offsetof(X_TIME_STAMP_BUNDLE, system_time), static_cast<uint64_t>(host_filetime_now()));
  store_be32(host + offsetof(X_TIME_STAMP_BUNDLE, tick_count), static_cast<uint32_t>(ns / 1'000'000));
}

bool Timers::has_pending_work() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !timers_.empty() || !dpcs_.empty() || dpc_running_;
}

void Timers::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stop_) return;
    stop_ = true;
    cv_.notify_all();
  }
  ::pthread_join(worker_, nullptr);
  worker_thread_.reset();  // frees the worker's guest stack and KTHREAD/KPCR block
}

void Timers::ensure_worker_locked() {
  if (started_ || stop_) return;
  auto t = create_internal_guest_thread(kWorkerStackSize);
  if (!t) guest_fatal("cannot create the kernel worker thread: " + t.error().message);
  worker_thread_ = std::move(t.value());
  pthread_attr_t attr;
  ::pthread_attr_init(&attr);
  ::pthread_attr_setstacksize(&attr, runtime::kHostStackSize);
  const int rc = ::pthread_create(&worker_, &attr, &Timers::worker_main, this);
  ::pthread_attr_destroy(&attr);
  if (rc != 0) guest_fatal("cannot start the kernel worker thread: pthread_create error " + std::to_string(rc));
  started_ = true;
}

void* Timers::worker_main(void* self) {
  auto* timers = static_cast<Timers*>(self);
  timers->worker_thread_->run_internal(&Timers::worker_body, self);
  return nullptr;
}

void Timers::worker_body(void* self) { static_cast<Timers*>(self)->loop(); }

void Timers::fire_locked(uint32_t header, Armed& a, Clock::time_point now) {
  uint8_t* const base = kernel_.memory().base();
  guest_word(base, header + 4).store(__builtin_bswap32(1u), std::memory_order_seq_cst);
  kernel_.dispatcher().signal(header);
  if (a.apc_routine != 0) {
    if (auto t = a.apc_thread.lock()) {
      const auto ft = static_cast<uint64_t>(host_filetime_now());
      t->queue_apc({a.apc_routine, a.apc_context, static_cast<uint32_t>(ft), static_cast<uint32_t>(ft >> 32)});
    }
  }
  if (a.period_ms != 0) {
    a.due += std::chrono::milliseconds(a.period_ms);
    if (a.due <= now) a.due = now + std::chrono::milliseconds(a.period_ms);  // missed periods are skipped
  }
}

void Timers::loop() {
  set_current_irql(kDispatchLevel);
  uint8_t* const base = kernel_.memory().base();
  std::unique_lock<std::mutex> lock(mutex_);
  Clock::time_point next_bundle = Clock::now();
  while (!stop_) {
    const Clock::time_point now = Clock::now();
    for (auto it = timers_.begin(); it != timers_.end();) {
      if (it->second.due > now) {
        ++it;
        continue;
      }
      fire_locked(it->first, it->second, now);
      it = it->second.period_ms == 0 ? timers_.erase(it) : std::next(it);
    }
    if (bundle_ != 0 && now >= next_bundle) {
      fill_time_stamp_bundle(base + bundle_);
      next_bundle = now + std::chrono::milliseconds(1);
    }
    if (!dpcs_.empty()) {
      // Dequeued before the routine runs, so the routine (or anyone) can queue it again.
      const uint32_t dpc = dpcs_.front();
      dpcs_.pop_front();
      queued_dpcs_.erase(dpc);
      const auto* d = reinterpret_cast<const X_KDPC*>(base + dpc);
      const uint64_t args[] = {dpc, d->context, d->arg1, d->arg2};
      const uint32_t routine = d->routine;
      if (routine == 0) guest_fatal("kernel worker: queued DPC has no routine (routine 0, cleared after insertion)");
      dpc_running_ = true;
      lock.unlock();
      (void)call_guest(routine, args);
      lock.lock();
      dpc_running_ = false;
      continue;
    }
    std::optional<Clock::time_point> next;
    for (const auto& [header, a] : timers_)
      if (!next || a.due < *next) next = a.due;
    if (bundle_ != 0 && (!next || next_bundle < *next)) next = next_bundle;
    if (next) cv_.wait_until(lock, *next);
    else cv_.wait(lock);
  }
}

}  // namespace lr::kernel
