# Round 3 lead `compute`: ORG's own async compute queue (2026-11-25)

CS branch `perf3/compute` on org_integration f6b0d91de. ORG `perf3/compute` 0d40d72 on 122d052, BasicRHI `perf3/compute`
963a70b on 4728be0, DXVK `perf3/compute` d966e5d1 on 94063a21 (its submodules initialised from the main checkout's to build it).
CS commits: 36d5fb8cc (submodule bumps), c16a9ee46 (own queues off DXVK's present lock, fold default, streamPoint fix),
386a06ae9 (CS_DCLF_ASYNC_COMPUTE removed), and these notes. Timings are Dev-Fast (/Od), guest mode, the frozen river scene (`trun.sh guest+quick ...
--set env.CS_ENGINE_ZONES=1`).

## Verdict

- The queue choice is ORG's now. DCLF declares its would-be async passes as `QueueKind::Compute` and nothing else;
  `CS_DCLF_ASYNC_COMPUTE`, its live `org:async-compute` command and its docs are deleted.
- ORG folds a kind the device has no queue for into the queue it aliases (`QueueRegistry::PhysicalSlot`): one slot, one
  timeline, no semaphore waits. This covers no compute family, and a compute family whose only queue DXVK takes.
- The perf2 deadlock was not FSR-FG sharing the compute queue. FSR-FG (sl.fsr_g) uses no compute queue: it composes on DXVK's
  graphics queue and two extra graphics-family queues, and Streamline's interposer adds its queues after DXVK's. The cycle was
  DXVK's submission lock: `LockSubmissionQueue` waits for DXVK's queued work and DXVK holds it through presents, and FSR-FG's
  present waits there for the frame's GPU work. ORG's compute submitter, then the uploader, waited for that lock with work the
  frame needed. Fixed by a new DXVK lock held only around DXVK's own `vkQueueSubmit` calls (`dxvkLockQueueSubmissions`), which
  ORG's compute, copy and upload submitters take instead. Kept a lock because a submission beside one of DXVK's once faulted
  the GPU at VA 0 (dclf-architecture.md); the runs here did not test going without one.
- guest+fg with async compute on its own queue still hangs when frame generation turns on, with every ORG submission done and
  every ORG and DXVK thread idle except FSR-FG's present (open item). So guest mode folds the compute kind by default
  (`CS_ORG_QUEUE_TOPOLOGY=fold`); `hardware` takes DXVK's compute queue, as a test override. Native modes take their compute
  queue.
- Pre-existing intermittent: a device loss at frame generation's teardown (`fg off`), seen in round 1 too.
- Found and fixed on the way: guest mode on a device where DXVK exports no compute queue crashed at boot. The deferred epoch
  signalled `streamPoint`, a null semaphore there, in DXVK's submission.

## Queue topologies

| Topology | Who decides | Mapping |
|---|---|---|
| (a) compute family with a free queue | DXVK exports it (`computeQueue`) | guest: folded by default (FG hang), `hardware` takes it; native: own queue |
| (b) one compute queue, taken by DXVK's transfer queue | DXVK exports none | compute aliases graphics, ORG folds it |
| (c) no compute-only family | DXVK exports none; native BasicRHI aliases | ORG folds it |

DXVK never exports a queue another submitter uses, so (b) arrives at ORG as (c). BasicRHI's native Vulkan device now shares one
handle when a kind falls back to a VkQueue an earlier kind already has (it allocated two handles for one VkQueue before). D3D12
always has a compute queue. The test override is `CS_ORG_QUEUE_TOPOLOGY` (guest; `fold` default, `hardware`).

## Measured (Dev-Fast)

GPU-bound frozen river scene, 6 s captures, guest mode, the final build:

| | fold (default) | hardware (own queue) |
|---|---|---|
| Frames per capture | 898, 916 | 936, 934 |
| Frame time | 6.87, 6.69 ms | 6.60, 6.61 ms |
| Render-thread window | 2.846, 2.750 ms | 2.904, 2.784 ms |
| ORG epochs (inclusive) | 0.774, 0.751 | 0.770, 0.750 |
| DCLF CPU | 0.532, 0.505 | 0.569, 0.523 |

The own queue is about 0.19 ms a frame faster on the GPU (3%), which reverses round 2's native result (folding faster there by
0.1 ms, measured with the old scheduling). The render thread is unchanged within noise: the compute submitter is off it. Other
leads' games ran during some captures (p3-se), so the absolute numbers carry about 2% noise; the direction held in both pairs.
The faster topology cannot be the guest default until the FG hang is solved.

## Tests

| Config | Label | Result |
|---|---|---|
| guest+fg, own queue, compute submitter without DXVK's lock | p3-compute-fg1 | hang: uploader waits for DXVK's lock behind FSR-FG's present |
| guest+fg, same with the uploader unlocked | p3-compute-fg2 | hang during a 35 s install at 3 fps beside another lead's game (not this cycle) |
| guest+fg, same | p3-compute-fg3 | hang at FG on: GPU stopped, no ORG work pending, threads idle |
| guest+fg, `none`, no lock at all | p3-compute-fg5-none | crash at boot in DXVK's vkQueueSubmit2: the null `streamPoint` (as fg7) |
| guest+fg, own queue, `dxvkLockQueueSubmissions` | p3-compute-fg6 | hang at FG on (machine loaded), same picture as fg3 |
| guest+fg, `none`, with the lock | p3-compute-fg7-none | crash at boot: the null `streamPoint` |
| guest+quick, `none`, streamPoint fixed | p3-compute-q-none | passed |
| guest+fg, `none`, streamPoint fixed | p3-compute-fg8-none | passed, 5 shots, FG presenting 2x |
| timing captures | p3-compute-t-fold, -t-fold2, -t-hw, -t-hw2 | captured |

Final build (the commits above), all with DXVK `perf3/compute` copied into the stage:

| Config | Label | Result |
|---|---|---|
| list-switches | p3-compute-switches2 | passed (switches.json regenerated) |
| guest+fg (default: folded) | p3-compute-f-fg | device lost at the `fg off` step after all 5 shots: the same fault as round 1's `p-post-slots-guest-fg` (pre-existing, intermittent) |
| guest+fg (default: folded) | p3-compute-f-fg2 | passed, 5 shots checked by eye |
| guest+quick (default) | p3-compute-f-guest | passed |
| guest+quick, `CS_ORG_QUEUE_TOPOLOGY=hardware` | p3-compute-f-guest-hw | passed, shot checked |
| native-vulkan+quick | p3-compute-f-nvk | passed |
| native-d3d12-quick | p3-compute-f-nd12 | passed |

## Decided by the agent, to confirm

- Guest folds the compute kind by default although the own queue is 3% faster on the GPU: guest+fg hangs with it.
- `CS_ORG_QUEUE_TOPOLOGY=hardware` stays as a test switch: it is the reproducer for the open FG hang, not an old path.
- Native modes now put DCLF's async passes on the compute queue (the CS switch that defaulted them to graphics is gone).
  Round 2 measured that 0.1 ms slower in native; not re-measured here. Native FG with async compute is untested.
- `dxvkLockQueueSubmissions` serialises ORG's own-queue submissions with DXVK's submissions, but not with FSR-FG's submissions
  inside DXVK's present (they run on DXVK's submission thread outside the lock). No fault was seen.
- Runs that set `CS_ORG_QUEUE_TOPOLOGY` used the worktree's copy of cs-test (identical but for line endings and three unrelated
  configs) with `CS_TEST_LAB_HELPER` pointing at the main checkout's helper, because the main checkout's switches.json does not
  know the switch until this branch merges.
- The copy queue (shared with the uploader on NVIDIA) stays off-stream when the compute kind folds.
- SE: untested at runtime (AE 1.6.1170); nothing here is runtime specific.

## Open items

- guest+fg with async compute on DXVK's compute queue (`CS_ORG_QUEUE_TOPOLOGY=hardware`): hangs when FG turns on. Stacks:
  DXVK's submission thread in FSR-FG's `vkQueuePresentKHR` (FFX waiting in the driver), DXVK's CS thread in `slEvaluateFeature`
  waiting for an sl.fsr_g mutex, the render thread in `acquireNextImage` (or `vkDeviceWaitIdle` from a swapchain rebuild); the
  graphics timeline stops while `off-stream submissions not done` is empty. Suspects: FFX's game-queue submissions from its own
  threads racing DXVK's on the same VkQueue (sl.fsr_g's `FfxGameQueueSubmit` takes only its own mutex), made likelier by the
  extra DXVK list splits async compute causes. Next step: have `FfxGameQueueSubmit` take `dxvkLockQueueSubmissions`, and capture
  an RGD/Aftermath dump of the stopped GPU.
- Re-measure native async compute (own queue vs folded) with the current scheduling.
