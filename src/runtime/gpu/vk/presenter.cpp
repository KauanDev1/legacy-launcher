// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/presenter.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/log.h"
#include "runtime/gpu/screenshot.h"
#include "vk_mem_alloc.h"

#ifdef LR_HAVE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#endif

namespace lr::gpu::vk {
namespace {

constexpr VkFormat kFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint64_t kAcquireTimeoutNs = 1'000'000'000;  // a hidden window must not block the GPU thread forever

Error vk_error(const char* what, VkResult r) {
  return Error{ErrorCode::Internal, std::string(what) + " failed (VkResult " + std::to_string(r) + ")", ""};
}

template <class T>
T make(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                   VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier b = make<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool can_blit_to(VkPhysicalDevice pd, VkFormat format) {
  VkFormatProperties p;
  vkGetPhysicalDeviceFormatProperties(pd, format, &p);
  return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
}

// The surface format the window gets: a UNORM 8-bit format we can blit to (the frames are raw UNORM values).
Result<VkSurfaceFormatKHR> pick_surface_format(VkPhysicalDevice pd, VkSurfaceKHR surface) {
  std::uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &n, formats.data());
  for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32}) {
    for (const VkSurfaceFormatKHR& f : formats)
      if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && can_blit_to(pd, f.format)) return f;
  }
  for (const VkSurfaceFormatKHR& f : formats)
    if (can_blit_to(pd, f.format)) return f;
  return Error{ErrorCode::ToolMissing, "the window surface has no format the Vulkan device can blit to",
               "update the Vulkan driver"};
}

}  // namespace

Result<Presenter> Presenter::create(Device& device, void* sdl_window) {
  Presenter p;
  p.device_ = &device;
  p.window_ = sdl_window;
  const VkDevice d = device.device();
  if (sdl_window != nullptr) {
    if (device.surface() == VK_NULL_HANDLE)
      return Error{ErrorCode::Internal, "Presenter: a window was given but the Device is headless", ""};
    VkSurfaceCapabilitiesKHR caps;
    if (VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.physical_device(), device.surface(), &caps);
        r != VK_SUCCESS)
      return vk_error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
      return Error{ErrorCode::ToolMissing, "the window surface does not accept transfer writes (TRANSFER_DST)",
                   "update the Vulkan driver"};
    LR_TRY(pick_surface_format(device.physical_device(), device.surface()));
  }

  VkCommandPoolCreateInfo cpci = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = device.graphics_family();
  if (VkResult r = vkCreateCommandPool(d, &cpci, nullptr, &p.pool_); r != VK_SUCCESS)
    return vk_error("vkCreateCommandPool", r);
  VkCommandBufferAllocateInfo cbai = make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  cbai.commandPool = p.pool_;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  if (VkResult r = vkAllocateCommandBuffers(d, &cbai, &p.cmd_); r != VK_SUCCESS)
    return vk_error("vkAllocateCommandBuffers", r);
  VkFenceCreateInfo fci = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
  if (VkResult r = vkCreateFence(d, &fci, nullptr, &p.fence_); r != VK_SUCCESS) return vk_error("vkCreateFence", r);
  if (sdl_window != nullptr) {
    VkSemaphoreCreateInfo sci = make<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    if (VkResult r = vkCreateSemaphore(d, &sci, nullptr, &p.acquired_); r != VK_SUCCESS)
      return vk_error("vkCreateSemaphore", r);
  }
  return p;
}

Presenter::Presenter(Presenter&& o) noexcept { *this = std::move(o); }

Presenter& Presenter::operator=(Presenter&& o) noexcept {
  if (this != &o) {
    destroy();
    device_ = std::exchange(o.device_, nullptr);
    window_ = std::exchange(o.window_, nullptr);
    pool_ = std::exchange(o.pool_, VK_NULL_HANDLE);
    cmd_ = std::exchange(o.cmd_, VK_NULL_HANDLE);
    fence_ = std::exchange(o.fence_, VK_NULL_HANDLE);
    acquired_ = std::exchange(o.acquired_, VK_NULL_HANDLE);
    last_ = std::exchange(o.last_, VK_NULL_HANDLE);
    last_alloc_ = std::exchange(o.last_alloc_, nullptr);
    last_w_ = std::exchange(o.last_w_, 0);
    last_h_ = std::exchange(o.last_h_, 0);
    frames_ = std::exchange(o.frames_, 0);
    swapchain_ = std::exchange(o.swapchain_, VK_NULL_HANDLE);
    swapchain_format_ = o.swapchain_format_;
    extent_ = o.extent_;
    images_ = std::move(o.images_);
    rendered_ = std::move(o.rendered_);
    recreate_ = o.recreate_;
    drawable_w_ = o.drawable_w_;
    drawable_h_ = o.drawable_h_;
    stop_ = std::exchange(o.stop_, nullptr);
    abandoned_ = o.abandoned_;
  }
  return *this;
}

Presenter::~Presenter() { destroy(); }

void Presenter::destroy() {
  if (device_ == nullptr) return;
  const VkDevice d = device_->device();
  vkDeviceWaitIdle(d);
  destroy_swapchain();
  if (last_) vmaDestroyImage(device_->allocator(), last_, last_alloc_);
  if (acquired_) vkDestroySemaphore(d, acquired_, nullptr);
  if (fence_) vkDestroyFence(d, fence_, nullptr);
  if (pool_) vkDestroyCommandPool(d, pool_, nullptr);  // frees cmd_
  last_ = VK_NULL_HANDLE;
  last_alloc_ = nullptr;
  acquired_ = VK_NULL_HANDLE;
  fence_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  cmd_ = VK_NULL_HANDLE;
  device_ = nullptr;
}

void Presenter::destroy_swapchain() {
  const VkDevice d = device_->device();
  for (VkSemaphore s : rendered_) vkDestroySemaphore(d, s, nullptr);
  rendered_.clear();
  images_.clear();
  if (swapchain_) vkDestroySwapchainKHR(d, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  extent_ = {0, 0};
}

Result<void> Presenter::create_swapchain() {
  const VkDevice d = device_->device();
  const VkPhysicalDevice pd = device_->physical_device();
  const VkSurfaceKHR surface = device_->surface();
  VkSurfaceCapabilitiesKHR caps;
  if (VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps); r != VK_SUCCESS)
    return vk_error("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
  int dw = 0, dh = 0;
  drawable_size(dw, dh);
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {  // the surface takes the swapchain's size (Wayland): the window's drawable size
    extent.width = std::clamp(dw > 0 ? uint32_t(dw) : last_w_, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(dh > 0 ? uint32_t(dh) : last_h_, caps.minImageExtent.height, caps.maxImageExtent.height);
  }
  if (extent.width == 0 || extent.height == 0) {  // minimised: no swapchain until it is shown again
    destroy_swapchain();
    return {};
  }
  LR_TRY_ASSIGN(format, pick_surface_format(pd, surface));

  std::uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount != 0) count = std::min(count, caps.maxImageCount);
  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
    if (caps.supportedCompositeAlpha & a) {
      alpha = a;
      break;
    }
  }
  VkSwapchainCreateInfoKHR sci = make<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
  sci.surface = surface;
  sci.minImageCount = count;
  sci.imageFormat = format.format;
  sci.imageColorSpace = format.colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = alpha;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always supported; paces the window at the display rate
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = swapchain_;  // the old one (if any) hands its resources over; the device is idle
  VkSwapchainKHR created = VK_NULL_HANDLE;
  const VkResult cr = vkCreateSwapchainKHR(d, &sci, nullptr, &created);
  if (swapchain_) vkDestroySwapchainKHR(d, swapchain_, nullptr);  // retired either way
  swapchain_ = VK_NULL_HANDLE;
  if (cr != VK_SUCCESS) {
    destroy_swapchain();
    return vk_error("vkCreateSwapchainKHR", cr);
  }
  swapchain_ = created;
  swapchain_format_ = format.format;
  extent_ = extent;
  drawable_w_ = dw;
  drawable_h_ = dh;
  std::uint32_t n = 0;
  vkGetSwapchainImagesKHR(d, swapchain_, &n, nullptr);
  images_.resize(n);
  vkGetSwapchainImagesKHR(d, swapchain_, &n, images_.data());
  // The "rendered" semaphores outlive a recreation while the image count stays the same (the device is idle, so
  // none is still waited on); otherwise they are made again, one per image.
  if (rendered_.size() == n) return {};
  for (VkSemaphore s : rendered_) vkDestroySemaphore(d, s, nullptr);
  rendered_.clear();
  VkSemaphoreCreateInfo semi = make<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
  for (std::uint32_t i = 0; i < n; ++i) {
    VkSemaphore s = VK_NULL_HANDLE;
    if (VkResult r = vkCreateSemaphore(d, &semi, nullptr, &s); r != VK_SUCCESS) return vk_error("vkCreateSemaphore", r);
    rendered_.push_back(s);
  }
  return {};
}

void Presenter::drawable_size(int& w, int& h) const {
  w = h = 0;
#ifdef LR_HAVE_SDL2
  if (window_ != nullptr) SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(window_), &w, &h);
#endif
}

Result<void> Presenter::ensure_last_frame(uint32_t w, uint32_t h) {
  if (last_ && last_w_ == w && last_h_ == h) return {};
  if (last_) {
    vmaDestroyImage(device_->allocator(), last_, last_alloc_);  // idle: every present waits for its submission
    last_ = VK_NULL_HANDLE;
    last_alloc_ = nullptr;
  }
  VkImageCreateInfo ici = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = kFrameFormat;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  VmaAllocation alloc = nullptr;
  if (VkResult r = vmaCreateImage(device_->allocator(), &ici, &aci, &last_, &alloc, nullptr); r != VK_SUCCESS)
    return vk_error("vmaCreateImage (presenter frame)", r);
  last_alloc_ = alloc;
  last_w_ = w;
  last_h_ = h;
  return {};
}

void Presenter::present(VkImage frame, uint32_t w, uint32_t h) {
  if (device_ == nullptr || abandoned_ || frame == VK_NULL_HANDLE || w == 0 || h == 0) return;
  const VkDevice d = device_->device();
  if (auto r = ensure_last_frame(w, h); !r) {
    log::error("[gpu] present: " + r.error().message);
    return;
  }

  // Window: the swapchain image this frame goes to (none when minimised, out of date or not ready in time).
  std::uint32_t index = UINT32_MAX;
  if (window_ != nullptr) {
    // A resized window: Wayland reports no out-of-date swapchain, and X11 may report it late.
    int dw = 0, dh = 0;
    drawable_size(dw, dh);
    if (swapchain_ != VK_NULL_HANDLE && (dw != drawable_w_ || dh != drawable_h_)) recreate_ = true;
    if (recreate_ || swapchain_ == VK_NULL_HANDLE) {
      vkDeviceWaitIdle(d);
      recreate_ = false;
      if (auto r = create_swapchain(); !r) log::error("[gpu] present: " + r.error().message);
    }
    if (swapchain_ != VK_NULL_HANDLE) {
      const VkResult r = vkAcquireNextImageKHR(d, swapchain_, kAcquireTimeoutNs, acquired_, VK_NULL_HANDLE, &index);
      if (r == VK_SUBOPTIMAL_KHR) {
        recreate_ = true;  // still presentable this time
      } else if (r != VK_SUCCESS) {
        if (r == VK_ERROR_OUT_OF_DATE_KHR) recreate_ = true;
        else if (r != VK_TIMEOUT && r != VK_NOT_READY) log::error("[gpu] vkAcquireNextImageKHR: " + std::to_string(r));
        index = UINT32_MAX;
      }
    }
  }

  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd_, &begin);
  // The frame's writes (earlier submissions on this queue) become visible to the copy below.
  VkMemoryBarrier mb = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd_,
                       VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  image_barrier(cmd_, last_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkImageCopy copy{};
  copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.extent = {w, h, 1};
  vkCmdCopyImage(cmd_, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, last_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                 &copy);
  image_barrier(cmd_, last_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);

  if (index != UINT32_MAX) {
    const VkImage target = images_[index];
    // The acquire semaphore is waited at the transfer stage: the layout change below runs after it.
    image_barrier(cmd_, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black{};
    black.float32[3] = 1.0f;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    image_barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    // Fit the frame in the window, aspect kept (black bars).
    const double scale = std::min(double(extent_.width) / w, double(extent_.height) / h);
    const int32_t dw = std::max(1, int32_t(w * scale)), dh = std::max(1, int32_t(h * scale));
    const int32_t dx = (int32_t(extent_.width) - dw) / 2, dy = (int32_t(extent_.height) - dh) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {dx, dy, 0};
    blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
    vkCmdBlitImage(cmd_, last_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
    image_barrier(cmd_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  }
  vkEndCommandBuffer(cmd_);

  VkSubmitInfo si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  if (index != UINT32_MAX) {
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait_stage;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_[index];
  }
  if (VkResult r = vkQueueSubmit(device_->graphics_queue(), 1, &si, fence_); r != VK_SUCCESS) {
    log::error("[gpu] present: vkQueueSubmit failed (VkResult " + std::to_string(r) + ")");
    return;
  }
  if (index != UINT32_MAX) {
    VkPresentInfoKHR pi = make<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    const VkResult r = vkQueuePresentKHR(device_->graphics_queue(), &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_ = true;
    else if (r != VK_SUCCESS) log::error("[gpu] vkQueuePresentKHR: " + std::to_string(r));
  }
  if (!wait_fence(d, fence_, stop_)) {
    abandoned_ = true;  // stopping (or device lost): this frame is dropped and the presenter does nothing more
    return;
  }
  vkResetFences(d, 1, &fence_);
  ++frames_;
}

Result<void> Presenter::capture_png(const std::filesystem::path& path) {
  if (device_ == nullptr || abandoned_ || frames_ == 0 || last_ == VK_NULL_HANDLE)
    return Error{ErrorCode::IoError, "cannot capture " + path.string() + ": no frame was presented yet", ""};
  const VkDevice d = device_->device();
  const VkDeviceSize bytes = VkDeviceSize{last_w_} * last_h_ * 4;
  VkBufferCreateInfo bci = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VkBuffer buffer = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  VmaAllocationInfo info{};
  if (VkResult r = vmaCreateBuffer(device_->allocator(), &bci, &aci, &buffer, &alloc, &info); r != VK_SUCCESS)
    return vk_error("vmaCreateBuffer (screenshot)", r);

  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd_, &begin);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {last_w_, last_h_, 1};
  vkCmdCopyImageToBuffer(cmd_, last_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
  VkMemoryBarrier host = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
  host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0,
                       nullptr);
  vkEndCommandBuffer(cmd_);
  VkSubmitInfo si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  VkResult r = vkQueueSubmit(device_->graphics_queue(), 1, &si, fence_);
  if (r == VK_SUCCESS) {
    if (!wait_fence(d, fence_, stop_)) {
      abandoned_ = true;  // the copy may still run: the buffer is left to the device teardown
      return Error{ErrorCode::Cancelled, "cannot capture " + path.string() + ": the GPU is stopping", ""};
    }
    vkResetFences(d, 1, &fence_);
  }
  std::vector<uint8_t> pixels;
  if (r == VK_SUCCESS) {
    vmaInvalidateAllocation(device_->allocator(), alloc, 0, VK_WHOLE_SIZE);
    pixels.resize(static_cast<size_t>(bytes));
    std::memcpy(pixels.data(), info.pMappedData, pixels.size());
  }
  vmaDestroyBuffer(device_->allocator(), buffer, alloc);
  if (r != VK_SUCCESS) return vk_error("vkQueueSubmit (screenshot)", r);
  return write_png(path, last_w_, last_h_, pixels);
}

}  // namespace lr::gpu::vk
