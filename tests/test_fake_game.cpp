// SPDX-License-Identifier: MIT
// Exercises src/runtime/main.cpp end to end through `lr_fake_game`: main.cpp + a tiny hand-written "generated"
// set (tests/support/fake_generated) + import thunks rendered by render_import_thunks. The guest image is a
// synthetic ELF built in memory (XenonUtils parses it like an XEX).
#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pipeline/pipeline.h"
#include "support/elf_builder.h"
#include "support/temp_dir.h"
#include "support/xex2_builder.h"
#include "volk.h"

// stb_image (thirdparty/stb, pinned with stb_image_write; test-only): decodes the --screenshot-after PNG.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#include "stb_image.h"

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

// Starts the fake game with stdout + stderr going to dir/out.log. Without an explicit --gpu (and with
// default_null_gpu), it runs with --gpu null: only the Vulkan tests below need a Vulkan device, and they pass
// --gpu vulkan.
pid_t spawn_game(const fs::path& dir, const std::vector<std::string>& args, const std::vector<std::string>& extra_env,
                 bool default_null_gpu) {
  const fs::path log = dir / "out.log";
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  std::vector<std::string> store = {LR_FAKE_GAME_PATH};
  if (default_null_gpu && std::find(args.begin(), args.end(), "--gpu") == args.end()) {
    store.emplace_back("--gpu");
    store.emplace_back("null");
  }
  store.insert(store.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& s : store) argv.push_back(s.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  std::vector<std::string> env_store(extra_env);
  for (char** e = environ; *e != nullptr; ++e) env_store.emplace_back(*e);
  std::vector<char*> envp;
  for (auto& s : env_store) envp.push_back(s.data());
  envp.push_back(nullptr);
  REQUIRE(posix_spawn(&pid, LR_FAKE_GAME_PATH, &fa, nullptr, argv.data(), envp.data()) == 0);
  posix_spawn_file_actions_destroy(&fa);
  return pid;
}

std::string read_text(const fs::path& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Waits for the game started by spawn_game; exit code -1 when it died of a signal.
RunResult wait_game(const fs::path& dir, pid_t pid) {
  int st = 0;
  REQUIRE(waitpid(pid, &st, 0) == pid);
  return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, read_text(dir / "out.log")};
}

RunResult run_game(const fs::path& dir, const std::vector<std::string>& args,
                   const std::vector<std::string>& extra_env = {}, bool default_null_gpu = true) {
  return wait_game(dir, spawn_game(dir, args, extra_env, default_null_gpu));
}

// A Vulkan driver the loader can use (lavapipe in the dev container); the Vulkan tests SKIP without one.
bool has_icd() {
  if (volkInitialize() != VK_SUCCESS) return false;
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  VkInstance inst = VK_NULL_HANDLE;
  if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return false;
  volkLoadInstance(inst);
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(inst, &n, nullptr);
  vkDestroyInstance(inst, nullptr);
  return n > 0;
}

bool has_validation_layer() {
  if (volkInitialize() != VK_SUCCESS) return false;
  uint32_t n = 0;
  if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS) return false;
  std::vector<VkLayerProperties> layers(n);
  if (vkEnumerateInstanceLayerProperties(&n, layers.data()) != VK_SUCCESS) return false;
  return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
    return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
  });
}

// The game's output with LR_VULKAN_VALIDATION=1 carries no validation message ("[vulkan] ...", from the Device's
// debug messenger) and, when the layer is installed, no "not installed" warning either: the layer really ran.
void check_validation_clean(const std::string& out) {
  CHECK(out.find("[vulkan]") == std::string::npos);
  if (has_validation_layer()) CHECK(out.find("VK_LAYER_KHRONOS_validation is not installed") == std::string::npos);
}

// The fake GPU guest's exit line: its vertex and pixel shader, translated in memory.
constexpr const char* kSummaryTranslated =
    "[gpu] shaders: keys=2 vs=1 ps=1 translated=2 disk=0 failed=0 new_last_100_swaps=2\n";

std::string write_image(const fs::path& dir, uint32_t entry) {
  const fs::path p = dir / "fake.elf";
  auto elf = lr::test::build_elf(entry, {{".rdata", 0x82000000, std::vector<uint8_t>(16, 0), false},
                                         {".text", 0x82000100, std::vector<uint8_t>(0x400, 0), true}});
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
  // Spec section 5.3: functions and variables are counted separately.
  CHECK(r.out.find("Functions: implemented 20, stub 1, missing 1 / 22\n"
                   "Variables: implemented 0, stub 0, missing 0 / 0\n") != std::string::npos);
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
  CHECK(r.out.find("Functions: implemented 20, stub 1, missing 1 / 22\n"
                   "Variables: implemented 2, stub 0, missing 1 / 3\n") != std::string::npos);
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

// Importing KeTimeStampBundle starts the kernel worker inside resolve_variable_imports, and creating the worker freezes
// the TLS layout: main.cpp must set the image (TLS template) first, or the load fails with exit 2.
TEST_CASE("fake game with a TLS header and a KeTimeStampBundle import loads and runs", "[unit][threads][time]") {
  TempDir td;
  std::vector<uint8_t> text(0x100, 0), data(0x100, 0);
  lr::testing::put_be32(data, 0x0, 0x0AD);  // xboxkrnl KeTimeStampBundle (variable)
  const auto xex = lr::testing::Xex2Builder()
                       .entry_point(0x82000180)
                       .image_base(0x82000000)
                       .tls(0, 0x82002010, 0x20, 0x10)
                       .import_library("xboxkrnl.exe", {0x82002000})
                       .pe_image(0x82000000, 0x3000, {{".text", 0x1000, text, true}, {".data", 0x2000, data, false}})
                       .build();
  const fs::path p = td.path / "tls_bundle.xex";
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(xex.data()), std::streamsize(xex.size()));
  auto r = run_game(td.path, {"--headless", "--image", p.string()});
  INFO(r.out);
  CHECK(r.code == 0);
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

// sub_82000340 (thread 1) waits forever for thread 2 (sub_82000360), which waits forever for thread 1's KTHREAD:
// with LR_DEADLOCK_SECONDS=1 the detector reports both waits with their object kinds and trails, then exits 6.
TEST_CASE("two guest threads waiting on each other exit 6 with both waits listed", "[unit][threads][deadlock]") {
  TempDir td;
  const auto start = std::chrono::steady_clock::now();
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x82000340)}, {"LR_DEADLOCK_SECONDS=1"});
  INFO(r.out);
  CHECK(r.code == 6);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
  CHECK(r.out.find("Deadlock: every guest thread is blocked in an infinite wait") != std::string::npos);
  CHECK(r.out.find("Thread 1: waiting on 0x") != std::string::npos);
  CHECK(r.out.find("Thread 2: waiting on 0x") != std::string::npos);
  CHECK(r.out.find("(thread)") != std::string::npos);
  CHECK(r.out.find("Trail (last 2): ExCreateThread -> NtWaitForSingleObjectEx") != std::string::npos);
  CHECK(r.out.find("Trail (last 1): KeWaitForSingleObject") != std::string::npos);
}

// The M2 finish line in miniature, through the generated thunks only. sub_82000300 (thread 1) allocates a scratch
// block (NtAllocateVirtualMemory), initialises a critical section in it, creates a notification event and starts
// sub_82000380 on thread 2 (ExCreateThread). The worker opens game:\data.bin (NtCreateFile, 9 arguments: the
// ninth on the guest stack), reads its first 4 bytes into the scratch block (NtReadFile) and sets the event. Thread
// 1 turns the event handle into its dispatcher object (ObReferenceObjectByHandle), waits on it
// (KeWaitForSingleObject), then waits for the worker thread to end (NtWaitForSingleObjectEx: trace lines are written
// after a call returns, so this makes every worker line precede it) and calls VdInitializeEngines with the 4 bytes
// read in r3, big-endian (the trace line of that call carries the bytes; since M3 it returns 1), then FakeMissing
// from 0x82000328: exit 3. Any failed status on either thread calls FakeMissing from another call site instead. The ninth
// NtCreateFile argument is surrounded by FILE_DIRECTORY_FILE sentinels, so a thunk reading the wrong stack word fails.
TEST_CASE("fake guest boots like a game", "[unit][threads][files]") {
  TempDir td;
  fs::create_directories(td.path / "game");
  std::ofstream(td.path / "game" / "data.bin", std::ios::binary) << "LR27 and the rest of the file";
  const fs::path trace = td.path / "trace.log";
  auto r = run_game(td.path, {"--headless", "--data", (td.path / "game").string(), "--user", (td.path / "user").string(),
                              "--trace-imports", "--trace-file", trace.string(), "--image",
                              write_image(td.path, 0x82000300)});
  INFO(r.out);
  CHECK(r.code == 3);
  CHECK(r.out.find("Unimplemented import: FakeMissing\nCalled from: 0x82000328\nThread: 1\n") != std::string::npos);

  // Trace lines are "[tid] Name r3=... -> result"; the two threads interleave, so order is checked per thread.
  std::ifstream f(trace);
  std::vector<std::string> lines;
  for (std::string l; std::getline(f, l);) lines.push_back(l);
  INFO([&] {
    std::string all;
    for (const auto& l : lines) all += l + "\n";
    return all;
  }());
  auto tid_of = [](const std::string& l) { return l.substr(0, l.find(' ')); };
  auto name_of = [](const std::string& l) {
    const size_t b = l.find(' ') + 1;
    return l.substr(b, l.find(' ', b) - b);
  };
  std::vector<std::string> main_calls, worker_calls;
  std::string worker_tid;
  const size_t none = lines.size();
  size_t read_at = none, set_at = none, wait_at = 0, join_at = 0, vd_at = none;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string name = name_of(lines[i]);
    if (tid_of(lines[i]) == "[1]") {
      main_calls.push_back(name);
      if (name == "KeWaitForSingleObject") wait_at = i;
      if (name == "NtWaitForSingleObjectEx") join_at = i;
      if (name == "VdInitializeEngines") vd_at = i;
    } else {
      if (worker_tid.empty()) worker_tid = tid_of(lines[i]);
      CHECK(tid_of(lines[i]) == worker_tid);
      worker_calls.push_back(name);
      if (name == "NtReadFile") read_at = i;
      if (name == "NtSetEvent") set_at = i;
    }
    if (name != "VdInitializeEngines" && name != "RtlInitializeCriticalSection" && name != "FakeMissing")
      CHECK(lines[i].ends_with("-> 0x00000000"));  // every status is STATUS_SUCCESS
  }
  CHECK(main_calls == std::vector<std::string>{"NtAllocateVirtualMemory", "RtlInitializeCriticalSection", "NtCreateEvent",
                                               "ExCreateThread", "ObReferenceObjectByHandle", "KeWaitForSingleObject",
                                               "NtWaitForSingleObjectEx", "VdInitializeEngines", "FakeMissing"});
  CHECK(worker_calls == std::vector<std::string>{"NtCreateFile", "NtReadFile", "NtSetEvent"});
  CHECK(read_at < wait_at);  // the event wait returned only after the worker had read the file
  CHECK(set_at < join_at);   // the thread wait returned only after the worker (and its trace lines) had finished
  REQUIRE(vd_at != none);
  CHECK(lines[vd_at].starts_with("[1] VdInitializeEngines r3=0x4C523237 "));  // "LR27", the first 4 bytes of data.bin
  CHECK(lines[vd_at].ends_with("-> 0x00000001"));  // implemented in M3
  CHECK(lines.back().starts_with("[1] FakeMissing "));
}

// sub_82000400 initializes the GPU through the Vd* imports (engines, interrupt callback, a 32 KiB ring, RPTR
// write-back), submits an INTERRUPT packet and three VdSwap frames through the ring and CP_RB_WPTR in the MMIO
// window, then EVENT_WRITE_SHD with the swap counter. It returns (exit 0) only once the written-back RPTR reached
// WPTR, three vblanks and the CP interrupt reached its callback, and the fence holds 3 swaps; otherwise exit 3.
TEST_CASE("fake guest initializes the GPU, submits a ring buffer and swaps", "[unit][gpu][threads]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x82000400)});
  INFO(r.out);
  CHECK(r.code == 0);
  // The only output: the shader summary (the guest loads a vertex and a pixel shader and draws once).
  CHECK(r.out == kSummaryTranslated);
}

// main.cpp prints the shader summary on every exit path once the guest started the GPU: a normal exit (here with the
// disk cache: translated, then read back) and --exit-after-swaps. Without the GPU there is no line (the other tests).
TEST_CASE("fake game prints the shader summary at every exit", "[unit][gpu][threads]") {
  TempDir td;
  const fs::path cache = td.path / "shaders" / "runtime";
  const std::string image = write_image(td.path, 0x82000400);
  auto first =
      run_game(td.path, {"--headless", "--shader-cache-dir", cache.string(), "--dump-shaders", "--image", image});
  INFO(first.out);
  CHECK(first.code == 0);
  CHECK(first.out == kSummaryTranslated);
  size_t spv = 0, raw = 0, txt = 0;
  for (const auto& e : fs::directory_iterator(cache)) {
    const std::string name = e.path().filename().string();
    if (name.ends_with(".raw.spv")) ++raw;
    else if (name.ends_with(".spv")) ++spv;
    else if (name.ends_with(".txt")) ++txt;
  }
  CHECK(spv == 2);
  CHECK(raw == 2);  // --dump-shaders: the bare modules
  CHECK(txt == 2);  // --dump-shaders: the disassembly

  auto second = run_game(td.path, {"--headless", "--shader-cache-dir", cache.string(), "--image", image});
  INFO(second.out);
  CHECK(second.code == 0);
  CHECK(second.out == "[gpu] shaders: keys=2 vs=1 ps=1 translated=0 disk=2 failed=0 new_last_100_swaps=2\n");

  auto swaps = run_game(td.path, {"--headless", "--exit-after-swaps", "1", "--image", image});
  INFO(swaps.out);
  CHECK(swaps.code == 0);
  CHECK(swaps.out == "[gpu] exit after 1 swaps\n" + std::string(kSummaryTranslated));

  auto usage = run_game(td.path, {"--headless", "--dump-shaders", "--image", image});
  CHECK(usage.code == 2);
  CHECK(usage.out.find("--dump-shaders needs --shader-cache-dir") != std::string::npos);
}

// The same guest with --trace-gpu: one line per PM4 packet, per swap, and the per-second counters line format.
TEST_CASE("fake game --trace-gpu writes packet and swap lines", "[unit][gpu][trace]") {
  TempDir td;
  const fs::path file = td.path / "logs" / "gpu-trace.log";
  auto r = run_game(td.path, {"--headless", "--trace-gpu", "--gpu-trace-file", file.string(), "--image",
                              write_image(td.path, 0x82000400)});
  INFO(r.out);
  CHECK(r.code == 0);
  std::ifstream f(file);
  std::vector<std::string> lines;
  for (std::string l; std::getline(f, l);) lines.push_back(l);
  INFO([&] {
    std::string all;
    for (const auto& l : lines) all += l + "\n";
    return all;
  }());
  const auto count = [&](const std::string& line) {
    size_t n = 0;
    for (const auto& l : lines) n += l == line ? 1 : 0;
    return n;
  };
  CHECK(count("pm4 type3 INTERRUPT dwords=2") == 1);
  CHECK(count("pm4 type0 dwords=7") == 3);  // VdSwap's fetch-constant write
  CHECK(count("pm4 type3 LR_SWAP dwords=57") == 3);
  CHECK(count("pm4 type3 EVENT_WRITE_SHD dwords=4") == 1);
  size_t swaps = 0;
  for (const auto& l : lines) swaps += l.starts_with("swap ") && l.ends_with(" 1280x720") ? 1 : 0;
  CHECK(swaps == 3);

  // Without --trace-gpu nothing is written, even with --gpu-trace-file.
  const fs::path off = td.path / "off.log";
  auto o = run_game(td.path, {"--headless", "--gpu-trace-file", off.string(), "--image", write_image(td.path, 0x82000400)});
  CHECK(o.code == 0);
  CHECK_FALSE(fs::exists(off));
}

// sub_82000180 calls FakeImplemented(41) -> 42 and FakeStub(7) -> 0 on thread 1, nothing else.
TEST_CASE("fake game --trace-imports writes one line per call", "[unit][trace]") {
  TempDir td;
  const fs::path file = td.path / "logs" / "game-trace.log";
  auto r = run_game(td.path, {"--headless", "--trace-imports", "--trace-file", file.string(), "--image",
                              write_image(td.path, 0x82000180)});
  INFO(r.out);
  CHECK(r.code == 0);
  std::ifstream f(file);
  std::stringstream ss;
  ss << f.rdbuf();
  CHECK(ss.str() ==
        "[1] FakeImplemented r3=0x00000029 r4=0x00000000 r5=0x00000000 r6=0x00000000 -> 0x0000002A\n"
        "[1] FakeStub r3=0x00000007 r4=0x00000000 r5=0x00000000 r6=0x00000000 -> 0x00000000\n");

  // A missing import is logged before the exit-3 report, and the buffer is flushed on that path.
  const fs::path missing = td.path / "missing.log";
  auto m = run_game(td.path, {"--headless", "--trace-imports", "--trace-file", missing.string(), "--image",
                              write_image(td.path, 0x82000100)});
  CHECK(m.code == 3);
  std::ifstream mf(missing);
  std::stringstream ms;
  ms << mf.rdbuf();
  CHECK(ms.str().find("[1] FakeStub ") != std::string::npos);
  CHECK(ms.str().find("[1] FakeMissing r3=") != std::string::npos);
  CHECK(ms.str().find("-> missing\n") != std::string::npos);

  // Without --trace-file the trace goes to stderr; without --trace-imports nothing is written.
  auto e = run_game(td.path, {"--headless", "--trace-imports", "--image", write_image(td.path, 0x82000180)});
  CHECK(e.code == 0);
  CHECK(e.out.find("[1] FakeStub r3=0x00000007") != std::string::npos);
  const fs::path off = td.path / "off.log";
  auto o = run_game(td.path, {"--headless", "--trace-file", off.string(), "--image", write_image(td.path, 0x82000180)});
  CHECK(o.code == 0);
  CHECK_FALSE(fs::exists(off));
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
  const std::vector<std::string> extra = {"--headless", "--gpu", "null"};
  auto r = lr::pipeline::launch(opts, extra);
  REQUIRE(r);
  CHECK(r.value() == 3);  // FakeMissing is unimplemented

  const std::vector<std::string> bogus = {"--bogus"};
  auto u = lr::pipeline::launch(opts, bogus);  // extra args reach the game
  REQUIRE(u);
  CHECK(u.value() == 2);
}

TEST_CASE("fake guest indirect call to a hole reports the guest address", "[unit]") {
  TempDir td;
  auto r = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x820003A0)});
  CHECK(r.code == 4);
  CHECK(r.out.find("Indirect call to 0x82000240 without a recompiled function (from 0x820003A4)") != std::string::npos);

  // Outside the code range: reported too, without reading the function table.
  auto o = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x820003B0)});
  CHECK(o.code == 4);
  CHECK(o.out.find("Indirect call to 0x12345678 without a recompiled function (from 0x820003B4)") != std::string::npos);

  // An indirect call to a real function still goes through.
  auto ok = run_game(td.path, {"--headless", "--image", write_image(td.path, 0x820003A8)});
  CHECK(ok.code == 0);
}
// Task 17: --exit-after-swaps and --screenshot-after take plain decimal digits only (strtoull would accept "+5" and
// " 5"); a sign, spaces, hex, 0, an overflow or an empty value is a usage error (exit 2) before anything runs.
TEST_CASE("exit-after-swaps rejects signs and spaces", "[unit]") {
  TempDir td;
  const std::string image = write_image(td.path, 0x82000180);
  for (const char* bad : {"+5", " 5", "5 ", "-1", "0", "0x5", "", "5a", "99999999999999999999999"}) {
    INFO("value '" << bad << "'");
    auto r = run_game(td.path, {"--headless", "--exit-after-swaps", bad, "--image", image});
    CHECK(r.code == 2);
    CHECK(r.out.find("--exit-after-swaps needs a positive integer (decimal digits only)") != std::string::npos);
    auto s = run_game(td.path, {"--headless", "--gpu", "vulkan", "--screenshot-after", bad, "x.png", "--image", image});
    CHECK(s.code == 2);
    CHECK(s.out.find("--screenshot-after needs a positive integer (decimal digits only)") != std::string::npos);
  }
  auto ok = run_game(td.path, {"--headless", "--exit-after-swaps", "5", "--image", image});
  CHECK(ok.code == 0);  // sub_82000180 never swaps: it just returns
  auto no_path = run_game(td.path, {"--headless", "--screenshot-after", "1"});
  CHECK(no_path.code == 2);
  auto null_gpu = run_game(td.path, {"--headless", "--gpu", "null", "--screenshot-after", "1", "x.png", "--image", image});
  CHECK(null_gpu.code == 2);
  CHECK(null_gpu.out.find("--screenshot-after needs the Vulkan backend") != std::string::npos);
  CHECK(run_game(td.path, {"--headless", "--gpu", "opengl", "--image", image}).code == 2);
}

// Review Focus 3: without a usable Vulkan device the game (headless too, and by default: --gpu vulkan) stops at
// start with exit 2 and names what is missing. Every driver is masked with an empty ICD list; with no Vulkan loader
// at all the message is the same "no Vulkan driver: ..." (libvulkan.so.1 could not be loaded).
TEST_CASE("no Vulkan device gives a clear error", "[unit][gpu]") {
  TempDir td;
  const fs::path empty = td.path / "empty_icd.json";
  { std::ofstream(empty).flush(); }
  const std::vector<std::string> env = {"VK_ICD_FILENAMES=" + empty.string(), "VK_DRIVER_FILES=" + empty.string()};
  const std::string image = write_image(td.path, 0x82000400);
  for (const bool explicit_gpu : {false, true}) {
    std::vector<std::string> args = {"--headless", "--image", image};
    if (explicit_gpu) args.insert(args.begin(), {"--gpu", "vulkan"});
    auto r = run_game(td.path, args, env, /*default_null_gpu=*/false);
    INFO(r.out);
    CHECK(r.code == 2);
    CHECK(r.out.find("error: no Vulkan driver: ") != std::string::npos);
    CHECK(r.out.find("hint: install a Vulkan loader and a driver") != std::string::npos);
    CHECK(r.out.find("[gpu] shaders:") == std::string::npos);  // stopped before the guest ran
  }
}

// The fake GPU guest (sub_82000400) fills its 1280x720 front buffer with 0xFF2040C0 (big-endian A8R8G8B8) and swaps
// three times; --screenshot-after 1 writes the first presented frame and exits 0. Headless (offscreen presenter),
// lavapipe, validation layer on.
TEST_CASE("clear-color frame is captured", "[unit][gpu][vulkan]") {
  if (!has_icd()) SKIP("no Vulkan ICD installed");
  TempDir td;
  const fs::path png = td.path / "shots" / "frame1.png";
  auto r = run_game(td.path, {"--headless", "--gpu", "vulkan", "--screenshot-after", "1", png.string(), "--image",
                              write_image(td.path, 0x82000400)},
                    {"LR_VULKAN_VALIDATION=1"});
  INFO(r.out);
  CHECK(r.code == 0);
  CHECK(r.out.find("[gpu] screenshot after 1 swaps: " + png.string() + "\n") != std::string::npos);
  check_validation_clean(r.out);
  REQUIRE(fs::exists(png));

  int w = 0, h = 0, comp = 0;
  stbi_uc* pixels = stbi_load(png.c_str(), &w, &h, &comp, 4);
  REQUIRE(pixels != nullptr);
  CHECK(w == 1280);
  CHECK(h == 720);
  size_t wrong = 0;
  for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
    const stbi_uc* p = pixels + i * 4;
    wrong += (p[0] == 0x20 && p[1] == 0x40 && p[2] == 0xC0 && p[3] == 0xFF) ? 0 : 1;
  }
  CHECK(wrong == 0);
  if (wrong != 0) UNSCOPED_INFO("first pixel " << int(pixels[0]) << "," << int(pixels[1]) << "," << int(pixels[2]) << ","
                                               << int(pixels[3]));
  stbi_image_free(pixels);
}

// Review Focus 4: SIGTERM while the GPU thread is presenting (sub_820004E0 swaps forever) ends the run in order:
// exit 0, no hang, and no validation message from the teardown (device idle first, every object destroyed).
TEST_CASE("closing during rendering exits cleanly", "[unit][gpu][vulkan]") {
  if (!has_icd()) SKIP("no Vulkan ICD installed");
  TempDir td;
  const fs::path trace = td.path / "gpu-trace.log";
  const pid_t pid = spawn_game(td.path,
                               {"--headless", "--gpu", "vulkan", "--trace-gpu", "--gpu-trace-file", trace.string(),
                                "--image", write_image(td.path, 0x820004E0)},
                               {"LR_VULKAN_VALIDATION=1"}, false);
  // Rendering: wait for the third presented swap in the GPU trace (flushed after every step).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  bool rendering = false;
  while (!rendering && std::chrono::steady_clock::now() < deadline) {
    rendering = read_text(trace).find("\nswap 3 ") != std::string::npos;
    int st = 0;
    if (!rendering && waitpid(pid, &st, WNOHANG) == pid)
      FAIL("the game ended before rendering:\n" << read_text(td.path / "out.log"));
    if (!rendering) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!rendering) kill(pid, SIGKILL);
  REQUIRE(rendering);
  REQUIRE(kill(pid, SIGTERM) == 0);
  // Exits by itself, without a hang (SIGKILL after 60 s fails the test).
  const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  int st = 0;
  pid_t done = 0;
  while ((done = waitpid(pid, &st, WNOHANG)) == 0 && std::chrono::steady_clock::now() < stop_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (done == 0) {
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    FAIL("the game did not exit within 60 s of SIGTERM");
  }
  const std::string out = read_text(td.path / "out.log");
  INFO(out);
  REQUIRE(WIFEXITED(st));
  CHECK(WEXITSTATUS(st) == 0);
  CHECK(out.find("[game] signal 15: exiting") != std::string::npos);
  CHECK(out.find("[gpu] vulkan: shut down") != std::string::npos);  // the teardown ran to the end
  CHECK(out.find("[game] shutdown timed out") == std::string::npos);
  check_validation_clean(out);
}

namespace {
// Waits until out.log contains `text` (or the game ended, or 30 s passed).
bool wait_for_output(const fs::path& dir, pid_t pid, const std::string& text) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (read_text(dir / "out.log").find(text) != std::string::npos) return true;
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) == pid) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

// Waits up to `limit` for the game to exit; -2 when it did not (it is then killed), -1 when a signal ended it.
int wait_exit(pid_t pid, std::chrono::seconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  int st = 0;
  pid_t done = 0;
  while ((done = waitpid(pid, &st, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (done == 0) {
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return -2;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
}  // namespace

// Fix round 1: the orderly exit is bounded. With --gpu test-stall the GPU thread hangs in its first swap, so
// GpuSystem::stop() never returns: the shutdown watchdog ends the run after 5 s ("[game] shutdown timed out", exit
// 0), and a second SIGTERM during the shutdown ends it at once.
TEST_CASE("shutdown is bounded when the GPU thread hangs", "[unit][gpu]") {
  using Clock = std::chrono::steady_clock;
  TempDir td;
  const std::string image = write_image(td.path, 0x820004E0);
  {
    const pid_t pid = spawn_game(td.path, {"--headless", "--gpu", "test-stall", "--image", image}, {}, false);
    REQUIRE(wait_for_output(td.path, pid, "[gpu] test-stall: swap blocked"));
    const auto sent = Clock::now();
    REQUIRE(kill(pid, SIGTERM) == 0);
    const int code = wait_exit(pid, std::chrono::seconds(20));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent);
    const std::string out = read_text(td.path / "out.log");
    INFO(out << "exit after " << took.count() << " ms");
    CHECK(code == 0);
    CHECK(out.find("[game] signal 15: exiting") != std::string::npos);
    CHECK(out.find("[game] shutdown timed out") != std::string::npos);
    CHECK(took >= std::chrono::milliseconds(4500));  // the watchdog waited its 5 s
    CHECK(took < std::chrono::milliseconds(7000));   // and no longer
  }
  {
    const pid_t pid = spawn_game(td.path, {"--headless", "--gpu", "test-stall", "--image", image}, {}, false);
    REQUIRE(wait_for_output(td.path, pid, "[gpu] test-stall: swap blocked"));
    REQUIRE(kill(pid, SIGTERM) == 0);
    REQUIRE(wait_for_output(td.path, pid, "[game] signal 15: exiting"));
    const auto sent = Clock::now();
    REQUIRE(kill(pid, SIGTERM) == 0);
    const int code = wait_exit(pid, std::chrono::seconds(20));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - sent);
    const std::string out = read_text(td.path / "out.log");
    INFO(out << "exit after " << took.count() << " ms");
    CHECK(code == 0);
    CHECK(out.find("[game] shutdown timed out") != std::string::npos);
    CHECK(took < std::chrono::milliseconds(2000));  // immediately, not after the 5 s
  }
}
#else
TEST_CASE("fake game tests need SDL2 (target lr_fake_game not built)", "[unit]") { SKIP("SDL2 not found"); }
#endif
