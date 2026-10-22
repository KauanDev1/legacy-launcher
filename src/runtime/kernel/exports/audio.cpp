// SPDX-License-Identifier: MIT
// Silent audio driver (M3): the render driver client API answers like a working driver but never calls the
// registered callback and plays nothing; real output is M4. Argument orders follow Xenia's xboxkrnl_audio.cc
// (semantic reference only, no code copied).
#include <cstdint>

#include "runtime/kernel/kernel.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"

namespace lr::kernel {
namespace {
using U32Ptr = GuestPtr<be<uint32_t>>;
using F32Ptr = GuestPtr<be<float>>;
constexpr uint32_t kDriverHandle = 0x41550000;  // 'AU' << 16 | client index 0
}  // namespace

namespace exports {

// XAudioRegisterRenderDriverClient(callback*, driver*): {callback, argument} is read by a real driver; ours keeps
// no client, never reads it (so, as Xenia, it is not validated) and hands out the fixed handle.
uint32_t XAudioRegisterRenderDriverClient(GuestCall& c, U32Ptr /*callback*/, U32Ptr driver_out) {
  if (!driver_out.valid_range(4)) return X_E_INVALIDARG;
  *driver_out.get(c.base) = kDriverHandle;
  return 0;
}

// XAudioUnregisterRenderDriverClient(driver): nothing was registered.
uint32_t XAudioUnregisterRenderDriverClient(uint32_t /*driver*/) { return 0; }

// XAudioSubmitRenderDriverFrame(driver, samples*): the frame is dropped (silent output).
uint32_t XAudioSubmitRenderDriverFrame(uint32_t /*driver*/, uint32_t /*samples*/) { return 0; }

// XAudioGetVoiceCategoryVolume(category, volume*): full volume for every category.
uint32_t XAudioGetVoiceCategoryVolume(GuestCall& c, uint32_t /*category*/, F32Ptr volume_out) {
  if (volume_out.valid_range(4)) *volume_out.get(c.base) = 1.0f;
  return 0;
}

}  // namespace exports

LR_KERNEL_STUB_FN(XAudioRegisterRenderDriverClient);    // handle 0x41550000, callback never invoked (silent, M4)
LR_KERNEL_STUB_FN(XAudioUnregisterRenderDriverClient);  // nothing was registered
LR_KERNEL_STUB_FN(XAudioSubmitRenderDriverFrame);       // frames dropped, no output device
LR_KERNEL_STUB_FN(XAudioGetVoiceCategoryVolume);        // constant 1.0f

}  // namespace lr::kernel
