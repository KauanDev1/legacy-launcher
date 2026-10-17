# AGENTS.md

LegacyRecomp statically recompiles Minecraft: Xbox 360 Edition to a native Linux executable (C++20, CMake, Clang) using hedge-dev's XenonRecomp. The user supplies their own legally obtained game ISO; no game data is ever distributed.

## Build and test

```
sudo pacman -S --needed clang cmake ninja sdl2 pkgconf   # toolchain
cmake --preset dev                                        # configure (Debug, ASan+UBSan)
cmake --build --preset dev
ctest --preset dev -L unit                                # fast tests, no game data needed
LEGACYRECOMP_ISO=/path/to/game.iso ctest --preset dev -L integration
```

On Windows or macOS (or without a local toolchain) use the Docker dev container: `tools/dev/container.sh test` runs the unit tests, and `shell`, `build`, `install --iso`, `run` are also available (see `docs/development.md`).

Requirements: x86-64 CPU with AVX (the game is compiled with `-march=sandybridge`; `doctor` checks it), Clang ≥ 18, CMake ≥ 3.25, Ninja, SDL2, 8 GiB free disk.

Release build and usage:

```
cmake --preset release && cmake --build --preset release
build/release/bin/legacyrecomp install --iso /path/to/game.iso   # CLI: doctor..build, cached via work/state.toml
build/release/bin/legacyrecomp run                               # exit 3 = first unimplemented kernel import (expected in M1)
build/release/bin/legacyrecomp-gui                               # GUI launcher (see docs/gui.md)
packaging/appimage/build.sh                                      # AppImage + source archive in packaging/appimage/out (docs/packaging.md)
```

Pass `--root <dir>` (or `LEGACYRECOMP_ROOT`) to choose the data root; use a disk, never a tmpfs. Binaries land in `build/<preset>/bin`. Only Clang is supported (CMake refuses other compilers). Our targets get strict flags through `lr_configure_target()` in `cmake/LrWarnings.cmake`; never apply it to submodules.

Include root is `src/`: write `#include "common/result.h"`.

## Module map (dependency direction: top depends on bottom)

```
launcher (cli, gui)
   |-> pipeline  (stages: doctor, extract, verify, patch, analyse, recompile, build)
   |-> runtime   (guest memory, loader, kernel/ imports, threads)
pipeline -> formats (xdvdfs, stfs, xex2) -> common
runtime  -> formats (xex2 only), common
common: Result/Error, log, sha256, paths, fs_util
thirdparty: XenonRecomp (and its bundled tomlplusplus/fmt/xxHash), Catch2, imgui
```

Namespaces: `lr`, `lr::formats`, `lr::pipeline`, `lr::runtime`, `lr::kernel`.

## Inviolable rules

- Public APIs return `lr::Result<T>`; no exceptions cross module boundaries (only two exceptions, both thrown and caught inside the runtime: `lr::kernel::UnimplementedImport` for a missing import, and `lr::kernel::GuestThreadExit`, thrown by `ExTerminateThread` and caught at the top of the guest thread; see `docs/adr/0004-m2-plan-deviations.md`).
- Never commit `*.iso`, `*.xex`, `*.xexp`, `private/`, `work/`, generated `ppc_*` files. Before every commit: `git status --short` must show none of them.
- Every commit passes `ctest --preset dev -L unit`. Conventional Commits, one commit per task step marked "Commit", ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- External tools are launched with `posix_spawn` and an argv vector, never through a shell.

Also: MIT license (changed from GPL-3.0 to MIT on 2026-10-14 by the project owner), see `docs/adr/0002-license.md`; every new source file starts with `// SPDX-License-Identifier: MIT` (`#` comment in CMake/Python/shell); never copy code from UnleashedRecomp (GPL-3.0), use it only as a reference; code, identifiers and commit messages in English, `docs/` prose in Portuguese.

## Docs

Map: `docs/architecture.md`, `docs/pipeline.md`, `docs/gui.md`, `docs/packaging.md` (AppImage), `docs/roadmap.md`, `docs/kernel-coverage.md` (regenerated from a real build, see its header), `docs/adr/`, `docs/formats/`, `docs/reverse-engineering/`.

Live in `docs/` (design specs and plans under `docs/superpowers/`, ADRs in `docs/adr/`, format notes in `docs/formats/`). Docs change in the same commit as the code they describe.

## Reverse-engineering workflow

See `.claude/skills/xbox360-re/`.
