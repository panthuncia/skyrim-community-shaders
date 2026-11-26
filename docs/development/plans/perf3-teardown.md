# Round 3 lead `teardown`: retired graphs torn down off the render thread, workers raised for waits (2026-11-25)

CS branch `perf3/teardown` on org_integration 69a1d2183, with org_integration 8ad13b3b7 (perf3/compute) merged in. ORG branch
`perf3/teardown` on ORG perf3/merge ebb5d04. OMS branch `perf3/teardown` on OMS perf3/install e6972ac (OMS has `update = none`:
check it out by hand when merging). BasicRHI and DXVK unchanged. Timings are Dev-Fast (/Od).

## Changes

- **Retired-graph teardown leaves the render thread (ORG `GraphHost::ReleaseRetiredGraphs`).** With async epochs it no longer
  takes `HoldAsync` (which drained the submission thread and held the host's thread while the teardown ran on the render
  thread). It moves the releasable graphs (GPU work done, task scopes idle) into an `Async::Message::Kind::Release` and posts it.
  The host's thread gives back their alias placements (`RenderGraph::ReleaseAliasPlacements`). It is the thread that places the
  graphs' transients, so the give-back is in order with every placement, as the hold made it before. Then it hands them to the
  runtime's deleter thread (`RetiredGraphTeardown`), which shuts down their task workers (the command-list pools' `CancelAndWait`)
  and destroys them. Extensions' `Shutdown` stays on the owner thread (no extension implements it). A release of all (shutdown,
  `DestroyGraph`) keeps the old path. The sync-host `destroyRetiredGraphsDetached` path is unchanged.
- **Workers are raised for waits (OMS `Executor::WaitBoost`).** While a thread waits on a scope of the executor (an
  `ExecutorScope`, or an ORG task scope of `ExecutorTaskService`), every worker runs at least at the waiter's priority, then goes
  back to its own. A running task is raised too, by thread handle (`ThreadPriority::thread`/`setThread`, set by CS's
  `Lanes.cpp` with `OpenThread`/`SetThreadPriority`). A worker's own raises (frame-critical jobs, ParallelFor helpers) go through
  the same per-worker lock, so neither undoes the other. ORG's `ITaskService::RaiseWorkersForWait` exposes it for waits outside a
  scope: `CommandListPool`'s up-to-20 ms wait for a background reset (a submission's) takes it.
- **Diagnostics.** The attach log names its submission drain and the host thread's hold. Each teardown logs `N replaced graph(s)
  torn down in X ms on the deleter thread (alias placements given back in Y ms on the host's thread)`.
- **ORG unit tests fixed (item 4).** Not the inversion. `Leases`/`LeasesOpen` failed with "Synchronous frame capacity exhausted":
  the test holds 24 runs' executions between seals, and a graph's `SynchronousAdmission` had a fixed capacity
  (`framesInFlight * 3 + 3`). Then "Admission unavailable" from `ExecutionTimelineAdmission::RecordSubmitted`, which recorded an
  execution already on the GPU against a fixed capacity and killed the host's thread. Both now grow with the runs in flight (the
  run leases already did); `Prepare` of a fresh execution keeps its bound (a test checks it). The 300 s hang was the failure's
  unwinding: `~GraphHost` waited in `StopAsync` for held executions that were never executed; the test now exits on a throw
  with executions held. `LeasesWide` also checked that a kept copy's run is `Leased()`, which a copy never is (`RunRef::Copy`
  has generation 0): fixed in the test. `OpenRenderGraphHeadlessBenchmark` never hung: it is CPU-bound, 5 runs of 11,000 frames,
  about 5 minutes in Debug with stdout buffered. It takes `--runs`/`--frames` now and ctest runs `--runs 1 --frames 1000`.
  All 50 ORG ctest tests pass (`out/build/vs-vulkan-tests` in this worktree, VS 18 2026, Debug); OMS `ORGAsyncExecutorTests`
  (with a new `TestWaitBoost`) and `ORGExecutorTaskServiceTests` pass.

## Measured

Before: the install lead's runs (its branch). After: this branch. The teleport run: guest+quick, coc Dragonsreach and back twice.
Load: the same with 16 normal-priority busy processes (the install lead's reproduction; it times out in its second load at the
engine's own job wait, as before, so it is a reproduction, not a pass).

| | attach on the render thread: median / p99 / max | teardown on the render thread | teardown elsewhere (deleter) median / p99 / max |
|---|---|---|---|
| before, normal (5 runs, 72 attaches) | 0.15 / 4.5 / 5.8 ms | 12 releases logged >= 10 ms: median 14.1, max 17.8 ms | none |
| after, normal (teleport, 20 attaches) | 0.24 / 3.8 / 3.8 ms | none (a message post) | 4.7 / 68.9 / 68.9 ms |
| before, load (9 attaches) | 2.6 / 10.7 / 10.7 ms | 4 logged: 18, 35, 104, 16 ms | none |
| after, load (18 attaches) | 0.20 / 4.6 / 4.6 ms | none | 3.5 / 37.4 / 37.4 ms |

The remaining attach wait is the host thread's hold (the submission drain is 0.001 ms): the time for the host's thread to finish
the ticket it is preparing and read the Detach. Host-thread alias give-back: median 0.006 ms, max 0.28 ms. Under load, no
replaced graph was held 100 ms or more after its attach (before: one held 27 s).

The final build's four runs (teleport, interior, native Vulkan, native D3D12; 60 attaches): attach median 0.22, p99 16.0, max
159 ms; deleter teardown median 4.6, max 414 ms (a load); alias give-back max 0.19 ms; no render-thread teardown. The 159 ms
attach (native D3D12, during the load) is all host-thread hold: the host's thread was in a staged graph's first recording (its
alias plan), and reads the Detach only after it. Other leads' runs today without this change have the same tail (44 runs:
attach p99 17.6 ms, one 3.2 s).

Steady window (frozen river, `trun.sh guest+quick`, Dev-Fast): `p3-teardown-after1` 2.842 ms a frame (other CS 0.782, ORG
epochs 0.767, engine 0.589, DCLF 0.557). Lead compute's captures on the same base without this change: 2.750 and 2.784 ms
(`p3-compute-t-fold2`, `-t-hw2`). No attach or release happens in that window; the one steady-state change is the per-worker
lock a ParallelFor helper takes when it raises itself. A baseline of my own failed (the director's stage has no Tracy zones).

## Test runs

| config | label | result |
|---|---|---|
| guest+quick + teleports (first build) | p3-teardown-teleport | passed, 5 shots |
| the same, 16 busy processes (first build) | p3-teardown-teleport-load | timeout at the engine's job wait (reproduction) |
| guest+quick + teleports | p3-teardown-teleport2 | passed, 5 shots |
| guest-interior | p3-teardown-interior | passed, 4 shots |
| native-vulkan+quick | p3-teardown-nvk | passed |
| native-d3d12-quick | p3-teardown-nd12 | passed |
| trun capture | p3-teardown-after1 | 2.842 ms window |
| ORG ctest (50 tests, Debug) | | all pass |
| OMS ORGAsyncExecutorTests, ORGExecutorTaskServiceTests | | pass |

## Decided by the agent, to confirm

- Every worker is raised during a wait, not only the ones running the waited scope's tasks: an ORG scope does not know which
  worker runs its tasks, and a queued task needs any worker to get CPU time. Waits are teardown-only or bounded (20 ms pool
  wait), so raising all is cheap.
- The admissions' capacities grow without a ceiling. Runs in flight are already bounded by the lease table; a leak would show
  there first.
- The attach keeps its `HoldAsync`. Removing it would mean letting the host's thread swap graphs itself (it reads
  `m_instances[...].graph` unlocked, and the submission thread's completions name epochs, not graphs), which overlaps lead
  `compute`'s submission-thread work. The drain is free now; the hold is up to about 4.5 ms.
- No baseline capture of the unchanged branch; see Measured. The after capture is 0.06-0.09 ms above lead compute's two
  captures (DCLF CPU +0.036), within the noise with other leads running; a back-to-back pair would settle it.

## Open items

- The attach's host-thread hold (p99 4.5 ms in the teleport runs, 159 ms once at a native D3D12 load behind a staged graph's
  first recording): to remove it, the host's thread would take the swap itself at its next message.
- DCLF's render-thread waits on executor jobs (`FrameValues`, `ShaderPrograms`, `SceneSet`) were sampled, not all audited. The
  sampled ones are frame-critical jobs, which already run raised. Any other render-thread wait on a below-normal job should use
  `Executor::BoostWorkersForWait()`.
- A retired graph whose tasks are still queued is now released late rather than waited for. Nothing waits on those tasks, so the
  boost does not reach them: under load such a graph's memory stays until the scheduler runs them.
