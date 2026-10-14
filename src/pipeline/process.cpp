// SPDX-License-Identifier: GPL-3.0-or-later
#include "pipeline/process.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>

extern char** environ;

namespace lr::pipeline {
namespace fs = std::filesystem;
namespace {

using Clock = std::chrono::steady_clock;

void ignore_sigpipe() {
  static std::once_flag once;
  std::call_once(once, [] { ::signal(SIGPIPE, SIG_IGN); });
}

Error io_error(const std::string& what) {
  return Error{ErrorCode::IoError, what + ": " + std::strerror(errno), ""};
}

// Resolves program to a concrete path (or ToolMissing).
Result<fs::path> resolve(const fs::path& program) {
  if (program.has_parent_path()) return program;
  auto p = find_program(program.string());
  if (!p) return Error{ErrorCode::ToolMissing, "tool not found: " + program.string(), "install it or add it to PATH"};
  return *p;
}

struct Child {
  pid_t pid = -1;
  int read_fd = -1;
};

// stdout (and optionally stderr) -> pipe; stdin <- /dev/null. extra_stderr_null: stderr -> /dev/null.
Result<Child> spawn_child(const fs::path& program, const std::vector<std::string>& args, const fs::path& cwd,
                          bool merge_stderr) {
  ignore_sigpipe();
  LR_TRY_ASSIGN(exe, resolve(program));
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) return io_error("pipe");

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);  // dup2 clears CLOEXEC on the new fd
  if (merge_stderr)
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  else
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str());

  std::string exe_s = exe.string();
  std::string argv0 = exe.filename().string();
  std::vector<char*> argv;
  argv.push_back(argv0.data());
  std::vector<std::string> copy = args;
  for (auto& a : copy) argv.push_back(a.data());
  argv.push_back(nullptr);

  pid_t pid = -1;
  int rc = ::posix_spawn(&pid, exe_s.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(fds[1]);
  if (rc != 0) {
    ::close(fds[0]);
    ErrorCode code = (rc == ENOENT || rc == EACCES) ? ErrorCode::ToolMissing : ErrorCode::IoError;
    return Error{code, "cannot start " + exe_s + ": " + std::strerror(rc), ""};
  }
  return Child{pid, fds[0]};
}

int wait_status(pid_t pid) {
  int st = 0;
  while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  return st;
}

Error exit_error(const fs::path& program, int st) {
  std::string name = program.string();
  if (WIFSIGNALED(st))
    return Error{ErrorCode::ToolFailed, name + " killed by signal " + std::to_string(WTERMSIG(st)), ""};
  return Error{ErrorCode::ToolFailed, name + " exited with " + std::to_string(WEXITSTATUS(st)), ""};
}

}  // namespace

std::optional<fs::path> find_program(std::string_view name) {
  if (name.empty()) return std::nullopt;
  const char* path_env = std::getenv("PATH");
  std::string path = path_env ? path_env : "/usr/local/bin:/usr/bin:/bin";
  size_t pos = 0;
  while (pos <= path.size()) {
    size_t end = path.find(':', pos);
    if (end == std::string::npos) end = path.size();
    std::string dir = path.substr(pos, end - pos);
    if (dir.empty()) dir = ".";
    fs::path cand = fs::path(dir) / std::string(name);
    std::error_code ec;
    if (::access(cand.c_str(), X_OK) == 0 && fs::is_regular_file(cand, ec)) return cand;
    pos = end + 1;
  }
  return std::nullopt;
}

Result<void> run_tool(const ToolInvocation& inv) {
  std::ofstream log;
  if (!inv.log_file.empty()) {
    std::error_code ec;
    if (inv.log_file.has_parent_path()) fs::create_directories(inv.log_file.parent_path(), ec);
    log.open(inv.log_file, std::ios::app);
    if (!log) return Error{ErrorCode::IoError, "cannot open log file " + inv.log_file.string(), ""};
  }

  LR_TRY_ASSIGN(child, spawn_child(inv.program, inv.args, inv.cwd, true));

  std::deque<std::string> tail;
  std::string buf;
  auto emit = [&](std::string line) {
    if (log.is_open()) {
      log << line << '\n';
      log.flush();
    }
    if (inv.on_line) inv.on_line(line);
    tail.push_back(std::move(line));
    if (tail.size() > 20) tail.pop_front();
  };

  bool cancelled = false;
  bool eof = false;
  std::optional<Clock::time_point> term_at;
  bool killed = false;
  char chunk[4096];
  while (!eof) {
    if (!cancelled && inv.cancelled && inv.cancelled()) {
      cancelled = true;
      term_at = Clock::now();
      ::kill(child.pid, SIGTERM);
    }
    if (cancelled && !killed && Clock::now() - *term_at >= std::chrono::seconds(3)) {
      ::kill(child.pid, SIGKILL);
      killed = true;
    }
    pollfd pfd{child.read_fd, POLLIN, 0};
    int pr = ::poll(&pfd, 1, 100);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    ssize_t n = ::read(child.read_fd, chunk, sizeof chunk);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) {
      eof = true;
      break;
    }
    buf.append(chunk, static_cast<size_t>(n));
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      emit(buf.substr(0, nl));
      buf.erase(0, nl + 1);
    }
  }
  if (!buf.empty()) emit(std::move(buf));
  ::close(child.read_fd);
  // A grandchild may hold the pipe open; if we were cancelled make sure the child is gone before waiting.
  if (cancelled && !killed) ::kill(child.pid, SIGKILL);
  int st = wait_status(child.pid);

  if (cancelled) return Error{ErrorCode::Cancelled, inv.program.string() + " cancelled", ""};
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return {};
  Error e = exit_error(inv.program, st);
  std::string hint;
  if (!inv.log_file.empty()) hint = "see " + inv.log_file.string();
  for (auto& l : tail) hint += (hint.empty() ? "" : "\n") + l;
  e.hint = std::move(hint);
  return e;
}

Result<std::string> capture_output(const fs::path& program, const std::vector<std::string>& args) {
  LR_TRY_ASSIGN(child, spawn_child(program, args, {}, false));
  std::string out;
  char chunk[4096];
  for (;;) {
    ssize_t n = ::read(child.read_fd, chunk, sizeof chunk);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.append(chunk, static_cast<size_t>(n));
  }
  ::close(child.read_fd);
  int st = wait_status(child.pid);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return out;
  return exit_error(program, st);
}

}  // namespace lr::pipeline
