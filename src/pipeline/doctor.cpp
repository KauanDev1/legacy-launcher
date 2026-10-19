// SPDX-License-Identifier: MIT
#include "pipeline/doctor.h"

#include <unistd.h>

#if defined(__x86_64__)
#include <cpuid.h>
#endif

#include <charconv>
#include <regex>
#include <system_error>
#include <utility>
#include <vector>

#include "pipeline/process.h"
#include "pipeline/toolchain.h"

namespace lr::pipeline {
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kGiB = 1024ull * 1024 * 1024;
const std::string kInstallHint(system_toolchain_hint());

Error missing(std::string_view what) {
  return Error{ErrorCode::ToolMissing, std::string(what) + " not found", kInstallHint};
}

bool parse_version(const std::string& text, int& major, int& minor) {
  static const std::regex re(R"((\d+)\.(\d+)(?:\.(\d+))?)");
  std::smatch m;
  if (!std::regex_search(text, m, re)) return false;
  auto to_int = [](const std::ssub_match& sm, int& out) {
    const std::string str = sm.str();
    auto res = std::from_chars(str.data(), str.data() + str.size(), out);
    return res.ec == std::errc() && res.ptr == str.data() + str.size();
  };
  return to_int(m[1], major) && to_int(m[2], minor);
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

bool SystemDoctorProbe::cpu_supports_avx() {
#if defined(__x86_64__)
  // CPUID + XGETBV instead of __builtin_cpu_supports: that builtin needs compiler-rt's __cpu_model, which zig's
  // runtime (the AppImage toolchain) does not provide. AVX is usable when the CPU has it, the OS enabled XSAVE and
  // saves the XMM and YMM state (XCR0 bits 1 and 2).
  unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
  if (!(ecx & bit_AVX) || !(ecx & bit_OSXSAVE)) return false;
  unsigned xcr0_lo = 0, xcr0_hi = 0;
  __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
  return (xcr0_lo & 0x6u) == 0x6u;
#else
  return true;  // the template only adds -march=sandybridge on x86_64
#endif
}

Result<DoctorReport> run_doctor(DoctorProbe& probe, const fs::path& root, const fs::path& resources) {
  std::vector<std::string> notes;  // non-fatal findings, reported as warnings
  // Checked first: no toolchain install can fix it. InsufficientResources (a property of the machine, like RAM/disk).
  if (!probe.cpu_supports_avx())
    return Error{ErrorCode::InsufficientResources, "CPU without AVX support",
                 "the recompiled game is built for x86-64 with AVX (-march=sandybridge, as UnleashedRecomp does) and "
                 "would crash with SIGILL on this CPU; an Intel Sandy Bridge / AMD Bulldozer (2011) or newer CPU is "
                 "required"};
  if (has_bundled_toolchain(resources)) {
    // AppImage: the bundled zig/cmake/ninja/SDL2 are pinned versions; only check that they are all there.
    LR_TRY(resolve_toolchain(resources));
    // DXC is mandatory only when the XenosRecomp host tool is staged (it links libdxcompiler.so). No AppImage ships
    // either yet (ADR 0005, Task 24), so otherwise this is a note, not a failure.
    std::error_code tool_ec;
    if (fs::exists(resources / "tools" / "XenosRecomp", tool_ec))
      LR_TRY(check_bundled_dxc(resources));
    else
      notes.push_back("DXC (shader compiler) is not bundled yet; the shader translator is unavailable in this build");
  } else {
    LR_TRY(check_tool(probe, "clang++", 18, 0));
    LR_TRY(check_tool(probe, "cmake", 3, 25));
    LR_TRY(check_tool(probe, "ninja", 0, 0));
    if (!probe.pkg_config_exists("sdl2")) return missing("SDL2 development files (pkg-config sdl2)");
    // Only the `dxc` executable is looked up on PATH; its libdxcompiler.so (the library XenosRecomp links) is not
    // checked separately.
    if (!probe.find("dxc"))
      return Error{ErrorCode::ToolMissing, "dxc (DirectX Shader Compiler) not found", std::string(system_dxc_hint())};
  }

  std::error_code ec;
  fs::path probe_path = fs::absolute(root, ec);
  if (ec) return Error{ErrorCode::IoError, "cannot resolve " + root.string() + ": " + ec.message(), ""};
  while (!probe_path.empty() && !fs::exists(probe_path, ec) && probe_path != probe_path.parent_path())
    probe_path = probe_path.parent_path();
  LR_TRY_ASSIGN(free, probe.free_bytes(probe_path));
  if (free < 8 * kGiB)
    return Error{ErrorCode::InsufficientResources,
                 "only " + std::to_string(free / (1024 * 1024)) + " MiB free at " + probe_path.string() +
                     " (need 8 GiB)",
                 "free disk space or choose another data root with --root"};

  DoctorReport report;
  report.warnings = std::move(notes);
  uint64_t ram = probe.total_ram();
  if (ram < 8 * kGiB)
    report.warnings.push_back("only " + std::to_string(ram / (1024 * 1024)) +
                              " MiB of RAM detected; recompilation may be slow or fail (8 GiB recommended)");
  return report;
}

}  // namespace lr::pipeline
