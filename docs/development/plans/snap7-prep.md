# snap7-prep: phase 7 preparation (2026-11-26)

Lead: `prep`, branch `snap7/prep` (from org_integration 62bbcb4e9), stage `s7-prep`. This does what phase 7 can do now, in
today's single-threaded frame, without creating the render thread. The phase 7 plan is at the end.

## 1. ImGui input through a queue

Most of the queue was already there: `Menu::ProcessInputEvents` (the input poll) and the window proc add events. The
ImGui frame's start takes them (`Menu::ProcessInputEventQueue`, from `OverlayRenderer::RenderOverlay`) under
`_inputEventMutex`. The DirectInput wheel delta is an atomic accumulator. What still crossed between the two sides:

- **Game-side reads of UI state.** The input poll called `ShouldSwallowInput()` and `IsPreviewFlying()` live. Those read
  the menu's, the CS Editor's and the Effects 11 editor's open state, which the ImGui frame writes. The ImGui frame now
  publishes them when it ends (`Menu::PublishInputState`, also on frames that skip drawing). The poll reads the published
  values (`SwallowsGameInput`, `PreviewFliesPublished`). In today's frame the poll comes after the previous Present's
  ImGui frame, so it sees the same values. One exception: a test step's `Menu::Open` lands after the overlay, so one poll
  later.
- **Game-state writes from the ImGui frame:**
  - `SyncVanityCamera` writes the INI value `fAutoVanityModeDelay:Camera`.
  - `OnFocusChanged` calls the game keyboard's `ClearInputState`.

  Both now run in `Menu::UpdateGameSide`, called by the input poll on the game thread. The ImGui half of the focus reset
  (`ClearInputKeys`) reaches the next settings frame as an event (`imguiKeysReset`). As before, a focus change waits until
  the settings window is drawn (`publishedSettingsDrawn` replaces "DrawSettings ran").
- **Polled OS state left as it is:** `Util::UpdateImGuiInput` (GetCursorPos, GetClientRect) and `ImGui_ImplWin32_NewFrame`
  (GetForegroundWindow, XInput, QueryPerformanceCounter). These are thread-safe Win32 reads, not window messages. One
  exception is the Win32 backend's `SetCursor` on a cursor-shape change: it acts only on the calling thread's input
  queue. Phase 7 either sets `ImGuiConfigFlags_NoMouseCursorChange` (the menu draws its own cursor,
  `io.MouseDrawCursor`) or posts the cursor change to the window thread. Decided by the agent, to confirm: left for
  phase 7, since it is a no-op difference today.

The `ProcessInputEventQueue` consumer also runs the hotkeys: menu open/close, shader toggles, screenshot request,
Effects 11 toggle, CS Editor toggle. They change CS's render-side state and the UI's own state, so they stay in the ImGui
frame on the render thread.

## 2. Settings through the revision lane

ImGui runs at the end of the render frame (at Present), so setting writes reach render-side readers between frames on
the same thread, in phase 7 too. The cross-thread readers are game-side code that reads settings the menu writes. Audit
(every registered capture, SideEffects, the core capture):

- 12 captures read no settings: CloudShadows, DynamicCubemaps, ExponentialHeightFog, InteriorSun, PhysicalSky,
  ProceduralAurora, ProceduralSun, ScreenSpaceShadows, Skylighting, TerrainShadows, TreeWind, VolumetricLighting.
  WetnessEffects, NativePost (`foreign` is fixed at install) and SideEffects read none either.
- **Fixed:** GrassCollision's capture read the bound `EnableGrassCollision` live. It now reads `.Adopted()`, the value of
  the last adopted revision.
- **Left for phase 7 (plain struct fields read on the game thread):**
  - CoreFrame and Effects11 read SkySync's moon intensities (`NewMoonIntensity`, `CrescentMoonIntensity`,
    `FullMoonIntensity`) and `MoonLightSource`.
  - SkySync's `Sky::Update` hook (game logic, decision 24) reads about ten SkySync fields.
  - PostProcessing's capture reads the Cinematic Camera's settings (Enabled, Filmback, Lens, Focus, Exposure), DoF's
    `consoleSelection` and `targetFocus`, and `bypass`. The `pipeline[idx]` pointer is read unsynchronised.

  Each needs `CS::Setting` binding plus `.Adopted()` reads in its game-side code. That is per-feature work in files no
  lead owns. Decided by the agent, to confirm: listed rather than converted here; the conversion is mechanical
  (`Bind(Declare(...))`, as GrassCollision does).
- **Engine and game writes from ImGui outside Menu's own two (above):**
  - `Menu/AdvancedSettingsRenderer.cpp:673-679` "Run test conditions" runs console scripts and `EditorWindow::PauseTime`.
  - `DrawWeatherDetailsWindow` forces weather through the CS Editor.
  - The CS Editor has about 60 such sites: the game hour, time scale, ForceWeather, console `tfc`, the light editor's
    scripts.

  In phase 7 they need a game-thread command queue (the SKSE task interface already runs closures on the main thread),
  fed from the ImGui frame. Not done here: the CS Editor is outside this lead's files.

## 3. Swapchain resize as a published event

The engine already treats a resize as an event: its WindowProc (SE 35635, AE 36649) calls
`Renderer::RequestWindowResize` (SE 75450, AE 77235), which sets a flag and a size in the Renderer. The next
`Renderer::Begin` (SE 75460, AE 77245) sees the flag and calls `WindowSizeChanged`, which releases the views, calls
ResizeBuffers and makes the targets again (CS's target set in graph modes).

Native now routes this through the snapshot:

- A call-site hook (`WindowProc_RequestWindowResize`, Hooks.cpp; SE +0xA1, AE +0xA5, native only) stores the request
  (`Snapshot::NoteWindowResize`: size plus serial) instead of setting the engine flag.
- Every snapshot, game update or frame-only, carries the newest request as `Events::windowSize`, with the
  `WindowResized` flag when the serial moved. It is a state, not a delta, so a dropped snapshot loses nothing; MergeOlder
  keeps the newer serial.
- At the render frame's start (the Begin detour, after Adopt), `Snapshot::WindowResizeToApply` returns a new serial once,
  and the render side calls the engine's own `RequestWindowResize`. The engine's Begin body then resizes as before.

A request that arrives between the capture and Begin now waits one game update. Guest and plain D3D11 are unchanged (the
hook is not installed). In phase 7 the render thread takes the event at its frame start and drives CS's own resize: the
native device's `Resize` and `QueueSwapchainResize`, and the target set. The engine's Begin no longer runs there.

## 4. The update-rate cap

- The setting is `State::updateRateCap`, saved as Advanced "Update Rate Cap". Default 60 updates per second, 0 for
  uncapped, at most 1000. Atomic: the UI writes it, the game thread reads it. There is no UI yet: today it would act only
  under a test switch.
- The wait is `Snapshot::PaceGameUpdate()`, on the game thread right after the update's render half (`MainUpdate_DrawFrame`),
  where phase 7's "publish, then wait for the next slot" goes. It uses a high-resolution waitable timer. Slots do not
  catch up in bursts: an update a whole slot late starts the slots again.
- It acts only with `CS_UPDATE_RATE_CAP=1` (a Test switch). With the engine's Present still blocking, the wait works as a
  game-frame limiter there. That is enough to test the pacing code, and default pacing does not change. Phase 7 removes
  the switch and turns the wait on whenever the render thread runs. Decided by the agent, to confirm: a test switch,
  since no default-on use is possible before the render side is decoupled.

## 5. PCL markers from snapshot and render identity

- The render frame's Streamline frame id (`renderFrameId`, set in `Streamline::BeginRenderFrame` at UpdateJitter) is now
  `Snapshot::Render().frame`, the render identity. It stays strictly increasing: a second call in one frame takes the
  next id. Before, it was Streamline's own counter plus the `simMarkerFrameId` latch, which is gone.
- The simulation id is the render frame that first renders the snapshot this update publishes: `LastRenderFrame() + 1`.
  Until the thread split, that is the next Begin's frame, so in a normal cadence both sides agree exactly as before.
- SimulationStart is emitted once per game update (`Snapshot::LastGameId() + 1`, the snapshot the update publishes)
  rather than once per predicted render frame. SimulationEnd is emitted once per adopted snapshot id, so a repeated render
  has none. RenderSubmitStart is emitted once per render frame (`Render().frame`; before, `frameCount`).
- Present, submit and TriggerFlash markers keep `RenderFrameToken()`. Loading-screen frames, which have no UpdateJitter,
  keep the last world id, as before.

Phase 7 open item: Reflex pairs markers by frame id. Once one snapshot renders several times, or several snapshots go by
per render, the simulation id must be the render frame that adopts the snapshot first. The render thread only knows that
at adoption. Either the sim markers are re-emitted with that id at adoption (SimulationStart's timestamp is lost), or
both sides key by snapshot id and DLSS-G's constants are keyed per render frame. Needs a frame-generation run on a
decoupled build to choose.

## Runs

(filled below)

## Phase 7 plan: what remains for the thread split

After phases 3, 5 and 6, and this prep, the split is:

1. **The render thread.** CS creates it at the first native Begin, after the device exists. It runs CS's own frame loop:
   adopt, then OnBegin (the frame clock, revision adoption, graph installs), then record the recipe's stages, submit and
   present. No engine call.
   - It takes ownership with `FrameAnchors::ClaimRenderOwnership()` (drain, then `RenderGraphRuntime::MoveOwnerThread()`)
     once, as the loading screen's serving thread does today. Native never hands ownership back.
   - The loading-screen thread is the precedent: it already renders with no game renderer call and claims ownership at
     its Begin (decision 73's producer rules).
2. **The Present hook becomes publish-and-wait.** On the game thread, `IDXGISwapChain_Present` and `MainUpdate_DrawFrame`
   in native do only this:
   - `Snapshot::Capture`, then SideEffects S1 and S4 right after the capture (decision 34 moves them).
   - `MenuManager::DrawInterface` as the UI producer (decision 16).
   - `Testing::OnGameUpdate`, then `Snapshot::PaceGameUpdate()` with the switch removed.

   The engine's render half (Main::DrawFrame's draw part, Renderer::Begin/End) is not called. The whole Present hook body
   (frame end, DCLF BeforePresent, seal, pacing, `State::Reset`, metrics, `Testing::OnPresent` for shots) moves to the
   render thread's frame end.
3. **Revision adoption at the render thread's frame start.**
   - `Revision.cpp:1629` takes fragments of `mainThreadOnly` sources only on the main thread. Those sources become
     render-thread adopters, or are taken by the render owner.
   - `FrameAnchors::IsMainThread()` checks in the frame driver (FreezeTestAtBegin, DecideKind) go: the kind comes from
     the snapshot (decision 71).
4. **ImGui on the render thread.**
   - `Menu::DrawOverlay` moves with the frame end. Input already arrives through the queue, and the game side reads only
     the published UI state.
   - Set `NoMouseCursorChange` (or post cursor changes to the window thread).
   - The `Menu::Open` test command goes through the queue (a queued "open" event) instead of writing `open`.
5. **Settings.** Convert the plain fields that game-side code reads (section 2) to bound `CS::Setting` with `.Adopted()`
   reads. Add a game-thread command queue (SKSE tasks) for the ImGui-driven engine writes: CS Editor weather and time, test
   conditions, console scripts.
6. **Resize.** The render thread acts on `Events::windowSize` itself: the native device's Resize and swap-chain queue, and
   the target set's new versions on the revision lane. It no longer goes through the engine's RequestWindowResize and
   Begin. `Upscaling::LatchWindowState` (focus, size-move) is already latched per frame from window-proc atomics.
7. **PCL.** Choose the simulation id rule (section 5's open item), with `fg` and `fg-dlss` runs on the split build.
8. **Test harness.** Shots and `expect.live_frames` count render frames on the render thread and name the snapshot
   (decision 51). Add configs that run the game slower and faster than the render thread (`update_rate_cap` 30 and 240
   with `CS_UPDATE_RATE_CAP`).

## Proposed decision-log entries

- **ImGui input through the queue; UI state published for the game:** the input poll and window proc only enqueue. The
  ImGui frame publishes swallow, flying and settings-drawn at its end. The UI's game writes (vanity camera INI, keyboard
  reset) run in `Menu::UpdateGameSide` at the input poll.
- **Game-side code reads settings adopted, never live:** GrassCollision converted. The SkySync, Cinematic Camera and DoF
  fields are listed for phase 7.
- **Window resize is a snapshot event** (`Events::windowSize`, newest request plus serial, native only). The render frame's
  start asks the renderer for it.
- **The update-rate cap** is `State::updateRateCap` (default 60, Advanced "Update Rate Cap"). `Snapshot::PaceGameUpdate`
  waits after the render half, only with `CS_UPDATE_RATE_CAP=1` until phase 7.
- **PCL ids:** a render frame's Streamline id is its render frame index. Simulation markers use the render frame that
  first renders the update's snapshot. SimulationStart is emitted once per game update and SimulationEnd once per
  snapshot.
