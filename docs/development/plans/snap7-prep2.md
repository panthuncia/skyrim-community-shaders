# snap7-prep2: ImGui and settings ready for the render thread (2026-11-26)

Lead `prep2`, branch `snap7/prep2` (from org_integration 7214593fd), stage `s7-prep2`. Phase 7 items 4 and 5 of
`snap7-prep.md`, landed in today's single-threaded frame with no behaviour change, before `snap7-split` creates the
render thread.

## The game-thread command queue (`src/Utils/GameThread.{h,cpp}`)

- `GameThread::Post(what, task)`: any thread. The task runs on the main thread at the next main tick
  (`RenderGraph::Engine::MainTick`, after Main::Update's frame-job wait), in post order. Installed in `Hooks::Install`
  in every run, right after `Snapshot::InstallHooks` (fatal when the tick cannot be hooked). VR has no main tick: tasks
  run where posted.
- `GameThread::Run(what, task)`: runs now where game state may be written (`CanWriteNow`: inside a posted task, or on
  the main thread outside the ImGui frame: game hooks, engine events, test commands at Present), else posts. Shared
  helpers that both the menu and game-side code call use it, so the game side keeps its timing.
- `GameThread::UiFrameScope`: opened by `OverlayRenderer::RenderOverlay` around the whole ImGui frame. Inside it:
  - the live-read detector counts each engine read as a UI-frame read too (`Site::ui`; `[Snapshot] UI-frame read site`
    lines and `report.engine_reads.ui_frame` under `CS_SNAPSHOT_DETECT=1`);
  - `GameThread::NoteEngineWrite(what)` catches an engine write made outside a posted task: logged once per name,
    counted (`report.ui_engine_writes`, `GameThread::MissedWrites()`), fatal in test runs. It sits in `Util::ForceWeather`
    and `EditorWindow::MaintainWeatherLock`.
- Timing today: the ImGui frame runs at Present inside Main::DrawFrame; the main tick runs right after DrawFrame in the
  same Main::Update. A posted write lands before the next game update and the next snapshot capture, as the direct
  write did.
- cs-test's own task queue (`Testing::AddTask`, decision 163) stays separate.

## ImGui ready for the render thread (item 4)

- **Entry point unchanged:** `Menu::DrawOverlay()` from wherever the frame end calls it. It needs nothing at frame start:
  it takes queued input and posted UI tasks itself and publishes the UI state at its end.
- **UI tasks:** `Menu::PostUiTask(task)` (any thread) runs a task at the start of the next ImGui frame, before input.
  `Menu::RequestOpen(bool)` is a queued open/close on it. Users: cs-test's `cs-menu` (open, close, font) and the native
  pause menu's "Open Community Shaders Menu" row. `Menu::Open`/`Close` are now ImGui-frame only.
- **Cursor:** `ImGuiConfigFlags_NoMouseCursorChange` is set at init. The menu draws its own cursor (`MouseDrawCursor`), so
  the Win32 backend never calls `SetCursor`, which acts only on the calling thread's input queue.
- **Reads of game state from the ImGui frame:** the CS Editor's clock and sky now come from `EditorWindow::GameView`,
  published by the game thread at each main tick (`EditorWindow::InstallGameTick`, also installed in `Hooks::Install`):
  game hour, timescale, the sky's hour, current/last/default weather, transition fraction, climate. Readers converted:
  the editor's time sliders and controls, the action bar's weather status, `WeatherUtils::GetCurrentGameTime` and
  `GetTimeOfDayFactors` (the period title), `SceneSettingsManager::GetCurrentGameHour`. The time sliders keep their own
  value while dragged, so a decoupled render thread does not snap them back between ticks.

## Settings read on the game thread (item 5)

Bound `CS::Setting` with `.Adopted()` reads on the game side (the menu writes them through `Util::Checkbox`/`SliderInt`/
`SliderFloat`, which raise):

| Feature | Fields | Game-side readers |
|---|---|---|
| Sky Sync | Enabled, UseAlternateSunPath, MoonLightSource, SunPath, CustomAngle, MinShadowElevation, ShadowTransitionDuration, DimSunlightUnderHorizon, DimVolumetricLighting, HorizonFadeHours, HorizonFadeElevation, HorizonElevation, NewMoonIntensity, CrescentMoonIntensity, FullMoonIntensity | `Sky::Update` hook and its helpers, `Capture`, CoreFrame's `CaptureSky`, Effects11's `Capture` |
| Sky Sync | DisabledWorldspaces (a string list, not a frame variable) | a game-side copy (`gameDisabledWorldspaces`) handed over through the queue (`PublishDisabledWorldspaces`) |
| Physical Sky | forceEnableAllInteriorCells | Sky Sync's update, Interior Sun's `IsInteriorWithSun` |
| Cinematic Camera | Enabled, Filmback, Lens, Focus, Exposure (one setting per group) | `Controller::Update`/`BuildState` in Post Processing's capture |
| DoF | consoleSelection, targetFocus | Post Processing's capture (focus target search) |

- Sky Sync's `sunAngle` member is gone: `GetSunAngle()` computes it from the adopted path settings, so the menu no
  longer writes game-side state (`SetSunAngle`, `UpdateWorldspaceDisabled` were called from the menu).
- The Cinematic Camera's groups are struct settings. The menu edits them in place (scene authoring binds the field
  addresses) and raises them when its page ends (`Controller::RaiseEdits`). `SettingRegistry::RaiseAt` now finds the
  setting that contains an address, so the scene interceptor's raw writes to a group member raise the group.
- Post Processing's `bypass` is never written; `pipeline[idx]` is built once at setup (only SaveSettings builds a
  temporary default pipeline before setup), so its pointer reads are safe.

## Every ImGui-driven engine or game write, and where it goes now

| Write | Where | Now |
|---|---|---|
| Game hour (time slider) and the lock's `lastWeatherUpdate` | `EditorWindow::DrawGameHourSlider` | `EditorWindow::SetGameHour` (Run) |
| Timescale slider | `DrawTimeScaleSlider` | `SetTimeScale` (Run) |
| Pause, resume, reset timescale | `PauseTime`, `ResumeTime`, `ResetTimeScale` (menu bar, time panel, Scene Manager panel's pause, test conditions) | post themselves unless `CanWriteNow`; the menu-guard (`SetTimeRunningForMenu`, an engine event) runs them at once |
| "Time paused" cleared by an external timescale change | `DrawTimeControls` | the editor's game tick |
| Weather lock: region save, ForceWeather, override slot | `LockWeather` | lock atomics at once, sky work via Run |
| Weather unlock: override, default weather, region | `UnlockWeather` | atomics at once, sky work via Run |
| The overlay's implicit lock (reads the sky's transition) | `LockWeatherForOverlay` from `UpdateOpenState` | posted, one attempt queued at a time |
| Weather lock upkeep | `CSEditor::Prepass` (render side) | the editor's game tick |
| Weather picker: select (Force/SetWeather), reset (ResetWeather) | `CSEditor::SelectWeather`, `ResetWeatherToDefault` | posted; the reset's new selection comes back through `s_resetToWeather` |
| Form edits (weather, ImageSpace, VL, precipitation, lens flare, reference effect, lighting template, art object, effect shader, cell lighting) | every `Widget::ApplyChanges` caller (apply buttons, auto-apply, revert, undo, load, inherit) | `Widget::RequestApply` (Run) |
| Weather reinit (ForceWeather, ReleaseWeatherOverride) | `Widget::ForceWeatherReinit`, `ForceCurrentWeatherReinit` | Run |
| Free camera `tfc`, `sucsm` | `EnterPreviewMode`, `ExitPreviewMode`, `AdjustFlySpeed` | posted |
| Game menus hidden/shown (`UI::ShowMenus`) | `HideGameMenus`, `ShowGameMenus` | posted |
| Scene Manager period bar's game hour | `SceneSettingsManager::SetGameHour` | Run |
| Run test conditions (console: speedmult, tgm, tcl, gamehour, coc, fw; pause) | `AdvancedSettingsRenderer::RunTestConditions` | posted |
| Light editor console scripts | `ScheduleConsoleCommand` (was SKSE's task interface, which runs on a job thread here) | posted |
| Light editor: reference position, light node transform, base light flags, Disable/Enable rebuild, shadow bias | `UpdateSelectedLight`, `RestoreOriginal`, `RequestRefRefresh`, `UpdateRefRefresh`, `ApplyShadowDepthBias` | posted (handles and NiPointers captured) |
| `bAllowScreenShot` INI | `ScreenshotFeature::ApplyVanillaScreenshotSetting` (menu checkbox, load) | Run |
| `bLensFlare:ImageSpace` INI | `PostProcessing::UpdateVanillaEffects` (settings change, tonemap owner) | Run, only when the choice changes |
| Vanity camera INI, keyboard `ClearInputState` | Menu (snap7-prep) | already at the input poll |

## Decided by the agent, to confirm

- `NoMouseCursorChange` rather than posting cursor changes: the game hides the OS cursor and the menu draws its own.
- `Widget::RequestApply` captures the widget, not a copy of its settings: a widget is deleted only from the ImGui frame
  after a confirmation popup. In a decoupled frame the task reads the widget's settings while the menu may edit them;
  the split should hand the task a copy (per widget type) or run the apply under the widget list's ownership.
- The editor's game view lags one game update in the menu (published at the main tick). Invisible at normal timescales.
- `GameThread::Run` keeps game-side callers synchronous (the menu-guard around fast travel needs the timescale back
  before the map menu's travel starts).

## Open for phase 7 (reads from the ImGui frame not converted)

The `UiFrameScope` detector lists them per run (`[Snapshot] UI-frame read site`). In `s7-prep2-nvk-menuui2` (menu open
about 300 frames, CS Editor closed) it counted 2875 reads at 17 sites at exit:
- every frame: `Features/CSEditor.cpp:1112` (the weather overlay's in-world check, the player, 1345) and
  `EditorWindow::CanBeOpen` (`EditorWindow.cpp:171` the UI, `:2574` the player, 488 each). These are left live because
  of loading screens: the main tick does not run while the main thread loads, so a published flag would still say "in
  the world". In the split, they belong with the frame kind the render thread already knows.
- while the menu draws: the Scene Manager's location and period code (`SceneSettingsLocation.cpp:85`,
  `SceneSettingsLocationTargets.cpp:138-140`, `SceneSettingsInternal.cpp:883-884`, `SceneSettingsManager.cpp:346, 1150`,
  `SceneSettingsUI.cpp:1124`; the sky and the player, about 10 a frame), and one-off reads in `Upscaling.cpp:357`,
  `PresetCompatibility.cpp:118`, `Climate.cpp:21`, `ImGuiRenderer.cpp:184`.
- `native-vulkan+quick` with the menu closed: 1084 at exit, all from the two every-frame sites.

Other known groups the menu-ui run does not reach:
- the light editor's scene traversal (`GatherLights`) and its writes to CS's own per-light data (`RuntimeLightDataExt`
  fade, the hover flash), which Light Limit Fix reads;
- the weather picker, transition bar and weather details window read the sky;
- form widgets read their forms when they open (static data);
- `EditorWindow::CanBeOpen` / `IsOpeningBlocked` read the player and the UI's menu stack;
- Sky Sync's debug section, the Skin editor's actor pick, Post Processing's camera summary (`GetState`);
- `PostProcessing::Capture` reads `State::GetTonemapOwner` (render-side state) through `IsPipelineRunning`.
- `Util::GameSetting`'s ImGui tree edits INI values in place; it has no callers.

## Tests

cs-test gets two commands (Testing/Commands.cpp) and a config:
- `cs-ui <action>`: posts the action as a UI task, so it runs inside the ImGui frame as a widget's handler does. Actions:
  `setting <ShortName>.<key>=<JSON>`, `hour=<h>`, `timescale=<x>`, `pause`, `resume`, `lock=<weather>`, `unlock`.
- `cs-ui-expect <state>`: fails the step unless the game state matches. States: `hour=`, `timescale=`, `paused`,
  `running`, `lock=`, `unlocked`, `setting ...` (the feature's saved value), and `ui-writes=0`.
- `configs/menu-ui.toml` (compose it: `guest+menu-ui`, `native-vulkan+menu-ui`): opens the menu, then sets a Sky Sync
  setting (one read adopted on the game thread), pauses, sets the hour to 20, locks the weather to 81A (SkyrimClear),
  unlocks, resumes, sets the timescale to 20, puts the setting back and checks `ui-writes=0`. Shots before, during and
  after.
- Until this merges, the main checkout's cs-test needs the config by path:
  `native-vulkan+C:/.../snap7-prep2/tools/cs-test/configs/menu-ui.toml`.

## Runs

All on stage `s7-prep2` (Dev-Fast; built from 3d53a6b12, plus clang-format and notes only):

| Config | Label | Result |
|---|---|---|
| guest+quick | s7-prep2-guest-quick | passed (shot reference) |
| native-vulkan+quick, `CS_SNAPSHOT_DETECT=1` | s7-prep2-nvk-quick | passed (shot reference); live reads 639 a world frame, engine render calls 1 a world frame |
| native-d3d12-quick | s7-prep2-nd12-quick | passed (shot reference) |
| native-vulkan+menu-scenes | s7-prep2-nvk-menuscenes | hang at the loading screen: data-loaded took 119 s, and the main thread was stuck in engine code with no CS frame on any stack (the machine was busy) |
| native-vulkan+menu-scenes | s7-prep2-nvk-menuscenes2 | passed (5 shots) |
| native-vulkan+window-resize-stress-taa | s7-prep2-nvk-wrstaa | passed (13 shots) |
| guest+menu-ui | s7-prep2-guest-menuui | passed; every cs-ui-expect holds |
| native-vulkan+menu-ui, `CS_SNAPSHOT_DETECT=1` | s7-prep2-nvk-menuui, s7-prep2-nvk-menuui2 | passed both; every cs-ui-expect holds, `ui-writes=0` |
| native-vulkan+cadence-drop-cells | s7-prep2-nvk-dropcells | passed |

The first native menu-ui run went to about 1 fps after the resume step. Bisected with throwaway configs:
- the same time and weather changes through the console were fast on this stage, with and without the menu open;
- removing the Sky Sync setting steps made menu-ui fast;
- the console sequence plus `MinShadowElevation=20` was fast on this stage and slow on the base stage `nfg`.

Other leads' games were running on the same GPU at the time, and the rerun (`s7-prep2-nvk-menuui2`) was fast throughout.
So this was contention, not this change.

Detector: the live-read totals are within the run-to-run range of the base (`native-vulkan+quick` 639 a world frame;
the snap0 baseline was 1848, and later phases brought it down to this range). The new `ui_frame` number in
`report.engine_reads` is the item 4 metric: 1084 with the menu closed. The two every-frame sites above make up all of it.

## Proposed decision-log entries

- **The game-thread command queue:** `GameThread::Post` runs a task on the main thread at the next main tick. The ImGui
  frame writes engine and game state only through it. `GameThread::Run` keeps game-side callers synchronous.
  `UiFrameScope` counts UI-frame engine reads, and `NoteEngineWrite` makes a direct write from the ImGui frame fatal in
  test runs.
- **UI state from other threads goes through UI tasks:** `Menu::PostUiTask` and `Menu::RequestOpen`. `Menu::Open`/`Close`
  are for the ImGui frame only. The menu draws its own cursor (`NoMouseCursorChange`).
- **The menu reads the game's clock and sky from `EditorWindow::GameView`,** which the game thread publishes at each main
  tick.
- **Settings read on the game thread are bound and read adopted:** Sky Sync (15 scalars, plus its worldspace list handed
  over through the queue), Physical Sky's `forceEnableAllInteriorCells`, the Cinematic Camera (5 settings, the groups as
  struct settings), DoF's `consoleSelection` and `targetFocus`. `RaiseAt` raises a struct setting from a member's
  address.
- **CS Editor form edits apply on the game thread** (`Widget::RequestApply`). The weather lock's upkeep moved from
  `CSEditor::Prepass` to the editor's game tick.
