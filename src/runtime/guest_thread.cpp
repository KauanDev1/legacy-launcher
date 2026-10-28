// SPDX-License-Identifier: MIT
#include "runtime/guest_thread.h"

#include <pthread.h>

#include <string>

namespace lr::runtime {

Result<void> start_host_thread(void* (*fn)(void*), void* arg) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kHostStackSize);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t th;
  const int rc = pthread_create(&th, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  if (rc != 0) return Error{ErrorCode::IoError, "cannot create a host thread", "pthread_create error " + std::to_string(rc)};
  return {};
}

}  // namespace lr::runtime
