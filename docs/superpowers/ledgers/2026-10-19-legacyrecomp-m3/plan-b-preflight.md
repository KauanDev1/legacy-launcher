# Pre-flight review of the plan B amendment (verdict: fix first)

Fix all of the following in the plan (and the ADR draft where relevant). Line numbers refer to the uncommitted plan.

## Critical
- C1 (729–735, 870–876, 1155–1172): VS `r0.x` = vertex index is never initialised, and no task uploads the index buffer. Fix:
  - 15.4: `r0.x = float(gl_VertexIndex)` plus a base index from the shared block.
  - Task 20: index buffer conversion (endian swap, VGT_INDX_OFFSET, min/max/reset index, 16/32-bit).
  - Tests for both.
- C2 (519, 809, 849–852): the 0×anything = 0 rule.
  - Cases: vector MUL, MAD (product only, not the addend), DP4/DP3/DP2ADD, DST.y; scalar MULs, MUL_PREVs, MUL_PREV2s, MUL_CONST_0/1.
  - Result is +0. The check is skipped when both operands are the same value.
  - Add a scalar test: `mul_const of zero by infinity is zero`.
- C3 (610, 729–730): aL-relative temp registers (`r[aL+n]`; only absolute and aL-relative, not a0).
  - Add `is_src_temp_relative`, `vector_dest_rel`, `scalar_dest_rel` to AluInstr, and the relative fields to FetchInstr.
  - When any relative temp is used, size the register array to 64 with a clamped index, or key it on the SQ_PROGRAM_CNTL register count.
  - Test: `r[aL+1] read inside a loop`.

## Important
- I1 (498–500, 732): VTE flags from PA_CL_VTE_CNTL (XYDividedByW, ZDividedByW, WNotReciprocal) are applied before ndc scale/offset. Add a `uint flags` field to the shared block, bump its size, and test it.
- I2 (608, 15.5): LOOP_END `is_predicated_break`. Add it to CfInstr and add a test in 15.5.
- I3 (660–679): SpirvBuilder has no `type_image`, `type_sampler` or `type_sampled_image`, and no image decorations, so 15.9 cannot be built. Add them, plus an image-sampling test in 15.3. Also state:
  - with SPIR-V 1.4+, the entry-point interface lists every global;
  - BDA via OpBitcast from uvec2, or `shaderInt64` required (choose one).
  - Name the `HarnessInputs` fields that 15.4/15.8/15.9 need: oC1 + depth readback, the fetch table, textures.
- I4 (1119–1166): nothing produces samplers. Task 19 or 20 adds a `SamplerCache` built from the fetch constant (address mode, filter, aniso, mip range, LOD bias, border) that fills the sampler table at shared-block offset 384.
- I5 (886): mini vfetch reuses the address/index of the previous vfetch_full, with its own offset and format. The offset is signed (−4194304..4194303), so the bounds check needs a lower bound. Add a test with a negative offset.
- I6 (1029): gate stop rule (c), "keys at 600 > 2× keys at 300", is noisy, and the counts come from a trace capped at 64 MiB. Replace it with a per-frame new-key rate after the title screen, or with "≥ 2 keys differing from an earlier key only in vfetch dwords, growing per frame". Print a `ShaderLoadStats` summary at exit and read the counts from it.
- I7 (505, 744): param_gen.
  - x = |floor(frag.x)|, with the sign bit set when back-facing.
  - y = |floor(frag.y)|, with the sign bit set for points.
  - The key needs a point flag; the test asserts the encoding; define the modifiers → TranslateOptions mapping.
- I8 (1041–1048, 1258): the AppImage build.sh needs glslangValidator. Task 16 adds the check and install hint to build.sh, and records or pins the glslang version (Dockerfile).
- I9 (785, 822): tests that depend on later tasks.
  - Move `predicated alu and fetch instructions` (its fetch part) to 15.8.
  - Reword the cube test in 15.6 to check only the CUBE output layout.

## Xenos facts to state in contract.h / 15.4
- VS: interpolators 0–15, position 62, point size/edge/kill 63.
- PS: colors 0–3, depth 61.
- eA = 32 and eM = 33–37 (memexport): fail loudly. This is why the gate's "PS export 32" failures are real memexport.
- PS r0..r(n−1) are initialised from the interpolator mask, the rest to 0. "Declares inputs only for registers it reads" means any r<16 that is referenced.
- MOVA/MAXA:
  - a0 = clamp(floor(x+0.5)), or floor(x) for the _floor variant, clamped to [−256, 255].
  - The result is max(a, b).
  - Line 851: "vector MOVA" should read `MAXA`.
  - The test at 858 must check the clamp and the result value.
- Kill: use demote or a killed flag, never OpKill mid-switch, because later tfetches need derivatives. No implicit derivatives in non-uniform control flow (the pc-switch).
- 15.9: unnormalized coordinates are scaled in the shader with OpImageQuerySize, because Vulkan unnormalized samplers forbid mips, 3D and cube.
- pm4.cpp:443 ignores the IM_LOAD `start` field. Make start≠0 a named fatal (Review Focus 6).

## Gaps / minor
- 15.10 renames `microcode_hash` into `lr::gpu::shader` in shader_key.h. List the callers to update (gpu_system.cpp, tests) and the new ShaderLoadStats fields.
- Flat shading / centroid come from SQ_INTERPOLATOR_CNTL (interpolator mask); add them to the key or to the task.
- Point lists write `PointSize = 1`.
- "PS absent" (1172) means RB_MODECONTROL depth-only.
- registers.h gains PA_CL_VTE_CNTL, PA_SU_VTX_CNTL, SQ_CONTEXT_MISC, VGT_INDX_OFFSET and SQ_INTERPOLATOR_CNTL.
- Carry into 15.5 the parked ledger item: a cond_jmp that leaves a loop skips the aL restore.
- Line 770: the 65 536 iteration cap could cut a legal 4-deep nested loop (255⁴). Count only backward jumps, or document the limit.
- Renderer shader byte arrays live in `lr_vulkan` only. Line 1045 contradicts line 1073 and the ADR draft.
- Task 22 uses `tools/frame_stats.py`, which no task creates. Add a step.
- Line 1064: `--exit-after-swaps` already exists (Task 12).
- Stale plan-A text in the header Architecture (8–11), Tech Stack (20–21), Global Constraints line 41 (reword to "untranslatable microcode") and File Structure (99–116). Update these to plan B.
- The M8 ADR draft must also list spec §5.4 (vertex formats), §11 (fork cases) and §12 criterion 5 (`pipeline.md` shaders stage) as moot.
- Each of 15.2–15.10 lists tests/CMakeLists.txt in Files.
