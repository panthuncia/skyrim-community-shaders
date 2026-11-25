# Round 2 lead `finish`: guest tickets finished off the render thread (2026-11-25)

CS branch `perf2/finish` on org_integration ed1425760, ORG branch `perf2/finish` on 783f369, DXVK branch `perf2/finish` on
e8f692d6. No BasicRHI or OMS changes. Timings are Dev-Fast (/Od), guest mode, the frozen river scene
(`trun.sh guest+quick ... --set env.CS_ENGINE_ZONES=1`), per frame.

The user asked: move `GraphHost::FinishTicket` off the render thread in guest mode, and see whether it is faster and correct.

## Verdict

Faster (render thread -0.8 ms a frame, Dev-Fast) and correct on every config run, with one condition: DCLF's would-be async
passes now default to the graphics queue in guest mode too. With them on DXVK's compute queue, FSR frame generation deadlocks
(below). It is the only path now (no switch for the old one).

## The payoff, measured first

In the baseline capture (`p2-finish-base1`, unchanged branch) `ORG.Host.FinishTicket` was 0.768 ms a frame inclusive on the
render thread (14.5 tickets a frame): admission 0.20, the external execution with its ledger commit 0.26, the ticket's
submission bookkeeping and handoffs 0.10, upload recording 0.12, its own self 0.07. Only the off-stream submission's stream
point (8 us) needed the render thread's place in DXVK's stream. Every other part only needs order between tickets, which the
submission thread keeps.

## Design as built

- **ORG.** The host runs its submission thread in guest mode too, and external tickets go through it like any other:
  `SubmitEpoch` commits on the render thread and enqueues the rest. The external execution is made at the commit
  (`CommitTicket`), so `TakeExternalExecution` hands it out at once; `FinishTicket` adds the ticket's segments to it on the
  thread. `ExternalEpochExecution` counts its tickets, and `WaitFinished` blocks a reader until all are finished (false when one
  failed; a discarded ticket counts as failed). `SetSubmissionContext` / `SubmissionContext` carry a caller value with each
  ticket to the hooks its finish calls; `OnSubmissionThread` names the thread. Seals are queued on the thread in guest mode too
  (`queuedSeals`), so the frame boundary does not drain it.
- **DXVK.** `dxvkEmitDeferredExternalCommands`: a place in the D3D11 stream whose external commands are known only later. Its
  callback runs on DXVK's CS thread in stream order, may block, and records any number of external commands through a recorder;
  a command marked split ends and submits DXVK's command list first (`flushCommandList` on the CS thread).
- **CS.** Each epoch opens an `EpochStream` (its `streamPoint` entry value; the compute and copy queues' exit values) that its
  tickets carry. The queue hook, now on the submission thread, adds the entry wait to each queue's first submission of the
  epoch and records the exits. `EmitEpoch` emits one deferred command per epoch at its place. On DXVK's CS thread,
  `RecordDeferredEpoch` waits for the execution, then records the entry signal (and ends the list when a queue waited for it),
  each segment (split ones in a new list), and the exit wait (in a new list when `exitSplit`). DCLF's CPU sites use the same path
  (`EmitExternalEpoch` with a `SegmentRecorder`); `CloseExternalEpoch` signals an unemitted epoch's segments unexecuted.
  `FlushPendingStreamWork` also flushes when epochs were emitted since its last flush (their signals sit in DXVK's current
  list). Removed: `EnqueueStreamPoint`, `EmitExecution`, `CloseComputeEpoch`, `SignalExecutionUnexecuted`, `SplitStream`, the
  render thread's D3D11 flushes for splits.
- **Ordering DXVK relies on.** DXVK reads the segments (lists, waits, signals, splits, `exitSplit`) only on its CS thread, after
  `WaitFinished`. The entry point is signalled at the epoch's place, after every D3D11 command before the epoch (it used to be at
  the first off-stream ticket's commit; for a joined stage with compute that now also covers the commits up to the outer
  epoch's emission, never anything after the epoch). The render thread never waits: the only new wait is DXVK's CS thread on
  the submission thread, 0.001 ms a frame (the finish is done long before the CS thread gets there). The submission thread never
  waits on DXVK's stream: a graphics submission from it is an error.
- **Native modes** already finished tickets on the submission thread (no external epochs). The change does not alter them.

## FSR frame generation and the compute queue

guest+fg hung 8 runs of 8 with the change and DCLF's async compute on, and passed twice on the unchanged base (also with this
branch's DXVK build, so DXVK is not the cause). The hang reports: DXVK's submission thread inside FSR-FG's `vkQueuePresentKHR`
waiting for the GPU while it holds DXVK's submission lock; DXVK's CS thread (or the uploader, or ORG's compute submitter)
waiting for that lock or the FSR-FG mutex; ORG's compute batches submitted with their waits met but not done, so the compute
queue is blocked by work queued ahead of them. FSR-FG's interpolation runs on DXVK's compute queue, which ORG treats as its own,
and the graph's compute work and the frame's graphics work depend on each other. Tried: compute batches waiting on the CPU for
their points while frame generation is selected (still hung: the interpolation then got ahead of them), and handing the batches
over at the epoch's place behind DXVK's submission lock (still hung). With `CS_DCLF_ASYNC_COMPUTE=0` the same build passed.
So `DCLF::Draws::AsyncComputeQueue` now defaults to the graphics queue in guest mode as in native mode (native measured the
cross-queue waits at 0.13 ms a frame against 0.03 ms of work); `CS_DCLF_ASYNC_COMPUTE=1` forces the compute queue. The base
passed by timing: its render thread submitted the compute batches later relative to DXVK's presents. The underlying sharing
is an open item.

## Measured (Dev-Fast, ms per frame)

`after1` ran on a busier machine (untouched zones 10-20% higher). `after2` (async compute on) and `after3` (the final build,
async compute off) match the baseline on untouched zones (`CS.Anchors.ClientCommit` 0.184 / 0.177 / 0.168).

| | base1 | after2 | after3 (final) |
|---|---|---|---|
| Window | 3.598 | 2.772 | 2.670 |
| ORG epochs (inclusive) | 1.504 | 0.737 | 0.726 |
| `ORG.Host.FinishTicket` on the render thread (inclusive) | 0.768 | 0 | 0 |
| `ORG.Host.EnqueueTicket` (new) | 0 | 0.044 | |
| `CS.ORG.EmitExecution` / `CS.ORG.EmitEpoch` | 0.055 | 0.007 | |
| `CS.ORG.FramePace` (waiting for the GPU) | 0.796 | 1.726 | 2.007 |
| Frames in the 6 s capture | 940 | 945 | 934 |

The scene is GPU-bound, so the frame rate is unchanged and the freed render-thread time shows up as frame pacing. Submission
thread (after1): `ORG.Host.SubmitThread.Ticket` 0.97 ms a frame busy, far from a limit. DXVK's CS thread:
`CS.ORG.DeferredEpoch.WaitFinished` 0.001 ms a frame. Render-thread drains: none in steady frames.

## Tests

ORG unit tests (standalone Vulkan build of ORG 122d052): `OpenRenderGraphPersistentVulkanHostTests` stream, stream thread (new:
two tickets per execution finished on the submission thread, compute batches submitted from it), stream open, stream open home,
external, external-reuse, segment-external, async, revision, segment, reuse, built, split and the default pass.
`external-reuse` timed out once in 33 runs (the first, while a game was loading; 32 later runs passed in 2-10 s).

Game runs on the final build (stage `p2-finish`), all passed, shots checked by eye:

| Config | Label | Result |
|---|---|---|
| guest+fg | p2-finish-f-fg1, p2-finish-f-fg2 | passed, 5 shots each |
| guest+quick | p2-finish-f-guest | passed |
| guest-interior | p2-finish-f-interior | passed, 4 shots |
| guest+quick, coc Dragonsreach and back twice | p2-finish-f-teleport | passed, 5 shots |
| native-vulkan+quick | p2-finish-f-nvk | passed |
| native-d3d12-quick | p2-finish-v-nd12 (the build before the async-compute default, which native already had) | passed |

Earlier builds: guest+fg `p2-finish-v-fg`..`fg8` hung (above); `p2-finish-v-fg9-noasync` passed; base `p2-finish-basefg1/2`
and `p2-finish-basefg-mydxvk` passed. Timing captures: `p2-finish-base1`, `-after1`, `-after2`, `-after3`.

## Lead clients' hang (director's request)

Run `1125-170348-p2-clients-guest-hdr-p2-clients-v-guest-hdr`: not a deadlock. The render thread sat in
`RenderGraph::StopFrameProduction` (the old graph's ownership-retirement task scope) inside an attach of ten staged graphs;
the attach finished on its own after 35.7 s (`attached together in 35749.484 ms`), and the run reached world-ready. The watchdog
fired at 15 s. It is unrelated to lead ticket2's pinned copy step: the stack holds no ticket, lease or `WaitTicket` frame, and the
same 40.5 s attach happened in round 1 (`1125-035928-p-tickets-guest-quick-p-tickets-base3`), before `Async::lastPinned` existed.
Both happened at 1-3 fps on a loaded machine. Every executor worker was idle in the snapshot, and the minidump has no heap to
read the scope's count, so why the scope stayed busy so long is not settled. Over 31,091 attaches in the run folders the render
thread blocks a median 0.26 ms, p99 70 ms, and twice 35-41 s. That wait is against the render-thread rule; moving
`StopFrameProduction`'s waits off the render thread (retiring the old graph after its scopes drain) is an open item.

## Decided by the agent, to confirm

- DCLF's async passes default to the graphics queue in guest mode too (`CS_DCLF_ASYNC_COMPUTE`), for FSR frame generation.
  Not measured on the GPU separately in guest mode; frames per capture stayed within noise (940 / 934).
- The compute entry point moved from the first off-stream ticket's commit to the epoch's emission.
- The stream-order guard (`GuardStreamWaits`) does not run for the deferred epochs: it never fired in any run folder, and their
  splits come from ORG. It stays for `EmitExternalCommands`.
- A split now ends DXVK's command list on its CS thread without bumping the immediate context's submission counter, so the
  profiler's split flag misses those (diagnostic only).
- Guest presents no longer drain the submission thread (`QueuesPresent` is true in guest mode).

## Open items

- ORG's compute queue is shared with FSR-FG's interpolation (DXVK exports family 2 index 0, which FSR-FG evidently uses too).
  A queue of ORG's own (or FSR-FG kept off it) would let async compute come back in guest mode.
- `external-reuse` timed out once in 33 runs after its rebuild for recording timestamps (synchronous host, no submission
  thread, so not this path).
- `StopFrameProduction` waits on the render thread at installs (above).
