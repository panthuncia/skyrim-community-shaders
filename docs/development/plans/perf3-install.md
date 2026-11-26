# Round 3 lead `install`: graph installs no longer wait in StopFrameProduction (2026-11-25)

CS branch `perf3/install` on org_integration f6b0d91de. ORG branch `perf3/install` on 122d052. OMS branch `perf3/install` on
ded0e73 (OMS has `update = none`: check it out by hand when merging). No BasicRHI or DXVK changes. Timings are Dev-Fast (/Od).

## Root cause of the 35-41 s attaches

An attach replaced each epoch's graph with `InstallGraph`, which called `old->StopFrameProduction()` on the render thread. That
joined three task scopes of the old graph: its worker builds, its frame-worker tasks, and its ownership retirements (each a
task that drops what a finished frame owned). All of them run on CS's shared executor, whose parallel workers
(`CS worker N`, `RenderGraph/Async/Lanes.cpp`) run at `THREAD_PRIORITY_BELOW_NORMAL`. The render thread runs at normal
priority. So this was a priority inversion: on a machine whose cores are busy with normal-priority threads (three lab games,
builds, the engine's own job threads during a load), a below-normal worker gets no CPU time except the Windows starvation
boost, about once every 4 s. An attach of ten graphs waits for many such tasks, so it took 35-41 s, and finished on its own.

That explains the hang report: every executor worker "idle" in the snapshot. A thread that its semaphore has released, but
that the scheduler has not run yet, shows the same wait stack as an idle one. The log agrees: during the 36 s wait one worker
logged a build at 17:04:47, 11 s in, which is a boosted quantum. The same cause makes the p99 of 70 ms in normal runs.

## Change

- **ORG** `RenderGraph::CancelFrameProduction` (never blocks): marks production stopped, cancels a running structural build's
  flag, and drops the queued builds and frame-worker tasks. Retirements are never dropped. `FrameProductionBusy` names the
  scopes that still have tasks (`ITaskScope::Idle`, new, never blocks). `StopFrameProduction` is Cancel plus the old waits, for
  teardown paths only (DestroyGraph, ShutdownTaskWorkers, failure paths).
- **ORG** `GraphHost::RetireGraph` replaces the wait in `InstallGraph` and `RetireStaged`: it cancels and lists the graph.
  `ReleaseRetiredGraphs` releases a retired graph only once its GPU work is done (as before) and its scopes are idle. Both only
  move forward once the graph is retired: nothing calls the old graph after the attach, and a task that submits a follow-up
  enters the scope before it leaves. So the teardown (extensions, task workers, alias placements given back, destruction) still
  runs after everything the old graph's in-flight work touches, and nothing is freed or placed again early. A release of all
  (DestroyGraph) still waits.
- **ORG** diagnostics: `GraphHost: epoch N's replaced graph is held X ms after its attach ...` names the busy scopes once a
  GPU-done graph has waited 100 ms; `... released X ms after its attach, once its tasks drained` follows it; `released N
  replaced graph(s) in X ms` when a release takes 10 ms or more.
- **OMS** `CountingScope::Idle`.
- **CS** `Revision.cpp` `Advance`: a revision with staged builds waits while `GraphHost::RetiredGraphCount()` is 64 or more,
  so rapid revisions cannot pile up unbounded retired graphs. Frames keep the adopted revision meanwhile.

## Measured

Attach wait on the render thread (`attached ... in X ms` log lines; the old wait was inside them):

| runs | attaches | median | p99 | max |
|---|---|---|---|---|
| before: every 2026-11-25 run folder (481 runs) | 5,683 | 0.23 ms | 76 ms | 40,529 ms (two over 35 s) |
| after: this branch's 7 runs | 93 | 0.16 ms | 9.2 ms | 10.7 ms |

The reproduction: `p3-install-teleport-load`, the teleport run with 16 normal-priority busy processes beside the game (all
16 cores busy, as with three lab games and builds). The game ran at a few frames a second. Every attach took 10.7 ms or less,
while one replaced graph (epoch 18) was released **27,197 ms** after its attach, once its frame-worker and ownership-retirement
tasks got CPU time: the old code would have held the render thread for those 27 s. The run timed out (300 s) in its second
load, the render thread in the engine's shadow-map job wait, not in CS: the machine was simply too slow for the harness. It
counts as the reproduction, not as a pass. In the same run a release of ten retired graphs took 104 ms (open item).

Steady window (frozen river, `trun.sh guest+quick`, Dev-Fast): `p3-install-after1` 2.810 ms a frame (ORG epochs 0.769, other
CS 0.767, engine 0.607, DCLF 0.521); round 2 finish's `p2-finish-after3` was 2.670 ms (ORG epochs 0.726, other CS 0.747).
No attach happens in that window, so the 0.14 ms is between sessions under other leads' load, not this change.

## Test runs

| config | label | result |
|---|---|---|
| guest+quick, coc Dragonsreach and back twice | p3-install-teleport | passed, 5 shots |
| the same with 16 busy processes | p3-install-teleport-load | timeout (reproduction, above) |
| guest+quick | p3-install-guest | passed |
| guest-interior | p3-install-interior | passed, 4 shots |
| native-vulkan+quick | p3-install-nvk | passed |
| native-d3d12-quick | p3-install-nd12 | passed |
| trun capture | p3-install-after1 | 2.810 ms window |

ORG's and OMS's unit tests were not built (the test task services gained `Idle` and compile with the CS build's headers only
by inspection).

## Decided by the agent, to confirm

- `ITaskScope::Idle` is a new pure virtual in ORG's task interface, so OMS changes with it (one line), on a branch. Tracking
  the scopes' tasks inside ORG instead would have duplicated OMS's counting scope.
- The cap of 64 retired graphs (about three revisions of every epoch) before revisions wait. It never triggered in the runs.
- No own baseline timing capture of the unchanged branch: the change cannot touch the frozen steady window (no attach happens
  there, and `ReleaseRetiredGraphs` returns at once with no retired graph). The after capture is compared with round 2's
  finish captures instead.

## Open items

- The attach still holds the async host thread (`HoldAsync`, which also drains the submission thread). Those are normal and
  above-normal threads, so they are short, but they are waits on the render thread.
- A retired graph's teardown runs on the render thread inside `HoldAsync` (`ORG.Host.ReleaseRetiredGraph`, 10-14 ms for ten
  graphs at a teleport). Its command-list pools and publication coordinator join their own scopes there
  (`CancelAndWait`), which can hit the same priority inversion if a task of theirs is running. Moving the teardown to the
  deleter thread (as `destroyRetiredGraphsDetached` does for synchronous hosts) needs the async host thread's services made
  safe for it.
- The root inversion stays for anything else the render thread joins on below-normal workers. Raising a worker that holds a
  task a normal-priority thread waits for (as ParallelFor's helpers are raised) would cover the remaining joins.
