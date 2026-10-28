// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace lr::kernel {

constexpr uint32_t STATUS_SUCCESS = 0;
constexpr uint32_t STATUS_ABANDONED_WAIT_0 = 0x80;
constexpr uint32_t STATUS_USER_APC = 0xC0;
constexpr uint32_t STATUS_ALERTED = 0x101;
constexpr uint32_t STATUS_TIMEOUT = 0x102;
constexpr uint32_t STATUS_PENDING = 0x103;
constexpr uint32_t STATUS_TIMER_RESUME_IGNORED = 0x40000025;
constexpr uint32_t STATUS_BUFFER_OVERFLOW = 0x80000005;
constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
constexpr uint32_t STATUS_ACCESS_VIOLATION = 0xC0000005;
constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;
constexpr uint32_t STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr uint32_t STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr uint32_t STATUS_INVALID_DEVICE_REQUEST = 0xC0000010;
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_CONFLICTING_ADDRESSES = 0xC0000018;
constexpr uint32_t STATUS_ACCESS_DENIED = 0xC0000022;
constexpr uint32_t STATUS_BUFFER_TOO_SMALL = 0xC0000023;
constexpr uint32_t STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024;
constexpr uint32_t STATUS_OBJECT_NAME_INVALID = 0xC0000033;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_NOT_FOUND = 0xC0000225;
constexpr uint32_t STATUS_INVALID_PAGE_PROTECTION = 0xC0000045;
constexpr uint32_t STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr uint32_t STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;
constexpr uint32_t STATUS_DISK_FULL = 0xC000007F;
constexpr uint32_t STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0;
constexpr uint32_t STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;
constexpr uint32_t STATUS_NOT_A_DIRECTORY = 0xC0000103;

constexpr uint32_t ERROR_SUCCESS = 0;
constexpr uint32_t ERROR_INVALID_HANDLE = 6;
constexpr uint32_t ERROR_NO_MORE_FILES = 18;
constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t ERROR_INSUFFICIENT_BUFFER = 122;
constexpr uint32_t ERROR_IO_INCOMPLETE = 996;
constexpr uint32_t ERROR_IO_PENDING = 997;
constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr uint32_t ERROR_NOT_FOUND = 1168;
constexpr uint32_t ERROR_NO_SUCH_USER = 1317;
constexpr uint32_t ERROR_FUNCTION_FAILED = 1627;

// HRESULT_FROM_WIN32 values returned by the XamUser* queries.
constexpr uint32_t X_E_INVALIDARG = 0x80070057;
constexpr uint32_t X_E_NO_SUCH_USER = 0x80070525;

}  // namespace lr::kernel
