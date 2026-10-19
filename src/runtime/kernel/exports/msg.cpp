// SPDX-License-Identifier: MIT
// XAM messages to system "apps" (XMsg*). Semantics follow Xenia (xam_msg.cc, app_manager.cc, apps/xam_app.cc);
// semantic reference only, no code copied.
//
// Only the XAM app (0xFE) exists, with the messages Xenia answers: 0x00022005 and 0x00021012 succeed without touching
// the buffer, 0x00020021 writes 0 through the pointer at buffer+0x44. Any other message of that app is X_E_FAIL and
// any other app is X_E_NOTFOUND (XMsgStartIORequest turns it into X_E_INVALIDARG, as Xenia). Deviation: the thread's
// last error is not set (this kernel does not maintain it).
#include <cstdint>

#include "runtime/be.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/xam.h"

namespace lr::kernel {
namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;

constexpr uint32_t kXamApp = 0xFE;
constexpr uint32_t kXSuccess = 0;            // X_E_SUCCESS
constexpr uint32_t kXFail = 0x80004005;      // X_E_FAIL
constexpr uint32_t kXNotFound = 0x80070490;  // X_E_NOTFOUND

uint32_t dispatch(const GuestCall& c, uint32_t app, uint32_t message, uint32_t buffer, uint32_t length) {
  if (app != kXamApp) return kXNotFound;
  switch (message) {
    case 0x00022005:
    case 0x00021012:
      return kXSuccess;
    case 0x00020021: {
      if (length != 0x4C || !U32Ptr{buffer}.valid_range(0x4C)) return X_E_INVALIDARG;
      const U32Ptr out{static_cast<uint32_t>(*U32Ptr{buffer + 0x44}.get(c.base))};
      if (out.valid_range(4)) *out.get(c.base) = 0;
      return kXSuccess;
    }
    default:
      return kXFail;
  }
}

}  // namespace

namespace exports {

// XMsgStartIORequest(app, message, overlapped*, buffer, length): the message completes at once; with an overlapped,
// its result goes there and the call returns ERROR_IO_PENDING.
uint32_t XMsgStartIORequest(GuestCall& c, uint32_t app, uint32_t message, uint32_t overlapped, uint32_t buffer,
                            uint32_t length) {
  uint32_t result = dispatch(c, app, message, buffer, length);
  if (result == kXNotFound) result = X_E_INVALIDARG;
  if (overlapped != 0) {
    complete_overlapped(overlapped, result, 0);
    return ERROR_IO_PENDING;
  }
  return result;
}

// XMsgInProcessCall(app, message, arg1, arg2): synchronous message with arg1 as the buffer and arg2 its length.
uint32_t XMsgInProcessCall(GuestCall& c, uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2) {
  return dispatch(c, app, message, arg1, arg2);
}

// XMsgSystemProcessCall(app, message, buffer, length).
uint32_t XMsgSystemProcessCall(GuestCall& c, uint32_t app, uint32_t message, uint32_t buffer, uint32_t length) {
  return dispatch(c, app, message, buffer, length);
}

}  // namespace exports

LR_KERNEL(XMsgStartIORequest);
LR_KERNEL(XMsgInProcessCall);
LR_KERNEL(XMsgSystemProcessCall);

}  // namespace lr::kernel
