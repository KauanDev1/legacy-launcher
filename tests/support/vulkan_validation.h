// SPDX-License-Identifier: MIT
// Test helpers for runs with LR_VULKAN_VALIDATION=1: is the Khronos validation layer installed here, and did a
// game's output stay free of validation messages with the layer really running.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "volk.h"

namespace lr::testing {

// The Vulkan loader lists VK_LAYER_KHRONOS_validation.
inline bool has_validation_layer() {
  if (volkInitialize() != VK_SUCCESS) return false;
  uint32_t n = 0;
  if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS) return false;
  std::vector<VkLayerProperties> layers(n);
  if (vkEnumerateInstanceLayerProperties(&n, layers.data()) != VK_SUCCESS) return false;
  return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
    return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
  });
}

// The game's output with LR_VULKAN_VALIDATION=1 carries no validation message ("[vulkan] ...", from the Device's
// debug messenger) and no sign that validation did not run: the "not installed" warning (checked when the layer is
// installed here, or always with `require_layer`) and the "no debug messenger" warning (runtime/gpu/vk/device.cpp).
inline void check_validation_clean(const std::string& out, bool require_layer = false) {
  CHECK(out.find("[vulkan]") == std::string::npos);
  if (require_layer || has_validation_layer())
    CHECK(out.find("VK_LAYER_KHRONOS_validation is not installed") == std::string::npos);
  CHECK(out.find("VK_EXT_debug_utils is missing") == std::string::npos);
}

}  // namespace lr::testing
