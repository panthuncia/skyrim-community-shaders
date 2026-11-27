# post-perf: is the render-thread route fully asynchronous? (roadmap "After phase 7", item 2)

Lead `post-perf`, branch `post/perf` (base `org_integration` 980320095), stage `s7-post-perf`. Inputs: decisions 213 and 226-232,
[snap7-thread.md](snap7-thread.md), [snap7-pace.md](snap7-pace.md), [snap7-devloss2.md](snap7-devloss2.md).

RTX 5080, Ryzen 7 9800X3D. Lab slots render at 1920x1080 (the slot INI), so "4K" below means `prefs_ini.Display` set to 3840x2160.
Timings are Dev-Fast (/Od) unless a table says optimized (section 5). One sample per row.

## 1. Verdict

In routed world frames the two threads run independently, with three exceptions, two of them fixed here:

- **The game thread does not wait on the render thread** in steady play. In Dragonsreach (sampled at 1 ms for 8 s) it spends
  87.4% in the update-rate cap's wait; every other wait is under 0.1% (no Renderer::Lock wait, no park, no job wait on render work).
  Its update rate holds the cap whatever the render side does: 30, 60 and 240 updates/s at 149, 146 and 138 fps; 60 updates/s at
  58 fps (4K, GPU-bound, fewer renders than updates) and at 85 fps (frame limit).
- **The render thread does not wait on the game thread.** It renders the newest snapshot or repeats the current one; it never sleeps
  waiting for a publish. Its rate follows the GPU, not the game: 146-149 fps at 1080p for any update cap, 137-138 fps with the game
  thread saturated (40 extra actors, 190 updates/s), 58 fps at 4K.
- **Fixed: Reflex slept on the game thread** (section 4.2). The frame limit paced game updates, not renders, so an 85 fps limit left
  146 fps; the game thread waited for the render side's pacing.
- **Fixed for after the interim wait: the present's pacing ran under Renderer::Lock** (section 4.3). Today the interim wait absorbs
  it; with the wait removed the lock was held 49-67% of the time, up to 49 ms at once, which game-side engine code would wait for.
- **Not fixed, interim by decision 229: `WaitForLastRender`.** It is 46-77% of the render thread's time, but removing it changes
  no frame rate here (section 3): the GPU is the limit. It does serialize CPU and GPU work, so it would cost frames where the render
  thread's CPU time matters: native D3D12 is 11% faster in an optimized build despite the wait (section 5).
- **Open: intermittent game-thread collapses** to about 2 updates/s for up to 100 s while the render side keeps 25-58 fps
  (2 of 23 runs, section 6). Not diagnosed; the likely coupling is DCLF's animated fade hand-off (section 7, D-3).

So: asynchronous in steady state, with one known game-thread wait on a render-side job that only bites when the render side stalls.

## 2. Tools

- Counters on the render thread's stats line (one line per 600 game updates, `[ORG native] routed updates: ...`): renders of fresh
  and repeated snapshots, render halves' total time, Renderer::Lock held (total, longest), publish to adoption and to the render's
  end (average, longest), and waits: update cap, frame jobs, Reflex sleep, Renderer::Lock, last render's GPU work. Tracy zones on
  the same waits (`CS.Native.RenderLock`, `CS.Native.WaitForLastRender`, `CS.Native.GameFrame.JobWait`, `CS.Streamline.ReflexSleep`;
  CS zones need TRACY_SUPPORT; Dev-Fast captures only ORG zones). Commit 2374ba7f4.
- The no-admin stack sampler (`perf-tools/sampler.py`) on the game thread and the render thread; scratchpad scripts `prun.sh`
  (run plus Tracy or samples after the start mark), `wrun.sh` (samples when the update rate dips), `wt.py` (wait attribution),
  `rl.sh` (the marked window's lines), `rtid.py` (the render thread's id by its description).

## 3. Measurements (Dev-Fast, native Vulkan unless named)

Per 10 s window between the marks. "Wait" columns are the render thread's share of its time. Game cap wait is the game thread's.

| Run | Config | fps | updates/s | renders fresh / repeat | last-render wait | lock held (longest) | game cap wait | publish to adopt / end (ms) |
|---|---|---|---|---|---|---|---|---|
| a-taa | perf-taa, cap 60 | 146 | 60.0 | 600 / 860 | 61% | - | 84% | 3.4 / 9.8 |
| b-cap30 | cap 30 | 148.8 | 30.0 | 600 / 2376 (20 s) | 66% | - | 91% | 3.7 / 9.8 |
| b-cap240 | cap 240 | 138 | 240 | 346 / 0 (2.5 s) | 48% | - | 54% | 2.5 / 9.3 |
| c-clones | cap 240, 40 player copies | 137 | 190 | 436 / 1 (3.2 s) | 45% | - | 0% (saturated) | 3.1 / 9.9 |
| b-ssgi | SSGI on | 146 | 60 | 600 / 858 | 62% | - | 84% | 3.6 / 9.7 |
| d-4k | 4K | 58.5 | 60.0 | 583 / 1 | 77% | 9% (14.5) | 85% | 9.1 / 25.7 |
| c-dr | perf-dragonsreach | 225 | 60.0 | 600 / 1657 | 46% | - | 86% | 2.6 / 6.5 |
| c-d3d12 | native D3D12 perf-taa | 85.6 | 60.0 | 600 / 256 | 75% | - | 87% | 6.1 / 17.3 |
| c-reflexcap | Reflex on, limit 85 (before the fix) | 146 | 60 | 600 / 863 | 62% | - | 84% | 3.7 / 9.7 |
| d-reflexcap | same, after the fix | 85.0 | 60.0 | 600 / 249 | 1% (Reflex sleep 74%) | 9% (14.9) | 86% | 6.2 / 8.9 |

Without the interim wait (experiment, not committed; it loses the device at times, as decision 229 says):

| Run | Config | fps | updates/s | lock held (longest) | notes |
|---|---|---|---|---|---|
| e-taa | perf-taa | 146 | 60.0 | 56% (11.4) | same rate as with the wait |
| e-4k | 4K | 58.4 | 60.0 | 67% (48.7) | |
| e-dr | dragonsreach | 225 | 60.0 | 49% (10.6) | |
| f-taa | perf-taa, present paced before the lock | 146 | 60.0 | 13% (11.4) | fix 4.3 |
| f-dr | dragonsreach, same | 224 | 60.0 | 19% (10.8) | fix 4.3 |

Readings:
- The render rate does not move with the update cap (149 / 146 / 138 fps at 30 / 60 / 240) or with a game thread three times
  busier (clones). The 6% at 240 is the cost of a fresh adoption per render (no repeats).
- The update rate does not move with the render rate: 60.0 at 58 fps (4K), 85 fps (limit), 146 fps, 225 fps.
- At 1080p, 4x the pixels costs 2.5x the frame time and SSGI changes nothing: these scenes are GPU-bound at 1080p too, so the
  interim wait's serialization costs no frame rate at these settings.
- Repeats: 1.4-2.8 renders per update at cap 60. At cap 240, 42% of updates are never rendered (newest wins), as designed.
- Publish to the render's end (the CPU side of latency) is 6.5-9.8 ms at 1080p, about one render plus half a render of waiting for
  the adoption; the queued present adds at most one frame (CS.Native.PresentPace keeps the render thread one present ahead).

## 4. Fixes

### 4.1 Streamline SimFrameId repeats (50298e087)

Rule, from the publish side: every game update takes its own simulation frame id at its input poll, at least the next render
frame's id and above the last update's (`SimFrameId`, cached per update in a small ring keyed by snapshot id). The render frame that
first renders an update's snapshot takes that id while it is still ahead (`BeginRenderFrame`), so the update's SimulationStart and
SimulationEnd and its render's markers share one frame. Repeats take the next id. Before, two publishes before one adoption both
took `LastRenderFrame() + 1`: two SimulationStart markers for one frame. Checked with native-vulkan+upscalers (section 8).

### 4.2 Reflex sleeps on the render thread (a0a356105)

`slReflexSleep` ran at the game thread's input poll (Hooks.cpp's input hook). With the render thread it paced game updates: an 85 fps
limit (`frameRateLimitDivisor = 2` on the 170 Hz display, Reflex on) left renders at 146 fps, and the game thread slept on the render
side's pacing. Now `Streamline::ReflexSleepRender` runs on the render thread before each routed render's adoption, with the id of the
frame it starts; the input poll sleeps only while the render thread is not running (guest, parked). After: 85.0 fps, 60.0 updates/s,
the render thread spends 74% in Reflex's sleep and 1% in the interim wait. Low-latency mode now delays the adoption, so a render
takes the newest snapshot when it wakes (publish to adoption 6.2 ms, publish to the render's end 8.9 ms).

### 4.3 The present's pacing before Renderer::Lock (b5e5d4d9f)

The render half takes Renderer::Lock around volumetric lighting, the UI replay and Renderer::End. End's present waits for the
previous present (`CS.Native.PresentPace`) and for the frame latency's GPU work (`CS.Native.FrameLatencyWait`). Today the interim wait
has already waited for the GPU, so these are short; without it (snap7-devloss3) they move under the lock: held 56% (perf-taa), 67%
(4K, up to 48.7 ms at once) and 49% (dragonsreach). snap7-pace measured 25% of the game thread waiting for this lock in BGSDecalManager
when the lock was held long. `Native::PacePresent()` now does the present's pacing before the render half takes the lock
(`NativeDevice::PaceQueuedPresent`, shared with `QueuePresent`). After, without the interim wait: held 13% (perf-taa), 19%
(dragonsreach), frame rates unchanged. The longest hold stays about 11 ms (an occasional long End; not traced).

## 5. Optimized build confirmation

The Release preset does not build per file on this base (section 9), so the optimized numbers come from the Dev-Fast preset with
/O2 /Ob2, `SC_DEVFAST_OPTS=OFF` and Tracy support on (`build/Opt`, no LTCG). Tracy's on-demand capture recorded only 123 zones in that
build (startup only), so the counters and the sampler carry the numbers.

| Run | Config | fps (Dev-Fast) | updates/s | last-render wait | render CPU per render | lock held (longest) | game cap wait | publish to adopt / end (ms) |
|---|---|---|---|---|---|---|---|---|
| o-taa | perf-taa | 146 (146) | 60.0 | 81% | 1.3 ms (2.7) | 6% (6.2) | 90% | 3.5 / 9.9 |
| o-cap240 | cap 240 | 138 (138) | 240 | 74% | 1.9 ms | 11% (3.1) | 69% | 2.2 / 9.3 |
| o-4k | 4K | 58.5 (58.5) | 60.0 | 90% | 1.8 ms | 4% (6.2) | 90% | 8.5 / 25.4 |
| o-dr | dragonsreach, sampled | 216 (225, unsampled) | 60.0 | 73% | 1.2 ms | 9% (6.2) | 90% | 2.4 / 6.8 |
| o-d3d12 | native D3D12 perf-taa | 94.8 (85.6) | 60.0 | 87% | 1.4 ms | 5% (5.7) | 91% | 5.6 / 15.8 |

The ranking holds: native Vulkan is GPU-bound at the same frame rates as Dev-Fast; the render thread's own CPU is 1.2-1.9 ms a render
and the rest is the interim wait. On native D3D12 the optimized build is 11% faster, so there the render thread's CPU time shows
through the interim wait's CPU/GPU serialization; removing the wait (snap7-devloss3) should help D3D12 first. Optimized samples
(dragonsreach, 8 s at 1 ms): the game thread waits 91.8% in the cap, 0.2% in the game frame's UI job wait, nothing else; the render
thread spends about 53% inside `WaitForLastRender` (NVIDIA's host wait spins, so half of it samples as busy) and 0.7% in ORG's
`WaitTicket`, with no wait on game-side work.

## 6. Open: game-thread collapses

Two runs (d-reflexcap at 15:17:32, f-4k at 15:33:55) dropped to 1-3 game updates/s for 60-100 s while the render thread kept 25-58 fps
of repeats, then recovered. Both follow a `GraphHost: epoch N has no published copy to submit (EpochCopiesBusyError)` warning, which
also appears at startup in every run, so it is not specific. Builds with and without the interim wait both showed it; 21 other runs
did not. Three watched 4K runs (`wrun.sh`, sampling once the rate dips) did not reproduce it: one crashed in ORG's memory
introspection on a staged-build job (flecs, `GraphHost` -> `MemoryIntrospection`, build E, no interim wait), one lost the device
(no interim wait), one passed. snap7-thread's open item 6 saw similar single-window dips (44, 41, 10 updates/s). Suspect: DCLF's
animated fade hand-off (D-3): the game thread's `WriteFadeWriters` waits in `WaitAnimatedDrain` until the render side's fade
producer job has taken the batch, so a stalled render side stalls the game.

## 7. Design decisions recorded for later

- D-1. **Port Renderer::Begin's and End's bodies (TH-b).** The render thread still takes Renderer::Lock twice per render; with 4.3 it
  holds it 0.5 ms on average but up to 11-15 ms at once in End. Porting the bodies removes the engine lock from the render side and
  the last place game-side engine code can wait for the render thread.
- D-2. **Repeats and the render rate.** The render thread never idles: with nothing new it repeats the snapshot (1.4-2.8 per update
  at 60 updates/s; 2,376 per 20 s at cap 30). A repeat shows the same game state, burns the GPU and makes the next fresh snapshot
  wait (publish to adoption 2.5-9 ms is mostly a repeat finishing). Options: cap renders at the display's rate when no frame
  generation needs them; Reflex's low-latency sleep already delays the adoption when it is on (4.2). Keep the never-wait rule.
- D-3. **DCLF's animated fade batch is shared between the halves.** The engine's animation jobs push into one queue that the render
  side's producer drains; the game thread waits for that drain at its next update (`WaitAnimatedDrain`, TH-h). Normally 0.02% of the
  game thread, but it is a game-thread wait on render-side work. Proposed: the game half drains the queue into its ticket at its end
  (as decision 227 did for the other DCLF state) and the render side reads the ticket's copy; no hand-off flag.
- D-4. **Non-world frame kinds on the render thread** (TH-a): menus over the world, the map and the loading screen render on their
  producer with the render thread parked. Parks were 0 in every perf window here; they matter at route changes only (see
  snap7-thread's frame kinds table). Moving them needs the UI producer's inline interface (`interfaceInline`) ported first.
- D-5. **Submission thread.** ORG's submit thread runs 39% of its time on ticket submission in perf-taa (Tracy, ORG zones). It is
  not the limit while the GPU is, but after the interim wait goes it is the next serial stage between the render thread and the GPU.
- D-6. **The interim wait's cost** goes with snap7-devloss3: today it hides the GPU-bound time inside the depth commit, so all the
  render thread's GPU waiting shows as "last render's GPU work" rather than present pacing.

## 8. Commits and runs

| Commit | What |
|---|---|
| 2374ba7f4 | chore(native): count both threads' waits, latency (stats line counters, Tracy zones) |
| 50298e087 | fix(upscaling): one Reflex simulation id per update (4.1) |
| a0a356105 | fix(native): Reflex sleeps on the render thread (4.2) |
| b5e5d4d9f | fix(native): pace the present before the lock (4.3) |
| a9141ca1a | fix(dclf): headers and names for per-file builds (section 9) |

No submodule changes.

Measurement runs (stage s7-post-perf, labels `s7-post-perf-<run>`): a-taa, b-cap30, b-cap240, b-1080, b-ssgi, c-dr, c-reflex,
c-reflexcap, c-clones, c-d3d12, d-reflexcap, d-4k, d-sample (Dev-Fast, interim wait on); e-taa, e-4k, e-dr, e-reflexcap, f-taa,
f-4k, f-dr (interim wait off, experiment); o-taa, o-cap240, o-4k, o-dr, o-d3d12 (optimized). Invalid: b-1080 equals the default
(the slot already runs 1080p), b-1080-cap240 and o-cap30 stayed in a paused menu after the load hold released with no progress
(Main::DrawFrame path at 25-40 fps), g-watch1 crashed in ORG memory introspection and g-watch2 lost the device (both interim wait off).

Checks on the final build (b5e5d4d9f + a9141ca1a, Dev-Fast):

| Label | Config | Result |
|---|---|---|
| h-quick | native-vulkan+quick | passed |
| h-upscalers | native-vulkan+upscalers, Reflex on, cap 240 | passed (Reflex low-latency on the render thread, no Streamline marker errors) |
| h-d3d12-quick | native-d3d12-quick | passed (NativeDevice changed) |
| h-guest-quick | guest+quick | passed (Streamline.cpp is shared) |
| h-reflexcap | native-vulkan+perf-taa, Reflex on, limit 85 | passed, 85.0 fps, 60.0 updates/s |

Skipped: cadence-drop-cells (no cadence code touched), window-resize-stress-taa (snap7-devloss3's area).

## 9. Open items

- The game-thread collapses (section 6) and D-3, the animated fade hand-off, which is the likely path.
- `WaitForLastRender` (snap7-devloss3): with it gone, check that the render thread's lock hold stays near f-taa's 13% and that
  D3D12 gains (section 5).
- The longest Renderer::Lock hold is still 6-17 ms at times (End or the UI replay); not traced. D-1 removes it.
- Per-file Release build: `ShadowEpochs.cpp` still fails (`ShadowCommit::payload` resolves to a type without `arena`, `ShadowInputs`
  without `addresses`); the unity Dev-Fast build hides it. Found while building the optimized confirmation.
- Tracy CS zones in a TRACY_SUPPORT build captured only startup zones (123 zones in 6 s); ORG zones in Dev-Fast work.
- Runs that stay paused after a load hold releases with no progress (2 of about 30 runs): the world renders on Main::DrawFrame and
  perf numbers are invalid. cs-test could flag `paused true` after world-ready.
