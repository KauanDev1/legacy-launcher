// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/vk_backend.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "runtime/gpu/vk/device.h"
#include "runtime/gpu/vk/draw.h"
#include "runtime/gpu/vk/edram.h"
#include "runtime/gpu/vk/presenter.h"
#include "runtime/gpu/vk/samplers.h"
#include "runtime/gpu/vk/stream.h"
#include "runtime/gpu/vk/textures.h"
#include "runtime/gpu/texture_format.h"
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
  // The frame's command stream (draws, EDRAM clears and resolves): created before and destroyed after its users.
  std::unique_ptr<CommandStream> stream;
  std::unique_ptr<Edram> edram;  // render targets, clears and resolves (Task 18); destroyed before the device
  std::unique_ptr<TextureCache> textures;  // Task 19; Task 20's draws bind through it
  std::unique_ptr<SamplerCache> samplers;
  std::unique_ptr<DrawPath> draws;  // Task 20: uses all of the above
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
  bool warned_size = false;
  std::set<std::string> front_buffer_errors;
  std::function<void(const std::string&)> fatal;  // GpuSystem's (Backend::set_fatal_handler)
  bool failed = false;  // a fatal was reported: no more work
  // request_stop (another thread): fence waits give up, swap() does nothing more.
  std::atomic<bool> stopping{false};
  bool abandoned = false;  // an upload wait gave up: its fence may still be pending, nothing more is submitted

  explicit Impl(Device d) : device(std::move(d)) {}

  // A draw or resolve error: Cancelled (stopping) is dropped; anything else is a guest fatal (exit 5) through
  // GpuSystem, naming it, and the backend does no more work. Without a handler (a backend used on its own), stderr.
  void report(const char* what, const Error& e) {
    if (e.code == ErrorCode::Cancelled) return;
    failed = true;
    const std::string message = std::string("GPU ") + what + ": " + e.message;
    if (fatal) {
      fatal(message);
    } else {
      std::fprintf(stderr, "error: [gpu] %s\n", message.c_str());
    }
  }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() {
    const VkDevice d = device.device();
    if (stream) stream->drain();
    if (d) vkDeviceWaitIdle(d);
    if (frame) vmaDestroyImage(device.allocator(), frame, frame_alloc);
    if (staging) vmaDestroyBuffer(device.allocator(), staging, staging_alloc);
    if (fence) vkDestroyFence(d, fence, nullptr);
    if (pool) vkDestroyCommandPool(d, pool, nullptr);
    draws.reset();
    edram.reset();
    samplers.reset();
    textures.reset();
    stream.reset();
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
    if (!wait_fence(d, fence, &stopping)) {
      abandoned = true;
      return Error{ErrorCode::Cancelled, "front buffer upload abandoned: the GPU is stopping", ""};
    }
    vkResetFences(d, 1, &fence);
    frame_written = true;
    return {};
  }

  void swap(const RegisterFile& regs, uint32_t phys, uint32_t w, uint32_t h) {
    if (failed || abandoned || stopping.load(std::memory_order_acquire)) return;
    // The frame's recorded work is submitted and complete before the next frame starts; a failure is fatal.
    if (auto r = stream->flush(); !r) {
      report("swap", r.error());
      return;
    }
    const uint64_t n = ++swaps;
    present_front_buffer(regs, phys, w, h);
    if (textures) textures->begin_frame();
    if (options.screenshot_after != 0 && n == options.screenshot_after && !stopping.load(std::memory_order_acquire)) {
      Result<void> written = presenter->capture_png(options.screenshot_path);
      if (options.on_screenshot) options.on_screenshot(n, written);
    }
  }

  // The front buffer through the texture loader (untiled, endian-swapped, fetch constant 0's format and swizzle;
  // runtime/gpu/texture_format.h front_buffer_rgba8), uploaded as RGBA8 and presented.
  void present_front_buffer(const RegisterFile& regs, uint32_t phys, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0 || w > kMaxFrontBufferSide || h > kMaxFrontBufferSide) {
      if (!warned_size) std::fprintf(stderr, "warning: [gpu] swap: front buffer size %ux%u skipped\n", w, h);
      warned_size = true;
      return;
    }
    auto rgba = front_buffer_rgba8(TextureFetch::from_registers(regs, 0), phys & pm4::kPhysicalMask, w, h, options.mem);
    if (!rgba) {
      if (front_buffer_errors.insert(rgba.error().message).second)
        std::fprintf(stderr, "warning: [gpu] swap: front buffer 0x%08X skipped: %s\n", phys,
                     rgba.error().message.c_str());
      return;
    }
    if (auto r = ensure_resources(w, h); !r) {
      std::fprintf(stderr, "error: [gpu] swap: %s\n", r.error().message.c_str());
      return;
    }
    std::memcpy(staging_map, rgba.value().data(), rgba.value().size());
    vmaFlushAllocation(device.allocator(), staging_alloc, 0, VkDeviceSize{w} * h * 4);
    if (auto r = upload(w, h); !r) {
      if (!abandoned) std::fprintf(stderr, "error: [gpu] swap: %s\n", r.error().message.c_str());
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
  LR_TRY_ASSIGN(stream, CommandStream::create(impl->device, &impl->stopping));
  impl->stream = std::move(stream);
  LR_TRY_ASSIGN(edram, Edram::create(impl->device, &impl->stopping, impl->stream.get()));
  impl->edram = std::move(edram);
  LR_TRY_ASSIGN(textures, TextureCache::create(impl->device, options.mem, &impl->stopping));
  impl->textures = std::move(textures);
  impl->textures->set_before_gpu_work([stream = impl->stream.get()] { return stream->flush(); });
  LR_TRY_ASSIGN(samplers, SamplerCache::create(impl->device));
  impl->samplers = std::move(samplers);
  LR_TRY_ASSIGN(draws, DrawPath::create(impl->device, options.mem, *impl->stream, *impl->edram, *impl->textures,
                                        *impl->samplers));
  impl->draws = std::move(draws);
  impl->edram->set_write_observer([cache = impl->textures.get()](uint32_t phys, uint32_t size) {
    cache->invalidate_range(phys, size);
  });
  LR_TRY_ASSIGN(presenter, Presenter::create(impl->device, options.sdl_window));
  impl->presenter.emplace(std::move(presenter));
  impl->presenter->set_stop_flag(&impl->stopping);
  impl->options = std::move(options);
  std::string name = impl->device.device_name();
  auto backend = std::unique_ptr<VkBackend>(new VkBackend(std::move(impl)));
  backend->name_ = std::move(name);
  return backend;
}

VkBackend::VkBackend(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

VkBackend::~VkBackend() = default;

// The shaders reach the draws already translated (GpuSystem's ShaderCache): nothing to do at load time.
void VkBackend::load_shader(ShaderStage, uint32_t, uint32_t) {}

void VkBackend::draw(const RegisterFile& regs, const DrawInfo& info, const ActiveShaders& shaders) {
  if (!impl_ || impl_->failed || impl_->abandoned || impl_->stopping.load(std::memory_order_acquire)) return;
  if (auto r = impl_->draws->draw(regs, info, shaders); !r) impl_->report("draw", r.error());
}

void VkBackend::resolve(const RegisterFile& regs) {
  if (!impl_ || impl_->failed || impl_->abandoned || impl_->stopping.load(std::memory_order_acquire)) return;
  // Guest-side problems of the copy itself (unsupported destination formats, destinations outside guest memory) are
  // warnings inside Edram and do not fail; an error (a target that cannot be bound, a Vulkan failure) is fatal.
  if (auto r = impl_->edram->resolve(regs, impl_->options.mem); !r) impl_->report("resolve", r.error());
}

void VkBackend::set_fatal_handler(std::function<void(const std::string& what)> fatal) {
  if (impl_) impl_->fatal = std::move(fatal);
}

DrawStats VkBackend::draw_stats() const { return impl_ && impl_->draws ? impl_->draws->stats() : DrawStats{}; }

void VkBackend::swap(const RegisterFile& regs, uint32_t frontbuffer_phys, uint32_t width, uint32_t height) {
  if (impl_) impl_->swap(regs, frontbuffer_phys, width, height);
}

uint64_t VkBackend::textures_created() const { return impl_ && impl_->textures ? impl_->textures->textures_created() : 0; }

void VkBackend::wait_idle() {
  if (!impl_) return;
  if (!impl_->failed && !impl_->abandoned && !impl_->stopping.load(std::memory_order_acquire)) {
    if (auto r = impl_->stream->flush(); !r) impl_->report("wait_idle", r.error());
  }
  vkQueueWaitIdle(impl_->device.graphics_queue());
}

void VkBackend::request_stop() {
  if (impl_) impl_->stopping.store(true, std::memory_order_release);
}

void VkBackend::shutdown() {
  if (!impl_) return;
  impl_.reset();
  std::fprintf(stderr, "[gpu] vulkan: shut down\n");
}

}  // namespace lr::gpu::vk
