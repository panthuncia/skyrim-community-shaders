# Rendering off the main thread: roadmap

Status: proposal, 2026-11-25. Nothing here is started. It follows round 3 of the render-thread work
(`render-thread-window.md`) and the fully native direction.

## Goal

Skyrim's main thread runs the game update and then its render half. Today CS's whole frame runs inside that render half,
driven by engine hooks. The goal:

- Everything that contributes to a produced frame becomes part of a **frame snapshot**: immutable data, built on the game
  side, with no pointers into live engine state that the game may change.
- The game side **publishes** each snapshot atomically (latest wins).
- A **CS render thread** swaps in the newest snapshot at the start of each of its frames, records and submits the frame, and
  presents. It never reads engine memory, never calls the engine, and never waits on the game.

This generalises DCLF's scene snapshot (`Scene::FrameInputs` in, `ScenePublication` out, adopted at frame start) to the
whole frame.

What the move buys: the render thread's CPU work overlaps the next game update instead of adding to it. In the Release
capture the window from DCLF's scene tables to Present is 1.31 ms. Of that, ORG epochs (0.32), CS features (0.33) and DCLF
(0.24) move to the render thread. The engine's own code (0.33: culling, accumulation, Scaleform traversal) stays on the
game thread as capture work.

## Where things stand

The graph modes already keep the engine's GPU work out of the frame. The engine's D3D11 context is a Tripwire handle that
keeps binds and drops draws (`src/RenderGraph/Engine/Tripwire/Tripwire.h:22-31`). Guest B6/B7 found no engine work
reaching DXVK inside a stage. So the remaining problem is not engine draws. There are three:

1. **The engine still drives the frame.** Every CS stage runs because the engine reached a call site: Renderer::Begin,
   Main::Draw, RenderShadowMaps, RenderWorld, the blended decals, the cube map cameras, first person, post processing,
   DrawInterface, Present. Anchor client `commit()` callbacks and `State::UpdateSharedData` run at those moments, so the
   capture points are scattered across the engine's render sequence.
2. **CS still reads live engine state while it records.** About 25 features read the sky, weather, TES, the player, the
   camera, the shadow state, timers and INI values from inside hooks and record code. Several read state the engine bound
   to the D3D11 context.
3. **Things assume one rendered frame per game frame**, in lockstep, on one thread. These include frame counters used as
   identity, temporal effects, game-time integrators, readbacks written back to the engine, load holds and the test
   harness.

The engine-side rendering that is left (from the inventory):

| Engine work | Today | Size |
|---|---|---|
| Scaleform HUD and menus (`MenuManager::DrawInterface`, 0.042 ms Release) | GFx runs; CS swaps the GRendererD3D1x vtable and records displays into the Interface anchor (`src/RenderGraph/Engine/Scaleform/Renderer.cpp:38-60`) | Large |
| Menu scenes: inventory 3D, MistMenu, lockpick, book, journal, local map | latched at DrawTriShape (`Features/MenuViews/MenuScenesCapture.cpp`); `UI3DSceneManager::Render` and `LocalMapRenderer::Render` run under scope hooks | Large |
| Precipitation, screen blood, decal occlusion tests | engine batch and setup run, CS latches the draw (`Core/Engine/Family/EngineDrawFamily.h`) | Medium each |
| ImageSpace steps still taken from the engine (`ImageSpace.cpp:2107-2141`): uncovered effects, `ISDownsampleCS`, no-raster steps, all menu steps | engine draw captured | Medium |
| ImageSpace bookkeeping: native steps still call each shader's PreRender and PostRender (`ImageSpace.cpp:2206-2246`) | call-through | Large |
| Volumetric Lighting with the CS feature off | engine descriptor, `VolumetricLighting::Render` runs | Small to medium |
| Shadow lights not fully covered | engine `Render` fallback (`ShadowMapsNative.cpp:271-294`) | Medium |
| Sky and water batch calls | still called on emptied batches for the engine's cleanup | Medium |
| Accumulators, culling, scene-list jobs | run; DCLF's PrimaryCull and PassCapture consume them | Large (but see below) |
| Loading screen and startup logo | rendered on the serving/MoviePlayer thread under `Renderer::Lock` | Medium |
| Bink | decode tick only; tiles drawn by CS at Present | Small |

## The rule that sets the scope

The render thread needs **data**, not native ports of everything. A capture that copies its inputs into the snapshot on
the game thread is as good as a native computation for this goal, as long as:

- it reads nothing the engine bound to the GPU context (it computes from engine objects, or records the values the engine
  wrote, as data); and
- the render thread only replays it.

This keeps the engine's culling, accumulation and pass registration on the game side as capture work. DCLF already works
this way. Native ports are still the goal (the fully native direction), but only what the render thread can't replay from
data blocks the move. That removes the largest items (accumulators, culling, menu scene latches as data) from the critical
path.

## Phases

Each phase lands on its own and is testable in today's single-threaded frame. The thread split comes last, when nothing is
left that needs the main thread at render time.

### Phase 0: the snapshot contract and a live-read detector

- Define `FrameSnapshot`: a snapshot id, game time and delta, frame kind (world, menu over world, main menu, loading,
  movie), load epoch, menu states, settings revision, cameras (current and previous, per view: main, cube faces, shadow
  views), the DCLF publication it pairs with, one typed block per feature, the UI command stream, the engine constant
  blocks, and the events since the last snapshot (below). It is immutable once published. Blocks are typed structs, not
  byte blobs; ORG latch regions are filled from them at record time.
- A **live-read detector**. Generalise `FrameGlobals::TakeUnscopedReads` (`Core/Shading/FrameGlobals.h:106`): route the
  engine singletons CS reads (sky, TES, player, camera, shadow state, renderer data, timers, INI) through accessors that
  count, per call site, any read made outside a capture scope. A dev build logs the table at exit. This is the progress
  metric for phases 1-2 and a guard against regressions.
- A **cadence test mode** (decided here, used from phase 3): render each snapshot twice, or drop every other one, still on
  the main thread. It finds cadence bugs before threads make them intermittent.

### Phase 1: one capture point per frame (live reads become snapshot fields)

Turn the scattered captures into one game-side capture that runs before any recording, and make recording read only
the snapshot.

- `State::UpdateSharedData` (`State.cpp:1668`, called three times from Deferred and once from Upscaling) becomes a capture
  of its inputs (sun light, DALC, camera data, timer, water tiles, sky flags, sun and moon) plus a pure function of them.
  Features' `ModifySharedLighting` follows.
- Each anchor client's `commit()` splits into `Capture(SnapshotBuilder&)` on the game side and a fill from the snapshot
  block at record time. Do the largest first: PhysicalSky and VolumetricClouds, Effects11, WetnessEffects, SkySync,
  Upscaling, LightLimitFix (already has `ORGLightCulling::FrameInputs`), PostProcessing (CinematicCamera,
  HistogramAutoExposure), ExponentialHeightFog, ViewAtmosphere, Skylighting, TerrainShadows, CloudShadows,
  VolumetricLighting, InteriorSun, UnifiedWater, TerrainBlending, GrassOptimizations, SSS, ScreenSpaceShadows,
  VolumetricShadows, DynamicCubemaps, TreeWind. The full table is in the investigation notes below.
- Remove engine reads from graph predicates and record code: `globals::state->frameCount` in `active()` and commits
  (ORGPhysicalSky, ORGVolumetricClouds, ORGSkylighting), `renderer->GetDepthStencilData()` (ORGSkylighting,
  ORGVolumetricShadows, ORGViewAtmosphere, Deferred), and the function-local `static RE::GetINISetting` values read live
  (Upscaling, PostProcessing, SkySync). INI and settings values go in the settings revision.
- Water's `WaterPort::CaptureFrame` and LLF's FrameInputs fold into the snapshot as blocks.
- CS settings edited in the menu reach the frame only through the settings revision, never by the render code reading the
  live settings object.

Done when the live-read detector shows no read during recording in the world frame kind.

### Phase 2: no reads of engine-bound GPU state

- DCLF `CaptureBindings` (`Core/Render/Resources/FrameSlots.cpp:177-213`), `BoundPixelBuffer`, `ResolveHeader`,
  `BoundTargetFormats`: replace each with values computed from engine objects or from CS's own records. The engine's
  constant writes captured at Map (`frameBufferCached`, `Globals.cpp:309`, and `EngineConstants`) are data and can stay,
  but they move into the snapshot as blocks captured once per frame, not read by consumers whenever they run.
- ViewAtmosphere's inherited SRVs (t19/22/31/76-79), PhysicalSky's VS b6 and samplers, ColorGrading's VS b1: bind CS's
  own resources from snapshot handles.
- The Tripwire handle becomes a pure capture sink: binds update a CS-side record, nothing goes to the real context. The
  save/restore helpers (Effects11 state backup, RasterPass, PhysicalLens, the NDF and storm code, HDRDisplay, Screenshot,
  TruePBR, HiZ, Skin) disappear with the shared context.

Done when no CS code calls a `*Get*` method on the immediate context during a frame, and the Tripwire forwards nothing.

### Phase 3: cadence independence (single-threaded, with the cadence test mode)

Make every frame-rate assumption explicit. Separate **game identity** (snapshot id, game time, game delta) from **render
identity** (render frame index, render delta).

- **Counters.** `FrameClock` (`RenderGraph/FrameClock.h`) stays the render counter: slot rotation, latch regions and
  published copies are render-side and correct as they are. Anything meaning "a new game frame" keys by snapshot id:
  DCLF's `SceneStep` and its 600-frame scene-pump watchdog (time-based instead), `State::Reset`'s `frameCount` and timer
  (`State.cpp:413-448`, split into game-side timer and menu state capture, render-side counters and feature resets), the
  profiler's idle frames, pacing statistics.
- **Temporal effects.**
  - Jitter is render-owned. Today `PostChain::UpdateJitter` writes `projectionPosScale` into the engine's `BSGraphics::State`
    (`RenderGraph/Frame/PostChain.cpp:241-268`) and Upscaling uses `frameCount`. The render thread applies its own
    jitter to the snapshot's projection; the engine's projection stays unjittered (DCLF culls with it).
  - Motion vectors. The snapshot carries current and previous transforms (camera and objects; DCLF's
    `DrawnPreviousWorld`, sky's `previousPosAdjust`, water). A repeated snapshot renders with zero motion. After skipped
    snapshots, "previous" means the last snapshot this render thread rendered, so the render side keeps the last rendered
    transforms per object key rather than trusting the snapshot's previous.
  - History-based effects (TAA and upscalers, SSGI's `prevInvView`, Skylighting's quadrant rotation, SSR, clouds) run once
    per render frame from the snapshot camera. That is already right, once their inputs stop reading the engine.
  - History reset on discontinuity: a camera cut, a teleport or a load must reset histories even if the snapshot that
    carried the cut was skipped. Events are sticky (below).
  - Auto exposure adapts by render delta, not `BSTimer::realTimeDelta` (`HistogramAutoExposure.cpp:80`).
- **Game-time integrators** on the render side (TreeWind, Wetness rainTimer, VolumetricClouds, Lightning, LODBlendRingFix,
  ENB adaptation and DOF, PhysicalGlare, SkySync, Skin wetness): integrate on the game side into the snapshot when the
  value is world state (wetness, clouds, lightning); use render delta when it is purely visual (glare, adaptation).
- **DCLF fades.** `SampleFrame` and `AnimatedUpdate` (`Features/Fades/FadeState.cpp:257-319`) step once per snapshot, not per
  render frame. A repeated snapshot does not step.
- **Events since the last snapshot.** Flags that happen once (camera cut, load started, teleport, lightning flash, first
  frame after load, menu opened) are OR-merged and counters accumulated until a render frame consumes them, so a skipped
  snapshot loses nothing.
- **Readbacks back to the game.** DCLF visibility (written as `kAccumulated` at `Main::Update`), actor rate, the sun
  occlusion query, fade write-back: tag results with the snapshot id they were rendered from; the game side applies the
  newest result, and applying one twice is harmless. Pre-exposure readback keys by render frame.
- **Writes into engine objects.** CS writes engine state in a few places: native shadow maps (descriptors, slices, free
  mask, light transforms, accumulate-array release, `State::SetCameraData`), the precipitation mask's posAdjust save and
  restore, jitter, the sun query's ring entries. All move to the game-side capture. The render thread never writes engine
  memory.

Done when the cadence test mode (each snapshot twice; every other snapshot dropped) passes the cs-test suite with
matching shots and no fade, jitter or motion artefacts.

### Phase 4: the remaining engine render work becomes data or native

In order of what blocks the split:

1. **Scaleform.** CS already owns the GRenderer vtable. Record the renderer stream (tessellated meshes and their vertex
   data, fills, bitmaps by handle, matrices, colour transforms, masks, blend modes) into the snapshot's UI command
   stream on the game thread, and replay it natively on the render thread. GFx's display-list traversal stays game-side.
   Texture lifetimes follow the snapshot (phase 5). This is the "native HUD" step, done as replay rather than
   reimplementation.
2. **ImageSpace bookkeeping.** Capture each effect's constants and the side effects of PreRender and PostRender as data,
   or port them per effect. Menu end-of-frame chains follow. The world post chain is already native.
3. **Latched families as data.** Precipitation, blood, decal occlusion tests and menu scenes are already latches taken on
   the game thread. Make each latch a self-contained data record (geometry by handle, material constants, transforms;
   no bound-state reads, phase 2) and they need no native port for this goal. Native ports stay optional.
4. **Coverage gaps.** Shadow lights the native path doesn't cover yet, Volumetric Lighting with the CS feature off, and the
   ImageSpace steps still taken from the engine (`ISDownsampleCS`, no-raster steps) either get CS passes or are captured as
   data.
5. **The sky and water batch call-throughs** stay on the game side as capture-time cleanup. They no longer need to happen
   in render order once nothing reads their side effects at record time.

### Phase 5: snapshot lifetimes and the engine's resources

- Snapshots reference engine-owned objects: geometry buffers, textures, materials, Scaleform bitmaps. Each snapshot holds
  references, and a **retire queue** returns them to the game thread once the render thread has retired the snapshot (its
  GPU work done). DCLF's `EngineReleases` and held detaches are the existing form of this; generalise them.
- In-place engine writes to resources a snapshot may still read need versioning. An example is the water normal textures
  assigned by `TESWaterSystem::Update` (the SWF hook already reports these). The game side gets a new version; the old one
  retires with the snapshots that use it.
- DCLF's `EngineReadWindow` (`Core/Engine/EngineReadWindow.h`) lets workers read the live scene graph from Main::Draw to
  Present. It becomes the game-side capture window: it opens after the update and closes before the next update. The
  render thread never takes a lease.
- The engine's dynamic constant ring (`Buffers::EndFrame`) and constant mirrors are copied into snapshot-owned memory.

### Phase 6: the thread split

Native mode first; guest mode follows once native is clean, as with the native frame redesign.

- **The render thread.** CS creates it. It owns ORG's host, DXVK's stream (`ClaimRenderOwnership` and `MoveOwnerThread`,
  `FrameAnchors.cpp:1955-1975`, `RenderGraphRuntime.cpp:2708-2716`; the loading-screen thread is the precedent) and the
  swapchain present.
- **Publication.** The engine's Present call on the game thread publishes the snapshot and returns. Revisions and graph
  installs adopt at the render thread's frame start, as they do now at Begin.
- **Pacing.** The render thread never waits on the game. The game side may wait: when two snapshots are unconsumed it waits
  for the render thread (the frame limiter's job today), which bounds latency to one frame.
- **Frame kinds.** `DecideKind` (`Frame/FrameDriver.cpp:172-177`) moves to the capture. Today it treats any frame off the
  main thread as Loading. The loading/serving thread publishes UI-only snapshots through the same channel. The load hold
  counts snapshots and load epochs, not Presents.
- **Input and UI.** Window messages stay on the game thread. ImGui input goes to the render thread through a queue.
  Settings changes go through the revision lane. Swapchain resize is a published event.
- **Frame generation and Reflex.** PCL markers carry the snapshot id (simulation) and the render frame (submit, present).
  FG gets render-frame ids. FFX's presenter thread already presents on its own queues.
- **Guest mode.** The game thread must no longer use the D3D11 immediate context at all (phase 2 makes the Tripwire a
  sink). Resource creation stays on the device, which is thread-safe. Engine uploads through the immediate context (Map,
  UpdateSubresource during streaming) need an inventory and a copy path before guest can split.

## Watch list: frame-cadence assumptions found

From the cadence investigation. Each one needs the rework named before the split.

| Assumption | Where | Rework |
|---|---|---|
| FrameClock slot = frame % 4, moves at Renderer::Begin | `RenderGraph/FrameClock.h:15-27`, `RenderGraphRuntime.cpp:2516-2521` | render-only; add snapshot id |
| State::Reset after every Present: frameCount, timer, menu flags, feature resets, profiler | `State.cpp:413-448`, `Hooks.cpp:730-735` | split game and render halves |
| Present hook order: Present anchor, DCLF BeforePresent, pacing, seal, constant and buffer EndFrame, State::Reset, test steps | `Hooks.cpp:586-743` | render-thread frame end, plus game-side publish |
| DCLF FrameDriver BeginFrame and EndFrame mix game and render work | `Core/Render/FrameDriver.cpp:164-398` | capture half and consume half |
| SceneStep and the 600-frame scene-pump watchdog | `Core/Engine/SceneClock.h`, `DrawcallLimitFix.cpp:344-354` | snapshot id, time-based watchdog |
| Jitter written into engine state at Begin | `Upscaling.cpp:549, 2092-2104`, `PostChain.cpp:241-268` | render-owned jitter |
| Previous transforms are the previous game frame | `Core/Gpu/FrameValues.cpp:192-197`, `SkyNative.cpp:599`, `WaterPortEvaluate.cpp:410` | last rendered transforms per key |
| Auto exposure by game delta, pre-exposure by frameCount | `HistogramAutoExposure.cpp:25-117`, `State.cpp:1671-1692` | render delta and render frame |
| Game-time integrators in render code | TreeWind, Wetness, VolumetricClouds, Lightning, LODBlendRingFix, ENB, PhysicalGlare, SkySync, Skin | game-side or render delta |
| DCLF fades step per frame and write back at Main::Update | `Fades/FadeState.cpp:257-319`, `FadeWriteBack.cpp` | step per snapshot |
| DCLF visibility and actor-rate readbacks written at Main::Update | `Core/Render/Readback/Visibility.cpp:859-886`, `DrawcallLimitFix.cpp:616-620` | snapshot-tagged results |
| Render ownership claimed by whoever calls Begin; non-main frames are Loading | `FrameAnchors.cpp:1912-1975`, `Frame/FrameDriver.cpp:172-177` | explicit render thread; kind from capture |
| Revision fragments adopt only on the main thread | `Frame/Revision.cpp:1628-1629` | adopt at render frame start |
| EngineReadWindow leases from Main::Draw to Present | `Core/Engine/EngineReadWindow.h:10-24` | game-side capture window |
| JobList::Finish waits on engine culling jobs inside RenderShadowMaps | engine, `Api/EngineEvents.h:26` | stays in the capture |
| Capture TLS assumes engine callbacks on one thread | `PassCapture.cpp:49-56`, `LocalLightCull.cpp:94-146`, `SunAccumulation.cpp:124` | capture-side only |
| Load hold counts frames and keeps the Mist menu open | `Core/Render/LoadHold.cpp:30-47`, `Hooks.cpp:622-636` | count snapshots and load epochs |
| ImGui input from the input poll and WndProc, drawn at Present | `Hooks.cpp:932-970, 1072`, `Menu/OverlayRenderer.cpp` | input queue; settings via revisions |
| PCL markers set in input poll and Present | `Hooks.cpp:582-584, 958`, `Upscaling.cpp:2088-2096` | snapshot id and render frame |
| Test steps count Presents | `Testing/Steps.cpp:23-25, 64, 165, 325-402` | game-frame steps; shots by snapshot id |

## Test harness

- Steps run on the game thread and count game frames (snapshots). `coc`, load and console commands stay there.
- Shots, `expect.live_frames` and `GraphFrames` count rendered frames and name the snapshot id they rendered. A step that
  waits for a picture waits for "the first render of snapshot N or later".
- New configs for the cadence test mode (repeat and drop) join the regular suite from phase 3. A config that runs the game
  update deliberately slower and faster than the render thread joins after phase 6.

## Order and dependencies

```
phase 0 (contract, detector, cadence mode)
  -> phase 1 (one capture point)  -> phase 2 (no bound-state reads)
  -> phase 3 (cadence independence, needs 0's cadence mode; can overlap 1-2 per feature)
  -> phase 4 (UI stream, ImageSpace bookkeeping, latches as data; needs 2)
  -> phase 5 (lifetimes, retire queue)
  -> phase 6 (split: native, then guest)
```

Phases 1 to 3 are per-feature work and can run as parallel leads by feature group. Phase 4's Scaleform stream is the
longest single item and can start after phase 0. Phase 6 is small if the others are done, which is the point of the
order.

## Decisions for the user

1. **Pacing.** Proposed: the render thread renders each new snapshot once and skips a frame when there is none; the game
   waits when two snapshots are unconsumed. The alternative is re-rendering the newest snapshot when the game is slow
   (smoother ImGui and FG cadence, but it costs GPU time and needs zero-motion repeats).
2. **Menu scenes, precipitation, blood, decal tests.** Proposed: latches made into self-contained data first, native ports
   later. The alternative is native ports before the split.
3. **Scaleform.** Proposed: record and replay the GRenderer stream. The alternatives are a full native UI renderer, or
   running GFx's display on the render thread (it shares state with game logic, so unlikely).
4. **Guest mode.** Proposed: split native first; guest after an inventory of the engine's immediate-context uploads.

## Investigation notes

The three inventories behind this roadmap (2026-11-25): engine render work still on the main thread, live engine reads in
CS by feature (with capture-point, record-time and bound-state categories), and frame-cadence dependencies. Their file
references are folded into the tables above. The per-feature live-read counts (raw grep, relative size only): DCLF 324,
Effects11 133, PostProcessing 66, PhysicalSky 52, Upscaling 50, WetnessEffects 34, LightLimitFix 25, SkySync 24,
TerrainBlending 20, HDR 18, GrassOptimizations 17, ExponentialHeightFog 16, Skylighting 15, UnifiedWater 13,
ViewAtmosphere 12. DCLF's are mostly at capture points by design.
