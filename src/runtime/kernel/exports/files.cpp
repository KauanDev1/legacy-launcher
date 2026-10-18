// SPDX-License-Identifier: MIT
// File imports over the VFS (spec section 9). Argument orders, dispositions and status codes follow Xenia's
// xboxkrnl_io.cc, xboxkrnl_io_info.cc, xfile.cc and vfs/virtual_file_system.cc (semantic reference only, no code
// copied); NtOpenFile takes share_access before open_options as in xenia-canary (Xenia master drops it). All I/O is
// synchronous: the result is in the IO_STATUS_BLOCK when the call returns, then the event (if any) is set and the
// APC (if any) is queued to the calling guest thread with (apc_context, io_status_block, 0).
//
// Deliberate differences from Xenia, each the NT behaviour: after a read/write at an explicit offset the file
// position is offset + bytes (Xenia adds the bytes to the old position); NtQueryFullAttributesFile reports
// STATUS_OBJECT_NAME_NOT_FOUND for a missing name (Xenia: STATUS_NO_SUCH_FILE); FILE_DIRECTORY_FILE on an existing
// file is STATUS_NOT_A_DIRECTORY; delete-on-close (FileDispositionInformation) is honoured (Xenia ignores it); the
// directory pattern is taken on the first query or a restart only (Xenia restarts on every non-empty pattern).
#include "runtime/kernel/files.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>

#include "runtime/kernel/dispatcher.h"
#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/registry.h"
#include "runtime/kernel/status.h"
#include "runtime/kernel/threads.h"

namespace lr::kernel {

namespace fs = std::filesystem;

// --- FileObject -----------------------------------------------------------------------------------------------

namespace {
constexpr uint32_t kFileBodyBytes = 0x40;  // zeroed; nothing in the guest reads a file object's fields in M2
}

std::shared_ptr<FileObject> FileObject::create(Kernel& k, int fd, fs::path host, bool read_only, bool directory) {
  const uint32_t body = k.pool_alloc(kFileBodyBytes, 0);
  if (body == 0) {
    ::close(fd);
    return nullptr;
  }
  auto f = std::make_shared<FileObject>(k, fd, std::move(host), read_only, directory, body);
  f->set_guest_body(body);
  return f;
}

FileObject::FileObject(Kernel& k, int fd, fs::path host, bool read_only, bool directory, uint32_t body)
    : KObject(ObjectKind::File), kernel_(k), fd_(fd), host_(std::move(host)), read_only_(read_only),
      directory_(directory), body_(body) {}

FileObject::~FileObject() {
  ::close(fd_);
  if (delete_on_close) {
    std::error_code ec;
    fs::remove(host_, ec);  // a non-empty directory stays, as NT refuses to delete it
  }
  kernel_.pool_free(body_);
}

bool wildcard_match(std::string_view pattern, std::string_view name) {
  if (pattern == "*.*") pattern = "*";
  const auto lower = [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); };
  // Iterative glob with backtracking to the last '*'.
  size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
  while (n < name.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(name[n]))) {
      ++p;
      ++n;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = n;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

bool read_ansi_string(const uint8_t* base, GuestPtr<X_ANSI_STRING> p, std::string& out) {
  if (!p.valid_range(sizeof(X_ANSI_STRING))) return false;
  const auto* s = reinterpret_cast<const X_ANSI_STRING*>(base + p.addr);
  const uint32_t len = s->length, buf = s->buffer;
  if (len == 0) {
    out.clear();
    return true;
  }
  if (!GuestPtr<char>{buf}.valid_range(len)) return false;
  out.assign(reinterpret_cast<const char*>(base + buf), len);
  return true;
}

namespace {

using U32Ptr = GuestPtr<be<uint32_t>>;
using IosbPtr = GuestPtr<X_IO_STATUS_BLOCK>;
using OffsetPtr = GuestPtr<be<int64_t>>;

// Creation dispositions and options (NT values, as Xenia's FileDisposition / create options).
constexpr uint32_t FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3, FILE_OVERWRITE = 4,
                   FILE_OVERWRITE_IF = 5;
constexpr uint32_t FILE_DIRECTORY_FILE = 0x1, FILE_NON_DIRECTORY_FILE = 0x40;
// IO_STATUS_BLOCK.information after a create (Xenia's FileAction).
constexpr uint32_t FILE_SUPERSEDED = 0, FILE_OPENED = 1, FILE_CREATED = 2, FILE_OVERWRITTEN = 3;
// OBJECT_ATTRIBUTES.root_directory value meaning "no root" besides 0 (ObDosDevices, as Xenia).
constexpr uint32_t kObDosDevices = 0xFFFFFFFD;
// Byte offsets meaning "the current file position": Xenia's -1 and NT's FILE_USE_FILE_POINTER_POSITION (-2).
constexpr int64_t kUseFilePointer = -2;

constexpr uint32_t FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80;

// Information classes handled.
constexpr uint32_t kFileDispositionInformation = 13, kFilePositionInformation = 14, kFileStandardInformation = 5,
                   kFileInternalInformation = 6, kFileEndOfFileInformation = 20, kFileNetworkOpenInformation = 34;
constexpr uint32_t kFileFsSizeInformation = 3, kFileFsAttributeInformation = 5;

// Fixed volume description (FileFs*Information): FATX-like, 512-byte sectors, 16 KiB clusters, 16 GiB of which
// 8 GiB free. The host's real free space is not reported, so a full host disk shows up as a write error instead.
constexpr uint64_t kVolumeTotalUnits = 0x100000, kVolumeFreeUnits = 0x80000;
constexpr uint32_t kVolumeSectorsPerUnit = 32, kVolumeBytesPerSector = 512;
constexpr uint32_t kVolumeAttributes = 0, kVolumeMaxComponent = 42;  // FATX names are at most 42 characters
constexpr char kVolumeFsName[] = "FATX";

constexpr uint32_t kAllocationGranularity = 0x1000;  // reported allocation size = size rounded up to 4 KiB

template <class T>
void put_struct(const GuestCall& c, uint32_t addr, const T& v) {
  std::memcpy(c.base + addr, &v, sizeof v);  // memcpy: the guest buffer need not be aligned
}

void put_u32(const GuestCall& c, U32Ptr p, uint32_t v) {
  if (p.valid_range(4)) *p.get(c.base) = v;
}

void put_iosb(const GuestCall& c, IosbPtr p, uint32_t status, uint32_t information) {
  if (!p.valid_range(sizeof(X_IO_STATUS_BLOCK))) return;
  X_IO_STATUS_BLOCK io;
  io.status = status;
  io.information = information;
  put_struct(c, p.addr, io);
}

std::shared_ptr<FileObject> file_of(uint32_t handle) {
  return std::dynamic_pointer_cast<FileObject>(kernel().objects().get(handle));
}

uint64_t filetime(const struct timespec& ts) {
  constexpr int64_t kUnixToFiletimeSeconds = 11644473600;
  return static_cast<uint64_t>((static_cast<int64_t>(ts.tv_sec) + kUnixToFiletimeSeconds) * 10'000'000 +
                               ts.tv_nsec / 100);
}

uint64_t allocation_size(uint64_t size) {
  return (size + kAllocationGranularity - 1) / kAllocationGranularity * kAllocationGranularity;
}

X_FILE_NETWORK_OPEN_INFORMATION network_open_info(const struct stat& st) {
  X_FILE_NETWORK_OPEN_INFORMATION info{};
  info.creation_time = filetime(st.st_mtim);
  info.last_access_time = filetime(st.st_atim);
  info.last_write_time = filetime(st.st_mtim);
  info.change_time = filetime(st.st_mtim);  // as Xenia: the write time
  const bool dir = S_ISDIR(st.st_mode);
  info.allocation_size = dir ? 0 : allocation_size(static_cast<uint64_t>(st.st_size));
  info.end_of_file = dir ? 0 : static_cast<uint64_t>(st.st_size);
  info.attributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  return info;
}

uint32_t status_from_errno(int e) {
  switch (e) {
    case ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR: return STATUS_OBJECT_PATH_NOT_FOUND;
    case EEXIST: return STATUS_OBJECT_NAME_COLLISION;
    case EACCES:
    case EPERM:
    case EROFS: return STATUS_ACCESS_DENIED;
    case EISDIR: return STATUS_FILE_IS_A_DIRECTORY;
    case EFAULT: return STATUS_ACCESS_VIOLATION;
    default: return STATUS_UNSUCCESSFUL;
  }
}

// OBJECT_ATTRIBUTES -> resolved host path. The name is relative to root_directory when that is an open directory.
uint32_t resolve_attributes(const GuestCall& c, GuestPtr<X_OBJECT_ATTRIBUTES> oa, ResolvedPath& out) {
  if (!oa.valid_range(sizeof(X_OBJECT_ATTRIBUTES))) return STATUS_INVALID_PARAMETER;
  X_OBJECT_ATTRIBUTES a;
  std::memcpy(&a, c.base + oa.addr, sizeof a);
  std::string name;
  if (!read_ansi_string(c.base, GuestPtr<X_ANSI_STRING>{a.name_ptr}, name)) return STATUS_INVALID_PARAMETER;
  std::shared_ptr<FileObject> root;
  const uint32_t root_handle = a.root_directory;
  if (root_handle != 0 && root_handle != kObDosDevices) {
    root = file_of(root_handle);
    if (!root || !root->is_directory()) return STATUS_INVALID_HANDLE;
  }
  auto r = kernel().vfs().resolve(name, root ? &root->host() : nullptr);
  if (!r) return r.error().code == ErrorCode::InvalidFormat ? STATUS_OBJECT_NAME_INVALID : STATUS_OBJECT_PATH_NOT_FOUND;
  out = std::move(r.value());
  return STATUS_SUCCESS;
}

// NtCreateFile / NtOpenFile.
uint32_t open_file(const GuestCall& c, U32Ptr handle_out, uint32_t /*access*/, GuestPtr<X_OBJECT_ATTRIBUTES> oa,
                   IosbPtr iosb, uint32_t disposition, uint32_t options) {
  if (!handle_out.valid_range(4)) return STATUS_INVALID_PARAMETER;
  const auto fail = [&](uint32_t status) {
    put_u32(c, handle_out, 0xFFFFFFFF);  // X_INVALID_HANDLE_VALUE, as Xenia
    put_iosb(c, iosb, status, 0);
    return status;
  };
  if (disposition > FILE_OVERWRITE_IF) return fail(STATUS_INVALID_PARAMETER);
  ResolvedPath p;
  if (const uint32_t s = resolve_attributes(c, oa, p); s != STATUS_SUCCESS) return fail(s);

  uint32_t information = FILE_OPENED;
  bool create = false, truncate = false;
  if (p.exists) {
    if (p.is_directory && (options & FILE_NON_DIRECTORY_FILE) != 0) return fail(STATUS_FILE_IS_A_DIRECTORY);
    if (!p.is_directory && (options & FILE_DIRECTORY_FILE) != 0) return fail(STATUS_NOT_A_DIRECTORY);
    switch (disposition) {
      case FILE_CREATE: return fail(STATUS_OBJECT_NAME_COLLISION);
      case FILE_SUPERSEDE: information = FILE_SUPERSEDED; truncate = true; break;
      case FILE_OVERWRITE:
      case FILE_OVERWRITE_IF: information = FILE_OVERWRITTEN; truncate = true; break;
      case FILE_OPEN:
      case FILE_OPEN_IF:
      default: break;
    }
    if (truncate && (p.read_only || p.is_directory)) return fail(STATUS_ACCESS_DENIED);
  } else {
    if (disposition == FILE_OPEN || disposition == FILE_OVERWRITE) return fail(STATUS_OBJECT_NAME_NOT_FOUND);
    if (p.read_only) return fail(STATUS_ACCESS_DENIED);
    information = FILE_CREATED;
    create = true;
  }

  // Write access asked on a read-only mount is downgraded to read (as Xenia); writes through it are refused.
  const bool directory = p.exists ? p.is_directory : (options & FILE_DIRECTORY_FILE) != 0;
  bool read_only = p.read_only;
  int fd = -1;
  if (directory) {
    if (create && ::mkdir(p.host.c_str(), 0755) != 0) return fail(status_from_errno(errno));
    fd = ::open(p.host.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  } else {
    int flags = O_CLOEXEC | (read_only ? O_RDONLY : O_RDWR);
    if (create) flags |= O_CREAT | O_EXCL;
    if (truncate) flags |= O_TRUNC;
    fd = ::open(p.host.c_str(), flags, 0644);
    if (fd < 0 && errno == EACCES && !read_only && !create && !truncate) {  // a host file without write permission
      fd = ::open(p.host.c_str(), O_RDONLY | O_CLOEXEC);
      read_only = true;
    }
  }
  if (fd < 0) return fail(status_from_errno(errno));
  auto file = FileObject::create(kernel(), fd, p.host, read_only, directory);
  if (!file) return fail(STATUS_NO_MEMORY);
  put_u32(c, handle_out, kernel().objects().add(std::move(file)));
  put_iosb(c, iosb, STATUS_SUCCESS, information);
  return STATUS_SUCCESS;
}

// Shared prologue of the I/O calls: a valid IO_STATUS_BLOCK, a file handle and (optional) an event handle.
// The event object is held for the whole request, so a concurrent NtClose of the event cannot free its pool body
// before complete_io signals it.
struct IoRequest {
  std::shared_ptr<FileObject> file;
  std::shared_ptr<KObject> event;
  uint32_t event_header = 0;
};

uint32_t begin_io(const GuestCall& c, uint32_t handle, uint32_t event, IosbPtr iosb, IoRequest& req) {
  if (!iosb.valid_range(sizeof(X_IO_STATUS_BLOCK))) return STATUS_INVALID_PARAMETER;
  req.file = file_of(handle);
  if (!req.file) return STATUS_INVALID_HANDLE;
  if (event != 0) {
    req.event = kernel().objects().get(event);
    if (!req.event) return STATUS_INVALID_HANDLE;
    req.event_header = req.event->guest_header();
    const uint8_t type = req.event_header != 0 ? c.base[req.event_header] : 0xFF;
    if (type != kNotificationEvent && type != kSynchronizationEvent) return STATUS_OBJECT_TYPE_MISMATCH;
  }
  return STATUS_SUCCESS;
}

// Completion of a synchronous request: IO_STATUS_BLOCK, then the event, then the APC (Xenia queues it only when
// both the routine, low bit masked, and the context are non-zero).
uint32_t complete_io(const GuestCall& c, const IoRequest& req, IosbPtr iosb, uint32_t apc_routine,
                     uint32_t apc_context, uint32_t status, uint32_t information) {
  put_iosb(c, iosb, status, information);
  if (req.event) (void)kernel().dispatcher().set_event(req.event_header);
  const uint32_t routine = apc_routine & ~1u;
  if (routine != 0 && apc_context != 0) {
    if (GuestThread* t = current_thread()) t->queue_apc({routine, apc_context, iosb.addr, 0});
  }
  return status;
}

// The starting offset: *byte_offset, or the file position when the pointer is null or holds -1 / -2. Status:
// STATUS_ACCESS_VIOLATION for a pointer outside guest memory, STATUS_INVALID_PARAMETER for any other negative offset.
uint32_t start_offset(const GuestCall& c, OffsetPtr byte_offset, const FileObject& f, uint64_t& out) {
  if (byte_offset.addr == 0) {
    out = f.position;
    return STATUS_SUCCESS;
  }
  if (!byte_offset.valid_range(8)) return STATUS_ACCESS_VIOLATION;
  be<int64_t> raw;
  std::memcpy(&raw, c.base + byte_offset.addr, 8);
  const int64_t v = raw;
  if (v == -1 || v == kUseFilePointer) {
    out = f.position;
  } else if (v < 0) {
    return STATUS_INVALID_PARAMETER;
  } else {
    out = static_cast<uint64_t>(v);
  }
  return STATUS_SUCCESS;
}

// pread until `length` bytes, EOF or an error; errno-derived status in `status` on error.
uint32_t read_at(int fd, uint8_t* dst, uint32_t length, uint64_t offset, uint32_t& done) {
  done = 0;
  while (done < length) {
    const ssize_t n = ::pread(fd, dst + done, length - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return status_from_errno(errno);
    }
    if (n == 0) break;
    done += static_cast<uint32_t>(n);
  }
  return STATUS_SUCCESS;
}

}  // namespace

namespace exports {

// NtCreateFile(handle*, desired_access, object_attributes*, io_status_block*, allocation_size*, file_attributes,
// share_access, creation_disposition, create_options). information = FILE_SUPERSEDED/OPENED/CREATED/OVERWRITTEN.
uint32_t NtCreateFile(GuestCall& c, U32Ptr handle_out, uint32_t access, GuestPtr<X_OBJECT_ATTRIBUTES> oa,
                      IosbPtr iosb, uint32_t /*allocation_size*/, uint32_t /*file_attributes*/,
                      uint32_t /*share_access*/, uint32_t disposition, uint32_t options) {
  return open_file(c, handle_out, access, oa, iosb, disposition, options);
}

// NtOpenFile(handle*, desired_access, object_attributes*, io_status_block*, share_access, open_options): FILE_OPEN.
uint32_t NtOpenFile(GuestCall& c, U32Ptr handle_out, uint32_t access, GuestPtr<X_OBJECT_ATTRIBUTES> oa, IosbPtr iosb,
                    uint32_t /*share_access*/, uint32_t options) {
  return open_file(c, handle_out, access, oa, iosb, FILE_OPEN, options);
}

// NtReadFile(handle, event, apc_routine, apc_context, io_status_block*, buffer, length, byte_offset*).
uint32_t NtReadFile(GuestCall& c, uint32_t handle, uint32_t event, uint32_t apc_routine, uint32_t apc_context,
                    IosbPtr iosb, GuestPtr<uint8_t> buffer, uint32_t length, OffsetPtr byte_offset) {
  IoRequest req;
  if (const uint32_t s = begin_io(c, handle, event, iosb, req); s != STATUS_SUCCESS) return s;
  FileObject& f = *req.file;
  if (f.is_directory()) return STATUS_INVALID_DEVICE_REQUEST;
  if (length != 0 && !buffer.valid_range(length)) return STATUS_ACCESS_VIOLATION;
  std::lock_guard lock(f.mutex);
  uint64_t start = 0;
  if (const uint32_t s = start_offset(c, byte_offset, f, start); s != STATUS_SUCCESS) return s;
  uint32_t done = 0;
  uint32_t status = length == 0 ? STATUS_SUCCESS : read_at(f.fd(), c.base + buffer.addr, length, start, done);
  if (status == STATUS_SUCCESS && length != 0 && done == 0) status = STATUS_END_OF_FILE;
  if (status == STATUS_SUCCESS) f.position = start + done;
  return complete_io(c, req, iosb, apc_routine, apc_context, status, done);
}

// NtReadFileScatter(handle, event, apc_routine, apc_context, io_status_block*, segment_array*, length,
// byte_offset*): segment_array holds one guest page address (be u32) per 4 KiB of `length`, as Xenia's ReadScatter.
uint32_t NtReadFileScatter(GuestCall& c, uint32_t handle, uint32_t event, uint32_t apc_routine,
                           uint32_t apc_context, IosbPtr iosb, U32Ptr segments, uint32_t length,
                           OffsetPtr byte_offset) {
  constexpr uint32_t kPage = 0x1000;
  IoRequest req;
  if (const uint32_t s = begin_io(c, handle, event, iosb, req); s != STATUS_SUCCESS) return s;
  FileObject& f = *req.file;
  if (f.is_directory()) return STATUS_INVALID_DEVICE_REQUEST;
  const uint32_t pages = static_cast<uint32_t>((uint64_t{length} + kPage - 1) / kPage);
  if (!segments.valid_range(size_t{pages} * 4)) return STATUS_ACCESS_VIOLATION;
  // One copy of the segment addresses, validated and then used: the guest cannot change them in between.
  std::vector<uint32_t> pages_at(pages);
  for (uint32_t i = 0; i < pages; ++i) {
    be<uint32_t> raw;
    std::memcpy(&raw, c.base + segments.addr + size_t{i} * 4, 4);
    pages_at[i] = raw;
    const uint32_t chunk = std::min(kPage, length - i * kPage);
    if (!GuestPtr<uint8_t>{pages_at[i]}.valid_range(chunk)) return STATUS_ACCESS_VIOLATION;
  }
  std::lock_guard lock(f.mutex);
  uint64_t start = 0;
  if (const uint32_t s = start_offset(c, byte_offset, f, start); s != STATUS_SUCCESS) return s;
  uint32_t total = 0, status = STATUS_SUCCESS;
  for (uint32_t i = 0; i < pages && status == STATUS_SUCCESS; ++i) {
    const uint32_t chunk = std::min(kPage, length - i * kPage);
    uint32_t done = 0;
    status = read_at(f.fd(), c.base + pages_at[i], chunk, start + total, done);
    total += done;
    if (done < chunk) break;  // end of file
  }
  if (status == STATUS_SUCCESS && length != 0 && total == 0) status = STATUS_END_OF_FILE;
  if (status == STATUS_SUCCESS) f.position = start + total;
  return complete_io(c, req, iosb, apc_routine, apc_context, status, total);
}

// NtWriteFile(handle, event, apc_routine, apc_context, io_status_block*, buffer, length, byte_offset*).
uint32_t NtWriteFile(GuestCall& c, uint32_t handle, uint32_t event, uint32_t apc_routine, uint32_t apc_context,
                     IosbPtr iosb, GuestPtr<uint8_t> buffer, uint32_t length, OffsetPtr byte_offset) {
  IoRequest req;
  if (const uint32_t s = begin_io(c, handle, event, iosb, req); s != STATUS_SUCCESS) return s;
  FileObject& f = *req.file;
  if (f.is_directory()) return STATUS_INVALID_DEVICE_REQUEST;
  if (f.read_only()) return STATUS_ACCESS_DENIED;
  if (length != 0 && !buffer.valid_range(length)) return STATUS_ACCESS_VIOLATION;
  std::lock_guard lock(f.mutex);
  uint64_t start = 0;
  if (const uint32_t s = start_offset(c, byte_offset, f, start); s != STATUS_SUCCESS) return s;
  uint32_t done = 0, status = STATUS_SUCCESS;
  while (done < length) {
    const ssize_t n = ::pwrite(f.fd(), c.base + buffer.addr + done, length - done, static_cast<off_t>(start + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      status = errno == ENOSPC ? STATUS_DISK_FULL : status_from_errno(errno);
      break;
    }
    if (n == 0) {  // no progress for a non-zero length: stop instead of spinning
      status = STATUS_DISK_FULL;
      break;
    }
    done += static_cast<uint32_t>(n);
  }
  if (status == STATUS_SUCCESS) f.position = start + done;
  return complete_io(c, req, iosb, apc_routine, apc_context, status, done);
}

// NtQueryInformationFile(handle, io_status_block*, info*, length, class): classes 5 Standard, 6 Internal (host inode
// number), 14 Position, 34 NetworkOpen; other classes -> STATUS_INVALID_PARAMETER.
uint32_t NtQueryInformationFile(GuestCall& c, uint32_t handle, IosbPtr iosb, GuestPtr<uint8_t> info, uint32_t length,
                                uint32_t cls) {
  if (!iosb.valid_range(sizeof(X_IO_STATUS_BLOCK))) return STATUS_INVALID_PARAMETER;
  auto f = file_of(handle);
  if (!f) return STATUS_INVALID_HANDLE;
  uint32_t size = 0;
  switch (cls) {
    case kFileStandardInformation: size = sizeof(X_FILE_STANDARD_INFORMATION); break;
    case kFileInternalInformation:
    case kFilePositionInformation: size = 8; break;
    case kFileNetworkOpenInformation: size = sizeof(X_FILE_NETWORK_OPEN_INFORMATION); break;
    default: return STATUS_INVALID_PARAMETER;
  }
  if (length < size) return STATUS_INFO_LENGTH_MISMATCH;
  if (!info.valid_range(size)) return STATUS_ACCESS_VIOLATION;
  struct stat st {};
  if (::fstat(f->fd(), &st) != 0) return status_from_errno(errno);
  std::lock_guard lock(f->mutex);
  switch (cls) {
    case kFileStandardInformation: {
      X_FILE_STANDARD_INFORMATION s{};
      const X_FILE_NETWORK_OPEN_INFORMATION n = network_open_info(st);
      s.allocation_size = n.allocation_size;
      s.end_of_file = n.end_of_file;
      s.number_of_links = 1;
      s.delete_pending = f->delete_on_close ? 1 : 0;
      s.directory = f->is_directory() ? 1 : 0;
      put_struct(c, info.addr, s);
      break;
    }
    case kFileInternalInformation: put_struct(c, info.addr, be<uint64_t>(static_cast<uint64_t>(st.st_ino))); break;
    case kFilePositionInformation: put_struct(c, info.addr, be<uint64_t>(f->position)); break;
    case kFileNetworkOpenInformation: put_struct(c, info.addr, network_open_info(st)); break;
    default: break;
  }
  put_iosb(c, iosb, STATUS_SUCCESS, size);
  return STATUS_SUCCESS;
}

// NtSetInformationFile(handle, io_status_block*, info*, length, class): 13 Disposition (delete on close), 14
// Position, 20 EndOfFile; others -> STATUS_INVALID_PARAMETER. Changing a read-only file -> STATUS_ACCESS_DENIED.
uint32_t NtSetInformationFile(GuestCall& c, uint32_t handle, IosbPtr iosb, GuestPtr<uint8_t> info, uint32_t length,
                              uint32_t cls) {
  if (!iosb.valid_range(sizeof(X_IO_STATUS_BLOCK))) return STATUS_INVALID_PARAMETER;
  auto f = file_of(handle);
  if (!f) return STATUS_INVALID_HANDLE;
  uint32_t size = 0;
  switch (cls) {
    case kFileDispositionInformation: size = 1; break;
    case kFilePositionInformation:
    case kFileEndOfFileInformation: size = 8; break;
    default: return STATUS_INVALID_PARAMETER;
  }
  if (length < size) return STATUS_INFO_LENGTH_MISMATCH;
  if (!info.valid_range(size)) return STATUS_ACCESS_VIOLATION;
  be<uint64_t> v64;
  if (size == 8) std::memcpy(&v64, c.base + info.addr, 8);
  std::lock_guard lock(f->mutex);
  switch (cls) {
    case kFileDispositionInformation:
      if (f->read_only()) return STATUS_ACCESS_DENIED;
      f->delete_on_close = c.base[info.addr] != 0;
      break;
    case kFilePositionInformation: f->position = v64; break;
    case kFileEndOfFileInformation:
      if (f->read_only() || f->is_directory()) return STATUS_ACCESS_DENIED;
      if (::ftruncate(f->fd(), static_cast<off_t>(uint64_t{v64})) != 0) return status_from_errno(errno);
      break;
    default: break;
  }
  put_iosb(c, iosb, STATUS_SUCCESS, 0);
  return STATUS_SUCCESS;
}

// NtQueryFullAttributesFile(object_attributes*, X_FILE_NETWORK_OPEN_INFORMATION*).
uint32_t NtQueryFullAttributesFile(GuestCall& c, GuestPtr<X_OBJECT_ATTRIBUTES> oa,
                                   GuestPtr<X_FILE_NETWORK_OPEN_INFORMATION> info) {
  ResolvedPath p;
  if (const uint32_t s = resolve_attributes(c, oa, p); s != STATUS_SUCCESS) return s;
  if (!p.exists) return STATUS_OBJECT_NAME_NOT_FOUND;
  if (!info.valid_range(sizeof(X_FILE_NETWORK_OPEN_INFORMATION))) return STATUS_ACCESS_VIOLATION;
  struct stat st {};
  if (::stat(p.host.c_str(), &st) != 0) return status_from_errno(errno);
  put_struct(c, info.addr, network_open_info(st));
  return STATUS_SUCCESS;
}

// NtQueryDirectoryFile(handle, event, apc_routine, apc_context, io_status_block*, X_FILE_DIRECTORY_INFORMATION*,
// length, file_name (X_ANSI_STRING*, the pattern), restart_scan). One entry per call (as Xenia), next_entry_offset 0;
// information = 0x40 + name length. The pattern of the first query (or of a restart) selects the names; later
// patterns are ignored. No match on that first query -> STATUS_NO_SUCH_FILE, end of the listing ->
// STATUS_NO_MORE_FILES, an entry that does not fit -> STATUS_BUFFER_OVERFLOW (not consumed).
uint32_t NtQueryDirectoryFile(GuestCall& c, uint32_t handle, uint32_t event, uint32_t apc_routine,
                              uint32_t apc_context, IosbPtr iosb, GuestPtr<uint8_t> info, uint32_t length,
                              GuestPtr<X_ANSI_STRING> file_name, uint32_t restart_scan) {
  constexpr uint32_t kMinLength = 72;  // Xenia: smaller buffers -> STATUS_INFO_LENGTH_MISMATCH
  IoRequest req;
  if (const uint32_t s = begin_io(c, handle, event, iosb, req); s != STATUS_SUCCESS) return s;
  FileObject& f = *req.file;
  if (!f.is_directory()) return STATUS_INVALID_PARAMETER;
  if (length < kMinLength) return STATUS_INFO_LENGTH_MISMATCH;
  if (!info.valid_range(length)) return STATUS_ACCESS_VIOLATION;
  std::string pattern;
  if (file_name.addr != 0 && !read_ansi_string(c.base, file_name, pattern)) return STATUS_INVALID_PARAMETER;

  std::lock_guard lock(f.mutex);
  const bool first = !f.listing || (restart_scan & 0xFF) != 0;
  if (first) {
    if (pattern.empty()) pattern = "*";
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(f.host(), ec), end; !ec && it != end; it.increment(ec)) {
      std::string n = it->path().filename().string();
      if (wildcard_match(pattern, n)) names.push_back(std::move(n));
    }
    std::sort(names.begin(), names.end());
    f.listing = std::move(names);
    f.next_entry = 0;
  }
  if (f.next_entry >= f.listing->size()) {
    return complete_io(c, req, iosb, apc_routine, apc_context, first ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES, 0);
  }
  const std::string& name = (*f.listing)[f.next_entry];
  const uint32_t needed = static_cast<uint32_t>(sizeof(X_FILE_DIRECTORY_INFORMATION) + name.size());
  if (needed > length) return complete_io(c, req, iosb, apc_routine, apc_context, STATUS_BUFFER_OVERFLOW, 0);
  struct stat st {};
  const bool have_stat = ::stat((f.host() / name).c_str(), &st) == 0;  // a name deleted meanwhile reports zeros
  const X_FILE_NETWORK_OPEN_INFORMATION n = have_stat ? network_open_info(st) : X_FILE_NETWORK_OPEN_INFORMATION{};
  X_FILE_DIRECTORY_INFORMATION d{};
  d.next_entry_offset = 0;
  d.file_index = 0;
  d.creation_time = n.creation_time;
  d.last_access_time = n.last_access_time;
  d.last_write_time = n.last_write_time;
  d.change_time = n.change_time;
  d.end_of_file = n.end_of_file;
  d.allocation_size = n.allocation_size;
  d.attributes = have_stat ? uint32_t{n.attributes} : FILE_ATTRIBUTE_NORMAL;
  d.file_name_length = static_cast<uint32_t>(name.size());
  put_struct(c, info.addr, d);
  std::memcpy(c.base + info.addr + sizeof d, name.data(), name.size());
  ++f.next_entry;
  return complete_io(c, req, iosb, apc_routine, apc_context, STATUS_SUCCESS, needed);
}

// NtQueryVolumeInformationFile(handle, io_status_block*, info*, length, class): 3 FileFsSizeInformation and
// 5 FileFsAttributeInformation with the fixed kVolume* values above; others -> STATUS_INVALID_PARAMETER.
uint32_t NtQueryVolumeInformationFile(GuestCall& c, uint32_t handle, IosbPtr iosb, GuestPtr<uint8_t> info,
                                      uint32_t length, uint32_t cls) {
  if (!iosb.valid_range(sizeof(X_IO_STATUS_BLOCK))) return STATUS_INVALID_PARAMETER;
  if (!file_of(handle)) return STATUS_INVALID_HANDLE;
  if (cls == kFileFsSizeInformation) {
    if (length < sizeof(X_FILE_FS_SIZE_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
    if (!info.valid_range(sizeof(X_FILE_FS_SIZE_INFORMATION))) return STATUS_ACCESS_VIOLATION;
    X_FILE_FS_SIZE_INFORMATION s{};
    s.total_allocation_units = kVolumeTotalUnits;
    s.available_allocation_units = kVolumeFreeUnits;
    s.sectors_per_allocation_unit = kVolumeSectorsPerUnit;
    s.bytes_per_sector = kVolumeBytesPerSector;
    put_struct(c, info.addr, s);
    put_iosb(c, iosb, STATUS_SUCCESS, sizeof s);
    return STATUS_SUCCESS;
  }
  if (cls == kFileFsAttributeInformation) {
    constexpr uint32_t kNameLen = sizeof(kVolumeFsName) - 1;
    if (length < sizeof(X_FILE_FS_ATTRIBUTE_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
    const uint32_t full = sizeof(X_FILE_FS_ATTRIBUTE_INFORMATION) + kNameLen;
    const uint32_t written = length >= full ? full : sizeof(X_FILE_FS_ATTRIBUTE_INFORMATION);
    if (!info.valid_range(written)) return STATUS_ACCESS_VIOLATION;
    X_FILE_FS_ATTRIBUTE_INFORMATION a{};
    a.attributes = kVolumeAttributes;
    a.component_name_max_length = kVolumeMaxComponent;
    a.name_length = kNameLen;
    put_struct(c, info.addr, a);
    if (written < full) {  // as Xenia: the name does not fit
      put_iosb(c, iosb, STATUS_BUFFER_OVERFLOW, written);
      return STATUS_BUFFER_OVERFLOW;
    }
    std::memcpy(c.base + info.addr + sizeof a, kVolumeFsName, kNameLen);
    put_iosb(c, iosb, STATUS_SUCCESS, written);
    return STATUS_SUCCESS;
  }
  return STATUS_INVALID_PARAMETER;
}

}  // namespace exports

LR_KERNEL(NtCreateFile);
LR_KERNEL(NtOpenFile);
LR_KERNEL(NtReadFile);
LR_KERNEL(NtReadFileScatter);
LR_KERNEL(NtWriteFile);
LR_KERNEL(NtQueryInformationFile);
LR_KERNEL(NtSetInformationFile);
LR_KERNEL(NtQueryFullAttributesFile);
LR_KERNEL(NtQueryDirectoryFile);
LR_KERNEL(NtQueryVolumeInformationFile);
// Device I/O controls (disk geometry, partition info) are not modelled; Xenia only answers two disk IOCTLs, which
// the game is not known to send in M2.
LR_KERNEL_STUB(NtDeviceIoControlFile, STATUS_NOT_IMPLEMENTED);
// The file system cache is the host's: there is no element count to report or set (Xenia returns 0 / success).
LR_KERNEL_STUB(FscGetCacheElementCount, 0u);
LR_KERNEL_STUB(FscSetCacheElementCount, STATUS_SUCCESS);

}  // namespace lr::kernel
