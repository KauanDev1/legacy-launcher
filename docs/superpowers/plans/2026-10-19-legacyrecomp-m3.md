# LegacyRecomp M3 — Graphics (Xenos GPU on Vulkan) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The recompiled game opens a window and draws its intro and title screen with real textures and translated
shaders through an emulated Xenos GPU rendered with Vulkan.

**Architecture:** The game's own recompiled D3D code writes PM4 command buffers; a host GPU thread consumes the ring
buffer, keeps the Xenos register file, and drives a Vulkan backend (EDRAM as host render targets, texture cache,
vertex fetch). Shaders are translated at runtime (plan B): the microcode the game loads with `IM_LOAD` is decoded and
translated to SPIR-V by our own translator, keyed by the microcode hash plus the register state that changes the
translation, and cached on disk in `work/shaders/runtime/`. Before any graphics, the analyse stage learns the jump
tables and pointer-only functions that crashed the M3 spike.

> **Plan B (2026-10-22):** the Task 15 gate failed. Shaders are no longer translated at install time: the runtime
> translates the microcode the game actually loads into SPIR-V with our own translator (spec §3.2 plan B). See
> "Plan B amendment (gate failed 2026-10-22)" after Task 15. XenosRecomp, the `shaders` stage and DXC leave the project
> (Task 15.1); SPIRV-Headers joins it, and `glslangValidator` is used only at launcher build time for the renderer's
> own shaders.

**Tech Stack:** C++20, Clang, CMake/Ninja, SDL2, Vulkan 1.2 via volk + VMA (+ Vulkan-Headers), SPIRV-Headers (our
own SPIR-V builder), glslangValidator (build time only, renderer's own shaders), SPIRV-Tools (tests only), Catch2,
Mesa lavapipe in the dev container. (Plan A's XenosRecomp fork and DXC were removed in Task 15.1.)

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
- Exit codes unchanged (0, 2, 3, 4, 5, 6). Unknown PM4 opcode, or untranslatable microcode with a backend that binds
  shaders → `guest_fatal` (exit 5) naming it.
- Tasks marked **[ISO]** use the owner-confirmed ISO only through `tools/dev/container.sh`; read only exit reports,
  counts, short log tails; never game data in context or repo.
- Tests that need a Vulkan device are tagged `[unit][vulkan]` and `SKIP` when no ICD is present (the container has
  lavapipe, so they run there).
- (plan B) The shader translator is clean-room MIT code in `src/runtime/gpu/shader/` (namespace `lr::gpu::shader`).
  It is CPU-only, uses no Vulkan API types (SPIRV-Headers enums are allowed), and goes into both runtime variants
  (ruling R-stage). Xenia's `ucode.h`, `shader_translator.cc` and `spirv_shader_translator*.cc` are references for
  **semantics only**; UnleashedRecomp is never read for this.
- (plan B) The translator takes only the microcode the game loads (`IM_LOAD`/`IM_LOAD_IMMEDIATE`) and the register
  state named in `ShaderKey`. It never uses container metadata. VS→PS interpolators link by register index.
- (plan B) No shader compiler at install or run time. The game's SPIR-V comes from our `SpirvBuilder`, and the
  renderer's own GLSL shaders are compiled by `glslangValidator` at launcher build time. `spirv-val`/`spirv-dis`
  (SPIRV-Tools) are test-only container tools and are never shipped.
- (plan B) Translated SPIR-V is game-derived. It lives only in `<root>/work/shaders/runtime/`, keyed by
  `kTranslatorVersion` plus the full `ShaderKey`, and is never committed. Any change to the translator's output
  bumps `kTranslatorVersion` (the golden-fingerprint test of Task 15.10 enforces it).
- (plan B) Every translator feature lands with a synthetic-microcode test (`tests/support/xenos_shader_builder.h`).
  Every translated test module passes `spirv-val --target-env vulkan1.2`, and wherever the result is a value, the
  test executes the module on lavapipe through `tests/support/vk_shader_harness.h` and checks the pixels.
- (plan B) Microcode the translator cannot handle is a `Result` error naming the feature and the instruction index.
  It is fatal (exit 5) only with a backend that binds shaders, and the NullBackend only reports it (the exit-code
  line above).

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
6. (plan B) **Guest-controlled microcode.** Possible cases:
   - CF addresses past the end;
   - jump cycles;
   - unknown opcodes;
   - register indices out of range.

   Each must give a decode error, never a host out-of-bounds read. Translated code must never hang the host GPU: the
   pc-dispatch loop has an iteration cap. Tests: Task 15.2 `exec past the end of the microcode is an error`, Task 15.5
   `runaway jump loop stops at the iteration cap`.
7. (plan B) **Guest-controlled vertex fetch in the shader.** An index × stride + offset past the slot's uploaded size
   reads zeros, never device memory outside the buffer. Test: Task 15.8 `fetch past the slot size reads zero`.

---

## File Structure

```
tools/dev/Dockerfile                        # + mesa (lavapipe), vulkan-icd-loader, vulkan-validation-layers, vulkan-tools
thirdparty/{volk,VulkanMemoryAllocator,Vulkan-Headers,SPIRV-Headers}   # submodules (pinned)
cmake/LrSubmodulePatches.cmake              # generalised from LrXenonRecompPatches.cmake (XenonRecomp)
cmake/LrGpuShaders.cmake                    # glslangValidator at launcher build time (renderer's own shaders)
src/pipeline/switch_scan.{h,cpp}            # + relative halfword/byte jump tables
src/pipeline/stages/analyse_stage.cpp       # + pointer-only function discovery
src/pipeline/function_discovery.{h,cpp}     # new: data-table code pointers + uncovered bl targets
src/runtime/gpu/registers.{h,cpp}           # Xenos register file + names
src/runtime/gpu/pm4.{h,cpp}                 # packet decoding + CommandProcessor
src/runtime/gpu/backend.h                   # Backend interface (draw/resolve/swap/shader load)
src/runtime/gpu/null_backend.{h,cpp}        # headless no-render backend
src/runtime/gpu/gpu_system.{h,cpp}          # ring buffer thread, write-back, interrupts, MMIO window
src/runtime/gpu/shader/*                    # plan B translator (see the amendment's file list)
src/runtime/gpu/shader_cache.{h,cpp}        # loaded microcode -> decode -> translate -> memory/disk cache
src/runtime/gpu/texture_format.{h,cpp}      # Xenos formats, untiling, endian swap (CPU reference)
src/runtime/gpu/vk/{device,presenter,edram,textures,samplers,pipelines,draw}.{h,cpp}
src/runtime/gpu/vk/shaders/*.{vert,frag,comp}   # our own GLSL shaders, compiled to SPIR-V at launcher build time
src/runtime/gpu/screenshot.{h,cpp}          # --screenshot-after (PNG via stb_image_write)
src/runtime/kernel/exports/{video,audio,xam_ui}.cpp
src/runtime/memory/physical_heap.{h,cpp}    # + guest_from_physical()
tests/test_switch_scan.cpp tests/test_function_discovery.cpp tests/test_gpu_*.cpp
tests/support/{pm4_builder.h,xenos_shader_builder.h,spirv_tools.h,vk_shader_harness.{h,cpp}}
tests/test_integration_m3.cpp
tools/{shader_cache_stats.sh,frame_stats.py}
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

> Partly superseded by Plan B: Task 15.1 removes the XenosRecomp submodule and DXC. `LrSubmodulePatches.cmake` stays,
> now for XenonRecomp only.

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

> Superseded by Plan B. Task 15.1 removes the fork. Its microcode encoders survive in
> `tests/support/xenos_shader_builder.h` (Tasks 15.2–15.9).

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

> Superseded by Plan B. Task 15.1 retires the stage, and Task 15.10 replaces its cache with the runtime disk cache.

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

> Superseded by Plan B. The gate ran and failed (ADR 0005 M7). Task 15.10 rewrites `ShaderCache`, and Task 15.11
> replaces the gate.

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

## Plan B amendment (gate failed 2026-10-22)

**Why.** The ADR 0005 M7 numbers:
- 64 of 85 containers translated;
- 7 distinct shaders loaded by 300 swaps, 5 of them in no container: 2 D3D-patched `vfetch` variants and 3 small
  container-less vertex shaders.

No cache keyed by container hash can reach 100%. The controller ruled for the spec's plan B, a runtime translator
from Xenos microcode to SPIR-V with Xenia as the semantic reference. The record is ADR 0005 M8 (draft:
`.superpowers/sdd/2026-10-19-legacyrecomp-m3/adr-plan-b-draft.md`). The rest of M3 (emulated GPU, EDRAM, textures)
is unchanged.

**Design decisions (planner's calls, recorded in M8):**
- **SPIR-V production: our own `SpirvBuilder` over SPIRV-Headers (option a).** The alternatives:
  - glslang's `spv::Builder` (b) is BSD-3-Clause and usable standalone, but it brings the whole glslang submodule
    for 4 files, and that repo carries mixed licenses;
  - GLSL + glslang/shaderc at runtime (c) puts a multi-MB front end in the game binary, adds compile latency and has
    the same license issue.

  Option (a) is about 1 000 lines with no new library in the game binary, so there is nothing new for zig to build
  against glibc 2.31. Debuggability comes from the microcode disassembler, `--dump-shaders` and test-only
  `spirv-val`/`spirv-dis`. Fallback if it gets too costly: swap the backend of `SpirvBuilder` for `spv::Builder`;
  the translator is untouched.
- **Vertex fetch is done inside the shader.** It reads from per-draw device-address buffers, with the format, stride
  and offset taken from the `vfetch` instruction (so they are inside the microcode hash), and the base address,
  size and endianness from the fetch constant at runtime. There is no `VkPipelineVertexInputState`. D3D-patched
  variants become distinct keys with no extra state, and Xenos formats without a `VkFormat` work.
- **Interpolators link by register index.** The VS always declares 16 `vec4` outputs (locations 0..15, zero unless
  written: 64 components, the Vulkan guaranteed minimum, so the fork's 72-component problem is gone). The PS declares
  an input for every register `r<16` it references.
- **Runtime contract: kept where it worked, changed where the fork's assumptions were.** Task 15.4 rewrites
  `docs/architecture.md` "Contrato dos shaders traduzidos" with these changes:
  - kept:
    - push-constant device addresses (BDA) for the VS and PS float constants (256 `float4` each, PS = registers
      256+i) and for the shared block;
    - the shared-block offsets 0..671: texture-heap index tables, the 256 bools and the 32 loop constants;
    - the bindless sets 0..3;
    - alpha test as specialization constant 0;
  - changed:
    - push constants grow from 24 to 32 bytes: offset 24 is the BDA of the **vertex-fetch table**, 96 entries of
      `{uint64 address; uint32 size_bytes; uint32 endian}`;
    - every BDA (push constants, fetch-table entries) is read as a `uvec2` and turned into a
      `PhysicalStorageBuffer64` pointer with `OpBitcast` (planner's call, over requiring `shaderInt64`);
    - `g_HalfPixelOffset` and `-fvk-invert-y` are replaced by `float4 ndc_scale` (672) and `float4 ndc_offset`
      (688). The runtime computes them from `PA_CL_VPORT_*` and the pixel center (`PA_SU_VTX_CNTL`), with the Y flip
      folded in;
    - new shared-block fields:
      - 704 `float` alpha reference;
      - 708 `uint` VTE flags from `PA_CL_VTE_CNTL`: bit 0 `vtx_xy_fmt`, bit 1 `vtx_z_fmt`, bit 2 `vtx_w0_fmt`. The VS
        applies them to the exported position **before** `ndc_scale`/`ndc_offset`, with Xenia's semantics: undo or
        keep the divide by W, and W is given as 1/W;
      - 712 `int` index offset (`VGT_INDX_OFFSET`);
      - 716 and 720 `uint` min and max vertex index (`VGT_MIN_VTX_INDX`/`VGT_MAX_VTX_INDX`);
      - 724..735 reserved;
      - the block is **736 bytes**;
    - 1D textures are sampled as height-1 `Texture2D` (set 0);
    - no vertex inputs and no `lr_shader_vertex_inputs`.
- **Xenos facts the contract states** (in `contract.h` and `architecture.md`):
  - VS export registers: interpolators 0..15, position 62, point size/edge flag/kill 63.
  - PS export registers: colors 0..3, depth 61.
  - `eA` = 32 and `eM0..4` = 33..37 are memexport, in both stages. The gate's 15 "PS export 32" failures are
    therefore real memexport.
  - VS `r0.x` = the vertex index, `float(clamp(gl_VertexIndex + index_offset, min, max))`.
  - PS registers `r0..r15` start from the interpolators selected by the interpolator mask; every other register
    starts at 0.
  - The VS writes `gl_PointSize = 1.0`; point-sprite expansion is out of M3.
- **Key.** `ShaderKey{stage, microcode_hash (XXH3-64 of the loaded big-endian dwords, unchanged from Task 15), dwords,
  uint64 modifiers}`.
  - PS modifiers:
    - bits 0..7: the `param_gen` register, or `0xFF` for none (`SQ_PROGRAM_CNTL.param_gen` +
      `SQ_CONTEXT_MISC.param_gen_pos`);
    - bit 8: the draw is a point list (`param_gen`'s point flag);
    - bits 16..31: the flat-shade mask (`SQ_INTERPOLATOR_CNTL.param_shade`);
    - bits 32..47: the centroid mask (`SQ_INTERPOLATOR_CNTL.sampling_pattern`).
  - The VS has no modifiers in M3; VS outputs carry no interpolation decorations.
  - `TranslateOptions` holds exactly the decoded fields (`key_options(const ShaderKey&)`). Each new modifier is a
    field in both and must be justified by a test.
- **When translation happens.** At `IM_LOAD`, the microcode is copied and decoded (an `IM_LOAD_IMMEDIATE` pointer is
  only valid during the call). At draw time, `(stage, key)` is translated if new, on the GPU thread, independent of
  the backend. So a NullBackend run measures translation. The last microcode loaded per stage is the active one
  (Xenia's model). `pm4.cpp` ignores today the `IM_LOAD` start field; a nonzero start becomes a named fatal (Task
  15.10, Review Focus 6).
- **Kill and derivatives (planner's call).**
  - Kill sets a function-local `killed` flag; `main` ends with `OpKill` only if it is set. There is never an
    `OpKill` mid-switch, so later fetches keep their quad derivatives.
  - Predicated fetches are evaluated unconditionally and merged with `OpSelect`, so implicit-LOD sampling never sits
    in predicate-divergent control flow.
  - Branches on bool and loop constants are dynamically uniform.
  - Implicit-LOD fetches inside a pc-switch case reached through a predicate-dependent `cond_jmp` are a documented
    M3 limitation, noted in `architecture.md`.
- **Install-time translation and the XenosRecomp fork: retired** (Task 15.1). The fork stays in git history
  (`0523eda`..`3b15c74`), and the code is removed rather than left dead. `lr_zstd` goes too: it came from
  XenosRecomp's zstd submodule, and the disk cache stores raw SPIR-V. The synthetic microcode encoders of
  `xenos_shader_builder.h` are **reused** (its container assembler is removed).
- **The renderer's own shaders:** GLSL compiled by `glslangValidator` at launcher build time (Task 16, revised), so
  DXC leaves the project entirely. Spec §12 criterion 4, "com o DXC embutido", becomes moot (M8).
- **Shading semantics that matter:**
  - the Xenos multiply rule: a product with a 0 factor is +0 (even 0 × inf and 0 × NaN), except when both operands
    are the same value. It applies to:
    - vector `MUL`, `MULADD` (the product only, not the addend), `DOT4`, `DOT3`, `DOT2ADD` and `DST`'s y;
    - scalar `MULs`, `MUL_PREVs`, `MUL_PREV2s` and `MUL_CONST_0/1`;
  - temporaries addressed relative to `aL` (`r[aL+n]`, sources and destinations; only absolute or aL-relative, never
    a0). When a shader uses any, the register array is 64 `vec4` and the index is clamped to 0..63 (planner's call,
    over keying on the `SQ_PROGRAM_CNTL` register count);
  - a `const_0`/`const_1` relative-addressing rule, carried over from ADR 0005 M5;
  - `aL` save/restore per loop;
  - `cond_exec*` respects its condition;
  - a vertex-shader `tfetch` uses an explicit LOD (the fork's implicit-LOD bug);
  - memory export (`eA`/`eM`) is decoded but not translated. It fails loudly, and the gate decides.

**Files (plan B):**
```
thirdparty/SPIRV-Headers                                     # submodule (pinned tag), header-only
src/runtime/gpu/shader/ucode.h                               # Xenos microcode bitfields (own, from public docs)
src/runtime/gpu/shader/{ir,disasm}.{h,cpp}                   # decode to Program, text disassembly
src/runtime/gpu/shader/spirv_builder.{h,cpp}                 # minimal SPIR-V module builder
src/runtime/gpu/shader/contract.h                            # push-constant/shared-block/set/location numbers
src/runtime/gpu/shader/translator.{h,cpp} translator_{cf,alu,fetch,export}.cpp
src/runtime/gpu/shader/{shader_key,disk_cache}.{h,cpp}
src/runtime/gpu/shader_cache.{h,cpp}                         # rewritten: decode + translate + memory/disk cache
tests/support/{spirv_tools.h,vk_shader_harness.h,vk_shader_harness.cpp}
tests/test_gpu_shader_{decode,builder,translate,cf,alu_vector,alu_scalar,vfetch,tfetch,cache}.cpp
tests/CMakeLists.txt                                         # every 15.2–15.10 task registers its test file here
```

### Task 15.1: Retire install-time translation, XenosRecomp and DXC [ISO]

**Files:**
- Delete: `src/pipeline/stages/shaders_stage.{h,cpp}`, `tests/test_shaders_stage.cpp`, `tests/shaders/`,
  `tests/support/fake_generated/fake_shader_cache.cpp`, `thirdparty/patches/xenosrecomp/`, and the
  `thirdparty/XenosRecomp` submodule (and its `.gitmodules` entry).
- Modify:
  - `CMakeLists.txt`: drop the XenosRecomp/DXC block and `LR_BUILD_XENOSRECOMP`/`LR_DXC_*`;
  - `src/runtime/CMakeLists.txt`: drop `lr_zstd`;
  - `src/runtime/template/CMakeLists.txt`: drop `LR_SHADER_CACHE` and `liblr_zstd.a`;
  - `src/pipeline/{pipeline.cpp,types.{h,cpp},resources.cmake,doctor.cpp,toolchain.{h,cpp},CMakeLists.txt}`;
  - `src/pipeline/stages/build_stage.{h,cpp}`: drop the `shader_cache` argument and the hash;
  - `src/launcher/cli/args.cpp`, `src/launcher/gui/{app,ui_model}.cpp`;
  - `src/runtime/main.cpp`: no generated-cache registration;
  - `src/runtime/gpu/{shader_cache,gpu_system}.{h,cpp}`: keep only `microcode_hash` and the per-hash load stats;
  - `tests/support/xenos_shader_builder.h`: keep the microcode encoders, drop `build_container`/`ShaderDesc`;
  - `tests/{CMakeLists.txt,test_gpu_shader_cache.cpp,test_build_stage.cpp,test_doctor.cpp,test_toolchain.cpp,test_gui_model.cpp}`;
  - `tests/support/check_submodule_patches.cmake`: XenonRecomp only;
  - `tools/dev/{Dockerfile,init-submodules.sh}`: no `directx-shader-compiler`;
  - `packaging/appimage/{build.sh,licenses.sh}`;
  - `AGENTS.md`: toolchain line and module map;
  - `docs/{architecture,pipeline,development,packaging,roadmap}.md`;
  - `docs/adr/0002-license.md`: remove the XenosRecomp/smol-v/zstd/DXC rows;
  - `docs/adr/0005-m3-plan-deviations.md`: entry M8 from the draft.

**Interfaces:**
- Consumes: the Task 15 `microcode_hash`, `GpuSystem::shader_stats()` and `Backend::binds_shaders()`.
- Produces:
  - stage list `doctor, extract, verify, patch, analyse, recompile, build`;
  - `ShaderLoadStats::Shader{hash, stage, dwords}`, with no `cached` field;
  - the trace line `shader vertex|pixel 0x<hash> dwords=N`;
  - `docs/architecture.md`: the fork contract section is replaced by a two-line pointer to ADR 0005 M8 and Task 15.4.
- Removes: everything listed under Files.
- Kept: `lr_apply_submodule_patches`, now used by XenonRecomp only.

- [ ] **Step 1: Write the failing tests:**
  - `stage order has no shaders stage` (replaces the Task 14 order test);
  - `state with a stale shaders entry still loads`: a `work/state.toml` written by an older install with a
    `[stages.shaders]` table is accepted and ignored;
  - `doctor does not require dxc`;
  - `shader loads are traced by hash` (replaces `unknown hash is reported`);
  - the template guard test compiles the game without `LR_SHADER_CACHE`.
- [ ] **Step 2:** `tools/dev/container.sh test` → FAIL. **Step 3:** Implement, and copy the M8 draft into ADR 0005.
  **Step 4:** PASS. `git grep -i "xenosrecomp\|dxc\|lr_zstd\|shader_blob"` must match only ADR 0005, the spec and the
  plans.
- [ ] **Step 5 [ISO]:**
  - `container.sh install --iso "<iso>" --force build` → exit 0;
  - `ctest -L integration` (M3a short test) → green;
  - in the data volume, delete `/data/scratch/gate` and the old `work/shaders/{in,hlsl,shader_cache.cpp}`
    (game-derived leftovers of Task 15);
  - the next `container.sh` sync must restore `gpu_system.cpp` in the `lr_build` volume (Task 15 concern 6); confirm
    that `LR_GATE_DUMP` appears in no staged file.
- [ ] **Step 6: Commit** `refactor(shaders)!: retire install-time translation and XenosRecomp` (body: plan B, ADR
  0005 M8).

### Task 15.2: Microcode decoder, IR and disassembler

**Files:** Create `src/runtime/gpu/shader/{ucode.h,ir.h,ir.cpp,disasm.h,disasm.cpp}` and
`tests/test_gpu_shader_decode.cpp`. Modify `src/runtime/CMakeLists.txt` (both runtime variants),
`tests/CMakeLists.txt` and `tests/support/xenos_shader_builder.h`.

**Interfaces:**
- Consumes: `ShaderStage` (`backend.h`).
- Produces:
  ```cpp
  namespace lr::gpu::shader {
  enum class CfOp { Nop, Exec, ExecEnd, CondExec, CondExecEnd, CondPredExec, CondPredExecEnd, LoopStart, LoopEnd,
                    CondCall, Return, CondJmp, Alloc, CondExecPredClean, CondExecPredCleanEnd, MarkVsFetchDone };
  struct CfInstr { CfOp op; uint32_t address, count, sequence, bool_index, loop_id, target;
                   bool condition, use_predicate, is_unconditional, is_repeat,
                   is_predicated_break, pred_break_condition; };                 // LOOP_END predicated break
  struct AluInstr  { /* every ucode field, decoded; vector_op, scalar_op, dests, masks, srcs, swizzles, negates,
                        abs_constants, saturate, predicated, pred_condition, export flag, const0/1_rel, a0_rel,
                        is_src_temp_relative[3], vector_dest_rel, scalar_dest_rel (aL-relative temps) */ };
  struct FetchInstr{ /* vertex: slot, offset (signed, -4194304..4194303), stride, format, signed,
                        normalized(num_format_all), exp_adjust, mini, index register/component, rounded;
                        texture: const_index, dimension, lod fields, offsets, unnormalized, filter overrides,
                        swizzles; both: src_rel, dst_rel (aL-relative registers); predicated, pred_condition */ };
  struct Program { ShaderStage stage; std::vector<CfInstr> cf; std::vector<std::variant<AluInstr, FetchInstr>> slots;
                   uint32_t temp_count; std::bitset<256> bools_used; std::bitset<32> loops_used;
                   std::bitset<96> vfetch_slots; std::bitset<32> tfetch_slots; uint32_t interpolators_written;
                   uint32_t colors_written; bool writes_position, writes_point_size, writes_depth, uses_memexport,
                   uses_kill, uses_relative_temps, has_jumps_calls_or_loops; };
  Result<Program> decode(ShaderStage stage, std::span<const uint32_t> microcode);  // host-order dwords
  std::string disassemble(const Program& program);
  }
  ```
  The input is host-order dwords (the caller byte-swaps from guest big-endian). Errors are `InvalidFormat` with
  `instruction <n>: <what>`. Builder additions: `cf_jmp`, `cf_cond_jmp`, `cf_call`, `cf_ret`, `cf_alloc`,
  `cf_cond_exec_pred(_clean)`, predicated ALU/fetch flags, scalar opcode constants, mini `vfetch`, `tfetch` LOD and
  offset fields.

- [ ] **Step 1: Write the failing tests:**
  - `cf pair unpacking matches the builder`;
  - `exec clause decodes its alu and fetch slots`;
  - **`exec past the end of the microcode is an error`**;
  - `jump target outside the program is an error`;
  - `unknown vector opcode is an error naming the slot`;
  - `empty microcode is an error`;
  - `program summary lists used constants and fetch slots`;
  - `memexport destination is recorded` (`eA` 32, `eM` 33..37);
  - `aL-relative source and destination temps are decoded`;
  - `negative vfetch offset is sign-extended`;
  - `loop_end predicated break is decoded`;
  - `disassembly names every opcode of a mixed shader`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement from the public field tables (Xenia `ucode.h` for semantics only).
  **Step 4:** PASS, ASan clean.
- [ ] **Step 5: Commit** `feat(shaders): decode Xenos microcode`.

### Task 15.3: SPIR-V builder, validation helper and the Vulkan shader harness

**Files:**
- Add the `thirdparty/SPIRV-Headers` submodule (pinned tag; header-only, SYSTEM include in both runtime variants and
  staged headers).
- Create `src/runtime/gpu/shader/spirv_builder.{h,cpp}`, `tests/support/spirv_tools.h`,
  `tests/support/vk_shader_harness.{h,cpp}` and `tests/test_gpu_shader_builder.cpp`.
- Modify `tests/CMakeLists.txt`, `tools/dev/Dockerfile` (+ `spirv-tools`), `docs/development.md`, `docs/adr/0002-license.md` (the
  SPIRV-Headers row, license verified from its `LICENSE`) and `packaging/appimage/licenses.sh`.

**Interfaces:**
- Produces:
  ```cpp
  namespace lr::gpu::shader {
  using Id = uint32_t;
  class SpirvBuilder {
   public:
    explicit SpirvBuilder(spv::ExecutionModel model);
    void capability(spv::Capability); void extension(std::string_view); void memory_model(spv::AddressingModel, spv::MemoryModel);
    Id type_void(); Id type_bool(); Id type_int(uint32_t width, bool is_signed); Id type_float(uint32_t width);
    Id type_vector(Id component, uint32_t n); Id type_array(Id element, Id length_const); Id type_runtime_array(Id);
    Id type_struct(std::span<const Id> members); Id type_pointer(spv::StorageClass, Id pointee); Id type_function(Id ret);
    Id type_image(Id sampled_type, spv::Dim dim, bool depth, bool arrayed, bool multisampled, uint32_t sampled /*1*/,
                  spv::ImageFormat format = spv::ImageFormatUnknown);
    Id type_sampler(); Id type_sampled_image(Id image_type);
    Id constant_u32(uint32_t); Id constant_i32(int32_t); Id constant_f32(float); Id constant_bool(bool);
    Id constant_composite(Id type, std::span<const Id>); Id spec_constant_u32(uint32_t spec_id, uint32_t default_value);
    Id variable(spv::StorageClass, Id pointer_type, std::string_view debug_name = {});
    void decorate(Id, spv::Decoration, std::initializer_list<uint32_t> = {});
    void member_decorate(Id struct_type, uint32_t member, spv::Decoration, std::initializer_list<uint32_t> = {});
    Id op(spv::Op, Id result_type, std::initializer_list<Id> operands);     // value-producing instruction
    void op_void(spv::Op, std::initializer_list<Id> operands);              // OpStore, OpKill, ...
    Id ext_inst(Id result_type, uint32_t glsl_std_450_op, std::initializer_list<Id> operands);
    Id new_block(); void set_block(Id); void branch(Id); void branch_conditional(Id cond, Id t, Id f);
    void selection_merge(Id merge); void loop_merge(Id merge, Id continue_target);
    void switch_(Id selector, Id default_block, std::span<const std::pair<uint32_t, Id>> cases);
    void begin_main(); void end_main(); void entry_point(std::string_view name, std::span<const Id> interface);
    void execution_mode(spv::ExecutionMode, std::initializer_list<uint32_t> = {});
    std::vector<uint32_t> finish();  // SPIR-V 1.5 header, sections in spec order; types/constants deduplicated
  };
  }
  ```
  - SPIR-V 1.5 (Vulkan 1.2). From SPIR-V 1.4 on, `entry_point`'s interface lists **every** global variable the
    entry point uses, not just Input/Output: the push-constant block, the private/function-independent globals and
    the descriptor arrays. `finish()` checks this and fails loudly (debug assert plus a test).
  - Decorations needed by the contract go through `decorate`/`member_decorate`: `DescriptorSet`, `Binding`, `Block`,
    `Offset`, `ArrayStride`, `Location`, `BuiltIn`, `Flat`, `Centroid`, `NonWritable`, `SpecId` and
    `AliasedPointer`/`RestrictPointer`. Image operations (`OpSampledImage`, `OpImageSample*`, `OpImageQuerySize*`)
    go through `op`.
  - `tests/support/spirv_tools.h`: `std::optional<std::string> spirv_val(std::span<const uint32_t>)` (nullopt =
    valid) and `std::string spirv_dis(std::span<const uint32_t>)`. Both run the container tools through
    `posix_spawn`, and tests `SKIP` when the tools are absent.
  - `vk_shader_harness.h`: `class ShaderHarness { static Result<ShaderHarness> create();
    Result<HarnessResult> draw(std::span<const uint32_t> vs, std::span<const uint32_t> ps, const HarnessInputs& in); }`.
    It draws `in.vertex_count` vertices (default: one triangle covering the target) into `in.width`×1 RGBA32F
    targets and reads them back.
    - `HarnessResult { std::array<std::vector<std::array<float,4>>, 4> color; std::vector<float> depth; }`.
    - `HarnessInputs` fields, added by the task that first needs them:
      - 15.3: `push_constants` (raw bytes) and `buffers` (BDA-addressed byte blobs);
      - 15.4: `vs_constants`/`ps_constants` (256 `float4` each), `bools` (8 words), `loops` (32), the shared-block
        overrides (`ndc_scale`, `ndc_offset`, `vte_flags`, `index_offset`, `min_index`, `max_index`, `alpha_ref`),
        `spec_alpha` (spec constant 0), `color_targets` (1..4) and `depth_target`;
      - 15.8: `fetch_slots` (`{slot, bytes, endian}` per vertex-fetch table entry);
      - 15.9: `textures` (`{fetch_slot, dim, w, h, d, mips, RGBA32F texels}`) and `samplers` (`{fetch_slot,
        address, filter, lod_bias}`), bound through the contract's sets 0..3 and index tables.

- [ ] **Step 1: Write the failing tests:**
  - `empty fragment shader validates`;
  - `types and constants are deduplicated`;
  - `structured if, loop and switch validate`;
  - `physical storage buffer load through a bitcast uvec2 validates`;
  - `entry point without a used global is rejected`;
  - `sampled image types and a sample instruction validate`;
  - `[vulkan] harness renders a builder-made constant colour`;
  - `[vulkan] harness samples a builder-made texture`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS (spirv-val clean, lavapipe).
- [ ] **Step 5: Commit** `feat(shaders): add a minimal SPIR-V builder and a Vulkan shader test harness`.

### Task 15.4: Translator skeleton — shader interface, contract and exports

**Files:**
- Create `src/runtime/gpu/shader/{contract.h,translator.h,translator.cpp,translator_export.cpp}` and
  `tests/test_gpu_shader_translate.cpp`.
- Modify `tests/CMakeLists.txt`, `tests/support/vk_shader_harness.*` and `docs/architecture.md`: the new "Contrato
  dos shaders traduzidos (plano B)", in Portuguese, with the numbers of `contract.h` and the Xenos facts of the
  design decisions.

**Interfaces:**
- Consumes: `Program` (15.2) and `SpirvBuilder` (15.3).
- Produces:
  ```cpp
  namespace lr::gpu::shader {
  inline constexpr uint32_t kTranslatorVersion = 1;
  inline constexpr uint32_t kNoParamGen = 0xFF;
  struct TranslateOptions { uint32_t ps_param_gen = kNoParamGen; bool ps_point = false;
                            uint16_t flat_mask = 0, centroid_mask = 0; };   // = the PS key modifiers, decoded
  struct ShaderInfo { std::bitset<96> vfetch_slots; std::bitset<32> tfetch_slots; std::array<uint8_t,32> tfetch_dims;
                      uint32_t interpolators_read; uint32_t colors_written; bool writes_depth, uses_kill; };
  struct TranslatedShader { std::vector<uint32_t> spirv; ShaderInfo info; };
  Result<TranslatedShader> translate(const Program& program, const TranslateOptions& options);
  }
  ```
  - `contract.h` holds the plan-B contract numbers from the decisions above:
    - push constants: 32 bytes (four `uvec2` BDAs);
    - shared block: 736 bytes, with the offsets;
    - vertex-fetch table entry: 16 bytes;
    - sets 0..3;
    - spec constant 0;
    - VS outputs at locations 0..15;
    - the export register numbers: VS 0..15 / 62 / 63, PS 0..3 / 61, memexport 32..37.
  - In this task: straight-line `exec`/`exec_end` only, with the registers as a function-local `vec4` array of
    `temp_count` (64 when `uses_relative_temps`, design decisions) and the vector ops `MAX` (mov), `ADD` and `MUL`.
  - VS inputs: `r0.x = float(clamp(gl_VertexIndex + index_offset, min_index, max_index))`, and every other register
    starts at 0.
  - VS exports:
    - position: the VTE flags are applied first (xy/z divide, `w0` reciprocal), then
      `gl_Position = p * ndc_scale + ndc_offset * p.w`;
    - interpolators 0..15;
    - `gl_PointSize = 1.0`; an exported point size (63) is dropped and counted in `ShaderInfo`.
  - PS inputs:
    - `r0..r15` come from the interpolators, the rest start at 0;
    - an input is declared for every `r<16` the program references;
    - inputs are decorated `Flat`/`Centroid` from `flat_mask`/`centroid_mask`.
  - `param_gen`: register `ps_param_gen` receives:
    - x = |floor(FragCoord.x)| with the sign bit set when back-facing;
    - y = |floor(FragCoord.y)| with the sign bit set when `ps_point`;
    - z and w per Xenia (point coordinate, or 0).
  - PS exports: `oC0..3`, depth (`FragDepth`, `DepthReplacing`) and the alpha test through spec constant 0.
  - Memexport (32..37) fails with `memexport (eA/eM) is not supported`.

- [ ] **Step 1: Write the failing tests:**
  - [vulkan], executed:
    - `vs exports position and interpolator 0 from constants`;
    - `ndc scale and offset are applied to the position`;
    - `VTE flags undo the divide by W before the ndc transform`;
    - `r0.x is the vertex index plus the index offset, clamped`;
    - `ps reads interpolator 5 by register index`;
    - `ps registers past the interpolators start at zero`;
    - `ps writes oC1 and depth` (reads back color target 1 and the depth target);
    - `param_gen encodes position, back-facing and point flags` (asserts the sign-bit encoding, front and back faces);
    - `flat interpolator is not interpolated`;
    - `alpha test greater-equal discards`;
  - checked with spirv-dis:
    - `vs declares all 16 interpolator outputs and a point size of 1`;
    - `ps declares only the interpolators it reads`;
  - other:
    - `memexport fails translation with its name`;
    - `every translated test module passes spirv-val`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(shaders): translate the Xenos shader interface to SPIR-V`.

### Task 15.5: Control flow — exec conditions, predicates, loops, jumps and calls

**Files:** Create `src/runtime/gpu/shader/translator_cf.cpp` and `tests/test_gpu_shader_cf.cpp`. Modify
`tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: 15.4.
- Produces: every `CfOp` translated.
  - **Algorithm** (not determined by the signature):
    - a program without jumps, calls or loops (`!has_jumps_calls_or_loops`) stays straight-line structured blocks;
    - otherwise `main` is one structured loop around an `OpSwitch` on a function-local `pc`, with one case per CF
      index that is a target or follows a branch. A case runs its clause and sets the next `pc`. `exec_end` and
      `return` from depth 0 leave the loop.
  - Loop and call state lives in function-local stacks of depth 4: the loop counter, `aL` and the return address.
    Loops follow ADR 0005 M5: start and step come from the constant, `is_repeat` keeps `aL`, and `aL` is restored on
    every exit, including a `cond_jmp` that leaves the loop body (the parked Task 13 item) and a `LOOP_END`
    predicated break (`is_predicated_break`/`pred_break_condition`).
  - **Iteration cap (planner's call):** only taken backward branches count (`LOOP_END` back-edges and backward
    `jmp`/`call`), capped at 2^20 per invocation. When hit, the shader leaves the loop with its outputs as written so
    far. Limit, documented in `architecture.md`: 4 nested loops of 255 iterations each (255^4) exceed the cap.
  - The predicate `p0` is a function-local bool. `cond_exec_pred*`, `cond_jmp` (with bool or predicate) and the
    per-instruction `predicated`/`pred_condition` follow Xenos semantics. A predicated ALU instruction is skipped
    with a branch; a predicated fetch follows the design decision "Kill and derivatives" (evaluate, then `OpSelect`).
  - Kill: the `killed` flag and the single `OpKill` at the end of `main` (design decisions), so the translator never
    emits `OpKill` inside the pc-switch.
  - `alloc` and `mark_vs_fetch_done` are no-ops.

- [ ] **Step 1: Write the failing tests** ([vulkan], executed through the harness unless noted):
  - `cond_exec follows bool constant 130`;
  - `cond_exec_pred skips when the predicate is false`;
  - `loop runs the count of loop constant 3 with its start and step`;
  - `is_repeat loop keeps aL`;
  - `nested loops restore aL`;
  - `jmp skips a clause`;
  - `cond_jmp on a bool constant`;
  - `call and return`;
  - `predicated alu instruction` (the fetch half moves to 15.8);
  - `loop_end with a predicated break exits and restores aL`;
  - `cond_jmp out of a loop restores aL`;
  - `r[aL+1] read inside a loop` (aL-relative temps, 64-register array);
  - **`runaway jump loop stops at the iteration cap`** (completes, no device lost);
  - `straight-line program has no switch` (spirv-dis).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(shaders): translate Xenos control flow`.

### Task 15.6: Vector ALU operations

**Files:** Create `src/runtime/gpu/shader/translator_alu.cpp` (vector part) and `tests/test_gpu_shader_alu_vector.cpp`.
Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: 15.4/15.5.
- Produces: all vector opcodes.
  - The opcodes: `ADD`, `MUL`, `MAX`, `MIN`, `SETE`, `SETGT`, `SETGTE`, `SETNE`, `FRAC`, `TRUNC`, `FLOOR`,
    `MULADD`, `CNDE`, `CNDGTE`, `CNDGT`, `DOT4`, `DOT3`, `DOT2ADD`, `CUBE`, `MAX4`, `PRED_SETE_PUSH`,
    `PRED_SETNE_PUSH`, `PRED_SETGT_PUSH`, `PRED_SETGTE_PUSH`, `KILLE`, `KILLGT`, `KILLGTE`, `KILLNE`, `DST`,
    `MAXA`.
  - Operand handling:
    - temp/constant sources with swizzle and negate;
    - `abs_constants`;
    - relative constants (`+a0`, `+aL`, with the `const_0`/`const_1` rule of ADR 0005 M5);
    - an out-of-range relative constant reads 0;
    - aL-relative temp sources and destination (`r[aL+n]`, clamped to 0..63);
    - write mask and saturate;
    - export writes.
  - Xenos multiply: a product with a 0 factor is +0, except when both operands are the same value. It applies to
    `MUL`, the product of `MULADD` (not the addend), every product in `DOT4`/`DOT3`/`DOT2ADD` and `DST`'s y.
  - `MAXA`: the result is max(a, b), and `a0 = clamp(floor(a.w + 0.5), -256, 255)`.
  - Kill ops set the `killed` flag (15.5).

- [ ] **Step 1: Write the failing tests** ([vulkan], executed):
  - `vector op table`: one table-driven case per opcode, with fixed inputs and expected outputs derived from the
    Xenos semantics and noted per row;
  - `mul of zero by infinity is zero` (and the same-operand exception: `x * x` with x = inf is inf);
  - `muladd keeps the addend when the product is zeroed`;
  - `dot products and dst zero their products`;
  - `maxa sets a0 clamped and returns the max`;
  - `aL-relative destination register is written`;
  - `swizzle and negate`;
  - `write mask keeps the other components`;
  - `vector saturate`;
  - `relative constant c[a0+2]`;
  - `relative constant with aL`;
  - `second constant operand uses const_1 addressing`;
  - `out-of-range relative constant reads zero`;
  - `cube op output layout` (tc, sc, 2·major-axis, face id — per Xenos ucode and Xenia; the fetch half is in 15.9);
  - `kill ops discard the pixel` (a fetch after the kill in the same shader still validates).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(shaders): translate Xenos vector ALU operations`.

### Task 15.7: Scalar ALU operations, predicates and address register

**Files:** Modify `src/runtime/gpu/shader/translator_alu.cpp` (scalar part) and `tests/CMakeLists.txt`. Create
`tests/test_gpu_shader_alu_scalar.cpp`.

**Interfaces:**
- Consumes: 15.6.
- Produces: all scalar opcodes.
  - Arithmetic:
    - `ADDs`, `ADD_PREVs`, `MULs`, `MUL_PREVs`, `MUL_PREV2s`, `MAXs`, `MINs`, `SUBs`, `SUB_PREVs`;
    - `SETEs`, `SETGTs`, `SETGTEs`, `SETNEs`;
    - `FRACs`, `TRUNCs`, `FLOORs`;
    - `MUL_CONST_0/1`, `ADD_CONST_0/1`, `SUB_CONST_0/1`;
    - `SIN`, `COS`, `RETAIN_PREV`.
  - Transcendentals:
    - `EXP_IEEE`, `LOG_CLAMP`, `LOG_IEEE`, `SQRT_IEEE`;
    - `RECIP_CLAMP`, `RECIP_FF`, `RECIP_IEEE`;
    - `RECIPSQ_CLAMP`, `RECIPSQ_FF`, `RECIPSQ_IEEE`.
  - Address register: `MOVAs`, `MOVA_FLOORs`.
  - Predicates: `PRED_SETEs`, `PRED_SETNEs`, `PRED_SETGTs`, `PRED_SETGTEs`, `PRED_SET_INVs`, `PRED_SET_POPs`,
    `PRED_SET_CLRs`, `PRED_SET_RESTOREs`.
  - Kills: `KILLEs`, `KILLGTs`, `KILLGTEs`, `KILLNEs`, `KILLONEs`.
  - State:
    - the previous scalar result `ps` persists across instructions;
    - `a0` is set by `MOVAs` (`clamp(floor(x + 0.5), -256, 255)`), `MOVA_FLOORs` (`clamp(floor(x), -256, 255)`) and
      the vector `MAXA` (15.6);
    - vector and scalar results that target the same register merge by write mask.
  - Xenos multiply (+0 for a 0 factor, except for the same operand twice) in `MULs`, `MUL_PREVs`, `MUL_PREV2s` and
    `MUL_CONST_0/1`.

- [ ] **Step 1: Write the failing tests** ([vulkan], executed):
  - `scalar op table` (one row per opcode, including the clamp/ff variants at 0 and ±inf);
  - `previous scalar result carries across instructions`;
  - `pred_set ops set p0 and the scalar result`;
  - `mova rounds and mova_floor floors into a0` (also checks the clamp at -256/255 and the result value written);
  - `mul_const of zero by infinity is zero`;
  - `vector and scalar results merge by write mask`;
  - `scalar kill discards the pixel`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(shaders): translate Xenos scalar ALU operations and predicates`.

### Task 15.8: Vertex fetch in the shader

**Files:** Create `src/runtime/gpu/shader/translator_fetch.cpp` (vertex part) and `tests/test_gpu_shader_vfetch.cpp`.
Modify `tests/CMakeLists.txt`, and extend the harness with a vertex-fetch table.

**Interfaces:**
- Consumes: 15.4–15.7 and `contract.h`'s vertex-fetch table.
- Produces: `vfetch` (full and mini).
  - Address computation:
    - the index is the source register component, rounded or floored per `is_index_rounded`;
    - dword offset = index × stride + offset, where offset is **signed** (-4194304..4194303);
    - byte address = entry.address + dword offset × 4;
    - an access with a negative dword offset or one past `size_bytes` returns 0 for every component (lower and
      upper bound both checked);
    - endian swap per entry (`8in16`, `8in32`, `16in32`).
  - Formats:
    - `8_8_8_8`, `2_10_10_10`, `10_11_11`, `11_11_10`;
    - `16_16`, `16_16_16_16`, `16_16_FLOAT`, `16_16_16_16_FLOAT`;
    - `32`, `32_32`, `32_32_32_32`;
    - `32_FLOAT`, `32_32_FLOAT`, `32_32_32_FLOAT`, `32_32_32_32_FLOAT`.
  - Conversion:
    - signed or unsigned, normalized or integer, `signed_rf_mode` and `exp_adjust`;
    - the destination swizzle includes the 0, 1 and keep selectors;
    - missing components are (0, 0, 0, 1).
  - A mini fetch reuses the slot, stride and computed index of the previous full fetch in the same program, with its
    own offset and format. A mini fetch with no earlier full fetch fails translation.
  - A predicated `vfetch` is evaluated unconditionally and merged with `OpSelect` (design decisions).
  - An unknown format fails translation and names the format.

- [ ] **Step 1: Write the failing tests** ([vulkan], executed):
  - `float4 vertex fetch with 8in32 swap`;
  - `k_16_16_FLOAT`;
  - `k_8_8_8_8 normalized unsigned and signed`;
  - `k_2_10_10_10 signed`;
  - `k_10_11_11 and k_11_11_10`;
  - `integer non-normalized fetch`;
  - `exp_adjust scales the result`;
  - `mini fetch reuses the index, slot and stride with its own offset and format`;
  - `index register is rounded`;
  - **`fetch past the slot size reads zero`**;
  - `negative offset before the slot start reads zero`;
  - `predicated fetch keeps the old value when the predicate is false` (moved from 15.5);
  - `unknown vertex format fails naming it`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(shaders): fetch vertices inside translated shaders`.

### Task 15.9: Texture fetch

**Files:** Modify `src/runtime/gpu/shader/translator_fetch.cpp` (texture part) and `tests/CMakeLists.txt`. Create
`tests/test_gpu_shader_tfetch.cpp`. Extend the harness with bindless textures and samplers.

**Interfaces:**
- Consumes: 15.8, and `contract.h`'s texture index tables and sets 0..3.
- Produces: `tfetch` for 1D (sampled from a height-1 `Texture2D` at v = 0.5), 2D, 3D and cube.
  - Cube coordinates come from the `CUBE` vector op layout (tc, sc, 2·major-axis, face id; x = tc, y = sc) and are converted to a direction.
  - LOD:
    - the PS uses implicit LOD unless the instruction asks otherwise;
    - **the VS always uses explicit LOD** (0 + bias);
    - also supported: `lod_bias`, register LOD (`setTexLOD`), gradients (`setGradientH/V`) and computed LOD.
  - Also supported: per-instruction offsets x/y/z and the destination swizzle.
  - Unnormalized coordinates are divided in the shader by `OpImageQuerySizeLod` (capability `ImageQuery`), because
    Vulkan unnormalized samplers forbid mips, 3D and cube. Samplers are always normalized.
  - A predicated `tfetch` is evaluated unconditionally and merged with `OpSelect` (design decisions).
  - `getTexLOD`, `getTexWeights` and `getBCF` fail translation naming the variant, until the game shows it needs one.

- [ ] **Step 1: Write the failing tests** ([vulkan], executed):
  - `2D fetch samples the bound texture`;
  - `1D fetch samples a height-1 texture`;
  - `3D fetch`;
  - `cube fetch after the cube op`;
  - `vertex shader texture fetch uses explicit lod` (the fork's DXC failure);
  - `lod bias and register lod`;
  - `gradients`;
  - `fetch offsets`;
  - `unnormalized coordinates on a mipmapped texture`;
  - `implicit-lod fetch under a predicate uses select`;
  - `unsupported tfetch variant fails naming it`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(shaders): translate Xenos texture fetches`.

### Task 15.10: Shader key, disk cache and runtime integration

**Files:**
- Create `src/runtime/gpu/shader/{shader_key,disk_cache}.{h,cpp}` and `tests/test_gpu_shader_cache.cpp` (rewritten).
- Rewrite `src/runtime/gpu/shader_cache.{h,cpp}`.
- Modify:
  - `src/runtime/gpu/{backend.h,null_backend.*,gpu_system.{h,cpp}}`;
  - `src/runtime/gpu/registers.{h,cpp}`: + `PA_CL_VTE_CNTL`, `PA_SU_VTX_CNTL`, `SQ_CONTEXT_MISC`, `VGT_INDX_OFFSET`,
    `VGT_MIN_VTX_INDX`, `VGT_MAX_VTX_INDX`, `VGT_MULTI_PRIM_IB_RESET_INDX` and `SQ_INTERPOLATOR_CNTL`, with indices
    from the public documentation (Xenia's `register_table.inc` as cross-check);
  - `src/runtime/gpu/pm4.cpp`: an `IM_LOAD`/`IM_LOAD_IMMEDIATE` with a nonzero `start` field is
    `Error{InvalidFormat, "IM_LOAD start 0x.. is not supported"}` (today it is ignored);
  - `src/runtime/main.cpp`: the `--shader-cache-dir <dir>` and `--dump-shaders` flags, and the shader summary at
    exit;
  - `src/pipeline/pipeline.cpp` (the `pipeline::launch` argv, next to `--gpu-trace-file`): it passes
    `--shader-cache-dir <root>/work/shaders/runtime`;
  - callers of the moved `microcode_hash`: `gpu_system.cpp`, `tests/test_gpu_shader_cache.cpp`,
    `tests/test_gpu_system.cpp`;
  - `tests/{CMakeLists.txt,test_gpu_system.cpp,test_gpu_pm4.cpp,test_gpu_registers.cpp,test_build_stage.cpp}` (the launch argv test lives in `test_build_stage.cpp`);
  - `docs/{architecture,pipeline}.md`.

**Interfaces:**
- Consumes: 15.2–15.9, and `microcode_hash`, moved from `runtime/gpu/shader_cache.h` into
  `lr::gpu::shader` in `shader_key.h`.
- Produces:
  ```cpp
  namespace lr::gpu::shader {
  struct ShaderKey { ShaderStage stage; uint64_t microcode_hash; uint32_t dwords; uint64_t modifiers;
                     auto operator<=>(const ShaderKey&) const = default; };
  uint64_t microcode_hash(const uint8_t* guest_microcode_be, uint32_t dwords);
  // PS: bits 0-7 param_gen register | 0xFF, bit 8 point list (from the draw), 16-31 flat mask, 32-47 centroid mask.
  uint64_t key_modifiers(ShaderStage, const RegisterFile&, const DrawInfo&);
  TranslateOptions key_options(const ShaderKey&);             // the decoded modifiers
  std::string cache_file_name(const ShaderKey&);              // "v<kTranslatorVersion>-<vs|ps>-<hash:016x>-<dwords>-<mod:016x>.spv"
  class DiskCache { public: explicit DiskCache(std::filesystem::path dir);
    std::optional<std::vector<uint32_t>> load(const ShaderKey&, std::span<const uint32_t> microcode) const;
    Result<void> store(const ShaderKey&, std::span<const uint32_t> microcode, std::span<const uint32_t> spirv); };
  }
  namespace lr::gpu {
  struct ActiveShaders { const shader::TranslatedShader* vs; const shader::TranslatedShader* ps;   // nullptr: none/failed
                         shader::ShaderKey vs_key, ps_key; };
  class ShaderCache { public:
    void set_disk_dir(std::optional<std::filesystem::path> dir);
    // IM_LOAD: copy + decode; returns the hash. Decode errors are kept and surface at translate().
    uint64_t load(ShaderStage, const uint8_t* guest_microcode_be, uint32_t dwords);
    Result<const shader::TranslatedShader*> translate(const shader::ShaderKey&);   // memory -> disk -> translator
  };
  }
  ```
  - The disk file holds a header (magic `LRSV`, `kTranslatorVersion`, the key), the microcode (for the collision
    check) and the SPIR-V. It is written to a temporary file and renamed.
  - Unreadable or mismatched files are ignored and retranslated (a warning to stderr). An unwritable directory means
    a warning and in-memory only.
  - `Backend::draw` gains `const ActiveShaders&`. `GpuSystem` copies microcode at `load_shader`. At each draw it
    computes both keys, translates new keys (any backend), records them and traces
    `shader ps 0x<hash> dwords=N mod=0x.. translated|failed: <reason>|disk`.
  - `ShaderLoadStats` becomes:
    - `loads`;
    - `std::vector<Key> keys`: one per distinct key, with its `ShaderKey`, `status` (translated, disk or failed),
      `reason`, and `first_swap` (the swap count when it first appeared);
    - `new_keys_last_100_swaps`.
    It replaces the 15.1 per-hash list.
  - At process exit (normal exit, `--exit-after-swaps`, exits 3/5/6), `main.cpp` prints one stderr line, from
    `ShaderLoadStats` rather than the trace, which is capped at 64 MiB:
    `[gpu] shaders: keys=N vs=N ps=N translated=N disk=N failed=N new_last_100_swaps=N`.
  - A failure is `GpuHooks::fatal("shader microcode 0x... (<stage>, N dwords) cannot be translated: <reason>")`
    only if `binds_shaders()`.
  - `--dump-shaders` also writes `<name>.txt` (the disassembly) next to each `.spv`.

- [ ] **Step 1: Write the failing tests:**
  - key:
    - `same microcode and modifiers give the same key`;
    - `D3D-patched fetch dwords give a different key that translates` (two microcodes differing in a `vfetch`
      format dword);
    - `param_gen and the point flag change the pixel shader key`;
    - `interpolator flat and centroid masks change the pixel shader key`;
    - `key options decode the modifiers`;
    - `new registers have the documented indices`;
    - `IM_LOAD with a nonzero start is an error`;
  - disk cache:
    - `translated module is written to and read from the disk cache`;
    - `cache file from another translator version is ignored`;
    - `corrupt cache file is retranslated`;
    - `hash collision with different microcode is retranslated`;
  - GpuSystem:
    - `IM_LOAD_IMMEDIATE microcode is copied before the packet ends`;
    - `untranslatable shader is fatal with a backend that binds shaders`;
    - `untranslatable shader is only reported with the NullBackend`;
    - `--trace-gpu names translated and failed shaders`;
    - `shader summary line at exit counts keys by status`;
  - other:
    - `launch passes the runtime shader cache dir`;
    - `translator output fingerprint matches kTranslatorVersion`: a golden XXH3 of the SPIR-V of 3 reference
      synthetic shaders, keyed by version, so a changed output with no version bump fails.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS, repeat x10 (GPU thread).
- [ ] **Step 5: Commit** `feat(gpu): translate loaded shaders at runtime with a disk cache`.

### Task 15.11: [ISO] The translator gate (replaces the Task 15 gate)

**Files:** Create `tools/shader_cache_stats.sh`: it runs `spirv-val --target-env vulkan1.2` over
`work/shaders/runtime/*.spv` and prints only counts (files, valid, invalid, per stage). Modify
`tests/test_integration_m3.cpp` and `docs/adr/0005-m3-plan-deviations.md` (entry M9).

- [ ] **Step 1:** With an empty `work/shaders/runtime/`, run `container.sh run -- --headless --trace-gpu
  --exit-after-swaps 600` (`timeout 1800`, NullBackend). Read the counts from the exit summary line
  `[gpu] shaders: …` (Task 15.10), not from the capped trace. Read the per-key `first_swap` values from the
  `shader … translated|failed` trace lines only as a cross-check. Run `tools/shader_cache_stats.sh`.
- [ ] **Step 2:** Run it again: every key must come from `disk`, with 0 new translations.
- [ ] **Step 3 (pass criteria):**
  - 0 `failed`;
  - 0 `spirv-val` errors;
  - the 7 hashes of ADR 0005 M7 are all among the translated keys;
  - the second run is 100% disk hits.

  Record the distinct keys per stage and `new_last_100_swaps` from the summary line.
- [ ] **Step 4 (fix loop, like Task 12):** each failure that falls inside the feature list of Tasks 15.4–15.9 gets a
  synthetic-microcode test that reproduces it (never game microcode in a test), then the fix, then one commit.
  Bump `kTranslatorVersion` whenever the output changes.
- [ ] **Stop rule: STOP and report to the controller** if any of these holds:
  - (a) a loaded shader needs a feature outside 15.4–15.9 (memexport, `getTexWeights`, …);
  - (b) 3 fix rounds have not reached 0 failures;
  - (c) planner's call, out of the two options the review offered: `new_last_100_swaps` > 10. New keys still appear
    at more than 1 per 10 swaps over swaps 500–600, when a static title screen should have a stable shader set. That
    is a sign of per-draw microcode patching that needs a key redesign. Before stopping, rerun with
    `--exit-after-swaps 900`: a loading screen that is still running ends the series by itself;
  - (d) a module fails `spirv-val` for a reason in the builder's design rather than a bug.
- [ ] **Step 5:** Add the integration test `the game's loaded shaders all translate` (ISO, headless,
  `--exit-after-swaps 300`, 0 `failed` lines in the trace). Write ADR 0005 M9 "Portão do tradutor" with the numbers.
- [ ] **Step 6: Commit** `test(integration): every shader the game loads translates` + `docs: record the translator
  gate` (+ the fix commits).

## Part E — Vulkan renderer

### Task 16: Our own GPU-side shaders build (revised for Plan B)

**Files:**
- `src/runtime/gpu/vk/shaders/*.{vert,frag,comp}`: GLSL 4.50 for blit, clear, untile and the resolve placeholders.
- `cmake/LrGpuShaders.cmake`: `lr_compile_gpu_shaders(<target> <sources…>)` runs
  `glslangValidator -V --target-env vulkan1.2` at **launcher build time** and turns the result into
  `gpu_shaders.h` byte arrays. They ship inside **`liblr_vulkan.a` only** (the renderer is their only user). Configure
  fails with an install hint when `glslangValidator` is missing.
- `tools/dev/Dockerfile` (+ `glslang`), the `AGENTS.md` toolchain line, `docs/development.md`, and
  `docs/adr/0002-license.md`: a glslang row marked "build tool, not distributed".
- `packaging/appimage/build.sh`: checks for `glslangValidator` before configuring, with the same install hint, and
  writes `glslangValidator --version` to the build log. `docs/packaging.md` records the version used. Planner's
  call: record the version instead of pinning it, because the Arch package cannot be pinned in the Dockerfile without
  an archive mirror. A different version can change only our own GLSL's SPIR-V, which the tests validate.
- `tests/test_gpu_vk_blit.cpp`.

No DXC (ADR 0005 M8), and the doctor checks no shader compiler: an installed game compiles no shader.

- [ ] **Step 1: Write the failing tests:**
  - `[vulkan] blit shader copies a texture to an offscreen image` (lavapipe);
  - `every built-in shader passes spirv-val`;
  - `appimage build refuses to start without glslangValidator` (script test with an empty PATH).
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `build(gpu): compile the renderer's own GLSL shaders at build time`.

### Task 17: Presenter, swapchain, headless, screenshots

**Files:** Create `src/runtime/gpu/vk/presenter.{h,cpp}`, `src/runtime/gpu/screenshot.{h,cpp}`,
`thirdparty/stb/stb_image_write.h` (pinned single header, public domain/MIT, noted in ADR 0002);
modify `src/runtime/main.cpp` (window loop: SDL window with `SDL_WINDOW_VULKAN`, GPU backend selection,
`--screenshot-after <N> <png>`; `--exit-after-swaps <N>` already exists since Task 12 and is only hardened here),
`tests/test_fake_game.cpp`, `src/pipeline/doctor.cpp`
(Vulkan probe: runs `legacyrecomp-game --probe-vulkan` or an equivalent small probe binary; reports missing pieces).

**Interfaces:**
- Produces: `class Presenter { static Result<Presenter> create(Device&, void* sdl_window /*nullptr = headless*/);
  void present(VkImage frame, uint32_t w, uint32_t h); Result<void> capture_png(const std::filesystem::path&); }`;
  `VkBackend : gpu::Backend` skeleton whose `swap()` presents the front buffer (from guest memory for now: untiled
  copy of the frontbuffer surface).
- Plan B additions:
  - Staging (ruling R-stage): `lr_vulkan`, `lr_vulkan_thirdparty` and the Task 16 shader arrays are staged as
    `runtime/lib/liblr_vulkan.a`, the headers of `runtime/gpu/vk/*.h` and `runtime/gpu/shader/*.h` are staged, and
    the game template links them.
  - `Device::create` also requires `maxVertexOutputComponents >= 64` and `maxFragmentInputComponents >= 64`. This is
    the carried Task 13 item; plan B needs only 64, the guaranteed minimum.
  - `Device` calls `vkDeviceWaitIdle` before destroying anything (parked from Task 5).
  - `--exit-after-swaps` and `--screenshot-after` accept only plain decimal digits; `"+5"` and `" 5"` are usage
    errors with exit 2 (carried from Task 12).
- [ ] **Step 1: Write the failing tests:**
  - fake game `clear-color frame is captured`: the fake guest writes a solid frontbuffer and calls VdSwap, and the
    `--screenshot-after 1` PNG has that color;
  - **`no Vulkan device gives a clear error`**: exit 2, and the message names the missing piece;
  - **`closing during rendering exits cleanly`**: SIGTERM mid-run gives exit 0 and no validation errors;
  - `doctor reports Vulkan status`;
  - `exit-after-swaps rejects signs and spaces`;
  - `device without 64 vertex output components is refused`: the check is a pure function over
    `VkPhysicalDeviceLimits`;
  - the template guard test still compiles the game with the staged Vulkan library.
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
8in16/8in32/16in32, format table), `src/runtime/gpu/vk/{textures,samplers}.{h,cpp}`, `tests/test_gpu_textures.cpp`.

**Interfaces:**
- Produces: `uint32_t tiled_offset_2d(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t bytes_per_block)`;
  `enum class XenosFormat`; `std::optional<VkFormat> host_format(XenosFormat)`; `class TextureCache {
  VkImageView get(const TextureFetch& fetch_constant); void invalidate_range(uint32_t phys, uint32_t size); }`;
  DXT1/2_3/4_5 → BC1/2/3, DXN → BC5, CTX1 decoded to RG8, 8888/565/1555/4444/8/88 etc.; invalidation by write
  watch (per-page hash compared each frame — the implementer may choose page protection instead and document it).
- Plan B contract additions:
  - a 1D texture fetch constant becomes a height-1 `VkImage` in the `Texture2D` heap (set 0);
  - the fetch constant's swizzle is applied through the image view's component mapping, and the swizzle is part of
    the cache key;
  - `TextureCache::heap_index(fetch_slot)` gives the indices the Task 20 draw writes into the shared block's tables;
  - signed texture formats are a named fatal in M3 until the game needs them;
  - `class SamplerCache { uint32_t heap_index(const TextureFetch& fetch_constant); }` (planner's call: Task 19, next
    to the textures).
    - It builds a `VkSampler` from the fetch constant: clamp/wrap/mirror/border address modes per axis, min, mag
      and mip filters, anisotropy (clamped to the device limit), mip range, LOD bias and border color
      (transparent black, opaque black or opaque white).
    - Samplers are always normalized (Task 15.9).
    - Samplers are cached by those fields and stored in the set-3 sampler heap. The Task 20 draw writes the index
      into the shared-block sampler table at offset 384.
- [ ] **Step 1: Write the failing tests:**
  - `tiled offset matches known values` (published Xenos tiling examples);
  - `sampler fields come from the fetch constant` (pure function: fetch constant → `VkSamplerCreateInfo`);
  - `equal sampler states share one heap entry`;
  - `endian swap modes`;
  - `[vulkan] DXT1 tiled texture uploads correctly`;
  - `CTX1 decode`;
  - **`edge-size and unsupported-format textures`**: 1×1, 1000×3, and 8192×8192 RGBA8 within memory; an
    unsupported format gives a fatal naming it;
  - `modified guest memory invalidates the texture`;
  - `1D texture uploads as a height-1 2D image`;
  - `fetch constant swizzle maps to the view`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS.
- [ ] **Step 5: Commit** `feat(gpu): add the texture cache with Xenos untiling`.

### Task 20: Draw path — vertex data, state, pipelines, translated shaders (revised for Plan B)

**Files:** Create `src/runtime/gpu/vk/{pipelines,draw}.{h,cpp}` and `tests/test_gpu_draw.cpp`. Modify the VkBackend.

**Interfaces:**
- Consumes:
  - `ActiveShaders` and `ShaderCache` (Task 15.10);
  - `contract.h` (Task 15.4);
  - `Edram` (Task 18), and `TextureCache::heap_index` and `SamplerCache::heap_index` (Task 19).
- Produces:
  - `VkBackend::binds_shaders()` returns `true`: the carried item, so an untranslatable shader is fatal with the
    Vulkan renderer.
  - `class PipelineCache { Result<VkPipeline> get(const PipelineKey&); }`, where `PipelineKey` = (VS key, PS key,
    render-target formats and MSAA, blend, depth, stencil and raster state, topology, alpha-test spec constant).
    `VkShaderModule`s are cached per `ShaderKey`. There is no vertex input state (vertex fetch happens in the
    shader).
  - Per draw, in a per-frame upload ring with BDA:
    - the VS and PS float constants (256 `float4` each, little-endian, from the register file);
    - the shared block:
      - the texture and sampler heap indices;
      - the 256 bools and 32 loops;
      - `ndc_scale`/`ndc_offset`, from `PA_CL_VPORT_*`, `PA_SU_SC_MODE_CNTL`, the window offset and the pixel
        center (`PA_SU_VTX_CNTL`);
      - the VTE flags (`PA_CL_VTE_CNTL`);
      - the alpha reference;
      - the index offset and the min and max index (`VGT_INDX_OFFSET`, `VGT_MIN/MAX_VTX_INDX`);
    - the index buffer, for indexed draws: `DrawInfo.index_words` indices of `index_size` (16 or 32 bits) are read
      from `index_base_phys` (translated and bounds-checked). They are endian-swapped per `index_endian` (`8in16`,
      `8in32`, `16in32`) and copied into the upload ring.
      - When primitive reset is enabled (`PA_SU_SC_MODE_CNTL.multi_prim_ib_ena`), every index equal to
        `VGT_MULTI_PRIM_IB_RESET_INDX` becomes all-ones and `primitiveRestartEnable` is set.
      - Reset on a list topology, which Vulkan 1.2 forbids, is a named fatal.
      - The offset and the min/max clamp are applied in the shader (r0.x, Task 15.4), so the draw uses
        `vertexOffset = 0`.
      - Auto-index draws use `vkCmdDraw`.
    - the vertex-fetch table: for every slot in `vs->info.vfetch_slots`, the guest range named by the fetch
      constant (base and size, translated physical→guest and bounds-checked) is copied raw and the entry
      {address, size, endian} written. An untranslatable address is fatal and names it (Review Focus 2).
  - Push constants (32 bytes).
  - Dynamic viewport and scissor.
  - Primitive types:
    - point, line and triangle lists, strips and fans;
    - rect list and quad list through index expansion;
    - anything else is a fatal naming the type.
  - A depth-only draw (`RB_MODECONTROL.edram_mode` = depth only; no PS bound) uses no fragment stage.
  - The PS key's point flag and interpolator masks come from this draw (`key_modifiers(…, DrawInfo)`, Task 15.10).
- [ ] **Step 1: Write the failing tests** ([vulkan]):
  - `triangle from IM_LOAD_IMMEDIATE microcode renders`: synthetic microcode from `xenos_shader_builder.h` → PM4
    stream → GpuSystem → VkBackend → resolved pixels;
  - `blend state from registers`;
  - `quad list expansion`;
  - `rect list expansion`;
  - `vertex k_16_16_FLOAT with 8in32 swap through the fetch table`;
  - `16-bit indices with 8in16 swap`;
  - `32-bit indices with VGT_INDX_OFFSET reach r0.x`;
  - `reset index becomes primitive restart on a strip`;
  - `indices outside min and max are clamped`;
  - `depth-only draw has no fragment stage`;
  - `sampler state from the fetch constant reaches the shader` (clamp vs wrap visible in the pixels);
  - `alpha test greater-equal discards`;
  - `ndc transform matches the D3D pixel center`;
  - `pipeline is reused for the same state`;
  - `untranslatable shader is fatal with the Vulkan backend`;
  - `vertex fetch constant at an unallocated physical address is fatal`.
- [ ] **Step 2:** FAIL. **Step 3:** Implement. **Step 4:** PASS, validation clean.
- [ ] **Step 5: Commit** `feat(gpu): draw with translated shaders`.

### Task 21: Fake game draws a textured quad end to end

**Files:** `tests/support/fake_generated/*`, `tests/test_fake_game.cpp`.

- [ ] **Step 1: Write the failing test** `fake guest draws a textured quad` (revised for Plan B). The hand-written
  guest code:
  1. initializes the GPU;
  2. writes synthetic VS/PS microcode (encoded with `xenos_shader_builder.h`: a `vfetch` of position and UV, and a
     2D `tfetch`) into guest memory and loads it with `IM_LOAD`;
  3. uploads a tiled DXT1 texture and a quad;
  4. swaps.

  `--screenshot-after 1` must show the texture's colors in the expected region. There is no container and no install
  step: the runtime translator handles it, and the disk cache goes to the test's temporary root.
- [ ] **Step 2:** FAIL. **Step 3:** Fix what breaks. **Step 4:** PASS x10.
- [ ] **Step 5: Commit** `test(gpu): render a textured quad from a fake guest`.

## Part F — The real game

### Task 22: [ISO] Burn-down to the title screen

- [ ] **Step 0:** Create `tools/frame_stats.py` (Python 3, run only inside the container, standard library only:
  `zlib` PNG decode). It prints one line, `colors=N mean_luminance=F edge_fraction=F`, and nothing else. Test:
  `frame_stats on a synthetic PNG` (a checkerboard written by the test → known numbers; ctest label `unit`). Commit
  `feat(tools): measure frame statistics without viewing the frame`.
- [ ] **Step 1:** `install --force build`, then `run -- --headless --trace-gpu --screenshot-after 300 /data/shot.png`
  (`timeout 1800`; translated shaders go to `work/shaders/runtime/`). Look only at:
  - the exit report;
  - the GPU counters and the `shader … translated|failed` counts;
  - objective image statistics from a tools script (`tools/frame_stats.py`: distinct colors, mean luminance, edge
    fraction).

  Never look at the image itself in context, and never commit it.
- [ ] **Step 2 (repeat):** fix whatever blocks or renders wrong, with a failing unit test first, and commit each fix.
  That covers an unknown opcode or format, missing state, a fault, a validation error, or a shader that fails to
  translate or renders wrong (synthetic-microcode test, `kTranslatorVersion` bump).
  - Carried items to resolve here when they show up, each with its own test:
    - the unguarded jump table `0x8235B7AC` (indirect call `0x8235B828`): a `switch_scan` rule;
    - function discovery must not emit functions at import-thunk addresses: replace the runtime workaround
      `762ba3d` with a root-cause fix;
    - `func_table` "first wins" drops duplicates silently: log and count them, fix the root cause in function
      discovery, and note it in `roadmap.md`;
    - the loader's whole-image copy assumes `image.data` holds `image.size` (BASIC compression): patch or document
      it;
    - a RAII guard for the KPCR current-CPU restore;
    - `EVENT_WRITE_ZPD` occlusion counts and `0xFFFFFEED` markers, if D3D queries deadlock (exit 6) (parked from
      Task 8);
    - which MMIO status register the game polls (`RBBM_STATUS` `0x5D0` vs `0x174`, from Task 9), if still open;
    - the PacedBySwap rate ruling of Task 10, if vblank-driven logic runs too fast;
    - escaping control characters in the message-box print, and null-argument message-box tests (parked from
      Task 11).
  - Already done: the CPU index on interrupts (vblank on CPU 2), from Task 12 (`architecture.md`).
  - Still M4, not here: the placeholder hardware answers (storage, controller, Kinect, voice, network, privileges)
    and `XamContentCreateEnumerator` ignoring `device_id`.
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

### Task 24: AppImage, licenses, docs, close M3 (revised for Plan B)

**Files:**
- `packaging/appimage/build.sh`: version `0.3.0-m3`; the staged runtime now holds the translator, SPIRV-Headers
  (headers only) and `liblr_vulkan.a`, all built by zig for glibc 2.31. Record the `readelf` floor of the game
  binary in `docs/packaging.md`.
- `packaging/appimage/licenses.sh`: + SPIRV-Headers, volk, VMA and Vulkan-Headers. The DXC, XenosRecomp, zstd and
  smol-v rows are gone since Task 15.1.
- `docs/{architecture,pipeline,packaging,roadmap,kernel-coverage}.md`, all seven READMEs,
  `docs/adr/0005-m3-plan-deviations.md` and the spec status.

Moot carries (ADR 0005 M8), stated in ADR 0005 and closed: the bundled DXC (layout, rpath, mandatory in the doctor),
XenosRecomp in the AppImage (`-DLR_BUILD_XENOSRECOMP=OFF`), XenosRecomp with zig/libc++, and the
`libdxcompiler.so` rpath. Spec §12 criterion 4's "com o DXC embutido" no longer applies.

- [ ] **Step 1:** Build the AppImage in the container. In a clean `archlinux` container with no compilers on PATH,
  `install --iso` + `run -- --headless --screenshot-after 600 …` must pass the Task 22 thresholds (lavapipe), and
  `work/shaders/runtime/` must be filled.
- [ ] **Step 2:** Docs:
  - GPU architecture: ring buffer, EDRAM, texture cache, the runtime translator and its contract;
  - the seven-stage pipeline;
  - packaging: no shader compiler, the Vulkan loader comes from the system;
  - roadmap: M3 done, M4 next, the remaining gaps (memexport, …);
  - READMEs: the status in each language, honestly (title screen drawn, no input or sound yet);
  - ADR 0005 complete (every ruling);
  - `docs/kernel-coverage.md` regenerated from the real build (carried from Task 10).
- [ ] **Step 3:** Full unit suite + both integration tests green.
- [ ] **Step 4: Commit** `docs: close M3`.
