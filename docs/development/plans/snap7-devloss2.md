# snap7-devloss2: the device loss on the native render thread, round 2

Lead `devloss2`, round `snap7`, branch `snap7/devloss2` (base `org_integration` 45f386c64, with snap7-devloss's interim wait),
stage `s7-devloss2`. Continues [snap7-devloss.md](snap7-devloss.md). Dev-Fast throughout. RTX 5080, native Vulkan.

Status: **not fixed.** The write is not named and the interim wait stays. This round ruled out most of the candidates the first
round listed, showed the fault is a GPU-side overlap and not a CPU write, and tied it to the resize transition that decision 94
already reported before the render thread existed. No code change is committed; only these notes.

## 1. Commits

| Repo | Branch | Commit | What |
|---|---|---|---|
| CS | snap7/devloss2 | see report | docs: snap7-devloss2 notes |

No submodule change. The experiment patches are kept outside the repo (section 6).

## 2. Baseline

Without the interim wait, window-resize-stress-taa lost the device in every run this round (11 of 11 baseline-like runs). The
first round saw about one in three; the machine or the build now makes it reliable, which made the bisection below cheap. The
faults are the same: an invalid read at a low address (0x5840000 again, 0x4e80000, 0x5494000) or an instruction-pointer fault,
with the graphics queue's last checkpoint in `cs.dclf.main-opaque` and `cs.dclf.cull-counters` begun. The CPU pipeline
(`CS_DCLF_PIPELINE=cpu`) loses the device too, so it is not the GPU-driven path alone.

## 3. What was ruled out (each without the interim wait)

| Suspect | How | Result |
|---|---|---|
| Latch regions written while their run is in flight (suspect 2 of round 1) | ORG hook in every `LatchBlock` write path (`Write`, `Bytes`, `Zero`, `ZeroStableRun`, `CopyStableRun`), checked against the host's lease state | never fired in 2 lost runs |
| The retirement clock | detector: a render's end point passed by the clock before its GPU point was reached | fired: the clock counts host tickets only, so it passes points whose external work (native context, present copies) still runs. Made exact (own points, each with its GPU point): 0 detections, still lost 2 of 2 |
| Frame-value backings reused while a render reads them | detector at `LeasePool` | fired (renders 5 behind); with binds moved to the end of the render it stopped firing, still lost 2 of 2 |
| Queue-scoped uploads | `queueScopedUploads = false` | lost |
| Merged ticket submissions | `mergeTicketSubmissions = false` | lost |
| DCLF async compute and uploads on another queue | DCLF compute passes on graphics and `queueScopedUploads = false` together | lost 3 of 3 |
| Replaced graphs (and their alias placements) released early | replaced graphs held 3 s after their attach | lost |
| Fade states and tree wind ping-pong (render parity) | reasoning plus the CPU-wait data below | not consistent with the data |
| DGC preprocess state lists keyed by lease | read: the spill lives in the executing list's scratch, the state list is a record-time snapshot | not a GPU-time read |

## 4. What the data says

- **It is GPU-side.** Making each render's GPU work wait on the GPU for the last one's (the frame-value producer's copies wait for
  the previous render's end point; every batch waits for the producer's signal) passed 2 of 2 with the CPU never waiting. A CPU
  write could not be fixed that way.
- **Any CPU wait in render N+1 for render N's GPU work helps, wherever it is.** Waits at the depth commit's parts (after the frame
  record, after the frame slots, after the streams, at its end), at the colour commit (3 of 3), at the colour commit's end, before
  the present (2 of 2) and after the present (2 of 2) all passed; waits right at the UI stage's commit and at DCLF's EndFrame passed
  1 of 2. Round 1's finer bisection of the depth commit was noise: the write is not in that window.
- **More overlap, more losses.** GPU serialization at distance 1 passed 2 of 2; at distance 2 (render N+2 waits for N) 5 of 6; at
  distance 3 0 of 1; at distance 4 0 of 2. Waiting on the last present's sealed point instead (the candidate fix, section 5) passed
  4 of 5. The interim CPU wait passed 12 of 13 in round 1. So every mitigation lowers the rate, none removes it.
- **It sits in the resize transition.** In the lost run with the sealed-point fix, the GPU stopped inside the window between the
  stages' new-size builds attaching (frame revision 29: "alias placements given back", two frame alias arena blocks freed) and
  DCLF adopting its new-size main resources: DCLF still draws its old-size scene against the new-size stages. That is the open
  item of decision 94 ("an intermittent GPU hang in `cs.dclf.main-opaque` ... in the ~250 ms after a resize while DCLF still draws
  its old-size scene", 3 of 10 resize runs, single-threaded). The render thread makes it frequent because it renders many more
  frames in that window, repeats included, with a deeper GPU queue.

## 5. The candidate fix that was tried (not committed)

`FrameValues::Kick` on the native render thread: the producer's uploads also wait for `Retirement::SealedGpuPoint()`, so every
batch of the render starts on the GPU after the last present's work. The render thread waits for nothing. It passed 4 of 5
(fix-resize4 lost the device inside the resize window). Not committed: it is a mitigation like the interim wait, and less proven
than it (decided by the agent, to confirm).

## 6. Experiment patches (outside the repo)

`C:\Users\matth\AppData\Local\Temp\claude\f5be53fdb6b18c23\c77789dc-39f2-41f3-80a1-db34faa4e8e2\scratchpad\devloss2\`:
`experiments-cs.diff` (detectors, the exact retirement clock, flag-file wait points and GPU serialization) and
`experiments-org.diff` (the `LatchBlock` write hook and `GraphHost::RunInFlight`). A flag file there switched the experiments
without rebuilds.

## 7. Next step

Instrument the resize transition, not the steady state:
1. Log, per render between a stage revision's attach and DCLF's adoption of new main resources, which target versions DCLF's
   recordings bind and which the stages wrote, and whether any DCLF read is of a version the revision replaced.
2. Check the frame alias arena around that attach: which reservations the replaced stage builds gave back, and whether any of them
   held a resource DCLF's old-size graph still reads (frame transients span DCLF's positions: "they bind the G-buffer from the
   Z-prepass to first person").
3. A decisive test: hold DCLF's frames off the new stage builds until DCLF adopts (or the reverse: attach the stages' new builds
   only with DCLF's). If the losses stop with no wait, the transition is the cause.

## 8. The director's D3D12 sample

The interim wait sits in DCLF's depth commit on the render thread, so it covers D3D12 too. The grey-land start shot
(1127-114515) has no device loss or tap error in its log; flat land with floating rocks looks like the land's draws read zeroed
rows at the start, not this overlap. Not investigated further. native-d3d12-quick on the unchanged base passed (section 9).

## 9. Runs (stage s7-devloss2, native-vulkan+window-resize-stress-taa unless named)

| Labels | Build | Result |
|---|---|---|
| a-detect1, b-lag1 | latch and clock detectors, no wait | lost 2 (clock detector fired, latch never) |
| c-exact1, d-exact2 | exact clock | lost 2, clock detector silent |
| e-noqs1, f-nomerge1 | no queue-scoped uploads; no merged submissions | lost 2 |
| g-record1, h-w_slots, h-w_streams, i-w_cmpend, i-w_colour, k-w_colourend | CPU wait at that point | passed 6 |
| j-none1 | no wait | lost |
| k-w_endframe-1, l-w_endframe-2 | wait at DCLF EndFrame | passed 1, lost 1 |
| l-w_end-2, m-w_end | wait before the present | passed 2 |
| m-w_colour x2 | wait at the colour commit | passed 2 |
| n-w_postui x2 | wait after the UI stage's commit | passed 1, lost 1 |
| n-w_postpresent x2 | wait after the present | passed 2 |
| o-cpu1 | `CS_DCLF_PIPELINE=cpu` | lost |
| p-gpuser x2 | GPU serialization, distance 1 | passed 2 |
| p-noshared | no shared alias arena | crashed while loading (unrelated) |
| q-gpuser2 x2, s-scoped x2, w-gpusergpd2 x2 | GPU serialization, distance 2 (scoped: only frame-value readers wait) | passed 5, lost 1 |
| r-gpd4 x2, r-gpd3 | distance 4, 3 | lost 3 |
| t-fvdetect | frame-value reuse detector | lost; detector fired |
| u-latebind x2 | binds retire at the render's end | lost 2, detector silent |
| v-sigwait, w-sigwait x2 | batches serialized, producer copies not | passed 3, lost 1 |
| x-gfxnoqs x3 | DCLF compute on graphics, no queue-scoped uploads | lost 3 |
| fix-resize1..5 | sealed-point serialization (section 5) | passed 4, lost 1 |
| y-graphhold1 | base without the wait (the hold patch did not apply) | lost |
| z-graphhold1 | replaced graphs held 3 s | lost |
| base-d3d12quick | native-d3d12-quick, unchanged base | passed |

## 10. Decisions to confirm

- DV2-a. Keep the interim wait (DL-a) until the transition is fixed; the sealed-point GPU serialization is not committed.
- DV2-b. The retirement clock counts only host tickets, so it can pass a point whose external work is still running. Not changed
  here because it did not cause this loss; worth a fix of its own (open item).

## 11. Open items

- The cause: the resize transition (section 7).
- The retirement clock's coverage of external work (DV2-b). The exact clock in `experiments-cs.diff` is one way: each point keeps
  the GPU point of everything on the queues when it was taken.
- Frame-value backings are reused up to five renders back while those renders' post-EndFrame work still runs; harmless here, but
  binds at DCLF's EndFrame do not cover work submitted after it.
