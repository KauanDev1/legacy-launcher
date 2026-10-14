// SPDX-License-Identifier: MIT
#include "pipeline/stages/patch_stage.h"

#include <cstdint>
#include <fstream>
#include <span>
#include <vector>

#include <xex_patcher.h>

#include "common/fs_util.h"
#include "common/sha256.h"

namespace lr::pipeline {

std::string_view xex_patcher_result_name(int result) {
  switch (static_cast<XexPatcher::Result>(result)) {
    case XexPatcher::Result::Success: return "Success";
    case XexPatcher::Result::FileOpenFailed: return "FileOpenFailed";
    case XexPatcher::Result::FileWriteFailed: return "FileWriteFailed";
    case XexPatcher::Result::XexFileUnsupported: return "XexFileUnsupported";
    case XexPatcher::Result::XexFileInvalid: return "XexFileInvalid";
    case XexPatcher::Result::PatchFileInvalid: return "PatchFileInvalid";
    case XexPatcher::Result::PatchIncompatible: return "PatchIncompatible";
    case XexPatcher::Result::PatchFailed: return "PatchFailed";
    case XexPatcher::Result::PatchUnsupported: return "PatchUnsupported";
  }
  return "Unknown";
}

namespace {

Result<std::vector<uint8_t>> read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f) return Error{ErrorCode::IoError, "cannot open " + p.string(), ""};
  auto size = f.tellg();
  if (size < 0) return Error{ErrorCode::IoError, "cannot stat " + p.string(), ""};
  std::vector<uint8_t> data(static_cast<size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!f) return Error{ErrorCode::IoError, "cannot read " + p.string(), ""};
  return data;
}

// XexPatcher reads headers at offsets taken from the files without bounds checks; reject inputs
// too small to hold an XEX2 header. (The hashes were verified by the verify stage anyway.)
bool looks_like_xex2(const std::vector<uint8_t>& d) {
  return d.size() >= 0x18 && d[0] == 'X' && d[1] == 'E' && d[2] == 'X' && d[3] == '2';
}

class PatchStage : public Stage {
 public:
  StageId id() const override { return StageId::Patch; }

  Result<std::string> fingerprint(StageContext& ctx) override {
    LR_TRY_ASSIGN(a, sha256_file(ctx.paths.input / "package.xex"));
    LR_TRY_ASSIGN(b, sha256_file(ctx.paths.input / "tu.xexp"));
    return a + "|" + b + "|" + ctx.game.hash_patched_image;
  }

  std::vector<fs::path> outputs(StageContext& ctx) const override {
    return {ctx.paths.image / "default_tu1.xex"};
  }

  Result<void> execute(StageContext& ctx) override {
    LR_TRY_ASSIGN(xex, read_file(ctx.paths.input / "package.xex"));
    LR_TRY_ASSIGN(xexp, read_file(ctx.paths.input / "tu.xexp"));
    if (!looks_like_xex2(xex) || !looks_like_xex2(xexp))
      return Error{ErrorCode::InvalidFormat, "package.xex or tu.xexp is not an XEX2 file", ""};
    ctx.sink.on_progress({StageId::Patch, 0.1f, "applying title update"});

    std::vector<uint8_t> out;
    auto r = XexPatcher::apply(xex.data(), xex.size(), xexp.data(), xexp.size(), out, false);
    if (r != XexPatcher::Result::Success)
      return Error{ErrorCode::InvalidFormat,
                   "XexPatcher failed: " + std::string(xex_patcher_result_name(static_cast<int>(r))),
                   "the title update does not apply to this package.xex"};

    Sha256 h;
    h.update(std::as_bytes(std::span<const uint8_t>(out)));
    const std::string found = to_hex(h.finish());
    if (ctx.game.hash_patched_image.empty())
      return Error{ErrorCode::Internal, "patched_image hash not configured; patched image has sha256 " + found,
                   "record it under [hashes] patched_image in config/minecraft-tu1/game.toml"};
    if (found != ctx.game.hash_patched_image)
      return Error{ErrorCode::UnsupportedVersion,
                   "patched image has sha256 " + found + ", expected " + ctx.game.hash_patched_image,
                   "LegacyRecomp supports the Minecraft X360 disc (title 584111F7) with TU1"};

    ctx.sink.on_progress({StageId::Patch, 0.9f, "writing patched image"});
    LR_TRY(atomic_write(ctx.paths.image / "default_tu1.xex", std::as_bytes(std::span<const uint8_t>(out))));
    return {};
  }
};

}  // namespace

std::unique_ptr<Stage> make_patch_stage() { return std::make_unique<PatchStage>(); }

}  // namespace lr::pipeline
