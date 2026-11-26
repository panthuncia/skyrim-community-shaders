# snap3-rworld: guest screen blood, Main::RenderWorld driven by CS (phase 3)

Lead: `rworld`, branch `snap3/rworld` (base `org_integration` e2dff8585), stage `s3-rworld`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 33-48, 66-70, 87-103 of
[render-thread-decisions.md](render-thread-decisions.md); starting point `snap3-world.md` (its RenderWorld inventory).

| Item | State | Commit |
|---|---|---|
| 1. Guest screen blood | done: guest takes native's route | 4bb4c0ee3 |
| screen-blood config fix (Health Low tutorial) | done | c3cfd7053 |
| 2a. Blur camera test kept by the native post chain (observe, then switch) | done | a52f4cd04 |
| 2b. CS drives Main::RenderWorld in native; the engine's function is not entered | done, with engine calls left inside (below) | 9a97e837c |
| 2c. The pre-resolve's and post-resolve's CS points as CS functions | not done: list below | |

## 1. Guest screen blood

Guest drew no blood: every batch the hits made was flares, and the latch refused them because the flare's t3 (the adapted
luminance) is a published graph texture. Guest now takes native's route (as snap5-bugs did for screenshots, decision 91): the
snapshot capture, which guest already runs before DrawFrame on the same thread, fills `BlockId::ScreenBlood` (the light flare setup
on the game thread), and Main::Draw's RenderScreenSplatter call draws from ScreenSplatter's root at the EngineDraws anchor, the same
anchor the latched runs used, so guest's ordering does not change. The flare reads its luminance as a published engine target in
both modes (`PublishedInputs` of kHDR_DOWNSAMPLE1-13). RenderWorld's splatter cull and submit do nothing in guest too.
`BloodSplatterCapture.cpp` (the batch latch, its DrawTriShape taker and SetupGeometry watch) is deleted; no graph mode uses it, and
D3D11 mode never installed the feature. The record-time renderer read for the flare's t3 view is gone (it was unused).

Proof: `s3-rworld-a-guest-blood` passed, `the first screen blood run drawn by the graph`, red flares and splatters in both shots
(before: `s3-world-bloodguest1`, no blood).

The screen-blood config raised health 30 frames after world-ready; in two native runs a guard's hits opened the Health Low tutorial
first, which paused the game, so the bandit never hit (`s3-rworld-a-nvk-blood`, `-blood2`: passed but no blood). Health is now raised at
world-ready and the run expects `cs.blood-splatter` on the graph (`expect.graph_frames`). The config is in this branch; until it
merges, runs pass its path: `native-vulkan+<worktree>/tools/cs-test/configs/screen-blood.toml`.

## 2. Main::RenderWorld in native

### What RenderWorld does (AE 107142 0x1414cdaa0, SE 100424 0x1412e3e70)

1. Render mode global = 7 (411475 / 524994), the reflections' saved camera (415299 / 528353) = the world camera.
2. `Sun::UpdateOcclusionQueries` (26355 / 25801): reads the query ring (GetQueryData answered by CS's readback) into the sun glare.
3. Hides a scene node (415031 / 528086, flags bit 0) until the end; accumulator +0x5C = 1; TLS +0x768 = 0x41; depth mode 1.
4. `StartAccumulating(world camera, main accumulator, p0 << 4 | 0x41)` (106437 / 99790): SetCameraData, then the accumulator's
   pre-resolve (vfunc 0x150 = 106583 / 99938) with its CS points.
5. SetRenderTarget slots 0-3 (and 4-5 with improved snow) to none.
6. RenderSSAO (helper, 1, 1) with 415071 = 0; the snow pass (100537 / 107267) or the kMAIN copy into 0x71 (107219, 107221, 107217);
   415071 = 1, 415072 = the reflections' activity; RenderSSAO (helper, 0x71, 1).
7. `FinishAccumulatingPostResolveDepth` (106438 / 99791): the post-resolve (vfunc 0x158 = 106584 / 99939) with its CS points.
8. AE 106324: copies 0x301f8 bytes onto its own stack and returns: no effect.
9. TLS restored, +0x5C = 0, `ClearItemsList` (106597 / 99952), `RenderFirstPersonView_End(false)` (106610 / 99964).
10. The light flare setup (106979 / 100269), the screen splatter cull and SubmitAccumulator.
11. The full-screen blur's camera test (inlined in AE; SE calls it at 0x1412e4e60), gated by 414598 / 527646.
12. `Sun::SubmitOcclusionQuery(1, accumulator, camera)` (26353 / 25799) -> CS's SunOcclusionQuery detour; 414748/9 = 0; node
    flag restored.

### 2a. The blur camera test (`NativePost.inl` `StepBlurCamera`)

Only the native post chain's `ReadB` (and the engine's full-screen blur, which does not run in native) reads what step 11 writes.
Native now steps the test itself once per render from the snapshot's main camera (position and rotation diagonal, relative change
over 0.01), gated by `WorldSettings::blurCameraGate` (captured with the ImageSpace block). Guest keeps reading the engine's globals.
The gate is off by default (no writer in the binary: a static setting), so a normal run never exercises it.

Parity (scaffolding removed): `s3-rworld-b-blurpar3`, native-vulkan+cadence-drop-cells with the gate forced on: 300 renders, 16
engine moves (cocs included), 2 differ, both on the render the gate was forced (the engine started one render later). With the gate
as shipped (`-blurpar2`): 300 compared, 0 differ.

### 2b. The driver (`RenderGraph/Engine/Native/RenderWorld.{h,cpp}`)

Deferred's Main::RenderWorld thunk (Main::Draw's call) calls `Native::RenderWorld` instead of the engine's function when native is
active. It repeats steps 1-12 in order with these changes:

| Step | In the driver |
|---|---|
| 1, 3 (globals, node, +0x5C, depth mode) | written as the engine does (engine state the pre-resolve and post-resolve still read) |
| 3, TLS +0x768 | not written: decided by the agent, to confirm (the engine's memory-context tag; no render effect found) |
| 2, 12 (sun queries) | engine functions, called by CS |
| 4, 7 (pre/post-resolve) | engine functions, called by CS; their CS points unchanged (2c) |
| 5 | the engine's SetRenderTarget, called by CS |
| 6 | `ImageSpace::RenderWorldImageSpace`: both RenderSSAO calls go to `RenderSSAO::Run` with the composite flag from the call order (415071 still written for the engine path); the snow pass and the copy are engine calls whose draws CS's ImageSpace detours take, as before |
| 8, 10, 11 | not done (no effect; the blood block; 2a) |
| 9 | engine functions, called by CS |

The engine's FrameAnnotations perf event "World" moves into the driver. Guest and D3D11 still enter the engine's RenderWorld.

### 2c. Not done: the CS points inside the pre-resolve and post-resolve

Each still runs as a thunk around an engine call inside 106583 / 106584 and reads state the engine's calls just set. In call order,
with what each reads (from snap3-world's inventory):

| Point | Reads (engine-set) | CS source to use |
|---|---|---|
| `Main_RenderWorld_Start` (106583 +0x84): StartDeferred, Prepass anchor, DCLF BeforeOpaquePass | shadow-state RT0-3, `perFrame` (bound b12), OMGetRenderTargets in ApplyPendingState | fixed forward target set; FrameCamera b12 |
| DCLF `SkyBatch` (+0x237), cloud group (+0x274) | main accumulator, `deferredPass`, shadow-state RTs, t17 | CS frame's target set; the snapshot camera (already) |
| `Main_RenderWorld_BlendedDecals` (+0x308): AfterOpaquePass, terrain blending, EndDeferred, DeferredPasses, composite | `forwardRenderTargets`, renderer targets, `perFrame` | the same set; graph targets |
| Skeleton depth copy (+0x39D), MaskPassesCheck (+0x3B4), stencil clear (+0x3C9), `WaterBatch<Mask>` (+0x492), Skeleton clears (+0x556) | bound headers set by SetRenderTarget at +0x3FF..+0x476 | the water epochs' own targets |
| RenderReflections (+0x55B) | sets SetCameraData itself | snapshot camera |
| `Accumulator_BlendedDraw<0>` (106584 +0x51), the EngineCopy, `WaterBatch<Surface>` (+0x143, +0x161), WaterBlend (+0x1BE), `PostWater_SetCamera` (+0x239), `Accumulator_BlendedDraw<1>` (+0x543) | accumulator pointers, shadow state, SetCameraData's camera | CS frame, snapshot camera |

When these are CS functions, the driver calls them in this order in place of steps 4 and 7, and the accumulator, SetCameraData and
the sun's functions go (the sun query draw from the sun's geometry in the snapshot; UpdateOcclusionQueries to the capture, decision 8).

## Detector (native Vulkan, per world frame)

| Site | Before (`s3-world-f-nvk-quick`) | After (`s3-rworld-c-nvk-quick`) |
|---|---|---|
| Renderer::StartAccumulating (in Main::RenderWorld) | 1.01 | 0 (CS calls it) |
| ImageSpace::RenderSSAO #1, #2 | 1.01 each | 0 (CS's route) |
| ImageSpace::DispatchCopySubRegion | 1.01 | 0 (CS calls it) |
| Renderer::FinishAccumulatingPostResolveDepth | 1.01 | 0 (CS calls it) |
| ShaderAccumulator::RenderFirstPersonViewEnd (in RenderWorld) | 1.01 | 0 (CS calls it) |
| BSCullingProcess::Process, Renderer::SubmitAccumulator (splatter, hooked) | 1.01 each | 0 (not called) |
| Sun::SubmitOcclusionQuery | 1.01 | 0 (CS calls it) |
| AE 107219, 107221, 106324, 106979 (helpers) | 1.01 each | 0 |
| pre-resolve and post-resolve sites | unchanged | unchanged |
| engine render calls | 65.0 | 56 |

Of the nine render-role sites that left the count, five are engine functions CS's driver still calls (StartAccumulating,
DispatchCopySubRegion, the post-resolve, RenderFirstPersonViewEnd, SubmitOcclusionQuery); they are not counted because CS calls them
directly, not through an engine call site (decision 57). Honest count of engine render functions still entered from RenderWorld's
work: those five plus the pre/post-resolve's own sites.

Per world frame, other configs (before from snap3-world's runs, after this branch): screen blood 55.7 -> 47, interior 48 -> 38;
guest unchanged (64-70, scene-dependent).

## Runs (stage `s3-rworld`, Dev-Fast, detector on unless noted)

| Config | Label | Result |
|---|---|---|
| guest+screen-blood (item 1 build) | `s3-rworld-a-guest-blood` | passed; blood drawn (64 render calls) |
| native-vulkan+screen-blood (item 1 build) | `-a-nvk-blood`, `-a-nvk-blood2` | passed, but the Health Low tutorial paused the game: no hit |
| native-vulkan+screen-blood, fixed config | `-a-nvk-blood3` | passed; blood drawn (56) |
| native-vulkan+cadence-drop-cells, blur parity | `-b-blurpar1/2/3` | passed; gate off: 0 differ; gate forced: 2 differ on the forcing render only |
| native-vulkan+quick (driver) | `-c-nvk-quick` | passed; 56 render calls (65 before); shot normal |
| guest+quick | `-f-guest-quick` | passed (70) |
| guest+screen-blood (fixed config) | `-f-guest-blood` | passed; blood client on the graph (64) |
| native-d3d12-quick | `-f-d3d12-quick` | passed (54) |
| native-vulkan+screen-blood (fixed config) | `-f-nvk-blood` | passed; blood client on the graph (47) |
| native-vulkan+cadence-drop, -drop-cells, -repeat | `-f-drop`, `-f-drop-cells`, `-f-repeat` | all passed (56, 47, 43) |
| native-vulkan+shadows-blend | `-f-shadows-blend` | passed (44) |
| native-interior | `-f-interior` | passed (38; 48 before) |
| native-river | `-f-river` | passed (41) |
| native-vulkan+post-effects | `-f-post-effects`, `-f-post-effects2` | passed; the first run's shots show the get-hit blur from the save's hostile guard, the rerun is sharp |
| native-tour | `-f-tour` | passed (39) |
| cadence.py, native-vulkan+cadence-still | `-f-cad-*` | all four passed; exterior repeat 16.6 / drop 14.1 against a floor of 12.1, interior 9.7 / 8.5 against 8.9: within noise |

Shot differences against snap3-world's runs are dominated by the save's hostile guard (pose, health, get-hit and low-health blur,
the Health Low tutorial); compare shots of the same config only when health matches.

## Decisions (agent, to confirm)

- **RW-a.** Guest screen blood takes native's route (a deliberate guest change, like decision 91); the latched blood family is deleted.
- **RW-b.** The driver came before the pre/post-resolve points: owning the call order first lets each remaining engine call be
  replaced in place, instead of converting thunks that would still be called from the engine's body.
- **RW-c.** The TLS +0x768 tag RenderWorld sets (0x41) is not written by the driver.
- **RW-d.** The blur camera test steps once per render (as the engine did per RenderWorld call), from the adopted snapshot's camera.
- **RW-e.** The flare setup in RenderWorld is not called in native (the capture runs it); guest still runs it (harmless: same outputs).

## Proposed decision-log entries

- **Guest screen blood takes native's route** (deliberate guest change, as 91): the capture fills the screen blood block in both
  graph modes and the draws come from ScreenSplatter's root; the latched blood family is deleted.
- **CS drives Main::RenderWorld in native** (refines 98): the engine's function is not entered; CS calls its remaining engine
  functions (StartAccumulating with the pre-resolve, the post-resolve, the sun's query functions, the accumulator's end) in its
  order, and its own CS points (RenderSSAO, the copy or snow, the composite) take CS's route. The light flare setup, the splatter cull
  and submit and the blur camera test are not run. The pre/post-resolve points are next, then those engine calls go.
- **The full-screen blur's camera test is kept by the native post chain** per render from the snapshot's main camera; its gate is
  captured in the ImageSpace block.
- **The screen-blood config raises health at world-ready** and expects the blood client on the graph.

## Open items

- 2c (above): the CS points in the pre-resolve and post-resolve, then the accumulator, SetCameraData and the sun functions.
- SE untested (every address is an SE/AE pair from the SE disassembly).
