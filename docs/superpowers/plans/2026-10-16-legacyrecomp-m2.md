# LegacyRecomp M2 — Kernel HLE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The recompiled game boots headless through its whole startup (memory, threads, sync, files, time,
config, minimal XAM) and stops with exit 3 at the first video import that creates or uses the GPU, with every
PowerPC instruction it uses translated.

**Architecture:** Kernel code lives in the prebuilt `lr_runtime_core` and only sees a fixed-layout
`GuestCall`; generated per-import thunks (compiled in the game template) are the only code touching
`PPCContext` (R16 stays). Guest memory is authoritative for every kernel structure (dispatcher headers,
critical sections, PCR/KTHREAD/TLS). Missing PowerPC instructions are added in a LegacyRecomp fork of
XenonRecomp.

**Tech Stack:** C++20, Clang ≥ 18, CMake ≥ 3.25 + Ninja, SDL2, Catch2 v3, XenonRecomp fork (branch
`legacyrecomp` on `ddd128b`), libmspack `lzxd.c` (already inside `libXenonUtils.a`), Docker (Arch) for
development on non-Linux hosts.

**Spec:** `docs/superpowers/specs/2026-10-16-legacyrecomp-m2-design.md`. Import catalog by subsystem:
`docs/reverse-engineering/kernel-imports.md`.

## Global Constraints

- Everything in `AGENTS.md` still applies: MIT + `SPDX-License-Identifier: MIT` header on every new file;
  never copy UnleashedRecomp code (Xenia and UnleashedRecomp are semantic references only); `lr::Result<T>`
  on public APIs; code/identifiers/commits in English, `docs/` prose in Portuguese; docs change in the same
  commit as the code they describe; never commit `*.iso`, `*.xex`, `*.xexp`, `work/`, generated `ppc_*`.
- Branch `feat/m2` for the whole plan (as R1). **Nothing is pushed** until the XenonRecomp fork exists
  (Task 7): the published repo never references a submodule commit others cannot fetch.
- Every commit passes `tools/dev/container.sh test` (= `ctest --preset dev -L unit` in the container) or the
  same command on a Linux host.
- Namespaces: kernel code `lr::kernel` (`src/runtime/kernel/`), allocators `lr::runtime`
  (`src/runtime/memory/`). Kernel import implementations live in `src/runtime/kernel/exports/<subsystem>.cpp`.
- Exceptions: `UnimplementedImport` (missing import) and `GuestThreadExit` (Task 17) only. Everything else
  fatal goes through `lr::kernel::guest_fatal` (exit 5).
- Game exit codes: 0 ok, 2 usage/load, 3 missing import, 4 fault, 5 guest fatal, 6 deadlock.
- Guest structures are big-endian (`be<T>` from `runtime/be.h`); every layout has `static_assert(offsetof)`
  checks. Offsets in this plan come from the spec/Xenia: verify each against Xenia's headers before use and
  note the source file in a comment.
- Stub policy (spec §5.2): an import the game reaches before the finish line is `implemented` or `stub`; GPU
  creating/using `Vd*` stays `missing`; video *queries* are stubs describing 1280x720 widescreen HDMI.
- Never read game data into context: inspect with scripts in `tools/` and read their summary only.
- Tasks marked **[ISO]** need the user's disc image: run them in the container only after the owner
  authorizes ISO use; otherwise they are human checkpoints (owner runs the command, returns the log).

## Review Focus

1. **Bad guest pointers** (null `IO_STATUS_BLOCK`, buffer running past 4 GiB or into the null guard) →
   `STATUS_INVALID_PARAMETER`/`STATUS_ACCESS_VIOLATION`, never a host crash. Tests: Task 8
   `GuestPtr rejects ranges past 4 GiB`, Task 22 `NtReadFile with null io status block`.
2. **Guest path oddities** (mixed case, `\??\game:\`, doubled `\\`, trailing `\`, `..` escaping the mount,
   host data root with spaces such as `D:\Jogos Teste`) → resolve or reject with the Xbox status. Test:
   Task 21 `resolve handles odd guest paths`.
3. **Handle misuse** (double close, wait on a closed handle, wait on a non-waitable object) →
   `STATUS_INVALID_HANDLE`/`STATUS_OBJECT_TYPE_MISMATCH`. Tests: Task 15 `double close`, Task 16
   `wait on file handle`.
4. **Thread lifetime races** (thread exits while waited on; its handle closed while it runs) → waiter wakes
   with `STATUS_WAIT_0`, no use-after-free under ASan. Test: Task 17 `close handle of running thread`.
5. **Odd allocation requests** (size 0, larger than the region, unaligned fixed base, reserving an already
   reserved range, freeing the middle of a region) → the NTSTATUS Xenia returns. Test: Task 11
   `page heap edge cases`.

---

## File Structure

```
tools/dev/Dockerfile  tools/dev/container.sh          # Arch dev container; build in a volume, run ctest
tools/recomp_warnings.py                              # summarizes recompile.log (counts, float16_4 sites)
docs/development.md                                   # dev on Windows/macOS via the container
thirdparty/XenonRecomp                                # -> LegacyRecomp fork, branch legacyrecomp
tests/recomp/{cases.h,gen_recomp_fixture.cpp,test_recomp_instructions.cpp,CMakeLists.txt}
src/runtime/memory_map.h                              # guest region constants
src/runtime/memory/{page_heap,physical_heap}.{h,cpp}
src/runtime/kernel/abi.h marshal.h status.h guest_structs.h
src/runtime/kernel/registry.{h,cpp}                   # name -> {state, fn}; trail; coverage
src/runtime/kernel/kernel.{h,cpp}                     # process-wide Kernel state, kernel(), guest_fatal
src/runtime/kernel/glue.{h,cpp}                       # call_guest via GuestGlue registered by main.cpp
src/runtime/kernel/objects.{h,cpp}                    # KObject, ObjectTable
src/runtime/kernel/dispatcher.{h,cpp}                 # wait engine over guest DISPATCHER_HEADERs
src/runtime/kernel/threads.{h,cpp}                    # GuestThread, per-thread PCR/KTHREAD/TLS, APC queue
src/runtime/kernel/timers.{h,cpp}                     # timers, DPC worker, timestamp bundle, TSC calibration
src/runtime/kernel/vfs.{h,cpp}                        # symlinks, mounts, case-insensitive resolution
src/runtime/kernel/guest_printf.{h,cpp}               # printf interpreter over guest args
src/runtime/kernel/variables.{h,cpp}                  # kernel data exports + variable import resolution
src/runtime/kernel/trace.{h,cpp}                      # --trace-imports writer
src/runtime/kernel/exports/{memory,objects,sync,threads,time,critsec,debug,files,lzx,rtl,xex,config,crypto,xam,video,net}.cpp
src/common/sha1.{h,cpp}
src/runtime/main.cpp  src/runtime/loader.{h,cpp}  src/runtime/guest_thread.{h,cpp}  src/runtime/fault.cpp
src/pipeline/import_stubs.{h,cpp}  src/pipeline/stages/recompile_stage.cpp  src/pipeline/pipeline.cpp
src/formats/xex2.{h,cpp}  src/common/paths.{h,cpp}  src/launcher/cli/main.cpp
tests/support/kernel_fixture.h  tests/support/fake_generated/*  tests/test_kernel_*.cpp
```

---

## Part A — Environment and recompiler

### Task 1: Docker dev container

**Files:**
- Create: `tools/dev/Dockerfile`, `tools/dev/container.sh`, `docs/development.md`
- Modify: `AGENTS.md` (Build and test: container commands), `README.md` + `README.pt-BR.md` (one line pointing to `docs/development.md`)

**Interfaces:**
- Produces: `tools/dev/container.sh <command>` with commands `shell`, `build`, `test [ctest args…]`,
  `install --iso <host path>`, `run [-- game args]`. Image `legacyrecomp-dev` (Arch, packages from
  `AGENTS.md` plus `git python binutils`). Repo mounted read-only at `/src`; tracked files (with submodules)
  synced into volume `lr-build` at `/work` (`git ls-files --recurse-submodules` + tar), build in
  `/work/build/<preset>`; data root `/data` in volume `lr-data`; an ISO is mounted read-only at `/iso/game.iso`.
  Ninja jobs = max(1, container memory / 2 GiB). Works from Git Bash on Windows (`MSYS_NO_PATHCONV=1`,
  paths with spaces).

- [ ] **Step 1:** `git checkout -b feat/m2`.
- [ ] **Step 2:** Write `Dockerfile` and `container.sh`; `container.sh` exits non-zero with a clear message
  when Docker is not running.
- [ ] **Step 3:** Run `tools/dev/container.sh test`. Expected: every `unit` test passes (baseline of M1).
  On a 4 GB Docker VM it must not crash the engine (it did with `-j6`).
- [ ] **Step 4:** Write `docs/development.md` (Portuguese): requirements, raising Docker Desktop memory
  (Settings → Resources, ≥ 8 GB recommended), every command, where build and data live, ISO mounting.
- [ ] **Step 5: Commit** `build(dev): add a Docker development container`.

### Task 2: XenonRecomp fork branch and instruction test harness

**Files:**
- Submodule: `thirdparty/XenonRecomp` → local branch `legacyrecomp` at `ddd128b`
- Create: `tests/recomp/cases.h`, `tests/recomp/gen_recomp_fixture.cpp`, `tests/recomp/test_recomp_instructions.cpp`, `tests/recomp/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt` (`add_subdirectory(recomp)`)

**Interfaces:**
- Produces: test executable `lr_recomp_tests` (Catch2, tag `[unit]`, discovered by ctest). Build flow:
  `lr_gen_recomp_fixture` writes a synthetic big-endian ELF (`tests/support/elf_builder.h`) with one
  function per case at fixed addresses plus a `recomp.toml` listing them; a custom command runs the
  `XenonRecomp` target on it; the generated `ppc_recomp.*.cpp` + `ppc_func_mapping.cpp` are compiled into
  `lr_recomp_tests` (separate executable so its `ppc_config.h` never meets `fake_generated`).
- `struct RecompCase { const char* name; uint32_t address; std::vector<uint32_t> words; };` in `cases.h`;
  each case's words end with `blr` (`0x4E800020`).
- Test helper `PPCContext run_case(uint32_t address, PPCContext in, std::vector<uint8_t>& guest_mem)`.

- [ ] **Step 1:** `cd thirdparty/XenonRecomp && git checkout -b legacyrecomp ddd128b`.
- [ ] **Step 2: Write the failing test** `addi baseline`: case `addi r3,r3,5` (`0x38630005`); with
  `r3 = 7` → `r3 == 12`. Run `container.sh test -R recomp` → FAIL (target missing).
- [ ] **Step 3:** Implement the generator, custom command and test executable.
- [ ] **Step 4:** Run `container.sh test -R recomp` → PASS.
- [ ] **Step 5: Commit** `test(recomp): add a harness that recompiles and runs synthetic instructions`.

### Task 3: Branch instructions `bdzf`, generic `bdnzf`, `bso`

**Files:**
- Modify (submodule, branch `legacyrecomp`): `XenonRecomp/recompiler.cpp` (cases near `bdz`/`bdnz`, ~:650)
- Modify: `tests/recomp/cases.h`, `tests/recomp/test_recomp_instructions.cpp`

**Interfaces:**
- Semantics: `bdzf BI,t`: `ctr -= 1; if (ctr == 0 && !CR[BI]) goto t`; `bdnzf BI,t`: `ctr -= 1;
  if (ctr != 0 && !CR[BI]) goto t` (field `BI/4`, bit `{lt,gt,eq,so}[BI%4]` — no more eq-only shortcut);
  `bso crN,t`: `if (cr(N).so) goto t`. If the emitted `fcmpu` does not set `so` on unordered (NaN) operands,
  make it do so (unordered: `lt=gt=eq=0, so=1`).

- [ ] **Step 1: Write the failing tests:** `bdzf takes branch when ctr hits zero and bit clear`,
  `bdzf falls through when bit set` (each of lt/gt/eq/so), `bdnzf uses the requested bit (gt)`,
  `bso after fcmpu with NaN branches`, `bso after fcmpu of 1.0,2.0 falls through`. Run → FAIL.
- [ ] **Step 2:** Implement in the submodule (credit PR #60 by Mystixor in the commit for `bdzf`).
- [ ] **Step 3:** Run `container.sh test -R recomp` → PASS.
- [ ] **Step 4: Commit** in submodule `feat: add bdzf and bso, make bdnzf honour its CR bit`; then in the
  superproject `test(recomp): cover bdzf, bdnzf and bso` (submodule pointer + tests).

### Task 4: Load/store with update: `lfsu`, `lfsux`, `stfsu`, `stfsux`, `lhzu`, `sthu`

**Files:** submodule `recompiler.cpp` (next to `lwzu` ~:1222, `stwu` ~:1668, `lfs` ~:1082, `stfs` ~:1550);
`tests/recomp/*`.

**Interfaces:**
- `ea = rA + d` (or `rA + rB`); load/store as the non-update form (respect `mmioStore()` for stores and the
  flush-mode handling of `lfs`); then `rA = ea`.

- [ ] **Step 1: Write the failing tests**, one per instruction: value in register/memory **and** `rA == ea`
  (e.g. `stfsu f1,-8(r1)` with `f1 = 1.5`, `r1 = 0x1000` → bytes `3F C0 00 00` at `0xFF8`, `r1 == 0xFF8`).
- [ ] **Step 2:** Implement. **Step 3:** tests PASS.
- [ ] **Step 4: Commit** (submodule `feat: add load/store-with-update forms lfsu, lfsux, stfsu, stfsux, lhzu,
  sthu`, crediting PR #60; superproject `test(recomp): …`).

### Task 5: `eqv`, `subfze`, `vsel128`

**Files:** submodule `recompiler.cpp` (`vsel` ~:2090, `subfe`/`addze`); `tests/recomp/*`.

**Interfaces:**
- `eqv rA,rS,rB`: `rA = ~(rS ^ rB)`; `eqv.` also sets cr0.
- `subfze rD,rA`: `rD = ~rA + CA`; `CA` = carry out; `subfze.` sets cr0.
- `vsel128 vD,vA,vB` (mask register is `vD`, `VS128 = VD128`): `vD = (vA & ~vD) | (vB & vD)`; confirm bit
  order against Xenia's `InstrEmit_vsel128` (read, don't copy).

- [ ] **Step 1: Write the failing tests:** `eqv of equal values is all ones`, `eqv. sets cr0.lt for
  negative result`, `subfze with CA=1 on 0 gives 0 and CA=1`, `subfze with CA=0 on 0 gives -1 and CA=0`,
  `vsel128 picks vB bits where mask set` (VMX128 register ≥ 32, e.g. v64).
- [ ] **Step 2:** Implement. **Step 3:** PASS. **Step 4: Commit** (submodule + superproject, as above).

### Task 6: Warning counter and `tools/recomp_warnings.py`

**Files:**
- Create: `tools/recomp_warnings.py`
- Modify: `src/pipeline/stages/recompile_stage.{h,cpp}`, `tests/test_recomp_config.cpp`, `docs/pipeline.md`

**Interfaces:**
- `struct RecompWarnings { std::map<std::string, uint32_t> unrecognized; uint32_t float16_pack = 0; };`
  `RecompWarnings count_recomp_warnings(std::string_view log);` in `recompile_stage.h`. The stage appends
  `unrecognized instructions: N (top: bdzf 754, …)` to `recompile.log` and emits it as the final progress
  message. The stage does not fail on warnings.
- `python3 -I tools/recomp_warnings.py <recompile.log>` prints a mnemonic count table and every float16_4
  site with its operands. Reads only the log, never the image.

- [ ] **Step 1: Write the failing test** `count_recomp_warnings counts mnemonics and float16 packs` using
  log lines in XenonRecomp's exact warning format (copy the format string from `recompiler.cpp:2432` and
  the float16_4 warning).
- [ ] **Step 2:** Implement. **Step 3:** PASS.
- [ ] **Step 4: Commit** `feat(pipeline): count XenonRecomp warnings in the recompile stage`.

### Task 7: [ISO] Remaining mnemonics, float16_4 packs, publish the fork

**Files:** submodule `recompiler.cpp`; `tests/recomp/*`; `.gitmodules`;
`docs/reverse-engineering/addresses.md`; `docs/roadmap.md`; `docs/adr/0004-m2-plan-deviations.md` (create).

**Interfaces:**
- Fork URL: `https://github.com/<owner>/XenonRecomp` created by the owner (human step); branch `legacyrecomp`.

- [ ] **Step 1:** `container.sh install --iso <iso>` (`--force recompile` if already installed), then
  `python3 -I tools/recomp_warnings.py <data>/work/logs/recompile.log`. Record the table (counts only) in
  `addresses.md`.
- [ ] **Step 2:** For each remaining mnemonic: failing test → implement in the submodule → PASS → commit,
  as Tasks 3–5.
- [ ] **Step 3:** float16_4: classify the sites by `(pack, shift)`; for each class write a failing test with
  the expected halves (semantics from Xenia's `VPKD3D128`, read only), implement, PASS, commit.
- [ ] **Step 4:** Re-run install with `--force recompile`; expected `unrecognized instructions: 0` and zero
  float16_4 warnings, or each remaining one documented in `addresses.md` with the reason.
- [ ] **Step 5 (human):** owner creates the fork on GitHub. Push branch `legacyrecomp`; set `.gitmodules`
  URL to the fork; `git submodule sync`.
- [ ] **Step 6:** ADR 0004 (Portuguese) records: fork instead of upstream bump (upstream `main` = `ddd128b`),
  credit to PR #60, the `bdnzf` fix. Update ADR 0001's pin note and `roadmap.md` blockers.
- [ ] **Step 7: Commit** `build(deps): point XenonRecomp at the LegacyRecomp fork`.

---

## Part B — Kernel foundation

### Task 8: Fixed ABI, marshalling, status codes, guest structs

**Files:**
- Create: `src/runtime/kernel/abi.h`, `marshal.h`, `status.h`, `guest_structs.h`, `tests/test_kernel_abi.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- `abi.h`:
  ```cpp
  namespace lr::kernel {
  struct GuestCall {
    uint8_t* base;
    uint64_t gpr[8];   // r3..r10
    double fpr[13];    // f1..f13
    uint32_t r1, r13, lr;
    uint64_t ret_gpr = 0;  // -> r3
    double ret_fpr = 0;    // -> f1 when has_fpr_return
    bool has_fpr_return = false;
  };
  using KernelFn = void (*)(GuestCall&);
  enum class ImportState : uint8_t { Missing, Stub, Implemented };
  }
  ```
- `marshal.h`: `template <class T> struct GuestPtr { uint32_t addr; T* get(uint8_t* base) const;
  explicit operator bool() const; bool valid_range(size_t bytes) const; }` (`valid_range` false if
  `addr < 0x10000` or `addr + bytes > 2^32`); `template <auto Fn> void marshal(GuestCall&)` converting by
  parameter position: integral/`GuestPtr` from `gpr[i]` for i < 8, else stack (32-bit BE at
  `r1 + 0x54 + (i-8)*8`, 64-bit BE at `r1 + 0x50 + (i-8)*8` — confirm in Xenia `shim_utils.h`); `float`/
  `double` from `fpr[next]` and still consume a GPR position. Returns: ≤ 32-bit → sign-extended from 32
  bits; 64-bit verbatim; floating → `ret_fpr` + `has_fpr_return`.
- `status.h`: `constexpr uint32_t` for `STATUS_SUCCESS 0`, `STATUS_ABANDONED_WAIT_0 0x80`,
  `STATUS_USER_APC 0xC0`, `STATUS_ALERTED 0x101`, `STATUS_TIMEOUT 0x102`, `STATUS_PENDING 0x103`,
  `STATUS_NO_MORE_FILES 0x80000006`, `STATUS_NOT_IMPLEMENTED 0xC0000002`, `STATUS_ACCESS_VIOLATION 0xC0000005`,
  `STATUS_INVALID_HANDLE 0xC0000008`, `STATUS_INVALID_PARAMETER 0xC000000D`, `STATUS_END_OF_FILE 0xC0000011`,
  `STATUS_NO_MEMORY 0xC0000017`, `STATUS_CONFLICTING_ADDRESSES 0xC0000018`, `STATUS_ACCESS_DENIED 0xC0000022`,
  `STATUS_BUFFER_TOO_SMALL 0xC0000023`, `STATUS_OBJECT_TYPE_MISMATCH 0xC0000024`,
  `STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034`, `STATUS_OBJECT_NAME_COLLISION 0xC0000035`,
  `STATUS_OBJECT_PATH_NOT_FOUND 0xC000003A`, `STATUS_INVALID_PAGE_PROTECTION 0xC0000045`,
  `STATUS_MUTANT_NOT_OWNED 0xC0000046`, `STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047`,
  `STATUS_MEMORY_NOT_ALLOCATED 0xC00000A0`; Win32: `ERROR_SUCCESS 0`, `ERROR_NO_MORE_FILES 18`,
  `ERROR_IO_PENDING 997`, `ERROR_DEVICE_NOT_CONNECTED 1167`, `ERROR_NOT_FOUND 1168`,
  `ERROR_FUNCTION_FAILED 1627`.
- `guest_structs.h` (all `be<>` fields, `static_assert` size/offsets): `X_DISPATCHER_HEADER` (type u8 +0,
  absolute +1, size +2, inserted +3, signal_state i32 +4, wait_list flink +8 / blink +0xC; 0x10),
  `X_KEVENT` (0x10), `X_KSEMAPHORE` (limit +0x10; 0x14), `X_KMUTANT` (owner +0x18, abandoned +0x1C; 0x20),
  `X_RTL_CRITICAL_SECTION` (lock_count +0x10, recursion_count +0x14, owning_thread +0x18; 0x1C), `X_KPCR`
  (tls_ptr +0x00, pcr_ptr +0x30, stack_base +0x70, stack_end +0x74, current_thread +0x100, current_cpu u8
  +0x10C; 0x2D8), `X_KTHREAD` (header type 6, stack_base +0x5C, stack_limit +0x60, tls_address +0x68,
  thread_id +0x14C, start_address +0x150, last_error +0x160), `X_ANSI_STRING` (length u16, max u16,
  buffer +4; 8), `X_OBJECT_ATTRIBUTES` (root_directory +0, name_ptr +4, attributes +8; 0xC),
  `X_IO_STATUS_BLOCK` (status +0, information +4; 8), `X_LARGE_INTEGER` (i64).

- [ ] **Step 1: Write the failing tests:** `marshal reads nine integer args (ninth from stack)`,
  `marshal sign-extends 32-bit returns` (`0xC0000034` → `r3 == 0xFFFFFFFFC0000034`), `marshal keeps 64-bit
  args`, `GuestPtr zero is null`, `GuestPtr rejects ranges past 4 GiB`, `float arg consumes a GPR position`
  (`f(int a, double b, int c)` reads `c` from `gpr[2]`), `float return sets f1`.
- [ ] **Step 2:** Run → FAIL (headers missing). **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): add the fixed guest-call ABI and argument marshalling`.

### Task 9: Registry v2, thunk generator, three-state coverage

**Files:**
- Modify: `src/runtime/kernel/registry.{h,cpp}`, `src/pipeline/import_stubs.{h,cpp}`,
  `src/runtime/template/CMakeLists.txt` (comment only), `src/runtime/main.cpp` (`--coverage`),
  `tests/support/fake_generated/fake_kernel.cpp`, `tests/test_kernel_registry.cpp`,
  `tests/test_import_stubs.cpp`, `tests/test_fake_game.cpp`, `docs/architecture.md` (Kernel section),
  `docs/adr/0004-m2-plan-deviations.md` (create if Task 7 has not yet)

**Interfaces:**
- Registry:
  ```cpp
  struct ImportEntry { ImportState state = ImportState::Missing; KernelFn fn = nullptr; };
  void register_import(const char* name, ImportState state, KernelFn fn);
  ImportEntry find_import(std::string_view name);
  void trace(const char* name);                        // unchanged, now called by every thunk
  [[noreturn]] void unimplemented_import(const char* name, uint32_t caller_lr);   // unchanged
  ```
  `register_implemented`/`is_implemented`/`LR_KERNEL_EXPORT` are removed. Macros:
  `LR_KERNEL(Name)` registers `marshal<&exports::Name>` as `Implemented` (function `exports::Name` declared
  in `namespace lr::kernel::exports`); `LR_KERNEL_STUB(Name, value)` registers a `Stub` returning the
  constant `value` in r3 (the comment next to it states why).
- Thunk generator: `std::string render_import_thunks(std::span<const std::string> names);` (replaces
  `render_import_stubs`). Per name: `PPC_FUNC(__imp__X)` that on first call caches `find_import("X")`
  in a function-local `static`, calls `trace("X")`, and if `fn == nullptr` calls
  `unimplemented_import("X", ctx.lr)`; else copies r3..r10, f1..f13, r1, r13, lr into a `GuestCall`
  (`base` = `base`), calls `fn`, writes `ret_gpr` to r3 and, when `has_fpr_return`, `ret_fpr` to f1. Keeps
  `lr_import_names[]`/`lr_import_count`. Still `--whole-archive` (registration statics).
- `--coverage` prints `| Import | Kind | Status |` with `function`, statuses `implemented`/`stub`/`missing`,
  and `Implemented: a, stub: b, missing: c / total`.

- [ ] **Step 1: Write the failing tests:** `render_import_thunks emits a cached lookup per import`,
  `registered stub is reported as stub`, fake game `--coverage lists three states` (fake kernel registers
  `FakeImplemented` via `LR_KERNEL` and `FakeStub` via `LR_KERNEL_STUB(FakeStub, 0)`; `FakeMissing`
  unregistered), fake game `trail contains every import called before the missing one`.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement; update `fake_recomp.cpp`/`ppc_recomp_shared.h` to
  call `FakeStub` too. **Step 4:** PASS (all old fake-game tests still pass).
- [ ] **Step 5:** Docs: `architecture.md` "Kernel" rewritten (thunks, `GuestCall`, states); ADR 0004 entry
  "LR_KERNEL_EXPORT model replaced (spec §4.1)".
- [ ] **Step 6: Commit** `feat(kernel): replace weak import stubs with thunks over a fixed ABI`.

### Task 10: Kernel state, guest glue, guest_fatal, exit codes

**Files:**
- Create: `src/runtime/kernel/kernel.{h,cpp}`, `src/runtime/kernel/glue.{h,cpp}`,
  `tests/support/kernel_fixture.h`, `tests/test_kernel_glue.cpp`
- Modify: `src/runtime/guest_thread.h` (exit codes), `src/runtime/main.cpp`, `src/runtime/CMakeLists.txt`,
  `tests/support/fake_generated/*`, `tests/test_fake_game.cpp`

**Interfaces:**
- `guest_thread.h`: add `kExitGuestFatal = 5`, `kExitDeadlock = 6`.
- `kernel.h`:
  ```cpp
  struct KernelConfig { std::filesystem::path game_dir, user_dir; std::string language; bool strict_memory = false;
                        std::optional<std::filesystem::path> trace_file; };
  class Kernel { public: runtime::GuestMemory& memory(); const KernelConfig& config() const; /* subsystems added by later tasks */ };
  Result<void> init_kernel(runtime::GuestMemory& mem, KernelConfig cfg);   // once per process
  Kernel& kernel();
  [[noreturn]] void guest_fatal(std::string_view what);  // prints "Guest fatal: <what>" + trail, flushes, _exit(5)
  ```
- `glue.h`:
  ```cpp
  struct GuestGlue {
    uint64_t (*call)(uint8_t* base, uint32_t fn, const uint64_t* args, int nargs, uint32_t r1, uint32_t r13);
    bool (*has_function)(uint32_t fn);
  };
  void set_guest_glue(const GuestGlue& g);
  uint64_t call_guest(uint32_t fn, std::span<const uint64_t> args);  // guest_fatal if !has_function(fn)
  ```
  `call_guest` uses the calling thread's current guest frame (set by every thunk from `GuestCall`:
  thread-local `r1`/`r13`); the callee runs with `r1 = (current r1 - 0x100) & ~0xF`. `main.cpp` implements
  `call` (fresh `PPCContext`, r3.. = args, `PPC_LOOKUP_FUNC`) and `has_function` (address inside
  `[PPC_CODE_BASE, PPC_CODE_BASE + PPC_CODE_SIZE)`, 4-aligned, slot non-null).
- `kernel_fixture.h`: `struct KernelFixture { KernelFixture(); GuestMemory mem; template<class... A> uint64_t
  call(const char* import, A... args); uint32_t alloc(uint32_t bytes); /* bump allocator in 0x20000000.. for
  test data */ void put_stack_arg(int index, uint64_t v); };` — `call` builds a `GuestCall` (r1 =
  `0x30000000`) and invokes `find_import(import).fn`.

- [ ] **Step 1: Write the failing tests:** `call_guest runs a recompiled callback` (fake game: fake kernel
  import `FakeCallback(fnaddr, arg)` calls back `sub_82000200`, which stores `arg+1` in r3; exit 0 and the
  value returned), `call_guest to an address without function is guest fatal` (exit 5, message names the
  address), `guest_fatal prints the trail`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): add kernel state, guest call glue and guest_fatal`.

### Task 11: Memory map and page heap

**Files:**
- Create: `src/runtime/memory_map.h`, `src/runtime/memory/page_heap.{h,cpp}`, `tests/test_page_heap.cpp`
- Modify: `src/runtime/CMakeLists.txt`

**Interfaces:**
- `memory_map.h` constants (spec §6.1): `kNullGuardEnd 0x00010000`, `kHeap4kBase 0x00010000`,
  `kHeap4kEnd 0x40000000`, `kHeap64kBase 0x40000000`, `kHeap64kEnd 0x70000000`, `kStacksBase 0x70000000`,
  `kStacksEnd 0x7F000000`, `kGpuReservedBase 0x7F000000`, `kKernelDataBase 0x80000000`,
  `kKernelDataEnd 0x82000000`, `kPhysicalViewA0 0xA0000000` (64 KiB pages), `kPhysicalViewC0 0xC0000000`
  (16 MiB), `kPhysicalViewE0 0xE0000000` (4 KiB), `kPhysicalSize 0x20000000`.
- `page_heap.h`:
  ```cpp
  enum PageProtect : uint32_t { /* Xbox PAGE_* values: NOACCESS 0x01, READONLY 0x02, READWRITE 0x04,
     EXECUTE 0x10, EXECUTE_READ 0x20, EXECUTE_READWRITE 0x40, GUARD 0x100, NOCACHE 0x200, WRITECOMBINE 0x400 */ };
  struct RegionInfo { uint32_t base, allocation_base, size; uint32_t protect, allocation_protect;
                      enum State { Free, Reserved, Committed } state; };
  class PageHeap {
   public:
    PageHeap(uint32_t base, uint32_t end, uint32_t page_size);
    Result<uint32_t> reserve(uint32_t size, uint32_t alignment, bool top_down, std::optional<uint32_t> fixed_base);
    Result<void> commit(uint32_t addr, uint32_t size, uint32_t protect);     // reports newly committed ranges
    Result<void> decommit(uint32_t addr, uint32_t size);
    Result<void> release(uint32_t allocation_base);                          // whole region only
    Result<RegionInfo> query(uint32_t addr) const;
    Result<uint32_t> allocation_size(uint32_t allocation_base) const;
    Result<void> protect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t* old_protect);
    void set_commit_hook(std::function<void(uint32_t addr, uint32_t size, bool committed)> hook);  // zeroing / mprotect
  };
  ```
  One state byte per page + per-page allocation base, mutex inside. Errors carry the NTSTATUS in
  `Error::message`-independent form: `enum class HeapError` mapped later in Task 13 (Result error code
  `InvalidFormat` = bad argument, `OutOfRange` = no space / conflicting, `NotFound` = not allocated).

- [ ] **Step 1: Write the failing tests:** `reserve bottom-up and top-down`, `alignment is honoured`,
  `fixed base conflicts with existing region`, `commit inside reserved region calls hook`, `query reports
  state and allocation base`, **`page heap edge cases`** (size 0 → error, size > region → OutOfRange,
  unaligned fixed base rounded down to page as Xenia does, double reserve of same range → OutOfRange,
  release with non-base address → NotFound).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(runtime): add the guest memory map and page heap`.

### Task 12: Physical heap and views

**Files:** Create `src/runtime/memory/physical_heap.{h,cpp}`, `tests/test_physical_heap.cpp`.

**Interfaces:**
- ```cpp
  class PhysicalHeap {
   public:
    PhysicalHeap();  // 512 MiB physical space
    // page_size from protect bits (Xbox MEM_LARGE_PAGES/16MB_PAGES semantics, see Xenia MmAllocatePhysicalMemoryEx)
    Result<uint32_t> allocate(uint32_t size, uint32_t protect, uint32_t min_phys, uint32_t max_phys, uint32_t alignment);
    Result<void> free(uint32_t guest_address);
    static uint32_t physical_address(uint32_t guest_address);  // spec §6.2 (& 0x1FFFFFFF, E0 view offset per Xenia)
    Result<uint32_t> allocation_size(uint32_t guest_address) const;
  };
  ```
  Returns view base + physical offset; top-down within `[min, max]`; one view per physical page.

- [ ] **Step 1: Write the failing tests:** `allocates top-down in range`, `honours alignment`,
  `view follows page size`, `physical_address round trip`, `free then reuse`, `out of physical memory`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(runtime): add the physical memory heap`.

### Task 13: Memory imports, null guard, `--strict-memory`

**Files:**
- Create: `src/runtime/kernel/exports/memory.cpp`, `tests/test_kernel_memory.cpp`
- Modify: `src/runtime/kernel/kernel.{h,cpp}` (heaps), `src/runtime/guest_memory.{h,cpp}`
  (`Result<void> protect(uint32_t addr, uint32_t size, int prot)`), `src/runtime/main.cpp`
  (`--strict-memory`), `docs/architecture.md` (memory map table replaced by spec §6.1)

**Interfaces:**
- `Kernel` gains `PageHeap& heap4k(); PageHeap& heap64k(); PhysicalHeap& physical(); uint32_t pool_alloc(uint32_t
  bytes, uint32_t tag); void pool_free(uint32_t addr);` (pool = 4K heap allocations, 16-byte aligned
  sub-allocation is out of scope: page-granular pool is acceptable).
- Imports (`LR_KERNEL`): `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtProtectVirtualMemory`,
  `NtQueryVirtualMemory`, `MmAllocatePhysicalMemoryEx`, `MmFreePhysicalMemory`, `MmGetPhysicalAddress`,
  `MmQueryAddressProtect`, `MmSetAddressProtect`, `MmQueryAllocationSize`, `MmQueryStatistics`,
  `ExAllocatePool`, `ExAllocatePoolTypeWithTag`, `ExFreePool`, `XamAlloc`, `XamAllocEx`, `XamFree`;
  `MmMapIoSpace` stays missing (M3). Argument order/semantics from Xenia `xboxkrnl_memory.cc`.
- Commit hook zeroes newly committed pages (unless `MEM_NOZERO`); `0..kNullGuardEnd` is `PROT_NONE` at
  init; with `strict_memory` the hook `mprotect`s decommitted/reserved pages `PROT_NONE`.

- [ ] **Step 1: Write the failing tests** (`KernelFixture`): `NtAllocateVirtualMemory reserve+commit writes
  base and size back` (in/out pointers are guest `be<uint32_t>*`), `MEM_LARGE_PAGES goes to the 64K heap`,
  `NtFreeVirtualMemory MEM_RELEASE`, `NtQueryVirtualMemory fills MEMORY_BASIC_INFORMATION`,
  `MmAllocatePhysicalMemoryEx returns a C0 view address for 16 MB pages`, `ExAllocatePoolTypeWithTag zeroed`,
  `null guard faults` (fork + touch `base+0` → SIGSEGV report "at guest 0x00000000").
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): implement virtual, physical and pool memory imports`.

### Task 14: Loader v2 — XEX header copy, variable imports, TLS and stack info, `inspect --imports`

**Files:**
- Create: `src/runtime/kernel/variables.{h,cpp}`, `tests/test_kernel_variables.cpp`
- Modify: `src/formats/xex2.{h,cpp}` (import records, TLS info `0x00020104`, default stack size
  `0x00020200`), `src/runtime/loader.{h,cpp}`, `src/runtime/fault.cpp`, `src/runtime/main.cpp`,
  `src/launcher/cli/main.cpp` (`inspect --imports`), `tests/test_xex2.cpp`, `tests/test_loader.cpp`,
  `tests/support/xex2_builder.h`, `docs/formats/xex2.md`

**Interfaces:**
- `formats::Xex2File` gains `struct ImportLibrary { std::string name; std::vector<uint32_t> records; }` (record
  guest addresses from header `0x000103FF`), `std::optional<TlsInfo> tls` (`slot_count, raw_data_address,
  data_size, raw_data_size`), `std::optional<uint32_t> default_stack_size`.
- `LoadedImage` gains `uint32_t header_guest; uint32_t ldr_entry; std::optional<TlsInfo> tls;
  uint32_t stack_size /* default 256 KiB if absent */`. The loader copies the XEX header to
  `kKernelDataBase` and builds a minimal `LDR_DATA_TABLE_ENTRY` after it (fields per Xenia `X_LDR_DATA_TABLE_ENTRY`,
  `xex_header_base` pointing to the copy).
- `variables.h`: `Result<void> resolve_variable_imports(GuestMemory&, const LoadedImage&, const formats::Xex2File&)`.
  For every record: the slot value in guest memory is the record **byte-swapped by `ParseImage`** (native
  order): `type = v >> 24`, `ordinal = v & 0xFFFF` — check this against `XenonUtils/xex.cpp:329-342`. For
  `type == 0`: if the ordinal names a known function (thunk exists for it, type-1 record of the same
  library/ordinal) write the BE thunk address; else if a kernel variable is registered for that name write
  its block address; else write the address of a fresh `PROT_NONE` page in the kernel data area and record
  `{page, name}` so `format_fault` prints `missing kernel variable <name>`. Kernel variables register with
  `void register_variable(const char* name, uint32_t size, void (*init)(uint8_t* base, uint32_t guest))`;
  M2 registers at least `XexExecutableModuleHandle` (→ LDR entry), `ExLoadedCommandLine`,
  `ExLoadedImageName`, `KeTimeStampBundle`, `XboxHardwareInfo`, `XboxKrnlVersion`, `KeDebugMonitorData`,
  `ExThreadObjectType`, `ExEventObjectType`, `ExSemaphoreObjectType`, `ExMutantObjectType`,
  `ExTimerObjectType`, `IoFileObjectType` (contents per Xenia).
- `inspect --imports <xex>` prints `library ordinal name kind(function|variable)`; `--coverage` adds rows of
  kind `variable`.

- [ ] **Step 1: Write the failing tests:** `xex2 parses import records, tls and stack size`
  (`xex2_builder`), `loader copies header to kernel data`, `variable import gets its block address`,
  `unknown variable points to a guard page and fault names it` (fork), `type-0 record of a function gets the
  thunk address`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(runtime): resolve kernel variable imports and expose XEX header data`.

### Task 15: Objects and handles

**Files:** Create `src/runtime/kernel/objects.{h,cpp}`, `src/runtime/kernel/exports/objects.cpp`,
`tests/test_kernel_objects.cpp`.

**Interfaces:**
- ```cpp
  enum class ObjectKind { Event, Semaphore, Mutant, Timer, Thread, File, Enumerator, NotifyListener, Other };
  class KObject : public std::enable_shared_from_this<KObject> {
   public: explicit KObject(ObjectKind k); virtual ~KObject(); ObjectKind kind() const;
     uint32_t guest_header() const;  // guest address of its DISPATCHER_HEADER, 0 if not waitable
     void set_guest_header(uint32_t);
  };
  inline constexpr uint32_t kHandleBase = 0xF8000000, kCurrentThread = 0xFFFFFFFE, kCurrentProcess = 0xFFFFFFFF;
  class ObjectTable {
   public:
    uint32_t add(std::shared_ptr<KObject>);                 // kHandleBase + 4*slot
    std::shared_ptr<KObject> get(uint32_t handle) const;    // kCurrentThread → current_thread() once Task 17 lands; nullptr before
    uint32_t close(uint32_t handle);                        // NTSTATUS
    Result<uint32_t> duplicate(uint32_t handle);
    std::shared_ptr<KObject> by_guest_header(uint32_t guest) const;
  };
  ```
  `Kernel` gains `ObjectTable& objects()`. Imports: `NtClose`, `NtDuplicateObject`,
  `ObReferenceObjectByHandle` (writes the object's guest header address; checks the type pointer against the
  `Ex*ObjectType` variables from Task 14), `ObDereferenceObject`, `ObOpenObjectByPointer`; `ObOpenObjectByName`
  and `ObCreateObject` → `LR_KERNEL_STUB(…, STATUS_NOT_IMPLEMENTED)`. (`ObCreateSymbolicLink`/`ObDeleteSymbolicLink`
  are Task 21.)

- [ ] **Step 1: Write the failing tests:** `handles start at 0xF8000000 and step by 4`, **`double close`**
  (second → `STATUS_INVALID_HANDLE`), `duplicate keeps object alive after original close`,
  `ObReferenceObjectByHandle with wrong type → STATUS_OBJECT_TYPE_MISMATCH`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): add the object table and handle imports`.

### Task 16: Dispatcher, events, semaphores, mutants, waits

**Files:** Create `src/runtime/kernel/dispatcher.{h,cpp}`, `src/runtime/kernel/exports/sync.cpp`,
`tests/test_kernel_sync.cpp`.

**Interfaces:**
- ```cpp
  struct Timeout { enum Kind { Infinite, Poll, Relative, Absolute } kind; int64_t ticks_100ns; };
  Timeout timeout_from_guest(GuestPtr<be<int64_t>> p);  // null → Infinite, 0 → Poll, <0 → Relative, >0 → Absolute
  enum class WaitType { Any, All };
  class Dispatcher {
   public:
    // headers: guest addresses of DISPATCHER_HEADERs. Returns STATUS_WAIT_0+i, STATUS_ABANDONED_WAIT_0+i,
    // STATUS_TIMEOUT, STATUS_USER_APC or STATUS_ALERTED. Consumes auto-reset events / semaphore counts / mutant ownership.
    uint32_t wait(std::span<const uint32_t> headers, WaitType type, Timeout t, bool alertable);
    void signal(uint32_t header);    // after the caller changed signal_state; wakes satisfiable waiters
    void wake_for_apc(uint32_t thread_id);
    std::vector<WaitInfo> snapshot() const;   // for the deadlock detector (Task 20)
  };
  ```
  One global mutex; waiters keyed by header address; per-waiter condvar; signal state read/written in guest
  memory (`X_DISPATCHER_HEADER::signal_state`), types 0/1 event, 2 mutant, 5 semaphore, 6 thread, 8/9 timer.
  `Kernel` gains `Dispatcher& dispatcher()`.
- Imports: `KeInitializeMutant`, `KeReleaseMutant`, `KeSetEvent`, `KeResetEvent`, `KeWaitForSingleObject`,
  `KeWaitForMultipleObjects`, `NtCreateEvent`, `NtSetEvent`, `NtClearEvent`, `NtPulseEvent`,
  `NtCreateSemaphore`, `NtReleaseSemaphore`, `NtWaitForSingleObjectEx`, `NtWaitForMultipleObjectsEx`,
  `NtSignalAndWaitForSingleObjectEx`. `Nt*Create*` allocate the object body (header) from the pool.

- [ ] **Step 1: Write the failing tests:** `manual-reset event releases all waiters`, `auto-reset event
  releases one`, `semaphore counts and limit` (`STATUS_SEMAPHORE_LIMIT_EXCEEDED`), `mutant recursion and
  release by non-owner → STATUS_MUTANT_NOT_OWNED`, `wait any returns index`, `wait all`, `relative timeout
  returns STATUS_TIMEOUT within 50 ms of 20 ms`, `poll returns immediately`, `event initialized by guest
  memory only (no handle) can be waited on`, **`wait on file handle`** → `STATUS_OBJECT_TYPE_MISMATCH`
  (a `KObject` of kind `File` with no header).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS (also under `ctest --repeat until-fail:20 -R sync`).
- [ ] **Step 5: Commit** `feat(kernel): add the dispatcher and event, semaphore and mutant imports`.

### Task 17: Guest threads, TLS, APCs, `GuestThreadExit`

**Files:**
- Create: `src/runtime/kernel/threads.{h,cpp}`, `src/runtime/kernel/exports/threads.cpp`,
  `tests/test_kernel_threads.cpp`
- Modify: `src/runtime/guest_thread.{h,cpp}` (remove `init_guest_pcr`, fixed `kGuestPcrAddress`/
  `kGuestTlsAddress`/`kGuestThreadAddress`/`kGuestStackBase`; `run_guest` replaced), `src/runtime/main.cpp`,
  `tests/test_guest_thread.cpp`, `AGENTS.md` (second allowed exception), `docs/adr/0004-…` (GuestThreadExit,
  exit codes 5/6), `docs/architecture.md` (Threads section)

**Interfaces:**
- ```cpp
  struct ThreadParams { uint32_t stack_size; uint32_t start; uint32_t context; uint32_t xapi_start;  // 0 = none
                        bool suspended; uint8_t cpu_mask; };
  class GuestThread : public KObject {
   public: uint32_t id() const; uint32_t pcr() const; uint32_t kthread() const; uint32_t handle() const;
     uint32_t exit_code() const; void request_suspend(); void resume(); /* APC queue */ };
  Result<std::shared_ptr<GuestThread>> create_guest_thread(const ThreadParams&);
  GuestThread* current_thread();                  // nullptr on non-guest host threads
  struct GuestThreadExit { uint32_t code; };      // thrown by ExTerminateThread, caught at thread top
  int run_main_thread(uint32_t entry, uint32_t stack_size);  // returns process exit code
  ```
  Per thread, from the pool: PCR (`X_KPCR`, r13), KTHREAD (header type 6), TLS block = XEX TLS template
  (Task 14) + `KeTlsAlloc` slots (64 slots, `be<uint32_t>`); stack from `kStacksBase` region with a
  `PROT_NONE` guard page below. Thread top catches `UnimplementedImport` (report → `_exit(3)`),
  `GuestThreadExit` (normal end), anything else (`_exit(4)`); on end sets the KTHREAD signal state and
  `Dispatcher::signal`. Main thread: same path with `start = entry`; when it returns the process exits 0.
  Suspend of another running thread is cooperative: checked in every thunk via `GuestCall` entry hook.
- Imports: `ExCreateThread`, `ExTerminateThread`, `ExRegisterTitleTerminateNotification`,
  `KeGetCurrentProcessType`/`KeSetCurrentProcessType` (default 1), `KeTlsAlloc`/`Free`/`GetValue`/`SetValue`,
  `KeResumeThread`, `NtResumeThread`, `NtSuspendThread`, `NtYieldExecution`, `KeDelayExecutionThread`
  (alertable), `KeQueryBasePriorityThread`, `KeSetBasePriorityThread`, `KeSetAffinityThread`,
  `KeSetDisableBoostThread`, `ObLookupThreadByThreadId`, `NtQueueApcThread`, `KiApcNormalRoutineNop`,
  `KeEnterCriticalRegion`/`KeLeaveCriticalRegion` (APC-disable counter),
  `KeSaveFloatingPointState`/`KeRestoreFloatingPointState`/`KeEnableFpuExceptions` (stubs, success).

- [ ] **Step 1: Write the failing tests** (fake game for the guest side, `KernelFixture` for the rest):
  `ExCreateThread runs start(context) and returns handle and id`, `suspended thread waits for resume`,
  `xapi start routine wraps start`, `thread exit code via wait + KTHREAD signal`, `TLS template copied per
  thread`, `KeTls slots are per thread`, `ExTerminateThread ends the thread with its code`, `alertable
  delay runs queued APC and returns STATUS_USER_APC`, `missing import on a secondary thread exits 3 with
  report` (report has a `Thread: <id>` line, spec §11), **`close handle of running thread`** (wait on a duplicate still returns `STATUS_WAIT_0` after the
  thread ends; ASan clean).
- [ ] **Step 2:** FAIL. **Step 3:** Implement; `main.cpp` calls `run_main_thread(entry, image.stack_size)`.
- [ ] **Step 4:** PASS, including all previous fake-game tests.
- [ ] **Step 5: Commit** `feat(kernel): add guest threads with per-thread PCR, KTHREAD and TLS`.

### Task 18: Timers, DPCs, spinlocks, IRQL, time

**Files:** Create `src/runtime/kernel/timers.{h,cpp}`, `src/runtime/kernel/exports/time.cpp`,
`tests/test_kernel_time.cpp`.

**Interfaces:**
- `uint64_t calibrate_tsc_hz();` (measure `__rdtsc()` against `CLOCK_MONOTONIC` over 50 ms at init; cached in
  `Kernel`). One kernel worker thread runs DPCs and timer expirations and updates `KeTimeStampBundle`
  (if registered, Task 14) every 1 ms.
- Imports: `KeQueryPerformanceFrequency` (= calibrated TSC Hz), `KeQuerySystemTime` (host time as FILETIME,
  BE), `RtlTimeToTimeFields`, `RtlTimeFieldsToTime`, `NtCreateTimer`, `NtSetTimerEx` (due time, period,
  APC routine), `NtCancelTimer`, `KeInitializeDpc`, `KeInsertQueueDpc` (runs routine on the worker via
  `call_guest`), `KfAcquireSpinLock`/`KfReleaseSpinLock`/`KeAcquireSpinLockAtRaisedIrql`/
  `KeReleaseSpinLockFromRaisedIrql` (CAS on the guest word with the thread id, returns/restores per-thread
  IRQL).

- [ ] **Step 1: Write the failing tests:** `performance frequency matches rdtsc rate within 2%`,
  `system time is close to host clock`, `time fields round trip (2026-10-16 12:34:56.789)`, `periodic timer
  signals and requeues`, `cancel timer`, `DPC routine runs once on worker`, `spinlock excludes two threads`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): add timers, DPCs, spinlocks and time imports`.

### Task 19: Critical sections

**Files:** Create `src/runtime/kernel/exports/critsec.cpp`, `tests/test_kernel_critsec.cpp`.

**Interfaces:**
- Imports: `RtlInitializeCriticalSection`, `RtlInitializeCriticalSectionAndSpinCount`,
  `RtlEnterCriticalSection`, `RtlLeaveCriticalSection`, `RtlTryEnterCriticalSection` over
  `X_RTL_CRITICAL_SECTION` in guest memory: `lock_count` −1 when free; owner = KTHREAD guest address; spin
  `spin_count` times then atomic increment of `lock_count` (host `std::atomic_ref<uint32_t>` on the BE word,
  compatible with the generated `lwarx`/`stwcx.` CAS) and wait on the embedded event header via `Dispatcher`.

- [ ] **Step 1: Write the failing tests:** `recursive enter by owner`, `try enter fails when owned by
  another thread`, `four threads incrementing a counter 10000 times each under the section reach 40000`,
  `leave by non-owner is guest fatal` (fork).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): implement Rtl critical sections over guest memory`.

### Task 20: Deadlock detector, debug, bugcheck, HAL, SEH minimum, `--trace-imports`

**Files:**
- Create: `src/runtime/kernel/trace.{h,cpp}`, `src/runtime/kernel/exports/debug.cpp`,
  `tests/test_kernel_debug.cpp`
- Modify: `src/runtime/kernel/dispatcher.cpp`, `src/runtime/main.cpp` (`--trace-imports`, `--trace-file`),
  `src/pipeline/pipeline.cpp` (`launch` always passes `--trace-file <root>/work/logs/game-trace.log`),
  `tests/test_fake_game.cpp`, `tests/test_build_stage.cpp`, `docs/architecture.md`

**Interfaces:**
- Deadlock: a watcher (on the kernel worker) checks every second; when every live guest thread is in an
  infinite wait, no timer/DPC/APC is pending, for > 10 s, it prints each thread id, the waited header
  addresses and their object kinds, plus the trail, then `_exit(kExitDeadlock)`.
- Trace: with `--trace-imports`, each thunk call appends `[<tid>] <Name> r3=0x.. r4=0x.. r5=0x.. r6=0x.. ->
  0x..` to the trace file (buffered, flushed on exit and on fatal paths).
- Imports: `DbgPrint` (uses Task 24's printf; until then prints the format string), `DbgBreakPoint` (log,
  continue), `EtxProducerRegister`/`Unregister`/`Log` (stubs, success), `KeBugCheck`/`KeBugCheckEx`
  (`guest_fatal`), `HalReturnToFirmware` (exit 0), `KeLockL2`/`KeUnlockL2` (stubs), `RtlCaptureContext`
  (fills guest CONTEXT from `GuestCall` registers), `RtlRaiseException` (`guest_fatal` with the exception
  code), `RtlUnwind` and `__C_specific_handler` (`guest_fatal`, named).

- [ ] **Step 1: Write the failing tests:** fake game `two guest threads waiting on each other exit 6 with
  both waits listed` (detector delay overridable by env `LR_DEADLOCK_SECONDS=1` for tests), fake game
  `--trace-imports writes one line per call`, `launch passes --trace-file`, `KeBugCheckEx exits 5 with code`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): add deadlock detection, import tracing and debug imports`.

### Task 21: VFS — symlinks, mounts, case-insensitive resolution, `user/`

**Files:**
- Create: `src/runtime/kernel/vfs.{h,cpp}`, `tests/test_kernel_vfs.cpp`
- Modify: `src/common/paths.{h,cpp}` (`DataPaths::user`), `src/pipeline/pipeline.cpp` (`launch` passes
  `--user <root>/user`), `src/runtime/main.cpp` (`--data` now used, `--user`), `tests/test_paths.cpp`,
  `docs/pipeline.md` (data folder layout: `user/` is never deleted)

**Interfaces:**
- ```cpp
  struct ResolvedPath { std::filesystem::path host; bool read_only; bool exists; bool is_directory; };
  class Vfs {
   public:
    void mount(std::string device, std::filesystem::path host_dir, bool read_only);  // e.g. "\\Device\\Cdrom0"
    uint32_t create_symlink(std::string_view link, std::string_view target);          // NTSTATUS
    uint32_t delete_symlink(std::string_view link);
    // guest path like "game:\\Data\\x.arc", "\\??\\game:\\…", "\\Device\\Cdrom0\\…"; relative to root if given
    Result<ResolvedPath> resolve(std::string_view guest_path, const std::filesystem::path* root_dir = nullptr) const;
  };
  ```
  Defaults: `\Device\Cdrom0` → `<game>` (ro), `\Device\Harddisk0\Cache0` → `<user>/cache` (rw); symlinks
  `game:`, `d:` → `\Device\Cdrom0`, `cache:` → `\Device\Harddisk0\Cache0`. Lowercase index per read-only
  directory built on first lookup; writable dirs resolved component-wise case-insensitively; new names keep
  the guest spelling. `..` that leaves the mount → `NotFound` (mapped to `STATUS_OBJECT_PATH_NOT_FOUND`).
  `Kernel` gains `Vfs& vfs()`; imports `ObCreateSymbolicLink`, `ObDeleteSymbolicLink`.

- [ ] **Step 1: Write the failing tests:** **`resolve handles odd guest paths`** (`GAME:\DATA\X.ARC` finds
  `Data/x.arc`; `\??\game:\Data\x.arc`; `game:\\Data\\\\x.arc`; trailing `\` on a directory;
  `game:\..\..\etc\passwd` → not found; mount root under a directory named `Jogos Teste`), `writes to game:
  are read-only`, `new file under cache: keeps guest spelling`, `symlink create and delete`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): add the virtual file system and user data folder`.

### Task 22: File imports

**Files:** Create `src/runtime/kernel/exports/files.cpp`, `tests/test_kernel_files.cpp`.

**Interfaces:**
- `class FileObject : public KObject` (host fd, position, path, directory enumeration state).
- Imports (`LR_KERNEL`, semantics from Xenia `xboxkrnl_io.cc`): `NtCreateFile` (9 args; `X_OBJECT_ATTRIBUTES`
  → `X_ANSI_STRING`; dispositions; `IO_STATUS_BLOCK.information` = `FILE_OPENED 1`/`FILE_CREATED 2`/
  `FILE_OVERWRITTEN 3`), `NtOpenFile`, `NtReadFile`, `NtReadFileScatter`, `NtWriteFile` (8 args; null byte
  offset = current position; synchronous; signal the event and queue the APC when given),
  `NtQueryInformationFile` (classes 5 Standard, 6 Internal, 14 Position, 34 NetworkOpen; others →
  `STATUS_INVALID_PARAMETER`), `NtSetInformationFile` (Position 14, EndOfFile 20, Disposition 13),
  `NtQueryFullAttributesFile`, `NtQueryDirectoryFile` (wildcards `*`/`?`, restart flag, `STATUS_NO_MORE_FILES`),
  `NtQueryVolumeInformationFile` (size 3, attribute 5), `NtDeviceIoControlFile` (stub
  `STATUS_NOT_IMPLEMENTED`), `FscGetCacheElementCount`/`FscSetCacheElementCount` (stubs).
  `NtWriteFileGather`/`NtFlushBuffersFile` stay missing (M4).

- [ ] **Step 1: Write the failing tests:** `open existing file reports FILE_OPENED`, `open missing →
  STATUS_OBJECT_NAME_NOT_FOUND`, `read at offset and at current position`, `read past end →
  STATUS_END_OF_FILE`, **`NtReadFile with null io status block`** → `STATUS_INVALID_PARAMETER`, `read into
  buffer crossing 4 GiB → STATUS_ACCESS_VIOLATION`, `query standard information size`, `directory enumeration
  with *.arc`, `create file in cache: then reopen`, `write to game: → STATUS_ACCESS_DENIED`, `read signals
  event`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): implement file I/O imports`.

### Task 23: LZX decompression (`LDI*`)

**Files:** Create `src/runtime/kernel/exports/lzx.cpp`, `tests/test_kernel_lzx.cpp`; modify
`src/runtime/CMakeLists.txt` (include path to libmspack headers inside the XenonRecomp submodule).

**Interfaces:**
- Imports: `LDICreateDecompression`, `LDIDecompress`, `LDIResetDecompression`, `LDIDestroyDecompression`
  (argument layout per Xenia's implementation, read only), built on `lzxd_init`/`lzxd_decompress` from the
  `lzxd.c` already linked in `libXenonUtils.a`, with an in-memory `mspack_system`. No new license
  obligation (ADR 0002 already covers libmspack); add a sentence to ADR 0002's libmspack row: "também usada
  pelo kernel (LDI*)".

- [ ] **Step 1: Write the failing tests:** `uncompressed LZX block round trips` (hand-built LZX stream with
  block type 3 and 1 KiB of known bytes), `reset allows a second independent stream`, `corrupt input
  returns an error status, no crash`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS (ASan clean).
- [ ] **Step 5: Commit** `feat(kernel): implement LDI LZX decompression on libmspack`.

### Task 24: Rtl strings and guest printf

**Files:** Create `src/runtime/kernel/guest_printf.{h,cpp}`, `src/runtime/kernel/exports/rtl.cpp`,
`tests/test_kernel_rtl.cpp`; modify `exports/debug.cpp` (`DbgPrint` formats).

**Interfaces:**
- ```cpp
  class GuestArgs { public: virtual uint64_t next_int(bool wide64) = 0; virtual double next_double() = 0; };
  class CallArgs : public GuestArgs { public: CallArgs(const GuestCall&, int first_index); };  // regs then stack
  class VaListArgs : public GuestArgs { public: VaListArgs(uint8_t* base, uint32_t va_list); };  // layout per Xenia
  std::string guest_format(uint8_t* base, std::string_view fmt, GuestArgs& args);      // narrow
  std::u16string guest_format_wide(uint8_t* base, std::u16string_view fmt, GuestArgs& args);
  ```
  Supports flags `-+ #0`, width/precision incl. `*`, length `h l ll I64`, conversions `d i u x X o c s S
  ls p f e g %`; `%s` reads guest `char*`, `%S`/`%ls` guest BE UTF-16.
- Imports: `RtlInitAnsiString`, `RtlInitUnicodeString`, `RtlFreeAnsiString`, `RtlUnicodeStringToAnsiString`,
  `RtlMultiByteToUnicodeN`, `RtlUnicodeToMultiByteN`, `RtlCompareMemory`, `RtlCompareMemoryUlong`,
  `RtlFillMemoryUlong`, `RtlNtStatusToDosError` (table of the statuses in `status.h`), `sprintf`,
  `_snprintf`, `vsprintf`, `_vsnprintf`, `swprintf`, `_snwprintf`, `vswprintf`, `_vscwprintf`.

- [ ] **Step 1: Write the failing tests:** `format integers with width and padding` (`"%08X"`, `0x1A` →
  `"0000001A"`), `format guest string and wide string`, `format double`, `star width from args`,
  `_snprintf truncates and returns -1 when full`, `vsprintf reads guest va_list`, `RtlInitAnsiString lengths`,
  `unicode to multibyte round trip`, `RtlNtStatusToDosError(STATUS_OBJECT_NAME_NOT_FOUND) == 2`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): add Rtl strings and a guest printf interpreter`.

### Task 25: XEX queries, config, crypto

**Files:** Create `src/common/sha1.{h,cpp}`, `src/runtime/kernel/exports/{xex,config,crypto}.cpp`,
`tests/test_sha1.cpp`, `tests/test_kernel_xex_config.cpp`; modify `docs/architecture.md` (fixed config values).

**Interfaces:**
- `lr::Sha1` with the same shape as `lr::Sha256` (`update`, `finish`, `to_hex`).
- Imports: `RtlImageXexHeaderField` (walks the guest copy of the header; value vs pointer by key low byte),
  `XexGetModuleHandle` (NULL/"default.xex" → LDR entry; "xboxkrnl.exe"/"xam.xex" → fake LDR entries),
  `XexCheckExecutablePrivilege`, `XexGetModuleSection`, `XexGetProcedureAddress` (ordinal → thunk address),
  `XamLoaderGetLaunchDataSize`/`XamLoaderGetLaunchData` (`ERROR_NOT_FOUND`), `XamLoaderTerminateTitle`
  (exit 0); `ExGetXConfigSetting`, `XGetLanguage` (from `LANG`: en 1, ja 2, de 3, fr 4, es 5, it 6, ko 7,
  zh_TW 8, pt 9, zh_CN 10, pl 11, ru 12; fallback 1), `XGetGameRegion`, `XamGetCurrentTitleId`
  (`0x584111F7`), `XamGetExecutionId`, `XamGetSystemVersion` (region and dash version: Xenia defaults,
  documented in `architecture.md`); `XeCryptSha`, `XeCryptShaInit/Update/Final`, `XeCryptSha256Init/Update/Final`,
  `XeCryptRotSumSha`, `XeCryptRandom` (`getrandom`). `XexLoadImage`, `XamLoaderLaunchTitle` etc. stay
  missing (M5); `XeKeys*`, `XeCryptBnQwBeSigVerify` stay missing (M4).

- [ ] **Step 1: Write the failing tests:** `sha1 FIPS vectors` ("abc" → `a9993e36…`), `RtlImageXexHeaderField
  returns entry point value`, `XexGetModuleHandle(NULL) is the LDR entry`, `XGetLanguage maps pt_BR.UTF-8 to 9`,
  `XamGetCurrentTitleId`, `XeCryptSha over two buffers equals sha1 of concatenation`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): add XEX queries, console config and crypto imports`.

### Task 26: Minimal XAM, video query stubs, absent-feature stubs

**Files:** Create `src/runtime/kernel/exports/{xam,video,net}.cpp`, `tests/test_kernel_xam.cpp`.

**Interfaces:**
- Real: `XamCreateEnumeratorHandle`, `XamEnumerate` (`ERROR_NO_MORE_FILES` when empty),
  `XamGetPrivateEnumStructureFromHandle`, `XamGetOverlappedResult` (reads `XOVERLAPPED` InternalLow/High,
  waits on its event), `XamTaskSchedule` (callback on a new guest thread via Task 17), `XamNotifyCreateListener`
  (waitable listener object), `XNotifyGetNext` (delivers queued startup notifications — sign-in changed,
  storage devices changed — then FALSE), `XNotifyPositionUI` (no-op); user: one local user "Player",
  XUID `0xE000000000000001`, signed in (state 1, offline): `XamUserGetSigninState`, `XamUserGetXUID`,
  `XamUserGetName`, `XamUserGetSigninInfo`, `XamUserReadProfileSettings` (defaults, no stored profile);
  content probes: `XamContentGetDeviceState` (`ERROR_DEVICE_NOT_CONNECTED`), `XamContentCreateDeviceEnumerator`
  (empty), `XamContentGetLicenseMask` (full license).
- Stubs (`LR_KERNEL_STUB`, value + reason comment): video queries `XGetVideoMode`, `VdQueryVideoMode`
  (fill the out struct: 1280x720, widescreen, HDMI, 60 Hz — `LR_KERNEL` since they write structs),
  `XGetAVPack` (HDMI), `XGetVideoCapabilities`, `VdQueryVideoFlags`, `VdGetCurrentDisplayInformation`,
  `VdGetCurrentDisplayGamma`, `VdIsHSIOTrainingSucceeded` (1); network `NetDll_WSAStartup`,
  `NetDll_XNetStartup` (success; remember NetDll_* take a leading caller argument in r3),
  `NetDll_XNetGetEthernetLinkStatus` (0 = no link); `XamIsUIActive` (0), `XamShowDirtyDiscErrorUI`
  (`guest_fatal("dirty disc")`); audio/input probes reached at startup (`XAudioGetSpeakerConfig`,
  `XamInputGetCapabilities` → `ERROR_DEVICE_NOT_CONNECTED`). Every other import keeps `missing` until the
  trace shows the game needs it (Task 28).

- [ ] **Step 1: Write the failing tests:** `enumerator returns items then no more files`, `overlapped result
  waits for completion`, `task runs callback on a guest thread` (fake game), `listener gets sign-in
  notification once`, `user is signed in offline as Player`, `XGetVideoMode reports 1280x720 widescreen`,
  `WSAStartup stub returns 0`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): add minimal XAM and stubs for absent hardware`.

### Task 27: Fake game end to end

**Files:** Modify `tests/support/fake_generated/*`, `tests/test_fake_game.cpp`.

**Interfaces:** Consumes Tasks 9–26 through thunks only (no direct kernel calls from the fake guest).

- [ ] **Step 1: Write the failing test** `fake guest boots like a game`: `sub_82000300` calls
  `NtAllocateVirtualMemory`, `RtlInitializeCriticalSection`, `NtCreateEvent`, `ExCreateThread` (worker
  `sub_82000380` opens `game:\data.bin` with `NtCreateFile`, reads 4 bytes, sets the event), waits with
  `KeWaitForSingleObject`, then calls `VdInitializeEngines` → exit 3 naming `VdInitializeEngines`, trace
  file lists the calls in order. The test writes `data.bin` into a temp `game/` passed with `--data`.
- [ ] **Step 2:** FAIL until all thunks/glue line up. **Step 3:** Fix what breaks. **Step 4:** PASS.
- [ ] **Step 5: Commit** `test(runtime): boot a fake guest through threads, files and waits`.

### Task 28: [ISO] Burn-down loop to the finish line

**Files:** whatever each iteration needs; `docs/kernel-coverage.md`; `docs/reverse-engineering/kernel-imports.md`
(replace the estimated startup order with the real one).

- [ ] **Step 1:** `container.sh install --iso /iso/game.iso` then `container.sh run -- --headless
  --trace-imports`. Read only the last 40 lines of `game-trace.log` and the exit report.
- [ ] **Step 2 (repeat):** For the stopping point — missing import (exit 3), fault (4), guest fatal (5) or
  deadlock (6): write a failing unit test reproducing the needed behavior (`KernelFixture` or fake game),
  implement (`LR_KERNEL` or a justified `LR_KERNEL_STUB`), PASS, commit
  `feat(kernel): <import or fix>`; re-run Step 1. A fault inside generated code is investigated with
  `.claude/skills/xbox360-re` (helpers, jump tables) before touching the kernel.
- [ ] **Step 3:** Stop when the run exits 3 on a GPU-creating `Vd*` import. Record the real startup order
  in `kernel-imports.md`.
- [ ] **Step 4:** Regenerate `docs/kernel-coverage.md` from `--coverage` (functions + variables).
- [ ] **Step 5: Commit** `docs: record the real startup import order and coverage`.

### Task 29: Acceptance, docs, AppImage

**Files:** `tests/test_integration_e2e.cpp`, `docs/roadmap.md`, `README*.md` (status table), `docs/architecture.md`,
`docs/adr/0004-…`, `docs/superpowers/specs/2026-10-16-legacyrecomp-m2-design.md` (status line).

- [ ] **Step 1:** Update the e2e integration test: exit code 3, first missing import matches `^Vd`, not a
  video query; run with a 300 s timeout. `LEGACYRECOMP_ISO=/iso/game.iso container.sh test -L integration`
  → PASS. [ISO]
- [ ] **Step 2:** `packaging/appimage/build.sh` inside the container; then in a fresh data root with a PATH
  lacking clang/cmake/ninja: AppImage `install` + `run -- --headless` → same exit 3 import. [ISO]
- [ ] **Step 3:** Docs: roadmap (M2 done, M3 next, remaining blockers), README status table in all seven
  languages, spec status "Implementado", ADR 0004 complete (every deviation from this plan, one entry each).
- [ ] **Step 4:** `container.sh test` → all unit tests PASS; `git status --short` shows no game files.
- [ ] **Step 5: Commit** `docs: close M2`. Then use superpowers:finishing-a-development-branch.
