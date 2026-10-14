# LegacyRecomp M1 — Pipeline + Runtime Skeleton Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A launcher (CLI + GUI) that turns the user's Minecraft X360 ISO into a native Linux
executable via XenonRecomp, plus a runtime skeleton that runs the recompiled entry point until
the first unimplemented kernel import.

**Architecture:** Five units with one-way dependencies: `common` ← `formats` ← `pipeline` ←
(`cli`, `gui`), and `common` ← `runtime`. The pipeline is a list of cached, resumable stages that
drives the XenonRecomp tools (git submodule) and finally builds a CMake "game template" against the
prebuilt runtime libraries. Nothing from the game ever enters the repository.

**Tech Stack:** C++20, Clang ≥ 18, CMake ≥ 3.25 + Ninja, SDL2 (system package), Dear ImGui,
tomlplusplus, Catch2 v3, XenonRecomp/XenonAnalyse/XenonUtils (hedge-dev).

**Spec:** `docs/superpowers/specs/2026-10-14-legacyrecomp-m1-design.md`

## Global Constraints

- License GPL-3.0; every new source file starts with `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Namespace `lr` (`lr::formats`, `lr::pipeline`, `lr::runtime`, `lr::kernel`); code, identifiers, commit messages in English; `docs/` prose in Portuguese.
- Public APIs return `lr::Result<T>`; no exceptions cross module boundaries (sole exception: `lr::kernel::UnimplementedImport`, thrown and caught inside the runtime).
- Never commit `*.iso`, `*.xex`, `*.xexp`, `private/`, `work/`, generated `ppc_*` files. Before every commit: `git status --short` must show none of them.
- Every commit passes `ctest --preset dev -L unit`. Conventional Commits, one commit per task step marked "Commit", ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- External tools are launched with `posix_spawn` and an argv vector — never through a shell.
- Target version: XBLA package XEX + TU1; title id `584111F7`; ISO game partition XGD2 at `0xFD90000`.
- Data root: `$XDG_DATA_HOME/legacyrecomp` (default `~/.local/share/legacyrecomp`), overridable with `--root`; subdirs `game/`, `work/`, `work/logs/`, `bin/`.
- Resources dir (tools, config, runtime libs, template) has the same layout in build tree and install prefix: `<prefix>/share/legacyrecomp/`; executables in `<prefix>/bin/`.
- Spec deviations (documented in `docs/adr/0003-m1-plan-deviations.md`, Task 16): SDL2 is a system dependency, not a submodule; pipeline state is `state.toml`, not `state.json`; GUI file picking uses drag-and-drop + `zenity`/`kdialog` (SDL2 has no file dialog); game paths+hashes live in `config/minecraft-tu1/game.toml` (not `hashes.toml`); guest memory is one `MAP_NORESERVE` RW mapping (kernel commits lazily) instead of PROT_NONE + manual commit; the import-stub generator reads the generated `ppc_recomp_shared.h` instead of the XEX import table (names already resolved by XenonRecomp).

## Review Focus

1. **ISO path with spaces and parentheses** (the real file is `Minecraft - Xbox 360 Edition (World).iso`) → every stage and tool invocation works unchanged. Test: Task 8 `run_tool passes args with spaces and parens verbatim`.
2. **Install interrupted mid-stage (Ctrl+C, cancel, crash)** → rerun redoes that stage; partial outputs are never treated as done. Test: Task 7 `failed or cancelled stage is not marked complete`.
3. **Wrong disc / other region / modified files** → `UnsupportedVersion` naming the file and the found hash, never a crash in a later stage. Test: Task 9 `verify rejects mismatched hash`.
4. **Truncated or non-Xbox ISO** (incomplete download, random file) → `InvalidFormat`/`OutOfRange` with hint, no out-of-bounds read. Tests: Task 3 `truncated image`, `random bytes`.
5. **Not enough disk space or missing toolchain** → `doctor` fails before extraction starts, with the exact install command. Test: Task 9 `doctor reports missing clang with pacman hint` and `doctor fails below 8 GiB free`.

---

## File Structure

```
CMakeLists.txt  CMakePresets.json  LICENSE  README.md  AGENTS.md  CLAUDE.md
cmake/LrWarnings.cmake                 # shared compile flags
thirdparty/{XenonRecomp,imgui,Catch2}  # submodules (tomlplusplus reused from XenonRecomp)
config/minecraft-tu1/game.toml         # source paths inside ISO + SHA-256 hashes
config/minecraft-tu1/recomp.toml       # XenonRecomp addresses (found by RE)
src/common/   result.h error.h error.cpp log.h log.cpp sha256.h sha256.cpp paths.h paths.cpp fs_util.h fs_util.cpp
src/formats/  reader.h reader.cpp xdvdfs.h xdvdfs.cpp stfs.h stfs.cpp xex2.h xex2.cpp
src/pipeline/ types.h stage.h runner.h runner.cpp state.h state.cpp process.h process.cpp
              game_config.h game_config.cpp doctor.h doctor.cpp
              stages/{doctor,extract,verify,patch,analyse,recompile,build}_stage.{h,cpp}
              import_stubs.h import_stubs.cpp pipeline.h pipeline.cpp
src/launcher/cli/ main.cpp progress_bar.h progress_bar.cpp
src/launcher/gui/ main.cpp app.h app.cpp queue_sink.h queue_sink.cpp
src/runtime/  guest_memory.h guest_memory.cpp loader.h loader.cpp func_table.h func_table.cpp
              kernel/registry.h kernel/registry.cpp fault.h fault.cpp guest_thread.h guest_thread.cpp
              main.cpp                     # -> lr_runtime_main (linked only by the game template)
              template/CMakeLists.txt      # game template
tests/        CMakeLists.txt support/{xdvdfs_builder,stfs_builder,xex2_builder}.h + test_*.cpp
tools/find_helpers.py                  # scans patched image for XenonRecomp helper prologues
.claude/skills/xbox360-re/SKILL.md
docs/ architecture.md pipeline.md roadmap.md kernel-coverage.md formats/*.md
      reverse-engineering/{README.md,addresses.md} adr/000{1,2,3}-*.md
```

---

### Task 1: Repository scaffold, common error types, agent docs

**Files:**
- Create: `CMakeLists.txt`, `CMakePresets.json`, `cmake/LrWarnings.cmake`, `LICENSE` (GPL-3.0 full text), `AGENTS.md`, `CLAUDE.md`, `src/common/{result.h,error.h,error.cpp,log.h,log.cpp}`, `src/common/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/test_result.cpp`
- Add submodules: `thirdparty/Catch2` (tag v3.x latest), `thirdparty/XenonRecomp` (`--recursive`), `thirdparty/imgui` (tag v1.91.x latest)

**Interfaces:**
- Produces:
  - `enum class lr::ErrorCode { InvalidFormat, OutOfRange, IoError, NotFound, UnsupportedVersion, ToolMissing, ToolFailed, InsufficientResources, Cancelled, UnimplementedImport, Internal };`
  - `struct lr::Error { ErrorCode code; std::string message; std::string hint; };` and `std::string_view lr::to_string(ErrorCode)`
  - `template<class T> class lr::Result` (+ `Result<void>`): implicit from `T` and from `Error`; `explicit operator bool`, `bool ok()`, `T& value()`, `const Error& error()`. Macro `LR_TRY(expr)` returns the error from the enclosing function on failure; `LR_TRY_ASSIGN(var, expr)`.
  - `lr::log::info/warn/error(std::string_view)` writing `[LEVEL] msg` to stderr; `lr::log::set_sink(std::function<void(Level, std::string_view)>)`.
  - CMake targets: `lr_common`; test executable `lr_tests`; Catch2 tags become ctest labels (`catch_discover_tests(lr_tests ADD_TAGS_AS_LABELS)`), so `[unit]` → label `unit`, `[integration]` → label `integration`.
  - Presets: `dev` (Debug, Ninja, clang/clang++, `build/dev`, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, `CMAKE_RUNTIME_OUTPUT_DIRECTORY=${binaryDir}/bin`) and `release` (`build/release`, RelWithDebInfo). Test preset `dev`.

- [ ] **Step 1:** `git checkout -b feat/m1-scaffold`; add the three submodules; pin each to its current HEAD/tag and run `git submodule update --init --recursive`.
- [ ] **Step 2: Write the failing test** `tests/test_result.cpp`, tag `[unit]`:
  - `Result<int> ok = 5;` → `ok.ok()`, `ok.value() == 5`.
  - `Result<int> bad = Error{ErrorCode::NotFound, "x", "y"};` → `!bad`, `bad.error().code == ErrorCode::NotFound`, `bad.error().hint == "y"`.
  - A function using `LR_TRY` on a failing `Result<void>` returns that same error; on success continues.
  - `to_string(ErrorCode::UnsupportedVersion) == "UnsupportedVersion"`.
- [ ] **Step 3:** `cmake --preset dev && cmake --build --preset dev && ctest --preset dev -L unit` → FAIL (does not compile: `lr/result.h` missing).
- [ ] **Step 4:** Implement `src/common/*`. Top-level CMake: `cmake_minimum_required(VERSION 3.25)`, `CMAKE_CXX_STANDARD 20`, refuses non-Clang compilers with a clear `FATAL_ERROR`, `add_subdirectory(thirdparty/XenonRecomp)` (exposes targets `XenonAnalyse`, `XenonRecomp`, `XenonUtils`; check its CMakeLists for the exact names and for the tomlplusplus target it already defines — reuse that, do not add a second tomlplusplus), `add_subdirectory(thirdparty/Catch2)`, `src/`, `tests/`. `cmake/LrWarnings.cmake` defines `lr_configure_target(target)` = `-Wall -Wextra -Wpedantic -Werror`, plus `-fsanitize=address,undefined` in Debug, applied only to our targets (never to submodules).
- [ ] **Step 5:** Re-run build + `ctest --preset dev -L unit` → PASS.
- [ ] **Step 6:** Write `AGENTS.md` (canonical): project one-liner; build/test commands (`cmake --preset dev`, `cmake --build --preset dev`, `ctest --preset dev -L unit`, `LEGACYRECOMP_ISO=... ctest --preset dev -L integration`); toolchain install line `sudo pacman -S --needed clang lld llvm cmake ninja sdl2`; module map + dependency direction; inviolable rules (Global Constraints bullets 3–6 verbatim); where docs live and that docs change in the same commit as code; RE workflow pointer to `.claude/skills/xbox360-re/`. Write `CLAUDE.md` = `@AGENTS.md` plus a Claude-specific section: use superpowers skills, specs in `docs/superpowers/specs`, plans in `docs/superpowers/plans`, never read/dump large binary game data into context (use `tools/` scripts).
- [ ] **Step 7: Commit** — `build: scaffold CMake project, submodules, common Result/Error, agent docs`.

---

### Task 2: SHA-256, data-root paths, atomic file writes

**Files:**
- Create: `src/common/{sha256.h,sha256.cpp,paths.h,paths.cpp,fs_util.h,fs_util.cpp}`, `tests/test_sha256.cpp`, `tests/test_paths.cpp`

**Interfaces:**
- Produces:
  - `class lr::Sha256 { void update(std::span<const std::byte>); std::array<uint8_t,32> finish(); };`, `std::string lr::to_hex(std::span<const uint8_t>)`, `lr::Result<std::string> lr::sha256_file(const std::filesystem::path&)` (lowercase hex, streams 1 MiB chunks).
  - `struct lr::DataPaths { fs::path root, game, work, logs, bin, input, image, analysis, generated, build, state_file; };` with `static DataPaths lr::DataPaths::from_root(fs::path)`. Subpaths: `game/`, `work/`, `work/logs/`, `bin/`, `work/input/`, `work/image/`, `work/analysis/`, `work/generated/`, `work/build/`, `work/state.toml`.
  - `fs::path lr::default_data_root(const char* xdg_data_home, const char* home)` (pure; callers pass `getenv` results).
  - `lr::Result<fs::path> lr::resources_dir(const fs::path& exe_path, const char* env_override)` → env if set and exists, else `exe_path.parent_path()/"../share/legacyrecomp"` if it exists, else `NotFound` with hint `set LEGACYRECOMP_RESOURCES`.
  - `lr::Result<void> lr::atomic_write(const fs::path&, std::span<const std::byte>)` (write `<path>.tmp`, fsync, rename) and `lr::Result<void> lr::atomic_copy_from(const fs::path& dst, const std::function<lr::Result<void>(std::ostream&)>& writer)`.

- [ ] **Step 1: Failing tests** `[unit]`:
  - SHA-256 of `""` = `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`; of `"abc"` = `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad`; of 1,000,000 × `'a'` fed in 7-byte chunks = `cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0`.
  - `default_data_root("/x", "/h") == "/x/legacyrecomp"`; `default_data_root(nullptr, "/h") == "/h/.local/share/legacyrecomp"`; empty string XDG treated as unset.
  - `DataPaths::from_root("/r").generated == "/r/work/generated"`.
  - `atomic_write` to a temp dir leaves no `.tmp` file and the content round-trips; when the writer of `atomic_copy_from` fails, the destination does not exist.
- [ ] **Step 2:** Run → FAIL (missing headers).
- [ ] **Step 3:** Implement (own SHA-256 per FIPS 180-4; no OpenSSL).
- [ ] **Step 4:** Run `ctest --preset dev -L unit` → PASS.
- [ ] **Step 5: Commit** — `feat(common): add sha256, data paths and atomic writes`.

---

### Task 3: Readers and XDVDFS image

**Files:**
- Create: `src/formats/{reader.h,reader.cpp,xdvdfs.h,xdvdfs.cpp,CMakeLists.txt}`, `tests/support/xdvdfs_builder.h`, `tests/test_xdvdfs.cpp`, `docs/formats/xdvdfs.md`

**Interfaces:**
- Consumes: `lr::Result`, `lr::Error`.
- Produces:
  - `struct lr::formats::IReader { virtual ~IReader(); virtual Result<void> read(uint64_t offset, std::span<std::byte> dst) = 0; virtual uint64_t size() const = 0; };` — reads past `size()` return `OutOfRange`.
  - `class FileReader : IReader { static Result<std::unique_ptr<FileReader>> open(const fs::path&); }` (pread), `class SpanReader : IReader { explicit SpanReader(std::span<const std::byte>); }`, `class SubReader : IReader { SubReader(IReader& parent, uint64_t offset, uint64_t size); }`.
  - `Result<std::vector<std::byte>> read_all(IReader&)`.
  - `struct XdvdfsEntry { std::string name; bool is_dir; uint32_t sector; uint32_t size; };`
  - `class XdvdfsImage { static Result<XdvdfsImage> open(IReader&); uint64_t partition_offset() const; Result<std::vector<XdvdfsEntry>> list(std::string_view dir_path) const; Result<std::unique_ptr<IReader>> open_file(std::string_view path) const; }` — paths use `/`, lookup is case-insensitive, `""`/`"/"` is root.
  - Library target `lr_formats`.

Format facts (also go in `docs/formats/xdvdfs.md`): sector = 2048 B. Probe partition offsets in order `0x0` (plain XISO), `0xFD90000` (XGD2), `0x2080000` (XGD3), `0x18300000` (XGD1); valid when the 20 bytes at `partition + 32*2048` equal `MICROSOFT*XBOX*MEDIA`. In that header sector: root dir sector `u32 LE @0x14`, root dir size `u32 LE @0x18`. Directory = binary tree of entries: `u16 left, u16 right` (offsets in 4-byte units from dir start; 0 = none), `u32 sector, u32 size, u8 attrs (0x10 = dir), u8 name_len, name[name_len]`; an entry with `left == 0xFFFF` is padding. Traverse the whole tree (do not rely on ordering); guard against cycles with a visited set and against offsets beyond the directory size.

- [ ] **Step 1: Failing tests** `[unit]` using `xdvdfs_builder.h` (builds an in-memory XGD2-offset image from `{path, bytes}` pairs; its directory tree is a right-linked chain, valid for the format):
  - `open` detects partition `0xFD90000`; `list("/")` returns `Default.xex` and dir `Content`; `open_file("content/A/b.bin")` (different case) returns the exact bytes.
  - `open_file("missing")` → `NotFound`.
  - `truncated image`: builder output cut in the middle of the root directory → `open` or `list` returns `OutOfRange`/`InvalidFormat`, never crashes (runs under ASan/UBSan via the dev preset).
  - `random bytes`: 4 MiB of `0xAB` → `InvalidFormat` with hint containing `"not an Xbox 360 disc image"`.
  - `cyclic tree`: entry whose `right` points to itself → `InvalidFormat`.
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run → PASS.
- [ ] **Step 5:** Write `docs/formats/xdvdfs.md` (facts above + our partition findings for this ISO). **Commit** — `feat(formats): add readers and XDVDFS image parser`.

---

### Task 4: STFS package reader

**Files:**
- Create: `src/formats/{stfs.h,stfs.cpp}`, `tests/support/stfs_builder.h`, `tests/test_stfs.cpp`, `docs/formats/stfs.md`

**Interfaces:**
- Consumes: `IReader`, `SubReader`, `Result`.
- Produces:
  - `struct StfsHeader { std::string magic; uint32_t content_type; uint32_t title_id; std::u16string display_name; bool read_only; };`
  - `struct StfsEntry { std::string path; bool is_dir; uint32_t size; uint32_t start_block; bool consecutive; };`
  - `class StfsPackage { static Result<StfsPackage> open(IReader&); const StfsHeader& header() const; const std::vector<StfsEntry>& entries() const; Result<std::unique_ptr<IReader>> open_file(std::string_view path) const; static uint64_t block_to_offset(uint32_t block, uint32_t header_size, bool read_only); }` — `entries()` paths are full (`dir/file`), built from each entry's parent index.

Format facts (all offsets from package start; put them in `docs/formats/stfs.md`): magic `"CON "`/`"LIVE"`/`"PIRS"` @0; header size `u32 BE @0x340`; content type `u32 BE @0x344`; title id `u32 BE @0x360`; display name UTF-16BE @0x411 (0x80 bytes); volume descriptor @0x379: flags byte @0x37B (bit0 = read-only format), file-table block count `u16 LE @0x37C`, file-table first block `int24 LE @0x37E`. File table entries are 0x40 bytes: name[0x28] (NUL-padded), flags @0x28 (bits0-5 name length, bit6 consecutive, bit7 dir), blocks `int24 LE @0x29`, start block `int24 LE @0x2F`, parent index `i16 BE @0x32` (−1 = root), size `u32 BE @0x34`; an entry with name length 0 ends the table. Block size 0x1000. Block → offset (Xenia `StfsContainerDevice` algorithm; confirm against its source when implementing):

```
shift = read_only ? 0 : 1
blk = block; base = 0xAA
for level in 0..2:
    blk += ((block + base) / base) << shift
    if block < base: break
    base *= 0xAA
offset = round_up(header_size, 0x1000) + blk * 0x1000
```

Non-consecutive files and file tables: return `InvalidFormat` with message `"non-consecutive STFS files are not supported yet"`; Task 6 confirms whether our two packages need it (if they do, implementing the hash-table chain becomes a follow-up task added then).

- [ ] **Step 1: Failing tests** `[unit]`:
  - `block_to_offset(0, 0x971A, true) == 0xB000`; `(169, 0x971A, true) == 0xA000 + 170*0x1000`; `(170, 0x971A, true) == 0xA000 + 173*0x1000`; `(0, 0x971A, false) == 0xA000 + 2*0x1000`.
  - Builder package (`LIVE`, content type `0x000D0000`, title `0x584111F7`, files `default.xex` (5000 B, spans 2 blocks), `data/x.bin`) → header fields match; `entries()` contains `"data/x.bin"`; `open_file("default.xex")` bytes equal input.
  - Bad magic → `InvalidFormat`; parent index pointing past table → `InvalidFormat`; file whose blocks run past the reader → `OutOfRange`.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5: Commit** — `feat(formats): add STFS package reader`.

---

### Task 5: XEX2 header parser

**Files:**
- Create: `src/formats/{xex2.h,xex2.cpp}`, `tests/support/xex2_builder.h`, `tests/test_xex2.cpp`, `docs/formats/xex2.md`

**Interfaces:**
- Produces:
  - `struct Xex2ExecutionInfo { uint32_t media_id, version, base_version, title_id; uint8_t platform, executable_type, disc_number, disc_count; };`
  - `struct Xex2Info { uint32_t module_flags; uint32_t pe_data_offset; std::optional<uint32_t> entry_point, image_base; std::optional<Xex2ExecutionInfo> execution; std::vector<std::string> import_libraries; };`
  - `Result<Xex2Info> parse_xex2(std::span<const std::byte> header_bytes)`; `std::string format_version(uint32_t)` → `"major.minor.build.qfe"` (bits 31-28, 27-24, 23-8, 7-0).

Facts: magic `"XEX2"` @0, all fields BE; module flags @4, PE data offset @8, security info offset @0x10, optional header count @0x14, then `{u32 key, u32 value}` pairs. Key low byte `0x00`/`0x01` → value inline; `0xFF` → value is offset to a block starting with its u32 size; otherwise offset to `(key & 0xFF) * 4` bytes. Keys: entry point `0x00010100`, image base `0x00010201`, import libraries `0x000103FF` (block: size, string-table size, library count, then string table of NUL-terminated, 4-byte-aligned names), execution info `0x00040006` (media id, version, base version, title id, then platform, executable type, disc number, disc count as bytes).

- [ ] **Step 1: Failing tests** `[unit]`: builder XEX with entry `0x82500000`, base `0x82000000`, execution info title `0x584111F7` version `0x01000100`, libraries `xboxkrnl.exe`, `xam.xex` → all fields round-trip; `format_version(0x01000100) == "0.1.1.0"`; wrong magic → `InvalidFormat`; optional header count claiming more entries than bytes → `OutOfRange`.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5: Commit** — `feat(formats): add XEX2 header parser`.

---

### Task 6: `legacyrecomp inspect` + first contact with the real ISO

**Files:**
- Create: `src/launcher/cli/main.cpp` (only `inspect` for now), `src/launcher/cli/CMakeLists.txt`, `config/minecraft-tu1/game.toml`, `tests/test_integration_formats.cpp`
- Modify: `docs/formats/{stfs,xex2}.md` (findings), `docs/reverse-engineering/README.md` (create)

**Interfaces:**
- Consumes: `FileReader`, `XdvdfsImage`, `StfsPackage`, `parse_xex2`, `sha256`.
- Produces:
  - Executable `legacyrecomp` with `legacyrecomp inspect --iso <path>`: prints XDVDFS tree (depth ≤ 4), for each STFS package found under `Content/` and `$TitleUpdate/` its header and entries, and XEX2 info + SHA-256 for every `*.xex`/`*.xexp` (disc root and inside packages).
  - `config/minecraft-tu1/game.toml`:
    ```toml
    title_id = 0x584111F7
    [sources]
    package = "Content/0000000000000000/584111F7/000D0000/49AAD81B9FCDA45E4A03D71BFCB353F8FADB236C58"
    title_update = "$TitleUpdate/584111F7/tu00000001_00000000"
    package_xex = "<name found in step 2, e.g. default.xex>"
    tu_xexp = "<name found in step 2, e.g. default.xexp>"
    [hashes]  # sha256, lowercase hex
    package_xex = "<from step 2>"
    tu_xexp = "<from step 2>"
    patched_image = ""  # filled in Task 10
    ```

- [ ] **Step 1:** Implement `inspect`; build.
- [ ] **Step 2:** Run `./build/dev/bin/legacyrecomp inspect --iso "Minecraft - Xbox 360 Edition (World).iso" | tee "$SCRATCH/inspect.txt"` (scratch dir outside the repo). Expected: both STFS packages open; title ids `0x584111F7`; one XEX inside the `000D0000` package, one `.xexp` inside the TU. If STFS returns the non-consecutive error, stop and add a task to implement hash-chain traversal before continuing.
- [ ] **Step 3:** Fill `game.toml` from the output. Record in `docs/reverse-engineering/README.md`: whether disc `Default.xex` is a loader (compare its imports/size/entry with the package XEX), package XEX version/base version vs. xexp, import library list. Hashes and names only — no game bytes.
- [ ] **Step 4: Integration test** `[integration]` in `test_integration_formats.cpp`: skips (`SKIP("LEGACYRECOMP_ISO not set")`) without the env var; otherwise opens the ISO, extracts the package XEX into memory, asserts its first 4 bytes are `XEX2` and its SHA-256 equals `game.toml`'s `package_xex`. Run `LEGACYRECOMP_ISO="$PWD/Minecraft - Xbox 360 Edition (World).iso" ctest --preset dev -L integration` → PASS; without the env → reported as skipped.
- [ ] **Step 5: Commit** — `feat(cli): add inspect command; pin game sources and hashes`.

---

### Task 7: Pipeline core — stages, state, runner

**Files:**
- Create: `src/pipeline/{types.h,stage.h,state.h,state.cpp,runner.h,runner.cpp,game_config.h,game_config.cpp,CMakeLists.txt}`, `tests/test_runner.cpp`, `tests/test_game_config.cpp`

**Interfaces:**
- Consumes: `DataPaths`, `Result`, `sha256_file`, tomlplusplus.
- Produces:
  - `enum class StageId { Doctor, Extract, Verify, Patch, Analyse, Recompile, Build };` `std::string_view stage_name(StageId)` (`"doctor"`, …) and `std::optional<StageId> parse_stage(std::string_view)`.
  - `struct Progress { StageId stage; float fraction; std::string message; };`
  - `struct ProgressSink { virtual ~ProgressSink(); virtual void on_progress(const Progress&) = 0; virtual void on_log(std::string_view line) = 0; virtual bool cancelled() const = 0; };`
  - `struct Options { fs::path iso; fs::path root; std::set<StageId> force; unsigned jobs = 0; };` (0 = hardware concurrency)
  - `struct GameConfig { uint32_t title_id; std::string package, title_update, package_xex, tu_xexp; std::string hash_package_xex, hash_tu_xexp, hash_patched_image; fs::path recomp_toml; };` `Result<GameConfig> load_game_config(const fs::path& dir)` (reads `game.toml`, sets `recomp_toml = dir/"recomp.toml"`).
  - `struct StageContext { const Options& options; DataPaths paths; fs::path resources; const GameConfig& game; ProgressSink& sink; };`
  - `struct Stage { virtual ~Stage(); virtual StageId id() const = 0; virtual Result<std::string> fingerprint(StageContext&) = 0; virtual std::vector<fs::path> outputs(StageContext&) const = 0; virtual Result<void> execute(StageContext&) = 0; };`
  - `class StateStore { static Result<StateStore> load(const fs::path&); bool is_complete(StageId, std::string_view fingerprint) const; void mark_started(StageId); void mark_complete(StageId, std::string fingerprint); Result<void> save() const; };` stored as `state.toml` (`[stages.<name>] complete = bool, fingerprint = "..."`), saved via `atomic_write` after every change.
  - `Result<void> run_stages(std::span<Stage* const>, StageContext&, StateStore&)`.

Runner rules: a stage is skipped iff `is_complete(id, fingerprint)` and every `outputs()` path exists and neither it nor any earlier stage is in `options.force` nor was re-executed in this run. Before `execute`: `mark_started` + `save`. On success: `mark_complete` + `save`. Check `sink.cancelled()` before each stage → `Cancelled`. Progress events: a skipped stage emits exactly `{id, 1.0f, "cached"}`; an executed stage emits `{id, 0.0f, "start"}` first and `{id, 1.0f, "done"}` on success. On failure the stage stays incomplete and the error is returned unchanged.

- [ ] **Step 1: Failing tests** `[unit]` with a `FakeStage` (configurable fingerprint, output file it writes, failure toggle, execution counter) and a recording sink:
  - first run executes all 3 fakes; second run executes none and the sink receives three `"cached"` events and no `"start"` event.
  - changing stage 2's fingerprint re-executes 2 and 3, not 1.
  - deleting stage 1's output re-executes 1, 2, 3.
  - `force = {stage 2}` re-executes 2 and 3.
  - `failed or cancelled stage is not marked complete`: stage 2 fails → returns its error, stage 3 not run, next run (fixed) executes 2 and 3 but not 1; sink `cancelled()` true before stage 2 → `Cancelled`, stage 2 re-runs next time.
  - `state.toml` survives a reload (`StateStore::load` sees completion).
  - `load_game_config` on the repo's `config/minecraft-tu1` returns `title_id == 0x584111F7`; missing file → `NotFound`.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5: Commit** — `feat(pipeline): add stage runner with cached, resumable state`.

---

### Task 8: External process runner

**Files:**
- Create: `src/pipeline/{process.h,process.cpp}`, `tests/test_process.cpp`

**Interfaces:**
- Produces:
  - `struct ToolInvocation { fs::path program; std::vector<std::string> args; fs::path cwd; fs::path log_file; std::function<bool()> cancelled; std::function<void(std::string_view)> on_line; };`
  - `Result<void> run_tool(const ToolInvocation&)` — `posix_spawn`, stdout+stderr merged into a pipe; each line appended to `log_file` and passed to `on_line`; polls every 100 ms; when `cancelled()` → `SIGTERM`, then `SIGKILL` after 3 s, returns `Cancelled`. Exit ≠ 0 → `ToolFailed`, message `"<program> exited with <code>"`, hint `"see <log_file>"` followed by the last 20 log lines.
  - `Result<std::string> capture_output(const fs::path& program, const std::vector<std::string>& args)` (stdout only, for version probes and `zenity`).
  - `std::optional<fs::path> find_program(std::string_view name)` (searches `PATH`).

- [ ] **Step 1: Failing tests** `[unit]` (use `/bin/sh -c` only as the *program under test*, not to run our tools):
  - `run_tool passes args with spaces and parens verbatim`: program `/usr/bin/printf`, args `{"%s\n", "Minecraft - Xbox 360 Edition (World).iso"}` → log file contains exactly that line.
  - `/bin/sh -c 'for i in $(seq 1 30); do echo line$i; done; exit 7'` → `ToolFailed`; message contains `exited with 7`; hint contains `line30` and `line11` but not `line10`.
  - `sleep 30` with `cancelled` returning true after 200 ms → `Cancelled` within 4 s.
  - `find_program("sh")` has value; `find_program("definitely-not-a-program-xyz")` is empty.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5: Commit** — `feat(pipeline): add posix_spawn tool runner with logs and cancellation`.

---

### Task 9: Stages `doctor`, `extract`, `verify`

**Files:**
- Create: `src/pipeline/{doctor.h,doctor.cpp}`, `src/pipeline/stages/{doctor_stage,extract_stage,verify_stage}.{h,cpp}`, `tests/test_doctor.cpp`, `tests/test_extract_verify.cpp`

**Interfaces:**
- Consumes: `Stage`, `StageContext`, `run_tool`/`capture_output`/`find_program`, `XdvdfsImage`, `StfsPackage`, `atomic_copy_from`, `sha256_file`.
- Produces:
  - `struct DoctorProbe { virtual std::optional<fs::path> find(std::string_view) = 0; virtual Result<std::string> version_output(const fs::path&) = 0; virtual Result<uint64_t> free_bytes(const fs::path&) = 0; virtual uint64_t total_ram() = 0; virtual bool pkg_config_exists(std::string_view) = 0; };` + `SystemDoctorProbe`.
  - `struct DoctorReport { std::vector<std::string> warnings; };` `Result<DoctorReport> run_doctor(DoctorProbe&, const fs::path& root)`. Requirements: `clang++` ≥ 18, `cmake` ≥ 3.25, `ninja` present, `pkg-config sdl2` exists, free space at root (or nearest existing parent) ≥ 8 GiB → else `InsufficientResources`. RAM < 8 GiB → warning only. Missing tool → `ToolMissing`, hint `sudo pacman -S --needed clang lld llvm cmake ninja sdl2` (plus `apt`/`dnf` equivalents on separate lines). Version parse: first `(\d+)\.(\d+)(?:\.(\d+))?` in the output.
  - `std::unique_ptr<Stage> make_doctor_stage(DoctorProbe&)`, `make_extract_stage()`, `make_verify_stage()`.
  - Extract: fingerprint = `iso path + size + mtime + game.toml hash`; outputs = `input/package.xex`, `input/tu.xexp`, `game/.extracted` marker. Writes package XEX → `work/input/package.xex`; every other package file → `game/<path>`; TU `.xexp` → `work/input/tu.xexp`; other TU files overlay `game/`. Progress = bytes written / total. Cancellation checked between files.
  - Verify: fingerprint = sha256 of both inputs; compares to `game.hash_package_xex` / `hash_tu_xexp`; mismatch → `UnsupportedVersion`, message names the file and the found hash, hint `"LegacyRecomp supports the Minecraft X360 disc (title 584111F7) with TU1; see docs/pipeline.md"`. Empty expected hash → `Internal` ("hash not configured").

- [ ] **Step 1: Failing tests** `[unit]`:
  - fake probe with everything OK → success; `doctor reports missing clang with pacman hint` (`find("clang++")` empty → `ToolMissing`, hint contains `pacman -S --needed clang`); clang output `"clang version 17.0.6"` → `ToolMissing`, message contains `18`; `doctor fails below 8 GiB free` → `InsufficientResources`; 4 GiB RAM → OK with 1 warning.
  - extract on an xdvdfs_builder ISO containing an stfs_builder package + TU (paths from a test `GameConfig`) → `work/input/package.xex` bytes match, `game/data/x.bin` exists, TU extra file overlays `game/`.
  - `verify rejects mismatched hash` → `UnsupportedVersion`, message contains the found hash; matching hashes → OK.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5: Commit** — `feat(pipeline): add doctor, extract and verify stages`.

---

### Task 10: Stages `patch`, `analyse`, `recompile` + reverse-engineered config

**Files:**
- Create: `src/pipeline/stages/{patch_stage,analyse_stage,recompile_stage}.{h,cpp}`, `src/pipeline/resources.cmake` (stages resources into `${CMAKE_BINARY_DIR}/share/legacyrecomp`), `config/minecraft-tu1/recomp.toml`, `tools/find_helpers.py`, `.claude/skills/xbox360-re/SKILL.md`, `docs/reverse-engineering/addresses.md`, `tests/test_recomp_config.cpp`, `tests/test_integration_pipeline.cpp`
- Modify: `config/minecraft-tu1/game.toml` (`patched_image` hash)

**Interfaces:**
- Consumes: XenonUtils `XexPatcher` (read `thirdparty/XenonRecomp/XenonUtils/xex_patcher.h` for the exact `apply` signature and result enum), `run_tool`, resources dir.
- Produces:
  - Resources staged at build: `tools/XenonAnalyse`, `tools/XenonRecomp`, `tools/ppc_context.h` (from XenonUtils), `config/minecraft-tu1/*`.
  - `make_patch_stage()`: `work/input/package.xex` + `tu.xexp` → `work/image/default_tu1.xex`; result hash must equal `game.hash_patched_image` (empty → `Internal`). Patcher failure → `InvalidFormat` with the patcher's result name.
  - `make_analyse_stage()`: runs `XenonAnalyse <image> <work/analysis/switch_tables.toml>`; log `logs/analyse.log`.
  - `Result<std::string> render_recomp_config(const std::string& template_toml, const fs::path& image, const fs::path& switch_tables, const fs::path& out_dir)` — parses the repo `recomp.toml`, overwrites `[main] file_path`, `switch_table_file_path`, `out_directory_path` with absolute paths, removes `patch_file_path`/`patched_file_path`, keeps everything else.
  - `make_recompile_stage()`: writes `work/recomp.toml` via `render_recomp_config`, clears `work/generated/`, runs `XenonRecomp work/recomp.toml <resources>/tools/ppc_context.h`; outputs `work/generated/ppc_recomp_shared.h`, `ppc_func_mapping.cpp`, `ppc_config.h`; progress driven by counting `ppc_recomp.*.cpp` lines in tool output. Fingerprint = image hash + `recomp.toml` hash + `LR_XENONRECOMP_COMMIT` (compile definition from `git -C thirdparty/XenonRecomp rev-parse HEAD`).
  - `tools/find_helpers.py <image>` (run with `python3 -I`): parses the image via XenonRecomp-compatible section info printed by `legacyrecomp inspect --sections`(add this flag), scans executable sections for these big-endian prologues and prints candidate addresses:
    - `savegprlr_14`: `F9C1FF68` (`std r14,-0x98(r1)`); `restgprlr_14`: `E9C1FF68` (`ld r14,-0x98(r1)`)
    - `savefpr_14`: `D9CCFF70` (`stfd f14,-0x90(r12)`); `restfpr_14`: `C9CCFF70` (`lfd f14,-0x90(r12)`)
    - `savevmx_14`: `3960FEE0 7DCB61CE`; `restvmx_14`: `3960FEE0 7DCB60CE`
    - `savevmx_64` / `restvmx_64`: `3960FC00` followed by a VMX128 `stvx128`/`lvx128` (confirm in Ghidra)

- [ ] **Step 1: Failing test** `[unit]` `render_recomp_config` on a sample TOML: output `file_path` is the absolute image path, `patch_file_path` absent, `restgprlr_14_address` preserved verbatim.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement the three stages + staging + `inspect --sections`. **Step 4:** Run → PASS.
- [ ] **Step 5 (RE):** Write `test_integration_pipeline.cpp` (`[integration]`): runs `doctor` through the stage named by env `LR_STOP_AFTER` (default `recompile`) into env `LEGACYRECOMP_TEST_ROOT` (default: a temp dir). Run it with `LR_STOP_AFTER=patch LEGACYRECOMP_TEST_ROOT=$SCRATCH/root` (it fails on the empty `patched_image` hash and prints the found hash); record `patched_image` hash in `game.toml`. Run `python3 -I tools/find_helpers.py work/image/default_tu1.xex`; confirm each address in Ghidra + XEXLoaderWV; locate `setjmp`/`longjmp` (CRT; find by their characteristic `mfcr`/`mflr` save sequence and callers) or document that they are absent. Write `recomp.toml` (`[main]` per XenonRecomp README: `file_path`, `out_directory_path`, `switch_table_file_path`, all helper `*_address` keys, `setjmp_address`/`longjmp_address` if present) and `docs/reverse-engineering/addresses.md` (table: name, address, how found, confirmed by).
- [ ] **Step 6:** Write `.claude/skills/xbox360-re/SKILL.md` (frontmatter `name: xbox360-re`, description "Use when finding addresses or fixing recompilation errors for the Xbox 360 XEX in LegacyRecomp"): tool setup (Ghidra + XEXLoaderWV), the prologue table above, how to read XenonRecomp errors (unresolved jump table → add `[[switch]]`/function boundary overrides in `recomp.toml`), rule: every address goes into `addresses.md` with its evidence; never paste game bytes into docs.
- [ ] **Step 7: Integration** — extend `test_integration_pipeline.cpp`: with the default `LR_STOP_AFTER`, asserts `ppc_func_mapping.cpp` exists and `ppc_config.h` defines `PPC_IMAGE_BASE`. Iterate on `recomp.toml` until XenonRecomp exits 0. Run `ctest --preset dev -L integration` → PASS.
- [ ] **Step 8: Commit** — `feat(pipeline): add patch, analyse and recompile stages with TU1 recomp config` (verify `git status` shows no `work/` or generated files).

---

### Task 11: Runtime core — guest memory, fault decoding, function table

**Files:**
- Create: `src/runtime/{guest_memory.h,guest_memory.cpp,fault.h,fault.cpp,func_table.h,func_table.cpp,be.h,CMakeLists.txt}`, `tests/test_runtime_core.cpp`

**Interfaces:**
- Produces:
  - `template<class T> struct lr::runtime::be` — stores big-endian, `operator T()`, assignment from `T` (integral types, `float`, `double` via bit_cast); `static_assert(sizeof(be<uint32_t>) == 4)`.
  - `class GuestMemory { static Result<GuestMemory> reserve(uint64_t bytes); uint8_t* base() const; uint64_t size() const; template<class T> T* translate(uint32_t guest) const; ~GuestMemory(); }` — one `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)`; move-only.
  - `std::optional<uint32_t> guest_address_of(const uint8_t* base, uint64_t size, const void* host)` and `std::string describe_fault(int signo, const uint8_t* base, uint64_t size, const void* host_addr)` → `"SIGSEGV at guest 0x82001234"` or `"SIGSEGV at host 0x... (outside guest memory)"`; `void install_fault_handlers(const GuestMemory&)` (SIGSEGV/SIGBUS/SIGILL; uses `describe_fault`, writes with `write(2)`, `_exit(4)`).
  - `struct FuncMapping { uint32_t guest; void* host; };` `void install_function_table(uint8_t* base, uint64_t table_offset, uint32_t code_base, std::span<const FuncMapping>)` — writes `host` at `base + table_offset + (guest - code_base) * 2` (XenonRecomp `PPC_LOOKUP_FUNC` layout: 8-byte slot per 4-byte instruction; table_offset = `PPC_IMAGE_BASE + PPC_IMAGE_SIZE`).
  - Library `lr_runtime_core`.

- [ ] **Step 1: Failing tests** `[unit]`: `be<uint32_t>` storing `0x11223344` has bytes `11 22 33 44`; `be<float>` round-trips `1.5f`; `reserve(0x1'0000'0000)` succeeds and `*translate<uint32_t>(0x82000000) = 7` reads back 7; `describe_fault(SIGSEGV, base, size, base + 0x82001234)` == `"SIGSEGV at guest 0x82001234"`; out-of-range pointer → contains `outside guest memory`; `install_function_table` with code_base `0x82000000`, mapping `{0x82000010, &f}` → `*(void**)(base + off + 0x20) == &f`.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5: Commit** — `feat(runtime): add guest memory, BE helpers, fault decoding, function table`.

---

### Task 12: Runtime — XEX loader, kernel registry, guest thread, game `main`

**Files:**
- Create: `src/runtime/{loader.h,loader.cpp,guest_thread.h,guest_thread.cpp,main.cpp}`, `src/runtime/kernel/{registry.h,registry.cpp}`, `src/pipeline/{import_stubs.h,import_stubs.cpp}`, `tests/test_kernel_registry.cpp`, `tests/test_import_stubs.cpp`

**Interfaces:**
- Consumes: `GuestMemory`, `install_function_table`, `install_fault_handlers`, XenonUtils `Image::ParseImage` (check `XenonUtils/image.h` for fields: base, size, entry_point, sections with base/size/data).
- Produces:
  - `struct LoadedImage { uint32_t base, size, entry; };` `Result<LoadedImage> load_image(GuestMemory&, std::span<const std::byte> xex)` — copies every section to `base + section.base`.
  - Kernel: `struct UnimplementedImport { std::string name; uint32_t caller; std::vector<std::string> trail; };` (exception type); `[[noreturn]] void lr::kernel::unimplemented_import(const char* name, uint32_t caller_lr)` throws it with the current trail; `void lr::kernel::trace(const char* name)` (ring buffer, last 32); `void lr::kernel::register_implemented(const char* name)`; `bool lr::kernel::is_implemented(std::string_view)`; macro `LR_KERNEL_EXPORT(Name)` = defines `PPC_FUNC(__imp__##Name)` and registers `#Name` via a static initializer. `std::string lr::kernel::format_report(const UnimplementedImport&)` → lines `Unimplemented import: <name>`, `Called from: 0x<caller>`, `Trail (last N): a -> b`.
  - Guest thread constants (`guest_thread.h`): stack `0x70000000`, size `0x100000` (r1 = top − 0x80); PCR `0x7FFF0000` (r13); TLS `0x7FFE0000`. `int run_guest(GuestMemory&, uint32_t entry, void (*entry_fn)(void* ctx, uint8_t* base))` — runs on a `std::thread` with 16 MiB host stack (`pthread_attr_setstacksize`), catches `UnimplementedImport` → prints `format_report` to stderr, returns 3; normal return → 0.
  - `std::vector<std::string> lr::pipeline::parse_import_names(std::string_view shared_header)` — every identifier starting with `__imp__` inside any `PPC_EXTERN_*`-style declaration, deduplicated, sorted, prefix stripped. `std::string lr::pipeline::render_import_stubs(std::span<const std::string>)` → C++ source: for each name a `__attribute__((weak)) PPC_FUNC(__imp__<Name>) { lr::kernel::unimplemented_import("<Name>", uint32_t(ctx.lr)); }` and a table `const char* const lr_import_names[] = {...}; const size_t lr_import_count = N;`.
  - `src/runtime/main.cpp` (library `lr_runtime_main`, only linked by the game template): args `--image <xex> --data <dir> [--headless] [--coverage]`. Flow: reserve `0x1'0000'0000 + PPC_CODE_SIZE * 2` bytes → `load_image` → `install_function_table(base, PPC_IMAGE_BASE + PPC_IMAGE_SIZE, PPC_CODE_BASE, PPCFuncMappings)` → `install_fault_handlers` → without `--headless`, open an SDL2 window "LegacyRecomp" on the main thread and pump events while the guest thread runs → exit with `run_guest`'s code. `--coverage` prints markdown table `| Import | Status |` from `lr_import_names` + `is_implemented`, plus `Implemented: X / N`, exit 0. Exit codes: 0 ok, 2 usage/load error, 3 unimplemented import, 4 fault.
  - Libraries `lr_runtime_core` (+ kernel registry) and `lr_runtime_main`; the template links them with `-Wl,--whole-archive` so strong `LR_KERNEL_EXPORT` definitions beat weak stubs.

- [ ] **Step 1: Failing tests** `[unit]`:
  - `parse_import_names` on a sample header containing `PPC_EXTERN_FUNC(__imp__NtCreateFile);`, `PPC_EXTERN_FUNC(__imp__XamInputGetState);`, duplicated `NtCreateFile`, and `PPC_EXTERN_FUNC(sub_82000000);` → `{"NtCreateFile", "XamInputGetState"}`.
  - `render_import_stubs({"A","B"})` contains `PPC_FUNC(__imp__A)`, `"B"`, `lr_import_count = 2`.
  - registry: after `trace("X"); trace("Y");`, calling `unimplemented_import("Z", 0x82001000)` throws `UnimplementedImport{name="Z", caller=0x82001000, trail={"X","Y"}}`; `format_report` contains `Called from: 0x82001000` and `X -> Y`; `register_implemented("Q")` → `is_implemented("Q")`.
- [ ] **Step 2:** Before implementing, open the `ppc_recomp_shared.h` generated in Task 10 and confirm how imports are declared; adjust the sample header in the test to match reality if it differs. Run → FAIL.
- [ ] **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5: Commit** — `feat(runtime): add image loader, kernel stub registry, guest thread and game main`.

---

### Task 13: `build` stage, game template, `launch`, end-to-end run

**Files:**
- Create: `src/runtime/template/CMakeLists.txt`, `src/pipeline/stages/build_stage.{h,cpp}`, `src/pipeline/{pipeline.h,pipeline.cpp}`, `tests/test_integration_e2e.cpp`
- Modify: `src/pipeline/resources.cmake` (stage `runtime/include`, `runtime/lib/{liblr_runtime_core.a,liblr_runtime_main.a,libXenonUtils.a}`, `runtime/template/`), top-level `CMakeLists.txt` (install rules: `bin/legacyrecomp*`, `share/legacyrecomp/**`)

**Interfaces:**
- Consumes: all stages, `render_import_stubs`, `parse_import_names`, `run_tool`.
- Produces:
  - Template `CMakeLists.txt`: inputs `-DLR_RESOURCES=… -DLR_GENERATED=…`; `file(GLOB …ppc_recomp.*.cpp)` + `ppc_func_mapping.cpp` + `import_stubs.cpp`; `find_package(SDL2 REQUIRED)`; compile flags from the XenonRecomp README (at least `-fno-strict-aliasing`, `-O2`); link runtime libs with `--whole-archive`; output `legacyrecomp-game`.
  - `make_build_stage()`: writes `work/generated/import_stubs.cpp` (atomic), runs `cmake -S <res>/runtime/template -B work/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ …` then `cmake --build work/build -j <jobs>`; copies the binary to `bin/legacyrecomp-game`; progress parsed from Ninja's `[n/N]`. Fingerprint = hash of generated dir listing + runtime lib hashes.
  - `std::vector<std::unique_ptr<Stage>> lr::pipeline::default_stages(DoctorProbe&)` (order 0–6); `Result<void> lr::pipeline::run(const Options&, ProgressSink&)` (resolves resources, loads `game.toml`, opens `StateStore`, runs); `Result<int> lr::pipeline::launch(const Options&, std::span<const std::string> extra_args)` — spawns `bin/legacyrecomp-game --image work/image/default_tu1.xex --data game [extra]` with inherited stdio, returns its exit code.

- [ ] **Step 1: Integration test** `[integration]` `test_integration_e2e.cpp`: `run()` into a temp root with the real ISO, then `launch` with `--headless` → exit code `3`; stderr capture contains `Unimplemented import:`. Second `run()` executes zero stages (sink records no `"start"` event). Run → FAIL (build stage missing).
- [ ] **Step 2:** Implement template, build stage, `pipeline.cpp`.
- [ ] **Step 3:** Run `LEGACYRECOMP_ISO=… ctest --preset dev -L integration` → PASS. If the guest faults (exit 4) before any import, debug with `superpowers:systematic-debugging` + the RE skill; record findings in `docs/reverse-engineering/`.
- [ ] **Step 4:** `ctest --preset dev -L unit` → PASS. **Commit** — `feat(pipeline): add build stage, game template and launch`.

---

### Task 14: CLI — `doctor`, `install`, `build`, `run`

**Files:**
- Modify: `src/launcher/cli/main.cpp`
- Create: `src/launcher/cli/{progress_bar.h,progress_bar.cpp,args.h,args.cpp}`, `tests/test_cli_args.cpp`, `tests/test_progress_bar.cpp`

**Interfaces:**
- Consumes: `pipeline::run`, `launch`, `run_doctor`, `DataPaths`, `default_data_root`.
- Produces:
  - `struct CliArgs { std::string command; std::optional<fs::path> iso, root; std::set<StageId> force; unsigned jobs = 0; std::vector<std::string> passthrough; };` `Result<CliArgs> parse_args(std::span<const std::string_view>)` — commands `doctor`, `inspect`, `install` (requires `--iso`), `build` (= install with `force = {Recompile}` and ISO not required when extract is already complete), `run` (args after `--` → `passthrough`); unknown stage name in `--force` → `InvalidFormat` listing valid names.
  - `class ProgressBar : ProgressSink { ProgressBar(std::ostream&, bool is_tty); }` — tty: single line `\r[<stage>] [#####.....] 45% <message>` (40 cols bar); non-tty: one line per change of stage or per 10% step. SIGINT handler sets `cancelled()`.
  - Exit codes: 0 ok, 1 pipeline error (prints `error: <message>` and `hint: <hint>`), 2 usage. `run` forwards the game's code and on 3 also prints `M1: game stopped at the first unimplemented kernel import (expected at this milestone).`
- [ ] **Step 1: Failing tests** `[unit]`: `install --iso "a b (c).iso" --force recompile --jobs 4` parses all fields with the path intact; `install` without `--iso` → error; `--force bogus` → error listing `extract`; `run -- --headless` → passthrough `{"--headless"}`; non-tty `ProgressBar` emits `[extract] 50%` once for updates 0.50 and 0.55.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5:** Manual: `./build/dev/bin/legacyrecomp install --iso "Minecraft - Xbox 360 Edition (World).iso"` completes; repeat completes instantly; `legacyrecomp install … --force recompile` reruns recompile+build only; `legacyrecomp run -- --headless` prints the M1 line.
- [ ] **Step 6: Commit** — `feat(cli): add doctor, install, build and run commands with progress bar`.

---

### Task 15: GUI launcher

**Files:**
- Create: `src/launcher/gui/{main.cpp,app.h,app.cpp,queue_sink.h,queue_sink.cpp,CMakeLists.txt}`, `tests/test_queue_sink.cpp`

**Interfaces:**
- Consumes: `pipeline::run`, `launch`, `capture_output`, `find_program`, imgui (+ `backends/imgui_impl_sdl2`, `imgui_impl_sdlrenderer2`).
- Produces:
  - `class QueueSink : ProgressSink { std::vector<Event> drain(); void cancel(); }` with `struct Event { enum Kind { Progress, Log, Finished } kind; Progress progress; std::string text; std::optional<Error> error; };` — thread-safe (mutex); `post_finished(std::optional<Error>)`.
  - Executable `legacyrecomp-gui`: ISO path text field; "Procurar…" runs `zenity --file-selection --file-filter="*.iso"` or `kdialog --getopenfilename . "*.iso"` (whichever `find_program` finds; button hidden if neither); `SDL_DROPFILE` fills the field; "Instalar" starts a `std::thread` running `pipeline::run(options, sink)`; per-stage list (pending / running % / done / error with hint); scrolling log (last 2000 lines); "Cancelar"; "Jogar" enabled when the build stage is complete, runs `launch` on a thread and shows the M1 exit-3 message. UI strings in Portuguese.
- [ ] **Step 1: Failing test** `[unit]`: two producer threads posting 1000 events each → `drain()` returns 2000 events total across calls with each thread's order preserved; `cancel()` → `cancelled()` true.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5:** Manual: launch `./build/dev/bin/legacyrecomp-gui`, drop the ISO, Install (on a fresh `--root` via `LEGACYRECOMP_ROOT` env read by the GUI), watch stages complete, press Jogar → M1 message. Take a screenshot to `docs/assets/gui-m1.png` (UI only, no game content).
- [ ] **Step 6: Commit** — `feat(gui): add SDL2/ImGui launcher`.

---

### Task 16: Documentation, README, coverage report, acceptance check

**Files:**
- Create: `README.md`, `docs/{architecture.md,pipeline.md,roadmap.md,kernel-coverage.md}`, `docs/adr/0001-static-recompilation.md`, `docs/adr/0002-gpl-3.md`, `docs/adr/0003-m1-plan-deviations.md`
- Modify: `AGENTS.md` (final commands), `docs/reverse-engineering/README.md`

- [ ] **Step 1:** `docs/kernel-coverage.md` = output of `~/.local/share/legacyrecomp/bin/legacyrecomp-game --coverage` (import names + status only) with a header explaining how to regenerate it.
- [ ] **Step 2:** Docs: `architecture.md` (units, dependency direction, Mermaid diagram, runtime memory map with the Task 12 constants), `pipeline.md` (stage table, data root layout, `state.toml`, `--force` semantics, error codes and exit codes, troubleshooting by error code), `roadmap.md` (M1–M5 from spec §13 with M1 marked done), ADR 0001 (static recomp with hedge-dev; pinned submodule commits), ADR 0002 (GPL-3.0 reasoning), ADR 0003 (each deviation from Global Constraints with its reason).
- [ ] **Step 3:** `README.md` (English, with a Portuguese section link): banner title + one-line pitch; badges (license, C++20, platform Linux); legal notice ("You must own and dump your own disc. This repository contains no game code or assets. Not affiliated with Mojang, Microsoft or 4J Studios."); quick start (toolchain install, clone `--recursive`, build preset, `legacyrecomp install --iso …`, `legacyrecomp run`); Mermaid pipeline diagram; status table (M1 ✅ … M5); GUI screenshot; credits (hedge-dev XenonRecomp/XenosRecomp/UnleashedRecomp, Xenia, free60); contributing pointer to `AGENTS.md`.
- [ ] **Step 4: Acceptance check** — spec §10, each item with evidence:
  1. fresh root: `legacyrecomp install --iso …` exit 0;
  2. GUI install shown working (Task 15 step 5);
  3. second install executes no stage; `--force recompile` runs recompile + build only;
  4. `legacyrecomp run -- --headless` → exit 3 with report;
  5. `ctest --preset dev -L unit` passes; `git ls-files | grep -Ei '\.(iso|xex|xexp)$|^work/|ppc_recomp'` prints nothing;
  6. docs/README/AGENTS.md/CLAUDE.md/skill exist and match reality.
- [ ] **Step 5: Commit** — `docs: add architecture, pipeline, ADRs, roadmap, coverage and README`.
