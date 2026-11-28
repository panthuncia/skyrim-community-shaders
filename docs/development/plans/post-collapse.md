# post-collapse: the game-update collapses

Lead `post-collapse`, branch `post/collapse` (base `org_integration` fd0dfd703, post-fades merged), stage `s7-post-collapse`.
Inputs: [post-fades.md](post-fades.md) section 3, [post-perf.md](post-perf.md) section 6. RTX 5080, 16 logical CPUs. Timings are
Dev-Fast (/Od).

## 1. Result

The collapse is not a CS bug and not render-thread specific. It is CPU starvation: other processes kept every CPU busy (other
leads' parallel builds, other lab games compiling shaders), and under that load Skyrim's game update falls to 2-4 updates a second
in every mode: native, guest and plain d3d11 alike. Native keeps rendering repeats at 50-80 fps, so there it shows as "updates
collapse while the render thread renders". In guest and d3d11 the frame rate falls with the updates.

Fix: the test harness runs the game in the above-normal priority class, and reports when other processes saturated the CPUs.
A below-normal CPU load of the same size leaves native at 60 updates/s and 145 fps, and so does a normal-priority load beside a
game in the above-normal class (60 updates/s, 137-142 fps; 2.9 CPUs for the game instead of 0.8-1.2).

## 2. Commits

| Commit | What |
|---|---|
| 9d83b045e | test(cs-test): report CPUs saturated by others |
| 8f183e7b3 | test(cs-test): run the game above normal priority |
| (this) | docs: post-collapse notes |

No CS code changes, no submodule changes.

## 3. How it was found

### 3.1 The Havok lock was a red herring

post-fades' all-thread dump caught an actor movement job sleeping in `BSReadWriteLock::LockForRead` on the Havok world lock
(bhkWorld+0xc598). A watcher reading the lock from outside every few microseconds (`lockwatch.py`, ReadProcessMemory only, no
suspends; the lock is { writer thread id, count }, count's high bit set while write-held) showed:
- normal play: write-held 2-3% of the time, about 1,000 short holds a second, by the engine's job threads;
- during a collapse (b-tour-3): write-held 0.1-1%, 20-200 holds a second. Nobody held it for long.

The reader in the dump was one of many acquisitions caught in its sleep, not a blocked one.

### 3.2 Every engine thread was idle or crawling

During collapses the engine's job threads sat 93% in their idle wait (`BSJobs::JobThread::Func2`), the job list serving thread waited
on a job list's done event, the main thread waited on the frame jobs (`JobListManager::WaitForFrameJobs`, 51-56%) and the UI jobs
(`WaitForJobTask(0, 2)` in GameFrame, 13-30%). Different dumps showed different engine spin locks sleeping: the Havok lock,
TaskQueueInterface's lock (0x1431993b0, in `UpdateManagedNodesJob`), BSResource archive locks. Nothing was stuck; everything ran
slowly.

### 3.3 The CPUs were saturated by other processes

Per-thread CPU at the start of d-tour-8's collapse (`threadcpu.py`): system 100% busy, this game 0.79 of a CPU, the other lab game
0.21: about 15 CPUs went to other processes. Compile times of every build folder (`.obj` mtimes) against every run of post-fades and
post-collapse (126 runs with a world phase, `corr.py`):

| | collapsed | no collapse |
|---|---|---|
| a build compiled during the world phase | 2 (b-tour-3: 362 objects; d-tour-8: 38) | 0 |
| no build found | 3 | 121 |

The three collapses without a build (16:41, 17:24, 19:15) came from worktrees deleted since, or other loads (another game's cold
shader compile used 11 CPUs in one sample). Collapses start and end with the load; one ended at a coc (d-tour-8) by chance.

### 3.4 A synthetic load reproduces it every time

`burn.py` keeps 16 CPUs busy for 40 s, started 4 s after world-ready (perf-taa, 1080p, exclusive slot):

| Mode | Burner priority | During the burn | After |
|---|---|---|---|
| native Vulkan (f-nvk-burn16) | normal | 1.9-3.8 updates/s, 47-78 fps | 56-60 updates/s, 145 fps |
| guest (f-guest-burn16) | normal | 3.3-4.2 updates/s = fps | 104 fps |
| d3d11, no render graph (f-d3d11-burn16) | normal | 2.6-3.3 updates/s = fps | 40-44 fps |
| native Vulkan (f-nvk-burnlow2) | below normal | 60 updates/s, 145 fps | same |
| native Vulkan, game above normal (g-nvk-burn16-prio2) | normal | 60 updates/s, 137-142 fps | same |

The game got 0.8-1.2 CPUs under a normal-priority burner and 2.9 CPUs under a below-normal one. A frame of the engine is a long chain
of hand-offs (main thread, the job serving thread, job threads, events back), and spin locks that yield (`Sleep(0)`,
`SwitchToThread`) give the CPU away to the burner's threads for a whole quantum each time. A single busy thread (the render thread)
keeps its share; the chain does not.

## 4. Is it render-thread specific?

No. Guest and d3d11 collapse the same way (3.5 and 3 updates/s). Native's update rate under the burn (2.8) is a little lower than
guest's: the render thread's repeats take about 19% of the CPU the process gets then (169 of 890 ms/s). That is roadmap item D-2
(repeats never idle the render thread), not the cause.

## 5. The fix

- cs-test runs the game in the above-normal priority class (`procs.raise_priority`, from the watch loop at the first poll that sees
  the game). Decided by the agent, to confirm: it changes every run's scheduling, but only matters when the machine is saturated; a
  player's foreground game gets a similar edge from Windows' foreground boost, and lab games run unfocused. Both lab games get the
  same class, so they still share fairly.
- cs-test counts the seconds in the world with the CPUs saturated by other processes (system 95% busy or more, others on half the
  CPUs or more), the most CPUs others used, and the build tools running then (`cpuload.py`). `result.json` has `cpu_load`; five
  seconds or more add a clause to the summary line.
- Proposed for the director (not in this branch): run builds below normal priority too (`build-wt.cmd`: `start /belownormal /wait
  /b cmake --build ...`), so a build never takes the CPUs from a game in any slot.

## 6. Runs (stage s7-post-collapse, labels `s7-post-collapse-<label>`)

| Labels | Config | Result |
|---|---|---|
| a-4k-1 | native-vulkan+perf-taa 4K | passed (lock watcher not reading yet: ASLR base) |
| b-4k-1..3, b-tour-1..3 | perf-taa 4K, native-tour | passed; b-tour-3 collapsed 20:40:47-20:42:10 (a 362-object build ran) |
| c-4k-1..10, c-tour-1..10 | as b | passed, no collapse |
| d-tour-1..8, d-4k-1..7 | as b | passed but d-tour-2 lost the device (Timeline::HostWait, snap7-devloss's area); d-tour-8 collapsed 22:17:45-22:18:08 (3 builds ran) |
| f-nvk-burn16, f-guest-burn16, f-d3d11-burn16 | native, guest, d3d11 perf-taa with the burner | passed; all collapsed during the burn |
| f-nvk-burnlow | native perf-taa, burner meant below normal | passed; collapsed (the burner's priority call failed, still normal) |
| f-nvk-burnlow2 | native perf-taa, burner below normal | passed; no collapse |
| g-nvk-burn16 | native perf-taa, normal burner, this branch's cs-test | not-run (the worktree had no lab helper DLL; copied from the main checkout) |
| g-nvk-burn16-prio2 | native perf-taa, normal burner, this branch's cs-test | passed; no collapse; the summary line named 20 saturated seconds |
| h-nvk-quick | native-vulkan+quick, this branch's cs-test | passed (game above normal priority) |

Skipped: guest+quick and native-d3d12-quick (no CS, ORG or BasicRHI change; the change is the harness's).

## 7. Tools (scratchpad `post-collapse/`)

`crun.sh` (one run; lock watcher from world-ready; at a collapse: per-process and per-thread CPU, an all-thread stack sample, a
dump), `lockwatch.py`, `sampleall.py` + `allstacks.py` (every thread's stacks, engine frames named through Ghidra), `threadcpu.py`,
`topproc.py`, `burn.py` (`low` for below normal), `corr.py` (collapses against compile times).

## 8. Open items and proposed decision-log entry

Proposed entry: The game-update collapses (post-perf, post-fades) are CPU starvation by other processes (parallel builds, other lab
games), not a CS or render-thread bug: a 16-CPU load drops native, guest and d3d11 alike to 2-4 updates/s; native only keeps
rendering. cs-test now runs the game above normal priority and reports CPU saturation in the summary line.

Open:
- D-2 (repeats never idle the render thread) costs the game about a fifth of its CPU when the machine is saturated.
- d-tour-2's device loss at the Dragonsreach coc (snap7-devloss's area).
