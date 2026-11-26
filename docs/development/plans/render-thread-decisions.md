# Render thread: decision log

Decisions for the snapshot roadmap ([render-thread-snapshots.md](render-thread-snapshots.md)), newest last. Each entry
says who decided. "User" decisions are settled; "director" decisions were delegated by the user and can be revisited.
Side-effect ids (S1-S25) refer to [render-side-effects.md](render-side-effects.md).

## 2026-11-25: the roadmap (user)

1. **Pacing is fully asynchronous.** The render thread renders as fast as the GPU allows. The game thread publishes when it
   has new data. Neither waits on the other.
2. **A separate update-rate cap paces the game thread** (default 60 updates per second), once the engine's Present no
   longer blocks it.
3. **Port first.** Native ignores the game's renderer. Latching engine draws, reading engine-bound GPU state or calling
   engine render functions for their side effects does not count as done.
4. **Scaleform gets a full native UI renderer**: a 1:1 port of the game's renderer, same shaders and calls, as ORG/BasicRHI
   passes.
5. **Guest mode is out of scope.** It stays on the main thread. Feature code is shared, so the snapshot model has two
   producers: native's capture from game data, guest's capture at today's hooks.
6. **Frame generation multiplies the render rate.** Snapshots carry data only and play no part in timing.
7. **Visual-only animation between snapshots, and each engine side effect's fate,** are decided per feature during its
   port.

## 2026-11-26: phase 0 results

### General rule (user)

8. **Game-side logic stays on the game thread; only rendering splits.** Running the engine's own CPU-only game logic on
   the game thread is correct and is not a "port first" violation. What moves to the render thread is rendering.

### Side-effect fates (user unless marked)

9. **S1, job stage events 5-9:** CS signals them from the game-side publish, at the points the render half did. Cadence
   drop mode already does this for skipped updates.
10. **S12/S13, stealth light lists:** rebuilt by CS from game data. The engine's queued-light processing is not run (it
    also does the renderer's light-attach walk). The `ImageSpaceManager+0xE0` sunlight-scale input still needs its writer
    found.
11. **S17, decal occlusion tests:** skipped; every queued decal is accepted. The test only decides whether a decal is
    placed or dropped. BGSDecalManager's update (AE 15220) moves a decal that needs the test from state 3 to 4 and reads
    the query: a non-zero count places it (state 5: the decal's create vfunc +0x128, then the queue finish), zero drops it
    (state 7). Nothing outside decal placement reads the result, so it is rendering only. The queue must still advance, so
    CS answers "visible" on the game thread (what a skipped test already returns) and clears the pending array. Placement
    of the decal geometry is visual and follows the decal port.
12. **S6, actor pre-cull (`kNotVisible`, `Actor+0x27C`):** decided by DCLF's existing SIMD CPU cull. The engine's culls
    stay off.
13. **S7/S8, roots DCLF does not stand in for:** the same. Extend DCLF's SIMD cull to cover them; no engine cull.
14. **S4, render-unsafe thread mask (director):** CS clears it on the game thread right after the snapshot capture, the
    last game-thread read of the scene graph in a frame. That matches the vanilla meaning (cleared after the world draw
    finished reading the scene). Once every render input comes from snapshots the mask only guards the capture.
15. **S24, PrintScreen and frame capture (director):** native keeps the vanilla key. The game side keeps its part (input
    consumed, screenshot index, INI save, HUD message, the capture toggle's fixed timer delta). The image itself is a
    snapshot event that the render thread turns into a file through CS's screenshot feature.
16. **S23, UI-driven updates (director):** `MenuManager::DrawInterface` becomes a game-side producer, not a render
    function. With CS's recorder owning every GRenderer slot (phase 4) it draws nothing: it advances GFx, runs the menus'
    PreDisplay/PostDisplay (TweenMenu's `PlayerCamera::Update`, the inventory 3D item's load and zoom, the book pages) and
    records the UI stream. In phase 7 CS calls it from the game-side publish instead of DrawFrame. Inside it, only the
    menu scene draws (`Inventory3DManager::Render`, `UI3DSceneManager::Render`) are split: their update half runs, their
    draw half becomes phase 5's native menu scene capture. The engine-render detector counts DrawInterface as a producer
    once the recorder owns every slot.

### Phase 0 choices confirmed (user: "everything else looks good")

17. **Snapshot contract:** published at Main::Update's call of Main::DrawFrame (SE 35565 +0x5D2, AE 36564 +0xA97), adopted
    at the Renderer::Begin detour. `cameras.previous` is the previous published camera; the render side keeps its own last
    rendered camera in phase 2. Settings revision from the frame revision's variable serial until phase 1 gives a game-side
    one. Cut at 512 units or 60 degrees per update; teleport at 1024 units or a cell change without a load. No Movie
    snapshots in phase 0. `DecideKind` stays until phase 7.
18. **Detectors:** `CS_ENGINE_ZONES` stays beside `CS_SNAPSHOT_DETECT` for the Tracy tooling. Read categories come from a
    name heuristic, fixed per site. Cached pointers count once per lookup. Tables log at loads, on demand and at test exit.
    A call site counts when the engine calls it, hooked or not. The live-read total varies by about 270 reads per frame
    between runs of one save, so progress is read per site.
19. **Native UI renderer:** streams are cut per present. CS keeps texgen rows in its own state. Replaying a stream depends
    on the streams before it; a per-stream state header comes before repeated or skipped snapshots (phase 2/7).

## 2026-11-26: phase 1 (the user is away; director decisions unless marked)

Process: while the user is away the director merges validated lead branches into `org_integration` locally and holds
pushes until the user is back.

20. **Core frame block (snap1-core, accepted by the director):** `Snapshot::CaptureCore` fills `BlockId::CoreFrame` first in
    every capture: sun light, DALC, sky flags, sun and moon, near/far, FOV, the interior rule, water tiles around the main
    camera, sky geometry keys. `State::UpdateSharedData` and `ModifySharedLighting` read only that block. Each `Util`
    helper serves both producers: it reads the game inside a capture scope and the block everywhere else. The settings
    revision is `CS::SettingRegistry::Serial()` (bound `CS::Setting` changes). Display size comes from the `DisplaySize`
    frame variable. VR makes no snapshots and keeps the block at defaults.
21. **Open from core:** near/far still come from the high actor culler's frustum copy, which the render half makes, so a
    change shows one frame late. Native moves to the world camera's frustum when the frame camera is ported. The
    frame-buffer camera matrices (`frameBufferCached`) wait for that frame-camera port too.
22. **One block per feature** (snap1-sky). No shared sky block: shared values come from `CoreFrame` and `SkySync::Block`.
23. **World-state clocks step once per new snapshot by `events.gameDelta`:** lightning, the cloud wind (which also drives
    the storm) and the wetness rain clock. A repeated snapshot holds them; a dropped one loses no time. The aurora keeps
    its wall-clock time. Moving visual-only clocks to render delta is decided per feature in phase 2.
24. **Game logic hooks open a capture scope** (decision 8 applied): Sky Sync's `Sky::Update` hook reads the game as game
    logic, not as record-time reads.
25. **Reads of same-frame engine render output stay record-time until phase 3** (for example Wetness's rain occlusion
    projection): capturing them a frame early would lag guest. Phase 3 replaces the engine output itself.
26. **`frameCount` in graph predicates and commits becomes `FrameClock::BeginFrame()`** (a render-frame count), since every
    use found means "render frame", not "new game frame".
27. **Visual-only post animation advances by render delta** (snap1-post): glare, lens, ENB adaptation, ENB DoF and the FX
    timer use `Snapshot::VisualDelta()` (render delta, 0 while paused). Auto exposure uses the snapshot's `realDelta`
    until phase 2 moves it to render delta.
28. **Grass collision is gathered in the capture** and its decay steps once per new snapshot. The old Main::Update hook
    after DrawFrame is gone. Colliders are now gathered one update earlier relative to the snapshot camera.
29. **The Cinematic Camera's update, including its world-FOV write, runs in the capture** (decision 8: it is game logic).
30. **Writes into engine state are not live reads:** the jitter undo, dynamic-resolution locks, dirty flags,
    `SetCameraData`, the vanilla SSAO toggle and INI writes are tagged Other in the detector. Their removal is phase 2/3
    (no render-side writes into engine objects).
31. **The frame camera is its own item** (director): the frame-buffer camera matrices (`frameBufferCached`, 53 sites) are
    what every phase 1 group still reads as bound state. Native computes them from the game camera in the capture; the
    render side keeps its own last-rendered camera per view and applies render-owned jitter. It is done together with
    phase 2's motion work, since the drop-mode smear and the repeat-mode singular Streamline matrices come from it. Light
    Limit Fix's `FrameInputs` folds into a block after it.
32. **Engine render-half output read the same frame waits for phase 3** (extends 25): Light Limit Fix's light walk and
    local shadows, Terrain Blending's depth-pass state, Skylighting's occlusion-map calls, Exponential Height Fog's engine
    local shadows.
