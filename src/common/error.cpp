// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/error.h"

namespace lr {

std::string_view to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::InvalidFormat: return "InvalidFormat";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
    case ErrorCode::ToolMissing: return "ToolMissing";
    case ErrorCode::ToolFailed: return "ToolFailed";
    case ErrorCode::InsufficientResources: return "InsufficientResources";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::UnimplementedImport: return "UnimplementedImport";
    case ErrorCode::Internal: return "Internal";
  }
  return "Unknown";
}

}  // namespace lr
