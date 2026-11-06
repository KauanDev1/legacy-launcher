# Task 20: items carried from earlier tasks (controller)

These are binding on Task 20 in addition to the brief. Each one was raised by an earlier task's review.

## Vertex fetch table (15.8 review)
- `size_bytes` is the size actually uploaded, **in bytes**. The fetch constant gives the size in words, so multiply by 4 and clamp to the real allocation, using `MemoryAccess::allocation_end`. The Review Focus 7 safety guarantee depends on this.
- The table is 8-byte aligned. Each slot's base address in the upload ring is 4-byte aligned, because the loads carry `Aligned 4`.

## Translator and keys (15.10)
- `VkBackend::binds_shaders()` returns true.
- Flat interpolation comes from `param_shade`. This is our own design, since Xenia never marks an input Flat, so confirm it against the game in Task 22.
- `sc_sample_cntl` = 3 uses the sampling pattern.
- Flat inputs take the provoking vertex: honour `PA_SU_SC_MODE_CNTL.provoking_vtx_last` through `VK_EXT_provoking_vertex` when the device has it, or document the limitation (15.4).

## EDRAM (Task 18)
- When drawing MSAA, use `host_sample_mask` (Xenos → Vulkan sample order, and the 2x-on-4x-host fallback).
- Fold clear and resolve submits into the frame's command buffer instead of submitting and waiting per operation.
- Re-confirm the order of the two halves of the 64bpp clear word (`RB_COLOR_CLEAR` / `_LO`) once 64bpp is rendered.
- Once work is batched, `Edram::bind`'s LRU `destroy_target` needs deferred destruction. Callers must not keep a `RenderTarget*` across a `bind`.

## Textures and samplers (Task 19; see the architecture.md "Para a Tarefa 20" note)
- Texture eviction, destruction and re-upload into an existing image must be deferred past the fence of the frame that used the image. Alternatively, upload into a new image and retire the old one later.
- Heap index reuse while a command buffer is pending: either `UPDATE_UNUSED_WHILE_PENDING` (feature `descriptorBindingUpdateUnusedWhilePending`) plus deferred recycling of freed indices, or deferred descriptor writes.
- Re-bind texture and sampler heap indices on every draw, because a freed index can be handed to another texture.
- Errors from `TextureCache`/`SamplerCache`/`Edram::bind` become `guest_fatal` (exit 5), naming the cause.
- Per-instruction sampler overrides (tfetch mag/min/mip/aniso when not `UseFetchConst`) need an override argument on `SamplerCache::heap_index`. Implement it, or document it as a limit.
- Clear the linear filter flags on formats that lack `SAMPLED_IMAGE_FILTER_LINEAR` (e.g. R32*_SFLOAT), to avoid VUIDs.
- Optional: re-hash a texture when its fetch-constant registers were rewritten since the last check.
- `Backend::swap(regs, phys, w, h)` already takes the register file.

## Device
- `maxVertexOutputComponents` and `maxFragmentInputComponents` ≥ 64 are already checked (Task 17).
