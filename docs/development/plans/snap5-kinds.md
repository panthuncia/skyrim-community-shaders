# snap5-kinds: every frame kind as a snapshot (roadmap phase 5, "Frame kinds")

Lead `kinds`, branch `snap5/kinds` (base `org_integration` f23e4f779), stage `s5-kinds`. Roadmap:
[render-thread-snapshots.md](render-thread-snapshots.md), phase 5 "Frame kinds" and phase 7's "Frame kinds" bullet. Contract:
[snap0-contract.md](snap0-contract.md). Decisions 17, 49-53, 60-65.

## 1. What changed

Every render frame adopts a snapshot, and the frame driver's kind is the adopted snapshot's kind. Nothing on the render side
reads the menu stack or the player to decide a kind any more.

| Frame | Producer | Snapshot |
|---|---|---|
| Game update (world, menu over world, main menu) | `Snapshot::Capture`, Main::Update's call of Main::DrawFrame (unchanged) | next `id`, `sub` 0 |
| Loading screen (serving thread, `RunLoadingScreen` SE 38085 / AE 39039) | `Snapshot::CaptureFrame(LoadingScreen)` at its Renderer::Begin | kind Loading, frame-only |
| Bink movie (startup logo, intro, a movie in game; `BSMoviePlayer::Tick`, any thread) | `CaptureFrame(Movie)` at the tick's Begin | kind Movie, frame-only |
| Main-thread frame outside Main::Update (TES cell change AE 13316, the RenderTest console commands) | `CaptureFrame(MainThread)` at its Begin | kind decided as for a game update, frame-only |

`FrameDriver.cpp`'s `DecideKind` now only maps the adopted snapshot's kind (Movie -> Bink, Loading -> Loading, MenuOverWorld ->
FrozenWorld, World, MainMenu). The frozen background's one capture is still a render-side streak rule (first FrozenWorld frame
since a non-frozen main-thread frame), now gated on the snapshot's new `worldLoaded` (player has 3D and a parent cell) instead
of a live player read. Main's second freeze test (`FreezeTestAtBegin`) returns the snapshot's `menus.freezesBackground`.

### The kind rule (main thread)

A game update's kind is now the rule Main's frame uses to choose what it draws, the one `DecideKind` applied at Begin:
a menu that freezes the background -> MenuOverWorld; else player 3D -> World; else MainMenu. The old snapshot rule put
"loading menu open" and "main menu open" first, so a main-thread frame drawing the world under a loading menu (the load hold's
held frames, the updates between a coc and the serving loop) had snapshot kind Loading while the render side drew World. Loading
is now only the serving thread's kind. Consequence for the two snapshot-kind readers: SideEffects' fade-enable count and actor
pre-cull, and ImageSpace's adaptation-reset take, now run on those world frames too, as the engine's render half did.

### Id rule for frame-only snapshots

`FrameSnapshot::sub` (new): a game update's snapshot takes the next `id` with sub 0; a frame-only snapshot keeps the last game
update's `id` and takes the next sub. So (id, sub) orders every snapshot, "a new game frame" is still a new id (features that
step on an id change don't step on loading frames), and cs-test's shot-to-id mapping is unchanged. Before the first game update
the id is 0 (startup logo and loading screen).

A frame-only snapshot copies the last snapshot (blocks by pointer, cameras with previous = current, scene, game time, settings
revision, interior) and sets its own kind, menus, load epoch, a fresh UI stream holder (native) and events: `gameUpdates` 0,
game delta 0, LoadStarted if a load began since the last snapshot, kind and menu events against the last snapshot. FirstAfterLoad
stays a game update's event. A game update's snapshot replacing an unadopted frame-only one merges its events as before.

### Load epoch

Unchanged source (`SetLoadingScreenActive(true)` moves it), now carried by loading snapshots too. `Snapshot::LoadEpoch()` (new)
reads it from any thread. The first loading snapshot carries LoadStarted (so `HistoryBreak` fires on it in native).

### Two producers, one consumer

The game update's publish runs on the main thread outside Renderer::Lock; the frame-only publishes run at Renderer::Begin, inside
Renderer::Lock, so they are serialised among themselves. The serving thread renders only while the main thread sits in the load
coordinator (SE 13214 / AE 13363) between SetLoadingScreenActive(true) and (false), and the startup logo plays before
Main::Update first runs, so the game update's publish never meets a frame-only one. A `PublishClaim` (one `atomic_flag`) checks
it: a producer that finds another under way is fatal, never waits. The slot stays OMS `LatestSlot`, latest wins, the producer
takes back what was not adopted. Adoption now runs at every Begin on the thread that renders it; Begin runs inside
Renderer::Lock, so adoptions never overlap and the render owner reads what its own Begin adopted.

### UI stream

`MenuManager::DrawInterface`'s detour (Scaleform `Renderer.cpp`) sets the finished stream into whatever snapshot the frame
adopted when it is the snapshot's first render (no longer only inside a game update), and replays the snapshot's stream on any
repeat. Loading screens' streams are now in their snapshots. The loading screen's model (the Mist Menu's scene) is phase 5's
menu-scene port, untouched here.

### Menu flags (decision 52's render-side read is gone)

`State::isMainMenuOpen`, `isLoadingMenuOpen`, `isMapMenuOpen`, `isStatsMenuOpen` are set at each Begin from the adopted snapshot
(`State::AdoptMenus`); `State::Reset` no longer reads the UI. `IsMainOrLoadingMenuOpen(ui)` keeps its live fallback (callers on
the game side).

### Load hold counts snapshots (LoadHold.cpp, DCLF's file: the counting change only)

A held world frame counts only on the first render of a game update's snapshot of the hold's load epoch (repeats, frame-only
snapshots and snapshots captured before the load don't count); `hold.frames` adds the snapshot's `gameUpdates`. The hold's load
epoch is taken at its start (`Snapshot::LoadEpoch()`; a re-entry takes the adopted snapshot's). Settled frames still need the
render side's DCLF progress, so in drop mode a counted frame stands for N updates.

### Bink

Movie frames are Movie snapshots (kind and identity). The decoded frame is still the render thread tick's decode into staging
(CS draws the tiles); moving the decode into the snapshot is phase 7's.

## 2. Runs

(filled below)

## 3. Decided by the agent, to confirm

- The main-thread kind rule is Main's frame rule (freeze, 3D), not "loading or main menu first"; Loading is only the serving
  thread's kind. Changes what SideEffects and the ImageSpace adaptation-reset do under a held loading menu (they now run, as the
  engine's render half did).
- Frame-only snapshots keep the game id and add a sub-id.
- Frame-only snapshots copy the last snapshot's world data instead of carrying no blocks, so readers see what they saw before.
- The freeze test (walks the menu stack) is asked only on the main thread; serving-thread and movie snapshots say false.
- Two producers checked by a fatal claim, not a lock.
- Upscaling's frame-generation `gameplay` test needs id != 0 (it was "a snapshot exists"; the startup logo now has one).
- VR (no publication hook) publishes a frame-only MainThread snapshot at every Begin, so its kinds come from snapshots too. Untested.
- Guest: kinds come from the capture instead of Begin (same update, microseconds apart); menu flags from the capture instead of
  the previous frame's end.
- The hold's `frames` (quiet rule) counts game updates; its settled count counts rendered game snapshots.

## 4. Open items

- The loading screen's Mist Menu model (phase 5 menu scenes).
- Bink decode into the snapshot (phase 7).
- `FaceGen::UpdatePending` in RunLoadingScreen is still a Render-role engine call on the serving thread.
