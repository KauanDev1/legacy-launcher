// SPDX-License-Identifier: MIT
// Kinect (NUI) as seen by a console with no sensor attached. Structure layout per Xenia's xam_nui.cc (semantic
// reference only, no code copied).
#include <cstdint>
#include <cstring>

#include "runtime/kernel/kernel.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"

namespace lr::kernel {
namespace {
constexpr uint32_t kDeviceStatusBytes = 24;  // X_NUI_DEVICE_STATUS: six dwords, the 4th is the status (0: absent)
}  // namespace

namespace exports {

// XamNuiGetDeviceStatus(status*): the whole structure zeroed, so status 0 = no sensor connected.
uint32_t XamNuiGetDeviceStatus(GuestCall& c, GuestPtr<uint8_t> status) {
  if (status.valid_range(kDeviceStatusBytes)) std::memset(status.get(c.base), 0, kDeviceStatusBytes);
  return 0;
}

}  // namespace exports

LR_KERNEL_STUB_FN(XamNuiGetDeviceStatus);  // no Kinect sensor on the host: status 0 (not connected)

}  // namespace lr::kernel
