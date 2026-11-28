# snap7-devloss7: the render-thread device loss, round 7

Lead `devloss7b` (took over `devloss7`, which stopped with no commits and no notes), round `snap7`, branch `snap7/devloss7`
(merged `org_integration` 6d7449d08), stage `s7-devloss7`. Continues [snap7-devloss6.md](snap7-devloss6.md). Dev-Fast
throughout. RTX 5080.

Status: **not fixed; the interim wait stays, and now also covers the park handover.** Without the wait, window-resize-stress-taa
lost the device again in its second run, so the wait still guards the steady-state overlap. Most of the other losses this round
came at a load hold's release, where the old per-thread wait waited for nothing: the wait now also waits, after a park, for what
the game thread submitted while the render thread was parked. The read with the float index was caught once by a shader guard:
a CPU pipeline Z-prepass draw read its diffuse texture index from a material row that held a negative float. Several candidate
causes are ruled out (section 5).

## 1. Commits

| Repo | Branch | Commit | What |
|---|---|---|---|
| CS | snap7/devloss7 | ca690cd8e | test(cs-test): graph-switches config |
| CS | snap7/devloss7 | 0d985ae56 | fix(native): wait covers renders made while parked |
| CS | snap7/devloss7 | (this file) | docs: snap7-devloss7 notes |

No submodule change. The experiments were deleted again; their patches are in the scratchpad (`scratchpad/devloss7b/
experiments-cs.diff`, `experiments-org.diff`, `experiments-oms.diff`).

## 2. What the first lead left

- A shader guard (`DeviceGuardIndex`): any heap index of 1M or more is replaced by an index that faults at a page naming the load
  site and the index's top byte. Sites: buffer refs (non-uniform, uniform, RW), the material texture index, the pipeline texture
  index, landscape seams. I kept it for this round's experiments and added row checks (section 4).
- `FloatIndexScan` in ORG and OMS: logged uploaded words that match the fault floats. Discarded: in its one run it matched 600
  times, nearly all in vertex data (`Engine::TriShape`) and the menu scene latches. It cannot name the reader.
- Two cadence runs, no loss.
- The submodules were reset to their recorded commits.

## 3. What the losses look like now

- Rare: 2 losses in 30 world runs without the wait (cadence-drop-cells, cloud-shadows, graph-switches), plus 1 in 2
  window-resize-stress-taa runs. devloss6 lost 5 of 17.
- Both cadence losses (k-cad1, o-cad1) came 2.0-2.1 s after the Dragonsreach load hold settled, after the graph conditions had
  switched to the CPU pipeline (interior). 2 s is the driver's detection time, so the GPU stopped in the first frames the render
  thread rendered after taking rendering back from the game thread. The last checkpoints were CPU pipeline sites:
  `cs.dclf.z.direct` and `cs.dclf.direct`.
- The lab also had losses at a hold release **with** the wait: post-land's final native-interior run (0x5840000) and lc14,
  mvec2's cadence run. The wait was per thread (`t_lastRender`), and the game thread's hold renders never set it, so the render
  thread's first render after a hold waited for nothing.
- The resize loss without the wait (v-rs2): GPU-driven path, last checkpoint `cs.dclf.z.depth` (top of pipe `cs.dclf.hzb`), an
  invalid read whose index is the float 33249.0, a world coordinate (a placement row's translation or a bound's centre).

## 4. Experiments

All builds below are without the interim wait. Build letters:
- (a) the guards plus row checks: CPU pipeline row offsets off a row's start (sites 9, 10), an animated row index past 64K (11);
  GPU path row addresses outside the frame record's backings (7, 8) or off a row's start (12). `[DLV]` logs of each frame record
  change and of each commit that staged latched values (values with no latched copy in the revision's layout). A guard faults at
  `0xF4610000 + site << 24 + top byte << 12`, wrapped to 32 bits.
- (b) + `ORG_LATCH_FULL_BARRIER` (a full barrier before each latched copy pass) and a log naming the staged misses.
- (c) + two frame arena checks (an older run freeing a newer run's regions; a run asking for a region, or a latch block's cached
  region, after its lease retired) and `ORG_ARENA_POISON` (a retired run's regions filled with 0x7FBADBAD), `ORG_ARENA_HOLD_MS`
  (retired regions kept unused that long).
- (d) + guard site 13: the material row came from the animated rows copy.
- (e) + a retirement clock check: each `Retirement::SubmittedPoint()` subscribes a waiter with the GPU point behind it; reaching
  the point while that GPU work is not done is logged.
- (f) + a log of every CPU pipeline site run whose latched animated rows ref differs from the frame's backing.
- (g) + guard sites 14 and 15: a CPU pipeline material row offset at or past the installed payload's material rows.

| Labels | Build | Result |
|---|---|---|
| g-cad1..3 | (a) | invalid: a row check assigned to a const ref, so every DCLF Lighting VS failed to compile |
| h-cad1..4 | (a) | passed 3, failed 1 on the dragonsreach shot only (the known coc case) |
| i-rs1 | (b) | crashed in the menu: an engine animation job (`NiBlendAccumTransformInterpolator::CreateClone`, SkyrimSE+0xCDE0A2) |
| i-cs1..3 | (b) | passed 3 |
| j-cad1..3 | (c) | invalid: the experiment switches were not in `SwitchRegistry.cpp` |
| k-cad1 | (c), arena poison, hold 1.5 s | **device lost** at the Dragonsreach hold release; guard site 4 (below) |
| k-cs2 | (c), arena poison, hold 1.5 s | passed |
| l-cad1 | (d), view ref poison | passed |
| m-gs1 | (d), view ref poison, graph-switches | hang: the Cloud Shadows fatal (section 7) |
| n-cad1 | (d), view ref poison | passed |
| o-cad1 | (e), view ref poison | **device lost** at the Dragonsreach hold release; no fault record; no check fired |
| p-cad1..4 | (f), view ref poison | passed 4; no animated rows mismatch |
| q-gs1 | (f), graph-switches | passed (12 cocs) |
| q-gs2 | (f), graph-switches | hang: the Cloud Shadows fatal again |
| r-cad1 | (g) | passed |
| r-cad2 | (g) | hang: a tap error fatal ("uncovered epoch") while other leads' builds took 12.5 of 16 CPUs |
| v-rs1 | wait deleted, no experiments | passed |
| v-rs2 | wait deleted, no experiments | **device lost** at the fourth 1920x1080 resize (section 3) |

## 5. Findings

- **k-cad1's fault is the float index caught by a guard.** Invalid read at 0xF86CE000: guard site 4 (`DCLFMaterialTextureIndex`)
  with top byte 0xBE, so the material row's texture index word held a negative float (-0.125 to -0.5). BasicRHI's decoder prints
  0x3E40BE00 because it assumes a plain float index; the guard index is 0x0E40BE00. The draw was a CPU pipeline Z-prepass draw
  (a pulled build whose vertex stage reads the alpha-tested diffuse index from the material row). The row checks did not fire:
  the row offset was a whole row and the animated row index was small. So the material row ref named a buffer whose row at that
  offset was not a material row (negative floats look like vertex data).
- **The frame record changes every frame.** The animated rows' backing (record backing 3) cycles through four buffers, one per
  frame, 5-17 texture registers change every frame (the frame values' buffers), and the material and shadow rows' backings swap
  every 15-40 frames. So "a render reads the next render's record" would misread every frame, not only after an attach.
- **Each load hold's release stages latched values.** The render thread's first depth commits after a hold stage 333-405, then
  12-22 values a commit for 3-5 commits (108-956 around a coc without a loading menu). They are tree LOD shape and instance rows
  (`cs.dclf.tree-lod-shapes`, `cs.dclf.tree-lod-instances`) that the revision's latched layout had no copy for, so they go
  through the ticket's upload list. Not shown to be related.
- **The fault floats of earlier rounds match Tree Wind's block.** The tree LOD draw row (`TreeInstances::DrawRow`) carries
  `TreeWindCB` at byte 64: wind speed 0-1 (0.51-0.62 with the weather), amplitude 80 (default) or 50 (a preset), and several
  0.6 settings. That made a frame arena region read after its run retired a good suspect (regions are bump-allocated in a fixed
  order, so the same offset holds another block after an attach). Ruled out below.

Ruled out this round (each without the wait):
- A frame arena region read after its run's lease retired, or freed by an older run: k-cad1 lost with retired regions held
  1.5 s and filled with 0x7FBADBAD, and neither arena check fired in any run.
- DCLF's retirement clock passing a point before its GPU work is done: the check never fired (o-cad1 lost with it on).
- A CPU pipeline site binding another animated rows copy than the frame's: never seen (6 runs, including o-cad1's build).
- A CPU pipeline material row offset at or past the installed payload's material rows (sites 14/15): never fired.
- From earlier rounds, still standing: stale released view refs (devloss6's poison runs lost with float indices and no POISON
  line), graph teardown, CPU slot lifetimes, cross-epoch barriers.

## 6. The change: the wait covers the park handover

`Native::RenderThread::WaitForLastRender()` waited for the render thread's own last render (thread_local `t_lastRender`). After a
park (Main::DrawFrame, a loading screen, a movie), the game thread renders, and the render thread's first render after the park
waited for nothing: the losses at a hold's release with the wait came from there. Now `OnPublished`, which releases the park on
the game thread, stores the GPU point of everything submitted so far (`Retirement::SubmittedGpuPoint()`, taken on the thread that
submitted it), and the render thread takes it as its last render when it leaves the park. Its first depth commit waits for it as
for its own last render. Guest and the game thread's own renders still never wait.

Decided by the agent, to confirm (DV7-a): extend the interim wait to the handover instead of deleting it. Deleting it brought the
resize loss back (v-rs2), and the hold-release losses happen with the old wait too, so the old wait was neither removable nor
complete.

## 7. The director's samples: post-small's h6 loss and F2

- post-small-h6 (1128-034027, post-small's build with the old wait): invalid read at 0x50D6000, a float index 0.542 (and 0.4237 in
  the other records), last checkpoint `cs.dclf.main-opaque` (GPU-driven colour), right after a `place` teleport step, with no load
  hold. So the float-index read also happens in steady play with the old wait on, on the GPU-driven path, not only at a hold's
  release. The handover change in section 6 does not cover it.
- F2 (post-small2; post-f2 owns it): repeat renders cull most of the near scene, suspected to read the cull camera from another
  run's latch region or one rewritten under it. My arena checks cover leased runs only: a region read after its lease retired
  (poisoned and held 1.5 s in k-cad1) or freed by an older run. Neither fired in any run. Repeats submit published copies, whose
  regions are pinned per lease index (stable runs: `LatchBlock::StableRegion`, `CopyStableRun` from the staging run,
  `ZeroStableRun` on a live copy's reuse); none of that was checked here. If F2 is a stable region written while a submission
  of the same copy still reads it, the same thing could feed the device loss on the GPU-driven path, which reads its frame record
  and BuildDraws latch from those regions. My change does not touch repeats, so it does not explain F2.

## 8. Other faults seen (not this item)

- **Cloud Shadows fatal at a handover** (2 of 2 graph-switches runs that reached their sixth coc): "Cloud Shadows: a run's layer
  operations read more than 32 faces" (`ORGCloudShadows::QueueLayerOp`, ORGCloudShadows.cpp:678). Once on the game thread right
  after a hold settled, once on the render thread (reflection faces -> `Sky::SubmitFace` -> `CloudShadows::PropagateToCompletion`
  -> `QueueLayerOp`). Then the run hangs in the fatal's message box. graph-switches reproduces it.
- A tap error fatal, "an epoch a fragment source must cover was left to the engine while the source draws (uncovered epoch)",
  once while other builds saturated the CPUs (r-cad2).
- An engine crash in the main menu, an animation job in `NiBlendAccumTransformInterpolator::CreateClone` (i-rs1).

## 9. Runs of the final branch (0d985ae56)

| Label | Config | Result |
|---|---|---|
| f-burst | native-vulkan + a temporary config: 180-frame bursts from `held-frame` into Dragonsreach and back out | passed; worst spike 0.66 / 0.8, 0 flickering tiles |
| f-cad1..4 | native-vulkan+cadence-drop-cells | passed 4 |
| f-rs1..2 | native-vulkan+window-resize-stress-taa | passed 2 |
| f-cs1 | native-vulkan+cloud-shadows | passed |
| f-int | native-interior | passed (its burst: spike 0.36, 0 tiles) |
| f-quick | native-vulkan+quick | passed |
| f-d3d12 | native-d3d12-quick | passed |

Skipped: guest+quick and guest resize (the change is in the native render thread only; guest never parks it), land-cocs and
native-tour (covered by cadence and interior for this change), and the perf configs: the extra wait happens once per park
release (a load or a menu), never in steady play, so frame rates cannot move. The flicker question: the frames right after a
hold's release show no flicker in this build (f-burst). I did not see the user's one-frame flicker after loads; post-corrupt owns
it. The device loss itself is not fixed, so I cannot say a fix changes them.

## 10. Open items

- The float index read. Next steps:
  1. k-cad1 points at the CPU pipeline's material row refs: the list's base (`set.materialRowsRef`, pushed per list) or the
     animated rows copy (`DCLFAnimatedRows`). Site 13 (in the scratchpad patch) tells the two apart; it has not caught a loss yet.
     A loss with `BASICRHI_VK_VIEW_REF_POISON=1` and a fault record would say whether the ref's slot was released.
  2. v-rs2 is the GPU path: an index read from placement-like data (33249.0). The z-prepass vertex stage reads its sequence's
     rows and buffer refs; a sequence read from the wrong buffer would give that.
  3. graph-switches gives about twelve handovers a run, but needs the Cloud Shadows fatal fixed first.
- The cadence dragonsreach shot still lands outdoors in some runs (the coc does not take).
