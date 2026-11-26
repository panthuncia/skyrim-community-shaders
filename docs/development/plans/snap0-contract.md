# Phase 0, items 1 and 4: the FrameSnapshot contract and the cadence test mode

Lead: `contract`, branch `snap0/contract` (base `org_integration` 8fb9b1dc5). Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md).

## 1. The contract (for phase 1 leads)

Code: `src/Snapshot/FrameSnapshot.h` (types, doxygen), `src/Snapshot/Publication.{h,cpp}` (build, publish, adopt, cadence).

### Life of a snapshot

| Step | Where | Thread | What |
|---|---|---|---|
| Build | `Snapshot::Capture()`, at Main::Update's call of Main::DrawFrame (SE 35565 +0x5D2, AE 36564 +0xA97) | main (game) | core fields from game data, then every registered feature capture |
| Publish | `Publish()` in `Publication.cpp` | main (game) | latest wins (`org::async::LatestSlot`). The producer first takes back a snapshot the render side never adopted and merges its events into the new one |
| Adopt | `Snapshot::Adopt()`, in the Renderer::Begin detour (`Frame/FrameDriver.cpp` `RendererBegin`), right after the frame clock moves and before `OnBegin` | main only | takes the newest if one is new; otherwise the render frame is a repeat of the snapshot it had. Loading-screen frames (serving thread) keep the last adopted |
| Read | `Snapshot::Current()` (`std::shared_ptr<const FrameSnapshot>`) | render owner | the only game input recording should read. `Snapshot::Render()` is the render identity |

Both modes publish and adopt. Guest adopts the snapshot its own update just published, on the same thread, so nothing it draws changes.

### Identity

- Game identity, in the snapshot: `id` (1, 2, ... one per game update), `gameTime` (sum of game deltas, stops while paused),
  `gameDelta` (`RE::GetSecondsSinceLastFrame`, 0 while paused), `realDelta` (`BSTimer::realTimeDelta`), `gameDaysPassed`.
- Render identity, `Snapshot::Render()`: `frame` (FrameClock's begin frame at the adoption), `delta` (wall clock between
  adoptions), `repeat` (nothing new adopted), `rendersOfSnapshot`.
- Rule for phase 2: "a new game frame" means a new `id`. Time that belongs to the world advances by
  `events.gameDelta` (summed over replaced snapshots) once per new id; visual-only time by `Render().delta`.

### Fields (phase 0)

`id`, `gameTime`, `gameDelta`, `realDelta`, `gameDaysPassed`, `kind` (world, menu over world, main menu, loading, movie),
`loadEpoch` (loading screens begun), `menus` (main, loading, map, stats, console, freezes background, paused, menus open),
`settingsRevision`, `cameras` (current and previous per `ViewId`; phase 0 has `Main` only), `interior`, `scene`
(`std::shared_ptr<const DCLF::ScenePublication>`, empty in phase 0), `ui` (`UiCommandStream`, empty), `events`, and the
per-feature blocks.

### Events

`Events` is a flag set plus counters: CameraCut, LoadStarted, FirstAfterLoad, Teleport, MenuOpened, MenuClosed,
KindChanged, Paused, Resumed, SettingsChanged; counters `gameUpdates` (updates this snapshot stands for), `loadsStarted`,
`menusOpened`, `menusClosed`, `cameraCuts`, `teleports`, and `gameDelta` (summed). A snapshot replaced before adoption is
merged into the next (`Events::MergeOlder`: flags OR, counters add), so a render frame sees every event since the last
snapshot it adopted. Thresholds: cut = main camera moved over 512 units or turned over 60 degrees in one update, or the first
snapshot after a load; teleport = player moved over 1024 units or changed cell without a loading screen.

### Per-feature blocks

1. Add a line to `Snapshot::BlockId` (`FrameSnapshot.h`).
2. Declare the block in the feature's header: a value struct with `static constexpr Snapshot::BlockId kId` and a defaulted
   `operator==`. No engine pointers.
3. Write `static void Capture(Snapshot::Builder&)`: read game data, `a_builder.Set(block)`. Register it with
   `Snapshot::RegisterCapture` in `PostPostLoad`. It runs on the game thread inside the capture scope.
4. Record code reads `Snapshot::Current()->Block<T>()` (null before the first snapshot or when the feature captured none).

`Builder::Set` compares with the previous snapshot's block and keeps its pointer when equal, so an unchanged feature costs
one comparison and no allocation; a repeated snapshot costs nothing.

Pilot: **Tree Wind** (`Features/TreeWind.{h,cpp}`). `TreeWind::Block` holds the sky's wind speed and angle, the weather
blend and the two weathers' wind data. `Reset` (after each Present) reads only the block and the core fields: the clock
steps once per new snapshot by `events.gameDelta`, so a repeated snapshot holds the trees still and a dropped one does not
lose time. The map-menu test reads `menus.map`.

### Capture scope

`Snapshot::Capture` is the one builder function. The `detect` lead's `Snapshot::EngineReads::CaptureScope`
(`src/Snapshot/EngineReads.h`, branch `snap0/detect` 5704779e6) wraps it with one line at the top of its body:
`const EngineReads::CaptureScope scope;` (marked by a comment there). To add at merge.

### Logs and test hooks

- `[Snapshot] first snapshot (...)` and `[Snapshot] load ended (...)`: one line per load with published, adopted, replaced,
  most updates folded into one, render frames, repeats, blocks shared and new, and the capture's mean time.
- The test status file has `"snapshot": <adopted id>`; each `shot` step logs `[TEST] shot <name> asked at snapshot <id> (render frame <n>)`.

## 2. The cadence test mode (item 4)

`CS_SNAPSHOT_CADENCE=repeat:N|drop:N` (N 2-16; switch table row, cs-test key `snapshot.cadence`, kind test). Native only:
guest and D3D11 log a warning and render once per update, so guest is unchanged. A malformed value is fatal.

At the publication hook (`MainUpdate_DrawFrame` in `Publication.cpp`), after the snapshot is published:
- **repeat:N** calls Main::DrawFrame N times for the update. The first render adopts the new snapshot, the others are
  repeats (`Render().repeat`). The whole render half runs again: engine and CS.
- **drop:N** calls it for one update in N. The others publish and skip the render half; the next render adopts the newest
  snapshot, which carries the skipped ones' events (`gameUpdates` up to N).

### What had to be fixed for drop to run

Skipping Main::DrawFrame hung the game in Main::Update's frame-job wait (`JobListManager::WaitForFrameJobs`, SE 34554,
AE 35409): the serving thread waited for a frame job list whose jobs wait for the main thread's render stage events
(FUN_1406d2e80 waits for stage 5 or 6, FUN_1406d3660 for 8 then 9). The render half sets them: 5 in
NiCamera::CalculateAndDrawShadowCasterLights, 6 and 7 in Main::Draw, 6 to 9 in Main::DrawFrame. A skipped update now
signals stages 5 to 9 itself (`SignalRenderStages`, SetJobTaskEvent SE 68165, AE 69514). **This is an engine render-half
side effect the game reads**: add it to the side-effect inventory. When native stops calling the engine's render half
(phase 3-5), the game thread must signal these stages itself.

No other change was needed: the Present hook, FrameClock, State::Reset, DCLF's FrameDriver and SceneStep, the load hold and
the test steps all ran. Steps count Presents (render frames), so in drop mode a step's frames are N game updates each.

### Results (native Vulkan, the autoloaded river save, Dev-Fast)

| Run | Result | Snapshots vs render frames |
|---|---|---|
| `native-vulkan+cadence-repeat` (`s0-contract-repeat`) | passed, 3 shots | 1 snapshot per 3 renders (load: 10 adopted, 20 repeats) |
| `native-vulkan+cadence-drop` (`s0-contract-drop`, `-drop2`) | hang at held-frame (before the fix) | |
| `native-vulkan+cadence-drop` (`s0-contract-drop3`) | passed, 3 shots | about 3 snapshots per render (load: 4 adopted, 6 replaced, at most 3 folded) |

### What breaks (feeds phase 2)

| Seen | Mode | Likely cause | Where |
|---|---|---|---|
| Strong smear and blur on the whole frame while the camera or actors move (the "start" and "turned" shots) | drop | Motion vectors and TAA reprojection use the previous *game* update's camera and transforms, which were never rendered: the history is three updates old | engine previous view-projection in `globals::game::frameBufferCached` (`Globals.cpp:309`), DCLF `Core/Gpu/FrameValues.cpp:192-197`, `PostChain.cpp:241-268` |
| `[Streamline] singular camera reprojection matrices` on 64+ frames (1 in the baseline) | repeat | On a repeated render the cached frame-buffer camera matrices are singular (not refreshed or zero on a repeat), so Streamline gets identity reprojection | `Features/Upscaling/Streamline.cpp:1050-1080`, `frameBufferCached` (`Globals.cpp:309`): to investigate |
| One engine Lighting draw of a skinned actor (`BSDismemberSkinInstance`, from Main::RenderWorld) dropped by the tripwire context, 1.3 s into the world | repeat | DCLF did not claim that actor's draw on a repeated render: the engine re-ran its accumulation and drew it, so it is missing for that frame (flicker); DCLF's per-frame claims assume one render per update | `[ORG] engine draws` (`RenderGraph/Engine/Flush/Flush.cpp`), DCLF frame set and `FrameDriver` BeginFrame/EndFrame (`Core/Render/FrameDriver.cpp:164-398`) |
| `GraphHost: epoch 16 has no published copy to submit ... EpochCopiesBusyError`, once | repeat | Renders three times as fast as updates filled the epoch's four copies before a seal; that frame's runs were skipped | ORG `GraphHost`, `RenderGraphRuntime::SealPresented` (`Hooks.cpp` Present hook) |
| Time-based effects run N times faster in game time | repeat | Anything stepped once per render by the game delta: DCLF fades (`Fades/FadeState.cpp:257-319`), auto exposure (`HistogramAutoExposure.cpp:80`), clouds, lightning, wetness, LODBlendRingFix, `State::Reset`'s `timer` (`State.cpp:426`). Tree Wind no longer does (pilot) | watch list in the roadmap |
| Engine UI and menus advance once per render | both | Main::DrawFrame calls the UI's per-frame functions (`UI` FUN_140fa5300 and FUN_140fa4f00 in AE 36555): menus animate N times faster in repeat and N times slower in drop | Main::DrawFrame (AE 36555) |
| frameCount-keyed work runs per render | both | `globals::state->frameCount` moves per Present: Skylighting's quadrant, pre-exposure, TreeWind's bend-weight eviction, test step counters | `State.cpp:413-448` |

The guest and native quick runs show no change from the snapshot plumbing (same warnings as before).

Other runs: `list-switches` (`s0-contract-switches`, passed; schema regenerated, only the new row added),
`guest+quick` (`s0-contract-guest`, `s0-contract-guest-final`: passed), `native-vulkan+quick` (`s0-contract-nvk`,
`s0-contract-nvk-final`: passed). Both quick runs publish one snapshot per render frame in the world (e.g. snapshot 143
at render frame 920, world from 865) and log the two `[Snapshot]` load lines.

## 3. Decisions by the agent, to confirm

- **Publication point**: Main::Update's call of Main::DrawFrame, as the director proposed. SE pair found in Ghidra:
  SE 35565 +0x5D2 (AE 36564 +0xA97), one call in each. Startup and loading-screen frames (serving thread) publish nothing;
  they keep the last adopted snapshot.
- **Adoption point**: the Renderer::Begin detour, right after the frame clock moves, main thread only, both modes.
- **Latest-wins slot**: OMS `LatestSlot`, with the producer taking back an unadopted snapshot before it posts (two pointer
  exchanges, one producer and one consumer). Lock-free; the slot's doc says one consumer takes, and the producer's take is
  only of its own unadopted value.
- **`cameras.previous`** is the previous *published* snapshot's camera, not the last rendered one. Phase 2 needs the
  render side's own previous (drop artefact above), so it is named as such in the header.
- **Settings revision**: `Revision::Mark().takenVariables` (the frame-variable change serial the last Begin took). A
  game-side serial would be better; phase 1 can replace it when settings reach the frame only through revisions.
- **Movie kind** exists but phase 0 never captures one (Bink frames do not pass through Main::Update).
- **Event thresholds**: cut at 512 units or 60 degrees per update; teleport at 1024 units or a cell change without a load.
- **Tree Wind pilot**: reads `menus.map` from the snapshot instead of `State::isMapMenuOpen` (one update fresher), and its
  clock steps once per new snapshot (during a loading screen it holds; it used to advance with a stale delta, unseen).
  Guest draws the same otherwise.
- **The FrameDriver's `DecideKind` stays** as the render side's kind. The snapshot's kind is computed on the game side from
  the same inputs; phase 7 moves `DecideKind` into the capture.
- **Cadence configs** are run with the worktree's `cs_test.py` (its schema has the new key) and
  `CS_TEST_LAB_HELPER` pointing at the main checkout's helper: the main checkout's schema does not know the switch yet.

## 4. Open items

- Add the capture scope line once `snap0/detect` merges.
- Investigate the repeat-mode singular reprojection and the skinned-actor engine draw.
- Port further blocks by feature group (phase 1); fill `scene` when DCLF's publication pairs with the snapshot.
