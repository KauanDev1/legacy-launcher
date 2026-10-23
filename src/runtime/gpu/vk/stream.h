// SPDX-License-Identifier: MIT
// The frame's command stream (Task 20): one command buffer shared by the draws, the EDRAM clears and resolves and
// the render-target initialisation, submitted at sync points instead of once per operation. A flush submits what
// was recorded and waits for it (a resolve needs the guest bytes on the CPU right away; swap, wait_idle and the
// texture cache's own uploads flush too), so after a flush the device uses nothing this stream recorded.
//
// That makes the lifetime rules simple and checkable:
//   - destruction of anything a recorded command may use goes through defer(): it runs after the next flush
//     completes (at once when nothing is recorded);
//   - a descriptor rewrite or an image re-upload that earlier recorded commands must not see is preceded by a flush
//     (TextureCache's before-GPU-work hook), so no descriptor is ever updated while a command buffer that uses it
//     is pending: every submission is waited for before the next host-side change.
// One render pass may be open at a time: begin() ends it (transfer, clears and compute work happen outside render
// passes), begin_render_pass() keeps it when the same (render pass, framebuffer) is asked for again.
//
// Thread-safety: GPU thread only.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "common/result.h"
#include "runtime/gpu/vk/device.h"

namespace lr::gpu::vk {

class CommandStream {
 public:
  // `stop` (may be null): raised, a flush's fence wait gives up (Cancelled) and the stream is abandoned: nothing
  // more is submitted (the submission may still be running, so deferred work stays queued until destruction).
  static Result<std::unique_ptr<CommandStream>> create(const Device& device, const std::atomic<bool>* stop = nullptr);
  ~CommandStream();  // waits for the device, then runs the deferred work
  CommandStream(const CommandStream&) = delete;
  CommandStream& operator=(const CommandStream&) = delete;

  // The command buffer being recorded (begun on first use), outside any render pass: an open one is ended.
  Result<VkCommandBuffer> begin();
  // The command buffer inside render pass `pass` on `framebuffer` (render area: `extent`). An open render pass with
  // the same pass and framebuffer is kept; any other is ended first.
  Result<VkCommandBuffer> begin_render_pass(VkRenderPass pass, VkFramebuffer framebuffer, VkExtent2D extent);
  void end_render_pass();
  // True while commands are recorded and not yet submitted.
  bool recording() const { return recording_; }
  // Bumped every time a new command buffer starts recording: bound state (pipeline, descriptor sets) is per value.
  uint64_t generation() const { return generation_; }

  // Submits what was recorded, waits for it, then runs the deferred work and the flush listeners. With nothing
  // recorded it only runs the deferred work. Cancelled when the GPU is stopping.
  Result<void> flush();
  // Waits for the device to be idle and runs every deferred function (shutdown).
  void drain();

  // `fn` runs once the device no longer uses anything recorded so far: after the next completed flush, or now when
  // nothing is recorded.
  void defer(std::function<void()> fn);
  // `fn` runs after every completed flush (the draw path resets its upload ring there); the id removes it.
  uint64_t add_flush_listener(std::function<void()> fn);
  void remove_flush_listener(uint64_t id);

  bool abandoned() const { return abandoned_; }
  uint64_t submissions() const { return submissions_; }  // completed flushes that submitted work (tests)

 private:
  CommandStream(const Device& device, const std::atomic<bool>* stop) : device_(device), stop_(stop) {}
  Result<void> init();
  void run_deferred();

  const Device& device_;
  const std::atomic<bool>* stop_;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool recording_ = false;
  bool abandoned_ = false;
  VkRenderPass open_pass_ = VK_NULL_HANDLE;
  VkFramebuffer open_framebuffer_ = VK_NULL_HANDLE;
  uint64_t generation_ = 0;
  uint64_t submissions_ = 0;
  std::vector<std::function<void()>> deferred_;
  std::map<uint64_t, std::function<void()>> listeners_;
  uint64_t next_listener_ = 1;
};

}  // namespace lr::gpu::vk
