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


## Environment notes (controller)
- Windows host; build/test only via tools/dev/container.sh (Docker). Restart Docker Desktop via PowerShell Start-Process "C:\Program Files\Docker\Docker\Docker Desktop.exe" if down.
- Repo: D:\Jogos Teste\legacy-launcher (space in path), branch feat/m3. No Python on host. Never push. Never read game data.

## Plan B Global Constraints (from plan, 2026-10-22)
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


## Review Focus 6-7 (plan B)
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
