// SPDX-License-Identifier: MIT
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/paths.h"
#include "common/result.h"
#include "common/sha256.h"
#include "formats/reader.h"
#include "formats/stfs.h"
#include "formats/xdvdfs.h"
#include "formats/xex2.h"

#include "launcher/cli/args.h"
#include "launcher/cli/progress_bar.h"
#include "pipeline/doctor.h"
#include "pipeline/pipeline.h"

#include <image.h>

namespace {

using lr::Result;
namespace fs = std::filesystem;
using namespace lr::formats;

constexpr int kMaxTreeDepth = 4;

std::string utf8_from_u16(const std::u16string& s) {
  std::string out;
  for (char16_t c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  return out;
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  s.remove_prefix(s.size() - suffix.size());
  for (size_t i = 0; i < s.size(); ++i) {
    char a = s[i], b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

bool is_xex_name(std::string_view n) { return ends_with_ci(n, ".xex") || ends_with_ci(n, ".xexp"); }

std::string join(const std::string& dir, const std::string& name) {
  return dir.empty() ? name : dir + "/" + name;
}

Result<void> print_xex(IReader& file, const std::string& label, const char* indent) {
  LR_TRY_ASSIGN(bytes, read_all(file));
  lr::Sha256 h;
  h.update(bytes);
  std::printf("%s%s: size=%zu sha256=%s\n", indent, label.c_str(), bytes.size(),
              lr::to_hex(h.finish()).c_str());
  auto info = parse_xex2(bytes);
  if (!info) {
    std::printf("%s  xex2 parse failed: %s\n", indent, info.error().message.c_str());
    return {};
  }
  const auto& x = info.value();
  std::printf("%s  module_flags=0x%08X pe_data_offset=0x%X\n", indent, x.module_flags,
              x.pe_data_offset);
  if (x.entry_point) std::printf("%s  entry_point=0x%08X\n", indent, *x.entry_point);
  if (x.image_base) std::printf("%s  image_base=0x%08X\n", indent, *x.image_base);
  if (x.execution) {
    const auto& e = *x.execution;
    std::printf("%s  execution: title_id=0x%08X media_id=0x%08X version=%s base_version=%s "
                "platform=%u type=%u disc=%u/%u\n",
                indent, e.title_id, e.media_id, lr::formats::format_version(e.version).c_str(),
                lr::formats::format_version(e.base_version).c_str(), e.platform, e.executable_type,
                e.disc_number, e.disc_count);
  }
  std::printf("%s  imports (%zu):", indent, x.import_libraries.size());
  for (const auto& l : x.import_libraries) std::printf(" %s", l.c_str());
  std::printf("\n");
  return {};
}

Result<void> print_tree(const XdvdfsImage& img, const std::string& dir, int depth) {
  LR_TRY_ASSIGN(entries, img.list(dir));
  for (const auto& e : entries) {
    std::printf("%*s%s%s", depth * 2, "", e.name.c_str(), e.is_dir ? "/" : "");
    if (!e.is_dir) std::printf("  (%u bytes)", e.size);
    std::printf("\n");
    if (e.is_dir && depth + 1 < kMaxTreeDepth) LR_TRY(print_tree(img, join(dir, e.name), depth + 1));
  }
  return {};
}

Result<void> inspect_package(const XdvdfsImage& img, const std::string& path) {
  LR_TRY_ASSIGN(file, img.open_file(path));
  auto pkg = StfsPackage::open(*file);
  if (!pkg) {
    std::printf("  [%s] not an STFS package: %s\n", path.c_str(), pkg.error().message.c_str());
    return {};
  }
  const auto& h = pkg.value().header();
  std::printf("\nSTFS package: %s\n", path.c_str());
  std::printf("  magic=%s content_type=0x%08X title_id=0x%08X read_only=%d display_name=\"%s\"\n",
              h.magic.c_str(), h.content_type, h.title_id, h.read_only ? 1 : 0,
              utf8_from_u16(h.display_name).c_str());
  std::printf("  entries (%zu):\n", pkg.value().entries().size());
  for (const auto& e : pkg.value().entries()) {
    std::printf("    %s%s  (%u bytes, start_block=%u, consecutive=%d)\n", e.path.c_str(),
                e.is_dir ? "/" : "", e.size, e.start_block, e.consecutive ? 1 : 0);
  }
  for (const auto& e : pkg.value().entries()) {
    if (e.is_dir || !is_xex_name(e.path)) continue;
    auto inner = pkg.value().open_file(e.path);
    if (!inner) {
      std::printf("  %s: open failed: %s\n", e.path.c_str(), inner.error().message.c_str());
      continue;
    }
    LR_TRY(print_xex(*inner.value(), e.path, "  "));
  }
  return {};
}

// Walks `dir` recursively; calls inspect_package on every file (STFS packages have no extension).
Result<void> walk_packages(const XdvdfsImage& img, const std::string& dir) {
  auto entries = img.list(dir);
  if (!entries) {
    std::printf("\n[%s] not present on disc\n", dir.c_str());
    return {};
  }
  for (const auto& e : entries.value()) {
    std::string p = join(dir, e.name);
    if (e.is_dir) {
      LR_TRY(walk_packages(img, p));
    } else {
      LR_TRY(inspect_package(img, p));
    }
  }
  return {};
}

Result<void> walk_xex(const XdvdfsImage& img, const std::string& dir) {
  LR_TRY_ASSIGN(entries, img.list(dir));
  for (const auto& e : entries) {
    std::string p = join(dir, e.name);
    if (e.is_dir) {
      LR_TRY(walk_xex(img, p));
    } else if (is_xex_name(e.name)) {
      LR_TRY_ASSIGN(f, img.open_file(p));
      LR_TRY(print_xex(*f, "disc:" + p, ""));
    }
  }
  return {};
}

Result<void> inspect(const std::string& iso_path) {
  LR_TRY_ASSIGN(reader, FileReader::open(iso_path));
  LR_TRY_ASSIGN(img, XdvdfsImage::open(*reader));
  std::printf("XDVDFS partition offset: 0x%llX\n\nTree:\n",
              static_cast<unsigned long long>(img.partition_offset()));
  LR_TRY(print_tree(img, "", 0));
  std::printf("\nDisc XEX files:\n");
  LR_TRY(walk_xex(img, ""));
  LR_TRY(walk_packages(img, "Content"));
  LR_TRY(walk_packages(img, "$TitleUpdate"));
  return {};
}

Result<std::vector<uint8_t>> load_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return lr::Error{lr::ErrorCode::IoError, "cannot open " + path, ""};
  auto size = f.tellg();
  if (size < 0) return lr::Error{lr::ErrorCode::IoError, "cannot stat " + path, ""};
  std::vector<uint8_t> data(static_cast<size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!f) return lr::Error{lr::ErrorCode::IoError, "cannot read " + path, ""};
  return data;
}

// Loads an XEX the way XenonRecomp/XenonAnalyse do (XenonUtils Image::ParseImage: decrypt,
// decompress, map PE sections, replace import thunks) and prints its layout. With `dump_path`,
// also writes the loaded image (byte 0 = base address) so byte patterns can be scanned.
Result<void> inspect_image(const std::string& xex_path, const char* dump_path) {
  LR_TRY_ASSIGN(bytes, load_bytes(xex_path));
  if (bytes.size() < 0x18 || bytes[0] != 'X' || bytes[1] != 'E' || bytes[2] != 'X' || bytes[3] != '2')
    return lr::Error{lr::ErrorCode::InvalidFormat, xex_path + " is not an XEX2 file", ""};
  Image image = Image::ParseImage(bytes.data(), bytes.size());
  if (!image.data || image.size == 0)
    return lr::Error{lr::ErrorCode::InvalidFormat, "XenonUtils could not load " + xex_path,
                     "encrypted/compressed variants other than retail AES + basic/LZX are unsupported"};
  std::printf("base=0x%08zX size=0x%08X entry=0x%08zX\n", image.base, image.size, image.entry_point);
  for (const auto& sec : image.sections) {
    std::printf("section name=%-8.8s base=0x%08zX size=0x%08X flags=%s\n", sec.name.c_str(), sec.base, sec.size,
                (sec.flags & SectionFlags_Code) ? "code" : "data");
  }
  for (const auto& sym : image.symbols)
    std::printf("import name=%s address=0x%08zX size=0x%zX\n", sym.name.c_str(), sym.address, sym.size);
  if (dump_path) {
    std::ofstream out(dump_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(image.data.get()), static_cast<std::streamsize>(image.size));
    out.close();
    if (!out) return lr::Error{lr::ErrorCode::IoError, std::string("cannot write ") + dump_path, ""};
    std::printf("dumped 0x%X bytes to %s\n", image.size, dump_path);
  }
  return {};
}

int report_error(const lr::Error& e) {
  std::fprintf(stderr, "error: %s\n", e.message.c_str());
  if (!e.hint.empty()) std::fprintf(stderr, "hint: %s\n", e.hint.c_str());
  return 1;
}

int run_inspect(const std::vector<std::string>& a) {
  Result<void> r = lr::Error{lr::ErrorCode::InvalidFormat, "bad inspect arguments", "run `legacyrecomp --help` for usage"};
  if (a.size() == 2 && a[0] == "--iso") r = inspect(a[1]);
  else if (a.size() == 2 && a[0] == "--sections") r = inspect_image(a[1], nullptr);
  else if (a.size() == 3 && a[0] == "--dump-image") r = inspect_image(a[1], a[2].c_str());
  else {
    report_error(r.error());
    return 2;
  }
  return r ? 0 : report_error(r.error());
}

int run_doctor_cmd(const fs::path& root) {
  lr::pipeline::SystemDoctorProbe probe;
  auto report = lr::pipeline::run_doctor(probe, root);
  if (!report) return report_error(report.error());
  for (const auto& w : report.value().warnings) std::printf("warning: %s\n", w.c_str());
  std::printf("doctor: all checks passed (data root: %s)\n", root.c_str());
  return 0;
}

int run_install(const lr::cli::CliArgs& args, const fs::path& root) {
  lr::pipeline::Options opts;
  if (args.iso) opts.iso = *args.iso;
  opts.root = root;
  opts.force = args.force;
  opts.jobs = args.jobs;

  // Tools run in their own process group and never see the terminal's Ctrl+C; the handler sets the flag that
  // ProgressBar::cancelled() reports, and the pipeline stops (killing the running tool) at its next check.
  lr::cli::ProgressBar::install_sigint_handler();
  lr::cli::ProgressBar bar(std::cout, ::isatty(STDOUT_FILENO) != 0);
  auto r = lr::pipeline::run(opts, bar);
  bar.finish();
  if (!r) {
    if (r.error().code == lr::ErrorCode::Cancelled) {
      std::fprintf(stderr, "cancelled; finished stages stay cached, run the command again to continue\n");
      return 130;
    }
    return report_error(r.error());
  }
  std::printf("done. Start the game with: legacyrecomp run\n");
  return 0;
}

int run_game(const lr::cli::CliArgs& args, const fs::path& root) {
  lr::pipeline::Options opts;
  opts.root = root;
  auto r = lr::pipeline::launch(opts, args.passthrough);
  if (!r) return report_error(r.error());
  if (r.value() == 3)
    std::fprintf(stderr, "M1: game stopped at the first unimplemented kernel import (expected at this milestone).\n");
  return r.value();
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args(argv + 1, argv + argc);
  for (auto a : args) {
    if (a == "--") break;
    if (a == "--help" || a == "-h") {
      std::fputs(std::string(lr::cli::usage_text()).c_str(), stdout);
      return 0;
    }
  }
  if (args.empty()) {
    std::fputs(std::string(lr::cli::usage_text()).c_str(), stderr);
    return 2;
  }
  auto parsed = lr::cli::parse_args(args);
  if (!parsed) {
    report_error(parsed.error());
    return 2;
  }
  const lr::cli::CliArgs& a = parsed.value();
  if (a.command == "inspect") return run_inspect(a.passthrough);

  const fs::path root =
      lr::resolve_data_root(a.root, std::getenv("LEGACYRECOMP_ROOT"), std::getenv("XDG_DATA_HOME"), std::getenv("HOME"));
  if (a.command == "doctor") return run_doctor_cmd(root);
  if (a.command == "run") return run_game(a, root);
  return run_install(a, root);  // install, build
}
