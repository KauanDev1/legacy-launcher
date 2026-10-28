// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/stream.h"

#include <string>
#include <utility>

namespace lr::gpu::vk {
namespace {

template <class T>
T make(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

Error vk_error(const char* what, VkResult r) {
  return Error{ErrorCode::Internal, std::string("command stream: ") + what + " failed (VkResult " + std::to_string(r) + ")",
               ""};
}

}  // namespace

Result<std::unique_ptr<CommandStream>> CommandStream::create(const Device& device, const std::atomic<bool>* stop) {
  std::unique_ptr<CommandStream> s(new CommandStream(device, stop));
  LR_TRY(s->init());
  return s;
}

Result<void> CommandStream::init() {
  const VkDevice d = device_.device();
  VkCommandPoolCreateInfo cpci = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = device_.graphics_family();
  if (VkResult r = vkCreateCommandPool(d, &cpci, nullptr, &pool_); r != VK_SUCCESS)
    return vk_error("vkCreateCommandPool", r);
  VkCommandBufferAllocateInfo cbai = make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  cbai.commandPool = pool_;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  if (VkResult r = vkAllocateCommandBuffers(d, &cbai, &cmd_); r != VK_SUCCESS)
    return vk_error("vkAllocateCommandBuffers", r);
  VkFenceCreateInfo fci = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
  if (VkResult r = vkCreateFence(d, &fci, nullptr, &fence_); r != VK_SUCCESS) return vk_error("vkCreateFence", r);
  return {};
}

CommandStream::~CommandStream() {
  const VkDevice d = device_.device();
  drain();
  if (fence_) vkDestroyFence(d, fence_, nullptr);
  if (pool_) vkDestroyCommandPool(d, pool_, nullptr);
}

Result<VkCommandBuffer> CommandStream::begin() {
  if (abandoned_ || (stop_ && stop_->load(std::memory_order_acquire)))
    return Error{ErrorCode::Cancelled, "command stream: the GPU is stopping", ""};
  if (!recording_) {
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (VkResult r = vkBeginCommandBuffer(cmd_, &bi); r != VK_SUCCESS) return vk_error("vkBeginCommandBuffer", r);
    recording_ = true;
    ++generation_;
  }
  end_render_pass();
  return cmd_;
}

Result<VkCommandBuffer> CommandStream::begin_render_pass(VkRenderPass pass, VkFramebuffer framebuffer,
                                                         VkExtent2D extent) {
  if (recording_ && open_pass_ == pass && open_framebuffer_ == framebuffer) return cmd_;
  LR_TRY_ASSIGN(cmd, begin());
  VkRenderPassBeginInfo rpbi = make<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
  rpbi.renderPass = pass;
  rpbi.framebuffer = framebuffer;
  rpbi.renderArea = {{0, 0}, extent};
  vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  open_pass_ = pass;
  open_framebuffer_ = framebuffer;
  return cmd;
}

void CommandStream::end_render_pass() {
  if (!open_pass_) return;
  vkCmdEndRenderPass(cmd_);
  open_pass_ = VK_NULL_HANDLE;
  open_framebuffer_ = VK_NULL_HANDLE;
}

Result<void> CommandStream::flush() {
  if (!recording_) {
    if (!abandoned_) run_deferred();
    return {};
  }
  end_render_pass();
  recording_ = false;
  if (VkResult r = vkEndCommandBuffer(cmd_); r != VK_SUCCESS) return vk_error("vkEndCommandBuffer", r);
  if (abandoned_ || (stop_ && stop_->load(std::memory_order_acquire)))
    return Error{ErrorCode::Cancelled, "command stream: the GPU is stopping", ""};
  VkSubmitInfo si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (VkResult r = vkQueueSubmit(device_.graphics_queue(), 1, &si, fence_); r != VK_SUCCESS)
    return vk_error("vkQueueSubmit", r);
  if (!wait_fence(device_.device(), fence_, stop_)) {
    abandoned_ = true;
    return Error{ErrorCode::Cancelled, "command stream: submission abandoned, the GPU is stopping", ""};
  }
  vkResetFences(device_.device(), 1, &fence_);
  ++submissions_;
  run_deferred();
  for (auto& [id, fn] : listeners_) fn();
  return {};
}

void CommandStream::drain() {
  if (recording_) {
    // Never submitted (shutdown): ended now, before anything it uses is destroyed; its commands are dropped.
    end_render_pass();
    vkEndCommandBuffer(cmd_);
    recording_ = false;
  }
  if (device_.device()) vkDeviceWaitIdle(device_.device());
  run_deferred();
}

void CommandStream::defer(std::function<void()> fn) {
  if (!recording_ && !abandoned_) {
    fn();
    return;
  }
  deferred_.push_back(std::move(fn));
}

uint64_t CommandStream::add_flush_listener(std::function<void()> fn) {
  const uint64_t id = next_listener_++;
  listeners_.emplace(id, std::move(fn));
  return id;
}

void CommandStream::remove_flush_listener(uint64_t id) { listeners_.erase(id); }

void CommandStream::run_deferred() {
  // A deferred function may defer more (it runs with nothing recorded, so that runs at once).
  std::vector<std::function<void()>> fns;
  fns.swap(deferred_);
  for (auto& fn : fns) fn();
}

}  // namespace lr::gpu::vk
