// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <string_view>

#include "common/result.h"
#include "pipeline/types.h"

namespace lr {

class StateStore {
 public:
  // Missing file -> empty store. Corrupt file -> InvalidFormat.
  static Result<StateStore> load(const fs::path& path);

  bool is_complete(StageId id, std::string_view fingerprint) const;
  void mark_started(StageId id);
  void mark_complete(StageId id, std::string fingerprint);
  Result<void> save() const;

 private:
  struct Entry {
    bool complete = false;
    std::string fingerprint;
  };
  fs::path path_;
  std::map<std::string, Entry, std::less<>> stages_;
};

}  // namespace lr
