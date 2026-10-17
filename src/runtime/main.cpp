// SPDX-License-Identifier: MIT
// Game entry point. NOT part of a prebuilt library: the game template compiles this file together with the
// generated ppc_config.h / ppc_recomp_*.cpp / ppc_func_mapping.cpp and the generated import thunks, and links
// lr_runtime_core with --whole-archive so its LR_KERNEL / LR_KERNEL_STUB registrations are kept.
//
// Usage: <game> --image <xex> [--data <dir>] [--headless] [--coverage] [--strict-memory]
// Exit codes: 0 ok (also: window closed by the user), 2 usage/load error, 3 unimplemented import, 4 fault,
// 5 guest fatal (lr::kernel::guest_fatal), 6 deadlock.

#include <SDL.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"
#include "runtime/fault.h"
#include "runtime/func_table.h"
#include "runtime/guest_memory.h"
#include "runtime/guest_thread.h"
#include "runtime/kernel/glue.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/threads.h"
#include "runtime/kernel/variables.h"
#include "runtime/loader.h"
#include "runtime/xex_imports.h"

// Emitted by lr::pipeline::render_import_thunks.
extern const char* const lr_import_names[];
extern const size_t lr_import_count;

namespace {

using namespace lr::runtime;

struct Options {
  std::string image;
  std::string data;
  bool headless = false;
  bool coverage = false;
  bool strict_memory = false;  // uncommitted heap pages PROT_NONE (lr::kernel::KernelConfig::strict_memory)
};

int usage(const char* msg) {
  if (msg) std::fprintf(stderr, "error: %s\n", msg);
  std::fprintf(stderr, "usage: game --image <xex> [--data <dir>] [--headless] [--coverage] [--strict-memory]\n");
  return kExitUsage;
}

std::vector<char> read_file(const std::string& path, bool& ok) {
  std::ifstream f(path, std::ios::binary);
  ok = static_cast<bool>(f);
  if (!ok) return {};
  return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool is_xex2(const std::vector<char>& b) { return b.size() >= 4 && std::memcmp(b.data(), "XEX2", 4) == 0; }

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
  size_t implemented = 0, stub = 0, missing = 0;
  std::printf("| Import | Kind | Status |\n|---|---|---|\n");
  for (size_t i = 0; i < lr_import_count; ++i) {
    const char* status = "missing";
    switch (lr::kernel::find_import(lr_import_names[i]).state) {
      case ImportState::Implemented:
        status = "implemented";
        ++implemented;
        break;
      case ImportState::Stub:
        status = "stub";
        ++stub;
        break;
      case ImportState::Missing:
        ++missing;
        break;
    }
    // Only function imports get thunks (XenonRecomp creates no symbol for variable imports).
    std::printf("| %s | function | %s |\n", lr_import_names[i], status);
  }
  for (const auto& v : variables) {
    char ordinal[8];
    std::snprintf(ordinal, sizeof ordinal, "0x%04X", static_cast<unsigned>(v.ordinal));
    const std::string name = v.name.empty() ? v.library + "@" + ordinal : v.name;
    const ImportState state = v.name.empty() ? ImportState::Missing : lr::kernel::variable_state(v.name);
    const char* status = "missing";
    switch (state) {
      case ImportState::Implemented:
        status = "implemented";
        ++implemented;
        break;
      case ImportState::Stub:
        status = "stub";
        ++stub;
        break;
      case ImportState::Missing:
        ++missing;
        break;
    }
    std::printf("| %s | variable | %s |\n", name.c_str(), status);
  }
  // Spec section 5.3: functions and variables are counted separately in the total.
  std::printf("\nImplemented: %zu, stub: %zu, missing: %zu / %zu functions + %zu variables\n", implemented, stub,
              missing, lr_import_count, variables.size());
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

// Runs the main guest thread (waited for on a helper thread) and keeps the SDL window alive on the main thread.
int run_with_window(uint32_t entry, uint32_t stack_size) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "error: SDL_Init failed: %s (use --headless to run without a window)\n", SDL_GetError());
    return kExitUsage;
  }
  SDL_Window* win = SDL_CreateWindow("LegacyRecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, 0);
  if (!win) {
    std::fprintf(stderr, "error: SDL_CreateWindow failed: %s (use --headless to run without a window)\n", SDL_GetError());
    SDL_Quit();
    return kExitUsage;
  }
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
    std::fflush(nullptr);
    _exit(0);
  }
  runner.join();
  SDL_DestroyWindow(win);
  SDL_Quit();
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--headless") opt.headless = true;
    else if (a == "--coverage") opt.coverage = true;
    else if (a == "--strict-memory") opt.strict_memory = true;
    else if (a == "--image" || a == "--data") {
      if (i + 1 >= argc) return usage(("missing value for " + a).c_str());
      (a == "--image" ? opt.image : opt.data) = argv[++i];
    } else {
      return usage(("unknown argument " + a).c_str());
    }
  }
  if (opt.coverage) return print_coverage(opt.image);
  if (opt.image.empty()) return usage("--image is required");

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
  cfg.game_dir = std::filesystem::path(opt.image).parent_path();
  if (!opt.data.empty()) cfg.user_dir = opt.data;
  cfg.strict_memory = opt.strict_memory;
  if (auto r = lr::kernel::init_kernel(mem.value(), std::move(cfg)); !r) {
    std::fprintf(stderr, "error: %s\n", r.error().message.c_str());
    return kExitUsage;
  }

  const auto image_bytes = std::as_bytes(std::span(bytes));
  auto img = load_image(mem.value(), image_bytes);
  if (!img) {
    std::fprintf(stderr, "error: cannot load %s: %s\n", opt.image.c_str(), img.error().message.c_str());
    return kExitUsage;
  }
  if (img.value().base != PPC_IMAGE_BASE) {
    std::fprintf(stderr, "error: image base 0x%08X does not match the recompiled build (0x%08llX)\n", img.value().base,
                 static_cast<unsigned long long>(PPC_IMAGE_BASE));
    return kExitUsage;
  }
  if (is_xex2(bytes)) {  // the test ELF images have no import records
    auto header = lr::formats::parse_xex2(image_bytes);
    lr::Result<void> r = header ? lr::kernel::resolve_variable_imports(mem.value(), img.value(), header.value())
                                : lr::Result<void>(header.error());
    if (!r) {
      std::fprintf(stderr, "error: cannot resolve the imports of %s: %s\n", opt.image.c_str(), r.error().message.c_str());
      return kExitUsage;
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
    return kExitUsage;
  }
  if (!entry_fn) {
    std::fprintf(stderr, "error: no recompiled function at entry point 0x%08X\n", img.value().entry);
    return kExitUsage;
  }
  if (auto r = install_fault_handlers(mem.value()); !r) {
    std::fprintf(stderr, "error: %s\n", r.error().message.c_str());
    return kExitUsage;
  }

  g_guest_base = mem.value().base();
  lr::kernel::set_guest_glue({glue_call, glue_has_function});
  // Every guest thread gets a copy of the XEX TLS template; ExCreateThread with stack size 0 uses the image's.
  if (auto r = lr::kernel::kernel().threads().set_image(img.value().tls, img.value().stack_size, img.value().base,
                                                        img.value().size);
      !r) {
    std::fprintf(stderr, "error: cannot load %s: %s\n", opt.image.c_str(), r.error().message.c_str());
    return kExitUsage;
  }

  const uint32_t entry = img.value().entry, stack_size = img.value().stack_size;
  const int rc = opt.headless ? lr::kernel::run_main_thread(entry, stack_size) : run_with_window(entry, stack_size);
  // The main guest thread has returned (or the window failed): end the process now, taking any other guest threads
  // down with it, without running static destructors under their feet.
  std::fflush(nullptr);
  _exit(rc);
}
