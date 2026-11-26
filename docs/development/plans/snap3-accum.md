# snap3-accum: native RenderWorld without the accumulator's resolve functions (phase 3)

Lead: `accum`, branch `snap3/accum` (base `org_integration` 5809202ae), stage `s3-accum`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 41-43, 57, 109-121 of
[render-thread-decisions.md](render-thread-decisions.md); the call-order inventory in `snap3-engstate.md` section 2.

| Item | State |
|---|---|
| 1. StartAccumulating and the post-resolve replaced by CS's points in order | done |
| 2. What RenderPostProcess reads of RenderWorld | given from CS (the shadow state the resolve functions leave); RenderPostProcess itself is next (below) |
| 3. The driver's other direct calls (SetRenderTarget, the runners' setters and SetCameraData, SetDirtyStates, DrawSun's SetCameraData) | done |
| 4. The water capture's engine globals, the ImageSpace copy step's rows | done |

## 1. The driver (`RenderGraph/Engine/Native/RenderWorld.cpp`)

`Native::RenderWorld` now calls no engine function. In the engine's order (AE 106437 -> 106583, RenderWorld's ImageSpace calls,
106438 -> 106584, SE 99790 / 99938 / 99939):

- **Pre-resolve.** Shift window A: `Deferred::WorldStart` (StartDeferred, DCLF BeforeOpaquePass). Outside a window: `Sky::MainBatch`
  (the sky root's draws with their cloud layers). Window B: `Deferred::WorldOpaqueEnd` (AfterOpaquePass, terrain blending,
  EndDeferred with DeferredPasses, the depth copy); the water mask block when `Water::Indirect::MaskBatchWanted` (the stencil clear
  of kMAIN through the skeleton, `Water::NativeMaskBatch`, TAA's form +0x2c = 1); with flag 0x40 `ImageSpace::NativeReflections`.
- **RenderWorld's ImageSpace calls:** RenderSSAO, the kMAIN copy into kSNOW_SWAP as a native `ISCopySubRegionCS` step (rows: the region
  at kMAIN's adopted shape, groups per 8x8), RenderSSAO's composite.
- **Post-resolve**, one window: the under-water blended draw (`Forward::NativeBlendedDraw(false)`), the kMAIN copy
  (`Skeleton::CopyMain`, SetRenderTarget's force-copy restore), the water blend test (`WorldPass::WaterBlendRuns`, kept as
  `WorldPass::RawWater`), `Water::NativeSurfaceBatch` (geometry group 10 when it is a batch, then the surface), `ImageSpace::NativeWaterBlend`,
  View Atmosphere's composite (Deferred's PostWater hook), the clears of kTEMPORAL_AA_MASK and kMAIN_ONLY_ALPHA when ImageSpaceManager
  +0x168's effect is active (`Skeleton::ClearColour`), the blended draw (`NativeBlendedDraw(true)`).
- **Left for the code after RenderWorld** (`LeaveResolveState`): RT1 and RT2 unbound, slots 3-4 in restore mode, the viewport's max
  depth 1, the alpha reference 128/255 and the dirty bits 0x1C7 (0x200 when the reference changed).

Each hook (`SkyBatch`, `WaterBatch`, `Accumulator_BlendedDraw`, `Main_RenderWorld_Start`, `Main_RenderWorld_BlendedDecals`) became a
point function with an optional engine call; the hooks pass it, the driver does not. Guest and the reflection faces keep the hooks.
No longer done in native: SetCameraData (x3), the accumulator's setup, the depth shift functions (the camera is not touched), the
first-person node hide, the accumulator's +0x5C byte, the depth mode write, SetRenderTarget x4-6, the per-frame block writes, ApplyState.

### Observe (`s3-accum-a-observe-*`, river x4, interior)

Scaffolding (removed): the shadow state, `BSGraphics::State`, the main accumulator and the world camera diffed at the driver's entry
and exit; every skeleton clear and copy inside the driver with its call site; parity at each reader.

- Clears inside RenderWorld, every render: `kMAIN` depth stencil at pre+0x3CE (the mask block), the TAA mask and kMAIN_ONLY_ALPHA at
  post+0x4BB (ApplyState), the water targets once (the blend's clear request). No clear at BeforeOpaquePass's flush.
- Copies: the depth copy (CS's, DeferredPasses) and the kMAIN copy at post+0x9D (SetRenderTarget mode 5), every render.
- Net state change: the shadow state as `LeaveResolveState` writes it (4 signatures over 1,000 renders each, all covered); none in
  `BSGraphics::State`; the accumulator's camera nulled (not reproduced, see open items); the current camera (414580) and current
  accumulator (414600) set to the world's.
- Readers: water capture (planes, eye, render size) inside the windows equal at every read; at a cell change the far distance
  differed twice (the render half's update reaches the block one snapshot later); water blend 0-2 differences (first frame, the
  same far distance); CaptureBindings targets 0 of 2,000 differ; the pipeline frame at Prepass (current accumulator main, eye 0)
  0 of 1,500; the sun eye 0 of 900; the effect camera inside RenderWorld always the shifted planes; the water blend test 1,000 of 1,000.

### The defect found on the switch

The first switched build drew the river black. A bisect (engine post-resolve on: fixed; perFrame writes, ApplyState, the shift, the
kMAIN copy, the effects call, the engine batch: not) and per-batch logs led to `WaterDraw.cpp`'s `SurfaceVariant`, the water
surface range's target choice, which read the engine global the post-resolve wrote (528167 / 415112). Native reads `RawWater` now.

## 2. RenderPostProcess

It sets RT0-3 and the depth target to none, flushes (`ApplyState(false)`), writes the current camera, copies the first-person camera's
planes into ImageSpaceManager +0x28's effect, and calls VolumetricLighting, the lens flares and the two ImageSpaceManager passes (CS's
detours). The only state of RenderWorld it reads is what its flush applies: the shadow state `LeaveResolveState` leaves. It is the
natural next item: the same driver shape (CS calls the detoured passes in order, no flush).

## 3. Direct calls

The runners (`RunReflections`, `RunSsao`, `RunWaterBlend`) and `DrawSun` write back no camera data or targets inside native's
RenderWorld (guest unchanged). `BeforeOpaquePass` does not flush; `CaptureBindings` takes the deferred targets from `Deferred::kTargets`
and the main depth. `StartDeferred` and `EndDeferred` write no shadow state there and bind no engine per-frame buffer.

## 4. Water capture and copy rows

`Port::CaptureFrame` in native: planes `WorldPass::CameraPlanes(InShiftWindow())` (the block gains the scene graph's near and far
distances and the shift scale, SE 304860 / AE 254903), eye the frame camera's, render size kMAIN's adopted shape, raw water
`WorldPass::RawWater()`. `EffectFrame`'s camera row, `GeometryPort`'s eye, `LatchAccumulator` and `BuildSun`'s eye likewise.

## Remaining direct engine render calls from RenderWorld's scope

- `RenderSnowSSS` (SE 100537, AE 107267), only with bEnableImprovedSnow and its helper active; no test scene reaches it.
- None else. Engine state still written: the render mode (411475 = 7), the reflections' camera (415299), TAA's form, 414748/9.
- Still read at record time: the TESWaterSystem underwater state and height, ImageSpaceManager +0x168 and +0x1f0, the bound PS views
  in `CaptureBindings` (CS features' textures).

## Detector

Engine render calls per native world frame (before: snap3-resolve's / snap3-engstate's runs; after: this branch): quick 56-57 -> 33,
cadence-drop 55 -> 37, drop-cells 46 -> 27, repeat 42 -> 22, shadows-blend 43 -> 24, interior 38 -> 20, river 41 -> 22, post-effects
42 -> 23, lens 42 -> 24, screen blood 47 -> 27, tour 39 -> 20, upscalers 24, resize stress (TAA) 24. The pre/post-resolve sites
count 0 in world frames (only loading frames enter the engine's resolve functions, through the cube faces). These were engine sites
inside functions CS called directly; decision 57's hidden direct calls from RenderWorld's scope drop to RenderSnowSSS alone.
Live engine reads per world frame: quick 774 (778 before), river 653 (693 before), interior 556.

## Runs (stage `s3-accum`, Dev-Fast, detector on from `-c-nvk-quick`)

| Config | Label | Result |
|---|---|---|
| native-river, observe | `s3-accum-a-observe-river2..4` | passed; the observe numbers above |
| native-interior, observe | `-a-observe-interior` | passed |
| native-river (first switch) | `-b-river`, `-b-river2..4` | hang on the snow copy's step lookup (fixed), then passed with black water |
| native-river, bisects | `-b-bisect1/4/8/16/32/64/128/512/1024`, `-b-wd0/1`, `-b-exit0/1` | passed; isolated the surface variant |
| native-river (fix) | `-b-river5` | passed; water normal |
| native-vulkan+quick | `-c-nvk-quick` | passed (33 render calls) |
| guest+quick | `-c-guest-quick` | passed |
| native-d3d12-quick | `-c-d3d12-quick` | passed |
| native-vulkan+cadence-drop, -drop-cells, -repeat | `-d-drop`, `-d-drop-cells`, `-d-repeat` | passed (37, 27, 22) |
| native-vulkan+shadows-blend | `-d-shadows-blend` | passed (24) |
| native-interior, native-river | `-d-interior`, `-d-river` | passed (20, 22) |
| native-vulkan+post-effects, lens | `-d-post-effects`, `-d-lens` | passed (23, 24) |
| native-upscalers | `-d-upscalers` | passed (24) |
| native-vulkan+screen-blood | `-d-blood` | passed (27), blood drawn |
| native-vulkan+window-resize-stress-taa | `-d-wrs-taa` | passed (12 resizes) |
| native-tour | `-e-tour` | passed (20) |
| cadence.py, cadence-still | `-e-cad-*` | all passed; exterior repeat 16.6 / drop 13.4 against a floor of 2.2 (the wind and pose sizes of snap3-resolve's 16.6 / 14.8), interior within noise |

SE was not run (new addresses are SE/AE pairs; the shift scale's SE id 304860 and the post-resolve's offsets were checked in the SE
disassembly).

## Decided by the agent, to confirm

- AC-a. The shadow state the resolve functions leave is written by CS at the driver's end until the first-person view and RenderPostProcess
  are CS's (an engine-state write, not a call).
- AC-b. The main accumulator's camera is not nulled and the first-person node is not hidden (no reader in native; both are engine-object
  writes from the render thread).
- AC-c. TAA's form +0x2c is still written by the driver (the post chain reads it); the render mode and reflections camera globals too.
- AC-d. RenderSnowSSS stays an engine call (untestable here).
- AC-e. The far distance of the window comes from the snapshot (one snapshot late when the render half changes it at a cell change).

## Proposed decision-log entries

- **Native's RenderWorld calls no resolve function** (refines 109, 116, 121): CS makes the pre-resolve's and post-resolve's points in
  their order with their clears and kMAIN copy through the skeleton, and writes the shadow state they leave for the engine code after
  it. Every reader of what StartAccumulating and the post-resolve set (current camera and accumulator, the accumulator's eye, the
  shifted planes, the raw-water flag, the deferred targets) reads CS's frame. Remaining direct call: RenderSnowSSS (improved snow only).
- **The depth shift window is CS state** (`WorldPass::ShiftWindow`): the camera's planes inside a window are computed from the
  RenderWorld block; the engine camera is no longer modified.
- **RenderPostProcess is next**: it reads nothing of RenderWorld but the shadow state its flush applies.

## Open items

- RenderPostProcess (section 2), then the first-person view: once both are CS's, `LeaveResolveState` goes.
- RenderSnowSSS as a native step (needs a scene with improved snow).
- The main accumulator's camera field stays set after RenderWorld (the engine nulled it); no reader found in native.
