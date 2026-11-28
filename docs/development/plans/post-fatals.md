# post-fatals: three fatal errors seen by snap7-devloss7b

Lead post-fatals, branch `post/fatals` (from org_integration 1e958cce4, snap7-devloss7 merged). Stage `post-fatals`.

## 1. Cloud Shadows: "a run's layer operations read more than 32 faces"

Runs m-gs1 and q-gs2 (snap7-devloss7). Both fatals were in the first render-thread frame after a load hold settled, in
`Sky::SubmitFace -> NoteGraphCloudLayer -> CheckResourcesSide -> PropagateToCompletion -> CopyLayerFace -> QueueLayerOp`.

Cause: not a queue left over at the handover. The read count is legitimate.
- The scratch array held 32 slots for all the layer reads of one frame's SkyReflections taps (the six face taps share it,
  since every gather runs before every scatter).
- Only copies read. `PropagateToCompletion` copies each layer that is new this frame (globalRenderedMask resets at
  EarlyPrepass) to the five other faces, from the nearest lower layer in that face's mask, and copies once to the top layer.
- The other faces' masks are from the last frame that drew them. After a load into an exterior with another weather, they
  hold the old weather's layers, so the new layers interleave with them and each copy reads a different face.
- I added a WIP high-water log (removed again). The first exterior frame of post-fatals-c-gs4 read 19 faces with only two new
  layers (2 and 24) plus layer 27: 6 by the first tap, 11 by the next (two layers times five faces, plus the top copy), then one
  per tap. Four or five interleaved new layers pass 32. So it depends on the weather the load picks, which is why about half
  the graph-switches runs hit it.
- Three runs with a "queue spans taps or frames" WIP log never logged it: the queue is always taken by its own tap.

Fix (f3f767270): `kScratchSlots = kLayers * (kFaces - 1) + kFaces` (166). Each layer is new at most once a frame and is copied to
at most five faces, and each of the six propagations copies once to the top layer, so a frame at one anchor never reads more.
The fatal stays as the invariant, and now prints the queue. Cost: the scratch is an R8 array at the reflection cube's size,
166 slices (10.4 MB at 256x256), about as much as the 32 layer cubes themselves.

Decided by the agent, to confirm: a bigger scratch over a smarter gather. Reading untouched sources straight from their layer
would need far fewer slots, but the face taps' scatters are dispatches in one pass with no barriers between them, so a direct
read could race another tap's write.

## 2. Tap error "uncovered epoch" under CPU saturation

Two runs have it with the same signature: r-cad2 (snap7-devloss7, cl.exe builds at 12.5 of 16 CPUs) and
`1128-023614-s7-mvec2-...-hold5-1` (s7-mvec2, cl.exe at 13 of 16). Both are native-vulkan+cadence-drop-cells.

What the two have in common:
- The transition is `coc Riverwood` from Dragonsreach: interior (CPU pipeline) to exterior (GPU pipeline), a 6.5 s load.
- The fatal is on the game thread, in the load hold's first frames (world frames under the kept loading menu, rendered by the
  producer with the render thread parked).
- Frame N: DCLF's staged GPU builds attach ("staged graphs of epochs 0, 1, 2, 5, 7, 8, 9 attached together") with its scene,
  through a fragment adopted alone. Frame N+1: a late frame revision is adopted (prepared in 83 ms on the lane in mvec2, 9
  frames after its first input) with new SSGI and Skylighting versions (the load's history and probe resets), and DCLF's
  fragment recorded with it. Frame N+1 is the fatal one.
- The adopted scene looks current in every field the message printed: build generation 58 against 58 attached, frame versions
  equal to the new revision, addresses current, payload installed. Yet every epoch is uncovered, including shadow and
  reflection, with no engine state. Two inputs the message did not print can do that: the CPU-pipeline branch taken with no
  view batches (the attached conditions still the interior's), or every epoch's recording on another build than the attached one.

What I did:
- 23eeae102: the coverage line now names those inputs: each uncovered epoch's shape and recording state (none, stale, current),
  the revision's version set against the adopted one, the payload pools, the attached CPU-pipeline flag and the view and forward
  batches. The next occurrence names the input.
- Tried to reproduce, with no luck: cadence-drop-cells under burn.py (14 and 16 workers at normal priority; cs-test counted
  13.6 and 13.8 of 16 CPUs), once with a WIP 150 ms delay on the revision lane (which does land a revision a frame after a build
  attach), and three times with the delay and a 15-worker burn together. All passed.
- Not fixed. The ordering that makes it fatal is not known yet; a guess fix would be a fallback, which the rules forbid.

## 3. The animation crash in the menu (i-rs1): an engine input bug, not CS

Run `1128-042604-s7-devloss7-native-vulkan-window-resize-stress-taa-s7-devloss7-i-rs1`, crash log `crash-2026-11-28-04-29-04.log`.

- CrashLogger names the nearest symbols, and they are wrong. The faulting function is `BSWin32KeyboardDevice::Poll`
  (AE id 68782, +0xF2), called from `BSInputDeviceManager::PollInputDevices` (AE 68617), called from
  `ServingThread::RunLoadingScreen` (AE 39039) through CS's `LoadingLoop` thunk (FrameDriver.cpp:167, a pass-through that parks
  the render thread). RBX is the keyboard device. The thread is the loading screen's serving thread, during the load from the
  main menu ("loaded false", before world-ready; no window resize had run yet).
- The faulting read is `keyState[scanCode]` with scan code 0x1A814CA9: the first entry of the device's DirectInput buffer
  (device+0x78) held garbage.
- `BSDirectInputManager::GetDeviceData` (AE 68678) calls Acquire, Poll and `IDirectInputDevice8::GetDeviceData`, and ignores
  GetDeviceData's result. The in/out count starts at 10 (the buffer's size). When GetDeviceData fails after a successful
  Acquire (input lost or not acquired, for example when focus moves between the two calls), DirectInput leaves the count at 10,
  and Poll walks ten entries that this call never wrote.
- CS does not touch the keyboard device on this path. CS's PollInputDevices hook is the event dispatch at +0x7B, after the
  device polls. The only CS write to the keyboard (`Menu::UpdateGameSide`'s ClearInputState) runs only while the CS settings
  window shows. The render thread was parked by the loading loop.
- So this is a pre-existing engine defect, made likelier by background runs (the game window rarely has focus, and other
  games start beside it). Decided by the agent, to confirm: no CS change. A guard is possible (zero the count when
  GetDeviceData fails), but it would be a game-side input fix with no link to rendering.

## Commits (branch post/fatals)

- f3f767270 fix(cloud-shadows): size layer scratch for a frame
- 23eeae102 chore(dclf): name coverage inputs in the tap error
- this notes file

No submodule pointer moved.

## Runs (stage post-fatals, Dev-Fast)

| Label | Config | Build | Result |
|---|---|---|---|
| post-fatals-a-gs1 | native-vulkan+graph-switches | diagnostics only | passed |
| post-fatals-b-gs2, b-gs3 | native-vulkan+graph-switches | + WIP span log | passed 2 (no queue spanned taps) |
| post-fatals-c-gs4 | native-vulkan+graph-switches | + WIP high-water log | device lost 62 frames after world-ready (Timeline::HostWait; snap7-devloss8's item); its log gave the read counts above |
| post-fatals-d-gs5, d-gs6 | native-vulkan+graph-switches | fix | passed 2 |
| post-fatals-e-cadburn1 | native-vulkan+cadence-drop-cells, burn.py 14 workers | fix | passed (13.6 of 16 CPUs) |
| post-fatals-e-cadburn2 | native-vulkan+cadence-drop-cells, burn.py 16 workers | fix | passed (13.8 of 16 CPUs) |
| post-fatals-f-native-vulkan+cloud-shadows | native-vulkan+cloud-shadows | fix | passed |
| post-fatals-f-native-vulkan+quick | native-vulkan+quick | fix | passed |
| post-fatals-f-guest+quick | guest+quick | fix | passed |
| post-fatals-g-lane1 | native-vulkan+cadence-drop-cells | fix + WIP 150 ms revision lane delay | passed |
| post-fatals-h-laneburn1..3 | native-vulkan+cadence-drop-cells, burn.py 15 workers | fix + WIP lane delay | passed 3 |
| post-fatals-i-head-graph-switches | native-vulkan+graph-switches | HEAD 23eeae102 | passed, with other leads' builds at 13.4 of 16 CPUs |
| post-fatals-i-head-quick | native-vulkan+quick | HEAD 23eeae102 | passed |

Skipped: native-d3d12-quick (no ORG, BasicRHI or native device change), menu-scenes and ui-menus (no menu or UI code
touched; the cloud change only runs in the reflection faces' runs, and the coverage change only formats an error message).

## Open items

- The uncovered-epoch fatal (item 2) is not reproduced and not fixed. The next occurrence prints the deciding input.
  If it names the CPU-pipeline branch with no view batches, look at when `PublishAttachedBuildState` runs relative to the
  fragment adopted alone in frame N. If it names stale recordings, look at which builds the fragment adopted with the late
  revision recorded on.
- Optional engine guard for item 3 (zero the DirectInput count when GetDeviceData fails, AE 68678). Not done: not CS's bug.
- post-fatals-c-gs4's device loss belongs to snap7-devloss8.
