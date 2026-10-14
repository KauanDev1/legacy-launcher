// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises src/runtime/main.cpp end to end through `lr_fake_game`: main.cpp + a tiny hand-written "generated"
// set (tests/support/fake_generated) + import stubs rendered by render_import_stubs. The guest image is a
// synthetic ELF built in memory (XenonUtils parses it like an XEX).
#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "support/elf_builder.h"

extern char** environ;

#ifdef LR_FAKE_GAME_PATH
namespace {
namespace fs = std::filesystem;

struct RunResult {
  int code;
  std::string out;  // stdout + stderr
};

RunResult run_game(const std::vector<std::string>& args) {
  const fs::path dir = fs::temp_directory_path() / "lr_fake_game_test";
  fs::create_directories(dir);
  const fs::path log = dir / "out.log";
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  std::vector<std::string> store = {LR_FAKE_GAME_PATH};
  store.insert(store.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& s : store) argv.push_back(s.data());
  argv.push_back(nullptr);
  pid_t pid;
  REQUIRE(posix_spawn(&pid, LR_FAKE_GAME_PATH, &fa, nullptr, argv.data(), environ) == 0);
  posix_spawn_file_actions_destroy(&fa);
  int st = 0;
  REQUIRE(waitpid(pid, &st, 0) == pid);
  std::ifstream f(log);
  std::stringstream ss;
  ss << f.rdbuf();
  return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, ss.str()};
}

std::string write_image(uint32_t entry) {
  const fs::path p = fs::temp_directory_path() / "lr_fake_game_test" / "fake.elf";
  fs::create_directories(p.parent_path());
  auto elf = lr::test::build_elf(entry, {{".rdata", 0x82000000, std::vector<uint8_t>(16, 0), false},
                                         {".text", 0x82000100, std::vector<uint8_t>(0x100, 0), true}});
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(elf.data()), std::streamsize(elf.size()));
  return p.string();
}
}  // namespace

TEST_CASE("fake game --coverage prints the import table and exits 0", "[unit]") {
  auto r = run_game({"--coverage"});
  CHECK(r.code == 0);
  CHECK(r.out.find("| Import | Status |") != std::string::npos);
  CHECK(r.out.find("| FakeImplemented | implemented |") != std::string::npos);
  CHECK(r.out.find("| FakeMissing | missing |") != std::string::npos);
  CHECK(r.out.find("Implemented: 1 / 2") != std::string::npos);
}

TEST_CASE("fake game stops at the first unimplemented import with exit 3 and a report", "[unit]") {
  auto r = run_game({"--headless", "--image", write_image(0x82000100)});
  CHECK(r.code == 3);
  CHECK(r.out.find("Unimplemented import: FakeMissing") != std::string::npos);
  CHECK(r.out.find("Called from: 0x82000108") != std::string::npos);
  CHECK(r.out.find("FakeImplemented") != std::string::npos);  // trail
}

TEST_CASE("fake game exits 0 when the entry point returns normally", "[unit]") {
  auto r = run_game({"--headless", "--image", write_image(0x82000180)});
  CHECK(r.code == 0);
}

TEST_CASE("fake game usage and load errors exit 2", "[unit]") {
  CHECK(run_game({"--headless"}).code == 2);
  CHECK(run_game({"--bogus"}).code == 2);
  CHECK(run_game({"--headless", "--image", "/nonexistent/x.xex"}).code == 2);
  const fs::path junk = fs::temp_directory_path() / "lr_fake_game_test" / "junk.bin";
  fs::create_directories(junk.parent_path());
  std::ofstream(junk, std::ios::binary) << std::string(64, 'x');
  CHECK(run_game({"--headless", "--image", junk.string()}).code == 2);
  CHECK(run_game({"--headless", "--image", write_image(0x82000104)}).code == 2);  // no function at entry
}
#else
TEST_CASE("fake game tests need SDL2 (target lr_fake_game not built)", "[unit]") { SKIP("SDL2 not found"); }
#endif
