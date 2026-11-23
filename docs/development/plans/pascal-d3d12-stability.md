# Native D3D12 on Pascal: hangs (investigation)

Status: 2026-11-23, in progress.

Native D3D12 on the GTX 1070 Ti stops in two different ways. They have different causes.

## 1. Graphics/copy queue deadlock (the frequent one)

**What it looks like:**
- The render thread stops presenting. The hang watchdog reports it after 15 s, and the run ends as `hang`.
- There is no device removal: the GPU sits idle on a queue wait.
- The log shows `CopyQueueUploadService 'CopyQueueUploads': a wait was not reached in 10 s (timeline t9 wants N, at M)`.

**How often:** 2 of the first 6 runs of 2026-11-23, then 2 in a row in the feature sweep. It is not tied to the exit step or to a
build switch-in.

**Where:** the first incomplete graphics submission is always at the same place in the frame (the deferred+water epoch, judged by
its signal values). The D3D12 timeline dump (`[HANG] D3D12 timelines`, added 2026-11-23) shows the cycle directly:
- The copy queue's frame-values submission waits for graphics value 384462, while graphics has completed only 384390.
- Graphics submissions before 384462 wait for frame-values signal 4109, which only that copy submission signals.

**Cause:**
- A DCLF frame-values job reused a backing from its capped pool, `cs.dclf.frame-values.*`. The cap is three copies (`SceneCopies`),
  while D3D12 keeps four frames in flight, and each frame leases twice (late poses and main).
- The backing was bound at `Kick`, or by `ReleaseLatePoses`, to `SubmittedPoint()`. At the start of a frame that point can already
  include the frame's early epochs, and those epochs wait for this job's signal.
- The job then put a copy-queue GPU wait on that point.
- The guard `Retirement::Sealed` did not catch it: it returns true while the last seal is still queued on the submission thread.

**Fix (2026-11-23):**
- The frame-values pools are uncapped and never reuse a backing still bound to unfinished work (`reuseBound` off). A job never
  waits on the GPU now, so this cycle cannot form.
- A first attempt kept reuse but replaced unsafe backings with fresh ones, using a strict sealed check. It still stalled: work that
  the last present sealed can still wait for a later frame-values signal, so the sealing model does not hold here. That guard is
  left in `LeaseBackings`, and it only matters if reuse is turned back on.
- Verification: 2 of 7 runs still stopped, both on the second cycle below (6 of 8 before).
- The frame is also about 1.7 ms faster (40.8 against 42.5 ms), since the copy queue no longer waits for graphics.

**A second cycle:** the graphics queue waits for "CS external work.Slot0" value N, which a later submission on the same graphics
queue signals.
- External work (`ExternalWork::Submit`: the native immediate context's operations, such as a streamed texture's initial layout,
  and the present copy) submits directly on the calling thread. Graph epochs go through the host's submission thread.
- An external submission can wait for a graph epoch still on that thread (`RenderGraphRuntime::DrainSubmissions` drains only on
  the render owner). Its GPU wait then reaches the queue before the epoch's signal, on the same queue.
- **Fix:** BasicRHI's D3D12 `q_submit` now waits on the CPU, before taking the queue's lock, until every wait value has been
  submitted by some queue (`Dx12WaitForSubmittedSignals`). It gives up after 2 s, logged, and skips timelines BasicRHI never
  signals.
- It waits 1-11 ms at times, for the frame-values signals (logged as "waited ... to be submitted", 32 times at most).
- Since the fix: no stall in 22 runs.

**Other fix:** with DynamicCubemaps off, native D3D12 stopped at the first world frame ("Skylighting's probe array is null").
- `Deferred.cpp` passed the probe array (t8) only with DynamicCubemaps, but the composite shader reads it under `SKYLIGHTING` alone.
- It is now passed whenever Skylighting is loaded. On D3D11 this also stops Skylighting's ambient reading a null texture without
  DynamicCubemaps.

**Also fixed on the way:** ORG `EventQueue` let pushes made after a spill overtake the spilled events, which broke the per-producer
order that `CopyQueueUploadService` relies on. Later pushes now spill too while spilled events wait. This bug was not shown to
cause the hang.

## 2. Device hung in DCLF's GPU build pass (frequent once the stalls were gone)

**What it looks like:** `0x887A0006` (DXGI_ERROR_DEVICE_HUNG), no page fault, about a second after a frame stall.
- In the frozen-scene perf runs, it hit 2 of 6 to 5 of 6 runs, from 50 to 2045 frames after world-ready.
- DRED, with the open labels: the last completed op is the `cs.dclf.build-draws` ExecuteIndirect (BuildDrawsCS), in the colour epoch
  (list of 180 ops) or a shadow epoch (45 ops). The next op, the barrier of `cs.dclf.sort-scan` or of
  `cs.volumetric-lighting.generate`, never completes.
- It predates this work: the first one was seen in a tour before any of these changes.

**Ruled out** (each a batch of runs that still lost the device):
- the frame-values pool change;
- the queue submission log;
- frame placement (`ORG_FRAME_PLACEMENT=false`);
- the waterfall loop caps;
- caps on BuildDrawsCS's latch-count loops (local shadow volumes, sun cascades and processes, classes), portal program operators
  (4096) and partition chains (1024). The caps are kept as hardening.

**Workaround, now the default on such GPUs:** the CPU pipeline (`CS_DCLF_PIPELINE=cpu`) never runs BuildDrawsCS.
- It passed 6 of 6 perf runs, and it is faster: 38.6 ms against 41.0 ms for the GPU pipeline.
- DCLF's pipeline auto mode now draws exteriors CPU-driven too on a GPU with the legacy binding model
  (`CpuExteriorsByDefault`: D3D12 resource heap tier 1, or Vulkan without bindless descriptors, which means Pascal and older).
- `CS_DCLF_PIPELINE=gpu` still selects the GPU pipeline.

**Open:** the BuildDrawsCS hang itself. Next steps:
- an iteration budget per input that bails out and counts into a build counter, to name the loop;
- or the D3D12 debug layer with GPU-based validation, in case a stale descriptor read faults (Pascal may report an MMU fault as a hang).

## Tools

- `[HANG] D3D12 timelines`: every timeline's completed value, with its recent signals and waits and the queue of each
  (`rhi::dx12::DumpTimelineHistories`). A wait whose value is past its timeline's completed one blocks its queue.
- DRED open labels: each incomplete list's labels at the op the GPU is on, and at the last completed op (an indirect op completes
  once issued, so its work may be what is still running).
- `BASICRHI_DX12_SUBMIT_LOG=1`: each D3D12 queue's last 64 submissions (waits, signals, thread) in the stall report.
- Soak: repeated `native-d3d12-quick` runs until the first failure.
