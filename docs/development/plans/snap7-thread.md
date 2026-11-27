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
| P | Game-half accessors and thread-identity cleanups: `Snapshot::Building()` for game-half readers; FrameGlobals scope over the whole game half; `SampleEngine` into the capture; `FrameOnlySource`/`FreezeTestAtBegin` without `IsMainThread`; the engine-read window closed at the update's end; `MakeRegistered` at install; the kept rows handed to the render side as copies or views; the fade statics and the per-render fade kick's camera from the ticket | yes | done (snap7/thread-prep), section 6 |
| U | UI producer record-only (3.5) | yes (record, then replay at once) | see section 6 |
| S1 | CS's game frame in place of Main::DrawFrame for routed updates, game half before the render half, scene pairing (3.2, 3.4), still one thread | yes | |
| S2 | The render thread (3.3), parking, cross-thread checks, PaceGameUpdate on | - | |
| R | Resize on the render thread (native device Resize, swap chain, target versions) instead of Renderer::RequestWindowResize + Begin | - | |
| I | ImGui on the render thread (Menu::DrawOverlay from RenderFrameEnd; it already is) and the harness (shots and live frames on the render thread) | - | |

## 5. Runs

| Config | Label | Build | Result |
|---|---|---|---|

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

## 7. Decided by the agent, to confirm

- TH-a. The render thread draws unpaused world updates only; every other frame stays on its producer with the render thread parked
  (3.1).
- TH-b. The render thread keeps calling Renderer::Lock/Begin/End (3.3).
- TH-c. Scene pairing by a fragment the game half takes into the ticket (3.4).
