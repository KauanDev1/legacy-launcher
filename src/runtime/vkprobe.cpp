// SPDX-License-Identifier: MIT
// legacyrecomp-vkprobe: the doctor's Vulkan check (src/pipeline/doctor.cpp, SystemDoctorProbe::vulkan_device). It
// creates the same headless Device the game creates (runtime/gpu/vk/device.h: Vulkan 1.2, swapchain, BC textures,
// descriptor indexing, buffer device address, 64 interpolator components) and prints one line on stdout:
//   ok<TAB><device name>
//   missing<TAB><message naming the missing piece><TAB><hint>
// It always exits 0 (the doctor reads the line), through _exit: a Vulkan driver's own allocations are not ours to
// report to LeakSanitizer. A separate process, so a crashing driver cannot take the launcher down; staged as
// share/legacyrecomp/tools/legacyrecomp-vkprobe (src/pipeline/resources.cmake).
#include <unistd.h>

#include <cstdio>
#include <string>

#include "runtime/gpu/vk/device.h"

namespace {
std::string one_line(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}
}  // namespace

int main() {
  lr::gpu::vk::DeviceOptions opts;
  opts.headless = true;
  {
    auto device = lr::gpu::vk::Device::create(opts);
    if (device)
      std::printf("ok\t%s\n", one_line(device.value().device_name()).c_str());
    else
      std::printf("missing\t%s\t%s\n", one_line(device.error().message).c_str(), one_line(device.error().hint).c_str());
  }
  std::fflush(stdout);
  _exit(0);
}
