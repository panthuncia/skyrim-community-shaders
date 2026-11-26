# Native rendering on its own thread: roadmap

Status (2026-11-26, late): phases 0, 1 and 2 done (`snap0-*.md`, `snap1-*.md`, `snap2-*.md`). Phase 3: game-side side
effects, stealth lights, native shadow casters, the main sky and water from game data, no main registration jobs and no
engine list culls in native (`snap1-sidefx.md`, `snap3-lights.md`, `snap3-scene.md`, `snap3-lists.md`, `snap3-culls.md`);
native Main::RenderWorld with no resolve function and screen blood (`snap3-rworld.md`, `snap3-resolve.md`, `snap3-engstate.md`, `snap3-accum.md`); left: decision 127. Phase 4 done in-frame: native makes no game UI renderer call
(`snap0-ui.md`, `snap4-ui2.md`). Phase 5: ImageSpace selection and constants, the TAA reset, vanilla and save screenshots,
and every frame kind as a snapshot are done (`snap5-imagespace.md`, `snap5-isrest.md`, `snap5-kinds.md`); menu scenes and
the image-based lens flares are left. Phase 6 part 1 (the retire queue outside DCLF) is done (`snap6-life.md`); part 2
(DCLF) is designed. Phase 7 is prepared and planned (`snap7-prep.md`). Live engine reads in native Vulkan's world frame:
1,848 at the phase 0 baseline, about 560-780 now (scene-dependent); engine render calls per native world frame: 74 at the
baseline, about 20-33 now.
Every decision since the roadmap, with the
side-effect fates, is in [render-thread-decisions.md](render-thread-decisions.md); add new ones there. It follows round 3
of the render-thread work (`render-thread-window.md`) and the fully native direction.

> The round notes, older handoffs and plans this file cites (`render-thread-window.md`, `perf2-*.md`, `perf3-*.md`,
> `handoff-2026-11-25.md`, `org-native-frame-graph.md`, `org-full-port.md`, ...) were removed from the tree in the docs
> cleanup of 2026-11-26. Read any of them with `git show 13403653f:docs/development/<path>`, for example
> `git show 13403653f:docs/development/plans/perf3-guards.md`.

## Goal

Skyrim's main thread runs the game update and then its render half. Today CS's whole frame runs inside that render half,
driven by engine hooks. The goal, for native mode:

- Everything that contributes to a produced frame becomes part of a **frame snapshot**: immutable data, built on the game
  thread from game data, with no pointers into live engine state that the game may change.
- The game thread **publishes** a snapshot whenever it has new data (latest wins). It never waits for the render thread.
- A **CS render thread** runs as fast as the GPU allows. At the start of each of its frames it swaps in the newest
  snapshot (the same one again if nothing new was published), records, submits and presents. It never reads engine
  memory, never calls the engine, and never waits on the game.
- Native **ignores the game's renderer completely**. CS drives every part of the frame itself, from game data. No engine
  render function runs in native mode: no RenderPlayerView, accumulators, shadow caster selection, ImageSpace, Scaleform
  renderer or menu scene renderer.

This generalises DCLF's scene snapshot (`Scene::FrameInputs` in, `ScenePublication` out, adopted at frame start) to the
whole frame, and DCLF's own scene pipeline to everything the engine's render half still does.

## Decisions (user, 2026-11-25)

1. **Pacing: fully asynchronous.** The render thread renders as fast as the GPU allows; the game thread publishes when it
   has new data. Neither waits on the other. Repeated snapshots are the normal case when the game is slower, and skipped
   snapshots when it is faster.
2. **Port first.** Every remaining engine render action becomes a native port. Capturing what the engine does (latching
   its draws, reading what it bound, calling its render functions for their side effects) does not count. The goal is
   for native to drive everything without the game's renderer.
3. **Scaleform: a full native UI renderer**, a 1:1 port of the game's own Scaleform renderer: the same shaders, the same
   calls, as standard ORG/BasicRHI passes.
4. **Guest mode is out of scope.** It stays as it is, on the main thread, for compatibility with mods that draw with
   D3D11 and must be ordered with CS's draws. Only native moves to the render thread. Shared feature code must keep
   working in guest (below).

## Where things stand

The graph modes already keep the engine's GPU work out of the frame: the engine's D3D11 context is a Tripwire handle that
keeps binds and drops draws (`src/RenderGraph/Engine/Tripwire/Tripwire.h:22-31`). What remains:

1. **The engine still drives the frame.** Every CS stage runs because the engine reached a call site (Renderer::Begin,
   Main::Draw, RenderShadowMaps, RenderWorld, the blended decals, the cube map cameras, first person, post processing,
   DrawInterface, Present). Anchor client `commit()` callbacks and `State::UpdateSharedData` run at those moments, so the
   capture points are scattered across the engine's render sequence.
2. **The engine's render half still produces inputs CS uses**: culling and scene-list jobs (DCLF's PrimaryCull and
   PassCapture consume them), pass registration, light selection, ImageSpace effect selection and constants, Scaleform's
   renderer calls, menu scene draws, and the latched families.
3. **CS still reads live engine state while it records.** About 25 features read the sky, weather, TES, the player, the
   camera, the shadow state, timers and INI values from inside hooks and record code. Several read state the engine bound
   to the D3D11 context.
4. **Things assume one rendered frame per game frame**, in lockstep, on one thread: frame counters used as identity,
   temporal effects, game-time integrators, readbacks written back to the engine, load holds, the test harness.

Engine render work still running in native, from the inventory:

| Engine work | Today | Port |
|---|---|---|
| Scaleform HUD and menus (`MenuManager::DrawInterface`, 0.042 ms Release) | GFx calls the game's GRendererD3D1x; CS swaps its vtable slots and records into the Interface anchor (`src/RenderGraph/Engine/Scaleform/Renderer.cpp:38-60`) | Large: native UI renderer (phase 4) |
| Menu scenes: inventory 3D, MistMenu (main menu, loading model), lockpick, book, journal, local map | latched at DrawTriShape (`Features/MenuViews/MenuScenesCapture.cpp`); `UI3DSceneManager::Render`, `LocalMapRenderer::Render` run | Large: a second scene renderer and a mini post chain |
| Precipitation, screen blood, decal occlusion tests | the engine's batch and setup run, CS latches the draw (`Core/Engine/Family/EngineDrawFamily.h`) | Medium each |
| ImageSpace: the world chain's selection and constants come from the snapshot's ImageSpace block (snap5-imagespace.md); left: the render-half reads (refraction, VL activity and colour, depth of field camera, water blend camera, portal fog), the menu chains, the frozen-background composite, DrawFrame's PrintScreen copies, `ISDownsampleCS` | the world chain draws natively from game data; the engine's render half still calls RenderSSAO, the reflections pass, the water blend and RenderPostProcess (phase 3) | Medium |
| Volumetric Lighting with the CS feature off | the engine's descriptor, `VolumetricLighting::Render` | Small to medium |
| Shadow maps: caster light selection (`CalculateAndDrawShadowCasterLights`), lights not fully covered (`ShadowMapsNative.cpp:271-294`) | engine selection; partial native; engine `Render` fallback | Medium |
| Sky and water batch calls, `TESWaterReflections::Update` cube cameras | called for the engine's cleanup and inputs | Medium |
| Accumulators, `BSCullingProcess`, scene-list jobs, `JobList::Finish` | run; DCLF consumes them | Large: DCLF culls and builds its lists from the scene graph itself |
| Loading screen and startup logo | rendered on the serving/MoviePlayer thread under `Renderer::Lock` | Medium |
| Bink | decode tick only; CS draws the tiles | Small |

## The engine render half's side effects on the game

Once native skips the engine's render half, anything the game itself reads from it has to be produced by CS on the game
thread. This is the less visible half of "port first", and it needs its own inventory (phase 0). Known so far:

- **Visibility.** Animation and AI update rates depend on per-object visibility the cull writes (`kAccumulated`). DCLF
  already writes it back from its GPU visibility readback (`Core/Render/Readback/Visibility.cpp:859-886`, applied at
  `Main::Update`), and actor rate likewise (`ActorRate.cpp`).
- **Fades.** DCLF's fade results are written to engine nodes at `Main::Update` (`Fades/FadeWriteBack.cpp`).
- **The sun occlusion query.** The engine's `GetQueryData` consumes the answer game-side (sun glare, lens flare); CS fills
  the query ring today (`Features/Sky/SkyHooks.cpp:466-508`).
- **Shadow light bookkeeping.** Light descriptors, slices, the free mask, light transforms, the accumulate array, and
  `State::SetCameraData` (`ShadowMapsNative.cpp`, `Core/Handoff/ShadowViews.cpp:411-469`). The question for each is
  whether game code reads it, or only the engine's renderer does.
- **Water.** `TESWaterSystem::Update` assigns water normal textures; `TESWaterReflections::Update` builds the cube cameras.
- **Facegen and decals.** `FaceGen::UpdatePending` (head texture baking) and the decal occlusion tests (decal placement)
  run in the render window.
- **Jitter and projection.** `PostChain::UpdateJitter` writes `projectionPosScale` into `BSGraphics::State`
  (`RenderGraph/Frame/PostChain.cpp:241-268`); game-side picking or culling that reads the projection must see an
  unjittered one.
- **Frame pacing.** Today the engine's Present blocks, which paces the game loop. With an asynchronous render thread, an
  update-rate cap setting (default 60 updates per second) paces the game thread instead: its Present call becomes
  "publish, then wait for the next update slot".

## Shared code and guest mode

Guest keeps today's model: the engine drives the frame on the main thread and CS's passes order with D3D11 mods' draws.
Feature code is shared between the modes, so the snapshot model is the one model for both, with two producers:

- **Native:** the game thread builds the snapshot from game data, publishes it, and the render thread consumes it later.
- **Guest:** the engine's hooks fill the same snapshot blocks at today's capture points (including reads of bound state,
  which guest needs for its ordering), and the frame consumes them at once on the same thread.

Feature code records only from snapshot blocks in both modes. Capture code splits into a native capture (from game data)
and a guest capture (from hooks) where they differ. Native-only ports (the UI renderer, menu scenes, ImageSpace, culling)
leave guest's capture paths in place.

## Phases

Phases 0 to 5 land in today's single-threaded native frame and are testable there. The thread split comes last, when no
native code needs the main thread at render time.

### Phase 0: the snapshot contract, detectors, and inventories

- Define `FrameSnapshot`: a snapshot id, game time and game delta, frame kind (world, menu over world, main menu, loading,
  movie), load epoch, menu states, settings revision, cameras (current and previous, per view: main, cube faces, shadow
  views), the scene publication it pairs with, one typed block per feature, the UI command stream, and the events since
  the previous snapshot. It is immutable once published. Blocks are typed structs; ORG latch regions are filled from
  them at record time.
- A **live-read detector**. Generalise `FrameGlobals::TakeUnscopedReads` (`Core/Shading/FrameGlobals.h:106`): route the
  engine singletons CS reads (sky, TES, player, camera, shadow state, renderer data, timers, INI) through accessors that
  count, per call site, any read outside a capture scope. A dev build logs the table at exit. This is the progress metric
  for phase 1 and a regression guard.
- An **engine-render detector** for native: count every engine render function still entered in a native frame (the
  engine zones table, `src/EngineZones.cpp`, already wraps the call sites). Phase 4 drives it to zero.
- A **cadence test mode**: render each snapshot several times, or drop snapshots, still on one thread. With fully
  asynchronous pacing, repeats and drops are the normal case, so this mode is what proves phases 1 and 3.
- The **side-effect inventory** (section above): every engine render-half write the game reads, with who reads it.

### Phase 1: one capture point per frame

Turn the scattered captures into one game-side capture that runs before any recording, and make recording read only
the snapshot.

- `State::UpdateSharedData` (`State.cpp:1668`, called three times from Deferred and once from Upscaling) becomes a capture
  of its inputs (sun light, DALC, camera data, timer, water tiles, sky flags, sun and moon) plus a pure function of them.
  Features' `ModifySharedLighting` follows.
- Each anchor client's `commit()` splits into `Capture(SnapshotBuilder&)` and a fill from the snapshot block at record
  time. Largest first: PhysicalSky and VolumetricClouds, Effects11, WetnessEffects, SkySync, Upscaling, LightLimitFix
  (already has `ORGLightCulling::FrameInputs`), PostProcessing (CinematicCamera, HistogramAutoExposure),
  ExponentialHeightFog, ViewAtmosphere, Skylighting, TerrainShadows, CloudShadows, VolumetricLighting, InteriorSun,
  UnifiedWater, TerrainBlending, GrassOptimizations, SSS, ScreenSpaceShadows, VolumetricShadows, DynamicCubemaps,
  TreeWind.
- Remove engine reads from graph predicates and record code: `globals::state->frameCount` in `active()` and commits
  (ORGPhysicalSky, ORGVolumetricClouds, ORGSkylighting), `renderer->GetDepthStencilData()` (ORGSkylighting,
  ORGVolumetricShadows, ORGViewAtmosphere, Deferred), and function-local `static RE::GetINISetting` values read live
  (Upscaling, PostProcessing, SkySync). INI values and CS settings reach the frame only through the settings revision.
- Water's `WaterPort::CaptureFrame` and LLF's FrameInputs fold into the snapshot as blocks.
- Native capture code reads no engine-bound GPU state: DCLF `CaptureBindings` (`Core/Render/Resources/FrameSlots.cpp:177-213`),
  `BoundPixelBuffer`, `ResolveHeader`, `BoundTargetFormats`, `frameBufferCached` (`Globals.cpp:309`), `EngineConstants`,
  ViewAtmosphere's inherited SRVs, PhysicalSky's VS b6 and samplers, ColorGrading's VS b1. Native computes these from game
  data (most of them are camera, fog, lighting and shadow constants the engine itself computed from game data); guest keeps
  its captures.

Done when the live-read detector shows no read during recording in native's world frame, and native's capture reads no
bound state.

### Phase 2: cadence independence

Separate **game identity** (snapshot id, game time, game delta) from **render identity** (render frame index, render
delta). With asynchronous pacing every item here is exercised constantly, not only under load.

- **Counters.** `FrameClock` (`RenderGraph/FrameClock.h`) stays the render counter: slot rotation, latch regions and
  published copies are render-side. Anything meaning "a new game frame" keys by snapshot id: DCLF's `SceneStep` and the
  600-frame scene-pump watchdog (time-based instead), `State::Reset`'s `frameCount` and timer (`State.cpp:413-448`: split
  into the game-side timer and menu capture and the render-side counters and feature resets), the profiler's idle frames,
  pacing statistics.
- **Repeated snapshots** (the game is slower than the render thread):
  - Jitter is render-owned and advances every render frame. The render thread applies it to the snapshot's projection.
  - A repeated snapshot renders with zero motion. Histories (TAA and upscalers, SSGI's `prevInvView`, SSR, clouds) keep
    accumulating, so a still frame converges.
  - Purely visual animation (glare, adaptation, cloud scrolling, water flow, wind sway if it is visual only) advances by
    render delta, so it stays smooth between snapshots. World state (wetness, lightning, weather) comes from the game in
    the snapshot.
  - DCLF fades step once per snapshot (`Features/Fades/FadeState.cpp:257-319`), never on a repeat.
  - Auto exposure adapts by render delta, not `BSTimer::realTimeDelta` (`HistogramAutoExposure.cpp:80`).
- **Skipped snapshots** (the game is faster):
  - "Previous" transforms for motion vectors are the last ones this render thread rendered, kept per object key on the
    render side, not the snapshot's own previous (DCLF's `DrawnPreviousWorld`, sky's `previousPosAdjust`, water).
  - Events since the last snapshot (camera cut, load started, teleport, lightning flash, first frame after load, menu
    opened) are OR-merged and counters accumulated until a render frame consumes them. A cut resets histories even if the
    snapshot that carried it was never rendered.
- **Readbacks to the game.** Visibility, actor rate, the sun query, fade write-back: results carry the snapshot id they
  were rendered from; the game applies the newest, and applying one twice is harmless. With many render frames per
  snapshot, the game reads each result once.
- **No render-side writes into engine objects.** The writes listed in the side-effect section move to the game thread or
  disappear with the port that makes them unnecessary.

Done when the cadence test mode (several renders per snapshot; dropped snapshots) passes the cs-test suite with matching
shots and no fade, jitter, motion or exposure artefacts.

### Phase 3: DCLF and the scene without the engine's render half

DCLF already draws the world natively but still consumes the engine's render-half work. Remove that dependency:

- Culling and scene lists from the scene graph: DCLF's own traversal and cull replace `BSCullingProcess`, the accumulators'
  pass registration (`PassCapture`) and the scene-list jobs (`JobList::Finish`). DCLF's CPU pipeline and GPU culling are
  the base.
- Shadow casters: native light selection and caster lists replace `CalculateAndDrawShadowCasterLights`; every light is
  native (no engine `Render` fallback).
- Reflection cube cameras from the water data, not `TESWaterReflections::Update`. Sky and water draw from game data
  without the batch call-throughs.
- Precipitation, screen blood and decal occlusion tests as native passes from game data.
- Volumetric Lighting's vanilla path as a CS pass (or the CS feature as the only path).
- The game-visible side effects (visibility, fades, sun query, decals, facegen) produced by CS on the game thread.

Done when the engine-render detector shows no world-frame engine render call in native.

### Phase 4: the native UI renderer (Scaleform)

A 1:1 port of the game's Scaleform renderer (the GRendererD3D1x implementation in the executable):

- CS implements the GRenderer interface that GFx calls on the game thread. Every call (`BeginDisplay`, `SetMatrix`,
  `SetCxform`, `FillStyle*`, `LineStyle*`, `DrawIndexedTriList`, `DrawLineStrip`, `DrawBitmaps`, `BeginSubmitMask`,
  `PushBlendMode`, render-target and filter calls, texture creation and glyph-cache updates) is recorded, with its data
  copied, into the snapshot's UI command stream in call order.
- The render thread replays the stream as ORG/BasicRHI passes with the same shaders (the game's Scaleform shader
  bytecode, ported the way CS ports the vanilla shaders), the same vertex formats, blend and stencil states, and the same
  draw sequence. Texture creation and updates become uploads in stream order; textures are owned by the UI renderer and
  live as long as GFx holds them.
- Parity method as for the sky and ImageSpace: an observe build compares every replayed draw with the game renderer's
  draw (shaders, constants, vertex data, states) before the switch. The game renderer is then not called in native.
- The loading screen and startup logo (serving/MoviePlayer thread) publish UI-only snapshots through the same channel.

### Phase 5: menu scenes, ImageSpace and the remaining frame kinds

- **Menu scenes** (inventory 3D, MistMenu, lockpick, book, journal, local map): a native scene path for UI3D scenes and the
  local map, from the menu's scene graph and camera, with its own small post chain. DCLF's scene pipeline is the base.
- **ImageSpace**: effect selection and parameters from game data (the active ImageSpace modifiers, weather and
  cinematics), every effect's constants computed by CS, PreRender/PostRender side effects reproduced or found unneeded.
  The menu end-of-frame chains follow.
- **Frame kinds**: main menu, loading, menu over world, frozen menu and Bink as snapshots of their own kind, with no engine
  render call.

Done when the engine-render detector reads zero in every native frame kind.

### Phase 6: snapshot lifetimes and engine resources

- Snapshots reference engine-owned objects (geometry buffers, textures, materials). Each snapshot holds references, and a
  **retire queue** returns them to the game thread once the render thread has retired the snapshot (its GPU work done).
  An unrendered snapshot replaced by a newer one retires at once. DCLF's `EngineReleases` and held detaches are the existing
  form of this; generalise them.
- In-place engine writes to resources a snapshot may still read get a new version; the old version retires with the
  snapshots that use it (example: the water normal textures `TESWaterSystem::Update` assigns, which the SWF hook reports).
- DCLF's `EngineReadWindow` (`Core/Engine/EngineReadWindow.h`) becomes the game-side capture window: it opens after the
  update and closes before the next one. The render thread never takes a lease.
- Engine data copied for the frame (constant mirrors, the dynamic ring's contents) lives in snapshot-owned memory.

### Phase 7: the thread split (native)

- **The render thread.** CS creates it. It owns ORG's host, the swapchain and present, and the frame's submission. ORG's
  stream and submission threads already exist; `ClaimRenderOwnership`/`MoveOwnerThread` (`FrameAnchors.cpp:1955-1975`,
  `RenderGraphRuntime.cpp:2708-2716`) and the loading-screen thread are the precedent for moving ownership.
- **Publication.** The engine's Present call on the game thread publishes the snapshot and returns at the next slot of the
  update-rate cap (a CS setting, default 60 updates per second). Revisions and graph installs adopt at the render
  thread's frame start, as they do at Begin today.
- **Pacing.** None between the threads. The render thread's own present mode (vsync, a CS frame limiter, frame
  generation) sets its rate.
- **Frame kinds.** `DecideKind` (`Frame/FrameDriver.cpp:172-177`, which treats any frame off the main thread as Loading)
  moves into the capture. The load hold counts snapshots and load epochs, not Presents.
- **Input and UI.** Window messages stay on the game thread. ImGui input reaches the render thread through a queue; ImGui
  draws on the render thread. Settings changes go through the revision lane. Swapchain resize is a published event.
- **Frame generation and Reflex.** Frame generation multiplies the render rate, interpolating between rendered frames.
  PCL markers carry the snapshot id (simulation) and the render frame (submit, present).

## Watch list: frame-cadence assumptions found

| Assumption | Where | Rework |
|---|---|---|
| FrameClock slot = frame % 4, moves at Renderer::Begin | `RenderGraph/FrameClock.h:15-27`, `RenderGraphRuntime.cpp:2516-2521` | render-only; add snapshot id |
| State::Reset after every Present: frameCount, timer, menu flags, feature resets, profiler | `State.cpp:413-448`, `Hooks.cpp:730-735` | split game and render halves |
| Present hook order: Present anchor, DCLF BeforePresent, pacing, seal, constant and buffer EndFrame, State::Reset, test steps | `Hooks.cpp:586-743` | render-thread frame end; game-side publish |
| DCLF FrameDriver BeginFrame and EndFrame mix game and render work | `Core/Render/FrameDriver.cpp:164-398` | capture half and consume half |
| SceneStep and the 600-frame scene-pump watchdog | `Core/Engine/SceneClock.h`, `DrawcallLimitFix.cpp:344-354` | snapshot id; time-based watchdog |
| Jitter written into engine state at Begin; Upscaling uses frameCount | `Upscaling.cpp:549, 1425-1431, 2092-2104`, `PostChain.cpp:241-268` | render-owned jitter |
| Previous transforms are the previous game frame | `Core/Gpu/FrameValues.cpp:192-197`, `SkyNative.cpp:599`, `WaterPortEvaluate.cpp:410` | last rendered transforms per key |
| Auto exposure by game delta, pre-exposure by frameCount | `HistogramAutoExposure.cpp:25-117`, `State.cpp:1671-1692` | render delta, render frame |
| Skylighting quadrant by frameCount % 4 | `Skylighting.cpp:624, 643-655` | render frame |
| Game-time integrators in render code | TreeWind, Wetness, VolumetricClouds, Lightning, LODBlendRingFix, ENB adaptation and DOF, PhysicalGlare, SkySync, Skin wetness | game-side (world state) or render delta (visual) |
| DCLF fades step per frame and write back at Main::Update | `Fades/FadeState.cpp:257-319`, `FadeWriteBack.cpp` | step per snapshot |
| Visibility and actor-rate readbacks written at Main::Update | `Core/Render/Readback/Visibility.cpp:859-886`, `DrawcallLimitFix.cpp:616-620` | snapshot-tagged results |
| Render ownership claimed by whoever calls Begin; non-main frames are Loading | `FrameAnchors.cpp:1912-1975`, `Frame/FrameDriver.cpp:172-177` | explicit render thread; kind from capture |
| Revision fragments adopt only on the main thread | `Frame/Revision.cpp:1628-1629` | adopt at render frame start |
| EngineReadWindow leases from Main::Draw to Present | `Core/Engine/EngineReadWindow.h:10-24` | game-side capture window |
| Capture TLS assumes engine callbacks on one thread | `PassCapture.cpp:49-56`, `LocalLightCull.cpp:94-146`, `SunAccumulation.cpp:124` | goes with phase 3's ports |
| Load hold counts frames and keeps the Mist menu open | `Core/Render/LoadHold.cpp:30-47`, `Hooks.cpp:622-636` | count snapshots and load epochs |
| ImGui input from the input poll and WndProc, drawn at Present | `Hooks.cpp:932-970, 1072`, `Menu/OverlayRenderer.cpp` | input queue; settings via revisions |
| PCL markers set in input poll and Present | `Hooks.cpp:582-584, 958`, `Upscaling.cpp:2088-2096` | snapshot id and render frame |
| Engine Present paces the game loop | `Hooks.cpp` Present hook | game-side rate limit |
| Test steps count Presents | `Testing/Steps.cpp:23-25, 64, 165, 325-402` | game-frame steps; shots by snapshot id |

## Test harness

- Steps run on the game thread and count game frames (snapshots). `coc`, load and console commands stay there.
- Shots, `expect.live_frames` and `GraphFrames` count rendered frames and name the snapshot id they rendered. A step that
  waits for a picture waits for "the first render of snapshot N or later".
- The cadence test mode's configs (several renders per snapshot; dropped snapshots) join the suite from phase 2. After
  phase 7, configs run the game update deliberately slower and faster than the render thread.
- Each port keeps the observe-then-switch parity method used for the sky, ImageSpace and shadow maps.

## Order and dependencies

```
phase 0 (contract, detectors, side-effect inventory, cadence mode)
  -> phase 1 (one capture point; native reads no bound state)
  -> phase 2 (cadence independence; per feature, can overlap 1)
  -> phase 3 (DCLF and the scene without the engine's render half)
  -> phase 4 (native UI renderer; can start right after phase 0)
  -> phase 5 (menu scenes, ImageSpace, other frame kinds; needs 3)
  -> phase 6 (snapshot lifetimes)
  -> phase 7 (native thread split)
```

Phases 1 and 2 are per-feature work and suit parallel leads by feature group. Phase 4 is self-contained and the longest
single item, so it can run alongside 1 to 3. Phases 3 and 5 are the largest ports. Phase 7 should be small if the rest is
done.

## Settled questions (user, 2026-11-25)

1. **The game thread's rate.** A separate update-rate cap setting paces the game's update, defaulting to 60 updates per
   second. It replaces the pacing the engine's blocking Present gives today (phase 7).
2. **Frame generation** multiplies the render rate: it interpolates between rendered frames, as it does now. Snapshots
   only carry data to the render thread and play no part in timing.
3. **Visual-only animation between snapshots** (what advances by render delta, what holds until the next snapshot) is
   decided per feature when that feature is ported.
4. **Engine render-half side effects**: each write's fate (CS produces it for the game, or it goes away) is decided per
   feature when its port is investigated, from the phase 0 inventory.

## Investigation notes

The three inventories behind this roadmap (2026-11-25): engine render work still on the main thread, live engine reads in
CS by feature (capture-point, record-time and bound-state categories), and frame-cadence dependencies. Their file
references are folded into the tables above. Per-feature live-read counts (raw grep, relative size only): DCLF 324,
Effects11 133, PostProcessing 66, PhysicalSky 52, Upscaling 50, WetnessEffects 34, LightLimitFix 25, SkySync 24,
TerrainBlending 20, HDR 18, GrassOptimizations 17, ExponentialHeightFog 16, Skylighting 15, UnifiedWater 13,
ViewAtmosphere 12. DCLF's are mostly at capture points by design.
