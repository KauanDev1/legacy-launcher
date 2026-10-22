// SPDX-License-Identifier: MIT
// Test helpers for the real-game integration tests: launch the built game with its output captured to a file, and a
// watchdog that kills it after a time limit.
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "pipeline/pipeline.h"

namespace lr::testing {

// Runs launch() with the process's stdout+stderr redirected to `log` (the game inherits them).
inline Result<int> launch_captured(const pipeline::Options& opts, const std::vector<std::string>& extra,
                                   const std::filesystem::path& log) {
  std::fflush(nullptr);
  const int saved_out = ::dup(1), saved_err = ::dup(2);
  const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  REQUIRE(saved_out >= 0);
  REQUIRE(saved_err >= 0);
  REQUIRE(fd >= 0);
  ::dup2(fd, 1);
  ::dup2(fd, 2);
  ::close(fd);
  auto r = pipeline::launch(opts, extra);
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

}  // namespace lr::testing
