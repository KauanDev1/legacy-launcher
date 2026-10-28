// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "launcher/cli/args.h"
#include "support/temp_dir.h"
#include "support/xdvdfs_builder.h"

extern char** environ;

using namespace lr;
using namespace lr::cli;
using lr::pipeline::StageId;

namespace {
Result<CliArgs> parse(std::vector<std::string_view> v) { return parse_args(v); }
}  // namespace

TEST_CASE("install parses every flag and keeps the path intact", "[unit]") {
  auto r = parse({"install", "--iso", "a b (c).iso", "--force", "recompile", "--jobs", "4"});
  REQUIRE(r);
  CHECK(r.value().command == "install");
  REQUIRE(r.value().iso);
  CHECK(r.value().iso->string() == "a b (c).iso");
  CHECK(r.value().force == std::set<StageId>{StageId::Recompile});
  CHECK(r.value().jobs == 4);
  CHECK_FALSE(r.value().root);
}

TEST_CASE("install without --iso is a usage error", "[unit]") {
  auto r = parse({"install"});
  REQUIRE_FALSE(r);
  CHECK(r.error().message.find("--iso") != std::string::npos);
}

TEST_CASE("unknown --force stage lists the valid names", "[unit]") {
  auto r = parse({"install", "--iso", "x", "--force", "bogus"});
  REQUIRE_FALSE(r);
  CHECK(r.error().code == ErrorCode::InvalidFormat);
  CHECK((r.error().message + r.error().hint).find("extract") != std::string::npos);
}

TEST_CASE("force accepts a comma list and repeats", "[unit]") {
  auto r = parse({"install", "--iso", "x", "--force", "patch,analyse", "--force", "build"});
  REQUIRE(r);
  CHECK(r.value().force == std::set<StageId>{StageId::Patch, StageId::Analyse, StageId::Build});
}

TEST_CASE("run forwards everything after -- to the game", "[unit]") {
  auto r = parse({"run", "--", "--headless"});
  REQUIRE(r);
  CHECK(r.value().command == "run");
  CHECK(r.value().passthrough == std::vector<std::string>{"--headless"});
}

TEST_CASE("build does not need --iso and forces recompile", "[unit]") {
  auto r = parse({"build"});
  REQUIRE(r);
  CHECK(r.value().command == "build");
  CHECK_FALSE(r.value().iso);
  CHECK(r.value().force.count(StageId::Recompile) == 1);
}

TEST_CASE("doctor and inspect parse; unknown command, flags and bad jobs fail", "[unit]") {
  CHECK(parse({"doctor", "--root", "/tmp/r"}));
  auto ins = parse({"inspect", "--iso", "g.iso"});
  REQUIRE(ins);
  CHECK(ins.value().passthrough == std::vector<std::string>{"--iso", "g.iso"});
  CHECK_FALSE(parse({"frobnicate"}));
  CHECK_FALSE(parse({"install", "--iso", "x", "--nope"}));
  CHECK_FALSE(parse({"install", "--iso", "x", "--jobs", "abc"}));
  CHECK_FALSE(parse({"install", "--iso"}));
  CHECK_FALSE(parse({}));
}

TEST_CASE("build --force extract or doctor without --iso is a usage error", "[unit]") {
  CHECK_FALSE(parse({"build", "--force", "extract"}));
  CHECK_FALSE(parse({"build", "--force", "doctor"}));
  CHECK(parse({"build", "--force", "extract", "--iso", "g.iso"}));
  CHECK(parse({"build", "--force", "patch"}));
}

TEST_CASE("inspect --iso terminates on a directory cycle", "[unit]") {
  // Root holds one directory, Content, whose entry points back at the root directory: Content/Content/... forever.
  auto img = testing::XdvdfsBuilder().add("Content/pkg", {std::byte{1}}).build();
  const size_t entry = static_cast<size_t>(img.root_sector) * testing::kSector;  // root's only entry: Content
  for (int i = 0; i < 4; ++i) img.data[entry + 4 + i] = std::byte((img.root_sector >> (8 * i)) & 0xFF);

  lr::testing::TempDir tmp{"lr_inspect_cycle"};
  const auto iso = tmp.path / "cycle.iso";  // XDVDFS at partition offset 0
  std::ofstream(iso, std::ios::binary)
      .write(reinterpret_cast<const char*>(img.data.data()), static_cast<std::streamsize>(img.data.size()));

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  std::vector<std::string> store = {LR_CLI_PATH, "inspect", "--iso", iso.string()};
  std::vector<char*> argv;
  for (auto& a : store) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = -1;
  REQUIRE(posix_spawn(&pid, LR_CLI_PATH, &fa, nullptr, argv.data(), environ) == 0);
  posix_spawn_file_actions_destroy(&fa);

  int st = 0;
  bool done = false;
  for (int i = 0; i < 600 && !done; ++i) {  // 60 s
    done = ::waitpid(pid, &st, WNOHANG) == pid;
    if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!done) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &st, 0);
  }
  REQUIRE(done);
  CHECK(WIFEXITED(st));
  if (WIFEXITED(st)) CHECK(WEXITSTATUS(st) == 0);
}

#include "support/xex2_builder.h"

TEST_CASE("inspect --imports lists every import with its kind", "[unit]") {
  // xboxkrnl.exe: DbgPrint (0x003) as function (type-0 + type-1 records), XexExecutableModuleHandle (0x193) as
  // variable; xam.xex: one unknown-ordinal variable.
  std::vector<uint8_t> text(0x100, 0), data(0x100, 0);
  lr::testing::put_be32(data, 0x0, 0x003);
  lr::testing::put_be32(text, 0x10, 0x01000003);
  lr::testing::put_be32(data, 0x4, 0x193);
  lr::testing::put_be32(data, 0x8, 0xFFFE);
  const auto xex = lr::testing::Xex2Builder()
                       .entry_point(0x82001000)
                       .image_base(0x82000000)
                       .import_library("xboxkrnl.exe", {0x82002000, 0x82001010, 0x82002004})
                       .import_library("xam.xex", {0x82002008})
                       .pe_image(0x82000000, 0x3000, {{".text", 0x1000, text, true}, {".data", 0x2000, data, false}})
                       .build();
  lr::testing::TempDir tmp{"lr_inspect_imports"};
  const auto path = tmp.path / "default.xex";
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char*>(xex.data()), static_cast<std::streamsize>(xex.size()));
  const auto log = tmp.path / "out.log";

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  std::vector<std::string> store = {LR_CLI_PATH, "inspect", "--imports", path.string()};
  std::vector<char*> argv;
  for (auto& a : store) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = -1;
  REQUIRE(posix_spawn(&pid, LR_CLI_PATH, &fa, nullptr, argv.data(), environ) == 0);
  posix_spawn_file_actions_destroy(&fa);
  int st = 0;
  REQUIRE(::waitpid(pid, &st, 0) == pid);
  std::ifstream in(log);
  const std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  INFO(out);
  REQUIRE(WIFEXITED(st));
  CHECK(WEXITSTATUS(st) == 0);
  CHECK(out == "xboxkrnl.exe 0x0003 DbgPrint function\n"
               "xboxkrnl.exe 0x0193 XexExecutableModuleHandle variable\n"
               "xam.xex 0xFFFE ? variable\n");
}
