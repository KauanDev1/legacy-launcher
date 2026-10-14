// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline/doctor.h"

#include <unistd.h>

#include <regex>
#include <system_error>

#include "pipeline/process.h"

namespace lr::pipeline {
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kGiB = 1024ull * 1024 * 1024;
constexpr const char* kInstallHint =
    "sudo pacman -S --needed clang lld llvm cmake ninja sdl2\n"
    "sudo apt install clang lld llvm cmake ninja-build libsdl2-dev pkg-config\n"
    "sudo dnf install clang lld llvm cmake ninja-build SDL2-devel pkgconf";

Error missing(std::string_view what) {
  return Error{ErrorCode::ToolMissing, std::string(what) + " not found", kInstallHint};
}

bool parse_version(const std::string& text, int& major, int& minor) {
  static const std::regex re(R"((\d+)\.(\d+)(?:\.(\d+))?)");
  std::smatch m;
  if (!std::regex_search(text, m, re)) return false;
  major = std::stoi(m[1]);
  minor = std::stoi(m[2]);
  return true;
}

Result<void> check_tool(DoctorProbe& probe, std::string_view name, int want_major, int want_minor) {
  auto path = probe.find(name);
  if (!path) return missing(name);
  if (want_major == 0) return {};
  auto out = probe.version_output(*path);
  if (!out) return Error(out.error());
  int major = 0, minor = 0;
  std::string want = std::to_string(want_major) + "." + std::to_string(want_minor);
  if (!parse_version(out.value(), major, minor))
    return Error{ErrorCode::ToolMissing,
                 std::string("cannot determine the version of ") + std::string(name) + " (need >= " + want + ")",
                 kInstallHint};
  if (major < want_major || (major == want_major && minor < want_minor))
    return Error{ErrorCode::ToolMissing,
                 std::string(name) + " " + std::to_string(major) + "." + std::to_string(minor) +
                     " is too old (need >= " + want + ")",
                 kInstallHint};
  return {};
}

}  // namespace

std::optional<fs::path> SystemDoctorProbe::find(std::string_view program) { return find_program(program); }

Result<std::string> SystemDoctorProbe::version_output(const fs::path& program) {
  return capture_output(program, {"--version"});
}

Result<uint64_t> SystemDoctorProbe::free_bytes(const fs::path& path) {
  std::error_code ec;
  auto info = fs::space(path, ec);
  if (ec) return Error{ErrorCode::IoError, "cannot query free space of " + path.string() + ": " + ec.message(), ""};
  return static_cast<uint64_t>(info.available);
}

uint64_t SystemDoctorProbe::total_ram() {
  long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
  if (pages < 0 || size < 0) return 0;
  return static_cast<uint64_t>(pages) * static_cast<uint64_t>(size);
}

bool SystemDoctorProbe::pkg_config_exists(std::string_view package) {
  auto pc = find_program("pkg-config");
  if (!pc) return false;
  return capture_output(*pc, {"--exists", std::string(package)}).ok();
}

Result<DoctorReport> run_doctor(DoctorProbe& probe, const fs::path& root) {
  LR_TRY(check_tool(probe, "clang++", 18, 0));
  LR_TRY(check_tool(probe, "cmake", 3, 25));
  LR_TRY(check_tool(probe, "ninja", 0, 0));
  if (!probe.pkg_config_exists("sdl2")) return missing("SDL2 development files (pkg-config sdl2)");

  fs::path probe_path = fs::absolute(root);
  std::error_code ec;
  while (!probe_path.empty() && !fs::exists(probe_path, ec) && probe_path != probe_path.parent_path())
    probe_path = probe_path.parent_path();
  LR_TRY_ASSIGN(free, probe.free_bytes(probe_path));
  if (free < 8 * kGiB)
    return Error{ErrorCode::InsufficientResources,
                 "only " + std::to_string(free / (1024 * 1024)) + " MiB free at " + probe_path.string() +
                     " (need 8 GiB)",
                 "free disk space or choose another data root with --root"};

  DoctorReport report;
  uint64_t ram = probe.total_ram();
  if (ram < 8 * kGiB)
    report.warnings.push_back("only " + std::to_string(ram / (1024 * 1024)) +
                              " MiB of RAM detected; recompilation may be slow or fail (8 GiB recommended)");
  return report;
}

}  // namespace lr::pipeline
