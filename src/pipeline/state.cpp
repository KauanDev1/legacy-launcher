// SPDX-License-Identifier: MIT
#include "pipeline/state.h"

#include <cstddef>
#include <span>
#include <sstream>

#include <toml++/toml.hpp>

#include "common/fs_util.h"

namespace lr::pipeline {

Result<StateStore> StateStore::load(const fs::path& path) {
  StateStore store;
  store.path_ = path;
  std::error_code ec;
  if (!fs::exists(path, ec)) return store;

  toml::table tbl;
#if TOML_EXCEPTIONS
  try {
    tbl = toml::parse_file(path.string());
  } catch (const toml::parse_error& e) {
    return Error{ErrorCode::InvalidFormat, path.string() + ": " + std::string(e.description()), ""};
  } catch (const std::exception& e) {
    return Error{ErrorCode::IoError, path.string() + ": " + e.what(), ""};
  }
#else
  auto parsed = toml::parse_file(path.string());
  if (!parsed)
    return Error{ErrorCode::InvalidFormat,
                 path.string() + ": " + std::string(parsed.error().description()), ""};
  tbl = std::move(parsed).table();
#endif
  if (const auto* stages = tbl["stages"].as_table()) {
    for (const auto& [key, node] : *stages) {
      const auto* t = node.as_table();
      if (!t) return Error{ErrorCode::InvalidFormat,
                     path.string() + ": 'stages." + std::string(key.str()) + "' is not a table", ""};
      Entry e;
      e.complete = (*t)["complete"].value_or(false);
      e.fingerprint = (*t)["fingerprint"].value_or(std::string());
      store.stages_[std::string(key.str())] = std::move(e);
    }
  } else if (tbl.contains("stages")) {
    return Error{ErrorCode::InvalidFormat, path.string() + ": 'stages' is not a table", ""};
  }
  return store;
}

std::optional<std::string> StateStore::complete_fingerprint(StageId id) const {
  auto it = stages_.find(stage_name(id));
  if (it == stages_.end() || !it->second.complete) return std::nullopt;
  return it->second.fingerprint;
}

bool StateStore::is_complete(StageId id, std::string_view fingerprint) const {
  auto it = stages_.find(stage_name(id));
  return it != stages_.end() && it->second.complete && it->second.fingerprint == fingerprint;
}

void StateStore::mark_started(StageId id) {
  Entry& e = stages_[std::string(stage_name(id))];
  e.complete = false;
  e.fingerprint.clear();
}

void StateStore::mark_complete(StageId id, std::string fingerprint) {
  Entry& e = stages_[std::string(stage_name(id))];
  e.complete = true;
  e.fingerprint = std::move(fingerprint);
}

Result<void> StateStore::save() const {
  toml::table stages;
  for (const auto& [name, e] : stages_) {
    toml::table t;
    t.insert("complete", e.complete);
    t.insert("fingerprint", e.fingerprint);
    stages.insert(name, std::move(t));
  }
  toml::table root;
  root.insert("stages", std::move(stages));
  std::ostringstream os;
  os << root << "\n";
  const std::string s = os.str();
  return atomic_write(path_, std::as_bytes(std::span(s.data(), s.size())));
}

}  // namespace lr::pipeline
