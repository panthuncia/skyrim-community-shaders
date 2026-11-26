# Pascal on Vulkan: plan

Status: planned 2026-11-22. Native D3D12 already runs on Pascal (see "D3D12, done" below). This plan covers native Vulkan and
guest mode (CS inside the DXVK fork) on GTX 10-series cards.

## The device

Measured on a GTX 1070 Ti, driver 582.78 (vulkaninfo and a D3D12 CheckFeatureSupport dump).

Vulkan 1.4.312:

- Has `VK_EXT_descriptor_buffer` (image and sampler descriptors 4 B, uniform buffer 8 B, storage and texel buffers 16 B),
  descriptor indexing with update-after-bind (1,048,576 per stage for sampled images, storage images, storage buffers and
  samplers), robustness2 with null descriptors, BDA, timeline semaphores, sync2, dynamic rendering, maintenance5-8,
  draw-indirect-count, every subgroup operation, int64 and int64 atomics.
- Lacks `VK_EXT_mutable_descriptor_type`, `VK_EXT_descriptor_heap`, `VK_EXT_device_generated_commands` (only the NV one),
  shaderFloat16, mesh shaders and ray query.
- Uniform buffers: 15 per stage and 180 per set, with or without update-after-bind. Update-after-bind uniform buffers are not
  supported at all.
- No resizable BAR: 214 MB of device-local memory is host-visible.

## What blocks Vulkan today

1. BasicRHI has two binding models: the descriptor heap (`VK_EXT_descriptor_heap`) and descriptor buffers with one mutable
   binding (`VkEvaluateBindingModels`, rhi_vulkan.cpp ~15649 and ~15680). Pascal has neither, so the model is None: the device
   is made, and the first pipeline layout fails.
2. Even with mutable descriptors, the descriptor-buffer model puts uniform buffers in the heap and caps the heap by the
   uniform-buffer limits (rhi_vulkan.cpp ~15711). 15 per stage fails that check.
3. Guest mode: DXVK stays on legacy descriptor sets on Pascal (`enableDescriptorBuffer = m_hasMeshShader`,
   dxvk_device_info.cpp ~667, because descriptor buffers are slow there), and `negotiateBindingModel` gives the client nothing.
   The graph then disables itself, which guest mode turns into a fatal error (RenderGraphRuntime.cpp ~1702).
4. DCLF needs `indirectBindings`, which is true only with a binding model (DrawPipelines.cpp ~142). Without it,
   `DrawcallLimitFix.cpp` ~238 calls `report_and_fail`.

Nothing else is a blocker. Shaders never pass `-enable-16bit-types`, use no mesh shaders, ray queries or 64-bit atomics, and
no groupshared block comes near 48 KB.

## Question 1: a separate path, or the default?

A separate path, chosen only when the device lacks mutable descriptors and the descriptor heap. An environment override
forces it on any GPU for testing.

It costs no shader variants. BasicRHI lowers SPIR-V when it creates each pipeline (`VkCreateBufferModelShaderModule`, called at
rhi_vulkan.cpp ~10836, ~11101 and ~11373), from the same module the heap model takes as is. DXC's output, the ORG and CS shader
stores and their cache keys do not change. Only the driver's pipeline cache sees different code, so it gains entries; the
binding model should join `VkPipelineCacheIdentity` so a model change starts a fresh cache.

It should not be the default because per-type arrays must each cover the whole slot range: a slot index can name any type.
That is about 56 B per slot on NVIDIA (4+4+16+16+16) and about 112 B on AMD, against 16 B and 32 B with one mutable binding,
and uniform buffers leave the heap. RTX and RDNA gain nothing from it.

The abstraction is two layers:

- One lowering target, "per type". Set 0 gets one binding per kind: 0 sampled image, 1 storage image, 2 uniform texel buffer,
  3 storage texel buffer, 4 storage buffer (5 acceleration structure later). Set 1 keeps the samplers. Push constants and the
  binding plan (`VkBuildBindingPlan`) stay as they are. `HeapAlias` already keys aliases by set and binding, so picking the
  binding by kind fits without restructuring.
- Two backings behind that same layout. Only these differ: set-layout flags, the pipeline create flag, how a descriptor is
  written, and how the heap is bound.
  - Update-after-bind descriptor sets: one `VkDescriptorSet` per shader-visible heap, written with `vkUpdateDescriptorSets`,
    bound with `vkCmdBindDescriptorSets`. This is the one Pascal needs first: it matches DXVK's legacy sets in guest mode,
    and it avoids Pascal's descriptor-buffer slowdown natively.
  - Per-type descriptor buffers: `vkGetDescriptorEXT` at `bindingOffset[type] + slot * size[type]`. Lower priority. It suits
    native Vulkan on other GPUs without mutable descriptors.

`Classify` (rhi_vulkan_spirv_lowering.cpp ~686) reports `Buffer<T>` as a sampled image and `RWBuffer<T>` as a storage image.
The mutable binding hides that; the per-type target must split on `Dim == Buffer` and send them to the texel-buffer bindings.

Writes go only into the slot's own type array. Each heap keeps one byte per slot with its type. When a slot changes type, the
old array's entry gets a null descriptor (robustness2), so a stale read finds null rather than a destroyed view.

Capacity on Pascal: sampled images and uniform texel buffers share one per-stage limit, as do storage images and storage
texel buffers. Two choices fit in 1,048,576:

- 524,288 slots with full texel arrays. `DescriptorHeapManager` adapts to the reported capacity, so nothing else changes.
- 1,000,000 slots with texel arrays of 48,576, so typed-buffer views must sit low. Heap-indexed `Buffer<T>` is barely used.

Start with the first.

## Question 2: keeping constant buffer access

Register-bound (root) cbuffers never use uniform-buffer descriptors today. They become push data or `PhysicalStorageBuffer`
loads through device addresses (`RewritePhysical`, rhi_vulkan_spirv_lowering.cpp ~1455; binding plan rhi_vulkan.cpp ~2977).
DCLF's draw constants come through `IndirectAddress`/`PushAddress`, the same path. So the hottest constants already read like
storage buffers on every GPU.

Only heap-indexed `ConstantBuffer` uses uniform-buffer descriptors. The four `ORG_SHARED` handles (FrameBuffer, SharedData,
FeatureData, Permutation; `OrgResources.hlsli` ~38) appear in about 97 shaders, plus three `ORG_CB` entries in the lens
shaders. A shader has 3-4 such buffers, at most about 6. Each is indexed by a push-data handle, and none is non-uniform.

Phase 0 lowers every heap-indexed `ConstantBuffer` to a read-only storage buffer:

- Change the storage class from Uniform to StorageBuffer, retype the access chains and loads (`RetypeChain`/`FollowPointer`
  already handle storage-class changes for the physical path), and add `NonWritable`.
- Keep `Block`, `Offset`, `ArrayStride`, `MatrixStride` and `RowMajor`. The dx-layout offsets are legal storage-buffer
  layouts under scalar block layout, which both buffer models already require.
- The write side exists: `VkCreateBufferDescriptorSlot` writes CBVs as storage buffers when `!descriptorBuffer.uniformBuffers`
  (rhi_vulkan.cpp ~4299).
- Make `evaluation.uniformBuffers` false when the uniform-buffer limits are below the heap capacity, and always false on the
  per-type target. That also stops those limits from capping the mutable heap elsewhere.

Expected cost on NVIDIA: the buffers are read once into statics at shader entry, so the extra loads are well under 1-2% of
shader time. The real risk is register pressure, since constant-bank operands can be rematerialised for free and storage
buffer values cannot. Divergent indexing into a constant buffer gets faster.

A uniform-buffer window is possible later: keep up to 12 uniform-buffer bindings (Pascal allows 15 per stage) for heap CBVs
whose index is a plain push-data load, and feed them per draw with `vkCmdPushDescriptorSetKHR` on a third set. It has one
catch: push descriptors capture the descriptor at record time, while D3D12 heap semantics and reused ORG recordings allow a
slot to change after recording. Do it only if profiling after phase 0 shows a cost, and only for slots shown to be stable.

Phase 0 is measurable on the RTX first: force the storage-buffer lowering with an override and A/B shots and pass timings.

## Question 3: a GPU-driven path on Pascal

DCLF already has one that needs no device-generated commands: the plain buckets. Each pipeline bucket is one
`vkCmdDrawIndexedIndirectCount`.

- Each draw finds its `DrawSequence` through `SV_InstanceID`, which DXC's SPIR-V already offsets by firstInstance
  (`DCLFObjects.hlsli` ~82-105). Rows, textures and constant addresses are read by the shader.
- Vertices are pulled, and one index pool serves all geometry (`IndexPool.h`).
- AMD and Intel use it today, and NVIDIA prefers it whenever every set index has a plain pipeline. The native D3D12 run on
  the 1070 Ti already uses it ("colour draws with plain indirect draws (the device has no pipeline sets)").

On Pascal, `deviceGenerated` is false (no pipeline sets) and `gpuDriven` is true (counted draws), once `indirectBindings` is
true. So what Pascal needs for the GPU path is the binding model above, plus:

- `VkBindlessModel` (rhi_vulkan.cpp ~1571) returns true for the new model, so `indirectBindings` is set.
- The `IndirectIndex`, `IndirectAddress` and `PushAddress` ranges keep working: the binding plan is shared by every model.
  Only the final descriptor access moves to the per-type binding.
- `DrawcallLimitFix.cpp` ~238 should log and stay off instead of `report_and_fail` when DCLF is unsupported outside graph mode.

What it gives up without device-generated commands, as on AMD:

- Keys whose plain colour pipeline fails to build stay with the engine (`BuildPulledColour`, MainPipelines.cpp ~557).
- Decal and forward ordering across state classes changes, since device-generated commands split ranges per class.

A `VK_NV_device_generated_commands` backend would recover those two. Its shader groups are fixed when the pipeline is made,
so every set version would rebuild a group pipeline. Its preprocessing also uses the recording command buffer's own state.
That is about 2-4 weeks on a legacy driver branch with unknown performance. Skip it unless bring-up shows many keys left to
the engine or visible ordering faults.

Pascal performance risks: counted draws per bucket are front-end bound, and empty buckets cost about 8-9 µs per shadow view on
Ampere (`dclf-async-compute.md`). Pascal's async compute is weak, so compare native's default (compute queue) with guest's (folded onto graphics, `CS_ORG_QUEUE_TOPOLOGY=hardware` to take it).

## Phases

| Phase | Work | Where | Size |
| --- | --- | --- | --- |
| 0 | Heap CBVs as storage buffers, behind the uniform-buffer limit check; lowering tests; RTX A/B with the override | rhi_vulkan_spirv_lowering, rhi_vulkan ~10346 and ~15711, tests/SpirvLoweringTests | ~250 lines, 2-3 days |
| 1 | Per-type lowering target: binding by kind, `Dim == Buffer` split, tests | rhi_vulkan_spirv_lowering | ~300 lines, 2-3 days |
| 2a | Update-after-bind set backing: model enum, evaluation with the shared limits, init, heap backing, writes with the type byte and null fill, bind, pipeline flags, `VkBindlessModel` | rhi_vulkan.h ~609, rhi_vulkan.cpp ~15678, ~15858, ~12694, ~4295, ~12956, ~2394, ~9270 | ~800 lines, 1-1.5 weeks |
| 2b | Native bring-up on the 1070 Ti: allow the model in NativeDevice.cpp ~319, DCLF on the plain path, the DCLF graceful-off fix | CS | 2-4 days |
| 3 | Guest: a Sets binding model in the interop header and DXVK, separate the client's model from DXVK's own, add the storage-image, storage-buffer and texel-buffer UAB features DXVK does not request, accept it in RenderGraphRuntime.cpp ~1702 | rhi_interop_vulkan.h ~104, dxvk_interop.h ~59, dxvk_device_info.cpp ~262 and ~852, ~1199 | ~1 week |
| 4 | Pascal performance: storage-buffer CBV cost, bucket bubbles, async compute on or off | | ~1 week |
| 5 (optional) | Per-type descriptor-buffer backing; uniform-buffer window; NV device-generated commands | | as measured |

Phases 0 and 1 change nothing on current GPUs unless forced. Each phase can be its own BasicRHI and CS change.

## Validation

- Offline: BasicRHISpirvLoweringTests with fixtures for heap CBVs (static, `ORG_CB` arrays, dynamically indexed arrays in a
  cbuffer, row-major matrices) and typed buffers. `BASICRHI_VK_VALIDATE_LOWERING=1` runs every lowered module through
  spirv-val in game; `BASICRHI_VK_DUMP_LOWERED=<dir>` keeps them.
- On the RTX, before Pascal: force the storage-buffer lowering, then each backing, with the override. A/B shots and per-pass
  timings against the mutable model. `native-vulkan+quick`, `native-vulkan+dclf-toggles`, and a validation run
  (`CS_ORG_NATIVE_VALIDATION=1`).
- On the 1070 Ti: the binding-model table (`VkLogBindingModels`) and DCLF's "plain indirect draws" line; native-vulkan quick,
  tour, river and interior; the lens CBUFFER passes, HZB and RunGrass (non-uniform indexing) and the buffer view refs. Then
  `guest-quick` once phase 3 lands, with DXVK's negotiation log saying the client got Sets. Compare frame rates against native
  D3D12 on the same card.

## D3D12, done

Native D3D12 needed two fixes, made 2026-11-22:

- Resource heap tier 1: BasicRHI's allocator had the D3D12MA tier-1 pools disabled. It now uses 12 default pools when the
  device reports tier 1, and ORG's alias pools, frame alias arena, frame placement heaps and per-frame pools pass their
  memory class as heap DENY flags (`alias::AliasClassHeapFlags`).
- No GPU upload heap: engine static vertex and index buffers are now device-local on every GPU, filled through the graph's
  upload service (EngineBuffers.cpp `MakeStaticBacking`). The next graph submission copies them behind a full barrier before
  its own work. ORG's `UploadInstance` stages a buffer upload posted from another thread straight into a batch the producers
  share, so the loader thread does the one memcpy and the render thread only records the copy. The dynamic ring stays a mapped
  ring, in GPU upload memory where the device has it, else upload memory.

Frame rate on the 1070 Ti (Dev-Fast, perf-taa, uncapped): native D3D12 69 ms, against 38 ms for plain D3D11. With the static
geometry in upload memory it was 81 ms. DCLF's main opaque pass (29 ms) is most of what is left.

Texture uploads go through the same service, for textures no queue has used yet:

- `StagedUploadBatch` takes texture entries. Its copies take each texture from Undefined to CopyDest, copy, and leave it in
  Common.
- Before a ticket's admission, under the sequencer, `Runtime::SeedFreshTextureUploads` gives each such texture its ledger
  state, Common on the graphics queue, so the ticket's entry barriers start from there. A later use on another queue is
  released on graphics behind the copies and waits for the release. A texture the ledger holds already is a fatal error.
- Work outside the graphs (`ExternalWork::Submit`, the native context) first submits the uploads waiting on the render
  thread (`IUploadService::TakeStagedUploadsForSubmission`), so a buffer or texture made with contents is filled before
  the context's next operation as well as before the graphs' next submission.
- Moved onto it: `GraphResources` texture uploads (the 3D LUT, the terrain heightmap; `MakeUploadPass` and its copy pass
  are gone), native tracked textures made with contents, and native engine textures (`EngineTextures` makes an ORG texture
  under a native handle instead of a BasicRHI image with an immediate upload and a CPU wait).

Still outside the service, because they are raw BasicRHI resources rather than ORG ones: native host-made fallback textures
(made before the graph starts, 1D, or formats ORG cannot make), exempt textures, `Native::WrapTexture` callers with
contents (streaming windows, the icon loader), native buffers made with contents (kept in upload memory), and
`UpdateSubresource` emulation (one committed upload buffer per call).

Pascal reports SM 6.8, binding tier 3, enhanced barriers and ID3D12Device14, so nothing else changed. Its shader-visible
heap limits are exactly 1,000,000 views and 2048 samplers with static samplers, which CS already asks for.

## Vulkan, phases 0 to 3 done

Made 2026-11-22, uncommitted in BasicRHI, DXVK and CS:

- Lowering (phases 0 and 1): `LoweringTarget::perType` puts each kind of heap resource on its own binding (`KindOf` splits
  images on `Dim == Buffer` and the Sampled operand), and `cbvAsStorageBuffer` turns heap constant buffers into read-only
  storage buffers in place, keeping their block layout. With both off the output is byte-for-byte the old one. 34 lowering
  cases pass, among them a new `cbv_heap` fixture.
- Descriptor-sets model (phase 2a): `VulkanBindingModel::DescriptorSets`, `BindingModel_DescriptorSets`,
  `BASICRHI_VK_BINDING=sets`. One update-after-bind set per shader-visible heap, bindings 0-5 by kind (the CBV kind maps to the
  storage buffer binding), samplers in set 1. Capacity is the largest power of two the shared per-stage limits hold: 524,288
  on the 1070 Ti, 491,520 after the buffer view refs. Selection order is heap, buffer, sets, so GPUs with mutable descriptors
  keep the buffer model. The descriptor-buffer model now leaves uniform buffers out when their limits cannot hold the heap,
  and lowers CBVs as storage buffers then.
- Native (phase 2b): NativeDevice allows both models. DCLF runs on the plain path in graph mode.
- Guest (phase 3): `DxvkInteropBindingModelSets` in DXVK. While DXVK runs on legacy sets, a client that accepts Sets on a
  device with the update-after-bind features gets them, and the sets features are enabled at its request. CS asks for the
  storage-image, storage-buffer and texel update-after-bind features and non-uniform indexing, accepts Sets in its
  negotiation, and adopts DXVK's device on either model.
- XeSS: its Vulkan library builds descriptor set layouts with the mutable type, and Streamline reports it supported on Pascal.
  Streamline.cpp now turns it off on Vulkan devices without mutable descriptors (native and guest); FSR takes its place.

Results on the 1070 Ti (Dev-Fast):

- native-quick, native-tour (Whiterun, Dragonsreach), guest-quick: pass, shots match native D3D12 on the same card.
- Validation (native-quick, `org.validation=true`): after the XeSS change, only 20 `VUID-vkCmdDraw-None-09600` messages are
  left, two per engine render target at startup: a command list expects TRANSFER_DST where the image is in
  COLOR_ATTACHMENT. The RTX run had the same message on the back buffer. Real descriptor sets let the layer check
  descriptor layouts, which it cannot do under descriptor buffers. Open.
- Streamed windows sharpen more slowly than on D3D12: the shot 60 frames after world-ready samples low mips, the one 1200
  frames later is sharp. With `CS_DCLF_TEX_STREAM=0` the first shot is sharp too.
- perf-taa uncapped: native Vulkan 77 ms, native D3D12 69 ms, plain D3D11 38 ms.

Not exercised on the GPU yet: texel buffers, storage images through a kind change on a reused slot, acceleration-structure
SRVs. Each shader-visible heap allocates the full 524,288 per binding.
