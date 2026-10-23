// SPDX-License-Identifier: MIT
#include "runtime/gpu/vk/device.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/log.h"
#include "vk_mem_alloc.h"

#ifdef LR_HAVE_SDL2
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#endif

namespace lr::gpu::vk {
namespace {

Error no_driver(const std::string& detail) {
  return Error{ErrorCode::ToolMissing, "no Vulkan driver: " + detail,
               "install a Vulkan loader and a driver (Arch: vulkan-icd-loader plus vulkan-radeon, vulkan-intel, "
               "nvidia-utils, or mesa's lavapipe in the vulkan-swrast package)"};
}

Error missing(const std::string& what) {
  return Error{ErrorCode::ToolMissing, "Vulkan device lacks " + what,
               "LegacyRecomp needs Vulkan 1.2 with swapchain, BC textures, descriptor indexing, buffer device "
               "address and 64 vertex output / fragment input components; update the driver or pick another GPU"};
}

VKAPI_ATTR VkBool32 VKAPI_CALL on_debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  const std::string msg = std::string("[vulkan] ") + (data && data->pMessage ? data->pMessage : "");
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    log::error(msg);
  } else {
    log::warn(msg);
  }
  return VK_FALSE;
}

bool has_layer(const char* name) {
  std::uint32_t n = 0;
  if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS) return false;
  std::vector<VkLayerProperties> v(n);
  if (vkEnumerateInstanceLayerProperties(&n, v.data()) != VK_SUCCESS) return false;
  return std::any_of(v.begin(), v.end(), [&](const VkLayerProperties& p) { return std::strcmp(p.layerName, name) == 0; });
}

bool has_instance_ext(const char* name) {
  std::uint32_t n = 0;
  if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS) return false;
  std::vector<VkExtensionProperties> v(n);
  if (vkEnumerateInstanceExtensionProperties(nullptr, &n, v.data()) != VK_SUCCESS) return false;
  return std::any_of(v.begin(), v.end(),
                     [&](const VkExtensionProperties& p) { return std::strcmp(p.extensionName, name) == 0; });
}

// Empty when the device meets the feature floor (global constraints), otherwise the first missing item.
std::string check_floor(VkPhysicalDevice pd) {
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  if (props.apiVersion < VK_API_VERSION_1_2) return "Vulkan 1.2";
  if (std::string limit = check_device_limits(props.limits); !limit.empty()) return limit;

  VkPhysicalDeviceVulkan12Features f12{};
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f2.pNext = &f12;
  vkGetPhysicalDeviceFeatures2(pd, &f2);

  if (!f2.features.textureCompressionBC) return "textureCompressionBC";
  if (!f2.features.shaderSampledImageArrayDynamicIndexing) return "shaderSampledImageArrayDynamicIndexing";
  if (!f12.runtimeDescriptorArray) return "runtimeDescriptorArray";
  if (!f12.descriptorBindingPartiallyBound) return "descriptorBindingPartiallyBound";
  if (!f12.descriptorBindingSampledImageUpdateAfterBind) return "descriptorBindingSampledImageUpdateAfterBind";
  if (!f12.bufferDeviceAddress) return "bufferDeviceAddress";

  // Checked even for headless devices so the feature floor is the same everywhere.
  std::uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> ext(n);
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, ext.data());
  const bool swapchain = std::any_of(ext.begin(), ext.end(), [](const VkExtensionProperties& p) {
    return std::strcmp(p.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
  });
  if (!swapchain) return VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  return {};
}

int device_rank(VkPhysicalDevice d) {
  VkPhysicalDeviceProperties p;
  vkGetPhysicalDeviceProperties(d, &p);
  if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) return 0;
  if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) return 1;
  return 2;
}

}  // namespace

std::string check_device_limits(const VkPhysicalDeviceLimits& limits) {
  constexpr std::uint32_t kComponents = 64;
  if (limits.maxVertexOutputComponents < kComponents)
    return "maxVertexOutputComponents >= 64 (device has " + std::to_string(limits.maxVertexOutputComponents) + ")";
  if (limits.maxFragmentInputComponents < kComponents)
    return "maxFragmentInputComponents >= 64 (device has " + std::to_string(limits.maxFragmentInputComponents) + ")";
  return {};
}

Result<Device> Device::create(const DeviceOptions& options) {
  if (volkInitialize() != VK_SUCCESS) return no_driver("libvulkan.so.1 could not be loaded");

  std::uint32_t inst_version = VK_API_VERSION_1_0;
  if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&inst_version);
  if (inst_version < VK_API_VERSION_1_2) return no_driver("the loader supports Vulkan older than 1.2");

  Device dev;

  std::vector<const char*> layers;
  std::vector<const char*> exts;
  bool debug_utils = false;
  if (options.validation) {
    if (has_layer("VK_LAYER_KHRONOS_validation")) {
      layers.push_back("VK_LAYER_KHRONOS_validation");
      if (has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        debug_utils = true;
      }
    } else {
      log::warn("Vulkan validation requested but VK_LAYER_KHRONOS_validation is not installed");
    }
  }

#ifdef LR_HAVE_SDL2
  std::vector<const char*> sdl_exts;
#endif
  // VK_KHR_swapchain (enabled on every device, see check_floor) requires VK_KHR_surface on the instance; a window
  // gets it from SDL below, a headless device asks for it here (VUID-vkCreateDevice-ppEnabledExtensionNames-01387).
  if (options.headless && has_instance_ext(VK_KHR_SURFACE_EXTENSION_NAME)) exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  if (!options.headless) {
#ifdef LR_HAVE_SDL2
    if (!options.sdl_window)
      return Error{ErrorCode::Internal, "DeviceOptions::sdl_window is null for a non-headless device", ""};
    unsigned count = 0;
    auto* win = static_cast<SDL_Window*>(options.sdl_window);
    if (!SDL_Vulkan_GetInstanceExtensions(win, &count, nullptr))
      return no_driver(std::string("SDL window not created with SDL_WINDOW_VULKAN, or SDL has no Vulkan support: ") + SDL_GetError());
    sdl_exts.resize(count);
    SDL_Vulkan_GetInstanceExtensions(win, &count, sdl_exts.data());
    for (const char* e : sdl_exts) exts.push_back(e);
#else
    return Error{ErrorCode::Internal, "built without SDL2: only headless devices are available", ""};
#endif
  }

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "LegacyRecomp";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  ici.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
  ici.ppEnabledLayerNames = layers.data();
  ici.enabledExtensionCount = static_cast<std::uint32_t>(exts.size());
  ici.ppEnabledExtensionNames = exts.data();
  const VkResult ir = vkCreateInstance(&ici, nullptr, &dev.instance_);
  if (ir == VK_ERROR_INCOMPATIBLE_DRIVER || ir == VK_ERROR_INITIALIZATION_FAILED)
    return no_driver("the loader found no usable driver (vkCreateInstance returned " + std::to_string(ir) + ")");
  if (ir != VK_SUCCESS)
    return Error{ErrorCode::ToolMissing, "vkCreateInstance failed (VkResult " + std::to_string(ir) + ")", ""};
  volkLoadInstance(dev.instance_);

  if (debug_utils) {
    VkDebugUtilsMessengerCreateInfoEXT mci{};
    mci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = on_debug;
    vkCreateDebugUtilsMessengerEXT(dev.instance_, &mci, nullptr, &dev.messenger_);
  }

#ifdef LR_HAVE_SDL2
  if (!options.headless) {
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(options.sdl_window), dev.instance_, &dev.surface_))
      return Error{ErrorCode::ToolMissing, std::string("cannot create a Vulkan surface: ") + SDL_GetError(), ""};
  }
#endif

  std::uint32_t npd = 0;
  if (vkEnumeratePhysicalDevices(dev.instance_, &npd, nullptr) != VK_SUCCESS || npd == 0)
    return no_driver("the loader reports no physical devices");
  std::vector<VkPhysicalDevice> pds(npd);
  vkEnumeratePhysicalDevices(dev.instance_, &npd, pds.data());

  // Preference order: discrete, integrated, then the rest (lavapipe is a CPU device).
  std::stable_sort(pds.begin(), pds.end(),
                   [](VkPhysicalDevice a, VkPhysicalDevice b) { return device_rank(a) < device_rank(b); });

  std::string first_problem;
  for (VkPhysicalDevice pd : pds) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    const std::string problem = check_floor(pd);
    if (!problem.empty()) {
      if (first_problem.empty()) first_problem = std::string(props.deviceName) + ": " + problem;
      continue;
    }
    std::uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
    bool found = false;
    for (std::uint32_t i = 0; i < nq && !found; ++i) {
      if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
      if (dev.surface_) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, dev.surface_, &present);
        if (!present) continue;
      }
      dev.family_ = i;
      found = true;
    }
    if (!found) {
      if (first_problem.empty())
        first_problem = std::string(props.deviceName) + ": graphics queue" + (dev.surface_ ? " with present support" : "");
      continue;
    }
    dev.physical_ = pd;
    dev.name_ = props.deviceName;
    break;
  }
  if (!dev.physical_) return missing(first_problem);

  // Enable exactly the floor, not everything the device offers.
  VkPhysicalDeviceVulkan12Features en12{};
  en12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  en12.runtimeDescriptorArray = VK_TRUE;
  en12.descriptorBindingPartiallyBound = VK_TRUE;
  en12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
  en12.bufferDeviceAddress = VK_TRUE;
  VkPhysicalDeviceFeatures2 en2{};
  en2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  en2.pNext = &en12;
  en2.features.textureCompressionBC = VK_TRUE;
  en2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;  // translated tfetch: heaps indexed by the shared block

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = dev.family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;
  const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.pNext = &en2;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dev_exts;
  const VkResult dr = vkCreateDevice(dev.physical_, &dci, nullptr, &dev.device_);
  if (dr != VK_SUCCESS)
    return Error{ErrorCode::ToolMissing, "vkCreateDevice failed (VkResult " + std::to_string(dr) + ")", ""};
  volkLoadDevice(dev.device_);
  vkGetDeviceQueue(dev.device_, dev.family_, 0, &dev.queue_);

  VmaVulkanFunctions fns{};
  fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
  VmaAllocatorCreateInfo aci{};
  aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  aci.physicalDevice = dev.physical_;
  aci.device = dev.device_;
  aci.instance = dev.instance_;
  aci.vulkanApiVersion = VK_API_VERSION_1_2;
  aci.pVulkanFunctions = &fns;
  const VkResult ar = vmaCreateAllocator(&aci, &dev.allocator_);
  if (ar != VK_SUCCESS)
    return Error{ErrorCode::Internal, "vmaCreateAllocator failed (VkResult " + std::to_string(ar) + ")", ""};

  return dev;
}

Device::Device(Device&& o) noexcept { *this = std::move(o); }

Device& Device::operator=(Device&& o) noexcept {
  if (this != &o) {
    destroy();
    instance_ = std::exchange(o.instance_, VK_NULL_HANDLE);
    messenger_ = std::exchange(o.messenger_, VK_NULL_HANDLE);
    surface_ = std::exchange(o.surface_, VK_NULL_HANDLE);
    physical_ = std::exchange(o.physical_, VK_NULL_HANDLE);
    device_ = std::exchange(o.device_, VK_NULL_HANDLE);
    queue_ = std::exchange(o.queue_, VK_NULL_HANDLE);
    family_ = o.family_;
    allocator_ = std::exchange(o.allocator_, nullptr);
    name_ = std::move(o.name_);
  }
  return *this;
}

Device::~Device() { destroy(); }

void Device::destroy() {
  // Work still queued (a frame in flight when the process is told to stop) must finish first.
  if (device_) vkDeviceWaitIdle(device_);
  if (allocator_) vmaDestroyAllocator(allocator_);
  if (device_) vkDestroyDevice(device_, nullptr);
  if (instance_) {
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    vkDestroyInstance(instance_, nullptr);
  }
  allocator_ = nullptr;
  device_ = VK_NULL_HANDLE;
  surface_ = VK_NULL_HANDLE;
  messenger_ = VK_NULL_HANDLE;
  instance_ = VK_NULL_HANDLE;
}

}  // namespace lr::gpu::vk
