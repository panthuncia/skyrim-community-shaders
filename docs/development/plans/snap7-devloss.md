# snap7-devloss: the device loss on the native render thread

Lead `devloss`, round `snap7`, branch `snap7/devloss` (base `org_integration` fbf14d20c), stage `s7-devloss`. Continues item 1 of
[snap7-thread2.md](snap7-thread2.md). Dev-Fast throughout. RTX 5080.

Status: window-resize-stress-taa passes with an interim wait (section 3). The exact write the wait guards is narrowed to a few
lines of DCLF's depth commit, not named (section 5).

## 1. Commits

| Repo | Branch | Commit | What |
|---|---|---|---|
| BasicRHI | snap7/devloss | 4a28e22 | feat(vulkan): address binding events in fault report |
| BasicRHI | snap7/devloss | 7a0197c | feat(vulkan): more binding lines in fault report |
| CS | snap7/devloss | see report | fix(native): depth commit waits for last render (BasicRHI pointer to 7a0197c) |

## 2. What the fault is

- Both backends. Vulkan: an invalid read in a DCLF graphics pass (main-opaque mostly). D3D12 (one run, s-d3d1): DEVICE_HUNG with
  DRED pointing at a `cs.dclf.main-opaque` ExecuteIndirect. So DCLF's main draws run with bad data, not a Vulkan-only bug.
- The read addresses were never bound. BasicRHI now logs every VK_EXT_device_address_binding_report event (binds and unbinds of
  images, buffers and memory) with crash markers on, and the fault report lists the events near a faulting address. In every
  run since, no event covered or neighboured the address. The lowest address any event named was 0x105d0000; the faults sit at
  0x48d9000..0x656f000 (0x5840000 in about half the runs), or in gaps between allocations. So the shaders read through garbage,
  not through freed memory.
- The GPU stops 50-500 ms after a frame revision (resize, the screenshot source, the upscaler switch). The loss is reported 1.5-2 s
  later. Lines in the log between the two are from the game thread; the render thread is already stuck.

## 3. Cause, as far as it is known

It is a CPU/GPU overlap race between consecutive renders, not repeats as such.

- No repeats: 4 of 4 passed (with thread2's run). Repeats capped at 10 ms (~90 fps): 1 of 3 lost. So the render rate matters.
- The render thread waiting for each render's GPU work before the next render: 3 of 3 passed at 136 fps.
- Waiting for render N-1 after render N is recorded (one render of overlap): 1 of 3 lost.
- A full barrier at the start of every command buffer (serialises the queues' own work): 1 of 3 passed. So it is not a
  same-queue GPU hazard.
- The DCLF async compute passes (fades, tree wind, portals) moved to the graphics queue: 0 of 3 passed.
- Bisection by where render N+1 waits for render N's GPU work (the point taken after render N's `RenderHalf`): after Begin 3/3
  passed; at the depth commit's start 3/3 passed; at its scene streams upload 2/3; at the colour commit 2/3. (After the frame
  record 2/3, but that run used the EndFrame point, so it does not narrow further.)
- So a CPU-side write between the start of `IndirectDraws::Impl::CommitMainPayload` (depth) and `UploadSceneStreams` reaches
  memory the previous render's GPU work still reads. In that window: `TakeRetiredImports`, `AcquireStagedBatch` (Reset),
  `LatchedUploads` for `CurrentRun()`, `WriteFrameRecord` (`FrameRecordOf`, then the latched frame record) and `LatchFrameSlots`
  (latched frame blocks and rows, `ZeroSharedLightOnce`).

Fix (interim, decided by the agent, to confirm): the render thread keeps the GPU point of everything a render submitted
(`Retirement::SubmittedGpuPoint()` after `RenderHalf`), and DCLF's next depth commit waits for it once, before writing anything
(`Native::RenderThread::WaitForLastRender`, called at the top of `CommitMainPayload` for the depth commit). It breaks "the render
thread never waits", but costs little: the previous render's GPU work is nearly always done by then (Dev-Fast, 155-170 fps against
about 170). Guest and producer-thread frames have no point, so they never wait.

A first version took the point in DCLF's `BindAdopted` (EndFrame) instead: it lost the device once in five runs (fix-resize2). So
the reader in render N is submitted after DCLF's EndFrame, or that point does not cover all of the render's work. Worth knowing
when the write is named.

## 4. Ruled out (each 3 runs unless noted)

| Experiment | Result |
|---|---|
| Pooled backings: reuse 1.5 s after return | 1 of 1 passed, 2 broke screenshots (inconclusive) |
| Pooled backings: reuse 64 retirement points late | 1 of 3 passed |
| Pooled backings: a returned backing bound to the submitted point | 1 of 3 passed |
| ORG descriptor slots: reuse 3 s late | 0 of 3 |
| ORG descriptor slots: never reused | 1 of 3 |
| Pipelines and layouts destroyed 3 s late | 0 of 3 |
| No auto aliasing | 0 of 3 |
| Texture streaming off (2 runs) | 0 of 2, 0x5840000 again |
| CPU record mode serial (2 runs) | 0 of 2 |
| Frustum culling only (no HZB) | 0 of 3 |
| Full barrier before each DGC preprocess | 1 of 3 |
| Full barrier at every command buffer's start | 1 of 3 |
| DCLF async compute passes on graphics | 0 of 3 |

Also ruled out earlier by snap7-thread2: game-half calls into DCLF render state, the claims race, deferring replaced main
resources, adopting revisions only at fresh snapshots.

## 5. Next step for the root cause

Narrow the window in section 3 the same way, with the end-of-render point: move `WaitForLastRender` to just after
`WriteFrameRecord`, then to just before it. Three runs each; a pass rate under 3/3 means the write is earlier. Suspects, in order:
1. `FrameRecordOf` making refs or descriptors (a view ref pair's or a heap slot's descriptor rewritten while a render in flight
   reads it).
2. The latched frame record going to a run region the previous render's copy still reads (check `CurrentRun()` in that commit
   against the ticket's run, and the copy lease's wait).
3. `AcquireStagedBatch` handing out a batch whose pages a submitted copy still reads (`use_count() == 1` while the batch's copies
   are recorded but not executed).
Once the write is named, guard that resource's lifetime instead and delete the wait.

## 6. Runs (stage s7-devloss, native-vulkan+window-resize-stress-taa unless named)

| Labels | Build | Result |
|---|---|---|
| a-resize | address binding report | lost, 0x5840000, nothing bound there |
| b-quar1..3, c-quar1..3 | backing reuse 1.5 s late | passed; 5 broke screenshots |
| d-pq1..3, e-pq1..3 | backing reuse 64 points late | 1 passed, 2 broke screenshots; 0 of 3 |
| f-dq1..3 | descriptor slots 3 s late | lost 3 |
| g-norep1..3 | no repeats | passed 3 |
| h-thr1..3 | repeats every 10 ms | passed 2, lost 1 |
| i-notex1..2, i-serial1..2 | tex stream off, serial record | lost 4 |
| j-attr1..2, m-low1..2, p-near1..2 | attribution builds | lost 6 (attribution in section 2) |
| k-pipe1..3 | pipelines destroyed late | lost 3 |
| l-noalias1..3 | no aliasing | lost 3 |
| n-ret1..3 | returned backings bound | passed 1, lost 2 |
| o-frustum1..3 | dclf.cull=frustum | lost 3 |
| q-leak1..3 | slots never reused | passed 1, lost 2 |
| r-prebar1..3 | barrier before preprocess | passed 1, lost 2 |
| s-d3d1..2 | native-d3d12+window-resize-stress-taa | hung in main-opaque (DRED), passed |
| t-serial1..3 | barrier at every list start | passed 1, lost 2 |
| u-idle1..3 | wait for each render's GPU work | passed 3 |
| v-one1..3 | one render of overlap | passed 2, lost 1 |
| w-afterbegin1..3 | wait after Begin | passed 3 |
| x-depthcommit1..3 | wait at the depth commit's start | passed 3 |
| y-colourcommit1..3 | wait at the colour commit | passed 2, lost 1 |
| z-streams1..3 | wait at the scene streams upload | passed 2, lost 1 |
| aa-noasync1..3 | async compute on graphics | lost 3 |
| ab-afterrecord1..3 | wait after the frame record (EndFrame point) | passed 2, lost 1 |
| fix-resize1..5 | wait at the depth commit, EndFrame point | passed 4, lost 1 |
| fix2-resize1..5 | final: wait at the depth commit, end-of-render point | passed 5 (8 in a row with x-depthcommit) |

## 7. Decisions to confirm

- DL-a. The depth commit waits for the last render's GPU work on the native render thread (interim, section 3).
- DL-b. BasicRHI enables VK_EXT_debug_utils and VK_EXT_device_address_binding_report whenever crash markers are on, with object
  names and labels left off when debug utils came only for this. The ring holds 2^18 events (about 25 MB). Only the fault report
  reads it.

## 8. Open items

- The exact write (section 5), then remove the wait.
- The director's native-interior loss after a coc (1127-084855, `cs.dclf.forward.direct`) is likely the same race; not rerun here.
- BasicRHI's report treats images bound into large blocks as the block's range (64 MB), so low-word matches on image blocks are
  noise.
