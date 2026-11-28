# post-small: four small items from post-land and post-flicker

Lead `post-small`, branch `post/small` (rebased on `org_integration` 16f1d2167, for post-flicker's burst step), stage `s7-post-small`.
RTX 5080, Dev-Fast builds. Continued by post-small2 (stage `post-small`, merged `org_integration` 5f8108e70), which
extended sections 2-4.

## 1. Game-thread crash reading the render side's frame (fixed)

Cause:
- fx5 crashed in `FrameView::GetSunCandidates` from `IndirectDraws::WriteAccumulated` at `PrimaryCull::AtUpdateEntry`.
- `Main_Update_Entry` runs on the game thread, outside the game half (`FrameDriver::GameHalfStart`..`GameHalfEnd` is set only
  around `RenderScene`'s game half). So `t_gameHalf` was false there.
- `FrameView::RenderSide()` was `!t_gameHalf && renderFrame`. On the game thread it picked `renderFrame`, the render side's adopted
  frame, which the render thread replaces at its frame start. Copying its shared pointers raced that replacement.
- The tree list loop (`TreeAnimation::BeforeListLoop`, later in Main::Update) reads verdicts the same way.

Fix:
- `FrameView::GameSide()`: inside the game half, or on the game thread while the render thread runs. `RenderSide()` and
  `SceneStep()` use it. Those callers now read the game half's own members (`acceptedTables`, `frameSunCandidates`, ...), which only
  the game thread writes.
- With the render thread parked or absent, the game thread renders itself and still reads `renderFrame` (no race there).
- One info line logs the first such read in a session.

Other readers checked:
- Every `renderFrame` read goes through `RenderSide()`; nothing else reads it.
- snap7-thread2's probe listed the game-thread callers into `IndirectDraws` (WriteAccumulated, AccumulatedVerdictOf,
  DecideDrawFamilies, PublicationOf, NoteEventAdoption). They reach FrameView only through these accessors.
- `HeldByInstalled` and `NewestObjectGeometry` read `installed` directly; their callers run in the game half (PrimaryCull) or the
  scene lane. Not changed.

Decided by the agent, to confirm: no always-on detector like DV5-c for FrameView. The remaining race class would be worker jobs the
game half dispatches reading `renderFrame`; a thread check cannot tell those from the render half's own jobs without tagging every job.
The game thread itself can no longer read the render side's frame, so a detector for it would never fire.

## 2. Native D3D12 texture import stall at startup (diagnostics)

What the logs say:
- 1127-114515: the load hold released after 632 frames with "no progress", material 7552 waiting; the prewarm imported 475 of 555
  textures, 0 rejected. So 80 requests were never answered.
- It is not D3D12 only. A scan of every run since 11-25 (scratchpad `scan.py`) finds the same "no progress, material ~7550" with an
  incomplete prewarm in native Vulkan too (s3-lists-reg1, s7-frameend-a menu-ui / update-cap30 / cap240 / cadence-drop-cells,
  dir-s7rd native-interior, s7-devloss3-k, s7-post-perf-o, ...): about 15 of ~1500 runs since 11-27. native-d3d12-quick: 2 of 192
  since 11-25 (p2-post 1125-135219, and 1127-114515).
- In every case the first load's hold ends because a second load began ("a load began during the hold"), then the second hold stalls.

Where a request can wait (`GpuTextures::Request`):
1. at the base window's gate (`SceneGraph::PostTextureGate`, ready once the texture's window artifact reaches revision 1 and its
   upload completion is done);
2. in the import thread's queue;
3. in the import itself.
A rejection counts as answered, and 0 were rejected, so the 80 stopped in one of these. The code paths read did not show which.

Diagnostics added:
- `GpuTextures` keeps every unanswered request with its stage and time (`DescribeWaiting`).
- A hold released with "no progress" logs `[DCLF] load hold: texture requests not answered: ...`: counts by stage, then the oldest
  eight with file, source, stage age, base ready, and the window artifact's revision, readiness and upload completion
  (`SceneGraph::DescribeTextureWindow`).

post-small2: not reproduced. 7 hunt runs (land-cocs with Dragonsreach bursts: 4 native D3D12, 3 native Vulkan; each with the
startup's two loads and 10 coc loads) gave no "no progress" release, and every load prewarm imported every texture it asked for.
Neither did the sync-validation run or the 13 dragonsreach-bursts runs. At the earlier rate (about 1 in 100 runs) that is expected.
The diagnostics stay in, so the next hit names the stage.

## 3. White land (diagnostics)

- lc7 round 2 (exterior-2..2c) and lc11 round 2 (exterior-2..2f) show flat white land and river surfaces, and in lc11 round 2 the
  bridge's posts black, with every placement row right (audit: 0 land rows differ). lc11 round 1 (B5, rows wrong) is white in places
  too.
- White and black textures with right rows point at material bindings: a texture index that names another or a released descriptor,
  a material entry left from another material, or a stale streamed window.
- The shot audit now also checks every drawn material: entry key against the object's material now, each texture index against
  its import's index, the import not superseded, and a shell's import against its current window (and a graph window's current
  index). The next hit names which.

post-small2: not reproduced in 35 coc-outs (7 hunt runs, 5 rounds each), and the audit named nothing wrong at any shot: no
"another material's", no "index differs", no stale window, nothing unresolved. Some shots list a few "replaced" imports (up to 64,
up to 27 of them land). Those shots look right and the next shot lists none: streamed windows that upgraded after the material
resolved, with the old import still held. Not a defect.

Probable cause, decided by the agent, to confirm: snap7-water2's 0d1fe7ce6 ("rewrite every pair's rows after a clear"), merged
after lc7 and lc11 ran. A coc changes the tables generation, so `MainBuild::BeginRows` clears every material row. Before that fix a
resident region rewrote only the pairs its logs named, and a pair nothing named kept a zeroed row: every texture index 0, the null
descriptor. That reads black for the posts' diffuse, and flat white for land fits a zeroed land row. The audit reads the CPU-side
lookups, not the GPU rows, so it could not see this. All white-land shots predate the fix.

## 4. F2: broken repeat renders (native Vulkan and D3D12)

What it looks like: one rendered frame with most of the near scene missing. Pillars, floor, ceiling, walls and the player are not
drawn, the sky shows through, far structures are drawn in the right places, and the floor shows thin stripes of its texture.
Small cases miss a few wall pieces near a window (sky through holes at x 1240, y 400).

Facts:
- Every broken frame is a repeat render (the 2nd or 3rd render of its snapshot), never a first render. About 18 broken frames seen.
- Native Vulkan too, not D3D12 only: hunt-vk h4 (`1128-032701-post-small-hunt-vk-post-small-h4`, burst dr4 frame 80, third
  render of snapshot 13219, spike 114.5) is the same picture as post-flicker's D3D12 frame.
- Dragonsreach: about one per run with occlusion culling (f1, f2u: one each in 8x300 frames), several per run with
  `dclf.cull=frustum` (ffr2: 4 of 8 bursts, tr1: 3, tr3: 2, pf1: 3; ffr1: none). One Whiterun-exterior frustum run had none
  (its one spike was a darker first render, section 5).
- Missing in depth too: the sky (drawn in epoch 12, after DCLF's depth prepass) shows where the near objects should be, so those
  objects were not in the depth prepass either. The culling dropped them; it is not the colour pass's EQUAL test.
- Not occlusion: worse with frustum culling only (phase 2 retests only occlusion rejects, so with occlusion on it hides some).
- Not the portal member results: with `MainFrustumRejected` replaced by the plain frustum test (`Culled`), it still broke
  (pf1, pf2: 5 of 16 bursts).
- Not async compute: with the portal batch on the graphics queue it still broke (gq3).
- Not a skipped epoch: no run logged an `EpochCopiesBusyError` reaching the runtime ("ran more than its published copies").
- Sync validation (`VK_KHRONOS_VALIDATION_VALIDATE_SYNC`, sv1, 200 frames in Dragonsreach) found no hazard in the world frames
  (only startup clears and the reflections cube's layout). The layer stops after 10 reports of one VUID, so this is weak.

What fits: near objects culled and far ones kept is what a frustum test with a wrong camera gives (the eye moved forward, or a wrong
near plane). The cull reads its camera from the latch (`FillCullLatch`: ViewProj from `FrameCamera::Main()`, the eye from
`capture.eye`, folded by `LatchCullCamera`), and the draws themselves are in the right places. So on a repeat the depth phase 1
cull probably reads a latch camera that is not this render's: another run's latch region, or a region rewritten while an earlier
run still read it. `CommitMainPayload` already waits before the depth commit for renders that overlap (snap7-devloss); this may
be the same overlap class. Not proven, and that wait is outside this lead's area.

Tools:
- New config `dragonsreach-bursts` (eight 300-frame bursts in Dragonsreach). `native-vulkan+dragonsreach-bursts --set
  dclf.cull=frustum` breaks several times a run (about 2.5 minutes).
- A per-execution readback of the colour cull counters (to label each render's drawn and culled counts) was tried and dropped:
  `NoteCullLatch` saw the colour latch once, not once per execution, so it could not label renders that way.

Next step: log, per render, the latch camera the depth phase 1 reads (latch region, eye, first ViewProj row) against the frame
camera, ideally read back from the GPU's copy; or catch one with a consecutive-frame RenderDoc capture on a frustum hunt.

## 5. Other things seen

- One-frame darker first render in Whiterun's exterior (`1128-062224-post-small-f2hunt-ext-post-small-ext1`, burst f2-3 frame
  195, first render of snapshot 9907, spike 63): the whole frame is darker (lighting or exposure), the frames around it are right.
  Not F2. Recorded only.
- hunt-vk h6 lost the device (`1128-034027-post-small-hunt-vk-post-small-h6`, "fault invalid read at 0x50d6000",
  `Timeline::HostWait`) after the third coc back into Dragonsreach: snap7-devloss7b's area.

## Commits

- `fix(dclf): game thread reads the game half's frame` (item 1)
- `test(dclf): name unanswered texture requests` (diagnostics for items 2 and 3)
- `docs: post-small notes so far`
- `test(cs-test): dragonsreach-bursts hunts repeat breaks`

## Runs (post-small2, stage `post-small`)

| config | label | result |
| --- | --- | --- |
| hunt-d3d12 (scratch: land-cocs with 90-frame Dragonsreach bursts, native D3D12) | post-small-h1, h3, h5, h7 | passed; no stall, no white land |
| hunt-vk (the same, native Vulkan) | post-small-h2 | passed |
| hunt-vk | post-small-h4 | failed: F2 at dr4 frame 80 |
| hunt-vk | post-small-h6 | device lost (devloss7b's area) |
| syncval (scratch: Dragonsreach with sync validation) | post-small-sv1 | passed, no world hazards |
| dragonsreach-bursts (as scratch f2hunt), native Vulkan | post-small-f1, f2u (uncapped) | failed: one F2 each |
| the same, `dclf.cull=frustum` | post-small-ffr1, ffr2 | ffr1 passed (worst 19.8), ffr2 four F2 |
| the same, portal batch on graphics (experiment) | post-small-gq1..3 | gq3 one F2 |
| the same, cull counter trace (dropped) | post-small-tr1..3 | F2 in every run; the trace did not fire |
| the same, portal batch without `Revision` (experiment) | post-small-nk1, nk2 | hang at the held frame; reverted |
| the same, plain frustum instead of portal results (experiment) | post-small-pf1, pf2 | F2 in both |
| the same, Whiterun's exterior | post-small-ext1 | one darker first render, no F2 |

Skipped: guest+quick, native-vulkan+quick and native-d3d12-quick. Since the merge the branch changes no code, only a cs-test config
and these notes; every experiment was reverted.
