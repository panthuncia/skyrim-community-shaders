# perf3-stability: correctness bugs from lead `se` (round 3)

Lead `stability`, CS branch `perf3/stability` (from org_integration 600b56386), ORG branch `perf3/stability` (from 8b7b709).

## 1. Fatal: the profiler's timestamp switch refused staged builds (fixed)

`FollowProfilerTimestamps` turns per-pass recording timestamps on while the profiler is in use and back to the startup mode
after 300 idle frames. `GraphHost::SetRecordingTimestampMode` bumped the host configuration, so every build in flight or staged
before the switch was refused at its attach. A staged set refused at a frame revision's adoption is fatal in CS
(`p3-se-guest6`). The idle switch lands around frame 300, close to the first save load's revisions in every run, AE too.

Fix (ORG 9a17735): the timestamp mode is a build input. `SnapshotBuildInputs` takes it with the other inputs, the graph keeps the
mode it was built with, and a change only requests a rebuild (no configuration bump). A graph built, staged or attached with
the old mode stays valid and runs until its rebuild replaces it; the build manager builds the request asynchronously and staged
epochs switch at the next revision. The refusal path itself stays: the frame-wait timeline, the queue boundary and
`DestroyGraph` still bump the configuration (all setup or diagnostics). `GpuTimestamps::Ready` already waits for
`!RebuildRequested()`, so it still reads timestamps only once every graph carries the new mode.

## 2. Intermittent "broken exterior" (test-harness artifact, fixed)

Not a rendering bug. Every bad shot (SE guest7, g10, nv1, nv2; AE `1120-212334-...-t5` exterior) is the run's first shot,
followed one frame later by a `coc`. The first shot of a run changes `Screenshot.Source`, so the copy switches in with the next
frame revision and the capture is taken some frames after the step (g10: "capture 1 waits for a run that holds its copy",
read back after 18 capture points, at frame 1316; the coc's load started at 1310). The shot is the loading screen: the LEVEL bar
is the loading menu's, and the sky with black silhouettes is the loading screen's view, which is why Riverwood (g10) and
Whiterun (guest7) give the same picture. Runs whose first shot is not followed by a coc (g11-g13, nv3, nv4) look right.

Fix (CS): the step runner holds the next step while a scripted capture is still being saved (`ScreenshotFeature::
ScriptedCapturesPending`), with the step's wait timeout failing the run. README "Steps" notes it.

## 3. The guest8 load hold (read only, not fixed)

Code reading, no runs. `GpuTextures::ResolveBinding` pins the full window (`TextureStreaming::PinFull`): the material waits until
the full mip chain is resident and asks again once a second. The window is built as a DCLF scene graph job
(`SceneGraph::PostTextureWindow`, streaming lane) and installed through `ORG::Queue` on the render thread, GPU ready once its
copy pass was submitted. Pinned textures count as visible, so the controller loads them regardless of the allowance, but
upgrades are capped per tick (`kMaxUpgradesInFlight`, the rate). Candidates for a 60 s stall with 2376 objects waiting:
- the window's copy pass rides a stage that the load hold withholds, so the window never becomes ready while the hold waits
  for it (a cycle; would need the hold and the upload to be on the same frame path);
- a `ReadWindow` result of Skip/Failed without an install, which leaves `residentTop` unchanged and the pin re-asking forever.
Next step if it reappears: `CS_DCLF_TEX_STREAM` dump (the controller's per-texture table, `pending`/`pendingTicks`/`pinned`)
during the hold. Decided by the agent, to confirm: left open, since it did not reproduce in se's later runs.

## Runs (cs-test, stage p3-stability, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| guest+imagespace-transitions (shot then coc at once, coc/teleport run) | p3-stability-ist1 | passed; the exterior shot is the world with the compass; the coc after it held 40 frames for the file; the CS and DCLF epochs staged right after the profiler's idle switch were adopted |
| guest+quick | p3-stability-gq | passed |
| guest-interior | p3-stability-gi | passed |
| native-vulkan+quick | p3-stability-nvq | passed |
| native-d3d12-quick | p3-stability-nd3q | passed |

Every run logged the profiler's idle switch and no build refusal or rejection. No timing capture: neither change touches the
render thread's per-frame work (the timestamp switch happens twice a run; the shot hold is test-only).

## Commits

- ORG `perf3/stability` 9a17735 fix(host): timestamp mode is a build input
- CS `perf3/stability`: 89e00da4c chore(org) ORG bump, 54c062ad4 fix(test): hold the next step until a shot saves, d174b4da8
  docs(test) README, and these notes.
- Not tested on SE at runtime (both fixes are runtime-independent; SE's guest6/guest7 timing is the same as AE's).
