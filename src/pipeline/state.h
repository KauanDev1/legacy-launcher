// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "common/result.h"
#include "pipeline/types.h"

namespace lr::pipeline {

class StateStore {
 public:
  // Lenient by design: within a stage table, a missing or mistyped `complete`/`fingerprint`
  // means "not complete", which forces a safe re-run. Only unparseable TOML or a non-table
  // `stages` / `stages.<name>` is an error.
  // Missing file -> empty store. Corrupt file -> InvalidFormat.
  static Result<StateStore> load(const fs::path& path);

  // Fingerprint of a stage that is marked complete; nullopt otherwise.
  std::optional<std::string> complete_fingerprint(StageId id) const;
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

}  // namespace lr::pipeline
