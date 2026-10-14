// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace lr {

enum class ErrorCode {
  InvalidFormat,
  OutOfRange,
  IoError,
  NotFound,
  UnsupportedVersion,
  ToolMissing,
  ToolFailed,
  InsufficientResources,
  Cancelled,
  UnimplementedImport,
  Internal,
};

struct Error {
  ErrorCode code;
  std::string message;
  std::string hint;
};

std::string_view to_string(ErrorCode code);

}  // namespace lr
