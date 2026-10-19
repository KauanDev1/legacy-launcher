# LegacyRecomp M3 — Graphics (Xenos GPU on Vulkan) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The recompiled game opens a window and draws its intro and title screen with real textures and translated
shaders through an emulated Xenos GPU rendered with Vulkan.

**Architecture:** The game's own recompiled D3D code writes PM4 command buffers; a host GPU thread consumes the ring
buffer, keeps the Xenos register file, and drives a Vulkan backend (EDRAM as host render targets, texture cache,
vertex fetch). Shaders are translated at install time by a forked XenosRecomp from the ~70 containers in the XEX and
bound at runtime by the hash of the microcode the game loads. Before any graphics, the analyse stage learns the jump
tables and pointer-only functions that crashed the M3 spike.

**Tech Stack:** C++20, Clang, CMake/Ninja, SDL2, Vulkan 1.2 via volk + VMA (+ Vulkan-Headers), XenosRecomp fork
(patches in-repo), DXC (official pinned Linux release), Catch2, Mesa lavapipe in the dev container.

**Spec:** `docs/superpowers/specs/2026-10-19-legacyrecomp-m3-design.md`. Spike data: spec §2. M2 record:
`docs/adr/0004-m2-plan-deviations.md`.

## Global Constraints

- Everything in `AGENTS.md`: MIT + SPDX header on every new file; Xenia, UnleashedRecomp and MarathonRecomp are
  **reference only** — never copy code (clean-room: read, then write from your own understanding; ADR 0005 records
  this); `lr::Result<T>` on public APIs; English code/commits, Portuguese `docs/`; docs change with the code; never
  commit `*.iso`, `*.xex`, `*.xexp`, `work/`, generated `ppc_*`, **generated shader caches, screenshots of the game,
  or any game-derived bytes**.
- Branch `feat/m3`. Nothing is pushed. Every commit passes `tools/dev/container.sh test`. Trailer
  `Co-Authored-By: Claude <model> <noreply@anthropic.com>` naming the model that wrote the commit (ADR 0004 M18).
- New code: GPU core in `src/runtime/gpu/` (namespace `lr::gpu`, no Vulkan types); Vulkan backend in
  `src/runtime/gpu/vk/` (namespace `lr::gpu::vk`); kernel imports in `src/runtime/kernel/exports/video.cpp`
  (extend) and `audio.cpp`/`xam_ui.cpp` (new).
- Vulkan 1.2 minimum: `VK_KHR_swapchain`, `textureCompressionBC`, descriptor indexing (runtime arrays, partially
  bound, update-after-bind), `bufferDeviceAddress`. The loader is the system `libvulkan.so.1`, loaded by volk; never
  ship a loader or driver.
- Exit codes unchanged (0, 2, 3, 4, 5, 6). Unknown PM4 opcode or shader hash missing from the cache → `guest_fatal`
  (exit 5) naming it.
- Tasks marked **[ISO]** use the owner-confirmed ISO only through `tools/dev/container.sh`; read only exit reports,
  counts, short log tails; never game data in context or repo.
- Tests that need a Vulkan device are tagged `[unit][vulkan]` and `SKIP` when no ICD is present (the container has
  lavapipe, so they run there).

## Review Focus

1. **Guest-controlled PM4 data** (packet counts past the buffer end, indirect buffers pointing outside guest memory,
   nested indirect buffers, zero-length packets) → a clear `guest_fatal`, never a host read out of bounds. Test:
   Task 8 `malformed packets are fatal, not out-of-bounds`.
2. **Physical addresses with no allocation** in GPU commands (texture/vertex/resolve addresses the guest never
   allocated) → fatal report with the physical address, not a read of zeros or garbage. Test: Task 9
   `untranslatable physical address is reported`.
3. **Host without a usable Vulkan device** (no loader, no ICD, missing BC or descriptor indexing) → `doctor` and the
   game say exactly what is missing and the game exits 2; `--headless` without Vulkan still fails clearly. Test:
   Task 17 `no Vulkan device gives a clear error`.
4. **Window closed / SIGTERM while the GPU thread is mid-frame** → orderly exit 0, no hang, no Vulkan validation
   error on teardown. Test: Task 17 `closing during rendering exits cleanly`.
5. **Texture/render-target sizes and formats at the edges** (1×1, non-power-of-two, max 8192, unsupported format) →
   correct upload or a named fatal for an unsupported format, never a silent black texture. Test: Task 19
   `edge-size and unsupported-format textures`.

---

## File Structure

```
tools/dev/Dockerfile                        # + mesa (lavapipe), vulkan-icd-loader, vulkan-validation-layers, vulkan-tools
thirdparty/{volk,VulkanMemoryAllocator,Vulkan-Headers,XenosRecomp}   # submodules (pinned)
thirdparty/patches/xenosrecomp/NNNN-*.patch                          # LegacyRecomp fork as patches
cmake/LrSubmodulePatches.cmake              # generalised from LrXenonRecompPatches.cmake (both submodules)
packaging/appimage/toolchain.lock           # + DXC release (sha256)
src/pipeline/switch_scan.{h,cpp}            # + relative halfword/byte jump tables
src/pipeline/stages/analyse_stage.cpp       # + pointer-only function discovery
src/pipeline/function_discovery.{h,cpp}     # new: data-table code pointers + uncovered bl targets
src/pipeline/stages/shaders_stage.{h,cpp}   # new stage between recompile and build
src/runtime/gpu/registers.{h,cpp}           # Xenos register file + names
src/runtime/gpu/pm4.{h,cpp}                 # packet decoding + CommandProcessor
src/runtime/gpu/backend.h                   # Backend interface (draw/resolve/swap/shader load)
src/runtime/gpu/null_backend.{h,cpp}        # headless no-render backend
src/runtime/gpu/gpu_system.{h,cpp}          # ring buffer thread, write-back, interrupts, MMIO window
src/runtime/gpu/shader_cache.{h,cpp}        # microcode hash -> SPIR-V entry (registered by main.cpp)
src/runtime/gpu/texture_format.{h,cpp}      # Xenos formats, untiling, endian swap (CPU reference)
src/runtime/gpu/vk/{device,presenter,edram,textures,pipelines,draw}.{h,cpp}
src/runtime/gpu/vk/shaders/*.hlsl           # our own compute/blit shaders, compiled to SPIR-V at build time
src/runtime/gpu/screenshot.{h,cpp}          # --screenshot-after (PNG via stb_image_write)
src/runtime/kernel/exports/{video,audio,xam_ui}.cpp
src/runtime/memory/physical_heap.{h,cpp}    # + guest_from_physical()
tests/test_switch_scan.cpp tests/test_function_discovery.cpp tests/test_gpu_*.cpp tests/test_shaders_stage.cpp
tests/support/pm4_builder.h tests/support/xenos_shader_builder.h
tests/test_integration_m3.cpp
docs/adr/0005-m3-plan-deviations.md
```

---

## Part A — Recompiler first

### Task 1: Relative halfword/byte jump tables

**Files:** Modify `src/pipeline/switch_scan.{h,cpp}`, `tests/test_switch_scan.cpp`, `docs/reverse-engineering/addresses.md`.

**Interfaces:**
- Consumes: `scan_switch_tables`, `SwitchTable{base, r, default_label, labels}`, the synthetic PPC encoder in
  `tests/test_switch_scan.cpp`.
- Produces: `scan_switch_tables` also returns tables of the forms
  `lhzx rT, rTable, rIdx(<<1)` / `lbzx rT, rTable, rIdx` → `add rT, rT, rBase` (optionally `rlwinm` scale) →
  `mtctr rT` → `bctr`, with labels `base + (entry << scale)`; same safety rules as today (bound from `cmplwi`/mask,
  every label 4-aligned inside the code section, `kMaxCases` 4096).

- [ ] **Step 1: Write the failing tests** with the encoder: `halfword relative table resolves 35 labels`,
  `byte relative table resolves 17 labels`, `relative table with scale 2`, `relative table without bound is
  unresolved`, `label outside code section rejects table`.
- [ ] **Step 2:** `tools/dev/container.sh test -R "relative table"` → FAIL.
- [ ] **Step 3:** Implement in `evaluate` (symbolic interpreter: model `lhzx`/`lbzx` table reads relative to a known
  base and `add` of two known terms).
- [ ] **Step 4:** PASS; full suite green.
- [ ] **Step 5: Commit** `feat(analyse): resolve relative halfword and byte jump tables`.

### Task 2: Functions reachable only through pointers

**Files:** Create `src/pipeline/function_discovery.{h,cpp}`, `tests/test_function_discovery.cpp`; modify
`src/pipeline/stages/analyse_stage.cpp`, `docs/pipeline.md`.

**Interfaces:**
- Consumes: `parse_pdata`, `fit_switch_functions` rules, the image sections (`SectionFlags_Code`).
- Produces:
  `std::vector<FunctionRange> discover_pointer_functions(std::span<const uint8_t> image, uint32_t base,
  const std::vector<CodeRange>& code, const std::vector<FunctionRange>& known)` — candidates are (a) 4-aligned BE
  words in data sections that point into code and start at an instruction that is not inside a known function, (b)
  `bl` targets not covered by a known function; each candidate's end is the first `blr`/`b` tail at the same nesting
  or the next known function start; rejected if it overlaps a known range or if a branch from outside lands inside
  it (same rules as `fit_switch_functions`). The analyse stage adds them to `functions.toml` and logs
  `pointer-only functions: N`.

- [ ] **Step 1: Write the failing tests:** `data table of code pointers yields functions` (synthetic image: 4 tiny
  functions without pdata + a table of their addresses in a data section), `uncovered bl target yields a function`,
  `candidate overlapping a pdata function is rejected`, `pointer into the middle of a function is ignored`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement and wire into analyse. **Step 4:** PASS, full suite.
- [ ] **Step 5: Commit** `feat(analyse): discover functions reachable only through pointers`.

### Task 3: Clear report for indirect calls without a function

**Files:** Modify `src/runtime/template/CMakeLists.txt` or `src/runtime/main.cpp` (define
`PPC_CALL_INDIRECT_FUNC` before `ppc_context.h`), `src/runtime/kernel/glue.{h,cpp}`, `tests/test_fake_game.cpp`,
`tests/support/fake_generated/*`.

**Interfaces:**
- Produces: `[[noreturn]] void lr::kernel::bad_indirect_call(uint32_t target, uint32_t lr)` → prints
  `Indirect call to 0x%08X without a recompiled function (from 0x%08X)` + trail and `_exit(kExitFault)`; the
  template's `PPC_CALL_INDIRECT_FUNC(x)` checks the slot (range + non-null) and calls it.

- [ ] **Step 1: Write the failing test** `fake guest indirect call to a hole reports the guest address` (exit 4,
  message names the target).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS; the staged-headers guard test still passes.
- [ ] **Step 5: Commit** `feat(runtime): report indirect calls to addresses without a function`.

### Task 4: [ISO] Spike crashes gone

- [ ] **Step 1:** `tools/dev/container.sh install --iso "<iso>" --force analyse`; read `analyse.log` summary lines
  only: relative tables resolved, pointer-only functions found; confirm `0x82AB1528` (35 cases), `0x8294E434`
  (17 cases) and functions covering `0x82A12650`–`0x82A127F0` are present (grep the generated `switch_tables.toml` /
  `functions.toml` for those addresses — numbers only).
- [ ] **Step 2:** Record counts in `docs/reverse-engineering/addresses.md`; no manual `recomp.toml` entries added.
- [ ] **Step 3: Commit** `docs: record M3 analyse coverage`.

## Part B — Environment and dependencies

### Task 5: Vulkan in the container and in the build

**Files:** Modify `tools/dev/Dockerfile`, `docs/development.md`, `.gitmodules`, `CMakeLists.txt`; add submodules
`thirdparty/volk`, `thirdparty/VulkanMemoryAllocator`, `thirdparty/Vulkan-Headers` (pinned tags, Vulkan-Headers
≥ 1.3.250); create `src/runtime/gpu/vk/device.{h,cpp}`, `tests/test_gpu_vk_device.cpp`; modify
`docs/adr/0002-license.md` (MIT/Apache-2.0 rows), `packaging/appimage/licenses.sh`.

**Interfaces:**
- Produces: CMake target `lr_vulkan` (volk + VMA implementation TU, Vulkan-Headers include, `VK_NO_PROTOTYPES`);
  `lr::gpu::vk::Device` with `static Result<Device> create(const DeviceOptions&)` where
  `struct DeviceOptions { bool validation; bool headless; void* sdl_window; }`; checks the Global Constraints
  features and returns `Error{ErrorCode::ToolMissing, "<what is missing>", hint}` otherwise; exposes `VkDevice`,
  queues, VMA allocator.

- [ ] **Step 1: Write the failing test** `[unit][vulkan] headless device has the required features` (lavapipe) and
  `missing ICD gives a clear error` (point `VK_ICD_FILENAMES` at an empty file → error names "no Vulkan driver").
- [ ] **Step 2:** FAIL. **Step 3:** Implement; Dockerfile adds `mesa vulkan-icd-loader vulkan-validation-layers
  vulkan-tools`. **Step 4:** PASS in the container.
- [ ] **Step 5: Commit** `build(gpu): add volk, VMA and a Vulkan device wrapper`.

### Task 6: XenosRecomp submodule, patch mechanism, DXC

**Files:** Add submodule `thirdparty/XenosRecomp` (pinned to upstream main HEAD); rename/generalise
`cmake/LrXenonRecompPatches.cmake` → `cmake/LrSubmodulePatches.cmake` (function
`lr_apply_submodule_patches(<name> <submodule dir> <patches dir> <out var copy dir>)`, same verification as Task 7
of M2); `packaging/appimage/toolchain.lock` + `build.sh` + `tools/dev/Dockerfile` get the pinned official DXC Linux
release (`dxc` + `libdxcompiler.so`, sha256); `src/pipeline/doctor.cpp` checks `dxc` (system mode: `dxc` on PATH or
the `directx-shader-compiler` package; bundled mode: `<resources>/toolchain/dxc/`); `src/pipeline/resources.cmake`
stages the `XenosRecomp` host tool to `share/legacyrecomp/tools/`; docs (`packaging.md`, `development.md`,
ADR 0002 rows NCSA/Apache-2.0/zstd-BSD/smol-v-MIT).

- [ ] **Step 1: Write the failing tests:** `doctor reports missing dxc with the install hint`, existing patch-mechanism
  tests parameterised over both submodules (`LrSubmodulePatches` verifies "Applied patch … cleanly" per file).
- [ ] **Step 2:** FAIL. **Step 3:** Implement; measure the DXC release's glibc floor with `readelf` and record it in
  `docs/packaging.md` (if > 2.31: stop and report BLOCKED with the number — the controller decides build-from-source).
- [ ] **Step 4:** PASS; `XenosRecomp` builds in the container.
- [ ] **Step 5: Commit** `build(deps): add XenosRecomp and the pinned DXC release`.

## Part C — GPU core

### Task 7: Xenos register file

**Files:** Create `src/runtime/gpu/registers.{h,cpp}`, `tests/test_gpu_registers.cpp`.

**Interfaces:**
- Produces: `namespace lr::gpu { inline constexpr uint32_t kRegisterCount = 0x6000;
  enum Reg : uint32_t { /* named indices used by later tasks: CP_RB_BASE, CP_RB_CNTL, CP_RB_RPTR_ADDR, CP_RB_WPTR,
  SCRATCH_REG0..7, RB_SURFACE_INFO, RB_COLOR_INFO, RB_COLOR1..3_INFO, RB_DEPTH_INFO, RB_COPY_CONTROL,
  RB_COPY_DEST_BASE, RB_COPY_DEST_PITCH, RB_COPY_DEST_INFO, PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL/BR,
  PA_CL_VPORT_*, PA_SU_SC_MODE_CNTL, RB_BLENDCONTROL0..3, RB_DEPTHCONTROL, RB_COLORCONTROL, SQ_PROGRAM_CNTL,
  VGT_DRAW_INITIATOR, SHADER_CONSTANT_000_X…, SHADER_CONSTANT_FETCH_00_0…, SHADER_CONSTANT_BOOL_000_031…,
  SHADER_CONSTANT_LOOP_00 … */ };
  class RegisterFile { public: uint32_t read(uint32_t index) const; void write(uint32_t index, uint32_t value);
  float read_float(uint32_t index) const; static const char* name(uint32_t index); /* nullptr if unnamed */ }; }`
  Index values from public Xenos documentation (cross-check Xenia's `register_table.inc` as reference); a comment per
  group names the source. Thread-safety: owned by the GPU thread only.

- [ ] **Step 1: Write the failing tests:** `named registers have the documented indices` (spot-check 10 indices),
  `float constants round-trip`, `out-of-range index is ignored on write and reads 0`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(gpu): add the Xenos register file`.

### Task 8: PM4 command processor

**Files:** Create `src/runtime/gpu/pm4.{h,cpp}`, `src/runtime/gpu/backend.h`, `src/runtime/gpu/null_backend.{h,cpp}`,
`tests/support/pm4_builder.h`, `tests/test_gpu_pm4.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  namespace lr::gpu {
  struct MemoryAccess { uint8_t* base; std::function<std::optional<uint32_t>(uint32_t phys)> to_guest; };
  enum class ShaderStage { Vertex, Pixel };
  struct DrawInfo { uint32_t prim_type, index_count, index_base_phys, index_size; bool indexed; uint32_t vgt_draw_initiator; };
  class Backend { public: virtual ~Backend();
    virtual void load_shader(ShaderStage, uint32_t guest_addr, uint32_t dwords) = 0;   // IM_LOAD(_IMMEDIATE)
    virtual void draw(const RegisterFile&, const DrawInfo&) = 0;
    virtual void resolve(const RegisterFile&) = 0;          // RB_COPY_CONTROL copy events
    virtual void swap(uint32_t frontbuffer_phys, uint32_t width, uint32_t height) = 0;
    virtual void wait_idle() = 0; };
  class CommandProcessor { public:
    CommandProcessor(MemoryAccess mem, RegisterFile& regs, Backend& backend, std::function<void(uint32_t source)> raise_interrupt);
    Result<uint32_t> execute(uint32_t guest_addr, uint32_t dwords);   // returns dwords consumed
    std::map<uint32_t, uint64_t> opcode_counts() const; };
  }
  ```
  Packet types 0 (register writes, one-reg flag), 2 (nop), 3 with the opcodes of spec §5.3; `INDIRECT_BUFFER`
  (depth limit 4); `WAIT_REG_MEM`/`COND_WRITE` against registers or guest memory (BE); `MEM_WRITE`, `EVENT_WRITE*`
  (fence values written BE to guest memory, `EVENT_WRITE_SHD` swap-counter semantics per Xenia), interrupts via
  `raise_interrupt`; `SET_CONSTANT(2)`/`LOAD_ALU_CONSTANT`/`SET_SHADER_CONSTANTS` into the register file;
  unknown type-3 opcode → `Error{InvalidFormat, "unknown PM4 opcode 0x.."}`; `NullBackend` counts calls.
  `pm4_builder.h` builds packets for tests.

- [ ] **Step 1: Write the failing tests:** `type-0 packet writes consecutive registers`, `one-reg type-0 packet`,
  `indirect buffer is executed`, `nested indirect depth limit`, `MEM_WRITE fence lands big-endian`,
  `EVENT_WRITE_SHD writes the swap counter`, `IM_LOAD reports the shader to the backend`, `DRAW_INDX reports a draw`,
  `WAIT_REG_MEM on memory waits until satisfied` (value written by another thread), `unknown opcode is an error`,
  **`malformed packets are fatal, not out-of-bounds`** (count past the end, IB outside guest memory, zero-length).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS (ASan clean).
- [ ] **Step 5: Commit** `feat(gpu): add the PM4 command processor`.

### Task 9: Physical → guest translation and the MMIO window

**Files:** Modify `src/runtime/memory/physical_heap.{h,cpp}`, `src/runtime/kernel/kernel.{h,cpp}`; create
`tests/test_gpu_memory.cpp`; modify `docs/architecture.md`.

**Interfaces:**
- Produces: `std::optional<uint32_t> PhysicalHeap::guest_from_physical(uint32_t phys) const` (the view address of the
  allocation that owns that physical page; nullopt if unallocated); `Kernel::gpu_memory_access()` returning a
  `gpu::MemoryAccess`; the GPU MMIO window at guest `0x7FC80000`–`0x7FCFFFFF` committed RW with initial register
  values per spec §5.2 (idle status), documented in `architecture.md`.

- [ ] **Step 1: Write the failing tests:** `physical address of a C0 allocation maps back to its view address`,
  `E0 view round trip honours +0x1000`, **`untranslatable physical address is reported`** (nullopt; the command
  processor turns it into a fatal naming the physical address — test via `CommandProcessor` + `MEM_WRITE` to an
  unallocated physical address).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(gpu): translate physical addresses and add the MMIO window`.

### Task 10: GPU system — Vd* imports, ring buffer thread, write-back, interrupts

**Files:** Create `src/runtime/gpu/gpu_system.{h,cpp}`, `tests/test_gpu_system.cpp`; modify
`src/runtime/kernel/exports/video.cpp`, `src/runtime/kernel/variables.cpp`, `src/runtime/kernel/kernel.{h,cpp}`,
`src/runtime/main.cpp`, `tests/support/fake_generated/*`, `tests/test_fake_game.cpp`, `docs/architecture.md`,
`docs/reverse-engineering/kernel-imports.md`.

**Interfaces:**
- Consumes: Tasks 7–9; `Timers`/kernel worker for interrupt delivery (`call_guest`); `variables.h`
  `register_variable`.
- Produces: `class GpuSystem { void initialize_ring_buffer(uint32_t phys, uint32_t size_log2);
  void enable_rptr_writeback(uint32_t phys, uint32_t block_log2); void set_interrupt_callback(uint32_t fn,
  uint32_t user_data); void set_backend(std::unique_ptr<Backend>); void start(); void stop();
  void set_vblank_mode(VblankMode mode); /* Realtime60Hz | PacedBySwap */ uint64_t swaps() const; }`,
  `Kernel::gpu()`. The GPU thread polls `CP_RB_WPTR` (the guest's MMIO write lands in the window), executes, writes
  `RPTR` BE to the write-back address. Interrupts: callback(source, user_data) per Xenia's convention (source 0 =
  vblank, 1 = command-processor interrupt — confirm), run on the kernel worker via `call_guest`. Imports per spec §5.1
  (`VdInitializeEngines` → 1, `VdGetSystemCommandBuffer` fills Xenia's values, `VdPersistDisplay` → 1,
  `VdSwap(...)` enqueues the swap packet semantics per Xenia — confirm arg layout), variables `VdGlobalDevice`,
  `VdGlobalXamDevice`, `VdGpuClockInMHz` (500), `VdHSIOCalibrationLock`, `KeCertMonitorData`; `MmMapIoSpace` returns
  the MMIO window address for the GPU register range (per Xenia). Headless default
  `PacedBySwap` (one vblank per processed swap, at least every 16 ms of wall time — spec §11), window `Realtime60Hz`.
  New game flag `--trace-gpu` (forwarded like `--trace-imports`, file `<root>/work/logs/gpu-trace.log`, capped at
  64 MiB): one line per PM4 packet (type, opcode, dwords), per draw (prim, count) and per swap, plus a per-second
  counter line (draws, resolves, textures created, swaps).

- [ ] **Step 1: Write the failing tests:** `ring buffer commands execute and RPTR is written back`,
  `vblank interrupt reaches the guest callback` (KernelFixture host-lambda glue), `paced vblank follows swaps`,
  fake game `fake guest initializes the GPU, submits a ring buffer and swaps` (exit 0 after N swaps, `swaps() == N`),
  `--trace-gpu writes packet and swap lines`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS, repeat x10.
- [ ] **Step 5: Commit** `feat(gpu): emulate the ring buffer, write-back and interrupts behind the Vd* imports`.

### Task 11: Silent audio, message box, relaunch

**Files:** Create `src/runtime/kernel/exports/{audio,xam_ui}.cpp`, `tests/test_kernel_audio_ui.cpp`; modify
`src/runtime/kernel/exports/xam.cpp` (move `XamShowDirtyDiscErrorUI` if convenient), docs.

**Interfaces:**
- Produces: `XAudioRegisterRenderDriverClient(callback_ptr, driver_out)` → writes `0x41550000`, returns 0;
  `XAudioUnregisterRenderDriverClient` → 0; `XAudioGetVoiceCategoryVolume(category, volume_out)` → writes 1.0f;
  `XAudioSubmitRenderDriverFrame` → 0 (only if the trace shows it); `XamShowMessageBoxUI(user, title, text, button_count,
  buttons, focus, flags, result_out, overlapped)` → prints `[game] message box: <title> — <text> [<buttons>]` (wide
  strings via the Task 24 M2 helpers), writes result 0 (first button), completes the overlapped; `XamLoaderLaunchTitle`
  → `guest_fatal("the game asked to relaunch itself (XamLoaderLaunchTitle)")`.

- [ ] **Step 1: Write the failing tests** for each (argument orders per Xenia `xam_ui.cc`, `apu` exports — confirm).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(kernel): silent audio driver and message box reporting`.

### Task 12: [ISO] M3a — GPU commands flow, no error box

- [ ] **Step 1:** Rebuild (`install --force build`) and run `container.sh run -- --headless --trace-imports
  --trace-gpu` (`timeout 600`), NullBackend.
- [ ] **Step 2 (repeat, like M2 Task 28):** implement whatever stops it (missing import, unknown PM4 opcode, fault,
  deadlock, message box text) with a failing unit test first; commit each fix.
- [ ] **Step 3:** Stop when `VdSwap` is called ≥ 300 times and no message box appears. Add
  `tests/test_integration_m3.cpp` **short** test: with the ISO, headless, `swaps() >= 60` within 300 s, exit by
  `--exit-after-swaps 60` (new game flag) with code 0.
- [ ] **Step 4: Commit** `test(integration): M3a — the game drives the GPU` (+ the fix commits).

## Part D — Shaders

### Task 13: XenosRecomp fork for the emulated GPU

**Files:** `thirdparty/patches/xenosrecomp/NNNN-*.patch` (from a local branch `legacyrecomp` in the submodule);
`tests/support/xenos_shader_builder.h` (synthetic containers: header per spec research — magic `0x102A11xx`,
virtual/physical size, constant table, definition table, microcode); `tests/shaders/` harness (like `tests/recomp`:
runs the patched XenosRecomp + DXC at test-build time on synthetic containers, checks the output).

**Interfaces:**
- Produces (in the fork, behind `#ifdef LR_LEGACYRECOMP`): (a) per-entry **microcode hash** = XXH3-64 of the
  microcode bytes exactly as they appear in the container (BE), emitted next to the container hash; (b) constants:
  VS/PS float buffers by BDA (unchanged), **128 booleans** and **32 loop/int constants** in the shared block;
  (c) vertex inputs: locations assigned per vertex-fetch slot from the container's vertex elements, no fixed usage
  table; (d) alpha test with all 8 Xenos compare functions via specialization constants; (e) `g_HalfPixelOffset` kept;
  (f) Unleashed-only paths compiled out; (g) the tool exits non-zero if any container fails to translate or DXC
  fails, and prints `translated N of M`.
- Shared constants layout documented in `docs/architecture.md` (Portuguese) as the runtime contract.

- [ ] **Step 1: Write the failing harness tests:** `vertex shader container translates and reports its microcode
  hash`, `pixel shader with alpha test greater-equal`, `shader using bool constant 100`, `shader with a loop constant`,
  `broken container makes the tool fail`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement on the submodule branch; regenerate patches. **Step 4:** PASS.
- [ ] **Step 5: Commit** (submodule branch commits + superproject `feat(shaders): adapt XenosRecomp to the emulated GPU`).

### Task 14: The `shaders` install stage

**Files:** Create `src/pipeline/stages/shaders_stage.{h,cpp}`, `tests/test_shaders_stage.cpp`; modify
`src/pipeline/pipeline.cpp` (`default_stages`), `src/pipeline/types.{h,cpp}` (`StageId::Shaders` between Recompile
and Build), `src/launcher/gui/ui_model.cpp` (Portuguese stage title), `src/runtime/template/CMakeLists.txt`
(compile `shader_cache.cpp`), `docs/pipeline.md`.

**Interfaces:**
- Produces: stage `shaders`: copies `work/image/default_tu1.xex` into `work/shaders/in/`, runs `tools/XenosRecomp`
  (posix_spawn, argv, cwd `work/shaders`) with DXC from the toolchain, writes `work/generated/shader_cache.cpp` and
  `work/logs/shaders.log`; fails with `ToolFailed` unless `translated N of M` has N == M; fingerprint = sha(image) |
  XenosRecomp commit | patches hash | DXC version. Generated file exports
  `extern "C" const lr_shader_cache_entry lr_shader_cache[]; extern "C" const size_t lr_shader_cache_count;` with
  `struct lr_shader_cache_entry { uint64_t microcode_hash; uint8_t stage; uint32_t spirv_offset, spirv_size; uint32_t spec_mask; }`
  and the compressed SPIR-V blob (`lr_shader_blob`, zstd).

- [ ] **Step 1: Write the failing tests:** `shaders stage runs the tool and records the count`,
  `fewer translated than found fails the stage`, `fingerprint changes with the XenosRecomp commit`, `stage order
  includes shaders between recompile and build`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(pipeline): translate the game's shaders at install time`.

### Task 15: Runtime shader cache and the plan A/B gate [ISO]

**Files:** Create `src/runtime/gpu/shader_cache.{h,cpp}`, `tests/test_gpu_shader_cache.cpp`; modify
`src/runtime/main.cpp` (registers the generated cache), `docs/adr/0005-m3-plan-deviations.md` (create).

**Interfaces:**
- Produces: `class ShaderCache { void register_entries(std::span<const lr_shader_cache_entry>, std::span<const uint8_t> blob);
  const ShaderEntry* find(uint64_t microcode_hash) const; std::vector<uint32_t> spirv(const ShaderEntry&) const; }`;
  `uint64_t microcode_hash(const uint8_t* guest_microcode, uint32_t dwords)` (XXH3-64 over the BE bytes) — the same
  hash the fork emits.
- [ ] **Step 1: Write the failing tests:** `hash of loaded microcode matches the fork's hash` (synthetic container
  through the Task 13 harness → load the microcode bytes → equal), `unknown hash is reported`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5 [ISO] Gate:** run `install --force shaders` (record `translated N of M`) and the M3a run with
  `--trace-gpu`, collecting the hashes of every `IM_LOAD` until 300 swaps; compute coverage (loaded hashes found in
  the cache). Write ADR 0005 entry "Portão dos shaders" with the numbers. **If coverage < 100% or translation
  < 90% of containers: STOP and report to the controller (plan B: own translator) — do not continue to Part E.**
- [ ] **Step 6: Commit** `feat(gpu): bind loaded microcode to translated shaders` + `docs: record the shader gate`.

## Part E — Vulkan renderer

### Task 16: Our own GPU-side shaders build

**Files:** `src/runtime/gpu/vk/shaders/*.hlsl` (blit, clear, untile, resolve placeholders), CMake rule compiling
them with DXC (`-spirv -T cs_6_0/vs_6_0/ps_6_0`) into `build/.../gpu_shaders.h` byte arrays at **launcher build
time** (they ship inside `liblr_runtime_core.a`); `tests/test_gpu_vk_blit.cpp`.

- [ ] **Step 1: Write the failing test** `[vulkan] blit shader copies a texture to an offscreen image` (lavapipe).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `build(gpu): compile the renderer's own shaders at build time`.

### Task 17: Presenter, swapchain, headless, screenshots

**Files:** Create `src/runtime/gpu/vk/presenter.{h,cpp}`, `src/runtime/gpu/screenshot.{h,cpp}`,
`thirdparty/stb/stb_image_write.h` (pinned single header, public domain/MIT, noted in ADR 0002);
modify `src/runtime/main.cpp` (window loop: SDL window with `SDL_WINDOW_VULKAN`, GPU backend selection,
`--screenshot-after <N> <png>`, `--exit-after-swaps <N>`), `tests/test_fake_game.cpp`, `src/pipeline/doctor.cpp`
(Vulkan probe: runs `legacyrecomp-game --probe-vulkan` or an equivalent small probe binary; reports missing pieces).

**Interfaces:**
- Produces: `class Presenter { static Result<Presenter> create(Device&, void* sdl_window /*nullptr = headless*/);
  void present(VkImage frame, uint32_t w, uint32_t h); Result<void> capture_png(const std::filesystem::path&); }`;
  `VkBackend : gpu::Backend` skeleton whose `swap()` presents the front buffer (from guest memory for now: untiled
  copy of the frontbuffer surface).
- [ ] **Step 1: Write the failing tests:** fake game `clear-color frame is captured` (fake guest writes a solid
  frontbuffer + VdSwap; `--screenshot-after 1` PNG has that color), **`no Vulkan device gives a clear error`**
  (exit 2, message names the missing piece), **`closing during rendering exits cleanly`** (SIGTERM mid-run → exit 0,
  no validation errors), `doctor reports Vulkan status`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS with validation layers on.
- [ ] **Step 5: Commit** `feat(gpu): present frames with Vulkan and capture screenshots`.

### Task 18: EDRAM render targets, clears and resolves

**Files:** Create `src/runtime/gpu/vk/edram.{h,cpp}`, `tests/test_gpu_edram.cpp`.

**Interfaces:**
- Produces: `class Edram { RenderTarget& bind(const RegisterFile&); void clear(const RegisterFile&);
  void resolve(const RegisterFile&, const MemoryAccess&); }` — host images keyed by (EDRAM base, pitch, format,
  MSAA); resolve writes the guest destination in Xenos tiled layout with endian swap via the Task 16 compute
  shaders; formats: k_8_8_8_8, k_2_10_10_10, k_16_16_16_16_FLOAT, k_32_FLOAT, depth D24S8/D24FS8; 1x/2x/4x MSAA
  resolve by averaging.
- [ ] **Step 1: Write the failing tests:** `[vulkan] clear then resolve writes the tiled guest surface` (compare with
  the CPU reference from Task 19), `2x MSAA resolve averages samples`, `depth resolve format`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(gpu): emulate EDRAM with host render targets`.

### Task 19: Texture formats and cache

**Files:** Create `src/runtime/gpu/texture_format.{h,cpp}` (CPU reference: tiling address math, endian swaps
8in16/8in32/16in32, format table), `src/runtime/gpu/vk/textures.{h,cpp}`, `tests/test_gpu_textures.cpp`.

**Interfaces:**
- Produces: `uint32_t tiled_offset_2d(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t bytes_per_block)`;
  `enum class XenosFormat`; `std::optional<VkFormat> host_format(XenosFormat)`; `class TextureCache {
  VkImageView get(const TextureFetch& fetch_constant); void invalidate_range(uint32_t phys, uint32_t size); }`;
  DXT1/2_3/4_5 → BC1/2/3, DXN → BC5, CTX1 decoded to RG8, 8888/565/1555/4444/8/88 etc.; invalidation by write
  watch (per-page hash compared each frame — the implementer may choose page protection instead and document it).
- [ ] **Step 1: Write the failing tests:** `tiled offset matches known values` (published Xenos tiling examples),
  `endian swap modes`, `[vulkan] DXT1 tiled texture uploads correctly`, `CTX1 decode`, **`edge-size and
  unsupported-format textures`** (1×1, 1000×3, 8192×8192 RGBA8 within memory, an unsupported format → fatal naming it),
  `modified guest memory invalidates the texture`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(gpu): add the texture cache with Xenos untiling`.

### Task 20: Draw path — vertex fetch, state, pipelines, translated shaders

**Files:** Create `src/runtime/gpu/vk/{pipelines,draw}.{h,cpp}`, `tests/test_gpu_draw.cpp`; modify the VkBackend.

**Interfaces:**
- Produces: `draw()` builds (or finds by hash) a `VkPipeline` from the translated VS/PS SPIR-V (`ShaderCache`), the
  vertex input layout from vertex fetch constants (formats → `VkFormat`, endian swap of vertex buffers on upload),
  blend/depth/stencil/raster/viewport/scissor from registers, primitive types (incl. rect list and quad list via
  index expansion); binds per the Task 13 contract: push constants with BDAs of VS/PS/shared constant buffers
  (floats from the register file, booleans 128, loops 32), bindless texture/sampler arrays filled from texture fetch
  constants; specialization constants for alpha test.
- [ ] **Step 1: Write the failing tests:** `[vulkan] triangle with a translated synthetic shader renders`
  (synthetic container through Tasks 13–14 → shader cache → fake PM4 stream → resolved pixels), `blend state from
  registers`, `quad list expansion`, `vertex format k_16_16_FLOAT with endian swap`, `alpha test greater-equal discards`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS, validation clean.
- [ ] **Step 5: Commit** `feat(gpu): draw with translated shaders`.

### Task 21: Fake game draws a textured quad end to end

**Files:** `tests/support/fake_generated/*`, `tests/test_fake_game.cpp`.

- [ ] **Step 1: Write the failing test** `fake guest draws a textured quad`: hand-written guest code initializes the
  GPU, uploads a synthetic shader container (built by the test harness and translated by the shaders stage at test
  build time), a tiled DXT1 texture and a quad, swaps, and `--screenshot-after 1` shows the texture's colors in the
  expected region.
- [ ] **Step 2:** FAIL. **Step 3:** Fix what breaks. **Step 4:** PASS x10.
- [ ] **Step 5: Commit** `test(gpu): render a textured quad from a fake guest`.

## Part F — The real game

### Task 22: [ISO] Burn-down to the title screen

- [ ] **Step 1:** `install --force shaders` then `run -- --headless --trace-gpu --screenshot-after 300 /data/shot.png`
  (`timeout 1800`); look only at the exit report, GPU counters and objective image statistics computed by a tools
  script (`tools/frame_stats.py`: distinct colors, mean luminance, edge fraction) — never at the image itself in
  context, never committed.
- [ ] **Step 2 (repeat):** fix whatever blocks or renders wrong (unknown opcode/format, missing state, fault,
  validation error) with a failing unit test first; commit each fix.
- [ ] **Step 3:** Stop when the frame at N=600 passes: ≥ 64 distinct colors, mean luminance between 0.05 and 0.95,
  edge-pixel fraction ≥ 2%, zero validation errors, and the run reaches 600 swaps without exit. Add the **full**
  integration test with those thresholds (`[integration]`, 3600 s timeout).
- [ ] **Step 4: Commit** `test(integration): the game reaches its title screen`.

### Task 23: WSLg visual checkpoint (owner's choice)

**Files:** Create `docs/wslg.md` (Portuguese); `tools/dev/wslg.sh` (optional helper).

- [ ] **Step 1:** Install a WSL distro (Ubuntu 24.04) with `wsl --install -d Ubuntu-24.04` (owner-approved in the
  spec §10), install Mesa with the `dzn` (D3D12) driver, check `vulkaninfo --summary`.
- [ ] **Step 2:** Build the AppImage (Task 24 may come first if needed), copy it into the distro, run it with the ISO
  and a window; record FPS from `--trace-gpu` counters and a local screenshot (never committed) — what is visible.
- [ ] **Step 3:** If `dzn` lacks a required feature, run with lavapipe (`VK_ICD_FILENAMES`) for the visual check and
  record that 60 Hz needs a real Linux GPU.
- [ ] **Step 4: Commit** `docs: run LegacyRecomp with a window under WSLg`.

### Task 24: AppImage, licenses, docs, close M3

**Files:** `packaging/appimage/build.sh` (DXC in `toolchain/dxc/`, XenosRecomp tool, version `0.3.0-m3`),
`packaging/appimage/licenses.sh`, `docs/{architecture,pipeline,packaging,roadmap,kernel-coverage}.md`, all seven
READMEs, `docs/adr/0005-m3-plan-deviations.md`, spec status.

- [ ] **Step 1:** AppImage built in the container; in a clean `archlinux` container without compilers or DXC on PATH:
  `install --iso` + `run -- --headless --screenshot-after 600 …` passes the Task 22 thresholds (lavapipe).
- [ ] **Step 2:** Docs: GPU architecture (ring buffer, EDRAM, texture cache, shader contract), `shaders` stage,
  packaging (DXC, Vulkan loader from the system), roadmap (M3 done, M4 next; remaining gaps), READMEs (status in each
  language — honest: title screen drawn, no input/sound yet), ADR 0005 complete (every ruling), coverage regenerated.
- [ ] **Step 3:** Full unit suite + both integration tests green.
- [ ] **Step 4: Commit** `docs: close M3`.
