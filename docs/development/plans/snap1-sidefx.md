# snap1-sidefx: the render half's game-visible side effects, produced on the game thread

Lead: `sidefx`, branch `snap1/sidefx` (base `org_integration` cce610019). Phase 3 work of
[render-thread-snapshots.md](render-thread-snapshots.md), rows of [render-side-effects.md](render-side-effects.md),
decisions 9-14 of [render-thread-decisions.md](render-thread-decisions.md).

## What was built

`src/Snapshot/SideEffects.{h,cpp}` (new): the game-side producer for native mode. Guest and D3D11 install nothing and are
unchanged. The publication hook (`Publication.cpp`, `MainUpdate_DrawFrame`) calls it around the render half:

```
Capture()                       // the snapshot, as before
SideEffects::AfterCapture()     // S17 decals, S5 fade enable count, S6 actor pre-cull
{ RenderHalfScope; Main::DrawFrame x renders }   // 0, 1 or N times (cadence mode)
SideEffects::AfterRenderHalf()  // S4 render-unsafe bit, S2 frame counter, S1 stages 5-9
```

The cadence mode's old `SignalRenderStages` (skipped updates only) is gone: native now signals the stages in one place for
every update, rendered or skipped. In native the engine's own writes are patched out at install (each patch checks the
engine bytes first and is fatal on a mismatch):

| Row | CS produces (game thread) | Engine write turned off in native (SE / AE) |
|---|---|---|
| S1 stages 5-9 | `SetJobTaskEvent(5..9)` after the render half | SetJobTaskEvent detoured (68165 / 69514): calls for 5-9 from the main thread inside the render half are skipped |
| S2 `gFrameCounter` | `++` once per update after the render half | Renderer::End's `inc` (75461 +0x50 / 77246 +0xBB) NOPed |
| S4 render-unsafe bit | the engine's helper (38080 / 39034) with 0, after the render half | Main::Draw's call (35560 +0xA01 / 36559 +0xA0F) thunked to nothing |
| S5 fade enable | AE 107143's count: 20 world updates with the flag clear, then both flags (513141/390865, 513142/390866) on, counter 527629 / 414558 | the count's `jne` at 100425 / 107143 +0x3D made a `jmp` |
| S6 actor pre-cull | `kNotVisible`, bit 12 and Actor +0x274 (SE) / +0x27C (AE) from a SIMD frustum test (`org::cpu` sphere cull, DCLF's `ShadowCameras::FrustumPlanes` on the snapshot's main camera); also the culler's camera and `NiCullingProcess::SetFrustum` (69699 / 71081), which CS's own capture reads | the high actor culler call in Main::Draw's camera setup (35562 +0x148 / 36561 +0x11B, target 39263 / 40336) thunked: it runs only its first, initialising call |
| S17 decal tests | every state-3 decal in BGSDecalManager +0x60 set to state 5 with count +0x40 = 1 (what a visible answer leads to), then +0x60 cleared | nothing to patch: DrawFrame's loop finds +0x60 empty, so Main::DecalTest (35563 / 36562) no longer runs in native |

Other changes:

- **Render-frame keys** (S2 follow-up). DCLF's "once a frame" keys read `gFrameCounter`, which now moves once per game update.
  They key on the render frame instead (`RenderGraph::FrameClock::BeginFrame`): `Family::GameFrame` became `RenderFrame`
  (family batches, kept owners, reflection faces, sky slots) and MasterParticles' held-frame count.
- **Fades on skipped and repeated renders** (S8). `FadeState::SampleFrame` steps a visible root's fade by the engine's
  per-update step times the updates the adopted snapshot stands for (`events.gameUpdates`), and by 0 on a repeated render;
  `stepMax` scales the same way. Guest adopts one update per render, so its step is unchanged. The animation job's fades
  already counted each update (`AnimatedUpdate`).
- `tools/cs-test/configs/cadence-drop-cells.toml`: drop:3 through three cell loads (Whiterun, Dragonsreach, Riverwood).
- A log line every 600 game updates in native: `[Snapshot] side effects over N updates (M skipped): ...` with each row's
  counters (stages signalled, engine signals skipped, frame counter, mask before and after the clear, decals accepted and
  stuck, fade enables, actor pre-cull counts). Fade enables also log their own line.

## Ghidra findings (AE 1.6.1170 unless marked)

- **Stage events are manual-reset Win32 events** (`CreateEventA(NULL, TRUE, FALSE)` in AE 0x140cfd180, called from SetupJobLists),
  reset all at once at Main::Update's entry (FUN_140cfd280, the call DCLF's `Main_Update_Entry` wraps). Setting one twice is
  harmless. Manager 1 (10 events) is the main thread's; the jobs use manager 0 (26 events).
- **Who waits on 5-9** (the frame job table at 0x142011920): "Wait For Shadow Culling" (5), "JobListEnd" FUN_1406d2e80
  (6, or 8 in menu mode), "JobListEnd" 0x1406d3640 (7), "Non render-safe AI" AE 40522 (updates the player's third-person 3D,
  waits 7, then first person), "JobListEnd" AE 39094 (8 then 9). After them run Cell animations, Ragdoll post physics,
  Garbage, Particles, Havok reset, the player 3D update, Face morphing, Sky, Shared particles, Update grass.
- **No deadlock from signalling late**: inside the render half the only wait is DrawFrame's `WaitForJobTask(0, 2)`, set by
  Job_UI, which sits before every 5-9 wait in the job table. Main::Update's frame-job wait (AE 35409) is after DrawFrame.
- **Frozen-menu frames** set 6, 7, 8, 9 and never 5 (no shadow pass). CS sets 5 too: the 5 waiter's list is not scheduled in
  menu mode (it would hang vanilla), and events reset each update, so the extra set changes nothing.
- **Frame counter**: Renderer::End's `inc dword [0x14328cc6c]` is its only writer (26 xrefs, one write). It also moved in the
  loading loop and Bink ticks; in native it now counts game updates only (starts at 0 in the main menu).
- **Fade enable count** (AE 107143 = SE 100425, same layout): `if (!flag && ++count > 20) { flag = flag2 = 1; count = 0; }`.
- **HighActorCuller::Operate** (AE 40335, SE 39262): root = TESObjectREFR::Get3D (AE 19735); not a fade node: set
  `kNotVisible`, bits 0. Else `BSFadeNodeCuller::Process1` (AE 0x1414e9a50: sets bit 12, six planes, outside when
  `n.c - d <= -r`, sets or clears bit 20), bits = 2 | 4 if `currentFade >= 1e-5`; if not `kNotVisible` and faded in, the
  occluder test FUN_1414a8ba0 sets bit 1 and, when occluded, bit 20 and clears 2. The bits go to Actor +0x27C (AE), +0x274
  (SE). The list walk skips references with form flag bit 21. Gate: `bPreCullActors:General` byte 508891 / 380927.
- **Decal queue** (AE 15220): state 0 with +0x30 set becomes 3 and joins +0x60; state 4 polls the query (AE 30117) and stays
  4 while the query slot +0x38 is null (returns 2). So a game-side answer must go 3 -> 5 directly: state 5 calls the create
  vfunc +0x128 and the finish (FUN_1404aaa40) in the same pass. +0x40 (count) non-zero is what a visible poll leaves.
- **`ImageSpaceManager+0xE0` (sunlightScale) is written game-side**: Main::Update calls AE 105687 (copies the current
  TESImageSpace's base data, the override at +0xB0 or the weather's at +0xA8, into ISM+0xC8 with AE 107114; +0xE0 is
  hdr.sunlightScale at +0x18 of it), the IMOD instances blend into it, and AE 105690 (AE 107115) finishes it. Sky::Update
  sets +0xA8 (AE 0x141482a80). Nothing in the render half writes it, so the stealth light level's sun term needs no producer.
  SE ids of 105687/105690 not resolved.

- **The culler's frustum is a side effect CS itself reads.** AE 40336 stores the world camera at culler +0x28 and calls
  SetFrustum on its culling process (+0x10); `Util::GetCameraData` (`Utils/Game.cpp:198`, the core block's cameraNear/Far)
  and `Globals.cpp:236` read near and far at culler +0x40/+0x44. Skipping the whole call left them 0: the first native runs
  lost the terrain (gray void, `s1-sidefx-f-native-vulkan-quick`). CS now does that setup in AfterCapture too.

## Proposed decisions (agent, to confirm)

- **D-a. S1 and S4 sit after the render half in phases 0-6, right after the capture in phase 7.** The render half still reads
  the live scene graph after the capture today, so signalling 5-9 or clearing the bit before it would let Garbage, Particles,
  Sky::Update, the grass update and the player 3D update run during the engine's culls. After it they are safe. Phase 7
  (the render half reads only snapshots) moves both lines to right after `Capture()`. Cost: jobs that vanilla overlapped
  with the draw now start after it (see the timing check below).
- **D-b. S2: one tick per game update in native, after the render half** (where Renderer::End had it, so the game's stamps
  and comparisons keep vanilla's one-update distance). Render code that used the counter as a per-render key reads the
  render frame instead of a snapshot copy, since a repeated render must still start a new frame. PostChain's SSR and water
  parity (`PostChain.cpp:380,430`) keeps the live counter because the engine's ImageSpace reads the same global in the same
  render; in repeat mode the ping-pong then does not flip between repeats (phase 2 watch list). Grass reads
  `graphicsState->frameCount` too (`GrassCollision.cpp:299,306`, `GrassBucketCulling.cpp:198`, `GrassOptimizations.cpp:532,559`,
  post lead's files): in repeat mode those keys now hold over the repeats, which reuses the update's results.
- **D-c. S6 occlusion is not repeated.** CS's pre-cull is the frustum and fade threshold only; a root in view and faded in
  counts as occluder-tested and not occluded (bit 1 set, bit 2 kept). Actors behind occlusion planes now count as visible:
  full animation rate (cost), head tracking and idles as if seen. The occluder test can follow from PortalViews' programs,
  but only the unbound-space program is occluder-only; a room program adds portal culling, which vanilla's pre-cull does
  not do and which would gate AE 41381's per-process update on more actors.
- **D-d. S6 runs only for world snapshots** (`FrameKind::World`), as Main::Draw ran the culler only when it drew the world.
- **D-e. S5 counts world snapshots**, one per update, not render frames.
- **D-f. S17 decals go straight to state 5.** State 4 would need a query object; 3 -> 5 with count 1 is what the visible
  answer produces. The `DecalOcclusion` feature's graph path is now guest-only in practice.
- **D-g. Equality on a frustum plane**: the SIMD kernel keeps a sphere exactly on a plane (`>= -r`), Process1 culls it
  (`<= -r`). Measure-zero difference, kept for the shared kernel.

## S7/S8 for roots DCLF does not stand in for (decision 13): design, not built

Today in native the engine's culls still run for roots outside DCLF's tables, so their `kAccumulated` and fade writes still
come from the engine. Extending DCLF's cull to them means: (1) list every scene root under the object root, cell roots and
the actor roots in DCLF's root tables with a "listed only" bit (no draws), so the CPU pipeline's cull (`Direct/Cull.cpp`,
lane bit `kLaneCullOnly`) and the GPU visibility events produce their verdicts; (2) `IndirectDraws::WriteAccumulated`
already writes verdicts for listed roots, so the write-back needs no change; (3) the fade write-back
(`FadeWriteBack.cpp`) must cover BSFadeNode roots that are neither leaf nor tree (actors, plain fade roots), with +0x13C
and +0x140 written too, since AE 19384 and the pre-cull read them; (4) then the engine's main cull can be skipped in native
(its Process1 is already DCLF's stand-in for listed roots). Step (3) is the risky one: actor roots' fades are stepped today
by the engine's main cull through BSFadeNode::OnVisible. The pre-cull reads `currentFade` (S6), so they must stay right.

## S12/S13 stealth light lists (decision 10): design, not built

What AE 39946 (`AIProcess::CalculateLightValue`) reads, verified: for the player it loops over the shader state's
ShadowSceneNodes (count at +0x158, AE 106361), keeps those the player can see (AE 42744), and sums with AE 106362 over each
node's active and shadow lights plus the ambient and sun terms; the sun term is scaled by ISM+0xE0 (game-side, above). For
an NPC it sums AE 107776 over its geometry's light data (`(process+8)+0x178 -> +0x70`).

Design: leave the engine's lists alone (the renderer and LLF still use them in phases 0-6) and give the light level its own
membership, computed on the game thread:

1. Membership for a scene node is what `ProcessQueuedLights` (AE 106335) would leave: `activeLights` and
   `activeShadowLights` (+0x130/+0x148), plus `lightQueueAdd` (+0x160), minus `lightQueueRemove` (+0x178), minus orphans
   (lights whose NiLight has no parent). Read on the game thread at the light-level call, without applying it.
2. Detour AE 106362 for calls from AE 39946 (game thread) to sum over that membership with the engine's own luminance
   (AE 108292), the same mask and outputs.
3. For NPCs, detour AE 107776 to build the light list from the same membership: each light whose radius sphere reaches the
   actor's geometry bound, as the attach walk (AE 106342, 106401) tests it, then the same sum.
4. Once the render half stops (phase 7), CS also drains the two queues into the active lists on the game thread (no attach
   walk), so they do not grow.

Not reproducible in drop mode: the render half runs every Nth update, so the queues are applied every Nth update and the
light level lags by at most N-1 updates.

## S15 save screenshot and S16 flow maps

Neither reproduces in drop mode, by reading: the render half still runs one update in N. The save pump (AE 35772) re-queues a
bit-29 save until the screenshot is ready, and the next rendered update makes it (at most N-1 updates late). The flow-map
ring (256 slots) is drained by Main::RenderWaterEffects (AE 36560) on each rendered update, far faster than cells load, so
the 128-entry pool never runs dry. `cadence-drop-cells` walks three cell loads in drop mode to confirm the loader does not
spin. Both become real only when the render half never runs on the game thread (phase 7). The fix then follows decision 15:
the screenshot request becomes a snapshot event the render side fulfils, and CS drains the flow-map ring game-side (frees
the pool entries) and hands the tile copies to the render side.

## Runs

All on stage `s1-sidefx` (Dev-Fast), the autoloaded river save unless noted.

| Config | Label | Result |
|---|---|---|
| native-vulkan+cadence-drop | `s1-sidefx-drop1` | passed (first build; terrain missing, see the culler frustum above) |
| native-vulkan+quick, parity build (engine culler run after CS, verdicts compared) | `s1-sidefx-parity1` | passed; **5,009 actor verdicts compared, 0 differ** in `kNotVisible` or the four bits (no occluder hit at this save) |
| native-vulkan+cadence-drop-cells | `s1-sidefx-cells1` | passed (terrain missing, as above) |
| guest+quick, native-vulkan+quick, cadence-drop, cadence-repeat, native-d3d12-quick | `s1-sidefx-f-*` | all passed; native shots without terrain, which led to the culler frustum fix |
| native-vulkan+quick | `s1-sidefx-g-native-vulkan-quick`, `s1-sidefx-g2-nvk` | passed; terrain back. The first shot is blurred by a combat hit; the second is clean and matches the baselines |
| native-vulkan+cadence-drop | `s1-sidefx-g-native-vulkan-cadence-drop` | passed, shots normal |
| guest+quick | `s1-sidefx-h-guest-quick` | passed, unchanged (no side-effect line: nothing installs in guest) |
| native-vulkan+cadence-repeat | `s1-sidefx-h-native-vulkan-cadence-repeat` | passed, shots normal |
| native-d3d12-quick | `s1-sidefx-h-native-d3d12-quick` | passed, shot normal |
| native-vulkan+cadence-drop-cells | `s1-sidefx-h-cadence-drop-cells` | passed; Whiterun, Dragonsreach, Riverwood shots normal |

What the drop-mode logs show (`s1-sidefx-h-cadence-drop-cells`, 400 of every 600 updates skipped): stages 5-9 signalled
600 times by CS per 600 updates, 1,200 engine signals skipped per 200 renders; the frame counter equals the update count
(600, 1200, ... 3600); the render-unsafe mask goes 0x1 to 0x0 at each clear; the fade enable flags come back on after each
of the three loads (snapshots 466, 1852, 3163); the actor pre-cull follows the cells (38-54 high actors near the river save,
10 in Dragonsreach, all in view there). No decal was queued in these scenes (no combat hit landed a decal), so S17 is proven
only by the queue staying empty (0 stuck). No hang, no cell-loader spin.

Not measured: the cost of starting the post-draw jobs after the render half (D-a). The quick runs are vsync-capped at 60 fps;
a perf capture (`native-perf-taa`, uncapped) would show it.

## Open items

- S7/S8 extension and S12/S13 membership (designs above).
- The occluder test for S6 (D-c).
- Phase 7: move S1 and S4 to right after the capture (D-a).
- Measure D-a's cost uncapped.
- A drop-mode run with combat, to see S17's accepted decals placed (the counter is in the log line).
- Snapshot's `cameraNear/Far` could come from the snapshot camera instead of the culler's frustum (core lead's `Utils/Game.cpp:198`); CS produces that frustum now, so nothing is broken.
