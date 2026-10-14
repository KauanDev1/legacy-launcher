// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/result.h"
#include "common/sha256.h"
#include "formats/reader.h"
#include "formats/stfs.h"
#include "formats/xdvdfs.h"
#include "formats/xex2.h"

namespace {

using lr::Result;
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

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::strcmp(argv[1], "inspect") == 0 && std::strcmp(argv[2], "--iso") == 0) {
    auto r = inspect(argv[3]);
    if (!r) {
      std::fprintf(stderr, "error: %s\n", r.error().message.c_str());
      std::fprintf(stderr, "hint: %s\n", r.error().hint.c_str());
      return 1;
    }
    return 0;
  }
  std::fprintf(stderr, "usage: legacyrecomp inspect --iso <path>\n");
  return 2;
}
