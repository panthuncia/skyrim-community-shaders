# snap3-spin: CS executor workers spinning in tbb's queue

Branch `snap3/spin` (worktree `org-port-worktrees/snap3-spin`), based on org_integration 95e32ae99. Stage `s7-spin`.
OMS branch `snap3/spin` inside `extern/ORGModuleServices`.

## 1. What spun, and why

The hang dump from the snap3-jobhang notes (`1126-154452-dir-...cadence-drop-cells`) and its text report show all 14
`CS worker` threads in

```
KERNELBASE!SwitchToThread <- tbb::detail::d0::spin_wait_until_eq <- micro_queue<Executor::State::Job>::pop
  <- internal_try_pop_impl <- org::async::Executor::State::Run
```

with 17-22 s of kernel time and 5-10 s of user time each (cdb `!runaway`). Those are the shared executor's parallel workers
(`RenderGraph::Frame::SharedExecutor`, `src/RenderGraph/Async/Lanes.cpp`; the pool is OMS's `org::async::Executor`).
By the time the minidump was written they had gone back to their semaphore (`WaitOnAddress`), so this is a burn during
the slow phase, not a deadlock.

Idle workers already block: `Executor::State::Run` takes a semaphore token before each pop. The spin is inside the pop.
`tbb::concurrent_queue::try_pop` claims a ticket first and then waits, with `pause` and then `SwitchToThread` forever,
for the earlier ticket on the same micro-queue to finish (`spin_wait_until_eq(head_counter, k)`), and for the pusher of
its own ticket to finish writing (`spin_wait_while_eq(tail_counter, k)`). Pushes wait the same way for earlier pushers.
The executor's workers run at mixed priorities: below normal by default, raised to above normal by WaitBoost, by
frame-critical jobs and by ParallelFor helpers of an above-normal caller. So when a below-normal worker that holds an
earlier ticket (or is mid-push of a continuation) is preempted on a busy machine, raised workers spin-yield on it.
`SwitchToThread` does not hand the core to a lower-priority thread, so the holder stays starved (until Windows'
starvation boost, every few seconds) and the spinners take CPU from the game's own threads and the other lab game.

Reproduced outside the game (`scratchpad/spin/bench/spinbench.cpp`: 14 workers at alternating below/above normal, a
producer pushing bursts of 14 jobs of 20 us, each job pushing 3 continuations, 16 normal-priority busy threads beside):

| queue | jobs done in 8 s | workers' user / kernel CPU |
|---|---|---|
| tbb::concurrent_queue | 331 (pool stalled) | 2.44 s / 17.28 s |
| InboxQueue | 91 000 | 2.25 s / 0.30 s |
| tbb (second run) | 186 854 | 4.45 s / 4.27 s |
| InboxQueue (second run) | 140 280 | 3.03 s / 0.33 s |

The first tbb run is the dump's shape: the pool nearly stops and the workers burn kernel time in `SwitchToThread`.

## 2. The fix

`extern/ORGModuleServices/include/ORGModuleServices/Async/InboxQueue.h` (new): an unbounded MPMC FIFO. A push links a
node onto a lock-free inbox (one CAS loop; never waits for another thread). A pop takes a mutex, and with nothing taken
yet swaps the whole inbox out and reverses it into push order. Consumers that meet block on the mutex (SRW lock: a
short spin, then a kernel wait) instead of spinning forever. Nothing waits for a preempted pusher.

Users switched from `tbb::concurrent_queue`:
- OMS `src/Async/Executor.cpp`: each domain's ring and frame-critical queue (the 14 spinning workers).
- `src/Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.cpp`: the build pump's two queues (several runners pop at once).
- `src/Features/DrawcallLimitFix/Core/Render/Snapshot/ViewBatchesInternal.h`: the batch storage pool's free list.
- `src/RenderGraph/RenderGraphRuntime.cpp`: the compute submitter's batches (one consumer; its push could still spin
  behind a preempted earlier pusher).

Files touched: those four, the new header, and `extern/ORGModuleServices/tests/Async/AsyncPrimitivesTests.cpp`
(`TestInboxQueue`: 4 producers and 4 consumers, every item once, each producer's items in order; leftover items freed).
OMS's `ORGAsyncPrimitivesTests` and `ORGAsyncExecutorTests` pass (built standalone against the worktree's vcpkg TBB).

Not changed: OMS's StateGraph queues (`Detail/StateGraphImplementation.h`) are drained by one serialized pump each, so
no popper waits for another popper; the remaining `std::this_thread::yield` loops in CS and ORG are short drains inside
work, not idle waits.

Decided by the agent, to confirm: InboxQueue lives in OMS (beside the executor) and CS includes it, rather than a copy
in CS. The OMS pointer moves (OMS has `update = none`: the main checkout needs `git -C extern/ORGModuleServices checkout`
of the new sha after the merge).

## 3. Measurements (Dev-Fast, /Od)

Per-thread CPU from `scratchpad/spin/threadcpu.py` (GetThreadTimes once a second, by thread name; no suspends).

| run | CS workers' CPU (user / kernel) |
|---|---|
| base, main menu idle 27 s (window 25-51 s) | 0.19 / 0.00 s (0.7% of a core) |
| fix, main menu idle 26 s (window 25-51 s) | 0.02 / 0.00 s (0.1% of a core) |
| base, native-vulkan+cadence-drop-cells (whole run, 47 s) | 15.74 / 1.61 s |
| fix, native-vulkan+cadence-drop-cells (45 s) | 15.34 / 1.50 s |
| base, native-vulkan+perf-dragonsreach (42 s) | 18.16 / 2.53 s |
| fix, native-vulkan+perf-dragonsreach (40 s) | 17.73 / 2.64 s |

Idle workers were already blocking on the quiet machine in both builds: the spin needs the busy-machine preemption above,
which the lab runs did not reproduce on demand; the benchmark does.

Frame time between the perf marks (exclusive runs):

| config | base | fix |
|---|---|---|
| native-vulkan+perf-dragonsreach (CPU-heavy interior, 600 frames) | 5.36 ms (186.7 fps) | 5.34 ms (187.4 fps) |
| native-vulkan+perf-quick (exterior, 900 frames) | 6.65 ms (150.3 fps) | 6.54 ms (152.8 fps) |

## 4. Runs

Stage `s7-spin`. Base = 95e32ae99 unchanged; fix = 54d4eca90.

- Base: native-vulkan+menu-main with a 1800-frame idle step (`s7-spin-base-menuidle`), native-vulkan+cadence-drop-cells
  (`s7-spin-base-dropcells`), native-vulkan+perf-quick, native-vulkan+perf-dragonsreach: all passed.
- Fix, passed: guest+quick, native-vulkan+quick, native-d3d12-quick, native-vulkan+native-tour, native-vulkan+menu-scenes,
  native-vulkan+cadence-drop-cells, native-vulkan+perf-dragonsreach, native-vulkan+perf-quick (`s7-spin-fix-perf-quick2`),
  the idle menu (`s7-spin-fix-menuidle2`), and native-vulkan+quick under a CPU hog (`s7-spin-fix-hog-quick`).
- Fix, in a slow phase (section 5): `s7-spin-fix-menuidle` (never-loaded) and `s7-spin-fix-perf-quick` (passed at
  56.6 fps, not used for the frame-time table). Both overlapped another lead's build.
- Detector numbers: not changed by this item (queue code only); live engine reads 629 (base) and 655 (fix) per world frame
  in drop-cells, 497-499 in Dragonsreach.

## 5. The slow phases themselves

Two of the fix runs had a slow phase (`s7-spin-fix-menuidle`: never-loaded, data-loaded at 143.7 s; `s7-spin-fix-perf-quick`:
loads at 2-5 fps). Both overlapped another lead's build (snap5-menus3 finished 17:24:52, the main checkout 17:27:53); the
fix runs right after them were normal. The process itself was mostly idle in them (22% and 79% of one core over the run),
and the CS workers spun no more than in a normal run (1.3 s kernel).

Reproduced with a CPU hog (`bench/hog.exe 16 90`: 16 normal-priority busy threads for 90 s from launch), fix build,
native-vulkan+quick: data-loaded at 124.3 s instead of about 17 s, then normal the moment the hog ended. The whole process
got 6 s of CPU in those 90 s, 5.8 s of it the main thread. The time went into Streamline's initialisation (40 s), Vulkan
device creation and the pipeline cache load, before CS's executor has much to do. So the slow phases are the machine:
a build at normal priority on every core starves the driver, Streamline and the game's start-up, not something CS's queue
does. The executor spin made them worse (it adds busy threads at above-normal priority), and that part is gone.

Open item: CS's parallel workers run below normal and its blocking workers at lowest, so under a build they get CPU only
from the starvation boost. WaitBoost covers scope waits; other waits on them (artifact readiness during loads) are not
covered. Not changed here (it is a frame-time trade-off, not a spin).
