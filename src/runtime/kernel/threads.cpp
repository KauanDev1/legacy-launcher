// SPDX-License-Identifier: MIT
// Semantics follow Xenia's xthread.cc / xboxkrnl_threading.cc (semantic reference only, no code copied): TLS
// template + dynamic slots after it, both KPCR tls_ptr and KTHREAD tls_address pointing at the template copy; stack
// rounded up to 4 KiB with a 16 KiB minimum; creation flag bit 0 = suspended, flags >> 24 = CPU mask; an XAPI
// startup routine is called as xapi(start, context) and the exit code is then 0, else the exit code is start's r3.
#include "runtime/kernel/threads.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "runtime/be.h"
#include "runtime/fault.h"
#include "runtime/guest_thread.h"
#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/loader.h"

namespace lr::kernel {

namespace {

using runtime::be;

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kMinStack = 0x4000;  // Xenia: 16 KiB minimum
constexpr uint32_t kKThreadOffset = 0;
constexpr uint32_t kPcrOffset = sizeof(X_KTHREAD);
constexpr uint32_t kTlsOffset = (kPcrOffset + sizeof(X_KPCR) + 0xF) & ~0xFu;
constexpr uint64_t kMaxThreadBlock = 0x01000000;  // far above the capped TLS layout (Threads::kMax*)

thread_local GuestThread* t_current = nullptr;

// Host threads still running guest-thread code or cleanup (wait_for_guest_threads_for_tests).
std::mutex& hosts_mutex() {
  static auto& m = *new std::mutex;  // leaked: threads may still finish during process exit
  return m;
}
std::condition_variable& hosts_cv() {
  static auto& cv = *new std::condition_variable;
  return cv;
}
int g_hosts = 0;  // guarded by hosts_mutex()

void host_started() {
  std::lock_guard<std::mutex> lock(hosts_mutex());
  ++g_hosts;
}
void host_finished() {
  std::lock_guard<std::mutex> lock(hosts_mutex());
  --g_hosts;
  hosts_cv().notify_all();
}

uint32_t round_up(uint32_t v, uint32_t a) { return static_cast<uint32_t>((uint64_t{v} + a - 1) / a * a); }

void store_signal_state(uint8_t* base, uint32_t header, int32_t v) {
  std::atomic_ref<uint32_t> word(*reinterpret_cast<uint32_t*>(base + header + 4));
  word.store(__builtin_bswap32(static_cast<uint32_t>(v)), std::memory_order_seq_cst);
}

[[noreturn]] void die(int code, const std::string& msg) {
  std::fputs(msg.c_str(), stderr);
  std::fflush(nullptr);
  _exit(code);
}

void* host_main(void* p) {
  auto* holder = static_cast<std::pair<std::shared_ptr<GuestThread>, ThreadParams>*>(p);
  std::shared_ptr<GuestThread> self = std::move(holder->first);
  const ThreadParams params = holder->second;
  delete holder;
  self->run(params);
  self.reset();  // may destroy the thread object (frees its guest memory through the kernel)
  host_finished();
  return nullptr;
}

// Import entry hook: a thread suspended by another one blocks at its next import.
void on_import_entry() {
  if (GuestThread* t = t_current; t != nullptr && t->suspend_pending()) t->wait_while_suspended();
}

uint32_t thread_token() { return t_current != nullptr ? t_current->kthread() : 0; }

}  // namespace

// --- GuestThread ------------------------------------------------------------------------------------------------

GuestThread::GuestThread(Kernel& k, uint32_t id, const ThreadParams& p, const Layout& l, uint8_t cpu)
    : KObject(ObjectKind::Thread), kernel_(k), id_(id), layout_(l), affinity_(1u << cpu) {
  suspend_count_.store(p.suspended ? 1 : 0);
}

GuestThread::~GuestThread() {
  // Guard page back to read/write (unless strict memory keeps free pages PROT_NONE), then both allocations.
  if (!kernel_.config().strict_memory) (void)kernel_.memory().protect(layout_.stack_alloc, kPage, PROT_READ | PROT_WRITE);
  (void)kernel_.stacks().release(layout_.stack_alloc);
  kernel_.pool_free(layout_.block);
}

uint32_t GuestThread::request_suspend() {
  std::lock_guard<std::mutex> lock(mutex_);
  return suspend_count_.fetch_add(1);
}

uint32_t GuestThread::resume() {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint32_t prev = suspend_count_.load();
  if (prev > 0) {
    suspend_count_.store(prev - 1);
    if (prev == 1) resumed_.notify_all();
  }
  return prev;
}

void GuestThread::wait_while_suspended() {
  std::unique_lock<std::mutex> lock(mutex_);
  resumed_.wait(lock, [&] { return suspend_count_.load() == 0; });
}

void GuestThread::queue_apc(const GuestApc& apc) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished()) return;
    apcs_.push_back(apc);
  }
  kernel_.dispatcher().wake_for_apc(kthread());
}

void GuestThread::deliver_apcs() {
  while (apc_disable_.load() == 0) {
    GuestApc apc;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (apcs_.empty()) return;
      apc = apcs_.front();
      apcs_.pop_front();
    }
    const uint64_t args[] = {apc.context, apc.arg1, apc.arg2};
    (void)call_guest(apc.routine, args);
  }
}

void GuestThread::enter_critical_region() { apc_disable_.fetch_add(1); }

void GuestThread::leave_critical_region() {
  if (apc_disable_.fetch_sub(1) != 1) return;
  // APCs that arrived while disabled: re-arm the next alertable wait.
  bool pending = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending = !apcs_.empty();
  }
  if (pending) kernel_.dispatcher().wake_for_apc(kthread());
}

uint32_t GuestThread::tls_slot_address(uint32_t index) const {
  if (index >= kernel_.threads().tls_slot_count()) return 0;
  return layout_.tls_slots + index * 4;
}

void GuestThread::run(const ThreadParams& p) {
  if (auto r = runtime::install_thread_altstack(); !r)
    die(runtime::kExitFault, "guest thread " + std::to_string(id_) + ": cannot install sigaltstack: " + r.error().message + "\n");
  t_current = this;
  wait_while_suspended();
  uint32_t code = 0;
  const uint32_t r1 = layout_.stack_top - runtime::kGuestStackTopSlack;
  try {
    if (p.xapi_start != 0) {
      const uint64_t args[] = {p.start, p.context};
      (void)call_guest_on_frame(p.xapi_start, args, r1, layout_.pcr);
    } else {
      const uint64_t args[] = {p.context};
      code = static_cast<uint32_t>(call_guest_on_frame(p.start, args, r1, layout_.pcr));
    }
  } catch (const GuestThreadExit& e) {
    code = e.code;
  } catch (const UnimplementedImport& e) {
    die(runtime::kExitUnimplementedImport, format_report(e, id_));
  } catch (const std::exception& e) {
    die(runtime::kExitFault, std::string("Uncaught host exception on a guest thread: ") + e.what() + "\nThread: " +
                                 std::to_string(id_) + "\n" + format_trail(current_trail()));
  } catch (...) {
    die(runtime::kExitFault, "Uncaught non-standard exception on a guest thread\nThread: " + std::to_string(id_) +
                                 "\n" + format_trail(current_trail()));
  }
  finish(code);
  t_current = nullptr;
}

void GuestThread::finish(uint32_t code) {
  exit_code_.store(code, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_.store(true, std::memory_order_release);
    apcs_.clear();  // rundown: queued APCs never run
  }
  // Nothing per-thread may outlive the thread in the dispatcher: a later thread can get the same KTHREAD address.
  kernel_.dispatcher().thread_exited(kthread());
  store_signal_state(kernel_.memory().base(), kthread(), 1);
  kernel_.dispatcher().signal(kthread());
  kernel_.threads().remove(this);
}

// --- Threads ----------------------------------------------------------------------------------------------------

Threads::Threads() : default_stack_size_(runtime::kDefaultStackSize), tls_used_(kMinTlsSlots, false) {}

Result<void> Threads::set_image(std::optional<formats::TlsInfo> tls, uint32_t default_stack_size, uint32_t image_base,
                                uint32_t image_size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (threads_created_)
    return Error{ErrorCode::Internal, "the image TLS layout cannot change after a guest thread exists",
                 "Threads::set_image must run before the first create_guest_thread"};
  if (tls) {
    if (tls->slot_count > kMaxTlsSlots)
      return Error{ErrorCode::InvalidFormat, "XEX TLS header: slot count " + std::to_string(tls->slot_count) +
                                                 " exceeds " + std::to_string(kMaxTlsSlots), ""};
    if (tls->data_size > kMaxTlsDataSize)
      return Error{ErrorCode::InvalidFormat, "XEX TLS header: data size " + std::to_string(tls->data_size) +
                                                 " exceeds " + std::to_string(kMaxTlsDataSize), ""};
    if (tls->raw_data_size > tls->data_size)
      return Error{ErrorCode::InvalidFormat, "XEX TLS header: raw data size exceeds the data size", ""};
    const uint64_t raw_end = uint64_t{tls->raw_data_address} + tls->raw_data_size;
    if (tls->raw_data_size != 0 &&
        (tls->raw_data_address < image_base || raw_end > uint64_t{image_base} + image_size))
      return Error{ErrorCode::InvalidFormat, "XEX TLS header: raw data lies outside the loaded image", ""};
  }
  tls_ = tls;
  if (default_stack_size != 0) default_stack_size_ = default_stack_size;
  const uint32_t slots = std::max(kMinTlsSlots, tls ? tls->slot_count : 0u);
  tls_used_.assign(slots, false);
  return {};
}

void Threads::mark_thread_created() {
  std::lock_guard<std::mutex> lock(mutex_);
  threads_created_ = true;
}

std::optional<formats::TlsInfo> Threads::tls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tls_;
}

uint32_t Threads::tls_slot_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<uint32_t>(tls_used_.size());
}

uint32_t Threads::default_stack_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return default_stack_size_;
}

uint32_t Threads::tls_alloc() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < tls_used_.size(); ++i) {
    if (!tls_used_[i]) {
      tls_used_[i] = true;
      return static_cast<uint32_t>(i);
    }
  }
  return kTlsOutOfIndexes;
}

bool Threads::tls_free(uint32_t index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= tls_used_.size() || !tls_used_[index]) return false;
  tls_used_[index] = false;
  return true;
}

void Threads::add(const std::shared_ptr<GuestThread>& t) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::erase_if(live_, [](const std::weak_ptr<GuestThread>& w) { return w.expired(); });
  live_.push_back(t);
}

void Threads::remove(const GuestThread* t) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::erase_if(live_, [t](const std::weak_ptr<GuestThread>& w) {
    auto s = w.lock();
    return !s || s.get() == t;
  });
}

std::shared_ptr<GuestThread> Threads::by_id(uint32_t id) const {
  for (auto& t : live())
    if (t->id() == id) return t;
  return nullptr;
}

std::vector<std::shared_ptr<GuestThread>> Threads::live() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<GuestThread>> out;
  for (const auto& w : live_)
    if (auto s = w.lock()) out.push_back(std::move(s));
  return out;
}

void Threads::add_terminate_notification(TerminateNotification n) {
  std::lock_guard<std::mutex> lock(mutex_);
  notifications_.push_back(n);
}

void Threads::remove_terminate_notification(uint32_t routine) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::erase_if(notifications_, [routine](const TerminateNotification& n) { return n.routine == routine; });
}

std::vector<Threads::TerminateNotification> Threads::terminate_notifications() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return notifications_;
}

// --- Creation ---------------------------------------------------------------------------------------------------

Result<std::shared_ptr<GuestThread>> create_guest_thread(const ThreadParams& p) {
  Kernel& k = kernel();
  Threads& threads = k.threads();
  threads.mark_thread_created();  // freezes the TLS layout (set_image refuses from now on)
  uint8_t* const base = k.memory().base();
  const std::optional<formats::TlsInfo> tls = threads.tls();
  const uint32_t slots = threads.tls_slot_count();
  // Sized in 64 bits: set_image caps the header values, this keeps a bad layout from wrapping into a short block.
  const uint64_t tls_static64 = tls ? (uint64_t{tls->data_size} + 3) / 4 * 4 : 0;
  const uint64_t block_size = uint64_t{kTlsOffset} + tls_static64 + uint64_t{slots} * 4;
  if (block_size > kMaxThreadBlock)
    return Error{ErrorCode::InvalidFormat, "guest thread TLS block too large", std::to_string(block_size) + " bytes"};
  const uint32_t tls_static = static_cast<uint32_t>(tls_static64);

  // Guest stack: one reserved guard page below the committed stack.
  const uint32_t stack_size = std::max(kMinStack, round_up(p.stack_size != 0 ? p.stack_size : threads.default_stack_size(), kPage));
  auto stack = k.stacks().reserve(stack_size + kPage, 0, false, std::nullopt, runtime::kPageReadWrite);
  if (!stack) return Error{ErrorCode::InsufficientResources, "no guest stack space for a new thread", stack.error().message};
  const uint32_t stack_alloc = stack.value();
  if (auto r = k.stacks().commit(stack_alloc + kPage, stack_size, runtime::kPageReadWrite); !r) {
    (void)k.stacks().release(stack_alloc);
    return Error{ErrorCode::InsufficientResources, "cannot commit a guest stack", r.error().message};
  }
  Result<void> prot = k.memory().protect(stack_alloc + kPage, stack_size, PROT_READ | PROT_WRITE);
  if (prot) prot = k.memory().protect(stack_alloc, kPage, PROT_NONE);
  const uint32_t block = prot ? k.pool_alloc(static_cast<uint32_t>(block_size), 0) : 0;
  if (block == 0) {
    (void)k.memory().protect(stack_alloc, kPage, PROT_READ | PROT_WRITE);
    (void)k.stacks().release(stack_alloc);
    return Error{ErrorCode::InsufficientResources, "no guest memory for a new thread's KTHREAD/PCR/TLS", ""};
  }

  GuestThread::Layout l{};
  l.block = block;
  l.kthread = block + kKThreadOffset;
  l.pcr = block + kPcrOffset;
  l.tls = block + kTlsOffset;
  l.tls_slots = l.tls + tls_static;
  l.stack_alloc = stack_alloc;
  l.stack_bottom = stack_alloc + kPage;
  l.stack_top = stack_alloc + kPage + stack_size;

  // TLS: the XEX template (raw data, zero-filled up to data_size; the pool block is already zeroed), then slots.
  if (tls && tls->raw_data_size != 0 && tls->raw_data_address != 0)
    std::memcpy(base + l.tls, base + tls->raw_data_address, std::min(tls->raw_data_size, tls->data_size));

  uint8_t cpu = threads.next_cpu();
  if (p.cpu_mask != 0 && std::countr_zero(p.cpu_mask) < 6) cpu = static_cast<uint8_t>(std::countr_zero(p.cpu_mask));
  const uint32_t id = threads.next_id();

  // KTHREAD (Xenia X_KTHREAD offsets, guest_structs.h). The header size byte cannot hold 0xAB0 / 4; left 0.
  auto* kt = reinterpret_cast<X_KTHREAD*>(base + l.kthread);
  kt->header.type = kThreadObject;
  kt->header.signal_state = 0;
  kt->header.wait_list_flink = l.kthread + 8;  // empty list: points at itself
  kt->header.wait_list_blink = l.kthread + 8;
  kt->stack_base = l.stack_top;
  kt->stack_limit = l.stack_bottom;
  kt->tls_address = l.tls;
  kt->thread_id = id;
  kt->start_address = p.start;
  kt->last_error = 0;

  // KPCR (Xenia X_KPCR offsets).
  auto* pcr = reinterpret_cast<X_KPCR*>(base + l.pcr);
  pcr->tls_ptr = l.tls;
  pcr->pcr_ptr = l.pcr;
  pcr->stack_base = l.stack_top;
  pcr->stack_end = l.stack_bottom;
  pcr->current_thread = l.kthread;
  pcr->current_cpu = cpu;

  auto thread = std::make_shared<GuestThread>(k, id, p, l, cpu);
  thread->set_guest_header(l.kthread);
  thread->set_handle(k.objects().add(thread));
  threads.add(thread);

  host_started();
  auto* holder = new std::pair<std::shared_ptr<GuestThread>, ThreadParams>(thread, p);
  if (auto r = runtime::start_host_thread(host_main, holder); !r) {
    delete holder;
    host_finished();
    threads.remove(thread.get());
    (void)k.objects().close(thread->handle());
    return r.error();
  }
  return thread;
}

GuestThread* current_thread() { return t_current; }

void deliver_current_apcs() {
  if (GuestThread* t = t_current) t->deliver_apcs();
}

int run_main_thread(uint32_t entry, uint32_t stack_size) {
  ThreadParams p;
  p.start = entry;
  p.stack_size = stack_size;
  auto t = create_guest_thread(p);
  if (!t) {
    std::fprintf(stderr, "error: cannot create the main guest thread: %s\n", t.error().message.c_str());
    return runtime::kExitFault;
  }
  const uint32_t header = t.value()->kthread();
  // The KTHREAD is signaled when the thread ends (whatever ended it).
  const uint32_t h[1] = {header};
  (void)kernel().dispatcher().wait(h, WaitType::Any, Timeout{Timeout::Infinite, 0}, false);
  return runtime::kExitOk;
}

void install_thread_hooks(Kernel& k) {
  k.dispatcher().set_thread_token_hook(thread_token);
  k.objects().set_current_thread_hook([]() -> std::shared_ptr<KObject> {
    return t_current != nullptr ? t_current->shared_from_this() : nullptr;
  });
  set_import_entry_hook(on_import_entry);
}

void wait_for_guest_threads_for_tests() {
  std::unique_lock<std::mutex> lock(hosts_mutex());
  hosts_cv().wait(lock, [] { return g_hosts == 0; });
}

}  // namespace lr::kernel
