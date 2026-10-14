// SPDX-License-Identifier: MIT
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

  if (!cwd.empty()) {
    std::error_code ec;
    if (!fs::is_directory(cwd, ec)) {
      ::close(fds[0]);
      ::close(fds[1]);
      return Error{ErrorCode::IoError, "working directory does not exist: " + cwd.string(), ""};
    }
  }
  std::string exe_s = exe.string();
  if (::access(exe_s.c_str(), X_OK) != 0) {
    int e = errno;
    ::close(fds[0]);
    ::close(fds[1]);
    return Error{ErrorCode::ToolMissing, "cannot execute " + exe_s + ": " + std::strerror(e), ""};
  }

  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init(&fa);
  posix_spawnattr_init(&attr);
  int rc = 0;
  auto chk = [&](int r) {
    if (rc == 0) rc = r;
  };
  chk(posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0));
  chk(posix_spawn_file_actions_adddup2(&fa, fds[1], 1));  // dup2 clears CLOEXEC on the new fd
  if (merge_stderr)
    chk(posix_spawn_file_actions_adddup2(&fa, fds[1], 2));
  else
    chk(posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0));
  if (!cwd.empty()) chk(posix_spawn_file_actions_addchdir_np(&fa, cwd.c_str()));
  // Own process group (so cancel can signal the whole tree); restore default SIGPIPE and an empty mask.
  sigset_t def, mask;
  sigemptyset(&def);
  sigaddset(&def, SIGPIPE);
  sigemptyset(&mask);
  chk(posix_spawnattr_setpgroup(&attr, 0));
  chk(posix_spawnattr_setsigdefault(&attr, &def));
  chk(posix_spawnattr_setsigmask(&attr, &mask));
  chk(posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK));

  std::string argv0 = exe.filename().string();
  std::vector<char*> argv;
  argv.push_back(argv0.data());
  std::vector<std::string> copy = args;
  for (auto& a : copy) argv.push_back(a.data());
  argv.push_back(nullptr);

  pid_t pid = -1;
  if (rc == 0) rc = ::posix_spawn(&pid, exe_s.c_str(), &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  ::close(fds[1]);
  if (rc != 0) {
    ::close(fds[0]);
    return Error{ErrorCode::IoError, "cannot start " + exe_s + ": " + std::strerror(rc), ""};
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
  bool killed = false;
  std::optional<Clock::time_point> term_at, kill_at;
  std::optional<Error> io_err;
  char chunk[4096];
  for (;;) {
    if (!cancelled && inv.cancelled && inv.cancelled()) {
      cancelled = true;
      term_at = Clock::now();
      ::kill(-child.pid, SIGTERM);
    }
    if (cancelled && !killed && Clock::now() - *term_at >= std::chrono::seconds(3)) {
      ::kill(-child.pid, SIGKILL);
      killed = true;
      kill_at = Clock::now();
    }
    // After SIGKILL + grace, stop reading even without EOF (a stray holder of the pipe must not block us).
    if (killed && Clock::now() - *kill_at >= std::chrono::milliseconds(500)) break;
    pollfd pfd{child.read_fd, POLLIN, 0};
    int pr = ::poll(&pfd, 1, 100);
    if (pr < 0) {
      if (errno == EINTR) continue;
      io_err = io_error("poll");
      break;
    }
    if (pr == 0) continue;
    ssize_t n = ::read(child.read_fd, chunk, sizeof chunk);
    if (n < 0) {
      if (errno == EINTR) continue;
      io_err = io_error("read");
      break;
    }
    if (n == 0) break;
    buf.append(chunk, static_cast<size_t>(n));
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      emit(buf.substr(0, nl));
      buf.erase(0, nl + 1);
    }
  }
  if (!buf.empty()) emit(std::move(buf));
  ::close(child.read_fd);
  if (cancelled || io_err) ::kill(-child.pid, SIGKILL);  // never leave the tree behind
  int st = wait_status(child.pid);

  if (io_err) return *io_err;
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
    if (n < 0) {
      Error e = io_error("read");
      ::close(child.read_fd);
      ::kill(-child.pid, SIGKILL);
      wait_status(child.pid);
      return e;
    }
    if (n == 0) break;
    out.append(chunk, static_cast<size_t>(n));
  }
  ::close(child.read_fd);
  int st = wait_status(child.pid);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return out;
  return exit_error(program, st);
}

}  // namespace lr::pipeline
