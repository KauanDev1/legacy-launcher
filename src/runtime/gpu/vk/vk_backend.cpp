// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/vk_backend.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "runtime/gpu/vk/device.h"
#include "runtime/gpu/vk/presenter.h"
#include "vk_mem_alloc.h"

namespace lr::gpu::vk {
namespace {

constexpr uint32_t kMaxFrontBufferSide = 8192;

template <class T>
T make(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

Error vk_error(const char* what, VkResult r) {
  return Error{ErrorCode::Internal, std::string(what) + " failed (VkResult " + std::to_string(r) + ")", ""};
}

}  // namespace

struct VkBackend::Impl {
  // Declaration order is destruction order reversed: the presenter goes before the device it uses.
  Device device;
  std::optional<Presenter> presenter;
  VkBackendOptions options;

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  // Host-visible upload buffer for the front buffer.
  VkBuffer staging = VK_NULL_HANDLE;
  VmaAllocation staging_alloc = nullptr;
  void* staging_map = nullptr;
  VkDeviceSize staging_size = 0;

  // The frame handed to the presenter (R8G8B8A8_UNORM, TRANSFER_SRC_OPTIMAL between swaps).
  VkImage frame = VK_NULL_HANDLE;
  VmaAllocation frame_alloc = nullptr;
  uint32_t frame_w = 0, frame_h = 0;
  bool frame_written = false;

  uint64_t swaps = 0;
  bool warned_size = false, warned_unmapped = false;

  explicit Impl(Device d) : device(std::move(d)) {}
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() {
    const VkDevice d = device.device();
    if (d) vkDeviceWaitIdle(d);
    if (frame) vmaDestroyImage(device.allocator(), frame, frame_alloc);
    if (staging) vmaDestroyBuffer(device.allocator(), staging, staging_alloc);
    if (fence) vkDestroyFence(d, fence, nullptr);
    if (pool) vkDestroyCommandPool(d, pool, nullptr);
    presenter.reset();  // then ~Device (vkDeviceWaitIdle first)
  }

  Result<void> init() {
    const VkDevice d = device.device();
    VkCommandPoolCreateInfo cpci = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = device.graphics_family();
    if (VkResult r = vkCreateCommandPool(d, &cpci, nullptr, &pool); r != VK_SUCCESS)
      return vk_error("vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cbai =
        make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (VkResult r = vkAllocateCommandBuffers(d, &cbai, &cmd); r != VK_SUCCESS)
      return vk_error("vkAllocateCommandBuffers", r);
    VkFenceCreateInfo fci = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    if (VkResult r = vkCreateFence(d, &fci, nullptr, &fence); r != VK_SUCCESS) return vk_error("vkCreateFence", r);
    return {};
  }

  Result<void> ensure_resources(uint32_t w, uint32_t h) {
    const VkDeviceSize bytes = VkDeviceSize{w} * h * 4;
    if (staging_size < bytes) {
      if (staging) vmaDestroyBuffer(device.allocator(), staging, staging_alloc);
      staging = VK_NULL_HANDLE;
      staging_size = 0;
      VkBufferCreateInfo bci = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
      bci.size = bytes;
      bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      VmaAllocationCreateInfo aci{};
      aci.usage = VMA_MEMORY_USAGE_AUTO;
      aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
      VmaAllocationInfo info{};
      if (VkResult r = vmaCreateBuffer(device.allocator(), &bci, &aci, &staging, &staging_alloc, &info);
          r != VK_SUCCESS)
        return vk_error("vmaCreateBuffer (front buffer upload)", r);
      staging_map = info.pMappedData;
      staging_size = bytes;
    }
    if (frame && frame_w == w && frame_h == h) return {};
    if (frame) vmaDestroyImage(device.allocator(), frame, frame_alloc);  // idle: every swap waits for its upload
    frame = VK_NULL_HANDLE;
    frame_written = false;
    VkImageCreateInfo ici = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (VkResult r = vmaCreateImage(device.allocator(), &ici, &aci, &frame, &frame_alloc, nullptr); r != VK_SUCCESS)
      return vk_error("vmaCreateImage (front buffer)", r);
    frame_w = w;
    frame_h = h;
    return {};
  }

  // Linear big-endian A8R8G8B8 at guest[0, w * h * 4) -> RGBA8 in the staging buffer (see the header).
  void convert(const uint8_t* src, uint32_t w, uint32_t h) {
    auto* dst = static_cast<uint8_t*>(staging_map);
    const size_t pixels = size_t{w} * h;
    for (size_t i = 0; i < pixels; ++i) {
      const uint8_t* p = src + i * 4;  // A, R, G, B in memory order
      dst[i * 4 + 0] = p[1];
      dst[i * 4 + 1] = p[2];
      dst[i * 4 + 2] = p[3];
      dst[i * 4 + 3] = 0xFF;
    }
    vmaFlushAllocation(device.allocator(), staging_alloc, 0, VkDeviceSize{w} * h * 4);
  }

  Result<void> upload(uint32_t w, uint32_t h) {
    const VkDevice d = device.device();
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier b = make<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    b.srcAccessMask = 0;  // the previous frame's reads (presenter copy) finished: execution dependency only
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // every texel is overwritten
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = frame;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &b);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (VkResult r = vkQueueSubmit(device.graphics_queue(), 1, &si, fence); r != VK_SUCCESS)
      return vk_error("vkQueueSubmit (front buffer upload)", r);
    vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(d, 1, &fence);
    frame_written = true;
    return {};
  }

  void swap(uint32_t phys, uint32_t w, uint32_t h) {
    const uint64_t n = ++swaps;
    present_front_buffer(phys, w, h);
    if (options.screenshot_after != 0 && n == options.screenshot_after) {
      Result<void> written = presenter->capture_png(options.screenshot_path);
      if (options.on_screenshot) options.on_screenshot(n, written);
    }
  }

  void present_front_buffer(uint32_t phys, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > kMaxFrontBufferSide || h > kMaxFrontBufferSide) {
      if (!warned_size) std::fprintf(stderr, "warning: [gpu] swap: front buffer size %ux%u skipped\n", w, h);
      warned_size = true;
      return;
    }
    const uint64_t bytes = uint64_t{w} * h * 4;
    const std::optional<uint32_t> guest = options.mem.to_guest ? options.mem.to_guest(phys & 0x1FFFFFFFu) : std::nullopt;
    if (!guest || *guest + bytes > options.mem.size) {
      if (!warned_unmapped)
        std::fprintf(stderr, "warning: [gpu] swap: front buffer 0x%08X (%ux%u) is not in guest memory; skipped\n",
                     phys, w, h);
      warned_unmapped = true;
      return;
    }
    if (auto r = ensure_resources(w, h); !r) {
      std::fprintf(stderr, "error: [gpu] swap: %s\n", r.error().message.c_str());
      return;
    }
    convert(options.mem.base + *guest, w, h);
    if (auto r = upload(w, h); !r) {
      std::fprintf(stderr, "error: [gpu] swap: %s\n", r.error().message.c_str());
      return;
    }
    presenter->present(frame, w, h);
  }
};

Result<std::unique_ptr<VkBackend>> VkBackend::create(VkBackendOptions options) {
  DeviceOptions dopts;
  dopts.validation = options.validation;
  dopts.headless = options.sdl_window == nullptr;
  dopts.sdl_window = options.sdl_window;
  LR_TRY_ASSIGN(device, Device::create(dopts));
  auto impl = std::make_unique<Impl>(std::move(device));
  LR_TRY(impl->init());
  LR_TRY_ASSIGN(presenter, Presenter::create(impl->device, options.sdl_window));
  impl->presenter.emplace(std::move(presenter));
  impl->options = std::move(options);
  std::string name = impl->device.device_name();
  auto backend = std::unique_ptr<VkBackend>(new VkBackend(std::move(impl)));
  backend->name_ = std::move(name);
  return backend;
}

VkBackend::VkBackend(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

VkBackend::~VkBackend() = default;

void VkBackend::load_shader(ShaderStage, uint32_t, uint32_t) {}
void VkBackend::draw(const RegisterFile&, const DrawInfo&, const ActiveShaders&) {}
void VkBackend::resolve(const RegisterFile&) {}

void VkBackend::swap(uint32_t frontbuffer_phys, uint32_t width, uint32_t height) {
  if (impl_) impl_->swap(frontbuffer_phys, width, height);
}

void VkBackend::wait_idle() {
  if (impl_) vkQueueWaitIdle(impl_->device.graphics_queue());
}

void VkBackend::shutdown() { impl_.reset(); }

}  // namespace lr::gpu::vk
