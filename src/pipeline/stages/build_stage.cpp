// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline/stages/build_stage.h"

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "common/fs_util.h"
#include "common/sha256.h"
#include "pipeline/import_stubs.h"
#include "pipeline/process.h"

namespace lr::pipeline {

namespace {

constexpr const char* kStubsFile = "import_stubs.cpp";
constexpr const char* kGameBinary = "legacyrecomp-game";

Result<std::string> read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return Error{ErrorCode::IoError, "cannot read " + p.string(), ""};
  std::ostringstream os;
  os << f.rdbuf();
  if (f.bad()) return Error{ErrorCode::IoError, "cannot read " + p.string(), ""};
  return os.str();
}

}  // namespace

std::optional<float> parse_ninja_progress(std::string_view line) {
  if (!line.starts_with('[')) return std::nullopt;
  const auto slash = line.find('/');
  const auto close = line.find(']');
  if (slash == std::string_view::npos || close == std::string_view::npos || slash > close) return std::nullopt;
  unsigned done = 0, total = 0;
  const char* b = line.data();
  auto r1 = std::from_chars(b + 1, b + slash, done);
  auto r2 = std::from_chars(b + slash + 1, b + close, total);
  if (r1.ec != std::errc() || r1.ptr != b + slash || r2.ec != std::errc() || r2.ptr != b + close || total == 0)
    return std::nullopt;
  return static_cast<float>(std::min(done, total)) / static_cast<float>(total);
}

Result<std::string> build_fingerprint(const fs::path& generated, const fs::path& runtime_dir) {
  std::error_code ec;
  if (!fs::is_directory(generated, ec))
    return Error{ErrorCode::NotFound, "generated sources not found: " + generated.string(), "run the recompile stage"};
  std::vector<std::pair<std::string, uintmax_t>> listing;
  for (fs::directory_iterator it(generated, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string name = it->path().filename().string();
    if (name == kStubsFile || name.ends_with(".tmp")) continue;
    listing.emplace_back(name, it->file_size(ec));
  }
  if (ec) return Error{ErrorCode::IoError, "cannot list " + generated.string() + ": " + ec.message(), ""};
  std::sort(listing.begin(), listing.end());

  std::vector<fs::path> runtime_files;
  for (fs::recursive_directory_iterator it(runtime_dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec)) runtime_files.push_back(it->path());
  if (ec) return Error{ErrorCode::IoError, "cannot list " + runtime_dir.string() + ": " + ec.message(), ""};
  if (runtime_files.empty())
    return Error{ErrorCode::NotFound, "runtime resources not found: " + runtime_dir.string(),
                 "rebuild LegacyRecomp (the runtime is staged into share/legacyrecomp/runtime)"};
  std::sort(runtime_files.begin(), runtime_files.end());

  std::string text;
  for (const auto& [name, size] : listing) text += name + " " + std::to_string(size) + "\n";
  text += "--\n";
  for (const auto& f : runtime_files) {
    LR_TRY_ASSIGN(h, sha256_file(f));
    text += f.lexically_relative(runtime_dir).generic_string() + " " + h + "\n";
  }
  Sha256 sha;
  sha.update(std::as_bytes(std::span<const char>(text.data(), text.size())));
  return to_hex(sha.finish());
}

Result<bool> reset_stale_build_dir(const fs::path& build_dir, const fs::path& template_dir) {
  std::error_code ec;
  const fs::path cache = build_dir / "CMakeCache.txt";
  if (!fs::exists(cache, ec) && !ec) return false;
  bool stale = true;
  if (std::ifstream f(cache, std::ios::binary); f) {
    constexpr std::string_view kKey = "CMAKE_HOME_DIRECTORY:INTERNAL=";
    for (std::string line; std::getline(f, line);) {
      if (!line.starts_with(kKey)) continue;
      const fs::path home = fs::path(line.substr(kKey.size())).lexically_normal();
      std::error_code eq;
      stale = home != template_dir.lexically_normal() && !fs::equivalent(home, template_dir, eq);
      break;
    }
  }
  if (!stale) return false;
  fs::remove_all(build_dir, ec);
  if (ec)
    return Error{ErrorCode::IoError, "cannot remove stale build dir " + build_dir.string() + ": " + ec.message(), ""};
  return true;
}

unsigned build_jobs(unsigned requested, unsigned hardware_threads, uint64_t total_ram_bytes) {
  if (requested) return requested;
  constexpr uint64_t kPerJob = 2ull << 30;
  const uint64_t by_ram = total_ram_bytes / kPerJob;
  const uint64_t jobs = std::min<uint64_t>(hardware_threads ? hardware_threads : 1, by_ram ? by_ram : 1);
  return static_cast<unsigned>(std::max<uint64_t>(jobs, 1));
}

namespace {

uint64_t total_ram() {
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long page = ::sysconf(_SC_PAGESIZE);
  return pages > 0 && page > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(page) : 0;
}

class BuildStage : public Stage {
 public:
  StageId id() const override { return StageId::Build; }

  Result<std::string> fingerprint(StageContext& ctx) override {
    return build_fingerprint(ctx.paths.generated, ctx.resources / "runtime");
  }

  std::vector<fs::path> outputs(StageContext& ctx) const override { return {ctx.paths.bin / kGameBinary}; }

  Result<void> execute(StageContext& ctx) override {
    std::error_code ec;
    const fs::path root = fs::absolute(ctx.paths.root, ec).lexically_normal();
    if (ec) return Error{ErrorCode::IoError, "cannot resolve " + ctx.paths.root.string() + ": " + ec.message(), ""};
    const fs::path resources = fs::absolute(ctx.resources, ec).lexically_normal();
    if (ec) return Error{ErrorCode::IoError, "cannot resolve " + ctx.resources.string() + ": " + ec.message(), ""};
    const DataPaths p = DataPaths::from_root(root);

    // 1. Weak stubs for every import the generated code declares.
    LR_TRY_ASSIGN(shared, read_text(p.generated / "ppc_recomp_shared.h"));
    const auto names = parse_import_names(shared);
    if (names.empty())
      return Error{ErrorCode::InvalidFormat, "no kernel imports found in " + (p.generated / "ppc_recomp_shared.h").string(),
                   "rerun the recompile stage (--force recompile)"};
    const std::string stubs = render_import_stubs(names);
    auto current = read_text(p.generated / kStubsFile);
    if (!current || current.value() != stubs)  // unchanged file keeps its mtime: no recompile / 48 MB relink
      LR_TRY(atomic_write(p.generated / kStubsFile, std::as_bytes(std::span<const char>(stubs.data(), stubs.size()))));
    ctx.sink.on_progress({StageId::Build, 0.0f, "wrote " + std::to_string(names.size()) + " import stubs"});

    // 2. Configure the template, 3. build it (incremental: work/build is kept between runs, unless it was
    // configured from another resources dir).
    const fs::path log = p.logs / "build.log";
    fs::remove(log, ec);  // one run per log
    const fs::path template_dir = resources / "runtime" / "template";
    LR_TRY_ASSIGN(reset, reset_stale_build_dir(p.build, template_dir));
    if (reset) ctx.sink.on_log("removed " + p.build.string() + ": it was configured from another template directory");
    ToolInvocation configure;
    configure.program = "cmake";
    configure.args = {"-S", template_dir.string(), "-B", p.build.string(), "-G", "Ninja",
                      "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=clang++",
                      "-DLR_RESOURCES=" + resources.string(), "-DLR_GENERATED=" + p.generated.string()};
    configure.log_file = log;
    configure.cancelled = [&] { return ctx.sink.cancelled(); };
    configure.on_line = [&](std::string_view line) { ctx.sink.on_log(line); };
    ctx.sink.on_progress({StageId::Build, 0.0f, "configuring the game build"});
    LR_TRY(run_tool(configure));  // ToolFailed hint: log path + output tail

    const unsigned jobs = build_jobs(ctx.options.jobs, std::thread::hardware_concurrency(), total_ram());
    ToolInvocation build;
    build.program = "cmake";
    build.args = {"--build", p.build.string(), "-j", std::to_string(jobs)};
    build.log_file = log;
    build.cancelled = configure.cancelled;
    build.on_line = [&](std::string_view line) {
      if (auto f = parse_ninja_progress(line)) {
        ctx.sink.on_progress({StageId::Build, *f * 0.99f, std::string(line.substr(0, line.find(']') + 1))});
      } else {
        ctx.sink.on_log(line);
      }
    };
    ctx.sink.on_progress({StageId::Build, 0.0f, "compiling with " + std::to_string(jobs) + " jobs"});
    LR_TRY(run_tool(build));

    // 4. Install the binary into <root>/bin atomically.
    const fs::path built = p.build / kGameBinary;
    if (!fs::exists(built, ec))
      return Error{ErrorCode::ToolFailed, "the game build did not produce " + built.string(), "see " + log.string()};
    fs::create_directories(p.bin, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create " + p.bin.string() + ": " + ec.message(), ""};
    const fs::path dst = p.bin / kGameBinary;
    const fs::path tmp = p.bin / (std::string(kGameBinary) + ".tmp");
    fs::copy_file(built, tmp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::permissions(tmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                      fs::perms::others_read | fs::perms::others_exec, ec);
    if (!ec) fs::rename(tmp, dst, ec);
    if (ec) {
      std::error_code ignored;
      fs::remove(tmp, ignored);
      return Error{ErrorCode::IoError, "cannot install " + dst.string() + ": " + ec.message(), ""};
    }
    return {};
  }

};

}  // namespace

std::unique_ptr<Stage> make_build_stage() { return std::make_unique<BuildStage>(); }

}  // namespace lr::pipeline
