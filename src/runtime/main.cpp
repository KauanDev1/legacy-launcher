// SPDX-License-Identifier: MIT
// Game entry point. NOT part of a prebuilt library: the game template compiles this file together with the
// generated ppc_config.h / ppc_recomp_*.cpp / ppc_func_mapping.cpp and the generated import thunks, and links
// lr_runtime_core with --whole-archive so its LR_KERNEL / LR_KERNEL_STUB registrations are kept, then lr_vulkan
// (the Vulkan backend, runtime/gpu/vk/vk_backend.h).
//
// Usage: <game> --image <xex> [--data <dir>] [--user <dir>] [--headless] [--gpu vulkan|null] [--coverage]
//        [--strict-memory] [--trace-imports] [--trace-file <path>] [--trace-gpu] [--gpu-trace-file <path>]
//        [--exit-after-swaps <n>] [--screenshot-after <n> <png>] [--shader-cache-dir <dir>] [--dump-shaders]
//        [--watch-guest <addr>:<len>]...
// --trace-imports logs every import call (runtime/kernel/trace.h) to --trace-file, or stderr without one; the
// launcher always passes --trace-file, which alone does nothing. --trace-gpu logs the GPU's PM4 packets, draws,
// swaps and per-second counters (runtime/gpu/gpu_system.h) to --gpu-trace-file, or stderr without one (capped at
// 64 MiB); the launcher always passes --gpu-trace-file, which alone does nothing too. Headless runs pace the vblank
// by swaps, window runs at 60 Hz of wall time. LR_DEADLOCK_SECONDS (default 10, 0 = off) sets the
// deadlock detector threshold (runtime/kernel/deadlock.h). --exit-after-swaps <n> (n > 0) ends the run with exit code
// 0 once the GPU has processed n swaps (KernelConfig::exit_after_swaps; the M3 integration test).
// --gpu picks the backend: vulkan (default; runtime/gpu/vk/vk_backend.h: an SDL window with SDL_WINDOW_VULKAN, or
// offscreen with --headless) or null (gpu/null_backend.h: renders nothing, needs no Vulkan; the unit tests use it).
// Without a usable Vulkan device the vulkan backend fails at start with exit 2 and a message naming the missing
// piece. LR_VULKAN_VALIDATION=1 enables VK_LAYER_KHRONOS_validation (when installed); its messages go to stderr.
// --screenshot-after <n> <png> (vulkan only) writes the frame of swap n to <png> (runtime/gpu/screenshot.h), prints
// "[gpu] screenshot after n swaps: <png>" and exits 0 (2 when the file cannot be written). Counts (--exit-after-swaps,
// --screenshot-after) are plain decimal digits: a sign, spaces or 0 are usage errors (exit 2).
// --shader-cache-dir <dir> keeps the translated shaders on disk (runtime/gpu/shader/disk_cache.h; the launcher passes
// <root>/work/shaders/runtime); without it they stay in memory. --dump-shaders (needs --shader-cache-dir) also writes
// the microcode disassembly next to each cache file (<name>.txt). Once the guest started the GPU, every exit path but
// the fault handler prints "[gpu] shaders: keys=N vs=N ps=N translated=N disk=N failed=N new_last_100_swaps=N" on
// stderr (lr::gpu::format_shader_summary, through lr::kernel::set_exit_report_hook).
// --watch-guest <addr>:<len> (repeatable, up to 16; a diagnostic) logs "[watch] ..." lines on stderr naming the guest
// functions that write guest [addr, addr + len) (runtime/write_watch.h); the writes still happen.
// SIGTERM, SIGINT and SIGHUP (and closing the window) end the run in order with exit code 0: the GPU thread stops
// (any frame in flight finishes or, after 100 ms slices, is abandoned), the Vulkan backend waits for the device to be
// idle and destroys everything ("[gpu] vulkan: shut down"), then the process exits. The signals are blocked in every
// thread and taken by one watcher thread (sigwait). The orderly exit is bounded: a second signal, or 5 s, ends the
// process at once ("[game] shutdown timed out"). --gpu test-stall (tests only) hangs the GPU thread in its first swap.
// Exit codes: 0 ok (also: window closed by the user, SIGTERM/SIGINT/SIGHUP), 2 usage/load error (also: no usable
// Vulkan device), 3 unimplemented import, 4 fault, 5 guest fatal (lr::kernel::guest_fatal), 6 deadlock.

#include <SDL.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime/fault.h"
#include "runtime/func_table.h"
#include "runtime/gpu/gpu_system.h"
#include "runtime/gpu/vk/vk_backend.h"
#include "runtime/guest_memory.h"
#include "runtime/guest_thread.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/trace.h"
#include "runtime/kernel/variables.h"
#include "runtime/loader.h"
#include "runtime/write_watch.h"
#include "runtime/xex_imports.h"

// Emitted by lr::pipeline::render_import_thunks.
extern const char* const lr_import_names[];
extern const size_t lr_import_count;

namespace {

using namespace lr::runtime;

struct Options {
  std::string image;
  std::string data;  // --data: the extracted game (read-only mount); default: the image directory
  std::string user;  // --user: writable data (saves, cache)
  bool headless = false;
  bool coverage = false;
  bool strict_memory = false;  // uncommitted heap pages PROT_NONE (lr::kernel::KernelConfig::strict_memory)
  bool trace_imports = false;
  std::string trace_file;  // empty: stderr
  bool trace_gpu = false;
  std::string gpu_trace_file;  // empty: stderr
  uint64_t exit_after_swaps = 0;  // 0: run until the game ends
  std::string shader_cache_dir;    // empty: translated shaders in memory only
  bool dump_shaders = false;
  bool null_gpu = false;           // --gpu null: NullBackend instead of the Vulkan backend
  bool stall_gpu = false;          // --gpu test-stall (tests only): StallBackend
  uint64_t screenshot_after = 0;   // 0: no screenshot
  std::string screenshot_path;
  std::vector<WatchRange> watches;  // --watch-guest
};

int usage(const char* msg) {
  if (msg) std::fprintf(stderr, "error: %s\n", msg);
  std::fprintf(stderr,
               "usage: game --image <xex> [--data <dir>] [--user <dir>] [--headless] [--gpu vulkan|null] [--coverage] "
               "[--strict-memory] [--trace-imports] [--trace-file <path>] [--trace-gpu] [--gpu-trace-file <path>] "
               "[--exit-after-swaps <n>] [--screenshot-after <n> <png>] [--shader-cache-dir <dir>] "
               "[--dump-shaders] [--watch-guest <addr>:<len>]...\n");
  return kExitUsage;
}

// A positive count written as plain decimal digits only: no sign, no spaces, no 0x, no overflow, not 0.
std::optional<uint64_t> parse_count(const char* v) {
  if (v == nullptr || *v == 0) return std::nullopt;
  uint64_t n = 0;
  for (const char* p = v; *p != 0; ++p) {
    if (*p < '0' || *p > '9') return std::nullopt;
    const uint64_t digit = static_cast<uint64_t>(*p - '0');
    if (n > (UINT64_MAX - digit) / 10) return std::nullopt;
    n = n * 10 + digit;
  }
  if (n == 0) return std::nullopt;
  return n;
}

std::vector<char> read_file(const std::string& path, bool& ok) {
  std::ifstream f(path, std::ios::binary);
  ok = static_cast<bool>(f);
  if (!ok) return {};
  return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool is_xex2(const std::vector<char>& b) { return b.size() >= 4 && std::memcmp(b.data(), "XEX2", 4) == 0; }

// Ends the process now: flushes stdio and the import trace and skips static destructors. Every exit once a kernel
// thread may run (resolve_variable_imports starts the kernel worker for KeTimeStampBundle; guest threads later)
// goes through here, so no destructor tears the kernel down under a running thread.
[[noreturn]] void exit_now(int rc) {
  lr::kernel::flush_at_exit();  // the shader summary, stdio, the traces
  _exit(rc);
}

// The Vulkan backend, owned by the GpuSystem; shutdown_and_exit tears it down after the GPU thread stopped.
lr::gpu::vk::VkBackend* g_vk_backend = nullptr;
bool g_kernel_ready = false;  // init_kernel succeeded (set on the main thread before any watcher can run)

// SIGTERM, SIGINT and SIGHUP: blocked in every thread (block_exit_signals runs before the first thread starts) and
// taken by one watcher thread, which ends the run in order with exit code 0.
sigset_t exit_signals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGHUP);
  return set;
}

constexpr int kShutdownTimeoutSeconds = 5;

// The orderly exit's bound: another exit signal, or kShutdownTimeoutSeconds without the exit, ends the process at
// once with "[game] shutdown timed out" (a GPU thread stuck in the backend, a driver that never answers).
void start_shutdown_watchdog(int rc) {
  std::thread([rc] {
    const sigset_t set = exit_signals();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kShutdownTimeoutSeconds);
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) break;
      timespec ts{};
      ts.tv_sec = static_cast<time_t>(left.count() / 1'000'000'000);
      ts.tv_nsec = static_cast<long>(left.count() % 1'000'000'000);
      siginfo_t info{};
      if (sigtimedwait(&set, &info, &ts) > 0) break;  // a second exit signal
      if (errno != EINTR) break;                     // EAGAIN: the time is up
    }
    std::fprintf(stderr, "[game] shutdown timed out\n");
    exit_now(rc);
  }).detach();
}

// Orderly end (a signal, the window closed, the main guest thread returned): stops the GPU thread (a frame in
// flight finishes, or is abandoned: VkBackend::request_stop), waits for the Vulkan device to be idle and destroys
// every Vulkan object, then exit_now. Bounded by start_shutdown_watchdog. Never called on the GPU thread (it joins
// it). A second caller, while the first is tearing down, just waits for the exit.
[[noreturn]] void shutdown_and_exit(int rc) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) {
    for (;;) pause();
  }
  start_shutdown_watchdog(rc);
  if (g_vk_backend != nullptr) g_vk_backend->request_stop();
  if (g_kernel_ready) lr::kernel::kernel().gpu().stop();
  if (g_vk_backend != nullptr) g_vk_backend->shutdown();
  exit_now(rc);
}

// --gpu test-stall (tests only): a backend whose first swap() never returns, so the GPU thread hangs mid-frame and the
// shutdown watchdog has to end the run. Prints "[gpu] test-stall: swap blocked" when it starts blocking.
class StallBackend final : public lr::gpu::Backend {
 public:
  void load_shader(lr::gpu::ShaderStage, uint32_t, uint32_t) override {}
  void draw(const lr::gpu::RegisterFile&, const lr::gpu::DrawInfo&, const lr::gpu::ActiveShaders&) override {}
  void resolve(const lr::gpu::RegisterFile&) override {}
  void swap(const lr::gpu::RegisterFile&, uint32_t, uint32_t, uint32_t) override {
    std::fprintf(stderr, "[gpu] test-stall: swap blocked\n");
    std::fflush(stderr);
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
  }
  void wait_idle() override {}
};

void block_exit_signals() {
  const sigset_t set = exit_signals();
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

void start_signal_watcher() {
  std::thread([] {
    const sigset_t set = exit_signals();
    int sig = 0;
    while (sigwait(&set, &sig) != 0) {
    }
    std::fprintf(stderr, "[game] signal %d: exiting\n", sig);
    shutdown_and_exit(kExitOk);
  }).detach();
}

// LR_VULKAN_VALIDATION=1: the Khronos validation layer (when installed).
bool vulkan_validation() {
  const char* v = std::getenv("LR_VULKAN_VALIDATION");
  return v != nullptr && std::strcmp(v, "1") == 0;
}

// The exit report (lr::kernel::set_exit_report_hook): the shader summary, once the guest started the GPU.
void print_shader_summary() {
  lr::gpu::GpuSystem& gpu = lr::kernel::kernel().gpu();
  if (!gpu.started()) return;
  std::fprintf(stderr, "%s\n", lr::gpu::format_shader_summary(gpu.shader_stats()).c_str());
  std::fflush(stderr);
}

// The exit report: the shader summary and the --watch-guest summary (runtime/write_watch.h).
void print_exit_reports() {
  print_shader_summary();
  write_watch_report();
}

// LR_DEADLOCK_SECONDS: the detector threshold (tests use 1). Unset or not a number: 10; 0 disables the detector.
uint32_t deadlock_seconds() {
  const char* v = std::getenv("LR_DEADLOCK_SECONDS");
  if (v == nullptr) return 10;
  char* end = nullptr;
  const unsigned long n = std::strtoul(v, &end, 10);
  return (end == v || *end != 0 || n > 86400) ? 10 : static_cast<uint32_t>(n);
}

// Function rows come from the generated thunks; with --image <xex>, the XEX's variable imports (type-0 records
// without a thunk) are added as `variable` rows: implemented when a kernel variable is registered for them.
int print_coverage(const std::string& image) {
  using lr::kernel::ImportState;
  std::vector<lr::runtime::ImportRecord> variables;
  if (!image.empty()) {
    bool ok = false;
    const std::vector<char> bytes = read_file(image, ok);
    if (!ok) {
      std::fprintf(stderr, "error: cannot read %s\n", image.c_str());
      return kExitUsage;
    }
    if (is_xex2(bytes)) {  // other images (the test ELF) have no import records
      auto list = list_imports(std::as_bytes(std::span(bytes)));
      if (!list) {
        std::fprintf(stderr, "error: cannot list the imports of %s: %s\n", image.c_str(), list.error().message.c_str());
        return kExitUsage;
      }
      for (auto& r : list.value())
        if (r.thunk == 0) variables.push_back(std::move(r));
    }
  }
  // Spec section 5.3: functions and variables are counted separately ("N funções + M variáveis").
  struct Counts {
    size_t implemented = 0, stub = 0, missing = 0;
    const char* add(ImportState s) {
      switch (s) {
        case ImportState::Implemented:
          ++implemented;
          return "implemented";
        case ImportState::Stub:
          ++stub;
          return "stub";
        case ImportState::Missing:
          break;
      }
      ++missing;
      return "missing";
    }
  } functions, vars;
  std::printf("| Import | Kind | Status |\n|---|---|---|\n");
  for (size_t i = 0; i < lr_import_count; ++i) {
    const char* status = functions.add(lr::kernel::find_import(lr_import_names[i]).state);
    // Only function imports get thunks (XenonRecomp creates no symbol for variable imports).
    std::printf("| %s | function | %s |\n", lr_import_names[i], status);
  }
  for (const auto& v : variables) {
    char ordinal[8];
    std::snprintf(ordinal, sizeof ordinal, "0x%04X", static_cast<unsigned>(v.ordinal));
    const std::string name = v.name.empty() ? v.library + "@" + ordinal : v.name;
    const char* status = vars.add(v.name.empty() ? ImportState::Missing : lr::kernel::variable_state(v.name));
    std::printf("| %s | variable | %s |\n", name.c_str(), status);
  }
  std::printf("\nFunctions: implemented %zu, stub %zu, missing %zu / %zu\n", functions.implemented, functions.stub,
              functions.missing, lr_import_count);
  std::printf("Variables: implemented %zu, stub %zu, missing %zu / %zu\n", vars.implemented, vars.stub, vars.missing,
              variables.size());
  return 0;
}

// --- Guest-call glue (lr::kernel::call_guest): the prebuilt kernel cannot build a PPCContext, this TU can. ---
// The PPCContext is built here, in the TU compiled with the game's real generated ppc_config.h, so its layout
// always matches the recompiled code. Every guest thread (the main one included) starts through this glue too
// (lr::kernel::call_guest_on_frame with its own stack top and KPCR).
uint8_t* g_guest_base = nullptr;  // set in main() before set_guest_glue

// Restores the host FP control word on scope exit (also when the callee unwinds), so the caller's cached
// ctx.fpscr.csr still matches the hardware after a callback changed rounding or flush mode.
struct HostFpGuard {
  PPCFPSCRRegister fp{};
  uint32_t saved = fp.getcsr();
  ~HostFpGuard() { fp.setcsr(saved); }
};

uint64_t glue_call(uint8_t* base, uint32_t fn, const uint64_t* args, int nargs, uint32_t r1, uint32_t r13) {
  const HostFpGuard fp_guard;
  PPCContext ctx{};
  ctx.r1.u64 = r1;
  ctx.r13.u64 = r13;
  // fpscr caches the host MXCSR: start from the real one (all SSE exceptions masked). A zeroed cache would make the
  // first enableFlushMode() write MXCSR = FTZ|DAZ with every exception unmasked, and the next inexact result
  // would raise SIGFPE.
  ctx.fpscr.loadFromHost();
  PPCRegister* const gpr[8] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  for (int i = 0; i < nargs && i < 8; ++i) gpr[i]->u64 = args[i];
  PPCFunc* const target = PPC_LOOKUP_FUNC(base, fn);
  target(ctx, base);
  return ctx.r3.u64;
}

// Range and alignment first: the function table is only read for addresses inside the code range.
bool glue_has_function(uint32_t fn) {
  if (fn < PPC_CODE_BASE || fn >= PPC_CODE_BASE + PPC_CODE_SIZE || fn % 4 != 0) return false;
  PPCFunc* const target = PPC_LOOKUP_FUNC(g_guest_base, fn);
  return target != nullptr;
}

// The game window (SDL_WINDOW_VULKAN for the Vulkan backend), or nullptr after printing why. SDL's own signal
// handlers stay off: the exit signals belong to the watcher thread.
SDL_Window* create_window(bool vulkan) {
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "error: SDL_Init failed: %s (use --headless to run without a window)\n", SDL_GetError());
    return nullptr;
  }
  const Uint32 flags = SDL_WINDOW_RESIZABLE | (vulkan ? SDL_WINDOW_VULKAN : 0);
  SDL_Window* win = SDL_CreateWindow("LegacyRecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, flags);
  if (!win) {
    std::fprintf(stderr, "error: SDL_CreateWindow failed: %s (use --headless to run without a window)\n", SDL_GetError());
    SDL_Quit();
    return nullptr;
  }
  return win;
}

// Runs the main guest thread (waited for on a helper thread) and keeps the SDL window alive on the main thread.
int run_with_window(uint32_t entry, uint32_t stack_size) {
  std::atomic<bool> done{false};
  std::atomic<int> rc{0};
  std::thread runner([&] {
    rc = lr::kernel::run_main_thread(entry, stack_size);
    done = true;
  });
  bool closed = false;
  while (!done && !closed) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) closed = closed || ev.type == SDL_QUIT;
    SDL_Delay(5);
  }
  if (closed && !done) {
    // The user closed the window while the guest is still running: report success and take the guest down with us.
    shutdown_and_exit(kExitOk);
  }
  runner.join();
  return rc;  // main ends the process (shutdown_and_exit); the window goes with it
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--headless") opt.headless = true;
    else if (a == "--coverage") opt.coverage = true;
    else if (a == "--strict-memory") opt.strict_memory = true;
    else if (a == "--trace-imports") opt.trace_imports = true;
    else if (a == "--trace-gpu") opt.trace_gpu = true;
    else if (a == "--dump-shaders") opt.dump_shaders = true;
    else if (a == "--exit-after-swaps") {
      if (i + 1 >= argc) return usage("missing value for --exit-after-swaps");
      const std::optional<uint64_t> n = parse_count(argv[++i]);
      if (!n) return usage("--exit-after-swaps needs a positive integer (decimal digits only)");
      opt.exit_after_swaps = *n;
    } else if (a == "--screenshot-after") {
      if (i + 2 >= argc) return usage("--screenshot-after needs a swap count and a PNG path");
      const std::optional<uint64_t> n = parse_count(argv[++i]);
      if (!n) return usage("--screenshot-after needs a positive integer (decimal digits only)");
      opt.screenshot_after = *n;
      opt.screenshot_path = argv[++i];
      if (opt.screenshot_path.empty()) return usage("--screenshot-after needs a PNG path");
    } else if (a == "--watch-guest") {
      if (i + 1 >= argc) return usage("missing value for --watch-guest");
      auto w = parse_watch_spec(argv[++i]);
      if (!w) return usage(w.error().message.c_str());
      if (opt.watches.size() >= 16) return usage("--watch-guest takes at most 16 ranges");
      opt.watches.push_back(w.value());
    } else if (a == "--gpu") {
      if (i + 1 >= argc) return usage("missing value for --gpu");
      const std::string v = argv[++i];
      if (v != "vulkan" && v != "null" && v != "test-stall") return usage("--gpu must be vulkan or null");
      opt.null_gpu = v == "null" || v == "test-stall";
      opt.stall_gpu = v == "test-stall";
    } else if (a == "--image" || a == "--data" || a == "--user" || a == "--trace-file" || a == "--gpu-trace-file" ||
               a == "--shader-cache-dir") {
      if (i + 1 >= argc) return usage(("missing value for " + a).c_str());
      (a == "--image"            ? opt.image
       : a == "--data"           ? opt.data
       : a == "--user"           ? opt.user
       : a == "--trace-file"     ? opt.trace_file
       : a == "--gpu-trace-file" ? opt.gpu_trace_file
                                 : opt.shader_cache_dir) = argv[++i];
    } else {
      return usage(("unknown argument " + a).c_str());
    }
  }
  if (opt.coverage) return print_coverage(opt.image);
  if (opt.image.empty()) return usage("--image is required");
  if (opt.dump_shaders && opt.shader_cache_dir.empty()) return usage("--dump-shaders needs --shader-cache-dir");
  if (opt.screenshot_after != 0 && opt.null_gpu) return usage("--screenshot-after needs the Vulkan backend (not --gpu null)");

  bool read_ok = false;
  const std::vector<char> bytes = read_file(opt.image, read_ok);
  if (!read_ok) {
    std::fprintf(stderr, "error: cannot read %s\n", opt.image.c_str());
    return kExitUsage;
  }

  auto mem = GuestMemory::reserve(0x1'0000'0000ull + PPC_CODE_SIZE * 2);
  if (!mem) {
    std::fprintf(stderr, "error: %s\n", mem.error().message.c_str());
    return kExitUsage;
  }

  // Kernel first: load_image maps the XEX header and the LDR entry into the kernel data area.
  lr::kernel::KernelConfig cfg;
  cfg.game_dir = opt.data.empty() ? std::filesystem::path(opt.image).parent_path() : std::filesystem::path(opt.data);
  if (!opt.user.empty()) cfg.user_dir = opt.user;
  cfg.strict_memory = opt.strict_memory;
  // Console language comes from the host locale (LC_ALL wins over LANG); XGetLanguage maps it (docs/architecture.md).
  for (const char* var : {"LC_ALL", "LANG"}) {
    if (const char* v = std::getenv(var); v != nullptr && *v != 0) {
      cfg.language = v;
      break;
    }
  }
  cfg.trace_imports = opt.trace_imports;
  if (!opt.trace_file.empty()) cfg.trace_file = opt.trace_file;
  cfg.trace_gpu = opt.trace_gpu;
  if (!opt.gpu_trace_file.empty()) cfg.gpu_trace_file = opt.gpu_trace_file;
  cfg.vblank_paced_by_swap = opt.headless;  // spec section 11: a slow headless renderer must not starve waits
  cfg.exit_after_swaps = opt.exit_after_swaps;
  cfg.deadlock_seconds = deadlock_seconds();
  // Before the first thread (the kernel's start in init_kernel): every thread inherits the blocked exit signals.
  block_exit_signals();
  if (auto r = lr::kernel::init_kernel(mem.value(), std::move(cfg)); !r) {
    std::fprintf(stderr, "error: %s\n", r.error().message.c_str());
    return kExitUsage;
  }
  g_kernel_ready = true;
  // Before the guest runs (the GPU thread starts at VdInitializeRingBuffer).
  if (!opt.shader_cache_dir.empty()) lr::kernel::kernel().gpu().set_shader_cache_dir(opt.shader_cache_dir);
  lr::kernel::kernel().gpu().set_dump_shaders(opt.dump_shaders);
  lr::kernel::set_exit_report_hook(&print_exit_reports);


  const auto image_bytes = std::as_bytes(std::span(bytes));
  auto img = load_image(mem.value(), image_bytes);
  if (!img) {
    std::fprintf(stderr, "error: cannot load %s: %s\n", opt.image.c_str(), img.error().message.c_str());
    exit_now(kExitUsage);
  }
  if (img.value().base != PPC_IMAGE_BASE) {
    std::fprintf(stderr, "error: image base 0x%08X does not match the recompiled build (0x%08llX)\n", img.value().base,
                 static_cast<unsigned long long>(PPC_IMAGE_BASE));
    exit_now(kExitUsage);
  }
  // Every guest thread gets a copy of the XEX TLS template; ExCreateThread with stack size 0 uses the image's.
  // Before resolve_variable_imports: importing KeTimeStampBundle starts the kernel worker, whose TLS block copies the
  // template too.
  if (auto r = lr::kernel::kernel().threads().set_image(img.value().tls, img.value().stack_size, img.value().base,
                                                        img.value().size);
      !r) {
    std::fprintf(stderr, "error: cannot load %s: %s\n", opt.image.c_str(), r.error().message.c_str());
    exit_now(kExitUsage);
  }
  if (is_xex2(bytes)) {  // the test ELF images have no import records
    auto header = lr::formats::parse_xex2(image_bytes);
    lr::Result<void> r = header ? lr::kernel::resolve_variable_imports(mem.value(), img.value(), header.value())
                                : lr::Result<void>(header.error());
    if (!r) {
      std::fprintf(stderr, "error: cannot resolve the imports of %s: %s\n", opt.image.c_str(), r.error().message.c_str());
      exit_now(kExitUsage);
    }
  }

  std::vector<FuncMapping> maps;
  PPCFunc* entry_fn = nullptr;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr; ++m) {
    maps.push_back({static_cast<uint32_t>(m->guest), reinterpret_cast<void*>(m->host)});
    if (m->guest == img.value().entry) entry_fn = m->host;
  }
  if (auto r = install_function_table(mem.value(), PPC_IMAGE_BASE + PPC_IMAGE_SIZE, PPC_CODE_BASE, PPC_CODE_SIZE, maps); !r) {
    std::fprintf(stderr, "error: %s\n", r.error().message.c_str());
    exit_now(kExitUsage);
  } else if (!r.value().empty()) {
    // A pipeline bug (runtime/func_table.h): the first mapping of each address was kept.
    std::fprintf(stderr, "[runtime] warning: %zu duplicate function mappings dropped (first kept):", r.value().size());
    for (size_t k = 0; k < r.value().size() && k < 16; ++k) std::fprintf(stderr, " 0x%08X", r.value()[k]);
    std::fprintf(stderr, "%s\n", r.value().size() > 16 ? " ..." : "");
  }
  if (!entry_fn) {
    std::fprintf(stderr, "error: no recompiled function at entry point 0x%08X\n", img.value().entry);
    exit_now(kExitUsage);
  }
  // The window (SDL_WINDOW_VULKAN with the Vulkan backend) and the GPU backend: after the image checked out (a bad
  // image never touches the GPU), before the fault handlers and the guest.
  SDL_Window* window = nullptr;
  if (!opt.headless) {
    window = create_window(!opt.null_gpu);
    if (window == nullptr) exit_now(kExitUsage);
  }
  if (!opt.null_gpu) {
    lr::gpu::vk::VkBackendOptions vk;
    vk.mem = lr::kernel::kernel().gpu_memory_access();
    vk.sdl_window = window;
    vk.validation = vulkan_validation();
    vk.screenshot_after = opt.screenshot_after;
    vk.screenshot_path = opt.screenshot_path;
    vk.on_screenshot = [path = opt.screenshot_path](uint64_t swap, const lr::Result<void>& written) {
      // On the GPU thread: exit right here, like --exit-after-swaps (no teardown needed, nothing is destroyed).
      if (!written) {
        std::fprintf(stderr, "error: %s\n", written.error().message.c_str());
        exit_now(kExitUsage);
      }
      std::fprintf(stderr, "[gpu] screenshot after %llu swaps: %s\n", static_cast<unsigned long long>(swap),
                   path.c_str());
      exit_now(kExitOk);
    };
    auto backend = lr::gpu::vk::VkBackend::create(std::move(vk));
    if (!backend) {
      std::fprintf(stderr, "error: %s\n", backend.error().message.c_str());
      if (!backend.error().hint.empty()) std::fprintf(stderr, "hint: %s\n", backend.error().hint.c_str());
      std::fprintf(stderr, "(--gpu null runs without rendering)\n");
      exit_now(kExitUsage);
    }
    g_vk_backend = backend.value().get();
    lr::kernel::kernel().gpu().set_backend(std::move(backend.value()));
  } else if (opt.stall_gpu) {
    lr::kernel::kernel().gpu().set_backend(std::make_unique<StallBackend>());
  }
  start_signal_watcher();

  if (auto r = install_fault_handlers(mem.value()); !r) {
    std::fprintf(stderr, "error: %s\n", r.error().message.c_str());
    exit_now(kExitUsage);
  }
  if (!opt.watches.empty()) {
    std::vector<HostCodeEntry> code;
    code.reserve(maps.size());
    for (const FuncMapping& m : maps) code.push_back({reinterpret_cast<uintptr_t>(m.host), m.guest});
    if (auto r = install_write_watch(mem.value(), opt.watches, HostCodeMap(std::move(code))); !r) {
      std::fprintf(stderr, "error: %s\n", r.error().message.c_str());
      exit_now(kExitUsage);
    }
  }

  g_guest_base = mem.value().base();
  lr::kernel::set_guest_glue({glue_call, glue_has_function});

  const uint32_t entry = img.value().entry, stack_size = img.value().stack_size;
  const int rc = opt.headless ? lr::kernel::run_main_thread(entry, stack_size) : run_with_window(entry, stack_size);
  // The main guest thread has returned: end the process now (GPU stopped, Vulkan torn down), taking any other guest
  // threads down with it, without running static destructors under their feet.
  shutdown_and_exit(rc);
}
