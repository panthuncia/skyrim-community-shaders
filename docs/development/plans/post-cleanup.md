# Post-cleanup: one pass over the render-thread work

Lead: post-cleanup, branch `post/cleanup` from `org_integration` 45f386c64. Roadmap item 1 of "After phase 7" in
[render-thread-snapshots.md](render-thread-snapshots.md).

Scope: everything after adfe49df4 (the last commit before snap0), about 34k added lines in 517 source files. The audit
was split by area (snapshot core, native drivers, Scaleform and menu scenes, DCLF core, DCLF features, other features and
the editor). Each finding was checked against the code and grepped for users. Line numbers are at 45f386c64.

Risk scale: SAFE = mechanical and behaviour-preserving; MEDIUM = picks one source of truth or changes timing in a corner;
RISKY = changes what a mode draws.

## Switches and env vars

The roadmap added three switches. None of them only brings back an old path, so all stay:

| Switch | Users | Verdict |
| --- | --- | --- |
| `CS_SNAPSHOT_DETECT` (`cs.snapshot_detect`) | the live-read and engine-render detectors; cs-test `--set`; docs | keep (guards the result) |
| `CS_SNAPSHOT_CADENCE` (`snapshot.cadence`) | `cadence-*.toml`, `tools/cs-test/cadence.py` | keep (test mode) |
| `CS_ENGINE_ZONES` | Tracy zones; in a non-Tracy build it still installs the sites, whose counts reach the test status (`engine_render`) | keep; the audit's "Tracy only" idea was wrong, see below |

No `getenv`, `Switches::` or constexpr toggle was added in the RenderGraph, DCLF or feature code for A/B or fallback. The
observe scaffolding the ports used was already deleted by each lead, apart from the leftovers listed under "Done".

## Done (this branch)

Grouped as committed. All behaviour-preserving unless a line says otherwise.

### Snapshot helpers and block reads
- `Snapshot::CurrentBlock<T>()` (Publication.h) replaces the hand-written
  `snapshot ? snapshot->Block<T>() : nullptr` read in about 30 places. Deleted the per-feature wrappers that only did this:
  `PhysicalSky::Frame`, `SkySync::Frame`, `PostProcessing::FrameBlock`, the anonymous `FrameBlock` in ScreenSpaceShadows
  and TerrainShadows, `NativeBlock` in ORGVolumetricLighting. GrassCollision and Effects11 keep theirs (static empty
  fallback) on top of the helper.
- `Snapshot::StepOnce(lastId)` replaces four copies of the `steppedSnapshot` pattern (GrassCollision, PhysicalSky,
  TreeWind, WetnessEffects). Same semantics as the copies (a caller that skipped the adopting frame still steps once),
  which `GameStep()` does not have.
- `Builder::Set(T&&)` moves blocks with heap members (GrassCollision, PhysicalSky, TerrainShadows) instead of copying
  them again into the shared block. PhysicalSky's worldspace and TerrainShadows' `FrameLandWorldspace()` are const
  references now (no per-frame string copy).

### Snapshot core
- Dead snapshot fields removed: `scene` (and the `DCLF::ScenePublication` forward declaration), `realDelta`,
  `Cameras::previous`/`Previous()`, `BlockAt`, the event counters `loadsStarted`/`menusOpened`/`menusClosed`/
  `cameraCuts`/`teleports`, the event flags MenuOpened/MenuClosed/KindChanged/Paused/Resumed/SettingsChanged/
  WindowResized (nothing in src or tools read them; `windowSize` stays), `settingsRevision` with
  `SettingRegistry::Serial()`, `MenuEvents` and `FillWindowSize`.
- `Snapshot::Capture()` (no callers) deleted; `RendersThisUpdate` moved into Publication.cpp.
- The Retire lifetimes line is logged at load ends only. It was also logged every 1800 updates, and both callers
  exchanged the same counters, so each cut the other's window.
- `State::WriteCsEngineState(gameStep)`: one function for CS's per-update engine writes (LOD ring, sun mask bits,
  ApplyReflections off, improved snow off). Guest calls it from `State::Reset`, native from SideEffects; the SSS
  character light stays native-only. Both modes now label the reads `Other`.
- One `EngineZones::BeginFrame` for both detectors: it calls `EngineReads::BeginFrame` (which returns the previous kind)
  and fires one "after a load" report. `kKindKeys` lives once in EngineReads.h and the kind numbers are
  `RenderGraph::Frame::Kind` constants. Report text and the fields `collect.py` reads are unchanged.
- The test runner's own task queue (TaskQueue/RunTasks/InstallTaskTick) is gone: `Testing::AddTask` posts to
  `GameThread::Post`. Test tasks now run at the queue's place in MainTick (before the editor tick, VRAM budget and DCLF
  family setups, not after them); posts stay first in, first out.
- Test tasks label their engine reads `Other` (they were `RecordTime`, inflating that column); `lockpick` and `book` wrap
  their singleton reads in `CS_ENGINE_READ`.
- Stale comments rewritten in present tense: Publication.h/.cpp (phase 0/phase 7 wording, `Building()`), FrameSnapshot.h,
  UiCommandStream.h, Testing.h, GameThread.h, EngineReads.h, EngineZones.cpp.

### Native drivers (src/RenderGraph)
- `Engine/Native/EngineAccess.h` (`Native::Access`): one `Global`, `Field`, `RequireCall` and `UntakenLog`. Deleted the
  copies in RenderThread, RenderScene, RenderWorld, WaterEffects, WorldPass and RenderDepth (where unity builds had
  already forced the `DepthGlobal`/`DepthUntaken` renames).
- `ReadBranches` fills `Prepared` directly; the mirror struct `Branches` is gone.
- WaterEffects' per-render-point `make_shared<std::vector<Work>>` is a local vector.
- `Point()` takes `PointTarget::Engine` for points that are meant to call the engine function (RenderSaoCs), so the
  "not hooked" log no longer fires there as a false alarm.
- RenderThread: one `PayloadOf` helper; `Park` takes a `ParkReason` enum instead of comparing strings. Unrendered updates
  were counted as "interface"; they have their own bucket at the end of the stats line now.
- ImageSpace: `Runtime::saveShotsDone` (always equal to `g_saveShotsDone`) removed; LensFlares and NativePost share one
  `At<T>`.
- OcclusionQueries: a dead `entry.begun = true` write removed. Flush and EngineTargetSet read their engine pointer once.
- Stale comments: RenderScene's SetupSunAndSky note (game work and recording are split), `Running()`'s orphaned doc,
  FrameAnchors.h "revisions adopt only on the main thread" (decision 70: the render owner adopts).

### Scaleform and menu scenes
- Observe leftovers removed: the per-draw `written` masks (computed for every constant group, never read; one masked
  pack is kept for the ShadowLightMaskSelect merge), the Scaleform cadence repeat check (`RepeatCheck`, `NoteSegment`,
  `Fnv`, `Replay::again`), and the `GameCall` census (always "called for nothing"). `ReportGameCalls` is now
  `ReportNativeUi` with its two useful lines.
- Dead code removed: the local map water cube branch (descriptor bit `kReflections` is never set there) with the
  `kREFLECTIONS` import, `LocalMapState::previous`, `ColourTargets`' always-false `a_taa`. (`Run::revision`/`Run::Hash` looked dead but `FamilyClient::Submit`
  sets the revision from the hash; kept.)
- `SceneGeometry` runs once per loading-model render: the shadows/depth step takes the colour pass's walk.
- Shared small helpers: biased alpha ref, frame-row tail, one accessor per shader singleton, the depth viewport through
  `MenuViewport`.
- Native-only compute helpers (`ComputeLighting`, `ComputeEffect`, `ComputeUtility`, membrane, permutation, bone rows,
  blended list) moved from MenuScenes.h into MenuScenesNative.cpp (review with `git diff -w`).
- UiStream's end-state tracking and the replay share `RefreshViewPerspective`, `BlendStackPush`/`Pop` and
  `kIdentityMatrix` (they were copies; decision 19 relies on the header equality they compute).
- `StartSegment` merged into `ClearSegment`. Stale comments (CS_ORG_LOADING_THREAD, "phase 4's second lead", observe
  parity, the guest-only file header) rewritten.

### DCLF core
- `FrameDriver::inGameHalf` removed for `Scene::FrameView::InGameHalf()` (set at the same calls on the same thread);
  `RepeatsTicket()` replaces the twice-written repeat test; `FrameTicket::installed` is typed.
- One `LoadingMenuOpen()`/`LoadingMenuIn(snapshot)` under the "is a load showing" variants; LoadHold's own `MenuOpen`
  and the one-line `LoadShown()` are gone.
- `StopDrawing` does its common two steps once.
- ShadowEpochs and ShadowCapture use `TicketUtilityShader()` instead of a live engine read.
- Engine reads written twice in one expression (`CS_ENGINE_READ(X) ? CS_ENGINE_READ(X)->...`) read once (GeometryPort,
  SceneLists, Visibility, ShadowBuild; also in the feature, editor and RenderGraph files). Site counts drop to one.
- The native 300-frame "bring-up" log is folded into the periodic coverage report as its first uncovered frame's state.
  This now prints in guest too (once per report window).
- Stale comments and dangling plan references ("plan step 10a", "cleanup plan A5", ...) removed across Core/Diagnostics;
  EngineReadWindow and the SceneFrame log describe the routed close and the render-owner check.

### DCLF features
- Frustum and sphere tests (`Corners`, `FrustumCorners`, `CornersOrEdgesInside`, `FrustaIntersect`, `SphereInside`)
  and a `LodFaded` test live once in ShadowCameras; LightSelection (guest) and LocalCameras (native) call them.
- `Engine::Address` gains the shadow light count, inside mask, lodFade cutoff, TLS index, focus shadows and focus targets;
  the Native shadow files use them and the existing scene graph entries instead of local copies. One
  `Engine::ScopedMemoryContext` replaces four copies. ShadowMapsNative's `Kind` is `Native::LightKind`.
- `CaptureSet` finds the sun once; `ReadDescriptors` is shared. ShadowMaskHooks shares the Keep/Submit tail.

### Other features
- Skylighting's precipitation preamble is one helper; Skin's stale-owner test is one function for guest and native.
- Stale "goes with phase N" comments (ScreenSpaceGI, TerrainBlending, Upscaling) and Streamline's `SimFrameId` comment
  describe the current behaviour.

## Not done: proposals for the director

| # | Where | What | Proposed change | Risk |
| --- | --- | --- | --- | --- |
| P1 | ShadowMapsNative.cpp:400-428, NativeCasters.cpp:262-268, LightSet.h:111-115 | **Likely bug.** `PrepareLightMaps` sets `coverageLeft` on a temporary; `CaptureSet` copies only `verdict`, `draws`, `left`, so the render-side coverage re-check in `RenderLightNatively` never runs for a game-half set. | Move the maps as one sub-struct (`captured.maps = std::move(maps.maps)`), or `captured.coverageLeft = maps.coverageLeft`. Then decide whether the `Fallback` enum, its counters and the 600-frame log (observe scaffolding) become a bool. | MEDIUM: enabling the check can drop native lights drawn today |
| P2 | WorldPass.cpp:117-125, PostChain.cpp:427/479, WorldSteps.inl:662-673 | Four copies of the normals-target rule; native has two sources (frame counter at capture vs at GameFrame). | Native derives from `WorldPass::NormalsTarget()`, the engine path from one formula helper. | MEDIUM |
| P3 | RenderWorld.cpp:75, WorldPass.cpp:207/215/220, RenderDepth.cpp:73-81, RenderWorld.cpp:108 | Render-half live engine reads; `underwater` is already in the ImageSpace block, others belong in `Scene::Prepared`. | Read the block / move into Prepared. | MEDIUM (ordering against the water capture) |
| P4 | PostChain.cpp:292-341 | Native does every engine read, then overwrites from `SnapshotInputs()`. | Branch first; engine reads in the guest branch only. | SAFE-MEDIUM |
| P5 | PostChain.cpp:340 vs NativePost.inl:512 | Two "screenshot due" definitions in native. | One accessor. | MEDIUM |
| P6 | ImageSpace.cpp:578-588, ~3007 | `ShaderOf` cache is a plain static array; `BuildTintRows` re-implements the lookup on the game side. | Atomic entries; note the detector category changes. | MEDIUM |
| P7 | ORGVolumetricLighting.cpp:368, 480-498 | Reads the sun shadow light's cascades and descriptors as raw engine memory on the render thread, outside `CS_ENGINE_READ`; races the game thread under phase 7. DCLF's light set already carries them. | Take them from `Shadows::Native::RenderSet()->sun`, the light from `Core()`. | MEDIUM-RISKY (native only, visual) |
| P8 | Feature blocks vs CoreFrame | `hideSky` (ExponentialHeightFog, DynamicCubemaps), sun light direction (TerrainShadows, PhysicalSky), `mode == kFull` captured four times, Effects11 sun/moon/billboard values, VolumetricLighting's interior/cellHasSun and screen size. | Add `fullSky` to CoreFrame, read `Core()`, delete the empty blocks and BlockIds. | MEDIUM |
| P9 | ExponentialHeightFog, ScreenSpaceGI, ScreenSpaceShadows, ViewAtmosphere, Border, LightLimitFix, ThemeManager, HiZPyramid | Display size read live from graphics state in seven places, copied into two blocks. | `Util::DisplaySize()` everywhere. | MEDIUM (native follows the adopted size) |
| P10 | LightLimitFix.cpp:2272-2276 | Native shadow fade steps by `snapshot->gameDelta` on every render, so repeats advance it again and drops lose time. | `Snapshot::GameStep()` in both modes. | SAFE in effect, changes cadence behaviour (a fix) |
| P11 | SunCamera.cpp:109-116 | `RealDelta()` reads `Snapshot::Render().repeat` from the game half (render-owner state): a race, and wrong in cadence repeat. FadeState and TreeWind compute their own "steps this render". | Drop the repeat check; add a `GameUpdates()` helper. | MEDIUM |
| P12 | LightSet.cpp, RenderScene.cpp:286/356 | The light set has its own game-to-render hand-off (`MadeSet`/`TakeSet`/`AdoptSet`, a RenderScene field) next to FrameDriver's `TicketPart`s. | Make it a `TicketPart`; at least rename `TakeSet` (it does not take). | MEDIUM (frozen-menu path) |
| P13 | Toggles.h:156, DrawPipelines.h:445-557, ShadowViews.cpp:204-226, FrameGlobals | Several per-half copies (`gameFrame`, `gameCatalog`/`heldCatalog`, two ShadowViews instances) beside FrameView's installed frame. | Read through FrameView's render-side accessors; delete the copies. | MEDIUM |
| P14 | FrameValues.cpp (devloss2 file) | Four "raise to max" CAS loops (also EngineReadWindow, EngineReleases); `RowsIfDone`/`PalettesIfDone`/`ShadingIfDone` share a body; a stale comment at ~2594 ("references go to the render thread": they go to the retire queue). | `AtomicRaise` helper, `UploadDone(wait)`, fix the comment, after devloss2 lands. | SAFE |
| P15 | FrameValues.cpp:816-821 | Sampling-job exceptions are caught and logged once; the frame carries on with stale rows (errors-are-fatal rule). | `RenderGraph::Fatal`, or record the exception in the decision log. | MEDIUM |
| P16 | FrameTicket `loading` vs FrameGlobals `loading` | Same read twice per game half, but not provably equal (LoadHold's hold state can change in between). | Decide which one the frame means. | MEDIUM |
| P17 | Native::Active / Native::Requested / FrameCamera::Owned (== Requested) / EngineReads::InCapture as control flow | "Is this native / game side" spelled four ways; InCapture ties behaviour to a diagnostic (Utils/Game.cpp, Deferred.cpp:708, ExponentialHeightFog.cpp:591). | `Snapshot::InCapture()` for behaviour; one predicate per meaning. | MEDIUM |
| P18 | Snapshot::FrameKind vs RenderGraph::Frame::Kind | Two enums with the same five values, mapped in FrameDriver.cpp:200-218. | Carry `Frame::Kind` in the snapshot. | MEDIUM (many files) |
| P19 | FrameSnapshot::interior vs EngineValues::interior | Two interior flags from different sources. | Keep one. | MEDIUM (can differ for one update at a cell change) |
| P20 | MenuViews: refusal handlers (Native:1769, 2047; Shadows:91), submit helpers (Native:1781, 2069; Shadows:109), three scene walks (Native:1461, Shadows:57, LocalMap:125), AddLighting/AddEffect tails | Duplicates between the menu scene, local map and shadow paths. | `Family::RefusalLog`-based `Refuse`, one submit helper with the draw limit, one visible-children walk with a billboard flag, a template draw tail. | SAFE-MEDIUM (hot path; billboard side effect) |
| P21 | Scaleform `DrawBitmaps` (Renderer:2470), UiStream growth, `EngineTargetOfView` (Passes:612, 727), menu `Run` latch growth, LocalMapVisible vectors | Per-call heap vectors, streams grown from empty every frame, a target scan per read per commit. | `UiStream::Reserve`, reserve from last sizes, cache the target in the declared read. | SAFE-MEDIUM (native only) |
| P22 | MaterialWrites.cpp:286-293 | Guest FaceTintUpdate uses an SKSE task (runs inside a frame job) where `GameThread::Post` exists. | Post to the queue. | MEDIUM (timing, guest) |
| P23 | Engine globals copied outside native drivers | `Global<T>` templates in ORGVolumetricLighting, DCLF's `Engine::Global`, `Native::Access::Global`. | One Utils-level header. | SAFE, many files |
| P24 | UnifiedWater.cpp:543 | Flow test uses `IsExteriorWorldspaceActive()` in guest while the hook uses `FrameExteriorActive()`. | `FrameExteriorActive()` in both. | SAFE (guest differs only on the toggle frame) |
| P25 | LightLimitFix `CapturedLight::rooms`, CS Editor's `PublishedGameView` | A vector per captured light rebuilt every update; a second game-to-UI channel overlapping snapshot data. | Flatten rooms; note why the editor channel is separate or make it a block. | SAFE / optional |
| P26 | EngineReads `Half`/`RenderHalfScope`/`FrameStartScope` counters | Phase-7 sub-counters; cs-test reads only the totals. | Keep for roadmap item 2 (performance), remove after. | decided by the agent, to confirm |

Checked and kept: the DCLF parity diagnostics (SetParity, PlayerParity, CaptureParity) predate the roadmap and are used by
`dclf-parity`, `dclf-capture-parity`, `dclf-compat` and `rx6600-parity`. `CS_SNAPSHOT_CADENCE` and every test command added
during the work (loadscreens, timestep, cs-ui, lockpick, book, page, uievent, snapshot:detect) have configs. The
`[Snapshot] side effects` 600-update line stays ungated: `stealth-lights.toml` and `cadence-drop-cells.toml` check it.

## Decided by the agent, to confirm
- `CS_ENGINE_ZONES` stays as is. In a non-Tracy build it still installs the sites and their counts reach the test status
  (`engine_render`); snap0-detfix used it to measure stub cost.
- The coverage report prints the first uncovered frame's state in guest too (the old native-only log was folded into it).
- Test tasks run earlier in MainTick (at the game-thread queue). No config depends on running after the editor or DCLF
  family ticks.
- The menu scene local map no longer imports `kREFLECTIONS`. A guest draw whose engine state bound it in a declared slot
  would be refused (non-fatal in guest), as before the native local map commit.

## Findings for later (devloss2 area, not touched)
- FrameValues `EndFrame` resets the render views only when `done >= seq`, so a slow upload keeps the previous views one
  frame longer. Intended, worth a look once devloss2 lands.
- P14 above (FrameValues helpers and the stale comment).

## Other open items
- Decision 60 ("still counted"), decision 219 ("called for nothing") and `docs/development/engine/scaleform.md:53` describe
  the GameCall census, which is gone. The director owns the decision log.
- Streamline `SimFrameId()` assumes the next adoption renders this update's snapshot. With routed updates two publishes
  can share an id. Needs a rule from the publish side.
- `WaterEffects::Render()` runs only in the routed render half; the unrouted `DrawFrame_RenderScene` path (paused or
  camera-path world frames) may never draw the queued water work. Worth checking.
- SunCamera and LocalCameras keep separate `SetViewFrustum` ports: they differ on NaN (`!(near >= limit)` vs
  `near < limit`).

## Commits

| sha | topic |
| --- | --- |
| d49364267 | refactor(snapshot): block read and step helpers |
| 1d37c67b7 | refactor(native): share engine access helpers |
| 197536938 | refactor(snapshot): drop dead fields and copies |
| 70e523e2f | refactor(testing): post test tasks to game queue |
| 823e77c55 | refactor(ui): drop menu scene observe leftovers |
| ea4719fbc | refactor(dclf): tidy frame driver and load checks |
| f13688fa6 | refactor(dclf): share shadow camera helpers |
| a385635e9 | refactor(features): read blocks through helpers |

1d37c67b7 removes `Park(const char*)`; Publication.cpp switches to `ParkReason` in 197536938, so 1d37c67b7 alone does
not build. No submodule pointer moved.

## Runs (stage post-cleanup, Dev-Fast, all at a385635e9)

| config | label | result |
| --- | --- | --- |
| native-vulkan+quick | post-cleanup-nvk-quick | passed; shot ref 0 wrong tiles; live reads 491/world frame (org_integration's dir-s7dl run: 501) |
| guest+quick | post-cleanup-guest-quick | passed |
| native-d3d12-quick | post-cleanup-d3d12-quick | passed (native device helpers touched) |
| native-vulkan+cadence-drop-cells | post-cleanup-nvk-cadence | passed (StepOnce, Scaleform repeat check removed) |
| native-vulkan+menu-scenes | post-cleanup-nvk-menu-scenes | passed; refs within tolerance |
| native-vulkan+map-local | post-cleanup-nvk-map-local | passed; local map shot identical by eye to dir-s7th's |
| native-vulkan+menu-loading | post-cleanup-nvk-menu-loading | passed (loading model walks once) |
| native-vulkan+menu-ui | post-cleanup-nvk-menu-ui | passed (UiStream shared rules) |
| native-vulkan+menu-book | post-cleanup-nvk-menu-book | passed |
| native-vulkan+stealth-lights | post-cleanup-nvk-stealth-lights | passed (side effects line, State::WriteCsEngineState) |

Live reads drop because reads written twice in one expression now count once. Skipped: the guest parity configs
(`dclf-parity`, `dclf-capture-parity`): only comments and a single-read change touched DCLF guest code; the director's
full suite covers them.

## Proposed decision-log entries
- The render-thread cleanup pass (post-cleanup.md): no migration switch remained; observe leftovers in the menu scene
  and Scaleform ports removed; `Snapshot::CurrentBlock`/`StepOnce` are the one way features read a block and step once.
- Decisions 60 and 219: the Scaleform `GameCall` census is gone; `ReportNativeUi` logs the DrawInterface counts and the
  UI resource stats.

## Second pass (post-cleanup2)

Lead: post-cleanup2, branch `post/cleanup2` from `org_integration` 4f7b26333 (the first pass merged). The director accepted
the proposals above as the work list. Stayed out of DCLF's main payload commit path, `WaitForLastRender` and FrameValues
(snap7-devloss2), so P14 and P15 wait. No submodule pointer moved.

### Per proposal

| # | Result | Commit |
| --- | --- | --- |
| P1 | Fixed: `CaptureSet` copies `coverageLeft`. The render-side re-check now runs. In cadence-drop-cells the first window after a load counts 52 lights as "views not DCLF's"; before the fix the same lights (37-58 a run) failed later as "capture failed" (CaptureNativeShadowView), so no light that drew before is lost. The `Fallback` enum stays: guest's LightRender needs the reason to call the engine's Render, and the 600-frame line is the only place native's dropped lights show. | 4b41b8efa |
| P10 | Fixed: native's local shadow fade steps by `Snapshot::GameStep()` (0 on a repeat, summed over dropped snapshots). Guest still reads the engine's delta. | cfd6d7f87 |
| P11 | Fixed: SunCamera's `RealDelta` checks `Render().repeat` only off the game half (a render owner selection in a frame the engine's skeleton draws). Native's game half runs once per update, so it needs no check. No `GameUpdates()` helper: nothing else needed it. | cfd6d7f87 |
| P2 | Done: `WorldPass::NormalsTargetOf(ssr, frameCounter)` is the one rule. Native's game half takes the world pass's target from the update's snapshot (ImageSpace block: SSR switch and frame counter), the same values the native post steps use, so the world pass and SSR/SAO/TAA cannot disagree. Native TAA's step key uses `EngineNormalsParity()` instead of Deferred's forward targets. Guest's engine-chain classifier (ChainSteps `ChainKeyOf`) keeps reading what Deferred bound: that is the bound target, not a copy of the rule. | bc1f1dfbf |
| P3 | Done: RenderWorld's post-resolve flag, the masks' scene shadows / kept casters / sun shadows off, and the water blend test's TAA water history, water height and local map flag are `Prepared` fields taken in the game half. The under water state is `WorldPass::Underwater()`, from the snapshot's ImageSpace inputs (the block the post chain's depth of field already reads). | 00850bc09 |
| P5 | Done: `ImageSpace::SaveScreenshotDue()`; the frame description (PostChain) took the raw request flag, the native chain the count test. | 172e70590 |
| P8 | Partly: `CoreFrame::fullSky` added; the Exponential Height Fog, Dynamic Cubemaps, Skylighting and Screen Space Shadows blocks (hide flag or full mode only) and their captures and BlockIds are gone; Cloud Shadows reads `fullSky` from the core. Not done: Effects11's sky fields (its block compares them as one value), TerrainShadows' sun direction (needs a "has sun light" flag the core lacks), WetnessEffects' fullSky (gates its own capture), VolumetricLighting's interior/cellHasSun/screen size. Low value each. | d50c57df8 |
| P9 | Skipped: the seven live display-size reads are the D3D11 (non-graph) paths (`Prepass`, `DrawSSGI`, `EnsureVolumetricResources`, Border's D3D11 clear, HiZ for grass), Light Limit Fix's `SetupResources` (before any adoption) and two game-thread captures. The graph paths already read `Vars::DisplaySize`. Switching the D3D11 paths changes guest/D3D11 timing for no native gain. | - |
| P12 | Renamed `TakeSet` to `LastSet`. Not a `TicketPart`: the set is made at the shadow casters' point, after the ticket (SetupSunAndSky, +0xD3), and `Prepared` already hands it to the render half. | bd16ae278 |
| P13 | Skipped: decision 227 (snap7-thread2) made the per-half copies deliberate (Toggles keep one frame word per half; the halves own their DCLF state). | - |
| P20 | Done in part: `Native::Refuse` (left-out log or fatal) and `SceneDraws::Hand` (draw limit, take or keep) replace the local map's and the loading model shadows' copies and the colour pass's inline submit. Every batch now checks its draw limit (fatal), as the local map did. The three scene walks and the AddLighting/AddEffect tails stay: they differ in billboard handling and hot-path shape. | c61a72fe3 |
| P21 | Done: a new UI stream reserves the last stream's command, payload and texture counts; DrawBitmaps' glyph vertices and the local map's visibility walk reuse thread scratch; a declared display read keeps whether it is an engine target, so a commit no longer scans the render targets per read. | f40ece545 |
| P7 | Done: native VL's generate rows take the sun's count, cascade ends and light transforms from `Shadows::Native::RenderSet()->sun` instead of raw engine memory on the render thread. | 2241aa8a9 |

### Open items from the first pass
- **WaterEffects in unrouted worlds: a real gap, fixed** (fff6b2ebb). `Render()` ran only with a routed ticket. A world drawn on
  Main::DrawFrame (paused menus over the world, the camera path) queued water work in each capture and never drew it, so the
  queue held face and flow-copy data until the next routed render. `RecordFrame` draws it in both paths now.
- **Streamline `SimFrameId`: recorded, not fixed.** With routed updates two publishes before one adoption take the same id
  (`LastRenderFrame() + 1`), so Reflex gets two SimulationStart markers for one frame id; the first update's snapshot is never
  rendered. Deduplicating by id keeps the earlier update's start (latency overstated); keying by game id needs the render side
  to know which game id each render frame adopts and to map Reflex's frame token to it. Needs a rule from the publish side, with
  roadmap item 2 (performance), where Reflex pacing is measured.
- `docs/development/engine/scaleform.md` no longer describes the GameCall census (fe845a42d). Decisions 60 and 219 are history.

### Decided by the agent, to confirm
- P1: keep the `Fallback` enum and its 600-frame line (guest's fallback needs the reason; the line is native's only view of
  dropped lights).
- P2: native's normals target comes from the update's ImageSpace block rather than a separate engine read in the game half.
- P3: the under water state comes from the ImageSpace block (captured at publication), not a game-half read.
- P20: every menu batch fails fatally past its draw limit (only the local map did).
- Water work draws in unrouted world frames (paused menus over the world).

### Runs (stage post-cleanup2, Dev-Fast; all passed)

| config | label | at | note |
| --- | --- | --- | --- |
| native-vulkan+quick | post-cleanup2-nvk-quick-1 | bc1f1dfbf | live reads 496/world frame |
| native-vulkan+cadence-drop-cells | post-cleanup2-nvk-cadence | c61a72fe3 | P1 coverage line above; side effects line present |
| native-vulkan+llf-local-shadows | post-cleanup2-nvk-llf | fe845a42d | P10 |
| native-vulkan+update-cap30 | post-cleanup2-nvk-cap30 | fe845a42d | repeats (P10, P11) |
| native-interior | post-cleanup2-interior | fe845a42d | P7; VL shots look right by eye |
| native-vulkan+taa-water | post-cleanup2-nvk-taaw | fe845a42d | P2, P3 (water blend, TAA water form) |
| native-river | post-cleanup2-river | fe845a42d | P2, P3 |
| native-vulkan+ui-menus | post-cleanup2-nvk-uimenus | fe845a42d | water fix: 427 updates on Main::DrawFrame, 584 water render points for 590 updates |
| native-vulkan+menu-scenes | post-cleanup2-nvk-menuscenes | fe845a42d | P20 |
| native-vulkan+map-local | post-cleanup2-nvk-maplocal | fe845a42d | P20, P21 |
| native-vulkan+menu-loading | post-cleanup2-nvk-menuload | fe845a42d | P20 (loading model shadows) |
| native-vulkan+save-screenshot | post-cleanup2-nvk-saveshot | fe845a42d | P5 |
| native-vulkan+shadows-blend | post-cleanup2-nvk-shblend | fe845a42d | P8 (Screen Space Shadows' full sky) |
| guest+quick | post-cleanup2-guest-quick | fe845a42d | shared feature code (P8, LLF) |
| native-vulkan+quick | post-cleanup2-nvk-quick-2 | fe845a42d | live reads 478/world frame (496 before) |

Skipped: native-d3d12-quick (no ORG, BasicRHI or device code touched), upscalers (Streamline not changed), the guest parity
configs (guest code paths unchanged apart from reading the core block's sky flags).
