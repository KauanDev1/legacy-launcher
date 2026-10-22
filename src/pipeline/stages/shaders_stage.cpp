// SPDX-License-Identifier: MIT
#include "pipeline/stages/shaders_stage.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <system_error>
#include <vector>

#include "common/sha256.h"
#include "pipeline/process.h"
#include "pipeline/toolchain.h"

#ifndef LR_XENOSRECOMP_COMMIT
#define LR_XENOSRECOMP_COMMIT "unknown"
#endif

namespace lr::pipeline {

namespace {

constexpr const char* kCacheFile = "shader_cache.cpp";

bool parse_uint(std::string_view s, unsigned& out) {
  if (s.empty()) return false;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && ptr == s.data() + s.size();
}

// The tool ends the file with the lr_shader_blob_uncompressed_size definition: a partial write lacks it.
bool cache_is_complete(const fs::path& p) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec || size == 0) return false;
  std::ifstream f(p, std::ios::binary);
  const auto tail = static_cast<std::streamoff>(std::min<uintmax_t>(size, 512));
  std::string buf(static_cast<size_t>(tail), char{});
  f.seekg(-tail, std::ios::end);
  f.read(buf.data(), tail);
  return f && buf.find("lr_shader_blob_uncompressed_size") != std::string::npos;
}

class ShadersStage : public Stage {
 public:
  explicit ShadersStage(ShadersStageConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.xenosrecomp_commit.empty()) cfg_.xenosrecomp_commit = LR_XENOSRECOMP_COMMIT;
  }

  StageId id() const override { return StageId::Shaders; }
  // Only the image, the tool and DXC matter: a recompile does not invalidate the cache.
  bool independent() const override { return true; }

  Result<std::string> fingerprint(StageContext& ctx) override {
    LR_TRY_ASSIGN(img, sha256_file(ctx.paths.image / "default_tu1.xex"));
    const std::string dxc = cfg_.dxc_version ? cfg_.dxc_version(ctx.resources) : default_dxc_version(ctx.resources);
    return shaders_fingerprint(img, cfg_.xenosrecomp_commit, dxc);
  }

  std::vector<fs::path> outputs(StageContext& ctx) const override { return {shader_cache_path(ctx.paths)}; }

  Result<void> execute(StageContext& ctx) override {
    const fs::path tool = ctx.resources / "tools" / "XenosRecomp";
    const fs::path common = ctx.resources / "tools" / "xenos_shader_common.h";
    std::error_code ec;
    if (!fs::exists(tool, ec) || !fs::exists(common, ec))
      return Error{ErrorCode::ToolMissing, "XenosRecomp (shader translator) is not available: " + tool.string(),
                   "this build has no shader translator; `legacyrecomp doctor` warns when DXC is missing: install "
                   "directx-shader-compiler and build from source (the AppImage does not ship DXC yet, "
                   "docs/adr/0005-m3-plan-deviations.md)"};

    const fs::path dir = (fs::absolute(ctx.paths.work, ec) / "shaders").lexically_normal();
    const fs::path in = dir / "in";
    const fs::path cache = dir / kCacheFile;
    fs::remove_all(dir, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot clear " + dir.string() + ": " + ec.message(), ""};
    fs::create_directories(in, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create " + in.string() + ": " + ec.message(), ""};
    fs::copy_file(ctx.paths.image / "default_tu1.xex", in / "default_tu1.xex",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot copy the image to " + in.string() + ": " + ec.message(), ""};
    fs::remove(cache, ec);  // the tool writes nothing on failure; never leave a stale cache behind

    std::optional<std::pair<unsigned, unsigned>> counts;
    ToolInvocation inv;
    inv.program = tool;
    inv.args = {in.string(), cache.string(), common.string(), "--hlsl-dir", (dir / "hlsl").string()};
    inv.cwd = dir;
    inv.log_file = ctx.paths.logs / "shaders.log";
    inv.cancelled = [&] { return ctx.sink.cancelled(); };
    inv.on_line = [&](std::string_view line) {
      if (auto c = parse_translated_line(line)) {
        counts = c;
        ctx.sink.on_progress({StageId::Shaders, 1.0f, std::string(line)});
      }
    };
    fs::remove(inv.log_file, ec);  // one run per log
    LR_TRY(run_tool(inv));
    if (!counts)
      return Error{ErrorCode::ToolFailed, "XenosRecomp did not report how many shaders it translated",
                   "see " + inv.log_file.string()};
    if (counts->first != counts->second || counts->second == 0) {
      fs::remove(cache, ec);
      return Error{ErrorCode::ToolFailed,
                   "XenosRecomp translated " + std::to_string(counts->first) + " of " +
                       std::to_string(counts->second) + " shaders",
                   "every shader the game loads must be translated; see " + inv.log_file.string()};
    }
    if (!fs::exists(cache, ec))
      return Error{ErrorCode::ToolFailed, "XenosRecomp did not produce " + cache.string(),
                   "see " + inv.log_file.string()};
    if (!cache_is_complete(cache)) {
      fs::remove(cache, ec);
      return Error{ErrorCode::ToolFailed, "XenosRecomp wrote an empty or truncated " + cache.string(),
                   "see " + inv.log_file.string() + "; check the free disk space"};
    }
    return {};
  }

 private:
  static std::string default_dxc_version(const fs::path& resources) {
    const fs::path dxc =
        has_bundled_toolchain(resources) ? resources / "toolchain" / "dxc" / "bin" / "dxc" : fs::path("dxc");
    return query_dxc_version(dxc);
  }

  ShadersStageConfig cfg_;
};

}  // namespace

std::optional<std::pair<unsigned, unsigned>> parse_translated_line(std::string_view line) {
  constexpr std::string_view kPrefix = "translated ";
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
  if (!line.starts_with(kPrefix)) return std::nullopt;
  line.remove_prefix(kPrefix.size());
  const auto of = line.find(" of ");
  if (of == std::string_view::npos) return std::nullopt;
  unsigned n = 0, m = 0;
  if (!parse_uint(line.substr(0, of), n) || !parse_uint(line.substr(of + 4), m)) return std::nullopt;
  return std::make_pair(n, m);
}

std::string shaders_fingerprint(std::string_view image_sha256, std::string_view xenosrecomp_commit,
                                std::string_view dxc_version) {
  return std::string(image_sha256) + "|" + std::string(xenosrecomp_commit) + "|" + std::string(dxc_version);
}

std::string query_dxc_version(const fs::path& dxc) {
  // run_tool merges stdout and stderr: DXC prints its version on either, depending on the build.
  std::string first;
  ToolInvocation inv;
  inv.program = dxc;
  inv.args = {"--version"};
  inv.on_line = [&](std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (first.empty() && !line.empty()) first = std::string(line);
  };
  if (!run_tool(inv) || first.empty()) return "unknown";
  return first;
}

fs::path shader_cache_path(const DataPaths& paths) { return paths.work / "shaders" / "shader_cache.cpp"; }

std::unique_ptr<Stage> make_shaders_stage(ShadersStageConfig config) {
  return std::make_unique<ShadersStage>(std::move(config));
}

}  // namespace lr::pipeline
