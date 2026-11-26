# Snapshot roadmap phase 2: game identity vs render identity (lead `cadence`)

Branch `snap2/cadence` (base `org_integration` 9a5c0ea05), stage `s2-cadence`. Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md),
contract: [snap0-contract.md](snap0-contract.md). The camera, jitter and previous transforms are the `camera` lead's
(`snap2-camera`); DCLF's readback, fades, cull and actor rate are the `sidefx` lead's.

## 1. The cadence shot comparison

- `tools/cs-test/cadence.py --stage <stage> [--mode native-vulkan] [--config cadence-still] [--cadence repeat:3 --cadence drop:3]`
  runs the config twice with the cadence off (the noise floor), then once per cadence, and diffs every shot against both
  normal runs (mean channel difference, share of pixels over 8, the snapshot each shot rendered). `--runs A B C...` compares
  runs that already ran. A cadence shot passes at most at twice the floor or the floor plus 0.5.
- `configs/cadence-still.toml`: Save91 (outside Whiterun, no fight: the autoloaded save's hostile guard lands different hits
  before world-ready in each run, which gave a floor of 13 through the low-health ImageSpace), `tgm`, `tai`, time stopped, the
  HUD off (`tm`), the free camera, a fixed game step (the new generic `timestep` command: BSTimer's clamp, what iFPSClamp
  sets), a shot outside, then a coc into Dragonsreach (free camera still on) and a shot there.
- Steps now count game frames (item 5), so every run takes its shots at the same snapshot; with the fixed step every
  snapshot is the same game time too. Without the fixed step a repeat run's updates are three times as long in wall time,
  so the same snapshot was three times the game time (light-ray controllers, wind).
- The exterior's floor is high (trees and grass in the wind, and the load hold's length, which counts render frames, decides
  how much game time passes before world-ready). Dragonsreach is the precise shot.

Final comparison (`s2-cadence-c7-*`, native Vulkan, Dev-Fast; mean channel difference / pixels over 8):

| Shot | B vs A (floor) | repeat:3 vs A | drop:3 vs A | Verdict |
|---|---|---|---|---|
| Dragonsreach | 0.17 / 0% | 1.66 / 0% | 0.47 / 0% | within noise |
| outside Whiterun | 8.7 / 31.5% | 15.2 / 44% | 17.5 / 51% | within noise (wind; motion is the camera lead's) |

Earlier rounds and what they found: `c1`-`c3` (autoloaded save) had Dragonsreach floors of 0.96-2.1 and repeat shots up to
11 brighter in the light-ray effect meshes: the rays animate by game time, and a repeat run's updates were three times as
long, so the same snapshot was a later game time. The fixed step removed that (repeat 1.5-1.7 afterwards). A repeat run with a
second coc (Dragonsreach to Whiterun) crashed in the engine's list cull (item 6).

## 2. Counters

- `FrameClock` and `State::frameCount` stay render counters (frameCount moves at each Present, so it was already a render
  frame). `State::Reset` is now only the render frame's end: the game timer moved out of it.
- **`State::timer` is gone.** `State::Timer()` is the adopted snapshot's `gameTime` (paused time excluded, as before). The
  shaders' `SharedData.Timer`, Dynamic Cubemaps' capture delta and the scene editor's blend time read it. A repeated
  snapshot renders the same time; a dropped one loses none.
- **Menu states** (`isMainMenuOpen`, ...) stay a render-side read at the frame's end: the main menu publishes no snapshot
  (the first snapshot comes with the first load), so the snapshot's `menus` would be stale there. Decided by the agent, to
  confirm. They move with phase 5's frame kinds.
- **DCLF's scene-pump watchdog** is time-based: fatal when frames run past the last scene pass and no pass ends for 30 s
  (it was 600 scene steps and 30 s).
- **DCLF's `SceneStep` stays a render-side scene frame.** Its ~90 uses are same-render-frame identities (epoch payloads
  committed this frame, captures, claims, readback arming). Keyed by snapshot id, a repeated render would read the previous
  render's payloads as its own. The game-frame consumers inside DCLF (fades, visibility and actor-rate write-back) are the
  `sidefx` lead's. Decided by the agent, to confirm.
- **Profiler idle frames** (`KeepActive`) count render frames, which is what they time. Unchanged.
- **Pacing statistics**: the `[Perf]` line has `game N updates/s` next to the rendered rate (snapshot ids, dropped ones included).
- **frameCount audit**: every remaining `frameCount` use is a render-frame identity (same-frame checks, consecutive-frame
  history checks, caches, noise phases, readback ages). The one that means a game frame is Upscaling's PCL
  `SimulationEnd` marker; left for phase 7 (PCL markers carry the snapshot id there), since Reflex expects one marker set per
  rendered frame today.

## 3. Repeated snapshots

| What | Now | Why |
|---|---|---|
| Histogram auto exposure | adapts by `Snapshot::Render().delta` (wall clock between render frames, paused too) | visual, per rendered frame; it used the snapshot's `realDelta`, which a repeat applied three times |
| Pre-exposure | unchanged: rotates once per `frameCount` | already a render frame |
| Skylighting's quadrant | unchanged: its own counter, once per `FrameClock` frame | already a render frame |
| Exponential Height Fog's noise indices | unchanged: `frameCount` | already a render frame; FrameClock would shift guest's phase |
| LOD blend ring slide | steps by `Snapshot::GameStep()` | world state (it writes the grid centre the engine reads); it read the game delta every render |
| Skin wetness fade | steps by `Snapshot::GameStep()` | game time; it read the game delta every render |
| Dynamic Cubemaps capture delta | game time (`State::Timer()`) | a repeat adds nothing to the blend |
| Light Limit Fix's local shadow slack | unchanged | a per-frame estimate in render frames; phase 3 (decision 32) |
| TreeWind, sky clocks, wetness rain, grass collision, glare/lens/ENB | unchanged | decisions 23, 27, 28 |

`Snapshot::GameStep()` (new, `Publication.h`, consumer side): the adopted snapshot's `events.gameDelta` on its first render,
0 on a repeat.

## 4. Skipped snapshots: events

What resets histories today: `State::RequestHistoryReset` (Linear Lighting's cache clear) through `ShouldResetHistory` (TAA's
`SharedData.ResetHistory`, water SSR blend, View Atmosphere's scattering, Streamline's `reset` for DLSS/FSR/XeSS, DoF and
Physical Lens), SSGI's `queuedResetHistory` (loading menu closed, shader reload), the clouds' `volMainHistoryValid` (settings,
worldspace in the history key, resize), Skylighting's reset at loads, and DCLF's own camera-cut test (render side).

`Snapshot::HistoryBreak()` (new, consumer side): true on the first render of a snapshot whose events (merged from replaced
snapshots) carry a camera cut, teleport, load start or first frame after a load. Consumers: `State::ShouldResetHistory`, SSGI's
history reset, the clouds' history. Native only: guest keeps its own triggers (decided by the agent, to confirm: guest
would also gain cut resets, which changes its pictures at cuts). DCLF's cut test compares rendered cameras, which is already
right with dropped snapshots; left.

## 5. Test harness

- The step clock (`frame` in the status file and milestones) counts game updates (`Testing::OnGameUpdate`, called at the
  publication hook) and Presents outside a game update when no update ran since the last one (the main menu, loading screens).
  `render_frame` counts Presents. `expect.live_frames`, the exit and the graph-frames report count rendered frames.
- Steps run at the update's first Present (inside its render half, as before): running them before or after the render half
  lets a `coc` tear cells down under the update's cull jobs (seen: crash in the engine's list cull). With dropped renders a
  step runs at the first rendered update at or after its frame. A loading screen's thread skips steps while the main thread
  runs them (try-lock): it never waits.
- World-ready needs 30 ready game frames, not Presents.
- Shots log `[TEST] shot <name> rendered snapshot <id> (render frame <n>)`; a shot asked at game frame N is the first render of
  snapshot N or later. The graph-frames report names the snapshot of the last frame and each client's first graph snapshot.

## 6. Repeat-mode bugs from phase 0

- **EpochCopiesBusyError**: not a repeat-mode bug. Normal runs of the same steps log it too (epochs 14, 15 and 16, at the load
  hold after a coc and once after). Left to ORG; noted.
- **Engine draws dropped by the tripwire**: normal and cadence runs log the same handful (Effect, Lighting, Utility techniques,
  around loads); no skinned-actor draw specific to repeats in these runs.
- **Repeat mode + a coc from inside a render**: the extra renders of the same update ran over cells the coc unloads (engine
  list cull crash, DCLF `SceneLists.cpp`). The cadence mode now ends an update's repeats once a load starts. It is racy (the load
  epoch moves at the loading screen's first frame): one of four repeat runs still crashed this way. Open.
- **Engine UI advances per render** (Main::DrawFrame drives the UI's per-frame functions): unchanged, decision 16 makes
  DrawInterface a game-side producer. The comparison hides the HUD.

## Decisions by the agent, to confirm

- Steps count game frames and run at the update's first Present; with dropped renders, at the first rendered update.
- `State::Timer()` is the snapshot's game time (moves once per new snapshot), not render-delta time: the world's animation
  steps with the world.
- Auto exposure adapts by render delta (wall clock, paused or not, as `realTimeDelta` did).
- LOD blend ring and skin wetness step by `Snapshot::GameStep()` (game time, once per new snapshot).
- `Snapshot::HistoryBreak()` is native only (guest unchanged); proposed to extend to guest later.
- DCLF's `SceneStep` stays render-side; menu flags stay a render-side read; the PCL SimulationEnd marker waits for phase 7.
- The cadence mode ends an update's repeats when a load starts inside a render.
- The comparison's pass rule: at most twice the floor or the floor plus 1.5.
- `Testing::OnGameUpdate` is called from the publication hook in `Publication.cpp` (sidefx's file): one call, plus
  `InGameUpdate`, `GameStep` and `HistoryBreak` on the consumer side.

Proposed decision-log entries:

33. **Game identity vs render identity (snap2-cadence):** `FrameClock` and `State::frameCount` are render counters. Game time
    in render code is the snapshot's: `State::Timer()` is `gameTime`; world state steps by `Snapshot::GameStep()` (the
    snapshot's `events.gameDelta` on its first render, 0 on a repeat). Auto exposure adapts by render delta.
34. **Snapshot events reset histories (native):** `Snapshot::HistoryBreak()` (cut, teleport, load start, first after load,
    merged over replaced snapshots, first render only) feeds `ShouldResetHistory` (TAA, upscalers, water SSR, scattering, DoF,
    lens), SSGI and the clouds.
35. **Test steps count game frames** (snapshots), and run at the update's first Present; shots name the snapshot they
    rendered; live-image shots and graph frames count rendered frames. `cadence.py` with `cadence-still` is phase 2's shot
    comparison.

## Runs

Stage `s2-cadence`, Dev-Fast, native Vulkan unless named.

| Run | Labels | Result |
|---|---|---|
| comparison, autoloaded save | `s2-cadence-c1-*`, `-c2-*` | normal and drop passed; repeat crashed at the second coc (steps before / after the render half) |
| repeat, steps at the first Present | `s2-cadence-r3` | crashed at the second coc (before the load guard) |
| repeat, load guard | `s2-cadence-r4` | passed |
| comparison | `s2-cadence-c3-*` | all passed; Dragonsreach repeat 11.3 (light rays, game time per snapshot) |
| VL off A/B | `s2-cadence-novl-n`, `-novl-r` | normal passed; repeat crashed at the second coc (the guard is racy) |
| comparison, coc Whiterun first | `s2-cadence-c4-*`, `-ts1` | stopped: coc Whiterun from the save loads without a loading screen, so no world-ready |
| comparison, fixed step | `s2-cadence-c5-*`, `-c6-normal-a`, `-c6b-*` | all passed; the autoloaded save's fight made one normal run 13 off |
| comparison, Save91 (final) | `s2-cadence-c7-*` | all passed, every shot within noise (table above) |
| guest+quick | `s2-cadence-f-guest-quick` | passed |
| native-vulkan+quick | `s2-cadence-f-native-vulkan-quick` | passed |
| native-d3d12-quick | `s2-cadence-f-native-d3d12-quick` | passed |
| native-vulkan+cadence-repeat | `s2-cadence-f-native-vulkan-cadence-repeat` | passed |
| native-vulkan+cadence-drop | `s2-cadence-f-native-vulkan-cadence-drop` | passed |

## Open items

- Repeat mode with a coc from Dragonsreach to Whiterun can still crash in the engine's list cull (DCLF kept scene lists over
  cells the load unloads); the load-epoch guard moves too late sometimes. Needs DCLF's lists (sidefx/phase 3) or an earlier
  signal that a load is pending.
- The DCLF load hold counts render frames, so the game time between a coc and world-ready differs by cadence (watch list,
  phase 7: count snapshots and load epochs).
- Guest has no cut-driven history reset (to confirm whether it should).
- Menu flags from the snapshot once the main menu and loading screens publish snapshots (phase 5).
- PCL `SimulationEnd` keyed by snapshot id (phase 7).
