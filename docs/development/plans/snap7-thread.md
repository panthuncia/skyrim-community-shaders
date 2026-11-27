# snap7-thread: the native render thread (roadmap phase 7, items 1, 2, 3, 6, 8 and the open items)

Lead `thread`, round `snap7`, branch `snap7/thread` (base `org_integration` 6bb5b7788), stage `s7-thread`. Inputs: the roadmap's phase 7
and "After phase 7", [snap7-prep.md](snap7-prep.md) (the phase 7 plan), [snap7-split.md](snap7-split.md), [snap7-ticket.md](snap7-ticket.md),
[snap7-lights.md](snap7-lights.md), [snap7-reads.md](snap7-reads.md), [snap7-hold.md](snap7-hold.md), [snap7-frameend.md](snap7-frameend.md),
[snap7-precip.md](snap7-precip.md), [snap7-sky.md](snap7-sky.md), [snap7-prep2.md](snap7-prep2.md), decisions 8, 34, 51, 62-75, 99,
138-141, 192-224.

Status: **in progress.** This file is the design, the work packages in order, and the steps landed. A successor continues from
section 6.

## 1. What still couples the two halves (survey, 2026-11-27)

Decision 224 says native's render half reads no game-mutated engine state. That is about engine reads. The halves are still coupled
by order and by CS state, because both run inside one Main::DrawFrame call on the game thread:

1. **Main::DrawFrame drives the frame** (AE 36555 / SE 35556, decompiled in section 2). Renderer::Lock/Begin/End run around it; Begin
   is where the render side adopts the snapshot and runs OnBegin (frame clock, revisions, DCLF's scene adoption). The game half
   (`Native::Scene::PrepareFrame`, DCLF's `GameFrameStart`) runs after Begin and reads what Begin adopted.
2. **Scene pairing** (snap7-hold 5.3, decision 141): DCLF's game half installs the claims of the publication the render side's
   revision adopted at this frame's Begin (`HandOverAtFrameStart(draws.AdoptedPublication())`), and the ticket names it. With the
   game half first, it has to choose the scene itself.
3. **The UI producer replays inline on the render owner.** `Scaleform/Renderer.cpp` records GFx's calls into a stream and replays
   each display at its EndDisplay (`RunReplay`), every slot gated on `FrameAnchors::IsRenderOwner()` (`Mirroring()`); the stream goes
   into `Snapshot::Current()->ui` (the adopted snapshot, not the one the update builds). DrawInterface's detours on the same function
   (82084) also make render-side writes: `Upscaling::PostDisplay` (jitter, viewport, camera data, `FrameCamera::OnFullResolution`),
   `ScreenshotFeature::SnapshotCleanScene`, `HDRDisplay::SetUIBuffer`, `Util::SetTemporal`.
4. **Game-half readers of render-side state:** `Snapshot::Current()`/`CurrentId()` read from game-half code (RenderScene.cpp's
   `RenderScene`, `WaterEffects::Render`, `FirstPersonLatch`, LoadHold, FrameView, `FrameDriver::MakeTicket(CurrentId())`);
   `FrameGlobals` (the game half binds a `Scope` only inside `GameFrameStart`; later game-half reads fall through to `latest`, which
   the render half's `Adopt` replaces); `DecideDrawFamilies` reads `impl->scene`; `Native::Scene::Current()` (`g_current`) is written by
   the game half and read by the render half (FirstPersonEpochs, NativeCasters, LightLimitFix, NativePost, WorldPass).
5. **Render-half readers of game-half state:** the kept row arrays (`FrameValues::RowsIfDone`, `PalettesIfDone`, `ShadingIfDone`,
   `ForwardRowsFilled`), `MakeRegistered` (lazy, both sides), fade statics `fadeEye`/`treeHeight` and the per-render fade kick's engine
   camera (`FadeState::SampleFrame`, `MainCamera`).
6. **Thread identity checks:** `FrameDriver.cpp` `FrameOnlySource` (a Begin off the main thread is a loading screen), `FreezeTestAtBegin`,
   `SampleEngine` (main thread only); `Revision.cpp:1629` (DCLF's scene source is `mainThreadOnly`); `EngineReadWindow::CloseAtPresent`
   (main thread in an update); `Retire.cpp` engine lane and `EngineRef` (fatal off the main thread: a snapshot or ticket dropped last on
   the render thread outside the lanes is fatal); `SideEffects` `t_renderHalf` (engine stage events 5-9 suppressed inside the render
   half); `Publication.cpp` `t_inGameUpdate`.
7. **Frame kinds with engine render work on the producing thread:** menus that pause the game (inventory, journal, map with the local
   map's engine draws until snap5-localmap2, book page copies, lockpicking, menu scenes recorded at UI3DSceneManager::Render and the
   interface's ImageSpace effects inside DrawInterface), the frozen background (RenderScene #1 and 107130), loading screens (serving
   thread), Bink, and DrawFrame's debug camera-path branch (52210/52211, RenderEffect #2/#3).

## 2. Main::DrawFrame in a world frame (AE 36555, SE 35556)

In order (AE ids; SE ids and both call offsets are in `EngineZones.cpp`'s table): Main::SetDRS (NOPed by Upscaling), Renderer::Lock,
Renderer::Begin; the decal manager's queued tests (36562; native: SideEffects S17 empties the queue first); FaceGen::UpdatePending
(removed by FaceTint); 77371 SetPerFrameBuffers, the framebuffer and depth binds, 77516/77258 state words; **82085 the menus' PreDisplay
(UI game logic)**; 107698/107646 (engine shader cache, used by engine-drawn menu scenes); **52210 MenuControls (debug camera path)**;
82075 the freeze test; world: **scrap heap mark (68150), 51639 (WorldPtToScreenPt3's camera globals: game side), RenderScene (CS's
driver), scrap heap release (68151)**; RenderEffect #1 0x22 (Volumetric Lighting, only when the console's TVL flags 415134/415135 are
set); **35406 FrameJobs, WaitForJobTask(0, 2) (the UI jobs)**; 77516/77258; **82084 DrawInterface (UI producer)**; 52211 (camera
path toggle, debug); the camera-path render branch (RenderEffect #2/#3, debug only); **36857/36858/36859 (save screenshot request:
game side)**; SetJobTaskEvent 8 (CS's in native: SideEffects); BSSceneGraph 74986-pair on the world root; 107502 (engine pools);
Renderer::End (present through CS's Present hook, frame counter removed by S2), Renderer::Unlock; 77385/77517 (dynamic resolution);
SetJobTaskEvent 9.

Bold items are game side. Renderer::Begin's body (window resize, BSGraphics::State and the shadow state reset, the context's
ClearState and samplers, the current render window, the target manager's size) and End's (present, the inFrame flag) touch only
render-owned memory.

## 3. Design (decided by the agent, to confirm)

### 3.1 Which frames the render thread draws

**Unpaused world updates** (`kind == World && !menus.paused && !menus.map`, and not DrawFrame's debug camera path): normal gameplay,
where all the performance is. Every other update (a pausing menu, menu over world, main menu, map, debug camera path) and every
non-game producer (loading screen thread, Bink) keeps today's path on its own thread with the render thread **parked**. SPLIT-e's rule,
narrowed by what still records engine work on the producing thread (section 1 item 7). Ownership moves with `ClaimRenderOwnership` at a
route change; the producer that takes over first parks the render thread (it finishes its current frame and stops), the only place one
side waits on the other, at most one render frame, only at a route change (a menu opening, a load starting).

### 3.2 The game thread's world update (in place of Main::DrawFrame)

`Snapshot` builds the snapshot without posting it (`Capture` split into build and publish). Then CS's game frame, in DrawFrame's order:
SideEffects after the capture (S1 and S4 move here, decision 34), 82085, 51639, the scrap heap mark, the driver's game half
(`PrepareFrame`, DCLF's `GameFrameStart` only), the scrap heap release, the VL flag, FrameJobs/WaitForJobTask, DrawInterface (record
only, into the snapshot being built), the save screenshot request, `Testing::OnGamePresent`, the engine-read window's close, then the
publish with the native frame payload (`Prepared`, DCLF's ticket, the light set, the UI stream) and `PaceGameUpdate` (switch removed).
No Renderer::Lock/Begin/End on the game thread.

### 3.3 The render thread

Created at the first routed update. Loop: wait for a published snapshot while parked or before the first; Renderer::Lock (pairs with the
loading screen thread); the Begin detour's CS part (claim, adopt the newest snapshot, resize event, menus, window state, OnBegin with
revision adoption and DCLF's scene from the ticket) plus Begin's body; FaceTint's render frame; DCLF's render half (`BeginFrame` from the
payload's ticket); `RecordFrame(payload.prepared)`; VL if flagged; the UI point (Upscaling PostDisplay, clean-scene snapshot, HDR UI
buffer, temporal off, then the snapshot stream's replay); Renderer::End (CS's Present hook: `RenderFrameEnd` only); Unlock. A repeat
(no new snapshot) renders the same payload again (its ticket, `Render().repeat`).

Decided by the agent, to confirm: the render thread keeps calling Renderer::Lock/Begin/End. Their bodies touch only render-owned memory
and are what the render owner called before; porting them is a cleanup item, not a split blocker.

### 3.4 Scene pairing

The game half takes DCLF's newest posted scene fragment out of the revision post itself (a new `Revision::TakePosted(source)` for
game-side sources), installs that publication's claims and puts the fragment in the ticket. The scene source's `takes` becomes a `take`
callback: at the render thread's OnBegin it hands the coordinator the adopted ticket's fragment, once; a repeat hands nothing. A fragment
the revision drops at adoption makes that render uncovered (the render half's coverage check, a tap error), as an uncovered frame is
today. `FragmentSource::mainThreadOnly` goes.

### 3.5 The UI producer

The native recorder records on the producer thread without render ownership (`Mirroring()` is "native and this thread produces the
UI"), never replays, and sets the stream into the snapshot being built. The render side replays the adopted snapshot's stream at its UI
point (`Scaleform::ReplaySnapshotUi`), with DrawInterface's render-side detours split out of the producer into that point.

## 4. Work packages (in order)

| # | Package | Single-thread testable | Status |
|---|---|---|---|
| P | Game-half accessors and thread-identity cleanups: `Snapshot::Building()` for game-half readers; FrameGlobals scope over the whole game half; `SampleEngine` into the capture; `FrameOnlySource`/`FreezeTestAtBegin` without `IsMainThread`; the engine-read window closed at the update's end; `MakeRegistered` at install; the kept rows handed to the render side as copies or views; the fade statics and the per-render fade kick's camera from the ticket | yes | done (snap7/thread-prep, merged), section 6 |
| U | UI producer record-only (3.5) | yes (record, then replay at once) | done (snap7/thread-ui, merged), section 6 |
| S1 | CS's game frame in place of Main::DrawFrame for routed updates, game half before the render half, scene pairing (3.2, 3.4), still one thread | yes | done (135f05e48), section 6 |
| S2 | The render thread (3.3), parking, cross-thread checks, PaceGameUpdate on | - | landed (49aabed95); long runs fail on the casters' race (section 6 "S2") |
| R | Resize on the render thread (native device Resize, swap chain, target versions) instead of Renderer::RequestWindowResize + Begin | - | not started (TH-j) |
| I | ImGui on the render thread (Menu::DrawOverlay from RenderFrameEnd; it already is) and the harness (shots and live frames on the render thread) | - | runs on the thread with S2 (menu-ui passes); `ui-writes` not checked |

## 5. Runs

Stage `s7-thread`, Dev-Fast. Build s1a = S1 (135f05e48); s2a = S2 first build; s2b = S2 with the fade and load-hold locks.

| Config | Label | Build | Result |
|---|---|---|---|
| native-vulkan+quick, detect | s7-thread-s1a-nvk-quick | s1a | passed with the shot reference; routed from snapshot 65 (the first 53 world updates are paused by the load's fade and go through Main::DrawFrame); Main::DrawFrame's own calls (Renderer::Lock in it) 0.33 per world frame = only those unrouted ones; the call-site census still counts CS's hook on Main::Update's call as 1.0 |
| native-vulkan+cadence-repeat | s7-thread-s1a-nvk-cadence-repeat | s1a | passed, 3 shots |
| native-vulkan+menu-scenes | s7-thread-s1a-nvk-menu-scenes | s1a | passed with shot references (inventory, journal, loading model, stats, world); 165 routed, 435 on Main::DrawFrame, 2 routed updates whose interface the render half drew (a menu opened by the UI jobs) |
| native-vulkan+quick, detect | s7-thread-s2b-nvk-quick | s2b | passed with the shot reference, render thread running; render half (record) game-mutated reads 0.00 per world frame, frame end none |
| guest+quick | s7-thread-s2c-guest-quick | s2c (49aabed95) | passed |
| native-vulkan+menu-scenes | s7-thread-s2c-native-vulkan-menu-scenes | s2c | passed with shot references |
| native-vulkan+menu-ui | s7-thread-s2c-native-vulkan-menu-ui | s2c | passed (ImGui on the render thread, every cs-ui-expect holds) |
| native-vulkan+cadence-drop-cells | s7-thread-s2c-native-vulkan-cadence-drop-cells | s2c | failed: tap error (fatal) 40 s in, frame 2132: "DCLF: occlusion map 0 of frame 1745 was taken for the shadow epoch, which did not draw it" (FrameDriver::OcclusionNeedsEngine, from Skylighting::RenderOcclusion on the render thread); 416 routed / 1340 routed renders before it |
| native-vulkan+first-person-switch | s7-thread-s2c-native-vulkan-first-person-switch | s2c | failed: the same tap error at frame 3279 (occlusion map 0 of frame 2556), after shots first-1 and third-1; 542 routed updates, 2070 routed renders (about 3.8 renders per update) |
| native-vulkan+quick, detect | s7-thread-s2a-nvk-quick | s2a | crashed 3 frames after world-ready, 0.4 s after the render thread started: both threads in `DCLF::Features::Fades::CountAnimatedRoots` (FadeWriters.cpp, a shared static scratch array; the animated fade batch is prepared by the game half and taken by the render side's commit) |

## 6. Steps landed

### Package P (sub-lead `th-prep`, branch `snap7/thread-prep`)

Still one thread; no visible change. What each item changed, and what the render thread needs from it:

1. **Game-half readers of the snapshot.** New `Snapshot::Building()` (Publication.h): the newest game update's snapshot `Capture()`
   built, set only by `Capture` (a frame-only snapshot never replaces it); its id is the existing `LastGameId()`. Today it equals the
   adopted one inside an update; a cadence repeat renders it again; a dropped update's is never adopted (its game half gets it passed).
   Converted: `RenderScene` (the snapshot `PrepareFrame` gets), `FirstPersonLatch`, the ticket's id in `SceneFrameStart`
   (`MakeTicket(LastGameId())`), and the map's re-entry (`LoadHold::BeginReentry(why, frame)` and `FrameView::LoadShownIn(frame)` /
   `LoadingScreenIn(frame)` take the asking half's snapshot). Left on `Current()` because they are render half: `WaterEffects::Render`
   (it records water faces; it still runs at SetupSunAndSky inside the game half, the lead must move it to the render half),
   `HoldsScene`, the load checks at the frame end. For S1: `Building()` is set after the feature captures, just before the publish;
   when DrawInterface records between build and publish (th-ui), it must be set when the snapshot is built.
2. **FrameGlobals.** The driver's game half binds the ticket's globals from DCLF's frame start to its end
   (`FrameGlobals::BeginGameHalf`/`EndGameHalf`, bracketed by the new `FrameDriver::GameHalfStart`, called from `PrepareFrame` through
   `DrawcallLimitFix::OnNativeGameHalfStart`); `Current()` order is scope, game half, render owner's latest. The render owner (not "the
   thread that last adopted") reads the latest adopted capture. The unscoped-read error (snap7-sky open item 5) was a loading screen's
   menu scene on the serving thread (`MenuViews::Scenes::Native::SampleInputs -> MaterialPort::SampleFrame`, 31,722 reads in quick's
   loading frames) and 90 reads in main-menu frames before the first adoption: both are render-owner reads, now served the latest.
   The error line logs its thread and callers (module+offset) if it ever fires again. Render thread: nothing; it reads the latest
   because it owns rendering after its Begin.
3. **Native::Scene::Current()** is the `Prepared` handed to `RecordFrame`, set only while it records (`g_rendering`); the game half
   writes no global. `PrepareFrame` takes the normals target by the engine's rule (`WorldPass::EngineNormalsTarget`); the skeleton's
   clear reads `a_frame.normalsTarget`. Render thread: call `RecordFrame(payload.prepared, ...)`; its readers (WorldPass,
   FirstPersonEpochs, NativeCasters, LightLimitFix, NativePost) need nothing else.
4. **Frame driver thread checks.** `SampleEngine`: the game thread takes the values into `FrameSnapshot::engine`
   (`Snapshot::EngineValues`: TES interior, the VL descriptor's size) in `Capture` and in a main-thread `CaptureFrame`; a loading
   screen's or movie's frame-only snapshot keeps the last. OnBegin sets the variables from the adopted snapshot on any thread.
   `FrameOnlySource`: marks instead of `IsMainThread`: movie ticks (as before), a new detour of the serving thread's loop
   (`RunLoadingScreen`, SE 38085 / AE 39039, `void()`), `Snapshot::InGameUpdate()`, and the hook point
   `Frame::Driver::GameSnapshotRenderScope` (FrameDriver.h): **the render thread opens one for its loop**, so its Begin adopts the
   newest published snapshot without a frame-only publish. `FreezeTestAtBegin` reads the freeze answer its own Begin decided on the
   same thread (thread-locals set by `DecideKind`), so a Begin on the render thread cannot race it. `State::loading` was only the
   warning's and is gone.
5. **Engine-read window.** Native closes it at the end of the update's renders in `MainUpdate_DrawFrame`, before
   `SideEffects::AfterRenderHalf` raises the job stages; DCLF's `EndFrame` closes at Present in guest only (`CloseAtPresent` without the
   thread check: only the main thread is ever inside a game update). The close/wait counters are atomics (`Take` from any thread).
   A cadence repeat now renders with the window open (it was closed at the first render's Present): only sampling jobs take leases,
   and the game half kicks those. Lead: move `Close()` to the publish.
6. **FrameValues.** The upload job takes copy-on-write views of the kept rows (`KeptWholeArray::Shared()`) just before `done`;
   `RowsIfDone`/`PalettesIfDone`/`ShadingIfDone` return `const KeptChunks<T>*` of those views; `EndFrame` releases them once the job
   is done, so in today's frame no sampling job ever copies. With a render thread a sampling job that runs while a render still holds
   them copies the array first (the store's own copy-on-write). `ForwardRowsFilled` was already safe (the job fills a render-owned
   slot before `done`). Registered pools are made with the built-in ones at FrameValues' construction (a magic static), with a fatal
   check that every feature registered first; no lazy make from either side. The chain stays lock-free.
7. **Fades.** New ticket part `TicketPart::Fades` (`FadeState::TakeTicketPart`, game half): the engine's fade values (one update's
   step), the main camera's position, LOD factor and frustum, the tree height test. `SampleFrame`, `FadeEye`, `TreeHeightTest`,
   `LatchGlobals` and the new `MainCameraFrustum` read it; native's render side reads only the ticket, guest keeps reading the fade
   values and the camera live at its render points. `FadeState::BeginFrame`, its statics, `MainCamera` and the root states'
   `frameStart` hook are gone. The fade counter and step time are written only in Main::Update before its DrawFrame call (Ghidra
   xrefs), so the game half's values equal the old per-render reads in native.
8. **Thread-locals.** `t_renderHalf` (SideEffects) is read only by the `SetJobTaskEvent` detour on the thread inside Main::DrawFrame;
   the render thread never calls DrawFrame, so nothing changes. `t_inGameUpdate` readers: `FrameOnlySource` (the render thread uses
   `GameSnapshotRenderScope` instead), guest's `CloseAtPresent`, the harness (`Testing::Shot`'s minimum snapshot on the game thread and
   `Testing::OnGamePresent` at Present: package I must give the harness's Present its update explicitly).

Runs (stage `s7-thprep`, Dev-Fast; build a = all items before the render-owner fix, b = final code; all passed):

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick, `CS_SNAPSHOT_DETECT=1` | s7-thprep-a-nvk-quick | passed with the shot reference; record game-mutated 0.00, frame start 0.04 (render-owned target tables, as snap7-ticket); one unscoped FrameGlobals read logged (the loading screen's menu scene) |
| guest+quick | s7-thprep-b-guest-quick | passed |
| native-vulkan+quick, `CS_SNAPSHOT_DETECT=1` | s7-thprep-b-nvk-quick | passed with the shot reference; record game-mutated 0.01 + 0.01 (one-time Grass ini and SSGI reads, as snap7-sky), frame end none, frame start 0.05 (8 reads, render-owned); no unscoped FrameGlobals read |
| native-vulkan+cadence-drop-cells | s7-thprep-b-nvk-cadence-drop-cells | passed, 3 shots |
| native-vulkan+cadence-repeat | s7-thprep-b-nvk-cadence-repeat | passed, 3 shots |
| native-vulkan+first-person-switch | s7-thprep-b-nvk-first-person-switch | passed, 7 shots |
| native-vulkan+native-interior | s7-thprep-b-nvk-native-interior | passed, 4 shots |
| native-vulkan+llf-local-shadows | s7-thprep-b-nvk-llf-local-shadows | passed, 6 shots |
| native-vulkan+menu-loading | s7-thprep-b-nvk-menu-loading | passed, 2 shots |

No run in build b logged the unscoped-read error. Skipped: native-d3d12-quick (no ORG, BasicRHI or native device code changed).

Left for the lead: split `FrameDriver::SceneFrameStart` (game: `MakeTicket` + `BeginGameHalf`; render: `BeginFrame` from the
payload's ticket); move `WaterEffects::Render` out of the game half; `EngineReadWindow`'s `mainThreadLeases` diagnostic still uses
`IsMainThread`; LoadHold's `hold` struct is written by both halves (game half's map re-entry, render half's occlusion re-entry and
frame end): a render-thread design needs the map's re-entry posted as a message.

Decided by the agent, to confirm:
- P-a. Registered frame-value pools are made at FrameValues' construction rather than literally at install: the built-in pools are
  made there too, and making either before the graph host exists is untested.
- P-b. A thread that begins a frame outside a movie tick, the serving loop and a game update counts as a main-thread frame (it was a
  loading screen with a warning); none is known.
- P-c. Native closes the engine-read window after all of an update's renders, so cadence repeats render with it open.
- P-d. Guest keeps live fade values and camera at its render points (as SKY-a); it reads eye and tree height from the ticket, which it
  already took at the frame start.
- P-e. A movie's or loading screen's frame-only snapshot keeps the last engine values (the old code did not sample off the main thread).

### Package U: the UI producer records without render ownership (sub-lead `th-ui`, branch `snap7/thread-ui`)

Built in today's single-threaded frame. No visible change is intended. Code: `src/RenderGraph/Engine/Scaleform/Renderer.cpp`
and `Scaleform.h`.

**Who records.** `Mirroring()` used to mean "native and the render owner". Now it means "native and this thread is inside a
DrawInterface it produces" (thread-local `UiMode`: `None`, `PerDisplay`, `RecordOnly`). A GFx call on the render owner outside
DrawInterface is counted and logged once. Every run below shows 0. Streams are per DrawInterface: `FrameClock::PresentSerial`
and `UiStream::frame` are gone, and so is the "stream left open past its present" retire in `Stream()`. `FinishStream()` no
longer replays.

**The frame's mode, decided at DrawInterface's start** (render owner, no `ProducerScope` open):
- *record-only* when all of these hold: the DrawInterface is inside a game update; the stream's snapshot is a World frame with
  `!menus.paused && !menus.map`; the UI asked again at DrawInterface's start says the same (`GameIsPaused`, the map); and no
  Book/Map PreDisplay is due. GFx only records, into `Snapshot::Building()->ui`, and nothing is replayed. Then
  `ReplaySnapshotUi` runs at once, on the same thread.
- *per display* otherwise: menus that pause, the map, the book, loading screens, frame-only snapshots. The render owner records
  and replays each display at its EndDisplay, as before, inside the UI point's scope.
- The live UI check was added after `s7-thui-nvk-menu-scenes`. There, an inventory opened by the update's UI jobs after the
  capture drew its menu scene in a frame that was record-only by the snapshot's menus.

**Stream holder.** In a game update the stream goes into `Snapshot::Building()` (th-prep's accessor, cherry-picked as
703f1d543). Outside one it goes into `Snapshot::Current()` (loading screen thread, frame-only main-thread frames), as before. A
holder that already has a stream marks a cadence repeat: GFx draws again and nothing is recorded.

**Render-owned state, slot by slot (native branches):**
- Safe on the producer: the recorder and its mirror, `NativeTargets` (GFx's own target stack), `ReadTargetViews` /
  `ReadRendererTarget` (CS objects' fields), `UiResources::AcquireTempTarget` / `ReleaseTempTargets` (GFx's pool; textures are
  made through the device from any thread; stream references keep replaced textures alive), `UiResources::Release`,
  `Open/CloseStream` (one atomic), `VideoModeSet`, texture creation and uploads (queued; `ApplyUploads` runs in the replay).
  Their docs now say "UI producer".
- Moved off the producer: BeginDisplay's engine target read (`ReadTargets`: the shadow state's target and the renderer's
  target array). A record-only stream marks the display (`command.c`) and keeps GFx's current target as the fallback. The
  replay reads the engine's targets (`ReadEngineTargets`, RecordTime). DrawInterface (AE 0x140fa4f00) binds slot 0 and depth
  at its start and does not undo them, so the targets read after it are the ones read during it.
- Replay side only: `RunReplay`, `StartReplay`, `ApplyUploads`, `Retire::AfterThisFrame`, and the repeat check (now keyed on
  `Replay::again`, a stream replayed again).

**The replay entry.** `void Scaleform::ReplaySnapshotUi(const Snapshot::FrameSnapshot&)` runs on the render owner (fatal
otherwise). It does the UI point's render-side work (`UiPoint`: Upscaling's PostDisplay, the clean-scene copy, HDR's
SetUIBuffer, `Util::SetTemporal(false)`), then replays the snapshot's stream whole from its header, then sets temporal back.
In native, the features' detours on 82084 (Upscaling, HDR, Hooks.cpp) just call through (`Scaleform::OwnsUiPoint()`).
Guest and plain D3D11 are unchanged. The lead calls `ReplaySnapshotUi(*Snapshot::Current())` on the render thread where
Main::DrawFrame called DrawInterface, after VL, before Renderer::End. A repeat calls it again.

**What the game thread opens.** `Scaleform::ProducerScope` around its call of `MenuManager::DrawInterface`, in a game update
whose snapshot `Building()` returns. The detour then records only, into `Building()->ui`, and never replays.

**Found, for S1/S2:**
- DrawInterface's own body writes render state before GFx draws: `Renderer::SetRenderTarget` on slots 0-2, the depth target,
  `Renderer::ApplyState`, a `UpdateViewPort` per menu, and at the end `ImageSpaceManager::RenderEffect(0x22)` when the target
  is not the framebuffer. On the game thread those race the render thread's shadow state. They need porting out, or a
  DrawInterface replacement that calls each menu's PostDisplay only.
- The snapshot's menus are captured before the update's UI jobs. A menu that opens in those jobs draws in the same update's
  DrawInterface. The S1 routing decision needs the menus as of DrawInterface's start (capture them after
  `WaitForJobTask(0, 2)`), or that frame draws a menu scene out of order.
- `OffscreenPreDisplay::thunk` runs PreDisplay at once off the render owner, so on the game thread book and map frames would
  draw engine work there. Those frames are not routed (TH-a).
- `UiResources::TakeStatsLine` reads the temp pool's sizes from the census thread. Harmless today; a race once the producer has
  its own thread.

**Decided by the agent, to confirm:**
- U-a. In record-only frames, the UI point's work (PostDisplay, clean scene, temporal off) now runs after DrawInterface's
  engine prologue instead of before it. Nothing in native reads the engine viewport or jitter between them. Upscalers and HDR
  shots pass.
- U-b. A cadence repeat is detected by the holder already having a stream, not by `Render().repeat`, so the producer reads no
  render identity. In a per-display repeat, book pages from a due PreDisplay are now drawn once, from the snapshot's stream.
  Before, they were drawn from a freshly recorded stream and again from the snapshot's.
- U-c. Engine render work inside a record-only DrawInterface (menu scene, ImageSpace effect, local map) is counted and logged,
  not fatal. After the live UI check every run shows 0, but the predicate is not proven complete (non-pausing menus with 3D
  scenes, if any).
- U-d. UiPoint uses `upscaling.loaded` / `hdrDisplay.loaded` to choose the work, as the detours' install conditions do. Nothing
  clears `loaded` after PostPostLoad.

Runs (Dev-Fast, stage `s7-thui`):

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick | s7-thui-nvk-quick | passed (shot reference); left the world before the first record-only frame |
| native-vulkan+menu-ui | s7-thui-nvk-menuui | passed; record-only from snapshot 29, HUD correct in the closed shot |
| native-vulkan+menu-scenes | s7-thui-nvk-menu-scenes | passed; 2 menu-scene calls inside a record-only DrawInterface (the inventory's first frame): led to the live UI check |
| native-vulkan+upscalers | s7-thui-nvk-upscalers | passed (dlss, fsr, taa, xess); 229 record-only |
| native-vulkan+cadence-repeat | s7-thui-nvk-cadence-repeat | passed; 137 repeated renders, 297 segments as in the first render, 0 differ |
| native-vulkan+cadence-drop-cells | s7-thui-nvk-cadence-drop-cells | passed; 351 record-only |
| native-vulkan+hdr | s7-thui-nvk-hdr | passed (6 shots); 179 record-only |
| guest+quick | s7-thui-guest-quick | passed |
| native-vulkan+menu-scenes (live UI check) | s7-thui-f-nvk-menu-scenes | passed; 222 record-only, 0 engine calls inside them |
| native-vulkan+ui-menus | s7-thui-f-nvk-ui-menus | passed (hud, inventory, map, console); 0 engine calls inside record-only |
| native-vulkan+menu-ui | s7-thui-f-nvk-menu-ui | passed |
| native-vulkan+quick | s7-thui-f-nvk-quick | passed |

Every run: 0 GFx calls on the render owner outside DrawInterface. The game's renderer was called for nothing. Skipped:
native-d3d12 (no ORG, BasicRHI or device change).

### S1: CS's game frame for routed updates (135f05e48, still one thread)

Code: `src/RenderGraph/Engine/Native/RenderThread.{h,cpp}` (game frame, render half, later the thread), `Snapshot/Publication.cpp`
(`MainUpdate_DrawFrame`: `Build()` without publishing, then `RoutedUpdate` or the old publish + Main::DrawFrame), `Native/RenderScene.*`
(`PrepareRouted`, `RecordRouted`, `InGameHalf()`, `Prepared::camera`/`sceneTicket`), DCLF `FrameDriver` (game/render ticket split, scene
pairing), `Revision` (`FragmentSource::take`, `TakePostedFragment`), `Scaleform/Renderer.cpp` (`ProduceUi`, `RecordsUiOnly`, the
replay's target rule).

- **Routing** (`RenderThread::Routes`, after the capture): native, `Scene::Owns()`, the snapshot is World, not paused, no map, and
  MenuControls' camera path test (SE 51363 / AE 52210) is off. A menu the update's UI jobs open is caught at DrawInterface's point:
  `Scaleform::RecordsUiOnly` asks the UI again (th-ui's live check); when it says no, the payload's `interfaceInline` is set and the
  render half calls the engine's DrawInterface itself on the render owner (per display, as before). S2 parks the render thread and
  renders that update on the game thread.
- **Game frame** (`RenderThread::GameFrame`, game thread, in DrawFrame's order): 82085 PreDisplay; `gIsInMenuMode2 = 0`
  (SE 516934 / AE 403436); the thread's scrap heap mark (66889 / 68150); WorldPtToScreenPt3's camera (50744 / 51639); the driver's
  game half (`PrepareRouted`: DCLF's frame start makes the ticket only, `SceneGameStart`); the release (66890 / 68151); the TVL flags
  (528190+528189 / 415135+415134) and target (513720 / 391674) into the payload; FrameJobs (34551 / 35406) and WaitForJobTask(0, 2)
  (68167 / 69516); the interface (`ProduceUi` or inline); the save screenshot request (35888 / 36859, the frozen branch's 36857/36858
  are not asked: no menu freezes the background in a routed update); the scene graph's upkeep (74986 / 76748); the render passes'
  returns (100722 / 107502). Not ported: the debug camera path toggle (52211), an update with the camera path running is unrouted.
- **The update's order** (`RoutedUpdate`): game frame, `EngineReadWindow::Close`, `SideEffects::AfterRenderHalf` (S1/S2/S4/S7, stage
  events 5-9), the update ends, `Testing::OnGamePresent` (the steps run at the publish), the publish with the payload. S1 then ran the
  render half the cadence's number of times on the same thread.
- **Render half** (`RenderThread::RenderHalf`): Renderer::Lock (75458 / 77243), Begin (CS's detour: claim, adopt,
  `RenderThread::OnAdopted` names the payload's ticket for DCLF's scene take, resize, menus, OnBegin; then the engine's body and
  FaceTint's render frame), `RecordRouted` (DCLF's `SceneRenderStart` from the ticket, `WaterEffects::Render`, then the driver's render
  half), volumetric lighting (99025 / 105676, effect 0x22), the interface (`ReplaySnapshotUi` or the engine's DrawInterface when
  inline), End (CS's Present hook: `RenderFrameEnd`; `Testing::OnGamePresent` is skipped inside a routed render), Unlock (75459 /
  77244), then dynamic resolution's scale (508794 / 380760, 75579 / 77385, 75708 / 77517). DrawFrame's other render-state calls before
  the world (per-frame buffers, the NONE target and depth binds, 77516/77258, 107698/107646) are not made: CS's points bind their own.
- **The interface without render state** (`Scaleform::ProduceUi`): DrawInterface's loop ported (TLS memory context 0x46, the movie
  lock 80605/82732, the menu stack lock SE 525557 / AE 412033, PostDisplay from the topmost menu with flag 0x200 up, 107502). Left
  out: the per-frame buffers, the target/depth binds, ApplyState, the per-menu viewport, VParams = 1 and the closing ISCopy when the bound
  target is not the framebuffer. `ReplaySnapshotUi` ports the binds' only effect the replay reads: a NONE slot 0 becomes kMAIN. The
  ISCopy never ran in record-only frames (th-ui: 0 ImageSpace effects inside them), so it is not ported. `ProducerScope` is deleted.
- **Scene pairing** (3.4 built): `FrameDriver::SceneGameStart` takes the newest posted fragment (`Revision::TakePostedFragment`) into
  `FrameTicket::scene` and installs its publication's claims (no new fragment: the publication the render side adopted last,
  `renderAdopted`, set in the source's `afterAdoption`). At the render's Begin the source's `take` callback (`FrameDriver::TakeAtBegin`)
  hands exactly the expected ticket's fragment, once (`FrameTicket::rendered`); a repeat hands nothing (`FragmentTake::Given` with
  none); a loading screen's or movie's Begin skips the source (as `mainThreadOnly` did off the main thread); other Begins (guest,
  native frames on Main::DrawFrame) take the newest and their game half installs what was adopted, as before (`SceneFrameStart`, which
  keeps TICKET-h's remake for its repeats). A render whose ticket's publication is not the adopted one draws no DCLF work and is not
  a tap error (`ticketsUnpaired`, on the tickets line). `HoldsScene`, `takes` and `mainThreadOnly` are gone. The game side keeps
  `gameTicket`; the render side `ticket`, retired through the engine lane when replaced.

### S2: the render thread (49aabed95)

Commit 49aabed95: `RenderThread.cpp` `Loop`,
`OnPublished`, `Park`, `Running`, `OnRenderThread`, the Main::DrawFrame entry detour (35556 / 36555) that parks; the loading loop
(`RunLoadingScreen` thunk) and `MovieTickScope` park too. `RoutedUpdate` publishes and returns (`OnPublished` starts the thread at the
first routed update and wakes it after a park); a routed update whose interface is drawn by the producer parks and renders on the game
thread. `PaceGameUpdate` runs after a routed update while the render thread runs; `CS_UPDATE_RATE_CAP` is deleted (SwitchRegistry,
switches.json, update-cap30/240 configs). The cadence test mode on the thread: `drop:N` holds a dropped routed update unpublished
(its events merge into the next publish), `repeat:N` makes the render thread's Adopt keep a routed snapshot until it rendered N times.
Cross-thread fixes so far: LoadHold's state under one lock (`HoldLock`); the animated fade batch's hand-off under `stampsLock`, its
scratch array thread-local.

Status: native quick, guest quick, menu-scenes and menu-ui pass with the thread. **Stopped at the first long-run failure** (s2c
cadence-drop-cells and first-person-switch): an uncovered occlusion-map tap error after 1,300-1,600 frames. The occlusion maps' state
(`FrameDriver::occlusionTaken/Drawn/Wanted`, reset in the render half's `BeginFrame`) is shared with the shadow casters' DCLF calls,
which still run in the game half on the game thread (`PrepareFrame`'s +0x1C4 point: `BeforeShadowCasters` -> `Draws::BeginShadowViews`,
`ShadowCasterKept`, `ShadowCastersSelected`, and the shadow epochs' `beforeShadowCasters`). They decide shadow coverage and the views
from render-side state (TICKET-a's coupling) and now race the render thread's frame (the map named is about 700 scene steps old, which
fits a cross-thread overwrite). Next step: make the casters' DCLF part a ticket product (the game half records the selection and the
views' inputs in the ticket or the light set; the render half decides shadow coverage and lists the views at its frame start from them),
then rerun drop-cells and first-person-switch. The next crash class to expect is the same kind as the fade batch:
DCLF state the game half writes and the render side's commit or frame end reads, which one thread never raced. Find them by running
native-vulkan+quick, symbolizing the crash address against `build/Dev-Fast/CommunityShaders.dll` with VS's `llvm-symbolizer`, and
guarding or handing off the state named. Then: the live-read detector on the render thread, the frame kinds below, R and I, the suite.

### Frame kinds (how each runs with the render thread)

| Kind | Producer and thread | Render thread |
|---|---|---|
| Unpaused world update (routed) | game thread: CS's game frame; render half on the render thread | renders it (and repeats) |
| Routed update whose UI jobs opened a pausing menu or the map | game thread renders it (interface per display) | parked for it |
| Menu over world, paused menus (inventory, journal, book, lockpick, menu scenes), map and local map | game thread, Main::DrawFrame (the engine-drawn local map and book pages draw as before, snap5-localmap2's port untouched) | parked at Main::DrawFrame's entry |
| Main menu | game thread, Main::DrawFrame | parked (never started before the first world update) |
| Loading screen | serving thread (`RunLoadingScreen`) | parked at the loop's start |
| Bink | the movie tick's thread | parked at the tick |
| Main-thread frame outside Main::Update (cell change) | game thread, Main::DrawFrame | parked at its entry |

## 7. Decided by the agent, to confirm

- TH-a. The render thread draws unpaused world updates only; every other frame stays on its producer with the render thread parked
  (3.1).
- TH-b. The render thread keeps calling Renderer::Lock/Begin/End (3.3).
- TH-c. Scene pairing by a fragment the game half takes into the ticket (3.4).
- TH-d. A routed render whose ticket's fragment was held for a revision in preparation, or dropped, draws no DCLF work for those renders
  (counted as unpaired) instead of a tap error. Why: the game half cannot know at its take whether the render's revision adopts the
  fragment alone; the window is a revision's preparation (settings, resize, builds).
- TH-e. `SideEffects::AfterRenderHalf` (S1/S4, stage events 5-9) runs at the end of the routed game frame, not right after the capture
  (decision 34's wording). Why: the game half after the capture still reads and writes the scene graph and the UI (DCLF's frame start,
  the casters, DrawInterface), which the next update's jobs those stages release would race.
- TH-f. DrawFrame's render-state calls around the world and the interface (per-frame buffers, NONE binds, 77516/77258, 107698/107646)
  are not made in a routed render; CS's points and the replay bind their own. The quick and menu-scenes shot references pass.
- TH-g. The debug camera path (52210/52211) is not ported: an update with it running goes through Main::DrawFrame.
- TH-h. LoadHold and the animated fade hand-off use short locks (held for the entry's work, never across a frame) rather than messages:
  neither side waits for the other's frame.
- TH-i. The update-rate cap paces only routed updates while the render thread runs; updates the game thread renders itself (menus,
  the map) are paced by their own Present.
- TH-j. R (resize): with TH-b the engine's resize already runs on the render thread (Begin's body, from `WindowResizeToApply`); porting
  it to the native device's Resize and target versions is left for the cleanup step.

