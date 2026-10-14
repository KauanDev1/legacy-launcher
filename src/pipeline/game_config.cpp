// SPDX-License-Identifier: MIT
#include "pipeline/game_config.h"

#include <toml++/toml.hpp>

namespace lr::pipeline {

namespace {

Result<std::string> get_str(const toml::table& t, std::string_view section, std::string_view key) {
  const toml::node* n = section.empty() ? t.get(key) : nullptr;
  if (!section.empty()) {
    if (const auto* s = t.get_as<toml::table>(section)) n = s->get(key);
  }
  const std::string full = section.empty() ? std::string(key) : std::string(section) + "." + std::string(key);
  if (!n) return Error{ErrorCode::InvalidFormat, "game.toml: missing key '" + full + "'", ""};
  if (auto v = n->value<std::string>()) return *v;
  return Error{ErrorCode::InvalidFormat, "game.toml: key '" + full + "' must be a string", ""};
}

}  // namespace

Result<GameConfig> load_game_config(const fs::path& dir) {
  const fs::path file = dir / "game.toml";
  std::error_code ec;
  if (!fs::exists(file, ec))
    return Error{ErrorCode::NotFound, "game config not found: " + file.string(), ""};

  toml::table tbl;
#if TOML_EXCEPTIONS
  try {
    tbl = toml::parse_file(file.string());
  } catch (const toml::parse_error& e) {
    return Error{ErrorCode::InvalidFormat, file.string() + ": " + std::string(e.description()), ""};
  } catch (const std::exception& e) {
    return Error{ErrorCode::IoError, file.string() + ": " + e.what(), ""};
  }
#else
  auto parsed = toml::parse_file(file.string());
  if (!parsed)
    return Error{ErrorCode::InvalidFormat,
                 file.string() + ": " + std::string(parsed.error().description()), ""};
  tbl = std::move(parsed).table();
#endif

  GameConfig g;
  const auto* tid = tbl.get("title_id");
  if (!tid) return Error{ErrorCode::InvalidFormat, "game.toml: missing key 'title_id'", ""};
  auto tv = tid->value<int64_t>();
  if (!tv || *tv < 0 || *tv > 0xFFFFFFFFll)
    return Error{ErrorCode::InvalidFormat, "game.toml: key 'title_id' must be a 32-bit integer", ""};
  g.title_id = static_cast<uint32_t>(*tv);

  LR_TRY_ASSIGN(package, get_str(tbl, "sources", "package"));
  g.package = std::move(package);
  LR_TRY_ASSIGN(title_update, get_str(tbl, "sources", "title_update"));
  g.title_update = std::move(title_update);
  LR_TRY_ASSIGN(package_xex, get_str(tbl, "sources", "package_xex"));
  g.package_xex = std::move(package_xex);
  LR_TRY_ASSIGN(tu_xexp, get_str(tbl, "sources", "tu_xexp"));
  g.tu_xexp = std::move(tu_xexp);
  LR_TRY_ASSIGN(h1, get_str(tbl, "hashes", "package_xex"));
  g.hash_package_xex = std::move(h1);
  LR_TRY_ASSIGN(h2, get_str(tbl, "hashes", "tu_xexp"));
  g.hash_tu_xexp = std::move(h2);
  LR_TRY_ASSIGN(h3, get_str(tbl, "hashes", "patched_image"));
  g.hash_patched_image = std::move(h3);
  g.recomp_toml = dir / "recomp.toml";
  return g;
}

}  // namespace lr::pipeline
