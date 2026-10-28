// SPDX-License-Identifier: MIT
// Open files and directories behind NtCreateFile/NtOpenFile handles (exports/files.cpp, spec section 9).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/kernel/guest_structs.h"
#include "runtime/kernel/marshal.h"
#include "runtime/kernel/objects.h"

namespace lr::kernel {

class Kernel;

// A host file or directory (fd opened with O_CLOEXEC, closed with the object). It is not waitable (guest_header()
// stays 0) but has a small zeroed pool body as its guest_body(), so ObReferenceObjectByHandle works on file
// handles; the body is freed with the object.
class FileObject final : public KObject {
 public:
  // Takes ownership of `fd`. Returns nullptr (fd closed) when the pool is exhausted.
  static std::shared_ptr<FileObject> create(Kernel& k, int fd, std::filesystem::path host, bool read_only,
                                            bool directory);
  FileObject(Kernel& k, int fd, std::filesystem::path host, bool read_only, bool directory, uint32_t body);
  ~FileObject() override;

  int fd() const { return fd_; }
  const std::filesystem::path& host() const { return host_; }
  bool read_only() const { return read_only_; }
  bool is_directory() const { return directory_; }

  // Guards the mutable state below (several guest threads may share a handle).
  std::mutex mutex;
  uint64_t position = 0;
  bool delete_on_close = false;  // FileDispositionInformation: the host file is removed in the destructor
  // NtQueryDirectoryFile: matching names (sorted) captured by the first query or a restart, and the next one.
  std::optional<std::vector<std::string>> listing;
  size_t next_entry = 0;

 private:
  Kernel& kernel_;
  const int fd_;
  const std::filesystem::path host_;
  const bool read_only_;
  const bool directory_;
  const uint32_t body_;
};

// Case-insensitive match of `name` against an NT wildcard pattern: `*` any run, `?` one character; "*.*" matches
// every name, as on NT.
bool wildcard_match(std::string_view pattern, std::string_view name);

// The text of an X_ANSI_STRING in guest memory (`base` = guest base); false when the descriptor or its buffer is
// outside guest memory.
bool read_ansi_string(const uint8_t* base, GuestPtr<X_ANSI_STRING> p, std::string& out);

}  // namespace lr::kernel
