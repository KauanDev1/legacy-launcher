// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "runtime/gpu/vk/device.h"
#include "vk_mem_alloc.h"

using lr::gpu::vk::Device;
using lr::gpu::vk::DeviceOptions;

namespace {

template <class T>
T make(VkStructureType type) {
  T s{};
  s.sType = type;
  return s;
}

bool has_icd() {
  if (volkInitialize() != VK_SUCCESS) return false;
  VkApplicationInfo app = make<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici = make<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
  ici.pApplicationInfo = &app;
  VkInstance inst = VK_NULL_HANDLE;
  if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return false;
  volkLoadInstance(inst);
  std::uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  vkDestroyInstance(inst, nullptr);
  return n > 0;
}

// Sets an environment variable for the scope and restores the previous value.
class EnvGuard {
 public:
  EnvGuard(const char* name, const std::string& value) : name_(name) {
    if (const char* old = std::getenv(name)) old_ = old;
    setenv(name, value.c_str(), 1);
  }
  ~EnvGuard() {
    if (old_) setenv(name_, old_->c_str(), 1);
    else unsetenv(name_);
  }

 private:
  const char* name_;
  std::optional<std::string> old_;
};

}  // namespace

TEST_CASE("headless device has the required features", "[unit][vulkan]") {
  if (!has_icd()) SKIP("no Vulkan ICD installed");

  DeviceOptions opts;
  opts.headless = true;
  auto r = Device::create(opts);
  if (!r.ok()) {
    UNSCOPED_INFO(r.error().message);
  }
  REQUIRE(r.ok());
  Device& d = r.value();
  CHECK(d.instance() != VK_NULL_HANDLE);
  CHECK(d.device() != VK_NULL_HANDLE);
  CHECK(d.graphics_queue() != VK_NULL_HANDLE);
  CHECK(d.allocator() != nullptr);
  CHECK(d.surface() == VK_NULL_HANDLE);

  VkPhysicalDeviceVulkan12Features f12 = make<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
  VkPhysicalDeviceFeatures2 f2 = make<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
  f2.pNext = &f12;
  vkGetPhysicalDeviceFeatures2(d.physical_device(), &f2);
  CHECK(f2.features.textureCompressionBC);
  CHECK(f2.features.shaderSampledImageArrayDynamicIndexing);
  CHECK(f12.runtimeDescriptorArray);
  CHECK(f12.descriptorBindingPartiallyBound);
  CHECK(f12.descriptorBindingSampledImageUpdateAfterBind);
  CHECK(f12.bufferDeviceAddress);

  // The allocator works: one small host-visible buffer.
  VkBufferCreateInfo bci = make<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
  bci.size = 4096;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VmaAllocationCreateInfo aci{};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  VkBuffer buf = VK_NULL_HANDLE;
  VmaAllocation alloc = nullptr;
  REQUIRE(vmaCreateBuffer(d.allocator(), &bci, &aci, &buf, &alloc, nullptr) == VK_SUCCESS);
  vmaDestroyBuffer(d.allocator(), buf, alloc);

  // The bufferDeviceAddress feature is enabled, not only supported: a device-address buffer yields an address.
  bci.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  aci.flags = 0;
  REQUIRE(vmaCreateBuffer(d.allocator(), &bci, &aci, &buf, &alloc, nullptr) == VK_SUCCESS);
  VkBufferDeviceAddressInfo bai = make<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
  bai.buffer = buf;
  CHECK(vkGetBufferDeviceAddress(d.device(), &bai) != 0);
  vmaDestroyBuffer(d.allocator(), buf, alloc);

  // A moved-from Device must not double-free.
  Device moved = std::move(d);
  CHECK(moved.device() != VK_NULL_HANDLE);
  CHECK(d.device() == VK_NULL_HANDLE);  // NOLINT(bugprone-use-after-move): the wrapper defines this state
}

TEST_CASE("missing ICD gives a clear error", "[unit][vulkan]") {
  if (!has_icd()) SKIP("no Vulkan ICD installed (nothing to mask)");

  const auto empty = std::filesystem::temp_directory_path() / "lr_empty_icd.json";
  { std::ofstream(empty).flush(); }
  {
    EnvGuard icd("VK_ICD_FILENAMES", empty.string());
    EnvGuard icd2("VK_DRIVER_FILES", empty.string());
    DeviceOptions opts;
    auto r = Device::create(opts);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == lr::ErrorCode::ToolMissing);
    CHECK(r.error().message.find("no Vulkan driver") != std::string::npos);
  }
  std::filesystem::remove(empty);
}
