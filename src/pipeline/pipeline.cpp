// SPDX-License-Identifier: MIT
#include "pipeline/pipeline.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "common/paths.h"
#include "pipeline/game_config.h"
#include "pipeline/game_exit.h"
#include "pipeline/runner.h"
#include "pipeline/stages/stages.h"
#include "pipeline/state.h"

extern char** environ;

namespace lr::pipeline {

std::vector<std::unique_ptr<Stage>> default_stages(DoctorProbe& probe) {
  std::vector<std::unique_ptr<Stage>> s;
  s.push_back(make_doctor_stage(probe));
  s.push_back(make_extract_stage());
  s.push_back(make_verify_stage());
  s.push_back(make_patch_stage());
  s.push_back(make_analyse_stage());
  s.push_back(make_recompile_stage());
  s.push_back(make_build_stage());
  return s;
}

Result<fs::path> find_resources() {
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec) return Error{ErrorCode::IoError, "cannot resolve /proc/self/exe: " + ec.message(), ""};
  return resources_dir(exe, std::getenv("LEGACYRECOMP_RESOURCES"));
}

fs::path data_root(const Options& options) {
  if (!options.root.empty()) return options.root;
  return default_data_root(std::getenv("XDG_DATA_HOME"), std::getenv("HOME"));
}

Result<void> run(const Options& options, ProgressSink& sink) {
  LR_TRY_ASSIGN(resources, find_resources());
  LR_TRY_ASSIGN(game, load_game_config(resources / "config" / "minecraft-tu1"));
  Options opts = options;
  opts.root = data_root(options);
  std::error_code ec;
  fs::create_directories(opts.root, ec);
  if (ec) return Error{ErrorCode::IoError, "cannot create " + opts.root.string() + ": " + ec.message(), ""};

  StageContext ctx{opts, DataPaths::from_root(opts.root), resources, game, sink};
  LR_TRY_ASSIGN(state, StateStore::load(ctx.paths.state_file));
  ctx.state = &state;
  SystemDoctorProbe probe;
  auto owned = default_stages(probe);
  std::vector<Stage*> stages;
  for (auto& s : owned) stages.push_back(s.get());
  return run_stages(stages, ctx, state);
}

Result<int> launch(const Options& options, std::span<const std::string> extra_args) {
  const DataPaths p = DataPaths::from_root(data_root(options));
  const fs::path game = p.bin / "legacyrecomp-game";
  const fs::path image = p.image / "default_tu1.xex";
  if (::access(game.c_str(), X_OK) != 0)
    return Error{ErrorCode::NotFound, "game not built: " + game.string(), "run `legacyrecomp install --iso <iso>` first"};
  if (::access(image.c_str(), R_OK) != 0)
    return Error{ErrorCode::NotFound, "patched image missing: " + image.string(),
                 "run `legacyrecomp install --iso <iso>` again"};

  // user/ holds the saves and the cache; the pipeline never deletes it. Creation failure aborts the launch.
  {
    std::error_code ec;
    fs::create_directories(p.user, ec);
    if (ec || !fs::is_directory(p.user))
      return Error{ErrorCode::IoError,
                   "cannot create user data folder " + p.user.string() + ": " + (ec ? ec.message() : "not a directory"),
                   "check the permissions of the data root or choose another one with --root"};
  }

  // --trace-file and --gpu-trace-file always: the game only writes them when --trace-imports / --trace-gpu is among
  // the extra arguments (spec section 11).
  std::vector<std::string> args = {game.string(), "--image", image.string(), "--data", p.game.string(),
                                   "--user", p.user.string(),
                                   "--trace-file", game_trace_file(p.root).string(),
                                   "--gpu-trace-file", gpu_trace_file(p.root).string()};
  args.insert(args.end(), extra_args.begin(), extra_args.end());
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  // Inherited stdio and process group: Ctrl+C / Ctrl+\ reach the game, while the launcher ignores them until the
  // game exits (so it can report 128 + signal). The child gets default SIGINT/SIGQUIT/SIGPIPE (ignored dispositions
  // survive exec) and an empty signal mask.
  struct sigaction ign{}, old_int{}, old_quit{};
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  ::sigaction(SIGINT, &ign, &old_int);
  ::sigaction(SIGQUIT, &ign, &old_quit);
  struct RestoreSignals {
    const struct sigaction &i, &q;
    ~RestoreSignals() {
      ::sigaction(SIGINT, &i, nullptr);
      ::sigaction(SIGQUIT, &q, nullptr);
    }
  } restore{old_int, old_quit};

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t def, mask;
  sigemptyset(&def);
  sigaddset(&def, SIGPIPE);
  sigaddset(&def, SIGINT);
  sigaddset(&def, SIGQUIT);
  sigemptyset(&mask);
  int rc = posix_spawnattr_setsigdefault(&attr, &def);
  if (rc == 0) rc = posix_spawnattr_setsigmask(&attr, &mask);
  if (rc == 0) rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
  pid_t pid = -1;
  if (rc == 0) rc = ::posix_spawn(&pid, game.c_str(), nullptr, &attr, argv.data(), environ);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) return Error{ErrorCode::IoError, "cannot start " + game.string() + ": " + std::strerror(rc), ""};

  int st = 0;
  while (::waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return Error{ErrorCode::IoError, std::string("waitpid: ") + std::strerror(errno), ""};
  }
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return WEXITSTATUS(st);
}

}  // namespace lr::pipeline
