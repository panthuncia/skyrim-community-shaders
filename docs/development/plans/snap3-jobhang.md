# snap3-jobhang: the drop-cells "hang" in WaitForFrameJobs

Branch `snap3/jobhang` (worktree `org-port-worktrees/snap3-jobhang`), based on org_integration 00d95ac1d. Stage `s7-jobhang`.

## 1. What the dumps show

Both dumps are minidumps (no heap), so the engine's counters were read from registers and from the threads, and the call
sites were decoded in Ghidra (AE 1.6.1170). The hang report's SkyrimSE names are wrong; the absolute addresses (and cdb's
`SkyrimSE+offset` frames) are right.

**The wait.** `JobListManager::WaitForFrameJobs` (AE 35409, 0x1405FC210) waits on the serving thread's done event (+0x60)
while its state (+0x6C) is 1. The serving thread (`ServingThread::Func2`, AE 0x1405FC870) runs the seven frame job lists in
order and sets that event after the last. In dump 2 it waits at +0x112 on list **5** (rsi=5 of rbp=7 lists): "Main render
end", whose table (AE 0x142011CF0) holds `Job_Post_process` (AE 0x1406D34B0).

**The job.** `Job_Post_process` runs `FUN_1406479E0`, the only caller of the task queue SKSE's task interface hooks. In dump 2
its thread (cdb 87, tid 82224) runs the test runner's SKSE task: `Testing::ConsoleCommand` -> `Script::CompileAndRun` ->
`coc Riverwood` -> `CenterOnCell_Impl` (AE 0x140730250), which always calls `FUN_1407304F0` (the pending teleport, which
Main::Update otherwise runs at its start, before the frame jobs) -> TES cell load (`FUN_14019DC90`) -> `IOManager` wait for
priority-4 work (`FUN_140E01800(io, 4)`, a Sleep loop). At the dump 9058 model-loader items were counted at the start
(r15=0x2362) and 589 were left (esi, the model loader's pending-count callback `FUN_140193FC0`). The loader's
BackgroundProcessThread was working (in a queue lock at report time, in CS code at dump time). Every other engine worker was
idle. So no counter is stuck and no list is corrupted: list 5's job is a whole cell load.

**Why nothing presents.** A coc inside the frame jobs blocks Main::Update's frame-job wait for the whole load. The engine's
loading screen can't help: `ServingThread::SetLoadingScreenActive(true)` (AE 0x1405FC050) only switches the serving thread to
the loading screen (state 2) when it is idle (state 0), and here it is in state 1, waiting for list 5. CS still logs "loading
screen: the serving thread renders until the load ends" (from the job thread), but nothing renders. The console never does
this: it runs while a menu is open, when Main::Update starts no frame jobs and processes UI messages on the main thread.

**Dump 1 is the same event.** Its log: `coc Riverwood` at 15:03:06.3, the hang report started at 15:03:22 (15.7 s later),
and the load ended at 15:03:27.7 (21.4 s). The game presented again afterwards (frames 1301-1303) while the report was still
being written (62.9 s), so the thread stacks in that report and dump are mostly from after the load. That is why they show
cell-animation jobs and a quest job running rather than the coc.

**Why it shows up now.** A Riverwood coc takes 5.3-6.4 s in every passing drop-cells run today. In the three hangs it took
34.4 s (s7-dclf, 14:32, before snap3-facegen merged), 21.4 s and more than 17 s. Those runs were in a slow phase (render
0.3-1 fps, game 1-3 updates/s). The slow phase shows up in the concurrent slot's run at the same time (s7-menus at 14:33, dir
guest+quick at 15:03), and in about 30 runs today in all modes, guest included. The process was mostly idle in it (dump 1: main
thread 24 s of user time in 136 s). So the machine was the slow part (two games and up to three builds at once), not a
lock. Face tint bakes are not the cause: the 14:32 hang predates them.

## 2. The fix

`src/Testing/Commands.cpp`: the runner's `AddTask` no longer uses SKSE's task interface. It queues onto its own list, which
`RunTasks` drains on the main thread at `RenderGraph::Engine::MainTick` (after Main::Update's frame-job wait). There the
engine is on one thread and the serving thread is idle, so a coc runs as the console's does: the main thread loads, and the
serving thread draws the loading screen and presents. `Testing::InstallTaskTick` registers the tick at kPostPostLoad in test
runs only; a failed install ends the run as failed. Every runner command that queued game work (console, cast, mouse, camera,
draw, place, morph, move, turn) now runs there. Non-test runs and the product are unchanged.

Decided by the agent, to confirm: the fix is in the test runner, not the engine path. Nothing in CS's frame was waiting
wrongly. A coc from a frame job is something only the runner did; the game's own teleports go through the pending
teleport at the start of Main::Update.

## 3. Water and other capture-side engine work

This hang was not engine work in the capture. For snap3-water: the capture runs inside Main::DrawFrame, beside the update's
frame jobs (lists 0-6: cell animations, Havok, actor and VM updates, the post-process tasks), as the engine's render half
did. `TESWaterSystem::Update` in the capture keeps the engine's concurrency as long as it runs where the engine's own call
ran (inside DrawFrame). It needs no reordering unless it lets go of engine references or edits the cell-animation or
dynamic-node lists. Those go through the retire queue's engine lane, which drains at the same MainTick (decision 139).

## 4. Runs

Stage `s7-jobhang`, build aeb453248 (Dev-Fast).

- native-vulkan+cadence-drop-cells, labels `s7-jobhang-dropcells-1..9`: 1, 2 and 4-9 passed. Runs 4-9 are six passes in
  a row. Run 3 was never-loaded: the first save load stalled before any step ran (DCLF load hold "no progress", 6199
  materials waiting; the slow phase), so the change was not involved.
- Every coc now logs its loading screen from the main thread, and the serving thread presents during the load (54.6 fps
  logged from it in the middle of the Riverwood load). Riverwood coc load: 5.8-6.4 s across the passing runs.
- guest+quick, native-vulkan+quick, native-d3d12-quick, native-vulkan+facegen (labels `s7-jobhang-<config>`): passed.
- Detector numbers: not changed by this item (test runner only); live engine reads in drop-cells were 603-648 per world frame.

## 5. Open items

- The slow phase itself (0.3-1 fps for minutes after a coc when the machine is busy) is worth a look on a quiet machine:
  dump 2 shows all 14 CS executor workers spinning in tbb's `micro_queue::pop` (12 of them about 27 s of CPU each, mostly
  kernel time in SwitchToThread). That executor spin may make the contention worse.
