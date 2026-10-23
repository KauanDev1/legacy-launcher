// SPDX-License-Identifier: MIT
#include "runtime/gpu/screenshot.h"

#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// stb_image_write (thirdparty/stb, pinned, public domain / MIT): its implementation lives in this file only, with
// internal linkage, and writes through a callback into memory (no stdio).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace lr::gpu {
namespace {

void append(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

Result<void> write_png(const std::filesystem::path& path, uint32_t width, uint32_t height,
                       std::span<const uint8_t> rgba) {
  constexpr uint32_t kMaxSide = 16384;
  if (width == 0 || height == 0 || width > kMaxSide || height > kMaxSide ||
      rgba.size() != uint64_t{width} * height * 4)
    return Error{ErrorCode::IoError,
                 "cannot write " + path.string() + ": bad image " + std::to_string(width) + "x" +
                     std::to_string(height) + " with " + std::to_string(rgba.size()) + " bytes",
                 ""};
  std::vector<uint8_t> png;
  if (stbi_write_png_to_func(append, &png, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                             static_cast<int>(width * 4)) == 0)
    return Error{ErrorCode::IoError, "cannot encode " + path.string() + " as PNG", ""};
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  f.close();
  if (!f) return Error{ErrorCode::IoError, "cannot write " + path.string(), "check the path and free disk space"};
  return {};
}

}  // namespace lr::gpu
