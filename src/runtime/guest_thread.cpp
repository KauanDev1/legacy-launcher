// SPDX-License-Identifier: MIT
#include "runtime/guest_thread.h"

#include <pthread.h>

#include <cstdio>
#include <cstring>
#include <exception>

#include "runtime/be.h"
#include "runtime/fault.h"
#include "runtime/kernel/registry.h"

namespace lr::runtime {

namespace {
struct ThreadArgs {
  GuestMemory* mem;
  GuestBody* body;
  void* user;
  int result = kExitFault;
};

void* thread_main(void* p) {
  auto* a = static_cast<ThreadArgs*>(p);
  if (auto r = install_thread_altstack(); !r) {
    std::fprintf(stderr, "guest thread: cannot install sigaltstack: %s\n", r.error().message.c_str());
    a->result = kExitFault;
    return nullptr;
  }
  try {
    a->body(a->mem->base(), a->user);
    a->result = kExitOk;
  } catch (const lr::kernel::UnimplementedImport& e) {
    std::fputs(lr::kernel::format_report(e).c_str(), stderr);
    a->result = kExitUnimplementedImport;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "guest thread: uncaught exception: %s\n", e.what());
    a->result = kExitFault;
  } catch (...) {
    std::fprintf(stderr, "guest thread: uncaught non-standard exception\n");
    a->result = kExitFault;
  }
  return nullptr;
}
}  // namespace

void init_guest_pcr(GuestMemory& mem) {
  std::memset(mem.translate<uint8_t>(kGuestThreadAddress), 0, 0x400);
  std::memset(mem.translate<uint8_t>(kGuestPcrAddress), 0, 0x150);
  auto put32 = [&](uint32_t off, uint32_t v) { *mem.translate<be<uint32_t>>(kGuestPcrAddress + off) = v; };
  put32(0x00, kGuestTlsAddress);
  put32(0x30, kGuestPcrAddress);
  put32(0x70, kGuestStackBase + kGuestStackSize);
  put32(0x74, kGuestStackBase);
  put32(0x100, kGuestThreadAddress);
  *mem.translate<uint8_t>(kGuestPcrAddress + 0x10C) = 0;
}

int run_guest(GuestMemory& mem, GuestBody* body, void* user) {
  ThreadArgs args{&mem, body, user};
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
