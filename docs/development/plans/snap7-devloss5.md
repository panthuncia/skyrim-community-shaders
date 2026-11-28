# snap7-devloss5: the render-thread device loss, round 5

Lead `devloss5`, round `snap7`, branch `snap7/devloss5` (base `org_integration` 2fc2920e0, with snap7-devloss4), stage
`s7-devloss5`. Continues [snap7-devloss4.md](snap7-devloss4.md) and the rounds before it. Dev-Fast throughout. RTX 5080.

Status: **the interim wait (decision 229) stays.** Without it the device is still lost, in resize, cell-change and
cloud-shadows runs. This round fixed a pre-existing render-thread deadlock at a resize, removed a game-thread read of the render
side's snapshot and added a check for any other, and left a poison diagnostic for stale buffer view refs, which has not caught a
loss yet.

## 1. Commits

| Repo | Branch | Commit | What |
|---|---|---|---|
| BasicRHI | snap7/devloss5 | 35a4fd4 | feat(vulkan): view ref poison diagnostic |
| CS | snap7/devloss5 | 1a4c5507a | fix(water): game thread reads its own exterior state |
| CS | snap7/devloss5 | 6c8a1e852 | feat(snapshot): log off-thread Current reads |
| CS | snap7/devloss5 | 53b61cb1f | fix(native): post window calls from render thread |
| CS | snap7/devloss5 | 85e0f7154 | chore(basicrhi): view ref poison diagnostic (pointer to 35a4fd4) |
| CS | snap7/devloss5 | (this file) | docs: snap7-devloss5 notes |

The BasicRHI change is on a new branch `snap7/devloss5` in that submodule; 85e0f7154 moves the pointer.

## 2. Diagnostics (devloss4's next steps)

- BasicRHI `BASICRHI_VK_VIEW_REF_POISON=1` (Vulkan, heap and universal binding models): a released buffer view ref pair's
  descriptors cover `0x100000000000 + pair * 0x10000000` (256 MiB a pair, nothing bound there). A load through a stale ref then
  faults at an address that names the pair and the ref's byte offset, whatever the robustness, and the fault report says
  "POISON: a load through released view ref pair N (slot S) at byte offset X; ... last buffer i:g (size at address), released T ms
  ago". Each release is logged with the buffer's name while it is on. Freed pairs are reused oldest first, and only once 4096 are
  free, so a stale ref rarely reads a new buffer. Registered in the switch table. Off by default; kept as a listed diagnostic.
- A per-site trace of the Z-prepass and direct sites' sequences buffer and view slot (`CS_DCLF_REF_TRACE`) was tried and removed:
  the GPU path's Z-prepass keeps one sequences buffer for the whole run (it is sizeless, shared across sizes), and the CPU
  pipeline's sequences versions change every few frames. Nothing pointed at a stale slot.
- Released ORG descriptor slots were not poisoned: devloss1 already showed slot reuse is not it (delayed and never-reused slots
  still lost), and those slots mostly name images.

Poison runs: 11 with it on (8 resize, 3 cadence-drop-cells). 10 had no loss. The one loss (g3, cadence-drop-cells, after
`coc Whiterun`, `cs.dclf.forward.direct`) had no fault record: `vkGetDeviceFaultInfoEXT` failed (-13), which also happens without
the poison (e2). So "a load through a released pair" is neither shown nor ruled out. With the poison on, 11 runs lost 1; with it
off and no wait, about one in five this round. Too few to say the quarantine helps.

## 3. What looked like a fix, and was not

With the wait removed, the first 13 resize runs lost nothing (8 with poison, 5 without). Reverting snap7-resizeui's 195e7cc02
(the water game half stops reading the render side's snapshot) brought a loss back (1 of 5, the old signature). That suggested the
loss was the game thread racing the render thread's `Snapshot::Current()`. The final no-wait branch then lost the device in 3 of
9 world runs: window-resize-stress-taa 1 of 6 (invalid read at 0x5780000 and instruction-pointer faults, 2 s after DCLF adopted
1920x1080 and 5 replaced graphs were torn down), cloud-shadows 1 of 2 (`cs.dclf.forward.direct`, read at 0x5b9c2000, after a
revision with 9 replaced graphs torn down) and cadence-drop-cells 1 of 1 (`cs.dclf.z.direct`, read at 0x5840000 again, after a
revision with 8 torn down). Those runs were under load from other leads ("CPUs saturated by other processes"), which may raise the
rate. So the water race was real but not the cause, or not the only one. The wait went back.

Same family as before: faults at low addresses (0x5780000, 0x5840000) or in gaps, or bad instruction pointers, 50 ms to 2 s after
a frame revision that tears down replaced DCLF graphs. The CPU pipeline's direct sites (`z.direct`, `forward.direct`) lose it too,
not only the GPU path's Z-prepass.

## 4. Fixes kept

- **Water exterior read (1a4c5507a).** `UnifiedWater::FrameExteriorActive` read `CurrentBlock` (the render side's adopted
  snapshot) and is called from the game half: the water port's game frame capture through `GetFlowRewrite`, and the displacement
  mesh update. Off the running render thread it now reads the game's own state, which the snapshot's block captures anyway.
- **Off-thread `Current()` check (6c8a1e852).** `Snapshot::Current()` logs each call site that reads it off the render thread
  while the render thread runs ("[Snapshot] Current() read off the render thread while it renders, a race with its adoption:
  file:line (function)"). `CurrentBlock` passes its caller's location. One atomic load and one thread-local read a call; no switch,
  it only logs. With 1a4c5507a no site fired in the final runs (resize, cloud-shadows, cadence, interior, tour, quick).
- **Window deadlock (53b61cb1f).** Two no-wait resize runs (a-poison3, a-poison5) hung: the render thread did not present for 15 s.
  It was in `SetWindowPos`, called from the engine's window apply (SE 75454, AE 77239) in `Renderer::Begin` after a resize; the game
  thread was in `Main::Update` waiting for `Renderer::Lock`, which the render thread holds for its render half. `SetWindowPos` on a
  window another thread owns sends to that thread and waits. snap7-devloss3's n-rev5 hung the same way with the wait in place. Now,
  on the render thread, that function's `ShowWindow` is `ShowWindowAsync` and its `SetWindowPos` gets `SWP_ASYNCWINDOWPOS`
  (call-site hooks in `RenderThread.cpp`); both calls are cosmetic there (show, not topmost). No hang in the runs since.

## 5. Runs (stage s7-devloss5, native-vulkan+window-resize-stress-taa unless named)

| Labels | Build | Result |
|---|---|---|
| a-poison1..8 | no wait, poison and trace | passed 6, hung 2 (window deadlock) |
| b-nowait1..5 | no wait | passed 5 |
| c-nowater1..5 | no wait, 195e7cc02 reverted | passed 4, lost 1 |
| d-detect1 | + window hooks, Current() check | passed; the check named UnifiedWater.cpp:561 |
| e1..5; e-cs1..3 (cloud-shadows); e-nvq1 (quick) | + game-half water read | resize passed 4, lost 1 (e2: after a 70 s game-thread stall, `cs.screenshot.copy`, no fault record); cloud-shadows 3 and quick passed |
| f-resize1..6 | final fixes, no wait | passed 5, lost 1 |
| f-cs1..3 (cloud-shadows) | same | passed 1, lost 1, not run 1 (slot busy) |
| f-cad (cadence-drop-cells) | same | lost |
| f-nvq, f-int (native-interior), f-tour (native-tour), f-d3d (native-d3d12-quick), f-gq (guest+quick), f-gresize (guest+window-resize-stress-taa) | same | passed 6 |
| f-perf, f-dr, f-d3dperf | same | passed (numbers below) |
| g1..3-poison | no wait, poison | cadence passed 1, resize passed 1, cadence lost 1 (g3, no fault record) |
| h-* | final branch, wait kept | section 6 |

Frame rates without the wait (Dev-Fast /Od, one sample, steady [Perf] lines): native-perf-taa 146-149 fps,
native-vulkan+perf-dragonsreach 217-221 fps, native-d3d12-perf-taa 91-93 fps. snap7-devloss3's wait build measured 146-147 and
220-224. So the wait costs nothing measurable, which matters for how long it may stay.

## 6. Final runs (branch 85e0f7154, wait kept)

| Label | Config | Result |
|---|---|---|
| h-resize1..3 | native-vulkan+window-resize-stress-taa | passed 3 |
| h-cs1 | native-vulkan+cloud-shadows | passed |
| h-cad | native-vulkan+cadence-drop-cells | passed |
| h-nvq | native-vulkan+quick | passed |
| h-gq | guest+quick | passed (shared code: Publication.h, UnifiedWater) |
| h-d3d | native-d3d12-quick | passed (BasicRHI pointer moved; the change is Vulkan only and off by default) |

No `[Snapshot] Current() read off` line in any of them. Skipped on the final branch: native-interior, native-tour and
guest+window-resize-stress-taa (all passed on the no-wait build f-*, and the final branch only adds the wait back), and the perf
configs (the final branch is the wait build plus three changes off the frame's path).

## 7. Decisions to confirm

- DV5-a. The interim wait (DL-a, decision 229) stays: without it 3 of 9 final world runs still lost the device. It costs no
  measurable frame rate (section 5).
- DV5-b. The render thread's window calls are posted (`ShowWindowAsync`, `SWP_ASYNCWINDOWPOS`), removing a deadlock with the game
  thread over `Renderer::Lock`. Decided by the agent: a narrow hook rather than moving the window apply to the game thread.
- DV5-c. `Snapshot::Current()` logs off-thread reads while the render thread runs, always on (logging only).
- DV5-d. `BASICRHI_VK_VIEW_REF_POISON` stays as a listed diagnostic (off by default, Vulkan only); `CS_DCLF_REF_TRACE` was removed.
- DV5-e. Runs with the new switch used a scratch copy of the main checkout's cs-test whose switches.json has it (cs-test checks
  `--set env.*` against its own schema, and the main checkout's does not have it yet). Same code, same lab locks.

## 8. Open items

- The device loss without the wait. Next steps:
  1. Run the poison until a loss with a fault record (cadence-drop-cells and cloud-shadows reach it faster than resize, in under
     2 minutes). A "POISON" line names the released pair and its buffer. A low-address fault without one means the bad address is
     not a ref read; the next suspect is then a front-end read (an indirect argument, an index buffer or a row address the pipeline
     layout reads) from a buffer version the render's recording did not hold.
  2. The common trigger is a frame revision that tears down replaced DCLF graphs (5-10 at a time) 50 ms to 2 s before the fault,
     in both pipelines. Check what those graphs own that a later render still names. The CPU pipeline's view batch sets bind their
     sequences lease at the forward slot's close (`CloseForwardSlot`), and `BindAdopted` binds at DCLF's EndFrame; work the render
     submits after either is not covered (devloss1 saw an EndFrame point lose once in five).
  3. `vkGetDeviceFaultInfoEXT` sometimes fails (-13) after the loss, leaving no record (g3, e2). Worth a retry of the size query,
     or logging the checkpoints only.
- e2: after the window fix, the game thread waited 70 s, then 50 s, for `Renderer::Lock` after a resize while the render thread
  rendered repeats. The render thread takes the lock back at once each render, so the game thread can starve. It is the lock's
  fairness (post-collapse's area); the deadlock before the fix hid it.
- The main checkout's switches.json needs `BASICRHI_VK_VIEW_REF_POISON` (this branch has it) before cs-test there can set it.
