// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/guest_thread.h"

#include <pthread.h>

#include <cstdio>
#include <exception>

#include "runtime/fault.h"
#include "runtime/kernel/registry.h"

// ppc_context.h insists on a ppc_config.h; none of its macros we use depend on the game's constants.
#define PPC_CONFIG_H_INCLUDED
#include "ppc_context.h"

namespace lr::runtime {

namespace {
struct ThreadArgs {
  GuestMemory* mem;
  uint32_t entry;
  GuestEntryFn* fn;
  int result = kExitFault;
};

void* thread_main(void* p) {
  auto* a = static_cast<ThreadArgs*>(p);
  if (auto r = install_thread_altstack(); !r) {
    std::fprintf(stderr, "guest thread: cannot install sigaltstack: %s\n", r.error().message.c_str());
    a->result = kExitFault;
    return nullptr;
  }
  PPCContext ctx{};
  ctx.r1.u64 = kGuestStackBase + kGuestStackSize - kGuestStackTopSlack;
  ctx.r13.u64 = kGuestPcrAddress;
  try {
    a->fn(ctx, a->mem->base());
    a->result = kExitOk;
  } catch (const lr::kernel::UnimplementedImport& e) {
    std::fputs(lr::kernel::format_report(e).c_str(), stderr);
    a->result = kExitUnimplementedImport;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "guest thread (entry 0x%08X): uncaught exception: %s\n", a->entry, e.what());
    a->result = kExitFault;
  } catch (...) {
    std::fprintf(stderr, "guest thread (entry 0x%08X): uncaught non-standard exception\n", a->entry);
    a->result = kExitFault;
  }
  return nullptr;
}
}  // namespace

int run_guest(GuestMemory& mem, uint32_t entry, GuestEntryFn* entry_fn) {
  ThreadArgs args{&mem, entry, entry_fn};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kHostStackSize);
  pthread_t th;
  const int rc = pthread_create(&th, &attr, thread_main, &args);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    std::fprintf(stderr, "cannot create guest thread (error %d)\n", rc);
    return kExitFault;
  }
  pthread_join(th, nullptr);
  return args.result;
}

}  // namespace lr::runtime
