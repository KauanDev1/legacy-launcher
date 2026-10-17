// SPDX-License-Identifier: MIT
// Exercises src/runtime/main.cpp end to end through `lr_fake_game`: main.cpp + a tiny hand-written "generated"
// set (tests/support/fake_generated) + import thunks rendered by render_import_thunks. The guest image is a
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

#include "pipeline/pipeline.h"
#include "support/elf_builder.h"
#include "support/temp_dir.h"
#include "support/xex2_builder.h"

extern char** environ;

#ifdef LR_FAKE_GAME_PATH
namespace {
namespace fs = std::filesystem;

struct RunResult {
  int code;
  std::string out;  // stdout + stderr
};

// One private directory per test case so `ctest -j` cannot race on shared file names.
struct TempDir : lr::testing::TempDir {
  TempDir() : lr::testing::TempDir("lr_fake_game") {}
};

RunResult run_game(const fs::path& dir, const std::vector<std::string>& args) {
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

std::string write_image(const fs::path& dir, uint32_t entry) {
  const fs::path p = dir / "fake.elf";
  auto elf = lr::test::build_elf(entry, {{".rdata", 0x82000000, std::vector<uint8_t>(16, 0), false},
                                         {".text", 0x82000100, std::vector<uint8_t>(0x200, 0), true}});
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(elf.data()), std::streamsize(elf.size()));
  return p.string();
}
}  // namespace

TEST_CASE("fake game --coverage lists three states", "[unit]") {
  TempDir td;
  auto r = run_game(td.path, {"--coverage"});
  CHECK(r.code == 0);
  CHECK(r.out.find("| Import | Kind | Status |") != std::string::npos);
  CHECK(r.out.find("| FakeImplemented | function | implemented |") != std::string::npos);
  CHECK(r.out.find("| FakeStub | function | stub |") != std::string::npos);
  CHECK(r.out.find("| FakeCallback | function | implemented |") != std::string::npos);
  CHECK(r.out.find("| FakeMissing | function | missing |") != std::string::npos);
  CHECK(r.out.find("Implemented: 4, stub: 1, missing: 1 / 6 functions + 0 variables") != std::string::npos);
}

TEST_CASE("fake game --coverage --image adds the XEX's variable imports", "[unit]") {
  TempDir td;
  // xboxkrnl.exe: DbgPrint (0x003) as function (type-0 + type-1 records), XexExecutableModuleHandle (0x193,
  // registered), XboxKrnlVersion4Digit (0x345, not registered) and KeTimeStampBundle (0x0AD, registered) as
  // variables.
  std::vector<uint8_t> text(0x100, 0), data(0x100, 0);
  lr::testing::put_be32(data, 0x0, 0x003);
  lr::testing::put_be32(text, 0x10, 0x01000003);
  lr::testing::put_be32(data, 0x4, 0x193);
  lr::testing::put_be32(data, 0x8, 0x345);
  lr::testing::put_be32(data, 0xC, 0x0AD);
  const auto xex = lr::testing::Xex2Builder()
                       .entry_point(0x82001000)
                       .image_base(0x82000000)
                       .import_library("xboxkrnl.exe", {0x82002000, 0x82001010, 0x82002004, 0x82002008, 0x8200200C})
                       .pe_image(0x82000000, 0x3000, {{".text", 0x1000, text, true}, {".data", 0x2000, data, false}})
                       .build();
  const fs::path p = td.path / "default.xex";
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(xex.data()), std::streamsize(xex.size()));
  auto r = run_game(td.path, {"--coverage", "--image", p.string()});
  INFO(r.out);
  CHECK(r.code == 0);
  CHECK(r.out.find("| XexExecutableModuleHandle | variable | implemented |") != std::string::npos);
  CHECK(r.out.find("| XboxKrnlVersion4Digit | variable | missing |") != std::string::npos);
  CHECK(r.out.find("DbgPrint") == std::string::npos);  // a function: its row comes from the generated thunks
  CHECK(r.out.find("| KeTimeStampBundle | variable | implemented |") != std::string::npos);
  CHECK(r.out.find("Implemented: 6, stub: 1, missing: 2 / 6 functions + 3 variables") != std::string::npos);
}

// The XEX TLS header (key 0x00020104) is validated at load: an unusable one is a load error (exit 2), never a
// corrupted TLS block. Entry sub_82000180 returns normally (exit 0) when the image loads.
TEST_CASE("fake game rejects an unusable XEX TLS header with exit 2", "[unit][threads]") {
  TempDir td;
  auto run_with_tls = [&](uint32_t slots, uint32_t raw_addr, uint32_t data_size, uint32_t raw_size) {
    std::vector<uint8_t> text(0x100, 0), data(0x100, 0);
    const auto xex = lr::testing::Xex2Builder()
                         .entry_point(0x82000180)
                         .image_base(0x82000000)
                         .tls(slots, raw_addr, data_size, raw_size)
                         .pe_image(0x82000000, 0x3000, {{".text", 0x1000, text, true}, {".data", 0x2000, data, false}})
                         .build();
    const fs::path p = td.path / "tls.xex";
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(xex.data()), std::streamsize(xex.size()));
    return run_game(td.path, {"--headless", "--image", p.string()});
  };
  auto ok = run_with_tls(0, 0x82002000, 0x20, 0x10);
  INFO(ok.out);
  CHECK(ok.code == 0);
  auto slots = run_with_tls(100000, 0x82002000, 0x20, 0x10);
  CHECK(slots.code == 2);
  CHECK(slots.out.find("XEX TLS header: slot count 100000 exceeds 4096") != std::string::npos);
  auto outside = run_with_tls(0, 0x90000000, 0x20, 0x10);
  CHECK(outside.code == 2);
  CHECK(outside.out.find("raw data lies outside the loaded image") != std::string::npos);
  auto huge = run_with_tls(0, 0x82002000, 0xFFFFFFFF, 0x10);
  CHECK(huge.code == 2);
  CHECK(huge.out.find("data size 4294967295 exceeds") != std::string::npos);
}

// sub_82000280 calls FakeCallback(sub_82000200, 41); the callback checks the frame call_guest built and returns
// 42, which sub_82000280 checks (a mismatch calls FakeMissing, exit 3).
TEST_CASE("call_guest runs a recompiled callback", "[unit]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x82000280)});
  CHECK(r.code == 0);
  CHECK(r.out.empty());
}

// sub_820002A0 calls FakeCallback(sub_82000220), which enables flush mode and does inexact/denormal float math:
// no SIGFPE (exit 4), and the caller's MXCSR is restored after call_guest (a mismatch calls FakeMissing, exit 3).
TEST_CASE("call_guest gives the callback the host FP environment and restores the caller's", "[unit]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x820002A0)});
  CHECK(r.code == 0);
  CHECK(r.out.empty());
}

TEST_CASE("call_guest to an address without function is guest fatal", "[unit]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x820002C0)});
  CHECK(r.code == 5);
  CHECK(r.out.find("Guest fatal: call_guest: no recompiled function at guest 0x82000240") != std::string::npos);
  CHECK(r.out.find("Trail (last 1): FakeCallback") != std::string::npos);

  // Outside [PPC_CODE_BASE, PPC_CODE_BASE + PPC_CODE_SIZE): rejected before the function table is read.
  auto o = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x820002E0)});
  CHECK(o.code == 5);
  CHECK(o.out.find("no recompiled function at guest 0x12345678") != std::string::npos);
}

TEST_CASE("fake game stops at the first unimplemented import with exit 3 and a report", "[unit]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x82000100)});
  CHECK(r.code == 3);
  CHECK(r.out.find("Unimplemented import: FakeMissing") != std::string::npos);
  CHECK(r.out.find("Called from: 0x8200010C") != std::string::npos);
}

TEST_CASE("fake game trail contains every import called before the missing one", "[unit]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x82000100)});
  CHECK(r.code == 3);
  // Every thunk traces, stubs and implemented imports alike; the missing import is the last entry.
  CHECK(r.out.find("Trail (last 3): FakeImplemented -> FakeStub -> FakeMissing") != std::string::npos);
}

// sub_82000180 checks the values the thunks wrote back to r3 and calls FakeMissing (exit 3) on a mismatch.
TEST_CASE("fake game exits 0 when the entry point returns normally", "[unit]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x82000180)});
  CHECK(r.code == 0);
}

// sub_820001E0 runs sub_82000170(41) on a second guest thread through ExCreateThread, waits on its handle and
// checks the value the thread stored (a mismatch calls FakeMissing, exit 3).
TEST_CASE("fake game runs a guest thread created by ExCreateThread", "[unit][threads]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x820001E0)});
  INFO(r.out);
  CHECK(r.code == 0);
  CHECK(r.out.empty());
}

// sub_820001C0 starts sub_82000160 (calls FakeMissing) on thread 2 and blocks waiting for it: the whole process
// exits 3 from that thread, and the report names it (spec section 11).
TEST_CASE("missing import on a secondary guest thread exits 3 with a Thread line", "[unit][threads]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x820001C0)});
  INFO(r.out);
  CHECK(r.code == 3);
  CHECK(r.out.find("Unimplemented import: FakeMissing\nCalled from: 0x82000164\nThread: 2\n") != std::string::npos);
  CHECK(r.out.find("Trail (last 1): FakeMissing") != std::string::npos);
}

TEST_CASE("fake game usage and load errors exit 2", "[unit]") {
  TempDir td;
  CHECK(run_game(td.path, {"--headless"}).code == 2);
  CHECK(run_game(td.path, {"--bogus"}).code == 2);
  CHECK(run_game(td.path, {"--headless", "--image", "/nonexistent/x.xex"}).code == 2);
  const fs::path junk = td.path / "junk.bin";
  std::ofstream(junk, std::ios::binary) << std::string(64, 'x');
  CHECK(run_game(td.path, {"--headless", "--image", junk.string()}).code == 2);
  CHECK(run_game(td.path, {"--headless", "--image", write_image(td.path, 0x82000104)}).code == 2);  // no function at entry
}

TEST_CASE("launch runs <root>/bin/legacyrecomp-game on the patched image and returns its exit code", "[unit]") {
  TempDir td;
  fs::create_directories(td.path / "bin");
  fs::create_directories(td.path / "work" / "image");
  fs::copy_file(LR_FAKE_GAME_PATH, td.path / "bin" / "legacyrecomp-game");
  fs::copy_file(write_image(td.path, 0x82000100), td.path / "work" / "image" / "default_tu1.xex");
  lr::pipeline::Options opts;
  opts.root = td.path;
  const std::vector<std::string> extra = {"--headless"};
  auto r = lr::pipeline::launch(opts, extra);
  REQUIRE(r);
  CHECK(r.value() == 3);  // FakeMissing is unimplemented

  const std::vector<std::string> bogus = {"--bogus"};
  auto u = lr::pipeline::launch(opts, bogus);  // extra args reach the game
  REQUIRE(u);
  CHECK(u.value() == 2);
}
#else
TEST_CASE("fake game tests need SDL2 (target lr_fake_game not built)", "[unit]") { SKIP("SDL2 not found"); }
#endif
