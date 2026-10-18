// SPDX-License-Identifier: MIT
#include "runtime/kernel/vfs.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <system_error>

#include "runtime/kernel/status.h"

namespace lr::kernel {
namespace fs = std::filesystem;
namespace {

constexpr int kMaxLinkHops = 16;

std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return r;
}

struct Parsed {
  bool absolute = false;
  bool trailing_sep = false;
  std::vector<std::string> tokens;  // no empty and no "." components
};

// Strips a leading \??\, splits on \ and /, drops empty and "." components. Control bytes (including NUL) -> nullopt.
std::optional<Parsed> parse(std::string_view s) {
  for (char c : s)
    if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
  if (s.substr(0, 4) == "\\??\\") s.remove_prefix(4);
  Parsed p;
  p.absolute = !s.empty() && (s[0] == '\\' || s[0] == '/');
  p.trailing_sep = !s.empty() && (s.back() == '\\' || s.back() == '/');
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find_first_of("\\/", i);
    if (j == std::string_view::npos) j = s.size();
    const std::string_view t = s.substr(i, j - i);
    if (!t.empty() && t != ".") p.tokens.emplace_back(t);
    i = j + 1;
  }
  return p;
}

// Key of the first n tokens: lowercase, joined by '\', with a leading '\' for absolute paths.
std::string key_of(bool absolute, const std::vector<std::string>& tokens, size_t n) {
  std::string k = absolute ? "\\" : "";
  for (size_t i = 0; i < n; ++i) {
    if (i) k += '\\';
    k += lower(tokens[i]);
  }
  return k;
}

// Symlink / device names: "game" and "game:" are the same drive-style name.
std::optional<Parsed> parse_name(std::string_view s) {
  auto p = parse(s);
  if (!p || p->tokens.empty()) return std::nullopt;
  if (!p->absolute && p->tokens.size() == 1 && p->tokens[0].back() != ':') p->tokens[0] += ':';
  return p;
}

bool is_dir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

bool path_exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

fs::path strip_trailing_sep(fs::path p) {
  p = p.lexically_normal();
  if (p.has_relative_path() && p.filename().empty()) p = p.parent_path();
  return p;
}

}  // namespace

void Vfs::mount(std::string device, fs::path host_dir, bool read_only) {
  auto p = parse_name(device);
  if (!p) return;
  std::lock_guard lk(mu_);
  mounts_[key_of(p->absolute, p->tokens, p->tokens.size())] = Mount{std::move(host_dir), read_only};
}

bool Vfs::mount_defaults(const fs::path& game_dir, const fs::path& user_dir) {
  bool ok = true;
  mount("\\Device\\Cdrom0", game_dir, true);
  if (!user_dir.empty()) {
    std::error_code ec;
    fs::create_directories(user_dir / "cache", ec);
    if (ec || !fs::is_directory(user_dir / "cache")) {
      std::fprintf(stderr, "[kernel] warning: cannot create %s: %s; cache: is unavailable\n",
                   (user_dir / "cache").c_str(), ec ? ec.message().c_str() : "not a directory");
      ok = false;
    } else {
      mount("\\Device\\Harddisk0\\Cache0", user_dir / "cache", false);
    }
  }
  create_symlink("game:", "\\Device\\Cdrom0");
  create_symlink("d:", "\\Device\\Cdrom0");
  create_symlink("cache:", "\\Device\\Harddisk0\\Cache0");
  return ok;
}

uint32_t Vfs::create_symlink(std::string_view link, std::string_view target) {
  auto l = parse_name(link);
  auto t = parse_name(target);
  if (!l || !t) return STATUS_INVALID_PARAMETER;
  std::lock_guard lk(mu_);
  const std::string k = key_of(l->absolute, l->tokens, l->tokens.size());
  if (links_.count(k)) return STATUS_OBJECT_NAME_COLLISION;
  links_[k] = Link{t->absolute, std::move(t->tokens)};
  return STATUS_SUCCESS;
}

uint32_t Vfs::delete_symlink(std::string_view link) {
  auto l = parse_name(link);
  if (!l) return STATUS_INVALID_PARAMETER;
  std::lock_guard lk(mu_);
  return links_.erase(key_of(l->absolute, l->tokens, l->tokens.size())) ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND;
}

bool Vfs::list_dir(const fs::path& dir, std::vector<std::string>& names) {
  names.clear();
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    names.push_back(it->path().filename().string());
  std::sort(names.begin(), names.end());
  return !ec;
}

bool Vfs::find_child(const fs::path& dir, const std::string& name, bool read_only, std::string& out) const {
  if (read_only) {
    std::lock_guard lk(index_mu_);
    auto it = index_.find(dir.string());
    if (it == index_.end()) {
      std::vector<std::string> names;
      if (!list_dir(dir, names)) return false;  // listing failed: cache nothing, retry on the next lookup
      std::unordered_map<std::string, std::string> m;
      for (auto& n : names) m.emplace(lower(n), n);  // sorted: the smallest spelling wins a clash
      it = index_.emplace(dir.string(), std::move(m)).first;
    }
    auto f = it->second.find(lower(name));
    if (f == it->second.end()) return false;
    out = f->second;
    return true;
  }
  if (path_exists(dir / name)) {
    out = name;
    return true;
  }
  const std::string want = lower(name);
  std::vector<std::string> names;
  list_dir(dir, names);
  for (auto& n : names)
    if (lower(n) == want) {
      out = n;
      return true;
    }
  return false;
}

Result<ResolvedPath> Vfs::resolve(std::string_view guest_path, const fs::path* root_dir) const {
  auto parsed = parse(guest_path);
  if (!parsed) return Error{ErrorCode::InvalidFormat, "control byte in guest path", ""};
  const std::string shown(guest_path);
  const auto not_found = [&](const char* why) { return Error{ErrorCode::NotFound, std::string(why) + ": " + shown, ""}; };

  fs::path cur, mount_root;
  bool read_only = false;
  std::vector<std::string> rest;

  const bool drive_style = !parsed->tokens.empty() && parsed->tokens[0].back() == ':';
  if (root_dir != nullptr && !parsed->absolute && !drive_style) {
    cur = strip_trailing_sep(*root_dir);
    mount_root = cur;
    {
      std::lock_guard lk(mu_);
      for (auto& [k, m] : mounts_) {  // the mount containing root_dir: read_only and the ".." limit
        const fs::path mh = strip_trailing_sep(m.host);
        const fs::path rel = cur.lexically_relative(mh);
        if (!rel.empty() && *rel.begin() != "..") {
          mount_root = mh;
          read_only = m.read_only;
          break;
        }
      }
    }
    rest = parsed->tokens;
  } else {
    bool absolute = parsed->absolute;
    std::vector<std::string> tokens = parsed->tokens;
    bool found = false;
    for (int hop = 0; hop < kMaxLinkHops && !found; ++hop) {
      bool relinked = false;
      std::lock_guard lk(mu_);
      for (size_t n = tokens.size(); n >= 1 && !found && !relinked; --n) {
        const std::string k = key_of(absolute, tokens, n);
        if (auto m = mounts_.find(k); m != mounts_.end()) {
          cur = mount_root = strip_trailing_sep(m->second.host);
          read_only = m->second.read_only;
          rest.assign(tokens.begin() + static_cast<std::ptrdiff_t>(n), tokens.end());
          found = true;
        } else if (auto l = links_.find(k); l != links_.end()) {
          std::vector<std::string> next = l->second.tokens;
          next.insert(next.end(), tokens.begin() + static_cast<std::ptrdiff_t>(n), tokens.end());
          absolute = l->second.absolute;
          tokens = std::move(next);
          relinked = true;
        }
      }
      if (!found && !relinked) return not_found("unknown device");
    }
    if (!found) return not_found("symbolic link chain too long");
  }

  for (size_t i = 0; i < rest.size(); ++i) {
    const std::string& tok = rest[i];
    const bool last = i + 1 == rest.size();
    if (tok == "..") {
      if (cur == mount_root) return not_found("path leaves the mount");
      cur = cur.parent_path();
      continue;
    }
    std::string name;
    if (!find_child(cur, tok, read_only, name)) {
      if (!last) return not_found("missing directory");
      return ResolvedPath{cur / tok, read_only, false, false};
    }
    cur /= name;
    if (!last && !is_dir(cur)) return not_found("not a directory");
  }
  ResolvedPath r;
  r.host = cur;
  r.read_only = read_only;
  r.exists = path_exists(cur);
  r.is_directory = r.exists && is_dir(cur);
  if (r.exists && !r.is_directory && parsed->trailing_sep) return not_found("not a directory");
  return r;
}

}  // namespace lr::kernel
