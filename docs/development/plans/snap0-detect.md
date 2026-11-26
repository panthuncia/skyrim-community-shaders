# Snapshot roadmap phase 0: the detectors (lead `detect`)

Items 2 and 3 of phase 0 in [render-thread-snapshots.md](render-thread-snapshots.md): the live-read detector and the
engine-render detector. They are the progress metrics of phases 1, 3, 4 and 5.

## How to use them

- Both count in every build. `CS_SNAPSHOT_DETECT=1` (cs-test: `--set env.CS_SNAPSHOT_DETECT=1`) installs the engine call
  sites and logs both tables after each load (first world frame after a loading screen) and at a test run's exit. The test
  step `snapshot:detect` logs them on demand.
- `CS_ENGINE_ZONES=1` installs the call sites without the log (Tracy zones in Tracy builds, as before).
- The exit reports go to the status file: `report.engine_reads` and `report.engine_render`. cs-test's `result.json` has a
  `detectors` summary and the summary line ends with `(live engine reads N/world frame; engine render calls M/world frame)`.
- Log lines: `[Snapshot] live engine reads ...` (totals), `[Snapshot] live reads by feature: ...`, `... by object: ...`,
  `[Snapshot] live read site: <path>:<line> <object> <category>: per world frame X; render.world=N other.loading=M ...`,
  `[EngineZones] engine render calls per world frame: N` (the line to assert on), `[EngineZones] engine call site: ...`.

## The live-read detector (`src/Snapshot/EngineReads.{h,cpp}`)

- `CS_ENGINE_READ(Object, Category, expr)` evaluates `expr` exactly as written (an immediately invoked lambda returning
  `decltype(auto)` of `(expr)`), after counting one read on a `static constinit Site` owned by that call site. A site is
  listed on a lock-free list at its first read. Cost: one relaxed atomic add, a thread-local load, a TEB thread id read.
- `CS_ENGINE_READ_MARK(Object, Category)` counts a read with no single expression (the bound-state markers).
- `CaptureScope` (thread-local depth, nests): reads inside it count as captured, not live. The contract lead's
  `Snapshot::Capture()` opens one (one line, added at merge). DCLF's `FrameGlobals::Capture` and `FrameGlobals::Scope` open
  one too.
- Live reads split by thread (the thread that began the current frame vs any other) and frame kind
  (`RenderGraph::Frame::Kind`, set at each Begin from `FrameAnchors::BeginFrame`).
- DCLF's unscoped-read counter is folded in: `FrameGlobals::Current()` read off the render thread with no scope counts on
  an `Object::FrameGlobals` site; `TakeUnscopedReads()` reads that site's delta for the scene-frame report.

### Coverage

The replacement was done with a script (`wrap_reads.py`, kept in the scratchpad) over `src/`:

- every read of `globals::game::{shadowState, graphicsState, renderer, smState, tes, ini*/gameSettingCollection, utilityShader,
  currentPixelShader, currentVertexShader, stateUpdateFlags, player, sky, ui, calendar, imageSpaceManager, bEnableLandFade,
  bShadowsOnGrass, shadowMaskQuarter, currentAccumulator, cameraNear, cameraFar, deltaTime, bEnableVolumetricLighting}`;
- `globals::game::{perFrame, mappedFrameBuffer, frameBufferCached}` as bound state;
- `RE::<X>::GetSingleton()` for PlayerCharacter, RendererShadowState, TES, ImageSpaceManager, TESDataHandler, UI,
  PlayerCamera, BSGraphics::State, BSShaderManager::State, BSGraphics::Renderer, ProcessLists, MenuTopicManager,
  TESWaterSystem, Sky, Calendar, BSUtilityShader, BSTimer, MenuControls, the INI and game setting collections, DrawWorld,
  BGSGrassManager;
- `RE::Main::WorldRootCamera()`, `RE::Main::WorldRootNode()`, `RE::GetSecondsSinceLastFrame()`,
  `RE::BSTimer::QGlobalTimeMultiplier[Target]()`, `RE::GetINISetting("...")`;
- bound-state markers at DCLF `CaptureBindings`, `BoundPixelBuffer`, `ResolveHeader`, `BoundTargetFormats`, the engine
  constants' `Capture`, ViewAtmosphere's inherited SRVs, PhysicalSky's VS b6 and samplers, ColorGrading's VS b1.

Not covered (decided by the agent, to confirm):
- Reads through a cached pointer: `static auto* s = RE::GetINISetting(...)` counts the lookup once, not each later
  `s->GetFloat()`. Likewise locals (`auto renderer = ...`) count once per function entry, not per member read.
- DCLF's raw REL global reads (`Global<T>(address)` in `FrameGlobals::Capture` and the address tables) are not wrapped: most
  run inside the capture. Engine-object members reached through scene-graph pointers are not counted.
- Reads in `src/Globals.cpp` (the singletons' setup) and namespace-scope initialisers.

### Categories

Assigned per site by the script from the enclosing function's name and file (decided by the agent, to confirm; cheap and
wrong in places, edit a site's macro argument to fix it):
- `CapturePoint`: thunks, files named *Hook*/*Capture*/*Latch*/*Tap*, functions named capture/latch/tap/sample,
  SharedData, Setup{Material,Geometry,Technique}.
- `Other`: settings, load/save, init/install/setup/create/reset/clear, menus, ImGui, editor, registration.
- `RecordTime`: everything else in feature code (the default: unknown helpers count as phase 1 work).
- `BoundState`: the bound-state markers and the frame buffer copies.

## The engine-render detector (`src/EngineZones.{h,cpp}`)

- The call-site thunks now count in every build (Tracy zones stay in Tracy builds). Each site counts entries per frame kind.
  Sites have a role: `render` (an engine render function itself) or `helper` (bookkeeping around it). The per-frame total
  counts render-role entries between two Begins and is kept per frame kind (frames, total, max, frames with any).
- The table is generated by `perf-tools/gensites2.py` (Ghidra over HTTP, both programs): every direct call in the frame's
  render functions (DrawFrame, RenderScene, RenderPlayerView, RenderWorld, RenderPostProcess, shadow caster selection,
  RenderDepth, RenderFirstPersonView, RenderWaterEffects, RenderShadowMaps, TESWaterSystem::Update,
  TESWaterReflections::Update, the accumulator's pre and post resolve, SetupSunAndSky, RunLoadingScreen) plus every direct
  caller of UI3DSceneManager::Render (inventory, MistMenu, lockpick, stats, book, journal), LocalMapRenderer::Render,
  MenuManager::DrawInterface, Renderer::Begin (loading screen, Bink), the precipitation render, VolumetricLighting::Render,
  shadow caster selection, the three ImageSpaceManager entry points and DecalTest. Same skip rules as before.
- SE: each AE function's call list is aligned with its SE function's (pairs from CS and CommonLibSSE-NG, plus id-delta
  guesses checked by the alignment). A site with no confident match has SE id 0 and is skipped on SE; the install line says
  how many ("the detector covers only part of the frame (AE-only sites)"). AE versions other than 1.6.1170 get nothing.
- A `hooked` site's call already goes to a CS or plugin hook: the engine called it, but the hook may not run the engine's
  function. It still counts: the engine still drives that step.

## Baseline (2026-11-26, Dev-Fast, autoloaded save, quick config)

These are the starting numbers of phases 1 and 3-5. All three modes still drive the frame from the engine's hooks, so they
read alike. The world-frame count varies with the scene (actors near the player raise DCLF's pass-capture reads), so compare
runs of the same save: nvk1 and nvk2 differ by 270 reads per frame with the same build and save.

Live engine reads per world frame (outside any capture scope):

| Run | Total | capture-point | record-time | bound-state | other |
|---|---|---|---|---|---|
| guest (s0-detect-guest3) | 1619 | 639 | 836 | 93 | 51 |
| native Vulkan (s0-detect-nvk2) | 1848 | 818 | 877 | 104 | 49 |
| native D3D12 (s0-detect-d3d12) | 1570 | 613 | 816 | 91 | 49 |
| guest, SE 1.5.97 (s0-detect-se-guest2) | 1219 | 284 | 797 | | |

Native Vulkan by feature (per world frame): DrawcallLimitFix 930 (capture-point 714, record-time 183, bound-state 19),
RenderGraph 371 (record-time 283, bound-state 70), Utils 170 (record-time), State 95 (capture-point 41, record-time 51),
CloudShadows 54, LightLimitFix 31, Upscaling 24, LinearLighting 24, PhysicalSky 14, Deferred 13, UnifiedWater 13,
TerrainShadows 10, TerrainBlending 10, GrassCollision 7, WetnessEffects 7, Skylighting 7, EngineFixes 6, HDRDisplay 5, the
rest under 5 each.

By object: Player 421, Renderer 325, ShadowState 279, Sky 166, ImageSpace 115, Data 106, BoundState 103, TES 85,
GraphicsState 79, ShaderManagerState 57, Shaders 42, UI 29, Accumulator 16, Camera 13, Timer 5, Calendar 5, Water 3.

Largest sites: DCLF `PassCapture.cpp:235` (the player, per registered pass, 400), `EngineDrawFamily.cpp:386` (shadow
state, 108), `SceneCapture.cpp:769` (game data, 103), `EngineTargetSet.cpp:194` (the renderer, 80), `State.cpp:137` and
`CloudShadows.cpp:256` (the sky, 51 each), `ImageSpace.cpp:564` (51), `Utils/Game.cpp:187` (45), `WaterNative.cpp:353`
(45), `Skeleton.cpp:585` (41).

Engine render calls per native world frame: 74 in native Vulkan (max 110), 71 in native D3D12, 70 in guest, 71 on SE (196
of 252 sites). Main menu 22.9 per frame; loading frames 11.2. Per world frame, by site (native Vulkan, render functions):
- once per frame: Main::DrawFrame, RenderScene, RenderPlayerView, SetupSunAndSky (hooked), RenderWaterEffects,
  TESWaterSystem::Update, its displacement render, TESWaterReflections::Update (hooked), CalculateAndDrawShadowCasterLights
  (hooked) with BSShadowDirectionalLight::Select and CalculateActiveShadowCasterLights, RenderShadowMaps (hooked),
  RenderShadowMasks, FirstPersonCulling, RenderDepth (hooked), VolumetricLighting::Render, RenderPrecipitation (hooked),
  RenderPostProcess, RenderEndOfFrameEffects (hooked), RenderReflections, the water blend, SSAO twice, DispatchCopySubRegion,
  StartAccumulating, SubmitAccumulator, FinishAccumulatingPostResolveDepth, BSCullingProcess::Process (RenderWorld),
  Sun::SubmitOcclusionQuery, JobList::Finish four times, JobListManager::FrameJobs, FaceGen::UpdatePending,
  MenuManager::DrawInterface;
- RenderDepth's second StartAccumulating 3 per frame (the shadow-map passes);
- the accumulator's pre/post resolve 1.9 per frame each, with 17 RenderBatches/RenderGeometryGroup/decal calls each time;
- UI3DSceneManager::Render from MistMenu 0.33 (frames just after the load, counted as world).

## Counting cost

- Live reads (always on): a microbenchmark of the same code shape costs 23-26 ns per read at /Od and 4.5-13 ns at /O2
  (uncontended). At 1100-1850 reads per world frame that is 25-50 us per frame in Dev-Fast, 5-25 us in Release.
- The call-site counting (the switch): native-vulkan+perf-quick, 900 frames between the marks, one sample each (Dev-Fast):
  switch off 167.5 fps (s0-detect-perf-off), on 164.5 fps (s0-detect-perf-on), about 0.1 ms, within run-to-run noise.

## Runs (cs-test, stage s0-detect, Dev-Fast, all with CS_SNAPSHOT_DETECT=1 unless noted)

| Config | Label | Result |
|---|---|---|
| guest+quick | s0-detect-guest2, s0-detect-guest3 | passed |
| native-vulkan+quick | s0-detect-nvk1, s0-detect-nvk2 | passed |
| native-d3d12-quick | s0-detect-d3d12 | passed |
| native-vulkan+perf-quick | s0-detect-perf-off (switch off), s0-detect-perf-on | passed |
| guest+quick on SE 1.5.97 (SETest, TressFXSSE off) | s0-detect-se-guest2 | passed (s0-detect-se-guest crashed in TressFXSSE.dll at load, the known SE trap) |

The runs used this worktree's `tools/cs-test/cs_test.py` (with the main checkout's lab helper copied in): the main
checkout's switch schema does not know `CS_SNAPSHOT_DETECT` yet and rejects it. After the merge the main script works.
The SE run added `*TerrainHelper.esp` to the SE profile for the run and restored the profile after.

## Open items

- The contract lead's `Snapshot::Capture()` needs `const EngineReads::CaptureScope scope;` at its top once both branches
  are merged (it leaves a comment there).
- The after-a-load table is cumulative since start; its per-world-frame figure covers one frame. The exit table is the one
  to read.
- 54 call sites have no SE match (RenderShadowMaps' internals, most of SetupSunAndSky, part of shadow caster selection,
  RenderDepth and the water reflections). SE counts are a subset.
- The categories need a hand pass when phase 1 starts on a feature: unknown helpers default to record-time.

## Decisions to confirm

- `CS_ENGINE_ZONES` is kept beside the new switch: perf tooling sets it for Tracy captures. `CS_SNAPSHOT_DETECT` implies it.
- Categories are a name heuristic (above).
- The live-read table is logged at a load and at exit of test runs only (`Testing::RegisterExitReport`), not at every
  process exit: logging from process teardown is not safe. Outside cs-test use the `snapshot:detect` command or a load.
- The call site counts "the engine called this", not "the engine's function ran" (see `hooked`).
