# post-small: four small items from post-land and post-flicker

Lead `post-small`, branch `post/small` (rebased on `org_integration` 16f1d2167, for post-flicker's burst step), stage `s7-post-small`.
RTX 5080, Dev-Fast builds.

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

## 3. White land (diagnostics)

- lc7 round 2 (exterior-2..2c) and lc11 round 2 (exterior-2..2f) show flat white land and river surfaces, and in lc11 round 2 the
  bridge's posts black, with every placement row right (audit: 0 land rows differ). lc11 round 1 (B5, rows wrong) is white in places
  too.
- White and black textures with right rows point at material bindings: a texture index that names another or a released descriptor,
  a material entry left from another material, or a stale streamed window.
- The shot audit now also checks every drawn material: entry key against the object's material now, each texture index against
  its import's index, the import not superseded, and a shell's import against its current window (and a graph window's current
  index). The next hit names which.

## 4. F2: a broken native D3D12 repeat render in Dragonsreach

- 1128-020905 burst frame 42: third render of snapshot 5365. Pillars, floor, walls and the player are missing; the sky shows; the
  floor's decals draw in stripes.

## Runs

| config | label | result |
| --- | --- | --- |
