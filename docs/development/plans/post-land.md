# post-land: B5, the land missing after a coc back outside

Lead `post-land`, branch `post/land` (base `org_integration` 9b0ed9aa8), stage `s7-post-land`. Problem: decision 240's B5.
native-interior's vl-exterior shot (1127-225423) showed snow, floating rocks and trees, no bridge and no player after
`coc Whiterun` from Dragonsreach. A native D3D12 quick start shot (1127-114515) showed a grey flat plane with rocks above it.

RTX 5080, Dev-Fast builds. Outcome: B5 is a rendering defect, localised to FrameValues' placement rows; the mechanism that leaves
them wrong is not found yet. The diagnostics that localised it stay in, so the next hit names more. No fix landed.

## 1. Rendering, not game state

- cs-test logs the player's place at every shot (player and camera position and rotation, cell, land, land height under the
  player) and, in the shot's render, the eye and view rows the frame drew with. A `place` step logs the same.
- In every bad shot the player stood on the bridge (35879 -17024 -4206, 390 units above the land, cell 8,-5, land loaded) and the
  frame's eye and view were the good runs' (35684 -17015 -4065). The player did not fall; the frame drew the world wrong.
- Guest and d3d11 never showed it in the runs I have (B5 is native only so far; not run here in numbers).

## 2. The rows are wrong

The shot's render logs an audit (`Diagnostics/Scene/FrameAudit.cpp`): for every object of the installed tables, its geometry's
buffers and its FrameValues placement row against the engine now, the first mismatches with whose place the row holds, and the
producer's counters since the last shot. In good frames only animated effects differ (flames, sparks, smoke: 20-320 rows).
s7-post-land-lc11 round 1 reproduced the full B5 (snowy slabs over the bridge, no player, floating trees):

- buffers: 8962 checked, 0 differ;
- rows: 9852 checked, 9808 differ, all 100 land quads among them;
- the same on all four shots of the round (25 s), also after DCLF's epochs were rebuilt twice (frustum culling, then occlusion);
- the next coc into Dragonsreach made the rows right again.

So the installed tables agree with the engine and the placement rows do not: nearly every object draws with another object's or a
stale transform. That explains the picture (right meshes at wrong places: land slabs over the camera, rocks and trees floating,
the bridge and the player somewhere else). The objects still right were the ones resampled every frame (effects), so the rows of
everything sampled once after the coc went wrong and were never sampled again.

What was ruled out:
- Slot/table corruption: buffers match the engine for every object.
- The GPU pipeline's epochs: lc7 round 2 kept its wrong state on DCLF's CPU pipeline too, and epoch rebuilds did not clear it.
- Stale carried "settling" items overwriting a reassigned slot: a build that dropped carried items whose slot a later plan gave
  to another object counted 0 such items over 6 runs (s7-post-land-fx1..6); reverted. The producer lists the newest plan's items
  first, which already protects that case.
- The draw-capacity pattern seen in the first bad runs ("16384 draws grown to 16384" at the coc out, no new sort staging): an
  experiment that forced it (exp1-3) passed, and 20 later rounds with it passed.
- Starvation: ni7 had no CPU saturation.

Notes for the next step:
- The producer's engine-read leases are refused far more often than they succeed (300k-900k refused items per 5-10 s against
  130k-580k sampled). Written items then wait in `settling`, movers owe their keys. Whatever leaves the rows wrong probably lives
  in that retry path or in how plans reach `KickSample`; a refused-lease storm right after a coc is the first thing to test.
- Rows are sampled once per written item; a static object whose first sample is wrong is never sampled again.
- The audit's next hit says, per wrong row, "holds (x y z), slot N's place" or "no object's place". "Slot N's place" means the
  rows and tables disagree on which object holds which slot; "no object's place" means the rows hold a departed or never-placed
  object's transform.

## 3. Repro rate and the hunt config

- native-interior: 2 of about 45 runs on 11-27 (pin2, ni7).
- `land-cocs` repeats native-interior's interior steps (its shots, Volumetric Lighting's custom size) and the coc back out five
  times per run, with the place logged and shots after each coc back out, then with frustum culling only and back to occlusion.
  s7-post-land-lc1..15 and fx1..6: about 100 coc-outs, one full B5 (lc11 round 1) and two white-land rounds (below).
- Every bad round had a short exterior load hold (9-12 frames, the claims settled before the exterior's objects joined), but most
  short-hold rounds were fine.

## 4. A second symptom: white land

lc7 round 2 and lc11 round 2 drew parts of the land flat white (snow-like) with every row right. In lc7 it lasted at least 300
frames, on the CPU pipeline too, and was gone 180 frames after switching back. Not investigated further; candidates are the land's
shading rows, the landscape seams quad table (`Features/Landscape/LandscapeSeams.cpp`) or a texture window swap.

## 5. The D3D12 start shot is another case

- 1127-114515's load hold never settled: "released after 632 frames, 11494 ms, no progress ... objects by reason: material 7552".
  Its load prewarm imported 475 of 555 textures. Every other native-d3d12-quick run since 11-26 settled in 35-130 frames and imported
  all of them.
- So the near land's materials never became ready and DCLF withheld the land. The grey plane is terrain LOD (lowered 230 units under
  the loaded grid, as the engine draws it), with the near objects above it. What stalled the imports is not known (texture
  streaming's base windows or the import thread).

## 6. Other things seen

- fx5 crashed on the game thread in `FrameView::GetSunCandidates` (`FrameView.h:231`), from `IndirectDraws::WriteAccumulated`
  (`Visibility.cpp:958`) at `PrimaryCull::AtUpdateEntry`, right after a `dclf:cull` toggle: the game thread read `renderFrame` (the
  render side's frame) while the render thread can replace it. A race outside the game half.
- lc6 timed out waiting for `settled` after the second `dclf:pipeline=cpu` of the run (the graph was never built for the CPU
  pipeline again); land-cocs no longer toggles the pipeline.
- lc14 lost the device ("fault invalid read at 0x5840000") during round 1 (snap7-devloss5's area).

## 7. Commits

- `test(cs-test): log the player's place at each shot`
- `test(dclf): audit the frame's rows at each shot` (native only; the audit takes the producer's counters, which CS_DCLF_STATS
  reports also take)
- `test(cs-test): land-cocs hunts B5 over five cocs`

Decided by the agent, to confirm: the audit stays on at every native shot (one pass over the tables, about 10k engine reads, at a
shot only), so any suite run that hits B5 records it.
