// SPDX-License-Identifier: MIT
// Video *queries* (spec section 1 and 5.2): fixed answers describing a 1280x720 widescreen 60 Hz progressive HDMI
// output. Values follow Xenia's xboxkrnl_video.cc / xam_video.cc / xam_module.cc (semantic reference only, no code
// copied). The Vd* imports that create or drive the GPU (VdInitializeEngines, VdSetGraphicsInterruptCallback, the
// ring buffer, VdSwap, ...) stay missing: reaching the first of them is the M2 finish line.
#include <cstdint>
#include <cstring>

#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/xam.h"

namespace lr::kernel {
namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;
constexpr float kRefreshRate = 60.0f;

void fill_video_mode(X_VIDEO_MODE* m) {
  std::memset(m, 0, sizeof(*m));
  m->display_width = kWidth;
  m->display_height = kHeight;
  m->is_interlaced = 0;
  m->is_widescreen = 1;
  m->is_hi_def = 1;
  m->refresh_rate = kRefreshRate;
  m->video_standard = 1;  // NTSC
  m->unknown_0x8a = 0x4A;  // Xenia's values for the two unknown fields
  m->unknown_0x01 = 0x01;
}

}  // namespace

namespace exports {

// VdQueryVideoMode(mode*) / XGetVideoMode(mode*): X_VIDEO_MODE of the fixed output.
void VdQueryVideoMode(GuestCall& c, GuestPtr<X_VIDEO_MODE> mode) {
  if (mode.valid_range(sizeof(X_VIDEO_MODE))) fill_video_mode(mode.get(c.base));
}
void XGetVideoMode(GuestCall& c, GuestPtr<X_VIDEO_MODE> mode) { VdQueryVideoMode(c, mode); }

// VdGetCurrentDisplayInformation(info*): the front buffer and display both 1280x720, scaler 1:1, Xenia's overscan.
void VdGetCurrentDisplayInformation(GuestCall& c, GuestPtr<X_DISPLAY_INFO> info) {
  if (!info.valid_range(sizeof(X_DISPLAY_INFO))) return;
  X_DISPLAY_INFO* d = info.get(c.base);
  std::memset(d, 0, sizeof(*d));
  d->front_buffer_width = kWidth;
  d->front_buffer_height = kHeight;
  d->scaler_parameters.scaler_source_rect.x2 = kWidth;
  d->scaler_parameters.scaler_source_rect.y2 = kHeight;
  d->scaler_parameters.scaled_output_width = kWidth;
  d->scaler_parameters.scaled_output_height = kHeight;
  d->scaler_parameters.horizontal_filter_type = 1;
  d->scaler_parameters.vertical_filter_type = 1;
  d->display_window_overscan_left = 320;
  d->display_window_overscan_top = 180;
  d->display_window_overscan_right = 320;
  d->display_window_overscan_bottom = 180;
  d->display_width = kWidth;
  d->display_height = kHeight;
  d->display_refresh_rate = kRefreshRate;
  d->actual_display_width = kWidth;
}

// VdGetCurrentDisplayGamma(type*, power*): Xenia's defaults, type 2 with power 2.22222233.
void VdGetCurrentDisplayGamma(GuestCall& c, GuestPtr<be<uint32_t>> type, GuestPtr<be<float>> power) {
  if (type.valid_range(4)) *type.get(c.base) = 2;
  if (power.valid_range(4)) *power.get(c.base) = 2.22222233f;
}

}  // namespace exports

LR_KERNEL_STUB_FN(VdQueryVideoMode);                // fixed 1280x720 widescreen mode, no display to ask
LR_KERNEL_STUB_FN(XGetVideoMode);                   // same fixed mode as VdQueryVideoMode
LR_KERNEL_STUB_FN(VdGetCurrentDisplayInformation);  // fixed 1280x720 display, no display to ask
LR_KERNEL_STUB_FN(VdGetCurrentDisplayGamma);        // fixed gamma (Xenia's defaults), no display to ask
// Widescreen (1) | at least 1024 wide (2), derived from the fixed mode as Xenia does.
LR_KERNEL_STUB(VdQueryVideoFlags, 3);
// Xenia's value: games treat anything but 3/4/6/8 as PAL; the console's HDMI/component packs are in that set.
LR_KERNEL_STUB(XGetAVPack, 6);
LR_KERNEL_STUB(XGetVideoCapabilities, 0);      // no extra capability bits (Xenia returns 0)
LR_KERNEL_STUB(VdIsHSIOTrainingSucceeded, 1);  // TRUE: no high-speed I/O link to train on the host

}  // namespace lr::kernel
