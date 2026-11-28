# post-fades: the last game-thread waits on the render side (D-3 and the update collapses)

Lead `post-fades`, branch `post/fades` (base `org_integration` 6c55b6ad4, with post/perf merged), stage `s7-post-fades`.
Inputs: [post-perf.md](post-perf.md) (D-3, section 6), [snap7-thread2.md](snap7-thread2.md), decision 227.
RTX 5080. Timings are Dev-Fast (/Od). Lab slots render at 1920x1080; "4K" is `prefs_ini.Display` set to 3840x2160.

## 1. Commits

| Commit | What |
|---|---|
| c2da635f7 | fix(dclf): animated fade batch in the ticket (D-3) |
| b6809f68f | test(cs-test): no-progress outcome |
| 0df84126c | chore(dclf): log long waits on the frame values chain (diagnostic, section 3) |

No submodule changes.

## 2. D-3: the animated fade batch is a ticket copy

Before: the animation jobs pushed fade updates into one queue. The CPU fade stage's kick (render side) set a hand-off flag, and its
producer job drained the queue. The game thread waited at Main::Update's entry (`WaitAnimatedDrain`) until that producer ran. The
game half also drained the same queue before the list jobs (`PrepareAnimatedFades`), under a lock shared with the render side.
A repeat render's kick could drain the next update's pushes.

Now:
- The game half drains the queue into its frame ticket: `TicketPart::AnimatedFades`, an immutable `AnimatedBatch` (roots, inputs,
  a serial). Only the game thread drains the queue.
- The render side reads the rendered ticket's batch (`TakeAnimatedBatch`). The first reader in a frame takes it: the CPU fade
  stage's kick on CPU frames, FadeStateCS's commit on GPU-driven frames (as before, where the kick's hand-off left the commit an
  empty batch). A repeat of the same ticket takes nothing (serial check), so its updates step once.
- A ticket no render took folds its batch into the next ticket (`RegisterTicketPart` takes an optional fold; `TicketSuperseded`
  applies it on the game thread before the fresh ticket is published). This matches the main fade step, which counts every update
  a snapshot stands for. The older batch's inputs stand for all, as within one update. Before, a skipped update's batch was dropped
  by the next drain.
- The producer reads the batch in place (shared pointer), so the copy stays off the render thread.
- Removed: the hand-off flag and its wait, the producer-side drain, `stampsLock`, the pre-counted stamps, and the stand-in observers'
  `beforeListJobsWait` / `listJobsDone` callbacks (fades was their only user).
- Late updates pushed during the list jobs (guest; "nothing expected" there) now go to the next ticket instead of this frame's commit.

Decided by the agent, to confirm:
- FadeStateCS's commit now counts the batch on the render side (`CountAnimatedRoots`); before, guest counted it before the list jobs'
  wait. It is a short loop over about 1,400 roots. Why: the stamps must match the render's tables, and a folded batch spans tickets.
- Folded batches keep the older update's inputs and do not set `varied` (that flag means within one update, and would log every fold).

## 3. The game-thread collapse

**Not fixed by D-3.** It reproduces with the ticket batch: native-tour (c-native-tour) collapsed twice in one run (17:24:14-17:24:30
at 1.0-1.5 updates/s with renders at 107-123 fps, and 17:25:41-17:26:11 at 1.4-2.8 updates/s), and f-4k-4 collapsed inside its
marked window (19:15:32 on, 0.4-1.8 updates/s for over 60 s, renders 45-50 fps). On the base build it came once in 13 runs (b-4k-3,
33 s at 1.8-3.2 updates/s right after world-ready).

What the game thread waits for (sampled 6 s at 1 ms, both builds): 56-58% in the engine's frame jobs (`MainTick::WaitForFrameJobs`),
25-30% in `RenderThread::GameFrame`'s wait for the UI job task (68167 / 69516). No sample in `WaitAnimatedDrain` on the base build,
none in any fade code after. So the game thread waits on the engine's job threads, not on CS's render side directly.

What the job threads do (every thread's stack, `CommunityShadersCrashDump.exe --pid`, f-4k-4 during the collapse): one engine job
thread runs `Job_Actor_movement` (AE 0x1406d2600); another, inside `Actor::Move` -> the character controller's on-ground state
(`bhkCharacterStateOnGround::Unk_08`) -> FUN_140e84230, sleeps in `BSReadWriteLock::LockForRead` on the Havok world lock
(bhkWorld + 0xc598, taken when `bUseWorldLock_HAVOK`, AE 0x14202c810). The other job threads are idle. The render thread is in
the interim `WaitForLastRender`, nothing else of CS's is in engine code. So some thread holds the Havok world's write lock for long
stretches (the lock word's high bit; a reader sleeps 1 ms per retry after 10,000 yields), and no thread in the snapshot shows it
inside Havok code. The holder is not identified yet: the next step is a minidump during a collapse and the lock's writer thread id
(the first dword of the lock).

Ruled out:
- D-3 (above).
- The frame values chain. The engine's cell animation jobs wait for the frame values' sampling job (`WindowWriters::BeginWriter` ->
  `FrameValues::WaitSampled`), which waits its turn behind the render side's upload jobs; an upload waits inside its turn for ORG's
  submission thread (`GpuPoint::Resolve`). Commit 0df84126c logs any of these waits over 100 ms. None was logged in 22 runs on that
  build, collapse included.
- CS code taking the Havok world lock: CS never calls bhkWorld's lock functions.

Both post-perf collapses and two of these started 1-10 s after world-ready, near `EpochCopiesBusyError` warnings; those warnings
also come in runs without a collapse (e-tour-2 had 7), so they are a sign of a loaded start, not the cause. Lab load matters: the
reproductions came while other leads' games ran in the second slot.

## 4. No-progress runs

post-perf's two invalid runs (b-1080-cap240, o-cap30) rendered every frame on Main::DrawFrame after world-ready: o-cap30 stayed
paused (a menu open; the load hold had released "no progress" and kept the Mist Menu's fade-out), b-1080-cap240 rendered on the
game thread unpaused. Both show the same thing: the render thread never ran after world-ready. The test runner now tracks that
(`Testing::WorldNeverRouted`: set at world-ready in native mode, cleared at the first lifecycle tick with
`RenderThread::Running()`, reset at a load) and reports `no-progress` instead of `passed` when the config has a `mark` step.
Configs without a mark (menu tests that keep a menu open) are not affected. Why the menu stayed is not found; it follows a load hold
that released with no progress (materials still waiting after 7 s), which these runs did not reproduce. About 45 marked runs here
passed, none was flagged.

## 5. Measurements and runs (Dev-Fast, stage s7-post-fades, labels `s7-post-fades-<label>`)

D-3, game thread at steady state (4K perf-taa, 8 s at 1 ms after the mark): before (a-4k) 87.3% in the update cap's wait,
0 of 4,606 samples in `WaitAnimatedDrain`; after (c-4k) 87.1% in the cap's wait, no fade wait left (the function is gone). Rates
unchanged: 57.6 / 56.7 fps at 60.0 / 59.9 updates/s before, 54.3-58.3 fps at 59.9-60.1 after; the routed stats line shows 565 vs
578 fresh renders and 0 repeats per 600 updates, publish to the render's end 25.8 vs 25.5 ms. The steady-state wait was already near
zero (post-perf: 0.02%); the change removes the path, so a stalled render side can no longer stall the game at Main::Update's entry.

| Build | Labels | Config | Result |
|---|---|---|---|
| base 6c55b6ad4 | a-4k | native-vulkan+perf-taa 4K, sampled | passed |
| base | b-4k-1..6, b-rc-1..6 | perf-taa 4K; perf-taa Reflex on, limit 85 | 12 passed; b-4k-3 collapsed (33 s) |
| fix b6809f68f | c-nvk-quick | native-vulkan+quick | passed |
| fix | c-nvk-cadence-drop-cells | native-vulkan+cadence-drop-cells | passed (shots match the director's run but for lighting) |
| fix | c-nvk-first-person-switch | native-vulkan+first-person-switch | passed |
| fix | c-native-tour | native-tour | passed; collapsed twice |
| fix | c-nvk-update-cap30, c-nvk-update-cap240 | update-cap30 / 240 (repeats, skipped updates) | passed |
| fix | c-guest-quick | guest+quick (shared fade code) | passed |
| fix | c-4k | perf-taa 4K, sampled | passed |
| fix | d-4k-1..8, d-rc-1..8 | as b | 16 passed, no collapse |
| fix | e-tour-1..5 | native-tour | 4 passed; e-tour-1 lost the device |
| diag 0df84126c | f-4k-1..4, f-tour-1..4 | as b, native-tour | passed; f-4k-4 collapsed (dumps) |
| diag | g-4k-1..7, g-tour-1..7 | as b, native-tour | passed but: g-tour-1 lost the device, g-4k-7 crashed |

Native-tour's device losses (e-tour-1, g-tour-1: "Timeline::HostWait", around the Dragonsreach coc) also happen on the director's
post-perf base (1127-162642 dir-pperf native-tour), so they are not from this branch (snap7-devloss3's area).

g-4k-7 crashed on the game thread in the game half: `GameFrameStart` -> the water port's frame capture (`WaterPortCapture.cpp:113`,
`WaterDraws::CanTake`) -> `FrameCamera::Main` -> `Snapshot::Core()` -> `Snapshot::Current()`, the render owner's adopted snapshot,
read from the game thread while the render thread adopts (null read at 0x210). The same stack crashed post-refs' ui-menus run
(1127-173428), so it is not from this branch.

Skipped: native-d3d12-quick (no ORG, BasicRHI or device change).

## 6. Decisions to confirm and open items

Decided by the agent, to confirm:
- The animated batch is a ticket part with a fold (section 2); ticket parts may now fold on supersede (`TicketPartFold`).
- The render side counts the animated batch at the commit; folds keep the older inputs and do not set `varied`.
- The frame values chain's long-wait log stays as a diagnostic (no switch; at most one line a second; it never fired).
- `no-progress` applies only to native runs with a `mark` step.

Proposed decision-log entry: D-3 done: the animated fade batch is the game half's ticket copy (TicketPart::AnimatedFades), read
once per ticket by the render side and folded into the next ticket when skipped; no game-thread wait on render-side fade work is
left. The update collapse is not that hand-off: the game thread waits on the engine's frame jobs, where an actor movement job waits
for the Havok world's read lock.

Open:
- The collapse: find the Havok world lock's writer during a collapse (minidump; the lock's first dword is the writer's thread id).
  2 collapses in 51 runs after the fix (c-native-tour, f-4k-4), more under lab load. The watcher that dumps on a low update rate is
  `frun4.sh` in the scratchpad (post-fades).
- Why a paused menu stayed after a "no progress" load hold release (section 4).
- Native-tour's intermittent device loss at the Dragonsreach coc (not this branch).
- The water port's game-half capture reads the render owner's snapshot (`FrameCamera::Main` from `WaterPortCapture.cpp:113`): a
  cross-thread race that crashes (g-4k-7, post-refs ui-menus).
