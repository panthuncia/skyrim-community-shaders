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

## 2026-11-26: phase 3 game-side side effects (snap1-sidefx; director accepted)

33. **CS produces the side effects in native and patches the engine's own write out.** `Snapshot/SideEffects.cpp` runs
    on the game thread around the render half (`AfterCapture`, then the render half 0..N times, then `AfterRenderHalf`).
    Each engine patch checks the original bytes and is fatal on a mismatch. Guest installs nothing.
34. **S1 and S4 happen after the render half until phase 7** (refines 9 and 14): the render half still reads the live
    scene graph in phases 0-6, so stage events 5-9 and the render-unsafe clear wait for it. Phase 7 moves both to right
    after the capture. The cost was not measured uncapped yet.
35. **S2:** `gFrameCounter` moves once per game update (CS increments it; Renderer::End's increment is removed). Render
    code that meant "once per rendered frame" keys on the render frame, not a snapshot copy of the counter.
36. **S5:** CS counts the 20 fade-enable updates after a teleport itself, on world snapshots only.
37. **S6:** the actor pre-cull is the `org::cpu` SIMD sphere cull against the snapshot camera's frustum, on world snapshots
    only; it writes `kNotVisible`, bit 12 and Actor +0x274 (SE) / +0x27C (AE). The engine culler runs only its
    initialising call, and CS does the frustum setup the culler used to do (CS's own capture reads it). Parity: 5,009
    verdicts, 0 differences. The occluder test is not repeated (a root in view counts as not occluded). A sphere exactly on
    a plane is kept, where the engine culled it.
38. **S17:** queued decals go straight from state 3 to state 5 (placed) and the pending array is cleared.
39. **S7/S8 fades** step once per game update a snapshot stands for (0 on a repeated render); guest is unchanged.
40. **The stealth sun term needs no producer:** `ImageSpaceManager+0xE0` (sunlightScale) is written game-side in
    Main::Update. S12/S13 (light membership from game data) and S7/S8 for roots DCLF does not stand in for are still to
    build; designs are in `snap1-sidefx.md`.

## 2026-11-26: phase 2 frame camera (snap2-camera; director accepted)

41. **The frame camera (snap2-camera):** the capture stores SetCameraData's unjittered matrices per view
    (`CameraView::view/proj/viewProj`). Native builds the engine's per-frame block (b12) itself once per render frame
    (`Snapshot::FrameCamera`): `Main()` jittered for the world passes, `Post()` unjittered for the sun query and the post chain,
    with ratios of 1 once the frame is at full resolution. The previous camera is the one this render side drew last. Guest and
    D3D11 keep `frameBufferCached`. SharedConstants' Start, PreDepth and Ui segments keep the engine's block until those views get
    snapshot cameras.
42. **Render-owned jitter (snap2-camera):** native never writes `projectionPosScale` or the jitter index into
    `BSGraphics::State`; game-side picking and culling see an unjittered projection. Guest keeps today's writes.
43. **Drawn previous transforms (snap2-camera):** in native a DCLF row's previous world is the world this render
    side last drew for the same geometry in that slot (LOD and first person excepted); sky and water take the previous posAdjust
    from the frame camera. Skinned bone palettes still use the engine's previous update (open).

## 2026-11-26: phase 3 lights (snap3-lights; director accepted)

44. **Stealth light level from CS's light membership** (S12/S13, decision 10 built): in native, `CalculateLightValue`
    (SE 38900 / AE 39946) sums over the membership `ProcessQueuedLights` would leave (active and shadow lists with the
    queued adds and removes applied, and the orphan sweep), read under `lightQueueLock`, rebuilt at every call with no
    cache. The engine's own luminance function and, for shadow lights, its light pick (with its line-of-sight raycasts)
    still do the per-light work, so occlusion still counts. The engine's own mask loop has its picks thunked to "true"
    because its result is unused. Parity: 0 differences in player sums and in 5,400 membership checks, drop mode
    included.
45. **NPC light sums** use the active lights whose radius reaches the NPC's skin bound, without a portal test: a light in
    another room within range counts. Shadow lights are left out, as in the engine's geometry light lists. Parity showed
    no difference in the scenes run.
46. **S7 for roots DCLF has no verdict for** covers the attached cells' animated references, frustum-tested with the
    shared S6 SIMD cull, written after the render half. It is frustum-only (no portal or occluder test), the same trade as
    decision 37: an object the engine's portal cull hid now counts as visible (about 3.7k of 23k compared roots), never
    the reverse.
47. **The shadow light bookkeeping has no game-side readers** (Ghidra xrefs: free mask, `shadowLightsAccum`, camera data),
    so it needs no producer and goes away with the engine's shadow code.
48. **Shadow caster selection (AE 107132) moves with the scene-lists port**, not on its own: it also sets up the sun
    cascades, runs the main list-accumulation jobs and picks the local lights. S8 fades for the decision 46 roots go with
    it.

## 2026-11-26: phase 2 counters and cadence (snap2-cadence; director accepted)

49. **Render counters and game time are separate.** `FrameClock` and `frameCount` are render counters. `State::Timer()` is
    the snapshot's game time (`State::timer` is gone) and `State::Reset` only ends the render frame. World state steps by
    `Snapshot::GameStep()` (LOD blend ring, skin wetness; Dynamic Cubemaps uses game time); auto exposure adapts by render
    delta. DCLF's `SceneStep` stays a render-frame identity (its uses are same-frame checks); its scene-pump watchdog is
    time-based (30 s without a scene pass).
50. **`Snapshot::HistoryBreak()`** is true on the first render after a cut, teleport or load, merged over replaced
    snapshots, and resets histories (TAA and upscalers, water SSR, scattering, DoF, lens, SSGI, clouds). Native only, so
    guest's pictures at cuts don't change.
51. **The test harness counts game frames.** Steps and milestones count game updates (world-ready needs 30), steps run at
    the update's first Present (running them around the render half let a `coc` crash the engine's cull jobs), shots name
    the snapshot they rendered. Phase 2's proof is `tools/cs-test/cadence.py` with `configs/cadence-still.toml`: cadence
    shots pass at most twice the noise floor between two normal runs, or the floor plus 1.5.
52. **Menu flags stay a render-side read until phase 5**, since the main menu publishes no snapshots. The PCL simulation
    marker waits for phase 7.
53. **Known test-mode gap:** repeat mode with a second `coc` can crash in the engine's list cull, because the repeated
    render half reuses scene lists over unloaded cells; the load guard narrows it but is racy. It goes away when phase 3
    stops running the engine's culls.

## 2026-11-26: phase 5 ImageSpace, stage 1 (snap5-imagespace; director accepted)

54. **The post chain's game inputs are one block** (`BlockId::ImageSpace`): blended ImageSpace data, HDR/TAA/adaptation
    settings, active slots, slot inputs, SSAO and SSR settings, the sun. One set of reader functions serves both producers:
    native in the capture, guest at its old hooks. Native's frame description reads the same block. Parity: 0
    differences on every switched field over 3,037 frames. A native world frame without a snapshot main camera is fatal.
55. **The adaptation reset is a game-side count:** Sky::Update's reset flag is taken on the game thread as a count, and the
    first native HDR effect after the count moves resets, so repeated and dropped snapshots behave.
56. **Values the render half writes stay render reads until phase 3** (extends 25 and 32): refraction's and VL's activity
    and colour, depth of field steps and camera, the full-screen blur's camera test, the water blend's camera and
    posAdjust, portal fog, `saoReflections`.
57. **The engine-render detector does not count CS's own calls through an engine call site** (`EngineZones::CallTarget`).
58. **The vanilla depth of field's `IsActive` includes Post Processing's skip flag** (CS state) and is captured on the game
    thread.
59. **Detector runs are not shot references** until the dark-shot bug is fixed: `CS_SNAPSHOT_DETECT=1` darkens quick-run
    start shots (mean about 18 vs 65). Compare shots only from runs without it. A diagnostic must not change the picture,
    so this is a bug under repair (snap0-detfix), not a property.
