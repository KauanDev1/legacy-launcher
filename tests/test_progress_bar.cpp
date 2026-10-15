// SPDX-License-Identifier: MIT
#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "launcher/cli/progress_bar.h"
#include "pipeline/process.h"
#include "support/temp_dir.h"

using namespace lr::cli;
using lr::pipeline::Progress;
using lr::pipeline::StageId;

TEST_CASE("non-tty bar prints one line per 10% step", "[unit]") {
  std::ostringstream os;
  ProgressBar bar(os, false);
  bar.on_progress({StageId::Extract, 0.50f, "x"});
  bar.on_progress({StageId::Extract, 0.55f, "x"});
  const std::string s = os.str();
  CHECK(s.find("[extract] 50%") != std::string::npos);
  CHECK(s.find("[extract] 50%") == s.rfind("[extract] 50%"));
  CHECK(s.find("55%") == std::string::npos);
}

TEST_CASE("non-tty bar prints again when the stage changes", "[unit]") {
  std::ostringstream os;
  ProgressBar bar(os, false);
  bar.on_progress({StageId::Extract, 1.0f, "done"});
  bar.on_progress({StageId::Verify, 1.0f, "cached"});
  CHECK(os.str().find("[verify] 100% cached") != std::string::npos);
}

TEST_CASE("tty bar redraws one line with a 40 column bar", "[unit]") {
  std::ostringstream os;
  ProgressBar bar(os, true);
  bar.on_progress({StageId::Build, 0.45f, "compiling"});
  const std::string s = os.str();
  CHECK(s.front() == '\r');
  CHECK(s.find("[build] [##################......................] 45% compiling") != std::string::npos);
  CHECK(s.find('\n') == std::string::npos);
  bar.on_progress({StageId::Build, 1.0f, "done"});
  bar.on_log("hello");  // clears the line, prints the log, redraws
  CHECK(os.str().find("hello\n") != std::string::npos);
}

TEST_CASE("cancelled() reflects the SIGINT flag", "[unit]") {
  std::ostringstream os;
  ProgressBar bar(os, false);
  ProgressBar::reset_cancel();
  CHECK_FALSE(bar.cancelled());
  ProgressBar::request_cancel();
  CHECK(bar.cancelled());
  ProgressBar::reset_cancel();
}

TEST_CASE("non-tty bar always shows the final done line", "[unit]") {
  std::ostringstream os;
  ProgressBar bar(os, false);
  bar.on_progress({StageId::Extract, 0.0f, "start"});
  bar.on_progress({StageId::Extract, 1.0f, "package: a"});
  bar.on_progress({StageId::Extract, 1.0f, "done"});
  CHECK(os.str().find("[extract] 100% done") != std::string::npos);
}

namespace {

// Forks; the child installs the launcher's signal handlers, raises `sig` once and exits 0 when it survived
// with the cancel flag set.
bool one_signal_only_cancels(int sig) {
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ProgressBar::reset_cancel();
    ProgressBar::install_signal_handlers();
    ::raise(sig);
    std::ostringstream os;
    ::_exit(ProgressBar(os, false).cancelled() ? 0 : 1);
  }
  int st = 0;
  REQUIRE(::waitpid(pid, &st, 0) == pid);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

}  // namespace

TEST_CASE("one SIGINT, SIGTERM or SIGHUP only requests a clean cancel", "[unit]") {
  CHECK(one_signal_only_cancels(SIGINT));
  CHECK(one_signal_only_cancels(SIGTERM));
  CHECK(one_signal_only_cancels(SIGHUP));
}

TEST_CASE("a second signal kills the running tool's process group before the launcher dies", "[unit]") {
  lr::testing::TempDir tmp{"lr_signal_test"};
  const auto pidfile = tmp.path / "grandchild.pid";

  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    ProgressBar::reset_cancel();
    ProgressBar::install_signal_handlers();
    // No `cancelled` callback: only the signal handler can stop the tool tree here.
    lr::pipeline::ToolInvocation inv;
    inv.program = "/bin/sh";
    inv.args = {"-c", "sleep 30 & echo $! > \"$1\"; wait", "sh", pidfile.string()};
    (void)lr::pipeline::run_tool(inv);
    ::_exit(0);
  }

  pid_t grandchild = 0;
  for (int i = 0; i < 100 && grandchild <= 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::ifstream f(pidfile);
    f >> grandchild;
  }
  if (grandchild <= 0) ::kill(child, SIGKILL);
  REQUIRE(grandchild > 0);
  REQUIRE(::kill(grandchild, 0) == 0);

  ::kill(child, SIGTERM);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // standard signals do not queue
  ::kill(child, SIGTERM);
  int st = 0;
  REQUIRE(::waitpid(child, &st, 0) == child);
  CHECK(WIFSIGNALED(st));
  if (WIFSIGNALED(st)) CHECK(WTERMSIG(st) == SIGTERM);

  bool gone = false;
  for (int i = 0; i < 100 && !gone; ++i) {
    gone = ::kill(grandchild, 0) != 0 && errno == ESRCH;
    if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!gone) ::kill(grandchild, SIGKILL);
  CHECK(gone);
}
