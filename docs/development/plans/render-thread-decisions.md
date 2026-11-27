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
59. **Detector runs are shot references again** (fixed by snap0-detfix): the dark shots came from two call sites of AE
    107221 whose seventh argument the old C++ thunk dropped. Each engine call site now jumps to a machine-code stub that
    counts and jumps on to the original target, so any signature is safe to list. Tracy zones at the sites come only with
    `CS_ENGINE_ZONES=1` in Tracy builds; their return-address swap is untested with Tracy connected.

## 2026-11-26: phase 4 finished in-frame (snap4-ui2; director accepted)

60. **Native owns every GRendererD3D1x slot except the destructor** (shutdown only, still counted). GFx textures and render
    targets are CS objects in the D3D1x field layout (vtables copied from the D3D1x ones); creation, updates and the
    render-target stack are CS ports. GFx base-class functions (change and event handlers, user data,
    MakeViewAndPersp3D) stay GFx's: they are GFx core, not the renderer. Textures go through the engine's device handles
    (CS's D3D11 layer). The census reads "the game's renderer was called for nothing" in native.
61. **Lifetime by reference:** streams hold references to the views they draw and uploads to their textures, so a texture
    GFx releases stays valid for any snapshot that names it. These are the per-snapshot references phase 6's retire queue
    takes over.
62. **Each UI stream carries its start state** (`StreamHeader`, decision 19 built): the replay depends on no earlier
    stream. Texgen rows start at zero per stream. Uploads are kept out of the replayable commands: they are queued per
    stream and applied once, in call order, before that stream's draws; a stream never replayed still has its uploads
    applied before the next.
63. **`FrameSnapshot::ui` is written once, at the end of the update's first DrawInterface** (until phase 7). Repeated
    renders replay it.
64. **DrawInterface counts as a producer, not a render call, once CS owns the renderer** (decision 16 built;
    `Role::Producer` in the engine-render detector, refreshed per frame for the call-site stubs).
65. **Loading-screen UI snapshots wait for phase 5:** they need a publish from a second producer (the serving thread),
    adoption there, and an id rule for snapshots that are not game updates. The serving thread already makes no game
    renderer call. Filters, 3D draws and GFx render targets were never observed in a run, so their parity is unproven.

## 2026-11-26: phase 3 shadow casters (snap3-scene milestone 1; director accepted)

66. **Native shadow caster selection:** in native, `SelectCasters` replaces the body of
    `CalculateAndDrawShadowCasterLights` (AE 107132) and CS's `RenderShadowMaps` the body of 107133. CS ports the lights'
    camera updates (sun cascades with smoothing and hull fit, spot, paraboloid and focus lights), the scissor, the
    lens-flare queue, light release and Accumulate's bookkeeping, and writes them into the engine's shadow-light objects,
    as ShadowMapsNative already does (decision 47 holds). Phase 6/7 moves them to render-owned memory. Parity: spot and
    point cameras identical.
67. **No cull, registration, attach walk or engine Render for shadow lights in native.** A light DCLF can't draw casts no
    shadow that frame; every light goes through ShadowMapsNative.
68. **Object creation stays engine calls:** allocation, constructors and the lights' own camera transform updates are not
    rendering.
69. **The sun port's cascade 1 is 88 units wider at its right edge** than the engine's; unexplained in the disassembly,
    accepted because it covers more, so no caster is lost.
70. **The main camera's list accumulation and registration jobs are still the engine's** (milestone 2, the scene lists).
    Native still builds sky and wading-water draws from engine-registered passes and submits CS stages at the
    accumulators' call sites; both must come from game data before the main cull stops. S8 for the decision 46 roots and
    the decision 53 test-mode crash wait for it. Engine call sites that CS hooks keep counting until CS drives Main::Draw
    itself (phase 7).

## 2026-11-26: phase 5 frame kinds (snap5-kinds; director accepted)

71. **Every render frame adopts a snapshot.** Frames with no game update (the loading screen on the serving thread, a Bink
    tick, a main-thread frame outside Main::Update such as the TES cell-change path AE 13316) publish a frame-only
    snapshot at `Renderer::Begin`: the same id with the next `sub`, the last snapshot's world data (blocks, cameras, scene,
    game time) and their own kind, menus, load epoch, events (`gameUpdates` 0) and UI stream. A new game frame still means a
    new id. The frame kind is the adopted snapshot's; `DecideKind` only maps it. Movie snapshots now exist (replaces part
    of 17).
72. **A main-thread frame's kind follows Main's own rule** (the freeze test, then player 3D); Loading is only the serving
    thread's kind. Side effects and the adaptation reset therefore run on world frames under a held loading menu, as the
    engine did. The freeze test is asked only on the main thread.
73. **Two producers never overlap:** frame-only publishes run inside `Renderer::Lock`, and the serving thread renders only
    while the main thread is in the load coordinator. An overlap is fatal (`PublishClaim`), not waited on.
74. **Menu flags come from the adopted snapshot** (`State::AdoptMenus`; replaces 52). In guest, kind and menus are taken at
    the capture.
75. **The load hold counts game snapshots and load epochs:** the first render of a game snapshot from the hold's load epoch,
    plus `gameUpdates`, not render frames. VR publishes a frame-only snapshot at every Begin (untested).

## 2026-11-26: phase 5 ImageSpace rest and screenshots (snap5-isrest; director accepted)

76. **The TAA reset is a game-side count** (`taaResets` in the ImageSpace block): only the loading menu sets the engine's
    flag, on open and close; the first native world render after the count moves resets histories through
    `State::ShouldResetHistory`. `HistoryBreak` alone missed closes that land a few frames after the load. Guest unchanged.
77. **The vanilla screenshot is a snapshot event** (15 built): the capture takes the PrintScreen request before
    Main::DrawFrame, runs the engine's screenshot function on the game thread (file name, HUD message, index, INI save) with
    its one render call redirected to record the name, and the render side saves the frame under the vanilla name in the
    Screenshot feature's format (PNG/AVIF/BMP, not the engine's). One hook covers PrintScreen, capture mode and Papyrus
    `TakeScreenshot`. PrintScreen and the capture toggle in one frame are both handled that frame.
78. **The save screenshot is a request count with a report back** (S15): the render side copies the frame and reports the
    count; the next capture completes the request on the game thread, so a save is written one game update later than
    vanilla.
79. **No-raster ImageSpace steps need no port** (ISMap and ISNoise draw nothing). The image-based lens flare port (about
    30 steps including `ISDownsampleCS`) is deferred: CS forces IBLF off unless the console turns it on.
80. **Known, older than this work:** `screenshot-clean` loses the device after its window-resize step (base build too);
    guest PrintScreen hits a fatal "ISCopy rt0->rt76 has no CS pass"; the save pump writes only the first of two saves in a
    config, in both modes.

## 2026-11-26: phase 7 preparation (snap7-prep; director accepted)

81. **ImGui input arrives only through the queue.** The game side reads UI state that the ImGui frame publishes at its end
    (`Menu::PublishInputState`: input swallowed, editor camera flying, settings window drawn); the UI's writes into game
    state (vanity-camera INI, `ClearInputState` after a focus change) run at the input poll (`Menu::UpdateGameSide`).
82. **Game-side code reads settings as adopted, never live.** GrassCollision's capture is converted; the plain fields read
    on the game thread (Sky Sync's moon settings and hook fields, Cinematic Camera, DoF) and the CS Editor's ~60 direct
    engine writes from ImGui are listed in `snap7-prep.md` for phase 7.
83. **A window resize is a snapshot event** (`Events::windowSize`, newest wins on merge), applied at the render frame's start
    (native). A resize arriving between the capture and Begin waits one game update.
84. **The update-rate cap** is the setting `State::updateRateCap` (Advanced "Update Rate Cap", default 60, 0 uncapped),
    applied by `Snapshot::PaceGameUpdate()` after the render half, and active only under the test switch
    `CS_UPDATE_RATE_CAP=1` until phase 7, so default pacing is unchanged today. The switch goes away in phase 7, when the cap
    becomes the game thread's pacing.
85. **PCL ids:** render markers use the render frame index; simulation markers use the render frame that first renders the
    snapshot. SimulationStart fires once per game update, SimulationEnd once per snapshot. Open for phase 7: once decoupled,
    the render thread learns which frame first renders a snapshot only at adoption; needs a frame-generation run on the
    split build. Win32 `SetCursor` from a non-window thread is left for phase 7.
86. **The phase 7 plan** is the eight-item list in `snap7-prep.md`: thread creation and ownership, Present as
    publish-and-wait, revision adoption, ImGui, settings, resize, PCL, harness.

## 2026-11-26: phase 3 scene lists, milestone 2 (snap3-lists; director accepted)

87. **Native's main sky comes from the sky root:** the engine's main-camera sky cull registers every sky object that isn't
    hidden, with no bound test; native builds the main sky batch and cloud layers from the sky root with the snapshot's
    main camera, sharing the reflection faces' cull and draw loop. The engine's still-registered cloud layers are detached
    at the group 13 call. Parity: 0 of 60 batches differed.
88. **Native's main water batches come from the tracked water shapes** the water epochs don't draw this frame (the wading
    mesh, unclaimed shapes), under the world root and in the frustum, with passes from the water port. The water census map
    now serves drawing as well as the report. Untracked water is missing until the walk tracks it, seen only inside load
    holds.
89. **The main registration jobs and the Z-prepass groups' prepare do not run in native;** the accumulators stay empty and
    RenderWorld's batch calls still fire CS's stage points. The engine's list culls (BuildSceneLists, the accumulation
    jobs, FirstPersonCulling) still run: they call OnVisible for every entry DCLF's PrimaryCull rejects (switch, billboard
    and ordered nodes, particle systems) and step the fades of mixed and not-yet-admitted entries. Those need native
    producers before the culls stop (milestone 3), then S8.
90. **The sky root and water shapes are read on the render thread** for now, as the reflection faces already did; phase 6/7
    moves them into the snapshot. CS-driven stage points need Main::RenderWorld replaced in native (milestone 3).

## 2026-11-26: bug fixes (snap5-bugs; director accepted)

91. **Guest screenshots take native's route** (a deliberate guest behaviour change, to fix a fatal "ISCopy rt0->rt76 has no
    CS pass"): guest already runs the snapshot capture before DrawFrame, so PrintScreen, capture mode and Papyrus
    `TakeScreenshot` go to the Screenshot feature in both graph modes and are saved in its format. The save-screenshot
    handshake stays native only.
92. **The "second save dropped" was a test-setup problem:** a hostile guard opened the Health Low tutorial, which pauses the
    game, and `Save_Impl` refuses saves while paused. Configs with hostile NPCs start with `tgm` (the standing rule);
    `window-resize` and `screenshot-clean` still lack it.
93. **Resize fixes in the ImageSpace step groups:** step render areas follow the size the next revision brings
    (`EngineTargetShapes`), kept SAO/TAA/bloom textures follow it too, reused step builds re-import the engine's own
    targets, and the start steps follow the display size. Since engine targets live in shared placed memory, a stale
    render area corrupts neighbouring resources, so these are correctness fixes, not cosmetics.
94. **Open:** an intermittent GPU hang in `cs.dclf.main-opaque` (seen again in the merge validation, 3 of 10 resize runs so far) in the ~250 ms after a resize while DCLF still draws its
    old-size scene (2 of 9 resize runs); ORG records a stage's old build against new target versions until the new build
    arrives (recordings dropped, runs pass); FSR via Streamline destroys an image view still in use when it remakes its
    context on resize.

## 2026-11-26: phase 3 list culls stopped, milestone 3 (snap3-culls; director accepted)

95. **Every value game code read from the main list culls has a native producer:** `kAccumulated` and fades of DCLF-listed
    roots (including mixed, not-yet-admitted entries and actors) come from DCLF; animated roots DCLF does not list get
    `kAccumulated` from S7; the portal map comes from DCLF's RoomVisibility. Everything else the culls did is render-only
    and stops (billboards, switches, ordered nodes, decal nodes, multibound caches, particle appends, light receivers, the
    first-person cull).
96. **S8 fades** (`SideEffects::StepFade`) step once per game update, rendered or skipped, for S7's in-view roots that DCLF
    doesn't write: the leaf/tree LOD step, BSFadeNode::OnVisible's fade part (type 6 ported, otherwise the engine's own CPU
    fade function SE 98849 / AE 105486, called on the game thread as AE 19384 does; decision 8) and the +0x13C stamp.
    Parity: 8,746 roots, 0 differences. Fade roots that are neither DCLF-listed nor animated references are no longer
    stepped: no game code reads their fade.
97. **Native skips the main list culls inside Main::Draw** (BuildSceneLists' queue, Begin and Finish, FirstPersonCulling, the
    list-clear jobs; bytes checked, mismatch fatal), and `SelectCasters` queues no list accumulation jobs. The engine function
    after the full-frustum cull (it sets culling globals) still runs. Mixed entries are stood in like whole ones. The
    decision 53 test-mode crash is gone.
98. **Main::RenderWorld is next** (about 28 of the 63 engine render calls per native world frame): CS drives its stage
    points from its own frame. A native screen-blood pass must come first (the last `BSCullingProcess::Process` is the
    screen splatter's cull inside RenderWorld), or blood disappears.

## 2026-11-26: phase 6 lifetimes, part 1 (snap6-life; director accepted)

99. **The retire queue has two lanes** (`Snapshot/Retire.{h,cpp}`). The engine lane is drained on the main thread at the
    capture, because the engine runs an object's destructor on whichever thread drops the last reference and those
    destructors touch engine state unlocked; it never drains inside the render half. The render lane (UI streams, upload
    textures; D3D11 handles release on any thread) is drained by the render side at adoption. Every entry waits until the
    GPU finished the render frame it names (graph mode), CPU-only entries included, so part 2 uses the same rule. Adopting
    a new snapshot retires the one rendered last; a snapshot never adopted retires at once.
100. **Engine objects a snapshot names are held keys** (`Snapshot::EngineRef`): a reference that is never dereferenced;
     making, copying or releasing one off the main thread is fatal. Outside DCLF no published snapshot dereferences a bare
     engine pointer at record time; the cloud layer and Secunda keys now hold references so a reused address can't match a
     different object. The player's cell is keyed by form id.
101. **Snapshot holders are never destroyed,** so nothing releases engine objects while the process exits.
102. **No in-place engine write outside DCLF reaches snapshot data:** GFx texture writes are versioned by stream order and a
     re-initialised GFx texture keeps its old version by reference.
103. **Part 2 (DCLF) folds in as designed in `snap6-life.md`:** `EngineReleases` moves into the engine lane, held detaches
     are pushed at the capture, `EngineReadWindow` opens on the main thread after the publish and closes at the start of
     the next update (the render thread never takes a lease), reported writes create a new version by keeping the old
     object in the publications that name it.

## 2026-11-26: phase 3 screen blood (snap3-world; director accepted)

104. **Native screen blood from game data** (`BlockId::ScreenBlood`): the native capture runs RenderWorld's light flare setup
     (SE 100269 / AE 106979, pure arithmetic whose outputs only the blood shader reads; decision 8) on the game thread and
     copies its outputs and ScreenSplatter::Update's globals into the block. The render side builds the draws from
     ScreenSplatter's root and camera at Main::Draw's call of RenderScreenSplatter, into `cs.blood-splatter`; RenderWorld's
     splatter cull and submit do nothing in native. Parity: 4,488 draws matched; only the order inside one technique
     differs (ScreenSplatter's child order; both blends are order-independent). The native viewport is the display size.
     Native drew no screen blood before this (its flares were refused); guest still draws none (open).
105. **The blood geometry, its property and ScreenSplatter's camera are read on the render thread** for now, like decision
     90's sky root and water shapes.
106. **The resize hang (94) needs Light Limit Fix and is not FSR:** `window-resize-stress` (six resizes) reproduces it
     within two cycles with FSR or TAA, and passes twelve resizes with LLF off. Both observed hangs follow the revision that
     adopts LLF's resized grid buffers. Leading hypothesis: a light loop reads `lightGrid` through a stale bindless index in
     an un-rewritten latch. The loop count is not clamped, which would hide the cause.
107. **Main::RenderWorld's replacement needs each CS point inside it fed from CS's frame** (the call-order inventory is in
     `snap3-world.md`): most points read engine state RenderWorld's own calls set just before them (shadow state targets,
     the accumulator, SetCameraData's camera, global 415071).

## 2026-11-26: phase 3 RenderWorld driven by CS (snap3-rworld; director accepted)

108. **Guest screen blood takes native's route** (a deliberate guest change, like 91): guest's capture fills the ScreenBlood
     block and the draws are built from ScreenSplatter's root at the same anchor, so guest's draw order is unchanged; the
     latched blood family is deleted. In guest, RenderWorld still runs the flare setup, which yields the values the capture
     already took. The `screen-blood` config raises health at world-ready and requires the blood client on the graph.
109. **CS drives Main::RenderWorld in native** (refines 98): the engine's function is not entered; CS's driver
     (`RenderGraph/Engine/Native/RenderWorld.cpp`) repeats its steps in order. RenderSSAO, the snow pass or kMAIN copy and
     the composite go through `ImageSpace::RenderWorldImageSpace`, the composite flag from call order instead of global
     415071. Not run: the flare setup (in the block), the splatter cull and submit, the blur test. Still engine functions CS
     calls: StartAccumulating (with the pre-resolve's CS points), the post-resolve, the sun's update and submit query,
     ClearItemsList and the first-person end; the pre/post-resolve points are next, then those calls go. The TLS +0x768 tag
     RenderWorld sets is not written (no rendering effect found).
110. **The full-screen blur's camera test is kept by the native post chain** once per render from the snapshot's main
     camera, its gate captured in the ImageSpace block.

## 2026-11-26: the resize GPU hang (snap3-llfhang; director accepted)

111. **The resize hang (94, 106) was a light-grid layout mismatch, now fixed:** on the frame that adopts new LightGrid and
     LightIndexList versions, the readers (DCLF's colour pass and the others) took the grid size from LLF's feature data
     block, packed in EarlyPrepasses before LLF's Prepass refreshed `clusterSize`, so they indexed the new grid with the old
     size and read entries the cull never wrote. `LightLimitFix::GetCommonBufferData` now packs `AdoptedClusterGrid`: the
     cull and every reader use one grid from the frame's start (guest too; it differs from before only on a revision's
     first frame). The stale-bindless-index hypothesis was wrong. The light loop count stays unclamped.
112. **Open: native draw families latch the engine's live viewport** (`Family::LatchViewport`,
     `RendererShadowState::viewPort`). The engine takes a new window size at once while the frame still draws into the
     adopted targets; with TAA (render size equal to display size) growing the window overruns and loses the device in
     native Vulkan. This is engine-bound state read at record time: the viewport must come from the revision the fragment is
     recorded against (the snapshot's display/render size), not the engine. Blood splatter latches it too.

## 2026-11-26: phase 3 resolve points and the sun query (snap3-resolve; director accepted)

113. **Native's sun occlusion query is CS's:** the native RenderWorld driver draws the sun's occlusion pass into a CS-owned
     query slot (`OcclusionQueries::kSunSlot`) tagged with the snapshot id; a game-side capture (`Sky/SunQuery.cpp`) ports
     `Sun::UpdateOcclusionQueries`, takes the answer without waiting, writes the accumulator's query results and captures
     `BlockId::SunQuery` (answers arrive about 1.9 snapshots after the draw). The engine's query functions are not called in
     native. The sun query geometry is read on the render thread (decision 105's rule). The glare value is unverified: no
     test scene has the sun in view.
114. **The main accumulator's end calls (ClearItemsList, RenderFirstPersonView_End) are not made in native:** its lists were
     empty at both ends of the world render over 1,800+ renders.
115. **RenderWorld's points read CS's frame (`WorldPass`)** for targets, depth target and render modes (the engine's resolve
     code uses fixed values, so they are not captured per frame); sky batch, water mask and surface, sun draw and
     StartDeferred's forward targets are converted with 0 parity differences.
116. **Not done, and what it needs:** StartAccumulating and the post-resolve are still called. Replacing them needs CS
     producers for the perFrame/camera constants (SetCameraData, AE 0x140e45680), the render-state flush and the clears it
     triggers, the pre-resolve's LOD near-plane camera tweak, and the render state RenderPostProcess still reads.
     Remaining engine-state readers: Forward's frame params z (engine 1, FrameCamera 0), DCLF's `CaptureBindings`, viewport
     and depth range in `Frame::Read` (with decision 112), the water capture's engine globals, the ImageSpace copy step's
     rows. Copying the engine bodies would still call the same engine functions, so that was not done.

## 2026-11-26: native engine-bound state (snap3-engstate; director accepted)

117. **Native viewports come from CS's frame** (fixes 112): world draws use Renderer::UpdateViewPort's rule (AE 0x140e441c0)
     applied to the adopted revision's `EngineTargetShapes` at the render frame's ratios (`Family::FrameViewport`,
     `FrameCamera::ViewportRatios`): sky, sun, water, the shadow mask (and its texel scale), precipitation. DCLF's main
     shapes take their size from the revision their recordings name (`Revision::FragmentVersions` carries its variables).
     Menu scenes, decal tests and reflection faces keep the engine's viewport for now. `window-resize-stress-taa` passes.
118. **Forward's FrameParams.z is 1 in both modes:** the post-resolve writes the per-frame block with flag 4 before both
     blended draws; FrameCamera's 0 is right only for the deferred part.
119. **The LOD depth shift is a game-side block** (`BlockId::RenderWorld`, giving `WorldPass::DepthRange`): the water points
     and DCLF's main pass are inside the shifted window, the sky and sun outside; the Z-prepass keeps the main pass's range.
120. **Native DCLF's capture reads no bound constant buffer:** the eye, previous eye (the last drawn camera) and cull camera
     come from the frame camera, viewport size from the revision, depth range from WorldPass. Its bound views (CS features'
     own textures) and targets remain read. About 11 bound-state reads per native world frame removed.
121. **Carried over from 116:** CS still calls StartAccumulating and the post-resolve; the full call order of AE 106583 and
     106584 with each CS point and what it needs is in `snap3-engstate.md`. Also left: the water capture's engine globals
     and the ImageSpace copy step's rows.

## 2026-11-26: native RenderWorld without the resolve functions (snap3-accum; director accepted)

122. **Native's RenderWorld calls no resolve function** (refines 109, 116, 121): CS runs each point of StartAccumulating
     (AE 106583) and the post-resolve (AE 106584) in the engine's order. Their clears (the mask's stencil, the TAA mask,
     kMAIN_ONLY_ALPHA) and the kMAIN copy go through skeleton calls (`ClearDepth`, `ClearColour`, `CopyMain`).
     `LeaveResolveState` writes the shadow state they leave, for the code after RenderWorld. Every reader of what they
     set reads CS's frame. Engine render calls per native world frame: quick 56 -> 33, interior 38 -> 20, tour 39 -> 20.
123. **The depth shift window is CS state:** `WorldPass::ShiftWindow` and `CameraPlanes` compute the shifted planes from
     the RenderWorld block (which gains the scene graph's near and far and the shift scale). The engine camera is not
     modified. The far distance is one snapshot late at a cell change (accepted).
124. **Engine state CS still writes, until RenderPostProcess and the first-person view are CS's:** the end shadow state
     (`LeaveResolveState`), TAA's form, the render mode (411475) and the reflections' camera globals. The accumulator's
     camera is not nulled and the first-person node is not hidden.
125. **`RenderSnowSSS` stays an engine call** (improved snow only; no test scene reaches it). It is the only direct
     engine render call left in RenderWorld's scope.
126. **The water capture's globals and the snow copy come from CS:** planes, eye, render size and raw water from the
     frame; the snow copy is a native `ISCopySubRegionCS` step. `WaterDraw.cpp`'s `SurfaceVariant` reads `RawWater`
     (reading the engine global drew the river black once the post-resolve stopped).
127. **What remains in a native world frame** (detector, tour, about 20 calls): the frame skeleton (DrawFrame,
     RenderScene, RenderPlayerView and their CS-hooked children, phase 7 per decision 70); RenderDepth's two
     StartAccumulating calls and the first-person end; RenderShadowMasks; VolumetricLighting::Render; RenderWaterEffects
     (TESWaterSystem::Update, its reflections update and RenderDisplacement); RenderPostProcess; FaceGen::UpdatePending
     and JobListManager::FrameJobs. Menu scenes and the loading frames' cube faces are phase 5.

## 2026-11-26: face tint bakes and the frame skeleton inventory (snap3-facegen; director accepted)

128. **S18 fate, face tint bakes:** the capture drains the engine's facegen tint queue under the engine's lock
     (`src/Snapshot/FaceTint.cpp`, `BlockId::FaceTint`), turns each job into a bake (mask references, ISAlphaBlend rows,
     the head, a new texture created on the game thread) and frees the job as the engine does. The render side draws one
     bake per render frame through the face tint ImageSpace client and copies it into the bake's texture the frame after.
     A later capture hands the texture to the head and releases the old one. Bakes are keyed by serial, so repeated
     snapshots draw nothing new and dropped ones lose nothing. A head keeps its old texture 2-3 updates longer than in
     vanilla (accepted).
129. **Both FaceGen::UpdatePending calls are off in native** (DrawFrame and the loading loop, AE 39039), patched with a
     byte check that is fatal on mismatch. Guest is unchanged.
130. **JobListManager::FrameJobs is bookkeeping** (it reads the serving thread's flag to decide a job wait): the detector
     counts it as a helper. Native tour: 20.0 -> 18.6 engine render calls per world frame.
131. **The frame skeleton inventory for phase 7** (every call in DrawFrame, RenderScene and RenderPlayerView, its kind
     and its phase 7 fate) is in `snap3-facegen.md`. No dead calls removed yet (AE 107018 is empty, AE 76557 resets list
     processes that no longer run).
132. **Open: purple noise on the player's face in native**, present before snap3-facegen (the old engine path shows it
     too). Vanilla NPCs use pre-built tint textures and are not affected by the bake path.

## 2026-11-26: native RenderPostProcess and Volumetric Lighting (snap3-post; director accepted)

133. **Native's RenderPostProcess body never runs** (AE 107148): `NativePost::RunNativeBody` runs its points in the
     engine's order (VL render, read-back, VL apply and blur, the engine's lens flare gather, the composites, the
     end-of-frame effects) with no flush and no target, depth-of-field plane, TLS tag or camera port writes. It still
     writes the current camera (414580) for native readers. Guest keeps the engine body.
134. **Volumetric Lighting: the render's CPU work is ported, both paths kept.** Making the CS feature the only path
     would remove nothing (its ORG path uses the same engine CPU work). Game side: activity, descriptor, sun direction
     and diffuse, the no-temporal request (a count), the noise scroll (once per game update, CS's RNG). Render side:
     `GenerateRows` from the snapshot camera and the sun light's cascades and splits; CS keeps its own VL counter. Main
     Draw's VL block (+0x2F1..+0x33A, byte-checked) is jumped over in native. Observe: 0 differences over 600 renders
     except one frame where weather arrives one game update late (accepted, like 123).
135. **Decision 124 trimmed:** the render mode (411475) is no longer written (only the engine's RenderSSAO and effect
     setup read it); RenderWorld no longer writes the reflections' camera (RenderScene writes the same value and the
     game side reads it through the radial blur modifier, AE 107753); TAA's form is CS state (`WorldPass::TaaForm`).
     `LeaveResolveState` stays until the first-person view is CS's (the engine's RenderFirstPersonView still flushes).
136. **The RenderPostProcess detector row is removed** like RenderWorld's (CS drives it); the sites inside it count, all
     0 in native. Native interior 20 -> 18, post-effects 23 -> 21 engine render calls per world frame.
137. **Left:** the lens flare gather (AE 106988) is still an engine call; the current camera's native readers should
     read the frame camera; SE and the VL-off path are unrun.

## 2026-11-26: DCLF on the retire queue (snap6-dclf, phase 6 part 2; director accepted)

138. **DCLF's engine releases go through the retire queue's engine lane:** `EngineReleases::Push`, `PushOwner` and
     `PushRetired` adapt to `Retire::AfterThisFrame(Lane::Engine, ...)`. An entry waits for the newest render frame begun
     when it was pushed; a retired batch advances `releasedThrough` only when it is let go, so `Alive()` is exact. No DCLF
     engine reference is dropped at Present. LocalLightCull's retired filters and PrimaryCull's list graveyard use the
     lane too.
139. **The engine lane drains at the frame-job wait after Main::DrawFrame** (refines 99), not at the capture: the capture
     runs beside the update's frame jobs (cell animations, Havok), and a release there corrupted the cell animation list
     (an endless loop; cadence-drop-cells hung). `Retire::LetGo` always goes through the lane. This is also the likely
     cause of the one drop-cells hang seen after snap3-facegen (main thread in WaitForFrameJobs, 1 of 3 runs): face tint
     bakes hold engine references that retire through the lane.
140. **The engine-read window is game-side:** it opens after the capture and side effects in the DrawFrame hook and
     closes at the end of that call. Until phase 7 removes the render half, a hold bit (`kHeld`) keeps it held from each
     DrawFrame call and Present to the next BeginFrame, because the engine's render half still writes the scene graph
     (decals attach before Main::Draw; the next update's jobs start after Present). Main-thread leases are counted (0 in
     every run).
141. **Held detaches are pushed at the capture in both modes** (a detach and its re-attach happen within one call).
     `FrameSnapshot::scene` stays empty: DCLF's retirement chain holds references per publication; pairing the
     publication with the snapshot is phase 7. No new mechanism for in-place writes or frame copies (SWF pointer swaps
     are versioned by the material owner blocks; the constant mirror is copied into frame rows). Per-load VRAM growth
     unchanged (branch 259 MiB over two loads, base 292).

## 2026-11-26: native RenderDepth and RenderShadowMasks (snap3-depth; director accepted)

142. **Native's RenderDepth calls no engine function:** `RenderGraph/Engine/Native/RenderDepth.cpp`, called from DCLF's
     innermost `Main_RenderDepth` thunk (Terrain Blending's still wraps it), clears the main depth, runs DCLF's Z-prepass
     (`DrawcallLimitFix::DepthPassDrawn`, shared with guest), the first-person draw point, the copy into
     kPOST_ZPREPASS_COPY, and writes the state the engine leaves (`LeaveDepthState`). The depth accumulator offered 0
     passes in observe runs.
143. **First person in native is DCLF's alone:** RenderDepth's first-person block (cull, camera and node shift, submit)
     is not made; AE 107123 in RenderPlayerView already does the camera setup and node shift, and the block undid and
     redid it with the same values.
144. **Native's shadow masks are CS's:** a call-site thunk on RenderPlayerView +0x39C runs `ShadowMask::DrawWorldMasks`
     (skeleton clear, the snapshot's frustum, the frame camera's posAdjust, constant starting modes) and CS's port of
     ReturnShadowmaps, then writes the state the engine leaves and the current camera (414580). The focus targets'
     stencil marks are not drawn (their only reader, the mask shader's focus branch, is never taken). Terrain Blending's
     SetCameraData before the depth pass is skipped in native.
145. **A refused native world batch is cleared and logged once, not fatal** (once per load, the first world frame, for a
     light whose map has no target yet). Engine render calls per world frame: interior 20 -> 16, shadows-blend 24 -> 18.5,
     quick 33 -> 30.7. Left: the masks' settings globals and shadow lights are read at record time; the state CS writes
     for engine code after these functions goes once that code is CS's.

## 2026-11-26: menu scenes, first round (snap5-menus; director accepted, unfinished)

146. **Menu scene draws are computed by CS from the scene graph** (`Features/MenuViews/MenuScenesNative.cpp`): Lighting
     and static Effect descriptors, every written constant, textures and samplers match the engine's (observe, 0
     differences apart from the items below), built on DCLF's ports plus Light Limit Fix's light rows and Linear
     Lighting's upload hooks. `UI3DSceneManager::Render`'s detour calls `Scenes::Native::Render` for an owned scheme;
     none is owned yet (the stats menu's blended draw order does not match the engine's accumulator sort).
147. **A menu scene's update half stays on the game side as engine CPU calls** (light chain, light add, root and light
     update; decision 8). The texture-streaming mark around the engine's draw (AE 106451/106452) is not made.
148. **Menu scene frame values are the scene's own:** previous-frame rows (PreviousWorld, c16, c41) come from the scene's
     own last render (the engine used whatever camera was set last); depth range 0..1 (the engine's 0.999998 is stale
     from the world frame); write mode 1 into kMAIN and 12 into the back buffer. FrameBuffer c20-c23 differ by 1 ulp
     (CS inverts the projection in double).
149. **Director's rule for switched menu schemes:** a scheme is owned only when CS computes every draw in it; a draw CS
     cannot compute inside an owned scheme is fatal, not skipped (the agent's skip-and-log is only for observe builds).
150. **Detector role `MenuScene`:** the Lockpicking, MistMenu and Stats call sites count as render until CS owns the
     scheme, then as producer. MistMenu's UI3DSceneManager::Render is about 19 of the ~22 render calls per main-menu
     frame. Observe scaffolding stays behind the diagnostic `CS_MENU_SCENES_OBSERVE` for the next lead. Found on the
     way: DCLF's world material port does not apply Linear Lighting's specular colour conversion
     (`LightingMaterialUpload`, PS 25).

## 2026-11-26: native water effects on the game thread (snap3-water; director accepted)

151. **Native skips Main::RenderWaterEffects** (Main::Draw +0x16F, byte-checked; `Native/WaterEffects.cpp`). The capture
     runs its game half on world snapshots: the S20 early-out resets, `TESWaterSystem::Update` on the game thread
     (decision 8) and the S16 flow-map ring drain. Inside the update, the reflection face render goes to a CS recorder
     (its clear-colour calls NOPed), RenderDisplacement and the displacement effect are CS ports, and the water
     visibility cull (AE 32189) is a frustum test against the snapshot camera (it can mark more water visible, never
     less). The reflection update and displacement are detector producers (`Role::WaterProducer`, decision 64's rule).
152. **Recorded water work goes through a queue keyed by snapshot id**, drawn once at DCLF's frame start (decision 62's
     pattern): faces through `ReflectionFaces::DrawRecorded`, displacement steps and flow copies through
     `ImageSpace::StageNativeWater`; the displacement restart's clear is a native ClearSimulation step. With dropped
     snapshots only the newest update's displacement steps are drawn. A face DCLF cannot draw natively is not drawn (the
     engine's draws of it were dropped by native before too).
153. **SetupSunAndSky runs in the capture** before the main camera is taken; DCLF's hook at Main::Draw +0xD3 returns the
     cached result.
154. **Native precipitation occlusion maps make no engine call:** `DrawcallLimitFix::RenderMask` computes the occlusion
     projection; the engine's SetupMask cull, shadow-state save/restore and dynamic-resolution lock write are not made in
     native (Skylighting and Upscaling too). Engine render calls per world frame: tour 20.0 -> 16.9, river 21.9 -> 17.3,
     quick 33.1 -> 25.4 (before the depth and post merges).
155. **Left for phase 7:** `WaterPort::CaptureFrame` reads engine globals (including what the water update writes) at
     record time; the water normal textures (AE 32170) need versioning; `Precipitation::UpdateProjection` and
     Skylighting's live reads are record-time. SetupSunAndSky and RenderPrecipitation are CS points behind skeleton
     calls (decision 70). The water update now runs in the capture beside the frame jobs: snap3-jobhang checks its
     ordering with the drop-cells frame-job hang.

## 2026-11-26: the player's face noise (snap3-facenoise; director accepted)

156. **Closes 132:** the noise was DCLF drawing the head's first, empty tint texture (made by PrepareHeadPartForShaders
     with `CreateRenderTexture`, no contents). DCLF captures the face material's views when the head attaches; the bake
     then swapped a new texture into the head and released the old one (as UpdatePending does), which DCLF never sees.
     The native device does not zero a texture created without contents, so it showed leftover memory; DXVK zeroes it,
     so guest draws a zero tint instead (flat dark face).
157. **A native bake copies into the head's existing texture** (no new texture, no swap, no release); a head texture that
     does not match kPLAYER_FACEGEN_TINT is fatal. The render side zeroes a new bake's head texture first, so the frames
     before the copy draw zeros.
158. **Director's decisions on the open items:** (a) the native device zeroes every texture created without initial
     contents, like DXVK and D3D11 drivers, so no texture read before its first write shows leftover memory; (b) guest's
     player face is fixed too: DCLF refreshes a material's captured views when the engine swaps a texture under it (the
     face tint swap first). Both go to the next lead.
159. **Open regression:** since the snap5-menus or snap3-water merge, native's race menu (menu over world,
     `facegen.toml`) draws the world only in the top-left 1280x720 of a 1920x1080 frame, and darker. Not caught because
     the water validation did not run the facegen config; menu-over-world configs belong in every validation set from
     now on. snap3-quarter bisects and fixes it.

## 2026-11-26: Linear Lighting colours in DCLF rows (snap3-llspec; director accepted)

160. **Linear Lighting's upload hooks are ported where DCLF makes each value:** LL rewrote colours from sRGB to its
     working space in the engine's Setup functions' Unmap calls, which DCLF's rows never pass through (guest and native
     alike). A new LL material contributor (`Integrations/LinearLighting/`) writes Lighting PS 25 SpecularColor for
     non-PBR specular passes; the effect port converts BaseColor (record and per-frame patch, via
     `MaterialFrameVariable::workingColour`) except GrayscaleToColor and Blood; the shading and geometry samples convert
     EmitColor (with `emitColorMult`), Effect PropertyColor and DirLightColor (dropping the HDR multiplier for a Sky Sync
     sun, as LL does). Capture parity with LL on: 158,300 -> 3,900 mismatched draws of about 202,000; what is left is
     older (MTLandLODBlend flags, LOD rounding). Guest's picture changes to match the engine path.
161. **Left:** lit Effect light colours (PS 8-11) are unchecked (parity compares Lighting draws only); a runtime LL
     toggle reaches already-sampled shading rows only when their property changes; the capture parity diagnostic races
     the scene work thread in `Coordinator::FindObject`; MenuScenesNative's own conversions are now duplicates
     (snap5-menus2 removes them).

## 2026-11-26: the drop-cells hang (snap3-jobhang; director accepted)

162. **The drop-cells hang was cs-test's coc running inside a frame job, not a job-list fault:** the runner queued its
     console command through SKSE's task interface, which runs in frame job list 5 ("Main render end",
     Job_Post_process); `CenterOnCell_Impl` loads the cell right there, and `SetLoadingScreenActive` only switches the
     serving thread to the loading screen when it is idle, so a slow load showed no loading screen and tripped the 15 s
     watchdog. Face tint was not the cause (an earlier hang predates it); slow loads came from a busy machine.
163. **cs-test runs its commands' game work at MainTick, after the frame-job wait** (`Testing::InstallTaskTick`, test runs
     only; a failed install ends the run): console, cast, mouse, camera, draw, place, morph, move and turn. Non-test runs
     are unchanged. Six consecutive drop-cells passes.
164. **Open: CS's executor workers spin while idle:** in a slow-phase dump all 14 spun in tbb's `micro_queue::pop`
     (about 27 s of CPU each, mostly SwitchToThread). Idle workers must block, not spin; snap3-spin fixes it.

## 2026-11-26: texture zero-init and the guest face (snap3-texinit; director accepted)

165. **Closes 158a:** the native device zeroes every texture made without contents inside its existing creation
     submission (no extra submit or wait): depth and stencil clear, RTV clear, UAV clear, otherwise copies from one 4 MB
     zero upload buffer in row bands; the texture returns to its resting state in the same submission. `ZeroWrapped` and
     FaceTint's render-side head clear are gone. Startup cost: 256 textures (1.6 GB, nearly all clears) in about 0.4 s
     (Dev-Fast); GPU time not measured. Staging textures are still not zeroed.
166. **Closes 158b:** in guest, DCLF hooks FaceGen::UpdatePending (beside Upscaling's hook), reads the queued heads under
     the queue's lock, and afterwards a main-thread task recaptures the player's materials that name one of those heads
     (`NoteTexturesReplaced` -> `NoteWritten`). Found by walking the player's 3D (a tint job names only its head
     texture). NPC tint jobs (NPCs without FaceGen files) are not refreshed yet. Native installs no hook.

## 2026-11-26: the stats menu scene is native (snap5-menus2; director accepted)

167. **The stats menu (scheme 6) is CS's:** the blended list modelled as the engine builds it (hint-1 passes in cull order,
     the GetRenderPasses hint rule, a stable far-to-near merge sort by bound centre along camera column 0 less the radius
     with `sortByClosestPoint`, `BSOrderedNode` groups interleaved by depth); billboards turned by the engine's CPU
     rotation (SE 69681 / AE 71061) on the game side, as 147; a scene light reaches a geometry by bound distance (the
     engine's geometry light lists are empty for menu geometry); permutation words as Linear Lighting's hooks set them
     (RenderToUI, BaseTextureIsWorking, SourceAlphaBlend). Its site reads producer; shots match the engine-drawn native.
168. **Every menu scene takes the non-TAA branch:** CS's UI::Render hook turns the engine's TAA off, so no TAA mask fill or
     TAA UI form is needed.
169. **A menu draw whose program is still building is left out of that render** and logged once (as the engine draw
     families already do; the manifest prebuilds it next session). Any other draw CS cannot compute in an owned scheme is
     fatal (149). Left: MistMenu (smoke particles, shadow-light rule, loading model shadow maps and mask, menu HDR chain
     with RenderEndOfFrameEffects), inventory (membrane glows, skinned Effects, particles), lockpick, book (Scaleform
     page render target), journal, local map. Main menu frames: 20 engine render calls, unchanged.

## 2026-11-26: native's code after RenderWorld (snap3-fpv; director accepted)

170. **Native's post-RenderWorld path calls no engine function apart from the skeleton:** `Native::RenderFirstPersonView`
     runs only DCLF's first-person colour point (`FirstPerson::ColourDrawPoint`, shared with guest); the first-person
     accumulator render, its blended groups, depth-range and target writes, the first-person sun glare and the second
     sun query are not made; Main::ClearMasks is skipped. AE 107123/107124 (first-person root shift and back) stay: DCLF
     reads the camera 107123 builds (phase 7 skeleton).
171. **The leave-state writes are deleted** (`LeaveResolveState`, `LeaveDepthState`, `LeaveMaskState`; RenderWorld's
     414748/9 writes): an observe run showed no engine reader left in native.
172. **The current camera (414580) is written only on the game side**, as the world camera in
     `SideEffects::AfterRenderHalf`, for the DOF modifier instances (AE 0x141482cd0); native readers use the frame camera
     (reflection faces keep the face camera).
173. **The lens flare gather (AE 106988) is CS's port** (`ImageSpace/LensFlares.inl`): game inputs in the ImageSpace block,
     the caster selection fills a CS-owned queue whose references retire through the queue; history counts render frames.
     The visibility pass writes the engine's rotating output channel (5..8) in native; guest still writes channel 5 (open,
     fixing it changes guest output).
174. **Closes 145:** RenderShadowMaps records which lights it drew; the masks leave out a kept light it did not draw (no
     shadow that frame), and a refused world mask batch is fatal again. Engine render calls per world frame: post-effects
     21 -> 13.9, lens 24 -> 14.9, shadows-blend 18.5 -> 14.5, upscalers 24 -> 14.0, quick 30.7 -> 27.2. Left:
     Main::RenderSaoCs with bSAO_CS_Enable; `gensites2.py` still lists the removed detector rows.

## 2026-11-26: the quarter-frame regression (snap3-quarter; director accepted)

175. **Fixes a regression from 154:** native unlocks dynamic resolution after RenderPrecipitation again (the
     `Main_RenderPrecipitation` call-site hook's native branch skipped both lock writes; with FaceTint's UpdatePending
     calls off it was the only unlock). With the lock left at 1, native readers of the engine's lock (ImageSpace
     `ReadEndOfFrame`, `BlurRatios`, vertex rows, `Util::ConvertToDynamic`, LegacyGraphicsCompatibility) treated
     the render-size world as full size: every native world frame drew in the 1280x720 corner, darker (eye adaptation
     averaged the empty rest). It was in origin/org_integration 830623810. In phase 7 these readers take full
     resolution from FrameCamera instead of the engine's lock.
176. **cs-test passes runs whose shots are wrong:** nothing compares shots against a reference. A kept reference with a
     tolerance in the basic configs is needed (next lead).
177. **Open regression: native world frames are about a quarter as bright since 3f5b50150** (mean luma of quick's
     start shot about 64 before and in each of snap3/post, snap6/dclf and snap3/depth alone; 17.5 after the three
     merges, still about 18 at 245ec15e5 with the quarter fix). An interaction between merges, likely exposure or
     tonemap; it is in origin/org_integration. snap3-dark bisects the three merges and fixes it; native shot references
     wait for the fix.

## 2026-11-26: executor queues block instead of spinning (snap3-spin; director accepted)

178. **CS's executor and pump queues use OMS's `InboxQueue`, not `tbb::concurrent_queue`:** a tbb pop claims a ticket and
     then spins (pause, then unbounded SwitchToThread) on the earlier popper and its pusher; with workers at mixed
     priorities (below normal, raised by WaitBoost, frame-critical jobs and ParallelFor helpers), raised workers spin on a
     preempted lower-priority holder that SwitchToThread will not run. `InboxQueue` (OMS
     `Async/InboxQueue.h`): lock-free CAS push that never waits, mutex pop that blocks. Used by the executor's ring and
     frame-critical queues, DCLF's BuildPump, the batch StoragePool free list and the ComputeSubmitter batches. Benchmark
     under contention: 331 jobs / 17.3 s kernel (tbb) vs 91,000 jobs / 0.30 s. Frame times unchanged (Dev-Fast).
     OMS's StateGraph tbb queues (single serialized pump each) stay.
179. **Slow lab phases are mostly machine contention** (builds starving the driver and startup: Streamline init, device
     creation, pipeline cache load); the spin made them worse. Worker priorities are left as they are (a frame-time
     trade-off).

## 2026-11-26: reference shot checks in cs-test (snap3-shotref; director accepted, merge after snap3-dark)

180. **cs-test checks shots against kept references:** configs opt in with `expect.shot_refs` (quick, facegen,
     native-interior, menu-scenes except the random loading model); `tools/cs-test/refs.json` holds hashes and sources,
     PNGs live in `C:\Modding\CS-AB\refs`; `ref accept <run>` by hand only (passed runs), `ref check`, `ref list`. A
     coarse tile check on square-root luma (16x9 tiles of a 96x54 grid) fails a run when more than 23% of tiles move by
     more than 56 or turn flat or busy, the mean moves by more than 20, or the frame is black or white; calibrated on
     741 good and 525 quarter-frame pairs from 2026-11-26 (no bad pair passes). A missing reference only logs. Native
     references are taken from before the 3f5b50150 darkness; the branch merges after snap3-dark's fix, then native
     references are re-accepted from a fixed run. Open: a native-vs-guest cross-check would have caught the darkness;
     `quick` has no `tgm` (a death puts a loading screen in the shot).

## 2026-11-26: the darkness regression (snap3-dark; director accepted)

181. **Closes 177:** native's shared constants take the permutation block from CS's state (`permutationData`) at each
     upload. They used `permutationDataPrevious`, the block `permutationCB` last uploaded, which only `State::Draw`
     refreshes from the engine's state flush hook; `State::Reset` sets it to all ones each frame. Each of snap3-post
     and snap3-depth alone left one engine flush before the world (VL's block, or RenderDepth and DCLF's depth-capture
     `ApplyPendingState`); merged, none was left, so world segments uploaded all ones (every extra-descriptor bit,
     RenderToUI set) and native frames came out a quarter as bright. Guest keeps the uploaded block. Open: any other CS
     state that only `State::Draw` refreshes is stale the same way in native world frames.
182. **Open: cadence-drop-cells' riverwood shot is black since snap3-jobhang:** steps count frames from the coc and
     loading-screen frames now count, so the shot lands in the load. snap3-cocwait makes steps after a load wait for
     the next world-ready.

## 2026-11-26: the main menu scene is native (snap5-menus3; director accepted)

183. **The main menu (MistMenu, scheme 4) is CS's** when the render has no depth pass and the frame is not a loading
     frame (`Owns(scheme, keepDepth)`): smoke through DCLF's particle expansion (shared slot fill; Effect.hlsl's ORG
     build reads the quads from the draw's latch), the menu HDR chain staged by `ImageSpace::MenuSceneHdr` from the
     menu's ImageSpace override (no engine call; framebuffer range 1), menu lights on `shadowSceneNode[1]`, fog from the
     menu scene node, the sun colour from the override's HDR value, kMAIN cleared. Shadow lights add their bits only when
     the render has shadow maps; owned renders draw none, only the dimmers (a port of the shadow-light camera update).
     The update half sets the menu culler's camera and frustum (engine CPU call, as 147). Main-menu frames: 20 -> 1
     engine render call (Main::DrawFrame).
184. **Menu geometry with no renderer buffers yet is skipped in that render** (it streams in), like 169's building
     programs. The loading model and loading frames stay the engine's: CS drawing a loading screen's early renders
     crashed DCLF's shadow-mask path once (vertex shader dirty with none bound), cause not found. Left: loading model
     (depth pass, shadow maps and mask), inventory (membrane pass, skinned Effects; `menu-items` config shows Chillrend
     and Savior's Hide), lockpick, journal, book, local map; the light fade-distance swap is not ported.

## 2026-11-26: cs-test steps wait for each load (snap3-cocwait; director accepted)

185. **Closes 182:** cs-test records every load again, including a coc with no loading menu, seen as a player jump of
     more than 1024 units between two Presents (the snapshot's teleport distance; logged as `[TEST] teleport at frame
     ...`); `held-frame` counts as a load milestone. Steps after a coc or `load` anchor at `world-ready` (39 configs;
     shots 400 rendered frames after it: 120 fell inside the post-load fade at full frame rate), and a step whose milestone never comes in a ready, non-loading world fails after
     `wait_s` / `step_wait` with a clear reason. `quick` starts with `tgm`. Limits: a coc to where the player stands is
     not seen (the step fails after 60 s). An exact signal (counting `SetLoadingScreenActive(false)` in
     Snapshot/Publication) was not added: the lead's edit outside its boundary was refused by the permission system;
     left for the user to decide.
186. **Refines 185 (user):** cs-test sees every load by counting ServingThread::SetLoadingScreenActive calls
     (`LoadingScreen::Calls()`, both halves; a coc nearby makes only the false call). The 1024-unit jump test is gone, so
     a `player.setpos` is no longer a load and a coc to the player's own spot is seen.

## 2026-11-26: the inventory scenes are native (snap5-menus4; director accepted)

187. **The inventory (schemes 1 and 2) is CS's:** enchantment glows (the membrane pass of a Lighting property with effect
     data) are computed by CS (descriptor from property flags and effect data, technique rows, textures, geometry rows
     with Linear Lighting's PropertyColor conversion, the exe's alpha-test constants, the membrane group's states: blend
     from the effect data, depth test without write, depth bias 10 through `Family::DepthBiasViewportOffset`, write mode
     11), drawn after the batches and before the blended list (FinishAccumulating's order); every menu glow is hint 3 (all
     76 observed). Skinned Effect draws are allowed. A draw whose texture is still streaming in is left out of that render
     (as 169 and 184). World frames with the inventory open: 20.6 -> 9.0 engine render calls.
188. **MistMenu renders without a depth pass are CS's in loading frames too.** The menus3 crash was the menu mask hook
     applying the engine's pending state with "vertex layout changed" set and no vertex shader ever bound (the first
     engine-drawn menu render after only CS renders); the hook clears the flag when no vertex shader is bound. Left: the
     loading model (scheme 4 with a depth pass: needs a menu-geometry shadow depth path, then masks and shadow bits;
     about 20 engine calls per loading-model render), lockpick (no cs-test step opens it), book (Scaleform page target
     import), journal (no 3D scene seen), local map.
