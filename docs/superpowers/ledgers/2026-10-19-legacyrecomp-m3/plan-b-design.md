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

