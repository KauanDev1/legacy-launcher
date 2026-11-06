# Task 22: extra carried items (controller)

These come in addition to the brief's list. Handle each one only when it shows up in the run, with its own test.

- **Memexport STOP rule (from 15.11).** If a loaded shader needs memexport (eA/eM, export registers 32..37), stop and report BLOCKED with the hash and the stage. 15 PS containers in the XEX use it. It is a planning decision, not a fix-loop item.
- **Cube S/T order (15.9).** The first cube-mapped content confirms or refutes the `.yxw` feeding.
- **Resolve and front-buffer writes into partly committed allocations (18).** These are bounded by the allocation, but not by committed pages. Watch for exit 4.
- **Unsupported render-target formats** k_16_16, k_16_16_16_16 and 7e3 (18). If the game binds one:
  - 7e3 → `R16G16B16A16_SFLOAT`;
  - k_16_16 → `R16G16_SNORM` ×32.
- **Draw state (20):**
  - An empty scissor or viewport now warns: read the warning if frames are blank.
  - `VGT_MAX_VTX_INDX` = 0 clamps every index to 0.
  - Flat from `param_shade` is our design: confirm it with the game.
  - The 64bpp clear-word order is unconfirmed.
  - A rect list needs `geometryShader`.
  - A reset index in a list is fatal; Xenia ignores reset in lists. Switch to "ignore" if this ever fires.
  - Pitch-0 draws are skipped with a warning.
- **Textures (19):**
  - Signed formats are a named fatal: implement them if the game needs them.
  - The first stacked texture is logged.
  - The YCbCr border is mapped to opaque black.
  - Denormal-as-zero in RECIP/RSQ/LOG is assumed from Xenos FTZ (15.7).
- **Translator (15.x):** decoder strictness rejections, IM_LOAD start≠0, flat/centroid key churn (`new_last_100_swaps`) and failed translations retried every run.
- **Performance:** the game now runs `--gpu vulkan` on lavapipe headless, with submit-and-wait at every resolve and swap. 600 swaps may be slow. Measure it and record the time.
- **`--gpu test-stall`** stays (ruling).
- **Python in the container.** `tools/frame_stats.py` needs `python3`. The host has no Python. Check whether the Arch dev image has python3; if not, add `python` to tools/dev/Dockerfile and docs/development.md.
