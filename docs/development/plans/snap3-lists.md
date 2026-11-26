# snap3-lists: the main camera's scene lists (phase 3, milestone 2)

Lead: `lists`, branch `snap3/lists` (base `org_integration` 5f772e4f1), stage `s3-lists`. Phase 3 of
[render-thread-snapshots.md](render-thread-snapshots.md); decisions 8, 12-13, 33-48, 53, 66-75 of
[render-thread-decisions.md](render-thread-decisions.md); starting point `snap3-scene.md` "Open items".

## What was done

Native draws nothing from the passes the engine registers for the main camera any more, and the engine's registration jobs
for the Z-prepass and main accumulators are not run in native. The main camera's list culls (BuildSceneLists, the list
accumulation jobs, FirstPersonCulling) still run: see "Not done".

| Piece | State | Commit |
|---|---|---|
| Sky main view from the sky root | done (observe, switch, scaffolding removed) | 7dc6af78b |
| Water batches from the tracked water shapes | done (observe, switch, scaffolding removed) | 4c0bb7403, f02119a12 |
| No main registration jobs in native | done | cb6199c74 |
| DCLF's own cull in place of the list culls | not started (blockers below) | |
| CS stages from CS's own points | not started | |
| S8 for the decision 46 roots | waits for the list cull to stop | |
| Decision 53 crash, cadence-drop-cells coc hang | the list culls still run, so not fixed (runs below) | |

### Sky (`Features/Sky/SkyNative.cpp`, `SkyHooks.cpp`)

At the main pass's sky batch, native builds the batch and the cloud layers from the sky root with the adopted snapshot's main
camera (`Native::BuildMain`), using the face path's cull (`RootPasses`, shared with `BuildFace`) and draw loop
(`DrawRootPasses`). Sky-object effects are left out (they are not the sky family's; the engine's draws of them are dropped in
native anyway). The run holds the cloud layers, so the engine's group 13 call finds them detached (`Pending::drawnClouds`).
Guest, D3D11 and the faces are unchanged.

Parity (temporary, removed): the registered pass list (batch by technique, sub-group and list order, then group 13's sky
passes) against the computed one, plus the eye, the frustum and the cloud group's shadow-state modes.

| Run | Result |
|---|---|
| `s3-lists-sky-par1` | batch identical; two cloud layers in the engine's list and not the port's (`02_CDUpper_04_E`, `13_CDHorizon_3_E`) |
| `s3-lists-sky-par2` (three cull variants) | only "no bound tests" matches: the main camera's cull registers every sky object that is not hidden. Eye and frustum identical |
| `s3-lists-sky-sw2` (switch, parity kept) | 0 of 60 batches differ; cloud group modes predicted 59 of 59 |

The first switch build (`s3-lists-sky-sw1`) hung at load on a fatal: the engine drew group 13's still-registered cloud layers
on D3D11 (Cloud Shadows' tap check). Fixed by detaching them at the group's call.

### Water (`Features/Water/WaterNative.cpp`, `WaterBatches.cpp`, `Scene/WaterSceneCensus.cpp`)

At the main camera's water mask, group 10 and water batches, native computes the passes from the water shapes Core tracks
(`WaterScene::TrackedShapes`, the census's map) that the water epochs do not draw this frame (`PassCapture::CurrentSet()`
without the main phase: refused shapes such as the wading mesh, and plain water not claimed yet). Each shape must hang under
the world root with no hidden ancestor and meet the main frustum; `Port::DeriveRenderPasses` gives its passes (the stencil pass
from the property's cache at +0xC0 when set), filed into the batch whose technique range holds them, hint 13 into group 10,
sorted by technique then sub-group. Light passes use a light count of 0 (none was registered in the parity runs).
`SceneHasPasses` keeps the old early-out, so the batch's bound headers are read only when there is water to draw.

| Run | Result |
|---|---|
| `s3-lists-water-par1` (refused shapes only) | wrong set: plain water not claimed yet is also the batch's |
| `s3-lists-water-par2` (unclaimed shapes) | 0 differences outside load holds; port-only entries were detached shapes of the old cell |
| `s3-lists-water-sw1` (switch, root test, parity kept) | 4,800 batches: 142 differ, all inside the two load holds (shapes the walk had not tracked yet); port-only none |

Decided by the agent, to confirm: the frames inside a load hold (loading menu still up) may miss water the walk has not
tracked yet. Shape order inside one technique is not modelled (no parity run saw two passes of one technique change order).

### Registration jobs (`Features/Shadows/Native/NativeCasters.cpp`)

`RenderShadowMaps` no longer queues `DoSceneListAccumRegisterJob` for the Z-prepass and main accumulators, waits on their
Finish, or prepares the Z-prepass groups (AE 106613). Nothing native draws reads those passes: latched families (blood,
precipitation, shadow mask, menu scenes) use their own accumulators, the forward and water epochs only compare accumulator
pointers, and PassCapture's drain is read by nobody. RenderWorld's batch calls still run (on empty accumulators), so every CS
stage point at them still fires. The main accumulator keeps the camera and flag RenderShadowMaps set before RenderWorld.

## Detector (native Vulkan quick, river save, per world frame)

| Site | Before (`s3-lists-sky-par1`) | After (`s3-lists-reg2-nvk`) |
|---|---|---|
| `JobList::Finish` (in Main::RenderShadowMaps) | 1.01 | 0 |
| every other render-role site | unchanged | unchanged |
| engine render calls | 65.9 | 64.9 |
| live read `PassCapture.cpp:235` (player, per registered pass) | 274.9 | 4.8 |
| live read `WaterNative.cpp` batch inputs | 27.1 | 0.3-8 (only batches with water to draw) |
| live engine reads, total | 1,260 | 796-842 |

The sky and water ports move no call site: the engine still calls RenderWorld's batch functions (Main::Draw is the engine's
until phase 7, decision 70).

## Not done, and why

**DCLF's cull in place of the list culls** (BuildSceneLists, the list accumulation jobs in `SelectCasters`,
FirstPersonCulling). Stopping them in native is mechanically small (PrimaryCull's `PrepareFrame`/`AfterListJobs` and the
stand-in observers would be called without the jobs), but the list culls are also where the engine runs OnVisible for every
entry PrimaryCull does not stand in for, and native would lose those writes:

- *Rejected entries* (an OnVisible kind the cut cannot stand for: particle systems, LOD and face nodes, among others; the
  "culls off" report line lists them by kind) get the engine's Process1 whole every frame. Their OnVisible side effects
  (fade steps, LOD selection, particle and facegen updates) must be ported per kind before the cull stops.
- *Mixed* entries (an engine-drawn part) and entries *not admitted yet* have their fades stepped by the engine; native would
  have to treat mixed as stood in (no engine-drawn part is drawn in native anyway) and step fades from admission on.
- *Decision 46 roots*: S8 is to be produced on the game thread when the cull stops (BSFadeNode::OnVisible's step, AE 105486,
  with the +0x13C stamp, for roots CS marks visible; AE 19384 steps the others itself, read in Ghidra).

**CS stages from CS's own points.** The stage points are RenderWorld's batch calls (Deferred's opaque start and blended
decals, the sky batch, the water batches, the forward blended draw, the sun query). Driving them from CS means replacing
Main::RenderWorld in native, which also calls RenderSSAO and the copy (ImageSpace, the `isrest` lead's) and the screen
splatter's cull and submit (milestone 3). Left for the next lead, together with phase 7's Main::Draw.

## Runs (stage `s3-lists`, Dev-Fast)

Development runs are in the tables above. Final build (cb6199c74):

| Config | Label | Result |
|---|---|---|
| guest+quick | `s3-lists-f-guest-quick` | passed (1,207 live reads; guest unchanged) |
| native-vulkan+quick, detector | `s3-lists-f-nvk-quick` | passed (764 live reads; 66.4 render calls, the pre/post resolve counts vary 1.65-1.72 a frame with the scene) |
| native-d3d12-quick | `s3-lists-f-d3d12-quick` | passed |
| native-vulkan+cadence-drop | `s3-lists-f-drop` | passed |
| native-vulkan+cadence-drop-cells | `s3-lists-f-drop-cells1`, `-cells2` | both passed, no hang; sky and water normal in the Riverwood shot |
| native-vulkan+cadence-repeat | `s3-lists-f-repeat` | passed (decision 53's crash did not show; the list culls still run, so it is not fixed) |
| native-vulkan+shadows-blend | `s3-lists-f-shadows-blend` | passed |
| native-interior, detector | `s3-lists-f-interior` | passed (51 render calls) |
| native-vulkan+stealth-lights | `s3-lists-f-stealth` | passed |
| native-tour | `s3-lists-f-tour` | passed |
| native-river (water) | `s3-lists-reg2-river` | passed, water as before |
| cadence.py, native-vulkan+cadence-still | `s3-lists-f-cad-*` | all four passed; interior within noise (repeat 2.6, drop 3.0); exterior 17.0 / 13.3 against a floor of 2.2, "outside noise" as in snap3-scene (16.0 / 15.3) and phase 2 (wind and pose) |

## Decisions (agent, to confirm)

- **LI-a.** The main camera's sky cull registers every sky object that is not hidden, with no bound test (parity above).
- **LI-b.** Native's main water batches come from the tracked water shapes the epochs do not draw, culled with the snapshot's
  main camera. Water the walk has not tracked yet is missing until it is (seen only inside load holds).
- **LI-c.** The water shape list is the water census's map (`TrackedShapes`), which now serves drawing as well as the report.
- **LI-d.** The main registration jobs and the Z-prepass groups' prepare are not run in native; the list culls still are.
- **LI-e.** The sky root and the water shapes are read on the render thread from the scene graph, as the face path already
  did. Phase 6/7 moves them into the snapshot (DCLF's scene tables already hold the water shapes' data).

## Open items

- The list culls (above), then S8 for the decision 46 roots, the decision 53 crash and the coc hang.
- CS-driven stage points (RenderWorld in native).
- SE is untested (no new address; the sky and water ports read the same fields on both).
