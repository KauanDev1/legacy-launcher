// SPDX-License-Identifier: MIT
// --screenshot-after <N> <png>: PNG encoding of a captured frame (stb_image_write, thirdparty/stb, ADR 0002). Pure
// GPU core: no Vulkan types; the Vulkan presenter (runtime/gpu/vk/presenter.h) reads the frame back and calls this.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

#include "common/result.h"

namespace lr::gpu {

// Writes `rgba` (width * height pixels, 4 bytes each in R, G, B, A order, rows top to bottom, no padding) to `path`
// as an 8-bit RGBA PNG, creating the parent directories. Error (IoError) when the size does not match, the encoder
// fails or the file cannot be written.
Result<void> write_png(const std::filesystem::path& path, uint32_t width, uint32_t height,
                       std::span<const uint8_t> rgba);

}  // namespace lr::gpu
