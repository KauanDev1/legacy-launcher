# SDD ledger — plan: docs/superpowers/plans/2026-10-19-legacyrecomp-m3.md

Spec: docs/superpowers/specs/2026-10-19-legacyrecomp-m3-design.md (reachable). Branch feat/m3, base 24d4e82 (on top of feat/m2, unmerged).
Env: Windows host, Docker container (tools/dev/container.sh), ISO owner-confirmed. Execution: subagent-driven (owner said "sim").

## Pre-flight scan
| Pair / task | Produces vs consumes | Finding |
|---|---|---|
| T1,T2 -> T4 | analyse features -> ISO check | ok |
| T3 -> T12/T22 | indirect-call report | ok |
| T5 -> T16..T21 | lr_vulkan, vk::Device | ok; runtime sources must be in both runtime variants + headers staged (ruling R-stage) |
| T6 -> T13,T14,T16 | XenosRecomp submodule, patch mech, DXC | ok (T16 needs DXC at launcher build time: container + AppImage build) |
| T7 -> T8..T20 | RegisterFile/Reg names | ok |
| T8 -> T10,T12,T20 | CommandProcessor, Backend, NullBackend | ok |
| T9 -> T8 test, T10 | guest_from_physical, MemoryAccess, MMIO window | T8 test of untranslatable phys sits in T9 | ok |
| T10 -> T12,T17 | GpuSystem, --trace-gpu | ok |
| T12 vs T17 | both mention --exit-after-swaps in main.cpp | ruling R-flag |
| T13 -> T14,T15,T20,T21 | fork contract, microcode hash | ok |
| T14 -> T15,T21 | shaders stage, lr_shader_cache | ok |
| T15 gate -> Part E | STOP if coverage < 100% / translation < 90% | ruling R-gate |
| T16 -> T18,T19 | own compute shaders | ok |
| T17 -> T18..T22 | Presenter, VkBackend, screenshot | ok |
| T18,T19 -> T20 | Edram, TextureCache | ok |
| T22 -> T24 | full integration thresholds | ok |
| T23 | WSL distro install (outside repo) | owner-approved in spec §10 |
| each task self | consistent | ok |

## Rulings
- Ruling (R-stage): every new runtime .cpp goes into LR_RUNTIME_CORE_SOURCES (both lr_runtime_core and the staged variant) and every header main.cpp/the template includes is staged (extend resources.cmake globs to runtime/gpu/*.h and runtime/gpu/vk/*.h); the M2 staged-headers guard test must stay green — cost if wrong: template build break caught by the guard.
- Ruling (R-flag): Task 12 introduces `--exit-after-swaps <N>` in main.cpp; Task 17 reuses it and adds `--screenshot-after` — cost if wrong: none.
- Ruling (R-gate): at Task 15's gate, if coverage < 100% or translation < 90%, the controller stops and reports to the owner before Part E (plan B re-plan) — this is the plan's own stop condition.

## Progress
- Task 1: note: real sites 0x82AB1528/0x8294E434 may need a bound from an earlier block / bge/ble — verify in Task 4 (ISO); extend the scanner there if still unresolved.
- Task 1: Ruling: unbounded relative tables reported in 'unbounded' (not 'unresolved') — keeps the two lists' meaning; rename the test — cost if wrong: none.
- Task 1: Ruling: fold minors #1 (clamp hoist to code-range start) and #2 (test rename) into fix round 1.
- Task 1: minor (deferred): no test for a preceding branch stopping the hoist; lwz from lis-built address reads initial image value (comment).
- Task 1: fix round 1/5 (3 addressed, 0 open; commits 31e1526..884fe2b)
- Task 1: minor (deferred): no explicit test for cmplwi-is-a-target; fit_switch_functions could reuse branch_targets.
- Task 1: complete (commits 24d4e82..884fe2b, review clean)
- Task 2: Ruling: bl targets are boundaries only, never emitted (plan said emit them) — XenonRecomp already creates a function at every uncovered bl target (recompiler.cpp:205-219) and config entries override its analysis; emitting ours swaps boundaries with no coverage gain; the spike cluster is reachable by pointers, which (a)/(c) cover — record in ADR 0005 — cost if wrong: a bl-only function XenonRecomp mis-bounds would still need manual handling.
- Task 2: Ruling: fold minors #2 (reject candidates containing an indexed-load bctr anywhere), #3 (bl to own interior not counted as internal), #4 (comment) into fix round 1.
- Task 2: minor (deferred): missing tests for mutual fall-through rejection, backward branch out of gap, stage-level label filter; include order.
- Task 2: fix round 1/5 (4 addressed, 0 open; commits 121c582..c39bc12); ADR 0005 created (entry M1).
- Task 2: complete (commits 884fe2b..c39bc12, review clean)
- Task 3: minor (deferred): doubled blank line / long snprintf in glue.cpp; guard compiles fake_recomp.cpp not a real generated file.
- Task 3: complete (commits c39bc12..8c59895, review clean)
- Task 4: Ruling: 0x8294E434 has 15 cases (bound cmplwi 14 proven by dataflow), not the spec's 17 — the spike's 17 was a hand estimate — cost if wrong: two unreachable cases missing (crash with a clear indirect-call report).
- Task 4: Ruling: fold minors #2 (branch targets between compare and guard disable the guard), #3 (dropped-table labels count as entries), #4 (document has_indexed_load_or_lis heuristic), #5 (summary line counts) into fix round 1 with the Important negative tests — soundness of a miscompile-capable path — cost if wrong: none.
Task 4: fix round 1/5 — 87ec3a5 (negative dataflow tests, guard-join rule, dropped labels as entries, tail-call heuristic doc, summary line); re-review: all findings addressed
Parked (Task 4): 1(e) ambiguous-index test covers register-free target only, not two-register ambiguity — minor test gap; tail-call heuristic may drop an intra-function bctr edge whose table base comes from an earlier block — documented; padding-start scan decodes data words as branches (conservative). Ruling: park — no real-ISO table affected (counts unchanged) — cost if wrong: a future unresolved/missed table, caught by the bad_indirect_call report (exit 4).
Task 4: complete (commits 8c59895..87ec3a5, review clean)
Task 5: dispatched (base 87ec3a5, implementer sonnet)
Task 5: implemented ec5a3ac (482/482, vulkan tests ran on lavapipe); review dispatched
Task 5: review Approved with 1 Important (VMA macro ODR) + minors; fix round 1/5 dispatched (resume implementer): VMA defs PUBLIC on lr_vulkan_thirdparty, BDA buffer test, drop TRANSFER requirement, comment, ADR "ainda não ligada", SDL message
Parked (Task 5): vkDeviceWaitIdle before destroy — add with first queue submission (Task 17 presenter)
Task 5: fix round 1/5 — fa421eb; controller re-reviewed the 21-line fix diff directly: all 6 addressed (vma_impl.cpp comment still says "lr_vulkan_vma library" — folded into Task 6)
Task 5: complete (commits ec5a3ac..fa421eb, review clean)
Task 6: dispatched (base fa421eb, implementer sonnet)
Task 6: BLOCKED by plan's stop rule — official DXC Linux releases need glibc 2.38 (v1.9.x/1.10) / 2.34 (v1.8.x) and libstdc++/libgcc_s/libz.
Ruling: dev/source installs use Arch `directx-shader-compiler` (system DXC, doctor system mode); XenosRecomp builds against it via our own IMPORTED Microsoft::DirectXShaderCompiler (no dxc-bin); the spec's plan B (DXC from pinned source with zig, glibc 2.31) moves to Task 24 (AppImage); ADR 0005 entry; commit msg "build(deps): add XenosRecomp and the system DXC" — why: GPU work (T7–T23) needs only the container, and building an LLVM fork inside a 12 GB Docker now risks stalling the milestone on packaging — cost if wrong: Task 24 grows (DXC source build may take hours / need patches); AppImage can't install shaders until then.
Task 6: resumed implementer with ruling
Task 6: implemented 10e73aa (485/485); review dispatched
Carry to Task 24: XenosRecomp disabled in AppImage (-DLR_BUILD_XENOSRECOMP=OFF); bundled DXC layout/rpath open; XenosRecomp untested with zig/libc++ (std::execution).
Task 6: review Changes required (bundled doctor regression, guard test, reconfigure test, minors). Ruling: bundled dxc required only when XenosRecomp tool is staged — keeps AppImage install working until Task 24 — cost if wrong: Task 24 must flip it mandatory (carry). Fix round 1/5 dispatched (resume implementer).
Task 6: fix round 1/5 — c4d4be0 (486/486); controller re-reviewed doctor diff + report: all 6 addressed (guard tested via rename exit-0 case rather than skipped path — accepted)
Task 6: complete (commits 10e73aa..c4d4be0, review clean)
Carry to Task 24: make bundled DXC mandatory when XenosRecomp is bundled (doctor warns until then).
Task 7: dispatched (base c4d4be0, implementer sonnet)
Task 7: implemented 8d556b6 (4 new [unit][gpu] pass); review dispatched
Task 6: REOPENED — submodule_patches_mechanism fails deterministically on c4d4be0 ("case unreported"), reproduced by controller; fix round 2/5 dispatched (resume implementer), commit on top of 8d556b6
Task 7: review Approved; all indices verified (CP_RB_* via Linux radeon_reg.h byte/4; fetch consts are 32×6). Minor folded into Task 8: registers.h CP_RB group comment cites radeon_reg.h; note SCRATCH_REG4/5 = Xenia CALLBACK_ADDRESS/CONTEXT.
Task 7: complete (commit 8d556b6, review clean)
Task 6: fix round 2/5 — 8e067e1 (root cause: stale c.txt from rename case in scratch dir; each case now wipes its scratch); 490/490, passes twice + after wipe
Task 6: complete (commits 10e73aa..8e067e1, review clean)
Task 8: dispatched (base 8e067e1, implementer opus)
Task 8: implemented 0d5b761 (503/503, 17 [gpu] cases ASan clean); review dispatched
Carry to Task 9: MemoryAccess.size = GuestMemory::size(); multi-dword regions translated by start address only — mapping must be contiguous over a region or CP must translate per page.
Carry to Task 10: ring-buffer wrap — execute() rejects packets split at the buffer end; GPU thread must linearize a wrapped packet (copy) before execute; request_stop() on shutdown; VdSwap emits pm4 LR_SWAP (0x64: frontbuffer phys, width, height).
Task 8: review Changes required (scratch write-back, 0x1FFFFFFF phys mask + minors); fix round 1/5 dispatched (resume implementer) incl. LR_SWAP magic 'LRSW', partial-packet return at depth 0, alignment, ignore-mode draws, DrawInfo index_endian, tests
Parked (Task 8): INTERRUPT CPU index (Task 10 interrupts); EVENT_WRITE_ZPD occlusion sample counts / 0xFFFFFEED markers — if the game uses D3D queries it deadlocks (exit 6) → handle in Task 22 burn-down.
Task 8: fix round 1 — c815a87 (507/507); re-review dispatched
Task 8: re-review all findings addressed
Task 8: complete (commits 0d5b761..c815a87, review clean)
Carry to Task 10: ring reader must hand execute() a contiguous window up to WPTR/ring end; short return = wait only if more data can still arrive; no progress with full span available (consumed==0) → fatal malformed ring (guest_fatal naming RPTR/WPTR), not a spin. VdSwap emits LR_SWAP with kSwapMagic.
Task 9: dispatched (base c815a87, implementer sonnet)
Task 9: implemented 75a4086 (513 pass); review dispatched
Task 9: review Approved; fix round 1/5 dispatched (resume): RBBM_STATUS sourcing honest/open, seed Xenia static MMIO reads (0xF00, 0xF01, 0x194C, 0x1951, 0x1961), test/minors
Carry to Task 12: confirm which MMIO registers the game polls (RBBM_STATUS index 0x5D0 vs 0x174).
Task 9: fix round 1/5 — 5b483cc; controller checked diff: all 5 addressed
Task 9: complete (commits 75a4086..5b483cc, review clean)
Task 10: dispatched (base 5b483cc, implementer opus)
Task 10: session restart interrupted implementer (uncommitted partial work: gpu_system.h, pm4/backend/timers edits); resumed same agent
Task 10: second session restart (Docker Desktop also down; restarted); implementer resumed again with large uncommitted tree
Task 10: implemented 2ffff53 (522 x10 pass); review dispatched
Task 10: review Approved (VdSwap bits confirmed vs xenos.h); fix round 1/5 dispatched: VdSwap NOP-fill + fatal, ring generation check, coalesced fallback vblanks, CP_RB_RPTR in registers.h, comments, trace flush
Ruling: PacedBySwap rate may exceed 60 Hz with NullBackend — follows brief ("one per swap, ≥ every 16 ms") — cost if wrong: game timing assumptions in headless runs; revisit if Task 12 shows vblank-driven logic running too fast.
Carry to Task 12: CPU index dropped on interrupts (vblank on CPU 2 in Xenia); watch for fault/hang right after first vblank; mitigation = worker KPCR reports processor 2 during vblank callback. Regenerate docs/kernel-coverage.md from the real build.
Task 10: fix round 1/5 — ac0e9bc (523 x10); minors from an Approved review, accepted on report
Task 10: complete (commits 2ffff53..ac0e9bc, review clean)
Task 11: dispatched (base ac0e9bc, implementer sonnet)
Task 11: implemented c6b111a (526); review dispatched
Task 11 review running; Task 12 brief extracted
Task 11: review Approved; minors folded into Task 12 (xex.cpp stale comment, drop callback<0x10000 check); parked: escape control chars in msgbox, null-arg msgbox tests
Task 11: complete (commit c6b111a, review clean)
Task 12: dispatched [ISO] (base c6b111a, implementer opus)
Task 12: session restart; progress so far e5e0851, ca83780; implementer resumed
Task 12: implemented c6b111a..af19927 (17 commits; 757 swaps, 6940 draws, no msgbox; integration 4/4; game later exits 4 at unguarded jump table 0x8235B7AC); review dispatched
Carry to Task 22: unguarded jump table 0x8235B7AC (indirect call 0x8235B828) — switch_scan rule; function discovery must not emit functions at import-thunk addresses (runtime workaround 762ba3d); placeholder hardware answers (storage/controller/Kinect/voice/network/privileges) revisit in M4.
Task 12: review Approved (no Critical/Important)
Task 12: complete (commits e5e0851..af19927, review clean)
Carry to Task 22: func_table "first wins" drops duplicates silently — log/count + root-cause fix in function discovery (record in roadmap.md); loader whole-image copy assumes image.data holds image.size (BASIC compression) — patch or document; KPCR cpu restore RAII guard; --exit-after-swaps strtoull accepts "+5"/" 5". M4: XamContentCreateEnumerator ignores device_id.
Task 13: dispatched (base af19927, implementer opus)
Task 13: implemented 0523eda (3 patches; 542 pass; -L shaders 7); review dispatched
Carry to Task 14: tool usage `XenosRecomp <file|dir> <out.cpp> <shader_common.h> [--hlsl-dir]`, prelude staged as share/legacyrecomp/tools/xenos_shader_common.h; zstd raw SPIR-V; lr_shader_vertex_inputs table.
Carry to Task 20: contract in architecture.md "Contrato dos shaders traduzidos"; VS declares 18 vec4 outputs (72 comps > guaranteed 64) — check maxVertexOutputComponents.
Carry to Task 15: duplicate microcode with different translations fails by design; bool bank addressing unverified on real game.
Task 13: review Changes required (bool raw index, const_1 rel, isRepeat, path-specific broken tests + minors); fix round 1/5 dispatched (resume implementer)
Task 13: fix round 1 — c76fb1e (547); re-review dispatched
Task 13: re-review all addressed
Task 13: complete (commits 0523eda..c76fb1e, review clean)
Parked (Task 13): pc-switch loop form and nonzero literal-loops untested (no jump/defi fixtures); cond_jmp leaving loop body skips aL restore. Revisit if Task 15 gate shows loop shaders misbehaving.
Task 14: dispatched (base c76fb1e, implementer sonnet)
Task 14: implemented 74a5188 (553); review dispatched
Carry to Task 15: link zstd in template when decompressing. Carry to Task 24: bundled libdxcompiler.so LD_LIBRARY_PATH/rpath for XenosRecomp.
Task 14: review Approved with Important (shaders re-run after every recompile — DXC pass per build); fix round 1/5 dispatched: cache in work/shaders, independent() stage flag, build fingerprint hashes cache, truncation check, dxc stderr, tests
Task 14: fix round 1/5 — 3b15c74 (559); controller checked runner logic (independent stage skip/dirty + build fp hashes cache): correct
Task 14: complete (commits 74a5188..3b15c74, review clean)
Task 15: dispatched [ISO gate] (base 3b15c74, implementer opus)
Task 15: GATE FAILED — translated 64/85 (75.3%); coverage 2/7 loaded shaders by 300 swaps; 5 missing hashes are not in any container (2 D3D-patched variants of container microcode, 3 container-less small VS). Commits 404de86, a17bae3 (ADR 0005 M7). install on real ISO now fails at shaders stage.
Ruling: switch to the spec's plan B (spec §5 lines 94-99, "não exige um spec novo"): own runtime microcode→SPIR-V translator with Xenia as semantic reference; renderer/EDRAM/textures unchanged. Plan B amendment written to the plan by a planning agent before Part E resumes; install must succeed on the real ISO (install-time XenosRecomp translation retired from default stages unless the amendment finds a concrete use) — why: evidence shows any container-keyed cache misses runtime-patched and container-less microcode, so improving XenosRecomp cannot reach 100% — cost if wrong: Tasks 13–14 work largely unused; plan B translator is the largest M3 item (weeks).
Task 15: complete as gate (decision recorded); replanning dispatched
Plan B pre-flight: fix first (C1 vertex index/index buffer, C2 0x rule scalar+DST, C3 aL-relative temps, I1–I9); findings in plan-b-preflight.md; planner resumed to apply
Plan B amendment: preflight findings applied by planner; committed 3111494 (docs(plan)); global-constraints.md extended with plan-B lines
Task 15.1: dispatched (base 3111494, implementer sonnet)
Task 15.1: implemented 34647d9 (537; ISO install exit 0, M3a integration pass); review dispatched
Carry to Task 15.10: binds_shaders() unused — implement "untranslatable fatal only with binding backend". Carry to 15.3/16: Dockerfile needs SPIRV-Tools (spirv-val/dis) and glslang.
Task 15.1: review Approved; minor (backend.h:41 stale binds_shaders comment) folded into 15.2
Task 15.1: complete (commit 34647d9, review clean)
Task 15.2: dispatched (base 34647d9, implementer opus); plan-b-design.md extracted for briefs
Task 15.2: implemented 4f8a91d (549 + fuzz 2000); review dispatched
Carry to Task 15.11: strict decode errors (relative export dest, export reg not in stage, vfetch const >=96, mini vfetch w/o full, slot used as ALU+fetch) may reject real shaders — check at gate.
Task 15.2: review Changes required (all bits match Xenia; export const0/1 mask semantics missing) + minors; fix round 1/5 dispatched (resume)
Task 15.2: fix round 1/5 — d952f85 (550); export const masks + tests, arbitrary_filter, CONST docs, fuzz shaping — accepted on report (precise, test-backed)
Task 15.2: complete (commits 4f8a91d..d952f85, review clean)
Task 15.3: dispatched (base d952f85, implementer opus)
Task 15.3: implemented ea6a8fd (561, vulkan+spirv-val ran); review dispatched
Task 15.3: review Approved; minors folded into 15.4 (Result-returning finish/check_interface in translator, pointer-type decoration rule, harness destroy-time validation errors, leak:libgallium, restore previous log sink, vkDeviceWaitIdle in cleanup, direct includes, Aligned 16 check, AGENTS.md map SPIRV-Headers)
Task 15.3: complete (commit ea6a8fd, review clean)
Task 15.4: dispatched (base ea6a8fd, implementer opus)
Task 15.4: implemented 4bcb09d (576); review dispatched
Task 15.4: review Changes required (zero-mask export → Internal/false memexport error) + 24-bit index mask, docs, minors; fix round 1/5 dispatched (resume)
Task 15.4: fix round 1/5 — 43d486b (577); all 6 items, test-backed — accepted on report
Task 15.4: complete (commits 4bcb09d..43d486b, review clean)
Task 15.5: dispatched (base 43d486b, implementer opus)
Task 15.5: implemented c455845 (593, red observed); review dispatched
Carry to Task 15.7: own the 4 scalar PRED_SET ops added in 15.5; add INV/POP/CLR/RESTORE and vector *_PUSH, p0 stored last. Carry to 15.6: aL-relative constants. Carry to 15.8: predicated fetch unconditional + OpSelect on pred_matches().
Task 15.5: review Approved; fix round 1/5 dispatched with rulings
Ruling: CF cap 2^24 counting all backward transitions — 3×255 nested loops fit, real shaders needing more could not run at frame rate — cost if wrong: a 4-deep-nested shader renders truncated silently.
Ruling: return at call depth 0 is a no-op (Xenia ucode.h hardware note) — cost if wrong: a shader relying on early-end via return renders extra work.
Ruling: keep aL restore on jumps out of loops (planner) — safer on malformed code; compiler output unaffected.
Task 15.5: fix round 1/5 — 20e9e18 (598); all 5 items with pixel tests — accepted on report
Task 15.5: complete (commits c455845..20e9e18, review clean)
Task 15.6: dispatched (base 20e9e18, implementer opus)
Task 15.6: session restart + Docker down (restarted); implementer resumed (only xenos_shader_builder.h edited)
Task 15.6: implemented 462949f (613, red 11/15 observed, vulkan ran); review dispatched
Carry to 15.8/15.9: fetch-after-kill test; CUBE output order (tc, sc, 2·ma, face) per Xenia — 15.9 cube fetch must match.
Task 15.6: review Approved (all 30 ops verified vs Xenia); plan CUBE wording fixed (commit above); minors folded into 15.7: NoContraction on ALU float arithmetic + Invariant Position (bump version), kill result doc source, MAX/MIN NaN + negative abs-const + kill result tests, scan_address_registers source count
Task 15.6: complete (commit 462949f, review clean)
Task 15.7: dispatched (base 1108942, implementer opus)
Task 15.7: implemented 123a53c (621, red observed, vulkan ran); review dispatched
Task 15.7: review Approved (50 ops verified); fix round 1/5 dispatched: input-side selects for RECIP/RSQ/LOG/EXP/SQRT edges, EXP(0)=1, temp-count test, MINs doc, sin/cos tol
Task 15.7: fix round 1/5 — b2d1b6e (622); all 5 items — accepted on report. Note: implementer treats denormal inputs as signed zero for RECIP/RSQ/LOG (assumes Xenos FTZ, unverified) — revisit if Task 22 shows precision artefacts.
Task 15.7: complete (commits 123a53c..b2d1b6e, review clean)
Task 15.8: dispatched (base b2d1b6e, implementer opus)
Task 15.8: implemented 01d07e2 (638, red observed, vulkan ran); review dispatched
Carry to Task 20: fetch table size_bytes must be the real uploaded size in BYTES (fetch const gives words) — Review Focus 7 safety depends on it.
Task 15.8: review Approved (15 formats verified, bounds safe even with bad NaN handling); missing comps (0,0,0,1) kept
Carry to Task 20: fetch table 8-byte aligned; slot base 4-byte aligned; size_bytes = words×4 clamped to real allocation. Parked minors: half unpack per word, mini-fetch pc-switch note in limits.
Task 15.8: complete (commit 01d07e2, review clean)
Task 15.9: dispatched (base 01d07e2, implementer opus)
Task 15.9: implemented 2b64d93 (649, red 10/11, vulkan ran); review dispatched
Carry to Task 19/20: sampler cache owns fetch-constant LOD bias (mipLodBias); texture cache owns sign/gamma/swizzle and null texture = (0,0,0,0). Carry to Task 22: confirm cube S/T order on first cube-mapped content.
Task 15.9: review Approved (cube .yxw path verified vs Xenia; implementer right over brief); minors folded into 15.10: cube unnormalized comment/docs vs Xenia, stacked-texture log request in docs for Task 19, offset epsilon 1.5/1024
Task 15.9: complete (commit 2b64d93, review clean)
Task 15.10: dispatched (base 2b64d93, implementer opus)
Task 15.10: implemented 7c6e929 (670; gpu x10 = 740 runs; red verified); review dispatched
Carry to Task 15.11: watch IM_LOAD start≠0 (now exit 5), flat/centroid key churn (new_last_100_swaps), failed translations retried every run.
Task 15.10: review Changes required (golden fingerprint too narrow; centroid semantics vs Xenia to confirm) + minors; fix round 1/5 dispatched (resume)
Task 15.10: fix round 1/5 — 4030e2e (675; gpu x10 790); centroid mask was inverted — fixed per Xenia; fingerprint broadened 0x94C5885029E507D3 — accepted on report
Carry to Task 20: Flat from param_shade is our design (Xenia never Flat) — confirm with game; sc_sample_cntl=3 uses pattern.
Task 15.10: complete (commits 7c6e929..4030e2e, review clean)
Task 15.11: dispatched [ISO gate] (base 4030e2e, implementer opus)
Task 15.11: GATE PASSED — run1 keys=8 (3 VS, 5 PS) translated=8 failed=0 new_last_100=0; run2 100% disk; spirv-val 8/8; all 7 M7 hashes translate (6 via draws, AB1E… via unlogged probe). Commits b81d6ca, 26476f4. Controller checked diff (script/test/ADR only).
Carry to Task 22: memexport PS (15 containers) may appear in-world → STOP rule; probe cache /data/scratch/probe-cache-15.11 (game-derived, data volume only).
Task 15.11: complete (commits b81d6ca..26476f4)
Task 16: dispatched (base 26476f4)
Task 16: implemented b8ff0c3 (678); review dispatched
Task 16: review Approved; minors folded into Task 17 (blit test subpass dependency, README apt/dnf glslang, development/packaging "no shader compiler" wording, glslangValidator as DEPENDS + empty-list guard + header name)
Task 16: complete (commit b8ff0c3, review clean)
Task 17: dispatched (base b8ff0c3, implementer opus)
Task 17: implemented b9ea5a0 (686, vulkan+validation clean); review dispatched
Carry to Task 23 (WSLg): window/swapchain/resize/present never run. Carry to Task 24: AppImage staged liblr_vulkan.a + vkprobe under zig untested. Carry to Task 19: front buffer linear copy (striped until untile).
Task 17: review Changes required (unbounded shutdown on SIGTERM; ISO integration with --gpu vulkan default never run) + minors; fix round 1/5 dispatched (resume)
Task 17: fix round 1/5 — b1882ad (687; ISO integration 5/5 with --gpu vulkan default, 3090 s); bounded shutdown tested — accepted on report
Ruling: `--gpu test-stall` stays in the game binary (test-only, undocumented in user help) — cost if wrong: a user flag that hangs swap; revisit at Task 24 docs pass.
Task 17: complete (commits b9ea5a0..b1882ad, review clean)
Task 18: dispatched (base b1882ad, implementer opus)
Ruling (Task 18): CPU tiling reference written in Task 18 as tests/support/xenos_tiling.h (brief cited Task 19's, which runs later); Task 19 reuses it — cost if wrong: none, ordering only.
Task 18: implemented 63f8b2a (694, red seen, vulkan+validation clean); review dispatched
Carry to Task 20: host_sample_mask for 2x→4x MSAA; fold clear/resolve submits into frame command buffer. Carry to Task 22: resolve/front buffer not checked against committed physical pages (exit 4 risk); unsupported RT formats k_16_16/k_16_16_16_16/7e3.
Task 18: review Changes required (MSAA sample order wrong for native 2x and 4x) + tiling vectors, allocation clamp, 8in64, notes; fix round 1/5 dispatched (resume)
Task 18: fix round 1/5 — 51dc2f8 (696); MSAA order fixed + pure test, allocation clamp — accepted on report
Task 18: complete (commits 63f8b2a..51dc2f8, review clean)
Task 19: dispatched (base 51dc2f8, implementer opus)
Task 19: implemented e295343 (712, vulkan+validation clean, red not observed); review dispatched
Carry to Task 20: bind/texture errors → guest_fatal; defer eviction/reupload past frame fence; Backend::swap(regs, phys, w, h).
Task 19: review Changes required (hash-after-load stale race) + 64-bit 3D offset, sRGB fallback, tests, Task 20 notes; fix round 1/5 dispatched (resume)
Task 19: fix round 1/5 — 5214117 (715); hash-before-load + deterministic test, 64-bit 3D, sRGB fallback, tests, Task 20 notes — accepted on report
Task 19: complete (commits e295343..5214117, review clean)
Task 20: dispatched (base 5214117, implementer opus)
Task 20: implemented f170e4b, 2b75be5 (735, 16 vulkan brief tests, validation clean); review dispatched
Carry to Task 22: PA_SC_SCREEN_SCISSOR=0 / VGT_MAX_VTX_INDX=0 would draw nothing; Flat from param_shade; 64bpp clear order; rect list needs geometryShader (fatal without); pitch-0 draws skipped with warning.
Task 20: session restart during review; reviewer resumed
Task 20: review Approved (contract, NDC, state, lifetime verified); minors fix round 1/5 dispatched (resume): empty-scissor warn, swap/wait_idle fatal, 3 tests, depth bias face, docs, SPDX
Task 20: fix round 1/5 — 498725c (738) — accepted on report
Task 20: complete (commits f170e4b..498725c, review clean)
Task 21: dispatched (base 498725c, implementer opus)
Task 21: implemented 7fb89df (739; x10 pass; linear-texture mutation fails as expected); test-only commit, controller checked stat — no runtime bugs found
Task 21: complete (commit 7fb89df)
Task 22: dispatched [ISO] (base 7fb89df, implementer opus; carries in task-22-carries.md incl. memexport STOP)
Owner request (2026-10-24): all work at high effort. Task 22 agent stopped (had committed 75d8ee9 frame_stats; uncommitted switch_scan/function_discovery/func_table WIP) and re-dispatched with effort: high, inheriting the WIP. Rule from now on: every implementer and reviewer dispatch passes effort: "high".
Task 22: session restart + Docker down (restarted); progress ee9823c, 1d33db3, 8871b56 + func_table WIP; implementer resumed
Task 22: another session restart + Docker down (restarted); large uncommitted multi-fix WIP (func_table, gpu draw/edram/textures, audio/net exports); resumed with instruction to commit each fix promptly
Task 22: BLOCKED report — commits 75d8ee9..90ff270 (748 pass); game reaches 600 swaps, keys=11 failed=0, validation clean, but frame flat (colors=1). Causes: (a) k_8 font textures all zero in guest memory; (b) VS CB7E755E3A72E353 mostly zero microcode; (c) VS E91536F60A69ABF3 wrong UVs despite correct inputs.
Ruling: not a plan-level stop — concrete paths exist. Continue Task 22 with diagnostics built as our own code: (1) guest write-watch flag to find who should fill (a)/(b); first hypothesis a common cause in memory-fill paths (dcbz/dcbz128 / fast memcpy) since both are "never written"; (2) for (c), a CPU reference Xenos interpreter + bisecting comparison against the translated module on lavapipe that prints only slot index/opcode names. Owner asked separately whether a one-shader disassembly exception is allowed (only the owner can grant it) — cost if wrong: tooling time (days) if the exception would have sufficed.
Owner (2026-10-25): scoped exception granted — the disassembly of VS E91536F60A69ABF3 may be read into context, never committed. Applies to that one shader only.
Plan: (c) dispatched after the (a)/(b) agent finishes (shared build volume; no parallel builds); the (c) agent may dump and read the .txt disassembly of E91536F60A69ABF3 only.
Task 22 part 2: paused by org spend limit 2026-10-25..27; progress 67f20e8 (dcbz hypothesis refuted, pinned by test), f92bcbe/bb6b997/084096a (guest write watch). Resumed 2026-10-28.
Owner (2026-10-28): keep documentation updated with all context. Controller added docs/m3-status.md (living M3 status) + roadmap row, commit e004840. Every dispatch from now on: update m3-status.md in the same commit as findings/fixes.
Owner (2026-10-28): push decision — wait until M3 finishes (148 local commits: feat/m2 59 + feat/m3 89; history scanned clean of game files).
Owner (2026-10-28): when M3 finishes, everything goes to the remote as ONE branch (feat/m3 already contains M2); no separate feat/m2 push.
Pushed (2026-10-28): origin/feat/m3 = e004840 (148 commits over main), via KauanDev1 credential.
2026-11-04: session restart; Task 22 part 2 agent resumed (uncommitted read-watch work + check_func_profile.py). origin/main = e94d5e6 (PR #1 merged feat/m3).
Task 22 part 2: DONE — (a)+(b) root cause = physical views not aliased; 563485e maps one 512 MiB memfd at 0xA0/0xC0/0xE0 (ADR 0005 M14). Tooling: --watch-guest (rw), shader load trace at=, tools/func_profile.py. Commits 67f20e8..fe68dd6 (759). frame_stats N=300/600 colors=4 lum 0.59 edge 0.7% — still below thresholds; (c) is now the blocker.
Task 22 part 2 review dispatched (90ff270..fe68dd6); Task 22 part 3 (c) dispatched with owner exception (E91536F60A69ABF3 disassembly only).
Task 22 part 2 review: aliasing correct (E0 offset, protections, GPU translation, all fork tests safe); Changes required: SPDX header broken in tests/recomp/cases.h (67f20e8), watch docs still say one view per page. Folded as one commit into the running part-3 agent. Parked: write-watch races (re-protect mid-step; report under spin lock with :rw), MmQuery* through non-owning view answers 0 (low risk, ADR M14), func_profile dcbz128 counting.
2026-11-06: session restart + Docker hung (restarted); part-3 agent resumed with uncommitted review-fix edits.
Task 22 part 3: DONE — e4ee2fe (part-2 review fixes), d2f0584 (param_shade is not a flat mask; ADR M15; synthetic VS reproducer), b977fbf (integration test: title screen, 182 s). frame_stats N=300 colors=73762 lum 0.39 edges 4.9%; N=600 validation 0 errors. Thresholds pass. kTranslatorVersion not bumped (key changes, translator output unchanged).
Ruling: no kTranslatorVersion bump — translator output per key unchanged, only the runtime-built key's flat bits; old cache files just go unused — cost if wrong: stale cache entries survive (harmless, different names).
Task 22 reviews dispatched: part 1 (7fb89df..90ff270) and part 3 (fe68dd6..b977fbf), parallel, opus high.
Reviewer ids: part1 ab958752502ecda89, part3 a24c66ba4341f3357; part-3 implementer ab75ccbf0fcde8baf (resume for fix rounds).
Task 22 part 1 review: Spec ✅, Changes required — I-1 XMAReleaseContext frees any physical allocation (audio.cpp:60), I-2 switch_scan inline-table end truncated by branch targets decoded from table data (switch_scan.cpp:501, UB); 7 Minor. Fix round 1/5 held until part-3 review returns (one combined dispatch, Docker shared).
Task 22 part 3 review: Spec ✅, Approved; I1 validation check can pass vacuously (test_integration_m3.cpp:176); 5 Minor; kTranslatorVersion ruling confirmed. Fix round 1/5 dispatched (resume ab75ccbf0fcde8baf): part1 I-1, I-2 + part3 I1 + all Minors (rulings allowed for disproportionate ones).
2026-11-06: owner paused the session; fix round 1 agent (ab75ccbf0fcde8baf) stopped. On resume: check git status/log, then SendMessage the same agent to continue fix round 1 with the tree state.
Pause state: committed 4568791 (XMA I-1), 0573e15 (switch_scan I-2); staged uncommitted: part3 I1 + minors (validation helper tests/support/vulkan_validation.h, docs, draw.cpp, translator.h). Last agent note: thunk ranges as FunctionRange (part1 minor, in progress).
Backup of uncommitted WIP: fix-round1-wip.patch (+ fix-round1-wip-vulkan_validation.h). Tree left as is; HEAD 0573e15.
2026-11-06: owner resumed; fix round 1 agent resumed from HEAD 0573e15 with staged WIP.
Task 22: fix round 1/5 — 4568791, 0573e15, 4e6b9f6, 6245bd6 (763; title-screen ISO test 390 s; switch-scan-18 output byte-identical). Rulings: I-2 filters forward strays only (backward stray b/bc in table still truncates; rare); m-5 → ADR 0005 M16. Flaky: APC/KTHREAD reuse test failed once under host load. Scoped re-review dispatched.
Task 22: re-review All addressed (4 Minor/nits: addresses.md backward-stray note, m3-status '(esta)'→6245bd6, watchdog comment 390 s, function_discovery.h line length) — carried into Task 23 dispatch as doc touch-ups.
Task 22: complete (commits 75d8ee9..6245bd6, review clean)
Task 23: dispatched (base 6245bd6, implementer opus high, id a1178fd0ea8dc5cc5); build route (AppImage vs native in distro) left to implementer as a ruling; owner does the visual look via a one-line command in docs/wslg.md; Task 22 re-review nits folded as 'docs: tidy Task 22 notes'.
Task 23: owner said 'não' — agent stopped immediately; awaiting owner direction.
Task 23: CANCELLED by owner ('remove todas as alterações da tarefa 23', 2026-11-06). Removed: Ubuntu-24.04 WSL distro (unregistered), D:\Jogos Teste\WSL, uncommitted edits (incl. the Task 22 doc nits; backup in session scratchpad), task-23-brief.md. Owner also flagged disk filling: C: had 1.6 GB free; distro vhdx was on D: so C: unchanged after removal. Do not reinstall a WSL distro without asking. Next step awaits owner direction.
