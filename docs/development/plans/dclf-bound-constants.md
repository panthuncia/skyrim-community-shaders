# DCLF constants as bound constant buffers: plan

Status: 2026-11-23, implemented (uncommitted).

## Results

Native D3D12 on the GTX 1070 Ti (Dev-Fast), frozen Whiterun scene. In-game timers:

| Build | GPU frame | Main opaque | Z-prepass |
| --- | --- | --- | --- |
| Before | 53.3 ms | 22.2 ms | 5.5 ms |
| Steps 1, 3, 4 with heap-indexed blocks (`CS_DCLF_HEAP_FRAME_BLOCKS=1`) | 50.6 ms | 18.9 ms | 5.4 ms |
| All steps | 42.3 ms | 13.0 ms | 4.6 ms |

- **Replay of a new capture:** main opaque 11.8 ms against 23.5 ms before; the tree bucket 3.1 ms against 8.5 ms.
- **Checks:** native D3D12, native Vulkan and guest quick checks pass. The native D3D12 tour passed 2 of 3 runs; the third lost the
  device 18 frames after load, which matches the known intermittent hang.
- **Implementation notes:**
  - DXIL reflection (ORGModuleServices `DxilReflection.cpp`) now records register spaces.
  - DCLF's binding check accepts the bound-block spaces and DXIL's `VS_PerFrame` alias at `register(b12, space1)`.
  - A second block at one register takes space 10, with a second table over the same slots.

### Follow-ups, 2026-11-23: frame 42.3 → 36.0 ms

Native D3D12 on the GTX 1070 Ti, frozen-scene perf runs (Dev-Fast; `perf-taa`, 1500 settle frames, 600-frame graph window). D3D11 in
the same runs: 32.9 ms.

| Step | Frame |
| --- | --- |
| All steps above | 42.3 ms |
| Z-prepass texture indices (below) | 41.9 ms |
| Frame-values pools never reuse a bound copy, so the copy queue no longer waits for graphics (pascal-d3d12-stability.md) | 41.0 ms |
| DCLF CPU pipeline for exteriors on legacy-binding GPUs (`CpuExteriorsByDefault`) | 38.6 ms |
| Graph compute programs read CS's shared constants as bound push tables (below) | 37.5 ms |
| The same for graph raster programs | 36.0 ms |

**Bound push tables for graph programs.**
- ORGModuleServices' program builds (compute and raster) define `ORG_BOUND_PUSH_TABLES` on DXIL.
- Common/NamespacedCBuffer.hlsli then declares the shared blocks (FrameBuffer, SharedData, FeatureData, Permutation) at
  `register(b<push word>, space11)` instead of reading them from the heap.
- The program's layout gets a BasicRHI PushIndex table for each such constant buffer the stage declares, found by DXIL reflection
  (`program_build::BoundPushTableRanges`).
- The SHARED entry is first in every push block (checked: 47 lists), so the handles are push words 0-3.
- A/B, four alternating pairs (`ORG_BOUND_PUSH_TABLES=0`): 38.5-38.9 ms off, 37.5-37.7 ms on (compute programs only). Raster
  programs too: 35.9-36.2 ms. Checks: native D3D12 quick and tour, native Vulkan and guest quick pass; shots match.

**Where the rest of the gap is.** Feature removal, one feature off per run, extra cost on D3D12 over D3D11's:
- Physical Sky +6.7 ms (8.1 against 1.4);
- Skylighting +2.9 (4.7 against 1.8);
- SSGI +2.8 (4.2 against 1.4);
- Screen Space Shadows +1.7;
- Cloud Shadows +1.7;
- Water Effects +1.1;
- Volumetric Shadows +0.2.

The extras overlap: each feature also adds to DCLF's colour stage (register pressure), so they sum past the gap. The D3D12 runs were
on the GPU pipeline, before the last two steps. Per-list graph timers bleed into each other; the ORG pass timestamps were not read
back in these runs.

**Serialized pass times (2026-11-23, later).** A `frame-timing` timestamp trace (`CS_NVPERF_TEMPLATE=frame-timing`, kept
recordings with a full barrier at each pass end, so passes do not bleed) on the frozen scene, native D3D12, against D3D11's event
timers in the same scene. Report: `C:\Modding\CS-AB\traces\d3d12-frame-timing-1123.txt`.
- The feature passes themselves are level with D3D11 (D3D12 p05 to median, then D3D11): SSGI GI 1.13 / 1.25 ms, Skylighting probe
  update 0.47 / 0.69, SSS raymarch 0.30 / 0.24, volumetric shadows downsample 0.27 / 0.37.
- So the removal deltas for Skylighting, SSGI, SSS and Cloud Shadows come mostly from their code in DCLF's Lighting pixel stage
  (register pressure), not from their passes.
- **Physical Sky is not like for like.** Native runs Physical Sky's aerial-perspective shadow (`cs.physical-sky.ap-shadow`, 3.2-4.4
  ms) and its AP composite pass. D3D11 runs neither: with the shared Atmosphere and Fog volume prepared it skips `AccumShadow`
  (`PhysicalSky.cpp`, `!viewAtmosphere.IsPrepared()`) and runs ViewAtmosphere's inject, integrate and composite instead (0.38 +
  0.13 + 0.35 ms). In graph modes the shared volume is not ported (view-atmosphere-port.md 3.6), so `IsPrepared()` is always false
  there.
- **After the ViewAtmosphere port** (shared volume, composite with Physical Sky, reflection medium; view-atmosphere-port.md
  items 3-5, 2026-11-23, uncommitted): the AP shadow no longer runs, and the new passes are level with D3D11's (inject 0.31 ms,
  integrate 0.13, composite 0.31, reflection SH and medium 0.15 once a frame, where D3D11 builds it twice). With
  `PostWaterComposite` on, DCLF main opaque drops from 15.6 to 12.1 ms in the serialized trace
  (`traces\d3d12-frame-timing-vatm-1123.txt`). Frozen-scene frame: native D3D12 33.9 ms (pre-main 5.44 -> 2.79), D3D11 32.7 ms.
  Native D3D12, native Vulkan and guest quick runs pass.
- Many passes show a second mode about 1.5 ms slower in 5-10% of frames (p95 around 1.5-2 ms). Compare medians or p05.

**Open items:**
- **The slow state.** Some perf runs settle into a state about 30% slower across every stage (45-51 ms against 36). The GPU clock is
  the same; its power is lower and the frame rate drops from 28 to 22 fps, so the GPU is starved. It came in batches: 4 of 6 runs in
  one batch, none in 11 others, and 3 of 5 runs in the last removal sweep. The cause is unknown, so judge a single run with care.
- **Native Vulkan.** One sample, 54.7 ms, with main opaque at 20.3 ms against 11.8 on D3D12. Vulkan has neither the CPU-pipeline
  default (`CpuExteriorsByDefault` sees bindless descriptors there) nor the bound push tables (SPIR-V builds are left out). A later
  frozen-scene frame: Vulkan 49.2 ms, with per-stage times matching D3D12 with its bound constants off (56.9 ms), not on (33.9 ms).
  The Vulkan half is below ("Vulkan: bound uniform buffers").
- **Next lever for graph passes.** Their per-pass constants (LATCH and EXTERN_LATCH entries) are still raw loads from the latch,
  and their textures are bindless. Physical Sky's aerial-perspective shadow (`ShadowAccum.cs.hlsl`: 30 steps per pixel, four
  samples each) is about 3 ms on D3D12, against 1.4 ms for all of Physical Sky on D3D11. Bound latch constants need a CBV per latch
  region in ORG's LatchBlock.

### Follow-up: Z-prepass texture indices (2026-11-23)

The alpha-tested Z-prepass pixel stage waited on a chain of five dependent loads (draw sequence, rows, material row) before
its one texture sample, and the late depth test of a discarding stage exposes that latency.

- **Replay, tree depth bucket:**
  - texture sampling mode makes no difference (mip 0, mip 4 or isotropic: 1.30-1.44 ms);
  - the chain alone without the sample costs 0.61 ms, and with the sample 1.43 ms;
  - with the texture and sampler indices passed as a flat attribute, it drops to 0.74-0.78 ms. The depth image is identical.
- **Implementation:** a Lighting build `DCLF_DEPTH_TEX`. The pulled depth pixel stage reads its diffuse texture and sampler
  indices from `TEXCOORD14`, and an eighth stage build (pulled depth texture vertex) writes them. Alpha-tested Z-prepass
  pipelines pair the two. Stage key bit 48 marks the build.
- **Result:** Z-prepass replay pass median 4.51 → 2.27 ms. In game, frozen scene: Z-prepass 4.6 → 4.0 ms, frame 42.3 → 41.9 ms.
- **What did not help:** the same indices on the colour stage (no change), and zeroing the colour vertex stage's unused outputs
  for depth (3%).

### Vulkan: bound uniform buffers (2026-11-23, uncommitted; built and run, results below)

On the GTX 1070 Ti BasicRHI runs the descriptor-sets model, which lowered every PushIndex range like a heap CBV, and heap CBVs are
read-only storage buffers there. So nothing was bound, and the colour stage paid the same registers as D3D12 with its tables off.

**Design.**
- BasicRHI, descriptor-sets model only: each layout's PushIndex ranges become a push descriptor set (`VK_KHR_push_descriptor`) at
  set 2, one uniform buffer binding per constant buffer, in range order, each visible to its range's stages.
  - The layout's own `VkPipelineLayout` is the universal sets 0 and 1, the universal push range, and that set. It stays compatible
    with the universal layout, so heap binds and push constants are unchanged. Layouts with the same bindings share one.
  - The lowering re-decorates those constant buffers to (set 2, binding) and keeps them uniform buffers (`BindingSource::BoundUniform`).
  - The command list pushes the set before the next draw, dispatch or ExecuteIndirect after BindLayout, a PushConstants write over a
    pushed slot, SetDescriptorHeaps, ExecuteCommands or generated commands. It reads binding k's buffer, offset and size from the
    bound heap's slot (pushed slot + recordOffset + i), the record each CBV write keeps, under the heap's lock. A push of unchanged
    slots is skipped. Descriptors are read at record time, as D3D12's static descriptors require.
  - A layout past the limits (uniform buffers per stage, push descriptors, set limits, the universal set's update-after-bind
    resources) keeps the old lowering, with a warning.
  - Not on adopted devices (guest), the descriptor-buffer model or the heap model, which maps PushIndex natively.
- ORGModuleServices: SPIR-V program builds take `ORG_BOUND_PUSH_TABLES` when `rhi::vulkan::BoundUniformBuffers` says the device binds
  them (CS's `ProgramEnvironment::spirvBoundPushTables`). `BoundPushTableRanges` reflects either format (`ShaderReflection::Parse`).
  Program builds shift no register class, so `register(b<w>, space11)` is set 11, binding w on both.
- DCLF needs no change: the Vulkan builds keep their registers (FrameSource is D3D12-only), and `MainLayoutRanges` already makes each
  pass-wide register a PushIndex range at set 0 with the FrameSource ordinals. That is 8 VS and 8 PS blocks, 16 push descriptors.
  Shadow layouts have no PushIndex range.

**Switch.** `BASICRHI_VK_BOUND_UBO=0` kept the heap storage buffers (read once at device creation); ORG's shared blocks followed
it. Deleted 2026-11-25 (perf2-native.md), with `ORG_BOUND_PUSH_TABLES`: only devices without push descriptors read them from the heap.

**Log lines.**
- Device: `descriptor-sets model: bound uniform buffers on: ... up to N per stage and M per layout`, or the reason it is off.
- Per layout: `Vulkan CreatePipelineLayout: bound uniform buffers: set 2, 16 push descriptors (8 in one stage): ...` for DCLF's
  main and Z-prepass layouts, and 1-4 for ORG programs.
- Once, if a pushed slot holds no CBV: `Vulkan bound uniform buffers: ... hold no constant buffer`.

**What to test.**
- `BasicRHISpirvLoweringTests` (three new cases).
- Native Vulkan on the 1070 Ti with `BASICRHI_VK_VALIDATE_LOWERING=1` and the validation layer: quick check and shots, then the
  frozen-scene frame A/B against `BASICRHI_VK_BOUND_UBO=0`. Expect main opaque to move toward D3D12's.
- Register counts of the tree pixel stage (`spv-pipeline-stats` on a `BASICRHI_VK_DUMP_LOWERED` dump).
- The RX 6600 and RTX native Vulkan and guest quick checks must log no bound uniform buffers and render as before.

## Why

On the GTX 1070 Ti (Pascal, native D3D12) DCLF's main opaque pass costs about 22 ms, against about 10.6 ms for the same draws in
D3D11. The cause is register pressure in the pulled pixel stages.

- **Register counts** (NVIDIA's compiler, through its Vulkan pipeline statistics; `spv-pipeline-stats`):
  - the tree pixel stage (`0C100011`) uses 255 registers pulled, against 95 for DXC's unpulled build and 96 for the D3D11 build
    through DXVK;
  - at 255 registers Pascal keeps about 8 warps per SM instead of about 20, so latency goes unhidden.
- **Why constants cost registers:** on NVIDIA a value read from a bound constant buffer is an instruction operand and costs no
  register. Heap-indexed constant buffers, raw device-ref loads and device-address loads put every value in a register first.
  DCLF reads its pass-wide frame blocks through heap-indexed CBVs and its rows through raw loads.
- **Same shading, measured on both sides:** the tree shading after the alpha test costs about 2 ms in D3D11 and 7.8 ms in DCLF.

Measured with the capture-and-replay harness (`dclf-replay.md`) on a frozen Whiterun frame. Every row below renders an image
byte-identical to the baseline, except constant rows (a bound, not a fix):

| Variant | Tree bucket | Main opaque | Z-prepass |
| --- | --- | --- | --- |
| Baseline | 8.5 ms | 23.5 ms | 4.0 ms |
| Frame blocks as a bound CBV table | 5.1 ms | 13.0 ms | 3.4 ms |
| Bound blocks, rows constant (upper bound, not a fix) | 2.0-3.3 ms | | |
| Bound blocks, rows as per-draw root CBVs, CBVs first | 4.8 ms | | |

Per-draw root CBVs are not viable on Pascal:
- When `ExecuteIndirect` writes a root CBV per draw, that alone costs about 1.9 ms on the tree bucket.
- Placed after the root constants, they hang the GPU when a pass draws several buckets, even with a small root signature.

## Findings that shape the steps

1. **The frame push words are mostly dead.**
   - `kFramePushWords` is 34. Words 4-33 are the pass-wide blocks' addresses, unread since those blocks moved to descriptors:
     the shaders declare only words 0-3 (`DCLFFrameRecord.hlsli`), and `MainLayoutRanges` sources everything from the record or
     the draw words.
   - The replay with 4 frame words renders the main pass identically.
   - Removing them takes D3D12 from 58 to 28 root DWORDs and Vulkan from 232 to 112 push bytes. That fits the Vulkan minimum of
     128, so nothing spills on any device.
2. **BasicRHI's Vulkan spill is a device-address load** (PhysicalStorageBuffer), the register-expensive path. DCLF does not spill
   on NVIDIA or AMD (256-byte push range) once step 1 is in, but other clients can.
3. **The non-pass-wide frame blocks are raw loads too.** On Vulkan, VS b7, b8, b11 and PS b7, b8, b11 come from record addresses
   (`IndirectAddress`). On D3D12 they come through `DCLFFrameBlockSpan` loads.
4. **The 15 frame-block CBVs are contiguous in the heap** (`FrameBlockViews`), so one descriptor table covers them.

## Steps

1. **Drop the dead frame push words.**
   - `kFramePushWords` becomes 4: the record address and its ref.
   - Delete the address writes in `FramePushWords` and the other places that fill them.
   - Check: every place that pushes `kFramePushWords`, the D3D12 root signature size, and the Vulkan push bytes (log line
     "main layout").
2. **Bound frame-block table on D3D12.**
   - BasicRHI:
     - a layout range for constant buffers bound through a descriptor table (D3D12: a root descriptor-table parameter placed
       after the root constants; a table base set per pass);
     - on Vulkan, the same range maps to a real uniform-buffer binding where the model allows it (sets model: a uniform buffer
       binding beside the per-type ones; descriptor buffer and heap models: their uniform-buffer path);
     - layout creation logs the root or push cost.
   - CS:
     - FrameSource declares the pass-wide blocks as `register(b<ordinal>, space9)`;
     - the main and Z-prepass layouts take the table over `FrameBlockViews`' range, set once per pass;
     - a diagnostic switch (`CS_DCLF_HEAP_FRAME_BLOCKS=1`) restored heap-indexed blocks for A/B (removed 2026-11-25).
   - Default on, every vendor.
3. **Non-pass-wide frame blocks as descriptors.**
   - Give b7, b8 and b11 (both stages) uniform-buffer views like the pass-wide ones, and read them through the same table (or
     record indices where they are per view).
   - D3D12 `DCLFFrameBlockSpan` reads go away for them.
4. **Row loads.**
   - Rows stay heap reads (per-draw root CBVs are out, see above).
   - Read material, pipeline and shadow rows as a heap `StructuredBuffer<float4>` (the benchmark's fastest heap form): one
     structured view per row backing, its slot in the record's `RecordBacking`.
   - Use register counts (`spv-pipeline-stats`, `tools/shader-compare`) and the replay to cut how many row values stay live in
     the hot stages, for example lazy member reads instead of eager statics.

### Step 4 findings (replay, tree bucket, frame blocks bound: 5.05 ms)

| Variant | ms | Image |
| --- | --- | --- |
| Rows as `StructuredBuffer<uint4>` (twin views) | 6.23 | identical |
| Material rows constant (bound) | 3.46 | differs |
| Pipeline rows constant (bound) | 2.18 | differs |
| Vanilla point-light arrays constant | 5.04 | differs |
| Runtime permutation descriptors as literals (the captured values) | **2.48** | identical |

- Structured rows lose in the real shader, so that part of the step is dropped.
- The pipeline row's cost is almost all the permutation descriptors (`Permutation::VertexShaderDescriptor`, `PixelShaderDescriptor`).
  - They are CS's runtime bits: `UploadedPixelDescriptor` strips the bits the shader was compiled for, and keeps runtime ones
    such as DefShadow and ShadowDir.
  - Read from the row, every permutation branch stays in the shader and keeps its registers.
  - As literals, DXC folds the branches away.
- Specialising pipelines on those bits needs more pipelines. In the capture, 30 main-opaque pipelines draw 48 (key, runtime bits)
  combinations; 11 pipelines have more than one, up to 6.

Each step is measured with the replay (per-bucket times, `--check` image hashes) and confirmed in game with the frozen scene
(`set timescale to 0`, `fw 81a`) on native D3D12. Native Vulkan and guest must keep passing their quick checks.

## Reusable pieces this adds

- **BasicRHI:**
  - the bound constant block range;
  - root and push cost logging;
  - a spill into a dynamic uniform buffer instead of a device address (follow-up);
  - the capture tables (`rhi::dx12` capture API);
  - `spv-lower --sets [--ub]`, and `spv-pipeline-stats` on devices without mutable descriptors (`SPV_STATS_DEVICE`).
- **CS:**
  - the replay (`pascal-fetch-bench replay`, `dclf_d3d12_stage.py`, `dclf_capture_check.py`);
  - a rules section in this doc's follow-up shader guide:
    - data every pixel reads lives in bound constant buffers;
    - device refs are for per-draw data read once;
    - keep the number of live per-draw values small;
    - measure with the replay and register counts, not in-game per-pass timers.

## Verification on other GPUs

Captures are adapter-independent (DXIL, resource files, heap slots). Before flipping the default for release, replay the same
capture on the RTX 3090 Ti (Ampere) and the RX 6600 (RDNA2). The expectation:
- Ampere: a smaller gain, since the uniform datapath softens register pressure;
- AMD: neutral, since a root table and a heap index both become a scalar descriptor load.

**Vulkan result (2026-11-23).** GTX 1070 Ti, frozen scene, Dev-Fast, native Vulkan with the DCLF CPU pipeline (legacy binding now
includes Vulkan's descriptor-sets model, `CpuExteriorsByDefault`): bound uniform buffers off (`BASICRHI_VK_BOUND_UBO=0`) 50.7 ms,
main opaque 19.4; on 35.0 ms, main opaque 10.3 (D3D12 33.9 / 12.0, D3D11 32.7). Before both changes native Vulkan was 68.4 ms on
the GPU pipeline. Native Vulkan quick and tour, native D3D12 quick and guest quick pass; shots match.
