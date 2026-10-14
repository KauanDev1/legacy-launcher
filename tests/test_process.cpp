// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <csignal>
#include <cerrno>
#include <unistd.h>

#include "pipeline/process.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {
fs::path tmp_log(const char* name) {
  fs::path d = fs::temp_directory_path() / "lr_test_process";
  fs::create_directories(d);
  fs::path p = d / name;
  fs::remove(p);
  return p;
}
std::string slurp(const fs::path& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
}  // namespace

TEST_CASE("run_tool passes args with spaces and parens verbatim", "[unit]") {
  auto log = tmp_log("args.log");
  std::vector<std::string> lines;
  ToolInvocation inv;
  inv.program = "/usr/bin/printf";
  inv.args = {"%s\n", "Minecraft - Xbox 360 Edition (World).iso"};
  inv.log_file = log;
  inv.on_line = [&](std::string_view l) { lines.emplace_back(l); };
  auto r = run_tool(inv);
  REQUIRE(r.ok());
  CHECK(slurp(log) == "Minecraft - Xbox 360 Edition (World).iso\n");
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "Minecraft - Xbox 360 Edition (World).iso");
}

TEST_CASE("run_tool reports non-zero exit with log tail hint", "[unit]") {
  ToolInvocation inv;
  inv.program = "/bin/sh";
  inv.args = {"-c", "for i in $(seq 1 30); do echo line$i; done; exit 7"};
  inv.log_file = tmp_log("fail.log");
  auto r = run_tool(inv);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::ToolFailed);
  CHECK(r.error().message.find("exited with 7") != std::string::npos);
  CHECK(r.error().hint.find("line30") != std::string::npos);
  CHECK(r.error().hint.find("line11") != std::string::npos);
  CHECK(r.error().hint.find("line10") == std::string::npos);
}

TEST_CASE("run_tool cancellation kills the child", "[unit]") {
  auto t0 = std::chrono::steady_clock::now();
  ToolInvocation inv;
  inv.program = "/bin/sleep";
  inv.args = {"30"};
  inv.cancelled = [&] { return std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(200); };
  auto r = run_tool(inv);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::Cancelled);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
}

TEST_CASE("run_tool missing tool and signal death", "[unit]") {
  ToolInvocation inv;
  inv.program = "definitely-not-a-program-xyz";
  auto r = run_tool(inv);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::ToolMissing);

  ToolInvocation k;
  k.program = "/bin/sh";
  k.args = {"-c", "kill -9 $$"};
  auto r2 = run_tool(k);
  REQUIRE_FALSE(r2.ok());
  CHECK(r2.error().message.find("killed by signal 9") != std::string::npos);
}

TEST_CASE("capture_output returns stdout", "[unit]") {
  auto r = capture_output("/bin/sh", {"-c", "printf 'a b\\n'; echo err >&2"});
  REQUIRE(r.ok());
  CHECK(r.value() == "a b\n");
  auto f = capture_output("/bin/sh", {"-c", "exit 3"});
  REQUIRE_FALSE(f.ok());
  CHECK(f.error().code == ErrorCode::ToolFailed);
}

TEST_CASE("find_program searches PATH", "[unit]") {
  CHECK(find_program("sh").has_value());
  CHECK_FALSE(find_program("definitely-not-a-program-xyz").has_value());
}

namespace {
bool pid_gone(pid_t pid) {
  for (int i = 0; i < 20; ++i) {
    if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}
pid_t read_pid(const fs::path& p) {
  std::ifstream f(p);
  long v = 0;
  f >> v;
  return static_cast<pid_t>(v);
}
}  // namespace

TEST_CASE("run_tool cancellation kills grandchildren and returns promptly", "[unit]") {
  fs::path pidf = tmp_log("gc.pid");
  auto t0 = std::chrono::steady_clock::now();
  ToolInvocation inv;
  inv.program = "/bin/sh";
  inv.args = {"-c", "sleep 30 & echo $! > " + pidf.string() + "; wait"};
  inv.cancelled = [&] { return std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(200); };
  auto r = run_tool(inv);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::Cancelled);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
  pid_t gc = read_pid(pidf);
  REQUIRE(gc > 0);
  CHECK(pid_gone(gc));
}

TEST_CASE("run_tool escalates to SIGKILL for a child ignoring SIGTERM", "[unit]") {
  auto t0 = std::chrono::steady_clock::now();
  ToolInvocation inv;
  inv.program = "/bin/sh";
  inv.args = {"-c", "trap '' TERM; while :; do sleep 1; done"};
  inv.cancelled = [&] { return std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(200); };
  auto r = run_tool(inv);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::Cancelled);
  auto dt = std::chrono::steady_clock::now() - t0;
  CHECK(dt > std::chrono::seconds(3));
  CHECK(dt < std::chrono::seconds(5));
}

TEST_CASE("run_tool children get default SIGPIPE; bad cwd is IoError", "[unit]") {
  std::vector<std::string> lines;
  ToolInvocation inv;
  inv.program = "/bin/sh";
  inv.args = {"-c", "while read k v; do [ \"$k\" = SigIgn: ] && echo $v; done < /proc/$$/status; exit 0"};
  inv.on_line = [&](std::string_view l) { lines.emplace_back(l); };
  REQUIRE(run_tool(inv).ok());
  REQUIRE(!lines.empty());
  // SIGPIPE is signal 13 -> bit 12 (0x1000)
  CHECK((std::stoul(lines.back(), nullptr, 16) & 0x1000UL) == 0);

  ToolInvocation bad;
  bad.program = "/bin/true";
  bad.cwd = "/nonexistent-dir-xyz";
  auto r = run_tool(bad);
  REQUIRE_FALSE(r.ok());
  CHECK(r.error().code == ErrorCode::IoError);
  CHECK(r.error().message.find("/nonexistent-dir-xyz") != std::string::npos);
}
