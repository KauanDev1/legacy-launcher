// SPDX-License-Identifier: MIT
// Video *queries* (spec section 1 and 5.2): fixed answers describing a 1280x720 widescreen 60 Hz progressive HDMI
// output. Values follow Xenia's xboxkrnl_video.cc / xam_video.cc / xam_module.cc (semantic reference only, no code
// copied). The imports that create and drive the GPU follow (M3), backed by runtime/gpu/gpu_system.h.
#include <cstdint>
#include <cstring>

#include "runtime/gpu/gpu_system.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/registers.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/xam.h"
#include "runtime/memory/physical_heap.h"
#include "runtime/memory_map.h"

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

// --- The GPU (spec section 5.1): the imports that create and drive it, backed by Kernel::gpu(). ---

namespace {

be<uint32_t>* word(GuestCall& c, uint32_t addr) {
  const GuestPtr<be<uint32_t>> p{addr};
  return p.valid_range(4) && (addr & 3) == 0 ? p.get(c.base) : nullptr;
}

constexpr uint32_t kSwapAreaDwords = 64;  // what D3D reserves in the ring for VdSwap (Xenia zeroes 64 dwords)
constexpr uint32_t kFetchDwords = 6;      // one texture fetch constant

}  // namespace

namespace exports {

// VdInitializeEngines(callback, arg, pfp_microcode*, me_microcode*): 1 (success), like Xenia; the microcode is
// not needed by our command processor.
uint32_t VdInitializeEngines(uint32_t /*callback*/, uint32_t /*arg*/, uint32_t /*pfp*/, uint32_t /*me*/) { return 1; }

// VdSetGraphicsInterruptCallback(callback, user_data): callback(source, user_data), source 0 = vblank, 1 = PM4
// INTERRUPT, run on the kernel worker. Starts the GPU threads (vblanks need no ring).
void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t user_data) {
  gpu::GpuSystem& g = kernel().gpu();
  g.set_interrupt_callback(callback, user_data);
  g.start();
}

// VdInitializeRingBuffer(physical address, size_log2): a 2^(size_log2 + 3)-byte ring; starts the GPU thread.
void VdInitializeRingBuffer(uint32_t phys, uint32_t size_log2) {
  gpu::GpuSystem& g = kernel().gpu();
  g.initialize_ring_buffer(phys, size_log2);
  g.start();
}

// VdEnableRingBufferRPtrWriteBack(physical address, block_size_log2): RPTR (dwords, big-endian) lands there.
void VdEnableRingBufferRPtrWriteBack(uint32_t phys, uint32_t block_log2) {
  kernel().gpu().enable_rptr_writeback(phys, block_log2);
}

// VdGetSystemCommandBuffer(p0*, p1*): Xenia's placeholder values: 0x94 zero bytes at p0 starting with 0xBEEF0000,
// and 0xBEEF0001 at p1.
void VdGetSystemCommandBuffer(GuestCall& c, uint32_t p0, uint32_t p1) {
  if (GuestPtr<uint8_t>{p0}.valid_range(0x94)) {
    std::memset(c.base + p0, 0, 0x94);
    *reinterpret_cast<be<uint32_t>*>(c.base + p0) = 0xBEEF0000u;
  }
  if (be<uint32_t>* w = word(c, p1)) *w = 0xBEEF0001u;
}

// VdInitializeScalerCommandBuffer(10 scaler arguments, dest*, dest_count): fills dest_count dwords with type-2 NOP
// packets (0x80000000) and returns dest_count, as Xenia (the scaler is not emulated).
uint32_t VdInitializeScalerCommandBuffer(GuestCall& c, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                         uint32_t, uint32_t, uint32_t, uint32_t, uint32_t dest, uint32_t count) {
  if (count != 0 && GuestPtr<uint8_t>{dest}.valid_range(uint64_t{count} * 4)) {
    auto* d = reinterpret_cast<be<uint32_t>*>(c.base + dest);
    for (uint32_t i = 0; i < count; ++i) d[i] = 0x80000000u;
  }
  return count;
}

// VdPersistDisplay(unk0, out*): 1; with out, a fresh 64-byte physical block's address is written to it (Xenia
// allocates 64 bytes from a physical heap; what the system does with it is unknown).
uint32_t VdPersistDisplay(GuestCall& c, uint32_t /*unk0*/, uint32_t out) {
  if (be<uint32_t>* w = word(c, out)) {
    auto r = kernel().physical().allocate(64, runtime::kPageReadWrite, 0, 0xFFFFFFFFu, 32);
    *w = r ? r.value() : 0u;
  }
  return 1;
}

// VdSwap(buffer*, fetch_constant*, unk2, unk3, unk4, frontbuffer*, format*, color_space*, width*, height*):
// argument layout per Xenia (width* and height* are the 9th and 10th arguments, on the guest stack). D3D reserved
// kSwapAreaDwords dwords of its ring at buffer; we fill them with a type-0 write of the 6 fetch-constant dwords to
// SHADER_CONSTANT_FETCH_00_0 (base address replaced by the front buffer's physical page), then one LR_SWAP type-3
// packet covering the rest of the area: kSwapMagic, front buffer physical address, width, height, zero padding.
// The command processor executes it in ring order. A front buffer outside the physical views, or a bad pointer,
// leaves the area untouched (Xenia logs and returns too).
void VdSwap(GuestCall& c, uint32_t buffer, uint32_t fetch, uint32_t /*unk2*/, uint32_t /*unk3*/, uint32_t /*unk4*/,
            uint32_t frontbuffer_ptr, uint32_t /*format_ptr*/, uint32_t /*color_space_ptr*/, uint32_t width_ptr,
            uint32_t height_ptr) {
  be<uint32_t>* const fb = word(c, frontbuffer_ptr);
  be<uint32_t>* const w = word(c, width_ptr);
  be<uint32_t>* const h = word(c, height_ptr);
  if (fb == nullptr || w == nullptr || h == nullptr || (buffer & 3) != 0 || (fetch & 3) != 0 ||
      !GuestPtr<uint8_t>{buffer}.valid_range(kSwapAreaDwords * 4) ||
      !GuestPtr<uint8_t>{fetch}.valid_range(kFetchDwords * 4))
    return;
  const uint32_t frontbuffer = *fb;
  if (frontbuffer < runtime::kPhysicalViewA0) return;  // not a physical allocation: no physical address
  const uint32_t phys = runtime::PhysicalHeap::physical_address(frontbuffer);
  uint32_t fetch_words[kFetchDwords];
  const auto* in = reinterpret_cast<const be<uint32_t>*>(c.base + fetch);
  for (uint32_t i = 0; i < kFetchDwords; ++i) fetch_words[i] = in[i];
  fetch_words[1] = (fetch_words[1] & 0xFFFu) | (phys & ~0xFFFu);  // base_address: bits 12-31 of dword 1

  auto* out = reinterpret_cast<be<uint32_t>*>(c.base + buffer);
  for (uint32_t i = 0; i < kSwapAreaDwords; ++i) out[i] = 0u;
  out[0] = ((kFetchDwords - 1) << 16) | gpu::SHADER_CONSTANT_FETCH_00_0;  // type 0
  for (uint32_t i = 0; i < kFetchDwords; ++i) out[1 + i] = fetch_words[i];
  constexpr uint32_t kSwapPayload = kSwapAreaDwords - 1 - kFetchDwords - 1;  // dwords after the LR_SWAP header
  out[1 + kFetchDwords] = (gpu::pm4::kType3 << 30) | ((kSwapPayload - 1) << 16) | (gpu::pm4::LR_SWAP << 8);
  out[2 + kFetchDwords] = gpu::pm4::kSwapMagic;
  out[3 + kFetchDwords] = phys;
  out[4 + kFetchDwords] = static_cast<uint32_t>(*w);
  out[5 + kFetchDwords] = static_cast<uint32_t>(*h);
}

}  // namespace exports

LR_KERNEL(VdInitializeEngines);
LR_KERNEL(VdSetGraphicsInterruptCallback);
LR_KERNEL(VdInitializeRingBuffer);
LR_KERNEL(VdEnableRingBufferRPtrWriteBack);
LR_KERNEL(VdSwap);
LR_KERNEL_STUB_FN(VdGetSystemCommandBuffer);         // Xenia's placeholder values; no system command buffer
LR_KERNEL_STUB_FN(VdInitializeScalerCommandBuffer);  // NOP-filled: the display scaler is not emulated
LR_KERNEL_STUB_FN(VdPersistDisplay);                 // nothing to persist across a title switch on the host
LR_KERNEL_STUB(VdShutdownEngines, 0);                // the GPU lives until the process ends
LR_KERNEL_STUB(VdSetSystemCommandBuffer, 0);         // no system command buffer (the spike: 0 was enough)
LR_KERNEL_STUB(VdQuerySystemCommandBuffer, 0);       // same; Xenia has no entry for it
LR_KERNEL_STUB(VdSetSystemCommandBufferGpuIdentifierAddress, 0);  // Xenia ignores it too
LR_KERNEL_STUB(VdRetrainEDRAM, 0);                   // no EDRAM link to train on the host
LR_KERNEL_STUB(VdRetrainEDRAMWorker, 0);             // same
LR_KERNEL_STUB(VdCallGraphicsNotificationRoutines, 0);  // Xenia calls no routine either
LR_KERNEL_STUB(VdSetDisplayMode, 0);                 // fixed 1280x720 output
LR_KERNEL_STUB(VdSetDisplayModeOverride, 0);         // same
LR_KERNEL_STUB(VdEnableDisableClockGating, 0);       // no clocks to gate

}  // namespace lr::kernel
