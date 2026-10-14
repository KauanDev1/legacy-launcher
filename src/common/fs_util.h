// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <span>

#include "common/result.h"

namespace lr {

// Writes <path>.tmp, fsyncs, renames over path. Creates the parent directory if missing.
Result<void> atomic_write(const std::filesystem::path& path, std::span<const std::byte> data);

// Like atomic_write, but the content is produced by `writer` into a stream bound to <dst>.tmp.
// On writer or stream failure the tmp file is removed and the error returned.
Result<void> atomic_copy_from(const std::filesystem::path& dst,
                              const std::function<Result<void>(std::ostream&)>& writer);

}  // namespace lr
