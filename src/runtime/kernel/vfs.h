// SPDX-License-Identifier: MIT
// Guest path layer (spec section 9): Xbox paths (game:\..., \??\game:\..., \Device\Cdrom0\..., cache:\...) to host
// paths. Devices are mounted on host directories; symlinks (ObCreateSymbolicLink) map names, possibly chained, to
// devices. Matching is ASCII case-insensitive everywhere. Task 22 builds the Nt*File imports on top of resolve().
//
// Case-insensitive lookup: read-only mounts build a lowercase name index per directory on first use and keep it for
// the whole run (the game tree never changes; a failed listing is not cached); writable mounts try an exact match and otherwise list the directory
// on every lookup (no cache: the guest creates and deletes files, so a stale cache would be worse than the cost).
// Host symlinks inside a mount are followed (the guest cannot create them).
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/result.h"

namespace lr::kernel {

struct ResolvedPath {
  std::filesystem::path host;  // existing name's host spelling, or the guest spelling for a missing last component
  bool read_only = false;
  bool exists = false;
  bool is_directory = false;
};

class Vfs {
 public:
  Vfs() = default;
  Vfs(const Vfs&) = delete;
  Vfs& operator=(const Vfs&) = delete;

  // Defaults: \Device\Cdrom0 -> game_dir (ro); \Device\Harddisk0\Cache0, \Device\Harddisk0\Partition0 and
  // \Device\Harddisk0\Partition1 -> <user_dir>/cache (rw, created, one folder for all three, spec section 9); symlinks
  // game: and d: -> \Device\Cdrom0, cache: -> \Device\Harddisk0\Cache0. An empty user_dir mounts no disk device. If
  // <user_dir>/cache cannot be created no disk device is mounted (cache: and the partitions then resolve to
  // NotFound), a warning goes to stderr and false is returned; the kernel carries on.
  bool mount_defaults(const std::filesystem::path& game_dir, const std::filesystem::path& user_dir);

  // `device` such as "\\Device\\Cdrom0" (case-insensitive). Replaces an existing mount of the same name.
  void mount(std::string device, std::filesystem::path host_dir, bool read_only);

  // Names may carry a leading \??\ and be given with or without the trailing ':' / '\'. NTSTATUS results:
  // STATUS_OBJECT_NAME_COLLISION (link exists), STATUS_OBJECT_NAME_NOT_FOUND (delete), STATUS_INVALID_PARAMETER.
  uint32_t create_symlink(std::string_view link, std::string_view target);
  uint32_t delete_symlink(std::string_view link);

  // guest_path: "game:\\Data\\x.arc", "\\??\\game:\\...", "\\Device\\Cdrom0\\..." or, with root_dir (the host path of
  // an open directory), a relative path. Components split on \ and /; empty and "." ones are ignored; ".." pops but
  // never leaves the mount root. Errors: NotFound (unknown device, missing intermediate directory, ".." out of the
  // mount, trailing separator on a file), InvalidFormat (NUL / control bytes).
  Result<ResolvedPath> resolve(std::string_view guest_path, const std::filesystem::path* root_dir = nullptr) const;

 private:
  struct Mount {
    std::filesystem::path host;
    bool read_only;
  };
  struct Link {
    bool absolute;
    std::vector<std::string> tokens;
  };
  static bool list_dir(const std::filesystem::path& dir, std::vector<std::string>& names);  // false: listing failed
  bool find_child(const std::filesystem::path& dir, const std::string& name, bool read_only, std::string& out) const;

  mutable std::mutex mu_;  // mounts_ and links_
  std::map<std::string, Mount> mounts_;
  std::map<std::string, Link> links_;

  mutable std::mutex index_mu_;
  mutable std::unordered_map<std::string, std::unordered_map<std::string, std::string>> index_;  // dir -> lower -> name
};

}  // namespace lr::kernel
