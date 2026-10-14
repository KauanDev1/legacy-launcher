// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "image.h"

namespace lr::runtime {

namespace {
std::string hex(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "0x%08llX", static_cast<unsigned long long>(v));
  return b;
}
}  // namespace

Result<LoadedImage> load_image(GuestMemory& mem, std::span<const std::byte> xex) {
  if (xex.size() < 4) return Error{ErrorCode::InvalidFormat, "image is too small", ""};
  Image image = Image::ParseImage(reinterpret_cast<const uint8_t*>(xex.data()), xex.size());
  if (!image.data || image.sections.empty()) {
    return Error{ErrorCode::InvalidFormat, "XenonUtils could not parse the image", "expected a patched, decrypted XEX2"};
  }
  if (image.base > UINT32_MAX) return Error{ErrorCode::InvalidFormat, "image base does not fit in 32 bits", ""};

  uint64_t end = std::max<uint64_t>(image.base + image.size, image.base);
  for (const Section& s : image.sections) {
    if (s.base < image.base || s.base + s.size > UINT32_MAX) {
      return Error{ErrorCode::InvalidFormat, "section " + s.name + " lies outside the 32-bit image range", ""};
    }
    if (s.base + s.size > mem.size()) {
      return Error{ErrorCode::OutOfRange, "section " + s.name + " at " + hex(s.base) + " does not fit in guest memory", ""};
    }
    end = std::max<uint64_t>(end, s.base + s.size);
  }
  if (image.entry_point < image.base || image.entry_point >= end) {
    return Error{ErrorCode::InvalidFormat, "entry point " + hex(image.entry_point) + " is outside the image", ""};
  }
  for (const Section& s : image.sections) {
    if (s.size && s.data) std::memcpy(mem.base() + s.base, s.data, s.size);
  }
  return LoadedImage{static_cast<uint32_t>(image.base), static_cast<uint32_t>(end - image.base),
                     static_cast<uint32_t>(image.entry_point)};
}

}  // namespace lr::runtime
