# snap7-split: the native render thread (roadmap phase 7, items 1, 2, 3, 6, 8)

Lead `split`, branch `snap7/split` (base `org_integration` 7214593fd, with `snap3/pov` 986fbfb03 and later `org_integration`
cde8a8411, which brings `snap7/prep2`, merged), stage `s7-split`.
Plan: [snap7-prep.md](snap7-prep.md), "Phase 7 plan". Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md), phase 7.

Status: **in progress.** The render thread is not created yet (section 9, SPLIT-a). This note is the plan, the inventory that sizes
it, and the steps landed so far (section 5: the driver's game and render halves, the screen blood, the normals target and the water
port's game values moved out of the render half; the cap configs), so a successor can continue with section 7.

## 1. Baseline (org_integration 7214593fd + snap3/pov, Dev-Fast)

| Run | Label | Result |
|---|---|---|
| native-vulkan+quick, detect | s7-split-base-nvk-quick | passed; 651 live engine reads and 1 engine render call per world frame |
| native-vulkan+perf-taa (exterior, uncapped) | s7-split-base-perf-taa | 141-147 fps (Dev-Fast, /Od) |
| native-vulkan+perf-dragonsreach (interior, uncapped) | s7-split-base-perf-dr | 155-201 fps (Dev-Fast, /Od) |

Live reads per world frame by category (quick): capture-point 182.6, record-time 403.6, bound-state 9.1, other 55. By feature: DCLF
325, RenderGraph 231.5 (mostly `Renderer` target tables: EngineTargetSet, Skeleton, PostChain), Utils 25, UnifiedWater 14, Deferred
8.5, TerrainBlending 8. The detector does not count raw `REL` global reads or scene-graph walks (snap0-detect.md, "Not covered"), so
these numbers undercount what the render side reads of the engine.

## 2. What the render thread may read

The rule: the render thread must not read engine state that the game thread mutates, and must not call the engine. Classes:

- **Render-owned engine memory** (safe once the game thread no longer runs engine render code): the Renderer's target tables in
  native (CS's target set writes them; the engine only on a resize, which the render thread will drive itself), RendererShadowState,
  BSGraphics::State, the Tripwire context's bound state. Today's game thread still enters engine render helpers inside
  Main::DrawFrame (Renderer::Begin's state resets, the target binds before the UI, ImageSpace effect 0x22, the menu scenes), so these
  stop being shared only when the game thread's DrawFrame runs none of them (section 4).
- **Game-mutated state** (must move into the capture or the driver's game half): the scene graph (nodes, hidden bits, transforms,
  the first-person subtree), ShadowSceneNode lights, Sky/TES/Player/PlayerCamera/NiCamera, UI, ImageSpaceManager (the game updates
  its modifiers), INI and game settings, timers, Data.

## 3. Inventory: the world frame's points (from the skeleton driver, native)

Per point, what it still reads or writes of game-mutated state on the render side (detail: file:line in the agent survey kept in
the scratchpad; sizes S/M/L/XL):

| Point | Game state read or written at record | Size |
|---|---|---|
| Driver game work (415299, branches, list cameras, queued lights, hidden bits, first-person shift, sky colours, 1P port, tail) | game writes interleaved with the points | done in step 1 (below) |
| SetupSunAndSky +0xD3 = DCLF `FrameDriver::BeginFrame` + water render | FrameGlobals::Capture (INI, ~25 raw globals, accumulator, portal-graph and 1P subtree hidden bits), FrameView captures, SampleClassifyInputs, PrimaryCull camera, PassCapture::RefreshMainRenderers, LatePoses, CaptureShadingFields, ServeFrameRequests (writes engine memory). Must run before the first-person shift (rows and palettes) | L: split BeginFrame into a capture half (game) and an adopt/coverage/payload half (render) |
| Shadow casters +0x1C4 (NativeCasters, LightSelection, SunCamera, LocalCameras) | ShadowSceneNode lists, light cameras and accumulators (writes), ~15 globals, MemoryManager and engine TLS, PrimaryCull::PrepareFrame | XL: a captured light set; render-owned shadow cameras |
| RenderShadowMaps +0x2EC | Skylighting/precipitation occlusion (Sky, precip, ComputeProjection engine call), `shadowLightsAccum` walk, light cameras, EarlyPrepasses (smState sun) | L |
| RenderDepth +0x395 | Terrain Blending (Renderer depth fields, posAdjust), LatePoses::ConfirmFrame scene walk, player 3D, first-person subtree walk, far depth range global | M |
| RenderShadowMasks +0x39C | shadow node 415033, ~12 shadow config globals, lights (ReturnShadowmaps writes) | M |
| RenderPrecipitation +0x3A1 | Sky, precipitation object and its projection (writes) | M |
| RenderWorld +0x841 | water state globals, ISM fields, TESWaterSystem, smState sun, refraction flags, `SetCameraData`, 415072 write, `renderSnow` engine call (the water port's frame values: done, step 5) | L |
| RenderFirstPersonView +0x954 | player, UI, PlayerCamera, 1P accumulator | S-M |
| RenderSaoCs +0xA4C | not ported: the engine's function runs | M-L |
| RenderWorldPostProcess | live refraction/DOF effect state, the engine's composite and end-of-frame functions on the live ImageSpaceManager | L |
| RenderScreenSplatter +0xA73 | done (step 2) | - |

Thread identity assumptions on these paths: engine TLS (`NativeCasters` MemoryContext writes the executable's TLS slot),
`EngineReadWindow`'s `IsMainThread` gates, DCLF's `mainThreadOnly` revision source (`FrameDriver.cpp:115`), `FrameView`'s latched
switch-event thread, `FrameGlobals`' render thread, thread_locals in WorldPass, ClassifyInputs and ShadowMapsNative.

## 4. Design for the split (decided by the agent, to confirm)

- **Frame kinds.** Only World frames move to the render thread at first. Main menu, menu over world (the frozen background and its
  capture through the engine's RenderScene #1), loading screens (serving thread) and Bink keep rendering on their producing thread,
  with the render thread parked, until their remaining engine paths are ported (the book and local map, the frozen composite
  1414cba80, the menu scenes recorded at UI3DSceneManager::Render, Bink's decode). Ownership moves with ClaimRenderOwnership at a
  kind change, the only place either side waits (one render frame, at a kind transition). Why: every non-world kind still records at
  engine call sites on the producing thread; splitting them is separate ports.
- **Game thread, native world frame:** Snapshot::Capture builds the snapshot but posts it only at the Present point (Renderer::End's
  present = publish). In between, Main::DrawFrame runs its game half: the driver's game half (`Native::Scene` PrepareFrame), the
  decal attaches, the UI's PreDisplay, DrawInterface as the UI producer (decision 63 changes: the stream is set before the post), the
  job events. Renderer::Begin's engine body and CS's frame start do not run there. Then SideEffects after the render half, the
  update-rate cap (`PaceGameUpdate`, switch removed) and the engine-read window's close.
- **Render thread:** created at the first native Begin. Loop: adopt the newest snapshot (repeat if none), OnBegin (frame clock,
  revisions, installs), resize from the snapshot's window event (native device Resize, swap chain, target versions), the driver's
  render half, the UI stream replay, the frame end (today's Present hook body: DCLF BeforePresent, seal, pacing, State::Reset's
  render half, metrics, Testing::OnPresent's shots), present.
- **Test harness:** steps on the game thread (OnGameUpdate), shots and live-frame probes on the render thread, naming the snapshot.

## 5. Steps landed

### Step 1: the driver's game half runs before its render half

`Native/RenderScene.cpp`'s driver is now `PrepareFrame` (game half) then `RecordFrame` (render half), handing over a value struct
`Native::Scene::Prepared` (`RenderScene.h`). The game half runs, in the engine's order: 415299, the branches, the list processes'
camera, SetupSunAndSky (DCLF's frame start, still here), the first-person root hidden around the queued lights, the shadow casters
with the sky node hidden (still here), the first-person shift with the first-person camera taken (world, frustum, port and DrawWorld's
eye), the sky colours, the first-person port, the clear values, the shift back, the end-of-frame flags, the SAO helper, the splatter
test, the depth of field camera's near and far, and Main::Draw's tail. The render half runs the shadow maps, depth, masks,
precipitation, the clears, RenderWorld, ClearMasks and the first-person view, the SAO, the post chain and the splatter, from
`Prepared` only.

Readers moved to `Prepared`: DCLF's first-person depth epoch (`FirstPersonEpochs.cpp CaptureCamera`, which read 415007 and 415027
at record time) and the post chain's depth of field near/far (`NativePost.inl ReadB` and `RunBody`, which read 415007). Frames the
engine's own skeleton draws (guest; native's frozen-menu capture) keep the engine's camera.

Changed order, checked in runs (first-person, first-person-switch, quick and interior shot references, post-effects, drop-cells):
the record points now run after the shift back and Main::Draw's tail, and with the first-person root in its end state (shown in
first person) instead of hidden during the depth pass and RenderWorld. No run showed a difference.

### Step 2: the screen blood's draws in the capture

`BloodSplatter::CaptureBlock` (game thread, world snapshots) now also takes the draws from ScreenSplatter's root and camera (the
walk, the properties' alpha, the world-view-projection) into its block, which holds the geometries as `Snapshot::EngineRef`s.
`CollectDraws` on the render side copies them from the adopted snapshot. A capture that could not take them carries the reason,
which the render side logs once, as before.

### Step 3: the normals target in the game half

`WorldPass::NormalsTarget()` (ImageSpaceManager +0x1C0 and the engine's frame counter, which CS moves on the game thread, S2) is
taken in the driver's game half; inside a native world frame's render half it returns the taken value.

### Step 5: the water port's game values in the capture

`Water::Port::CaptureFrame` (called at the water scene rows and batches, render side) read about 25 engine values each time: the
fog of the scene graph's ShadowSceneNode, the reflection timer, the sun's direction and colour, the SSR effect and INI values, the
water lighting, flowmap and displacement globals, the underwater, water-enabled, local-map and render-mode globals, and TES and the
player for the cubemap fix's interior test. They are now `ReadGame`, taken once per update by a registered capture
(`Port::CaptureGame`, block `BlockId::WaterPort`, after the water update the capture runs). In native, CaptureFrame starts from the
adopted snapshot's block and fills only the view's values (frame camera, planes, render size, eye, raw water, CS's feature hooks).
Guest reads everything live as before. Unified Water's flow rewrite takes the exterior test from its snapshot block in native.

### Step 4: update-rate cap configs

`tools/cs-test/configs/update-cap30.toml` and `update-cap240.toml` (item 8): the game at 30 and 240 updates a second, shots after
settling, after a turn and once it settled. Until the render thread runs they need `snapshot.update_rate_cap` (the switch phase 7
removes) and the engine's Present still paces the render side to the game. Checked through `--set` on the main checkout's quick
config (the main checkout does not have these files until merge): cap 30 and cap 240 passed.

## 6. Next: split DCLF's frame start (`DCLF::FrameDriver::BeginFrame`)

The SetupSunAndSky point is DCLF's frame start. It still runs in the driver's game half (step 1) because it mixes both sides. The
map of its calls (FrameDriver.cpp:163-326), side by side:

- **Game half, once per update (reads or writes engine memory):** LoadHold::OnFrameStart (UI), the diagnostics notify,
  ReleaseRenderHalf (the window), SetScenePassMode, SampleClassifyInputs, FrameView::BeginFrame (CaptureLightingShader,
  FrameGlobals::Capture with the first-person subtree sync, SceneClock, PostFrameInputs), PrimaryCull::BeginFrameCamera,
  RootStatesFrameStart (fades: camera, list processes), PaletteRowsFrameStart (particles), the first-person view work
  (RunViewWork BeforeCoverage: the skeleton walk), ComponentsAdopt (LodGates::Flip writes LOD node hidden bits) and
  ClearAdoptedDeltas, ServeFrameRequests (switch catch-ups write engine memory), ShadowSourcesFrameStart (LocalLightCull),
  PassCapture::RefreshMainRenderers (accumulators), DecideDrawFamilies' engine part (grass manager node, grass collision),
  PublishListFilter (TES, list processes), CaptureShadingFields, LatePoses::PrepareFrame, and FrameValues::Kick's sampling (fade
  seeds, fade and particle kicks, SampleRootViews before the first-person shift, the rows job under leases).
- **Render half, once per render frame:** BuildPoint (already at Begin in native), HandOverAtFrameStart's table sync and
  SyncFrameTables, ShadowExclusionFrameStart, PostLookupInputs (move the one-time depth-stencil read to init), DecideCoverage
  (take `loading` from the frame's FrameGlobals, not the UI), Reports::GateReleases, PostAheadContext, RootStatesKick (fade
  write-back decode), KickVisibilityReadback, DecideDrawFamilies' decision, the reflection view's Prepare (one cube format read
  to move to init), SealCpuFrames, FrameValues::Kick's GPU half (backings, PublishFrameVersion, the frame wait value, the upload),
  MakeFramePayload.

Conflicts to resolve first:

1. Adoption and coverage are render-side (the revision adopts the publication at Begin; coverage decides whether claims install),
   but the claims' consumers are game-side (the cull and registration hooks, LOD flips, CastersWithheld, SetFamilyWithheld, the
   list filter, shading and seed reads). Proposal: the game half picks the publication and installs the claims once per update
   (a "game ticket"), and the render half only checks coverage of the ticket it renders (an uncovered frame is a tap error or the
   load hold, as today).
2. View coverage (`SetViewCovered`) gates SetRootViewClaims, which SampleRootViews needs in the game half before the shift. With
   the ticket above, the game half decides coverage for the views it samples.
3. FrameValues::Kick fuses engine sampling with per-render-frame GPU sequencing; its one-shot queues (placement plans, shading
   items, root view claims) must be drained once per update.
4. `FrameGlobals::Current()` keys on the render thread id; once the capture is game-side it must be handed over in the ticket.
5. `sceneStep` counts render frames and is used as a frame number by the coordinator inputs, SceneClock, NoteCameraCut, the
   first-person 16-frame walk, the family mirror drains, the reports and LatePoses' pairing. Decide per use whether it means an
   update (ticket) or a render frame.
6. `EngineReadWindow`'s Hold/Release are main-thread and in-update only: the game half keeps them, the render half never calls them.

DCLF's other points: DepthCommitted (LatePoses::ConfirmFrame walks roots), EarlyPrepass (ServeMaterialRequests reads
`property->material`), Prepass (LatchAccumulator, RefreshFrameConstants' shadow state), BeforeOpaquePass (ApplyPendingState
writes renderer state), BeforePresent (LatePoses::CheckAtPresent), EndFrame (HoldForRenderHalf, the list graveyard, the UI read
in NoteLoadingScreen) also need their engine reads moved into the game half or the ticket.

## 7. Remaining work, in order

The thread can only be created once the world frame's render half reads no game-mutated state (the user's rule; a render thread
that waits for a read window would make it wait on the game). So the order is reads first, then the thread:

1. **DCLF's frame start split** (section 6): the per-update "game ticket" (FrameGlobals, the chosen publication and installed claims,
   consumed deltas, sampled rows, cameras), made in the driver's game half; the render half adopts the ticket. L.
2. **Shadow casters** (+0x1C4): a captured light set (selected lights, their cameras and caster planes as values) made in the game
   half; render-owned shadow cameras; `RenderShadowMaps` walks the captured set, not `shadowLightsAccum`. The engine TLS and
   MemoryManager use in NativeCasters/SunCamera stay on the game thread. XL.
3. **The remaining points' reads** (section 3): RenderDepth (Terrain Blending's depth fields and posAdjust, LatePoses' confirm walk,
   the far depth range), the masks (shadow node and config globals), precipitation (Sky, the precipitation object, its projection:
   move ComputeProjection and the occlusion setup to the capture), RenderWorld (water globals, ISM fields, smState sun, refraction
   flags, the 415072 write), the first-person view (player, UI, PlayerCamera), the post chain (refraction and DOF effect state; the
   engine composite and end-of-frame calls need ports), SAO compute (port RenderSaoCs). Also the record-time `Renderer` target-table
   reads are render-owned once the game thread enters no engine render code (section 2).
4. **The other features' record-time reads** the detector lists (WetnessEffects' Sky, Skylighting's Sky, UnifiedWater's TES and
   Player, Utils/Game.cpp's posAdjust and GraphicsState, LightLimitFix, Skin, ExponentialHeightFog): each into its block.
5. **Present as publish** (item 2): native world frames post the snapshot at the Present point after DrawInterface (decision 63
   becomes "set before the post"); Renderer::Begin's engine body and CS's frame start do not run on the game thread; SideEffects
   S1/S4 move right after the capture (decision 34); `PaceGameUpdate` loses its switch; `EngineReadWindow`'s render-half hold goes.
6. **The thread** (item 1): created at the first native Begin; ClaimRenderOwnership once; its loop as section 4. Non-world kinds stay
   on their producing thread with the render thread parked until their ports land (section 4).
   ImGui (snap7-prep2, merged): `Menu::DrawOverlay()` moves with the frame end; input already arrives through the queue, test
   menu opens go through `Menu::PostUiTask`/`RequestOpen`, engine writes from the ImGui frame go through `GameThread::Post`
   (`Utils/GameThread.h`; `CanWriteNow` keys on the main thread, so everything posts once the ImGui frame runs on the render
   thread). snap7-prep2.md lists the UI-frame reads still left.
7. **Revision adoption** (item 3): DCLF's `mainThreadOnly` source (FrameDriver.cpp:115) becomes a render-owner adopter;
   `FrameAnchors::IsMainThread()` checks in FrameDriver (FrameOnlySource, FreezeTestAtBegin, SampleEngine) go: the kind and the
   sampled engine values come from the snapshot.
8. **Resize** (item 6): the render thread acts on `Events::windowSize` itself (native device Resize, QueueSwapchainResize, target
   versions), not through Renderer::RequestWindowResize and Begin.
9. **Harness** (item 8): steps from `Testing::OnGameUpdate` on the game thread; shots, live-frame probes and the exit from the render
   thread's frame end, naming the snapshot (decision 51).

## 8. Runs

All on stage `s7-split`, Dev-Fast.

| Config | Label | Build | Result |
|---|---|---|---|
| native-vulkan+quick, detect | s7-split-base-nvk-quick | base | passed; 651 live reads, 1 render call per world frame |
| native-vulkan+perf-taa | s7-split-base-perf-taa | base | passed; 141-147 fps |
| native-vulkan+perf-dragonsreach (uncapped) | s7-split-base-perf-dr | base | passed; 155-201 fps |
| native-vulkan+first-person, detect | s7-split-a-nvk-fp | step 1 | never-loaded (no data-loaded within 120 s at plugin load, before any changed code; machine contention); rerun below |
| native-vulkan+first-person-switch | s7-split-a-nvk-fpswitch | step 1 | passed (5 shots, first person drawn) |
| native-vulkan+quick, detect | s7-split-a2-nvk-quick | step 1 | passed with the shot reference; 693 live reads, 1 render call |
| native-vulkan+native-interior, detect | s7-split-a2-nvk-native-interior | step 1 | passed with shot references; 512 / 1 |
| native-vulkan+first-person, detect | s7-split-a2-nvk-first-person | step 1 | passed; 568 / 1 |
| guest+quick | s7-split-a2-guest-quick | step 1 | passed |
| native-vulkan+cadence-drop-cells | s7-split-a2-nvk-dropcells | step 1 | passed, 3 shots |
| native-vulkan+post-effects | s7-split-a2-nvk-post | step 1 | passed, 13 shots |
| native-vulkan+quick, cap 30 (`--set`) | s7-split-a2-cap30 | step 1 | passed |
| native-vulkan+quick, cap 240 (`--set`) | s7-split-a2-cap240 | step 1 | passed |
| native-vulkan+screen-blood, detect | s7-split-b-nvk-blood | step 2 | passed (blood shots, no refusal); 565 / 1 |
| native-vulkan+quick, detect | s7-split-c-native-vulkan-quick | step 3 | hang at the loading screen: the main thread in an engine sleep loop inside Main::Update's game logic (hdtSMP's update hook -> FUN_140fa4020), no CS frame on its stack; rerun below |
| native-d3d12-quick, detect | s7-split-c-native-d3d12-quick | step 3 | passed; 646 / 1 |
| guest+quick, detect | s7-split-c-guest-quick | step 3 | passed; guest 66 render calls (unchanged) |
| native-vulkan+native-tour, detect | s7-split-c-native-vulkan-native-tour | step 3 | passed; 525 / 1 |
| native-vulkan+shadows-blend, detect | s7-split-c-native-vulkan-shadows-blend | step 3 | passed, 7 shots; 554 / 1 |
| native-vulkan+rain, detect | s7-split-c-native-vulkan-rain | step 3 | passed; 565 / 1 |
| native-vulkan+native-river, detect | s7-split-c-native-vulkan-native-river | step 3 | passed; 564 / 1 |
| native-vulkan+menu-scenes, detect | s7-split-c-native-vulkan-menu-scenes | step 3 | passed with shot references (inventory, journal, loading model, stats, world) |
| native-vulkan+window-resize-stress-taa, detect | s7-split-c-native-vulkan-window-resize-stress-taa | step 3 | passed, 13 shots |
| native-vulkan+quick, detect | s7-split-d-native-vulkan-quick | step 5 | passed with the shot reference; 624 / 1 |
| native-vulkan+native-river, detect | s7-split-d-native-vulkan-native-river | step 5 | passed; water, reflections and flow drawn; 545 / 1 |
| native-vulkan+native-interior, detect | s7-split-d-native-vulkan-native-interior | step 5 | passed with shot references; 491 / 1 |
| guest+river, detect | s7-split-d-guest-river | step 5 | passed; guest 56 render calls |
| native-vulkan+cadence-drop-cells, detect | s7-split-d-native-vulkan-cadence-drop-cells | step 5 | passed, 3 shots; 3 render calls (DrawFrame per update, 1 in 3 renders) |
| native-vulkan+perf-taa | s7-split-d-perf-taa | step 5 | passed |
| native-vulkan+perf-dragonsreach (uncapped) | s7-split-d-perf-dr | step 5 | passed |

| native-vulkan+quick | s7-split-e-native-vulkan-quick | after merging org_integration cde8a8411 | passed |
| guest+quick | s7-split-e-guest-quick | merged | passed |
| native-vulkan+first-person | s7-split-e-native-vulkan-first-person | merged | hang at the loading screen, the same stack as s7-split-c-native-vulkan-quick; rerun below |
| native-vulkan+facegen | s7-split-e-native-vulkan-facegen | merged | passed with the shot reference |
| native-vulkan+first-person | s7-split-e2-native-vulkan-first-person | merged | passed |

The two loading-screen hangs have the main thread asleep in an engine wait inside Main::Update's game logic (hdtSMP's update hook,
an SKSE task, FUN_140fa4020 or FUN_140e01980), with no CS code on any stuck stack. The snap7-prep2 lead's run
`s7-prep2-nvk-menuscenes` (without this branch's changes), at the same time, has the identical stack: a base or machine-load flake
while several games ran, not this branch.

Frame rates (Dev-Fast, /Od; from the marks' frame counts and times, which count game updates, one render each until the thread
runs):

| Scene | Before (base) | After (step 5) |
|---|---|---|
| perf-taa, the autoloaded exterior, TAA, uncapped (the heavier GPU scene) | 139.2 fps | 135.3 fps |
| perf-dragonsreach, interior, uncapped (lighter GPU, CPU-bound in Dev-Fast) | 180.8 fps | 179.6 fps |

The differences are within run-to-run noise: the steps moved reads, not work. The gain phase 7 is after comes only with the thread.

Live reads move within scene noise (the moved reads are mostly raw `REL` reads the detector never counted).

## 9. Decided by the agent, to confirm

- SPLIT-a. Reads first, thread last: the thread is not created on this branch, because every world point still reads
  game-mutated state at record time, and a render thread would race the next update (or have to wait for a read window).
- SPLIT-b. The driver's game half runs SetupSunAndSky (DCLF's frame start) and the shadow casters until those are split; both are
  named in `PrepareFrame`'s comment.
- SPLIT-c. The game half runs Main::Draw's tail before the render half (in a native world frame nothing fills what it releases).
- SPLIT-d. The render half sees the first-person root in its end state (shown in first person), not hidden during depth and
  RenderWorld: in phase 7 the render thread sees only the end state anyway.
- SPLIT-e. Non-world frame kinds keep rendering on their producing thread, with the render thread parked, until their ports land;
  ownership moves at kind changes (section 4).
- SPLIT-f. The post chain's DOF write into the engine's DOF effect (`RunBody`) uses the values ReadB took (the game half's in native)
  instead of reading 415007 again.

## 10. Proposed decision-log entries

- **The native driver has a game half and a render half** (phase 7, step 1): `PrepareFrame` runs the engine-order game work and
  hands `Native::Scene::Prepared` (branches, clear values, end-of-frame flags, the first-person camera after the shift, the DOF
  near/far, the normals target) to `RecordFrame`, which runs the points from the shadow maps on. DCLF's first-person depth epoch and
  the post chain's DOF read `Prepared`. SetupSunAndSky and the shadow casters still run in the game half until split.
- **Screen blood draws are captured** with their geometries held (`BloodSplatter::Block::draws`, `geometries`).
- **Reads first, thread last** (SPLIT-a), with the DCLF frame-start ticket (section 6) and the captured light set as the next two
  items.
