# snap7-frameend: the frame end ready for the render thread (phase 7 groundwork)

Lead `frameend`, round `snap7`, branch `snap7/frameend` (base `org_integration` ca9035fe0), stage `s7-frameend`. Still today's
single-threaded frame: no render thread. Plan: [snap7-prep.md](snap7-prep.md) "Phase 7 plan" item 2 (Present becomes publish),
[snap7-split.md](snap7-split.md) sections 6-7, [snap7-reads.md](snap7-reads.md) section 5 (ImageSpace and frame end).

## 1. The Present hook: a render-side frame end and a game-side part

`IDXGISwapChain_Present::thunk` (Hooks.cpp) is now two calls:

- **`RenderFrameEnd(swapChain, sync, flags)`**: the old body, unchanged in order: GPU timing, the UI stage's commit and the overlay
  (inside HDR Display's and Upscaling's chain), DCLF's `BeforePresent` (`queueFrameEnd`), the present, the seal, `Constants`/`Buffers`
  `EndFrame`, `WaitFramePace`, metrics, the [Perf] line, `State::Reset` (every feature's `Reset`, DCLF's `EndFrame` among them), the
  test runner's render side (`Testing::OnFrameEnd`), the frame recorder, GPU counters, Tracy. It holds the live-read detector's
  `Half::FrameEnd` scope. It reads only the adopted snapshot, CS's frame and render-owned state, apart from DCLF's reads (below).
  The render thread calls it later.
- **`Testing::OnGamePresent()`** after it: the game side of the Present point (below). Phase 7 calls it at the game thread's publish.

Nothing moved in time inside `RenderFrameEnd`; the test runner's steps still run after the present, as before.

## 2. What moved to the game side

### CS's per-update engine writes (`Snapshot::SideEffects::AfterCapture`, native)

`State::Reset` made these at every Present. In native they now run once per game update right after the capture, before the render
half (`SideEffects::WriteCsEngineState`); guest keeps them in `State::Reset` (`SideEffects::Installed()` picks, the same predicate as
the side effects' hooks):

| Write | Reads | Native now |
|---|---|---|
| `LODBlendRingFix::Update` | TES, player, the shader state's grid centres, `uGridsToLoad` | after the capture, slides by the update's `gameDelta` (guest: `Snapshot::GameStep()`, as before) |
| `SunShadowMaskFix::Update` | the shader state's shadow node, its sun and `shadowLightsAccum` | after the capture (the same light list the previous Present saw: the casters select later in the game half) |
| Subsurface Scattering's character light (`ApplyCharacterLight`) | settings, shader state | after the capture, from the adopted settings; `CharacterLightingStrength` is now a bound `CS::Setting<float>` (edited with `Util::SliderFloat`) |
| ApplyReflections' `active` off | ImageSpaceManager +0x168 | after the capture |
| `bEnableImprovedSnow:Display` false | INI | after the capture |

Effects:
- LODBlendRingFix's center is written before the update's draws, so a grid shift is seen in the same update instead of one later
  (DCLF's frame globals take the grid centres in the game half after this point).
- The ApplyReflections flag: Ghidra shows one engine writer, the console's `ToggleAR` (AE 0x140376f40, id 23140), on the game thread.
  With the clear after the capture the flag is always off in a native render half, so native's post-resolve no longer reads it
  (RenderWorld.cpp: the TAA mask and kMAIN_ONLY_ALPHA clears it guarded never ran).

### The test runner (`Testing::OnPresent` split)

- `Testing::OnFrameEnd` (render side): the render frame count, the memory probe start, the VRAM spill lab, the live-image shots and
  the exit's wait for the shots.
- `Testing::OnGamePresent` (game side): the step clock for own frames, the lifecycle (milestones, the player read), phase timeouts,
  queued commands, the steps and their ticking commands.
- **Shots name the snapshot they render (decision 51).** `ScreenshotFeature::RequestCapture(path, minSnapshot)`: a shot step inside a
  game update asks for `Snapshot::LastGameId() + 1`, the next update's snapshot, the first with the step's work in it (the steps'
  game work runs after the update). The render side takes a request only in a frame whose adopted snapshot is at least that
  (`Snapshot::CurrentId()`), so a repeated or older snapshot is never shot, and a decoupled render thread takes it when it renders
  that snapshot. Outside an update (main menu, loading screen) the next frame, as before. The log gives both lines:
  `shot X asked at game snapshot N: renders snapshot N+1 or newer` and the existing `shot X rendered snapshot M (render frame F)`.
  In a normal cadence this is the same frame as before; in a repeat cadence the shot waits for the next update's first render.

## 3. ImageSpace

- **The world scopes' state at Begin** (`PostChain::ReadWorld`, used by the frame description at Begin and by the step-lag check at
  record, PostChain.cpp:402): in native it reads the snapshot's ImageSpace block (`ReadWorldFrom`): the INI switches the block already
  had, the frame counter (`GameInputs::frameCounter`, which S2 moves on the game thread once per update), and new `WorldSettings`
  fields taken in the capture: the reflections', SAO's and the snow pass's active flags (`ssrOn`, `saoOn`, `snowOn`; the console's
  toggles are their only engine writers, Ghidra scan of the manager's users) and the composite's portal fog (`portalFog`,
  `portalFogValues`: the portal graph's fog property and its ten values). Only the SAO counter (+0x54) and kMAIN's height, render
  state, are still read there. Guest reads the engine as before.
- **The native draws read the same values** so the description and the draws agree: `ReadComposite` (form and fog rows),
  `EngineNormalsParity` (native: the snapshot's SSR flag and frame counter), `RenderWorldImageSpace` (the snow test and
  `saoReflections`). Guest's `ReadComposite` reads `ReadWorldSettings()` at its hook (the same reads at the same point as before).
- **ApplyReflections' `active`**: section 2.
- **Left at Begin** (`PostChain::Read`, the post fields): in native the game fields already come from the snapshot (snap5); still read
  live are refraction's slot (its geometry turns it on in the render), the adaptation target (render state), TAA's +0x18/+0x38 and the
  manager pointer itself. Decided by the agent, to confirm: render-owned (written by render code), so left.

## 4. Detector (native-vulkan+quick, Dev-Fast, per world frame)

| | before (s7-reads-c) | after (s7-frameend-a) |
|---|---|---|
| frame end, all | 13.4 | 6.7 |
| frame end, game-mutated | 8.7 | 2.02 (all DCLF: FrameView.cpp:291 1.66, LoadHold.cpp:152 0.34, :133 0.01; snap7-hold's) |
| frame end, game-mutated, not DCLF | 6.7 | 0 |
| record, game-mutated | 10.9 | 10.9 (PostChain.cpp:402's 0.03 gone; the rest is DCLF scene work and precipitation, other leads) |
| whole run live reads | 545 | 526 |

Gone from the frame end: State.cpp:432 (ImageSpace), SunShadowMaskFix.cpp:24, LODBlendRingFix.cpp:51/52/60, SubsurfaceScattering.cpp:425,
Testing/Lifecycle.cpp:133, Testing/Commands.cpp:311. The render-owned frame-end reads left: ThemeManager (GraphicsState, the overlay),
HDRDisplay.cpp:1188 (Renderer), Utils/Game.cpp:288 (GraphicsState), ScreenshotFeature.cpp:300 (Renderer).

DCLF's frame-end reads (EndFrame's load note, LoadHold) belong to `snap7-hold`, which keeps DCLF's calls where they are in
`RenderFrameEnd` and makes them read the snapshot (its `Snapshot::RegisterUpdateEnd` for LoadHold's engine part).

## 5. Runs

Stage `s7-frameend`, Dev-Fast.

| Config | Label | Result |
|---|---|---|
| native-vulkan+quick, detect | s7-frameend-a-nvk-quick | passed (shot reference); frame end 6.7 reads, 2.02 game-mutated (DCLF's), live 526 |
| guest+quick | s7-frameend-a-guest-quick | passed (shot reference) |
| native-vulkan+post-effects | s7-frameend-a-native-vulkan-post-effects | passed, 13 shots |
| native-vulkan+vanilla-screenshot | s7-frameend-a-native-vulkan-vanilla-screenshot | passed |
| native-vulkan+save-screenshot | s7-frameend-a-native-vulkan-save-screenshot | passed (loaded, saved) |
| native-vulkan+menu-ui (ImGui) | s7-frameend-a-native-vulkan-menu-ui | passed, every cs-ui-expect holds |
| native-vulkan+window-resize-stress-taa | s7-frameend-a-nvk-wrstaa | passed, 13 shots |
| native-vulkan+update-cap30 | s7-frameend-a-native-vulkan-update-cap30 | passed |
| native-vulkan+update-cap240 | s7-frameend-a-native-vulkan-update-cap240 | passed |
| native-vulkan+cadence-drop-cells | s7-frameend-a-native-vulkan-cadence-drop-cells | passed, 3 shots (each renders the first rendered update after the step) |
| native-vulkan+sss (character light) | s7-frameend-a-native-vulkan-sss | passed, 4 shots |
| native-vulkan+ssr-divider4 (scope switches) | s7-frameend-a-native-vulkan-ssr-divider4 | passed |

Built from 9439f2165 (later changes: line endings and one blank line). Skipped: native-d3d12-quick (no ORG, BasicRHI or native
device code changed).

## 6. Decided by the agent, to confirm

- FE-a. **The game side of the Present point runs after the render side** (`RenderFrameEnd`, then `Testing::OnGamePresent`), so the
  steps keep running after the present, as before; phase 7 moves `OnGamePresent` to the publish.
- FE-b. **Guest keeps CS's engine writes in `State::Reset`**; native makes them after the capture. Why: guest's behaviour must not
  change, and in guest the engine's own render half reads them at its own times.
- FE-c. **LODBlendRingFix moves earlier by one update in native** (its center is written before the update's draws), and slides by the
  update's own `gameDelta`, so a dropped render loses no slide time.
- FE-d. **A shot inside a game update renders the next update's snapshot or a newer one** (section 2): in repeat cadence it waits for
  the next update instead of shooting a repeat.
- FE-e. **ApplyReflections stays off without a render-side write**: its only engine writer is the console's ToggleAR, so the game-side
  clear after the capture suffices and native's post-resolve drops its read.
- FE-f. **The world scope switches and the portal fog are snapshot values in native**, for the description at Begin and the draws
  alike; a portal change inside the game half (after the capture) reaches the composite one update later.
- FE-g. **`CharacterLightingStrength` became a bound setting** so the game side reads it adopted (decision 199's rule).

## 7. Proposed decision-log entries

- **The Present hook is a render-side frame end plus a game-side part:** `RenderFrameEnd` (Hooks.cpp, the old body, under
  `Half::FrameEnd`) reads only the snapshot, CS's frame and render-owned state apart from DCLF's load reads (snap7-hold);
  `Testing::OnGamePresent` follows it (phase 7: at the publish). Frame-end game-mutated reads in quick: 8.7 -> 2.0 (DCLF's only).
- **CS's per-update engine writes are game-side in native** (`SideEffects::WriteCsEngineState` after the capture): LODBlendRingFix,
  SunShadowMaskFix, the character light (adopted settings), ApplyReflections off, improved snow off. Guest keeps them in `State::Reset`.
- **Shots name the snapshot they must render** (`RequestCapture(path, minSnapshot)`; a shot step in an update asks for the next
  update's snapshot); the test runner's render side (`Testing::OnFrameEnd`) counts render frames, takes shots and exits.
- **The world scopes' state is a snapshot value in native** (`WorldSettings` gains the scope switches and the portal fog;
  `PostChain::ReadWorld`, `ReadComposite`, `EngineNormalsParity`, `RenderWorldImageSpace` read it).
