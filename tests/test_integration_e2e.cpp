// SPDX-License-Identifier: MIT
// End to end on the real game: pipeline::run (doctor..build) into a data root, then launch the recompiled game
// headless. M2 acceptance (spec section 14): the guest runs its startup until the first video import that creates the
// GPU (exit 3 on a Vd* import that is not one of the video query stubs) within 300 s, and a second run executes no
// stage at all.
#include <catch2/catch_test_macros.hpp>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pipeline/pipeline.h"
#include "support/temp_dir.h"

using namespace lr;
using namespace lr::pipeline;
namespace fs = std::filesystem;

namespace {

// Prints stage changes and every 10% step; records every progress event.
struct RecordingSink : ProgressSink {
  std::vector<Progress> events;
  int last_percent = -1;
  StageId last_stage = StageId::Build;
  void on_progress(const Progress& p) override {
    events.push_back(p);
    const int pct = static_cast<int>(p.fraction * 100.0f);
    if (p.stage != last_stage || pct / 10 != last_percent / 10 || p.fraction >= 1.0f || p.fraction == 0.0f) {
      std::fprintf(stderr, "[%s] %3d%% %s\n", std::string(stage_name(p.stage)).c_str(), pct, p.message.c_str());
      last_stage = p.stage;
      last_percent = pct;
    }
  }
  void on_log(std::string_view) override {}
  bool cancelled() const override { return false; }
  size_t starts() const {
    size_t n = 0;
    for (const auto& e : events) n += e.message == "start";
    return n;
  }
};

void report(const Result<void>& r) {
  if (!r)
    std::fprintf(stderr, "pipeline failed: [%s] %s\nhint: %s\n", std::string(to_string(r.error().code)).c_str(),
                 r.error().message.c_str(), r.error().hint.c_str());
}

// Runs launch() with the process's stdout+stderr redirected to `log` (the game inherits them).
Result<int> launch_captured(const Options& opts, const std::vector<std::string>& extra, const fs::path& log) {
  std::fflush(nullptr);
  const int saved_out = ::dup(1), saved_err = ::dup(2);
  const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  REQUIRE(saved_out >= 0);
  REQUIRE(saved_err >= 0);
  REQUIRE(fd >= 0);
  ::dup2(fd, 1);
  ::dup2(fd, 2);
  ::close(fd);
  auto r = launch(opts, extra);
  std::fflush(nullptr);
  ::dup2(saved_out, 1);
  ::dup2(saved_err, 2);
  ::close(saved_out);
  ::close(saved_err);
  return r;
}

// Kills (SIGKILL) every child process of this test once `limit` elapses, unless stopped first; launch() then
// returns 128 + 9 and the exit-code check fails. Children are found through /proc (ppid field of /proc/<pid>/stat).
class Watchdog {
 public:
  explicit Watchdog(std::chrono::seconds limit) : thread_([this, limit] { watch(limit); }) {}
  ~Watchdog() {
    {
      std::lock_guard lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  bool fired() const { return fired_; }

 private:
  void watch(std::chrono::seconds limit) {
    std::unique_lock lock(mu_);
    if (cv_.wait_for(lock, limit, [this] { return stop_; })) return;
    fired_ = true;
    for (pid_t pid : children()) ::kill(pid, SIGKILL);
  }
  static std::vector<pid_t> children() {
    std::vector<pid_t> out;
    DIR* d = ::opendir("/proc");
    if (!d) return out;
    while (const dirent* e = ::readdir(d)) {
      const pid_t pid = static_cast<pid_t>(std::atoi(e->d_name));
      if (pid <= 0) continue;
      std::ifstream f(std::string("/proc/") + e->d_name + "/stat");
      std::string stat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      const auto close = stat.rfind(')');  // comm may contain spaces; fields after it: state ppid ...
      if (close == std::string::npos) continue;
      std::istringstream rest(stat.substr(close + 1));
      std::string state;
      pid_t ppid = 0;
      if (rest >> state >> ppid && ppid == ::getpid()) out.push_back(pid);
    }
    ::closedir(d);
    return out;
  }

  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<bool> fired_{false};
  std::thread thread_;
};

// The video queries are stubs (spec section 5.2): reaching one of them is not the finish line.
const std::set<std::string> kVideoQueryStubs = {"VdQueryVideoMode", "XGetVideoMode", "VdGetCurrentDisplayInformation",
                                                "VdGetCurrentDisplayGamma", "VdQueryVideoFlags",
                                                "VdIsHSIOTrainingSucceeded"};

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream os;
  os << f.rdbuf();
  return os.str();
}

}  // namespace

// Env: LEGACYRECOMP_ISO (required), LEGACYRECOMP_TEST_ROOT (default: fresh temp dir, removed on success).
// Resources come from the build tree (<build>/share/legacyrecomp, found next to lr_tests).
TEST_CASE("real ISO: install, run the startup until the first GPU-creating Vd import, cached rerun", "[integration]") {
  const char* iso = std::getenv("LEGACYRECOMP_ISO");
  if (!iso) SKIP("LEGACYRECOMP_ISO not set");

  const char* root_env = std::getenv("LEGACYRECOMP_TEST_ROOT");
  const bool temp_root = !(root_env && *root_env);
  fs::path root;
  if (temp_root) {
    root = lr::testing::make_temp_dir("lr_integration_e2e");
  } else {
    root = root_env;
    fs::create_directories(root);
  }

  Options opts;
  opts.iso = iso;
  opts.root = root;

  RecordingSink first;
  auto r = run(opts, first);
  report(r);
  REQUIRE(r);
  CHECK(fs::exists(root / "bin" / "legacyrecomp-game"));

  const fs::path log = root / "work" / "logs" / "e2e-launch.log";
  const auto started = std::chrono::steady_clock::now();
  Result<int> code = Error{ErrorCode::IoError, "not launched", ""};
  bool timed_out = false;
  {
    Watchdog watchdog(std::chrono::seconds(300));
    code = launch_captured(opts, {"--headless"}, log);
    timed_out = watchdog.fired();
  }
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
  REQUIRE(code);
  const std::string out = slurp(log);
  std::fprintf(stderr, "legacyrecomp-game exited with %d after %lld s:\n%s\n", code.value(),
               static_cast<long long>(seconds.count()), out.c_str());
  CHECK_FALSE(timed_out);
  CHECK(code.value() == 3);
  std::smatch m;
  const std::regex unimplemented("Unimplemented import: (\\S+)");
  REQUIRE(std::regex_search(out, m, unimplemented));
  const std::string first_missing = m[1].str();
  CHECK(std::regex_search(first_missing, std::regex("^Vd")));
  CHECK(kVideoQueryStubs.count(first_missing) == 0);

  RecordingSink second;
  auto again = run(opts, second);
  report(again);
  REQUIRE(again);
  CHECK(second.starts() == 0);
  CHECK(second.events.size() == 7);  // one "cached" event per stage

  if (temp_root) fs::remove_all(root);
}
