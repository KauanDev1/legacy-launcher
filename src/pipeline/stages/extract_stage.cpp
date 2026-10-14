// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline/stages/extract_stage.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <ostream>
#include <system_error>

#include "common/fs_util.h"
#include "common/sha256.h"
#include "formats/reader.h"
#include "formats/stfs.h"
#include "formats/xdvdfs.h"
#include "pipeline/state.h"

namespace lr::pipeline {
namespace {

constexpr uint64_t kChunk = 1024 * 1024;

bool iequals(std::string_view a, std::string_view b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                    [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

// Entry paths come from untrusted media: they must stay inside game/.
Result<void> check_entry_path(const std::string& path) {
  bool bad = path.empty() || path.front() == '/';
  size_t pos = 0;
  while (!bad && pos <= path.size()) {
    size_t slash = path.find('/', pos);
    std::string_view part = std::string_view(path).substr(pos, slash == std::string::npos ? slash : slash - pos);
    if (part == "..") bad = true;
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
  if (path.find('\\') != std::string::npos) bad = true;
  if (bad) return Error{ErrorCode::InvalidFormat, "unsafe path in package: " + path, ""};
  return {};
}

struct Totals {
  uint64_t total = 0, done = 0;
};

// Extracts every entry of `pkg`. `xex_name` goes to `xex_dst`; everything else goes under `game`.
Result<void> extract_package(const formats::StfsPackage& pkg, const std::string& xex_name, const fs::path& xex_dst,
                             const fs::path& game, StageContext& ctx, Totals& t, std::string_view label) {
  bool found_xex = false;
  for (auto& e : pkg.entries()) {
    if (ctx.sink.cancelled()) return Error{ErrorCode::Cancelled, "cancelled", ""};
    LR_TRY(check_entry_path(e.path));
    std::error_code ec;
    if (e.is_dir) {
      fs::create_directories(game / e.path, ec);
      if (ec) return Error{ErrorCode::IoError, "cannot create " + (game / e.path).string() + ": " + ec.message(), ""};
      continue;
    }
    fs::path dst = iequals(e.path, xex_name) ? (found_xex = true, xex_dst) : game / e.path;
    LR_TRY_ASSIGN(file, pkg.open_file(e.path));
    uint64_t size = file->size();
    auto writer = [&](std::ostream& os) -> Result<void> {
      std::vector<std::byte> buf(static_cast<size_t>(std::min<uint64_t>(kChunk, std::max<uint64_t>(size, 1))));
      for (uint64_t off = 0; off < size;) {
        if (ctx.sink.cancelled()) return Error{ErrorCode::Cancelled, "cancelled", ""};
        size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - off));
        LR_TRY(file->read(off, std::span<std::byte>(buf.data(), n)));
        os.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(n));
        off += n;
        t.done += n;
        ctx.sink.on_progress({StageId::Extract,
                              t.total ? static_cast<float>(static_cast<double>(t.done) / static_cast<double>(t.total)) : 1.0f,
                              std::string(label) + ": " + e.path});
      }
      return {};
    };
    LR_TRY(atomic_copy_from(dst, writer));
  }
  if (!found_xex) return Error{ErrorCode::NotFound, std::string(label) + " has no " + xex_name, ""};
  return {};
}

uint64_t sum_files(const formats::StfsPackage& p) {
  uint64_t n = 0;
  for (auto& e : p.entries())
    if (!e.is_dir) n += e.size;
  return n;
}

class ExtractStage : public Stage {
 public:
  StageId id() const override { return StageId::Extract; }

  Result<std::string> fingerprint(StageContext& ctx) override {
    std::error_code ec;
    if (ctx.options.iso.empty()) {
      // `legacyrecomp build` without --iso: an already extracted game stays valid as is.
      // Only when every output is still on disk; otherwise fail here, before the runner touches state.toml
      // (mark_started would erase the stored fingerprint).
      if (ctx.state) {
        if (auto stored = ctx.state->complete_fingerprint(StageId::Extract)) {
          bool present = true;
          for (const auto& out : outputs(ctx)) present = present && fs::exists(out, ec);
          if (present) return *stored;
        }
      }
      return Error{ErrorCode::NotFound, "no usable extracted game and no --iso given",
                   "run `legacyrecomp install --iso <file>` first"};
    }
    auto size = fs::file_size(ctx.options.iso, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot stat " + ctx.options.iso.string() + ": " + ec.message(), ""};
    auto mtime = fs::last_write_time(ctx.options.iso, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot stat " + ctx.options.iso.string() + ": " + ec.message(), ""};
    LR_TRY_ASSIGN(cfg, sha256_file(ctx.game.recomp_toml.parent_path() / "game.toml"));
    auto abs_iso = fs::absolute(ctx.options.iso, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot resolve " + ctx.options.iso.string() + ": " + ec.message(), ""};
    return abs_iso.string() + "|" + std::to_string(size) + "|" +
           std::to_string(mtime.time_since_epoch().count()) + "|" + cfg;
  }

  std::vector<fs::path> outputs(StageContext& ctx) const override {
    return {ctx.paths.input / "package.xex", ctx.paths.input / "tu.xexp", ctx.paths.game / ".extracted"};
  }

  Result<void> execute(StageContext& ctx) override {
    std::error_code ec;
    if (ctx.options.iso.empty())
      return Error{ErrorCode::NotFound, "the extracted game is missing or incomplete and no --iso was given",
                   "run `legacyrecomp install --iso <file>`"};
    // Re-extraction owns game/: clear it so versions never mix. Done after validation below.

    // Lifetime chain: FileReader -> XdvdfsImage -> package reader -> StfsPackage (each borrows the previous).
    LR_TRY_ASSIGN(iso, formats::FileReader::open(ctx.options.iso));
    LR_TRY_ASSIGN(image, formats::XdvdfsImage::open(*iso));
    LR_TRY_ASSIGN(pkg_reader, image.open_file(ctx.game.package));
    LR_TRY_ASSIGN(pkg, formats::StfsPackage::open(*pkg_reader));
    LR_TRY_ASSIGN(tu_reader, image.open_file(ctx.game.title_update));
    LR_TRY_ASSIGN(tu, formats::StfsPackage::open(*tu_reader));

    for (auto* p : {&pkg, &tu})
      for (auto& e : p->entries()) LR_TRY(check_entry_path(e.path));

    fs::remove_all(ctx.paths.game, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot clear " + ctx.paths.game.string() + ": " + ec.message(), ""};
    fs::create_directories(ctx.paths.game, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create " + ctx.paths.game.string() + ": " + ec.message(), ""};

    Totals t;
    t.total = sum_files(pkg) + sum_files(tu);
    LR_TRY(extract_package(pkg, ctx.game.package_xex, ctx.paths.input / "package.xex", ctx.paths.game, ctx, t,
                           "package"));
    LR_TRY(extract_package(tu, ctx.game.tu_xexp, ctx.paths.input / "tu.xexp", ctx.paths.game, ctx, t, "title update"));

    static constexpr std::byte kMarker[] = {std::byte{'o'}, std::byte{'k'}, std::byte{'\n'}};
    LR_TRY(atomic_write(ctx.paths.game / ".extracted", kMarker));
    return {};
  }
};

}  // namespace

std::unique_ptr<Stage> make_extract_stage() { return std::make_unique<ExtractStage>(); }

}  // namespace lr::pipeline
