// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/result.h"
#include "runtime/be.h"

// Minimal XAM (spec section 10): enumerator handles, XOVERLAPPED completion, notification listeners, the local
// user, plus the guest layouts the video query stubs fill. Layouts and semantics follow Xenia (xbox.h,
// xenumerator.h, xboxkrnl_video.cc; semantic reference only, no code copied). The imports live in
// exports/xam.cpp, exports/video.cpp and exports/net.cpp.

namespace lr::kernel {

using lr::runtime::be;

// XAM_OVERLAPPED (Xenia xbox.h). XAM operations here complete synchronously: they fill result/length/context/
// extended_error, set the event and queue the completion routine as an APC, then return ERROR_IO_PENDING.
struct XAM_OVERLAPPED {
  be<uint32_t> result;              // 0x00 (InternalLow)
  be<uint32_t> length;              // 0x04 (InternalHigh)
  be<uint32_t> context;             // 0x08
  be<uint32_t> event;               // 0x0C
  be<uint32_t> completion_routine;  // 0x10
  be<uint32_t> completion_context;  // 0x14
  be<uint32_t> extended_error;      // 0x18
};
static_assert(sizeof(XAM_OVERLAPPED) == 0x1C);
static_assert(offsetof(XAM_OVERLAPPED, length) == 0x04);
static_assert(offsetof(XAM_OVERLAPPED, event) == 0x0C);
static_assert(offsetof(XAM_OVERLAPPED, extended_error) == 0x18);

// X_KENUMERATOR (Xenia xenumerator.h): the guest body of an enumerator handle; the private structure of
// XamCreateEnumeratorHandle (extra_size bytes) follows it.
struct X_KENUMERATOR {
  be<uint32_t> app_id;               // 0x00
  be<uint32_t> open_message;         // 0x04
  be<uint32_t> close_message;        // 0x08
  be<uint32_t> user_index;           // 0x0C
  be<uint32_t> items_per_enumerate;  // 0x10
  be<uint32_t> flags;                // 0x14
};
static_assert(sizeof(X_KENUMERATOR) == 0x18);
static_assert(offsetof(X_KENUMERATOR, items_per_enumerate) == 0x10);

// X_VIDEO_MODE (Xenia xbox.h, 48 bytes).
struct X_VIDEO_MODE {
  be<uint32_t> display_width;   // 0x00
  be<uint32_t> display_height;  // 0x04
  be<uint32_t> is_interlaced;   // 0x08
  be<uint32_t> is_widescreen;   // 0x0C
  be<uint32_t> is_hi_def;       // 0x10
  be<float> refresh_rate;       // 0x14
  be<uint32_t> video_standard;  // 0x18 (1 = NTSC)
  be<uint32_t> unknown_0x8a;    // 0x1C
  be<uint32_t> unknown_0x01;    // 0x20
  be<uint32_t> reserved[3];     // 0x24
};
static_assert(sizeof(X_VIDEO_MODE) == 0x30);
static_assert(offsetof(X_VIDEO_MODE, refresh_rate) == 0x14);
static_assert(offsetof(X_VIDEO_MODE, unknown_0x01) == 0x20);

// X_DISPLAY_INFO and its scaler parameters (Xenia xboxkrnl_video.cc).
struct X_D3DPRIVATE_RECT {
  be<uint32_t> x1, y1, x2, y2;
};
static_assert(sizeof(X_D3DPRIVATE_RECT) == 0x10);

struct X_D3DFILTER_PARAMETERS {
  be<float> nyquist, flicker_filter, beta;
};
static_assert(sizeof(X_D3DFILTER_PARAMETERS) == 0xC);

struct X_D3DPRIVATE_SCALER_PARAMETERS {
  X_D3DPRIVATE_RECT scaler_source_rect;                 // 0x00
  be<uint32_t> scaled_output_width;                     // 0x10
  be<uint32_t> scaled_output_height;                    // 0x14
  be<uint32_t> vertical_filter_type;                    // 0x18
  X_D3DFILTER_PARAMETERS vertical_filter_parameters;    // 0x1C
  be<uint32_t> horizontal_filter_type;                  // 0x28
  X_D3DFILTER_PARAMETERS horizontal_filter_parameters;  // 0x2C
};
static_assert(sizeof(X_D3DPRIVATE_SCALER_PARAMETERS) == 0x38);
static_assert(offsetof(X_D3DPRIVATE_SCALER_PARAMETERS, horizontal_filter_type) == 0x28);

struct X_DISPLAY_INFO {
  be<uint16_t> front_buffer_width;                   // 0x00
  be<uint16_t> front_buffer_height;                  // 0x02
  uint8_t front_buffer_color_format;                 // 0x04
  uint8_t front_buffer_pixel_format;                 // 0x05
  X_D3DPRIVATE_SCALER_PARAMETERS scaler_parameters;  // 0x08
  be<uint16_t> display_window_overscan_left;         // 0x40
  be<uint16_t> display_window_overscan_top;          // 0x42
  be<uint16_t> display_window_overscan_right;        // 0x44
  be<uint16_t> display_window_overscan_bottom;       // 0x46
  be<uint16_t> display_width;                        // 0x48
  be<uint16_t> display_height;                       // 0x4A
  be<float> display_refresh_rate;                    // 0x4C
  be<uint32_t> display_interlaced;                   // 0x50
  uint8_t display_color_format;                      // 0x54
  be<uint16_t> actual_display_width;                 // 0x56
};
static_assert(sizeof(X_DISPLAY_INFO) == 0x58);
static_assert(offsetof(X_DISPLAY_INFO, scaler_parameters) == 0x08);
static_assert(offsetof(X_DISPLAY_INFO, display_window_overscan_left) == 0x40);
static_assert(offsetof(X_DISPLAY_INFO, display_refresh_rate) == 0x4C);
static_assert(offsetof(X_DISPLAY_INFO, display_color_format) == 0x54);
static_assert(offsetof(X_DISPLAY_INFO, actual_display_width) == 0x56);

// The one local user (user index 0): signed in locally (state 1), offline XUID, no LIVE.
inline constexpr uint64_t kLocalXuid = 0xE000000000000001ull;
inline constexpr char kLocalUserName[] = "Player";

// System notifications (XNID(version 0, area XNCALLER_SYSTEM = 0, index)); the area selects bit `area` of a
// listener's 64-bit mask, so XNOTIFY_SYSTEM = 1.
inline constexpr uint32_t kXnSysUi = 0x00000009;
inline constexpr uint32_t kXnSysSigninChanged = 0x0000000A;
inline constexpr uint32_t kXnSysStorageDevicesChanged = 0x0000000B;

// Creates a XAM enumerator handle over fixed items (`items.size()` must be a multiple of item_size; item_size > 0):
// each XamEnumerate copies up to items_per_enumerate of them and returns ERROR_NO_MORE_FILES once all were handed
// out. `header` initializes the guest X_KENUMERATOR (items_per_enumerate is overwritten); extra_size zeroed bytes
// follow it (the XamCreateEnumeratorHandle private structure). OutOfRange for a bad size, InsufficientResources
// when the kernel pool is exhausted.
Result<uint32_t> create_enumerator(uint32_t item_size, uint32_t items_per_enumerate, std::vector<uint8_t> items,
                                   const X_KENUMERATOR& header = {}, uint32_t extra_size = 0);

// Completes the guest XAM_OVERLAPPED at `overlapped` synchronously (see XAM_OVERLAPPED). Ignores an invalid address.
void complete_overlapped(uint32_t overlapped, uint32_t result, uint32_t length);

}  // namespace lr::kernel
