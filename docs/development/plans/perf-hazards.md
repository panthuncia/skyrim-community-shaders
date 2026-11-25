# Render thread: cross-frame hazards (plan item 2)

Lead `hazards`, 2026-11-25. Plan: `render-thread-window.md`, item 2. All timings are Dev-Fast (/Od), guest, the frozen river
scene, `trun.sh` + `buckets.py` (ORG self time per frame, 6 s Tracy captures).

## What the measurements showed

The plan's guess was wrong in one point. Every ticket already commits through a stream: no ticket commits fixed points, and no
prediction slot is reset in a steady scene (checked with temporary zones on every branch of `AdmitLedger`). The prediction
missed for other reasons:

- 5 of the 6 misses a frame compared a grid with only producer references. A grid's fingerprint covered its readers, and
  readers come and go every frame (a write clears them, the next read adds one back).
- The `sun` stage runs on about half the frames. Grids it reads are left in one of two states when the next frame's stages
  are admitted, so a single capture held only every other frame (`start`, `post`, `pre-depth`, `deferred+water`).
- `deferred+water` never had a capture at all: its batches wait on more than 64 sources, and the capture gave up there.
- The commit never found its cells unchanged as a whole: in most commits some grid changes every frame (a read after another
  stage's write), so a whole-commit fast path never fired.

## What changed (ORG `perf/hazards`, `BackingAccessAdmissionLedger` and `SynchronousAdmission::AdmitLedger`)

1. **Writer fingerprints.** A read gives waits only on its cells' writers. Each grid now keeps two fingerprints, the whole one
   and the writers' alone. A capture compares the writers' fingerprint for a resource its batches only read, and the whole one
   for a resource they write.
2. **Several captures.** A prediction slot keeps three access captures, most recent first. The one that holds moves to the
   front; a miss resolves in full and replaces the oldest.
3. **No source cap.** A capture lists each producer once per batch through a per-batch mark (an array indexed by producer),
   so large stages are captured too. Fixed points are still found by a search (there are few).
4. **Quiet grids in the commit.** A stream commit skips a grid when the stream's last commit (same graph, backings and batch
   count) left that grid unchanged and nothing changed it since (`AccessGrid::changedIn` against `Stream::lastCommit`). The
   walk would find the cells naming its producers already; only the producers' signals move. Every other change to a grid
   (other commits, fixed points, closing producers, resolving planned points) moves `changedIn`; a grid made again is never
   skipped. A retired grid is found again by key and made again, so it is walked.
5. **Repeated references.** Neighbouring cells mostly name the same access. The full resolution and the boundary access waits
   add a reference once when the cell before named it too.

Interface: `AccessPrediction::entries` is now `std::vector<AccessPrediction::Entry>` (adds `readers`); `PredictionSlot::access`
is a vector of captures. Both are used only by the ledger and `AdmitLedger`. No public ledger method changed.

## Measured effect (per frame, Dev-Fast)

| Zone | Baseline (`base3`) | After (`final2`) |
|---|---|---|
| `ResolveCrossFrameHazards` (full walks) | 0.361 (5.6/f) | 0.003 (0.1/f) |
| `PredictedCrossFrameHazards` | 0.037 (14.6/f) | 0.056 (19.5 tries/f) |
| `CommitCrossFrameHazards` | 0.552 | 0.277 |
| `ResolveBoundaryAccessWaits` | 0.022 | 0.017 |
| Hazard zones together | 0.97 | 0.35 |
| ORG epochs bucket | 3.87 | 3.04 |
| Window | 7.27 | 6.03 |

The machine was busier during both baseline captures (five leads share it): engine self time, which this change cannot
touch, read 1.45-1.55 ms there against 1.27-1.30 in both captures of the change. Scaling the baseline by that ratio, the
hazard zones save about 0.5 ms a frame (Dev-Fast), and the window about 0.4 ms. An earlier capture with the old waits (`diag1`: diagnostic zones and a whole-commit fast path that never fired, in a
quieter moment) put the hazard zones at 0.94. The two "after" captures agree
(6.06 and 6.03 ms windows).

The remaining commit cost is grids that do change every frame: a stage reads a cell that a later stage writes, so its read is
added back each frame (`start` 0.05, `post` 0.035, `deferred+water` 0.03). Each such walk is a real change.

## Tests

ORG unit tests (`tests/AsyncCompileTests.cpp`), built by hand against the Dev-Fast `OpenRenderGraph.lib`:

- quiet grids: a writer and a reader stream reach quiet commits; a fixed point on the same backing, another backing set under
  the same streams, and a retired grid all send the walk through again, with waits checked after every commit;
- the existing stream test, now counting the read side's earlier prediction;
- alternating frames: a reader on odd frames only; each state's capture holds again and its waits equal a full resolution;
- boundary access waits name the writer and the reader at their latest signals.

`OpenRenderGraphPersistentTests` passes too.

Game runs (stage `p-hazards`):

| Config | Label | Result |
|---|---|---|
| `guest+quick` | `p-hazards-t-guest-quick` | passed |
| `native-vulkan+quick` | `p-hazards-t-native-vulkan-quick` | passed |
| `native-d3d12-quick` | `p-hazards-t-native-d3d12-quick` | passed |
| `guest+quick`, coc Dragonsreach and back | `p-hazards-t-teleport` | shots fine (fails only on the config's missing `start` shot) |
| `native-vulkan+quick`, same steps | `p-hazards-t-teleport-nvk` | before and interior fine; exterior foggy (see below) |
| `native-vulkan+quick`, same steps, baseline DLL | `p-hazards-base-teleport-nvk` | all fine, exterior clear |
| `native-vulkan+quick`, same steps (repeats) | `p-hazards-t-teleport-nvk3`, `-nvk4` | never-loaded: from 05:28 every lead's run stopped at the loading screen (stray saves), not this change |

Both native teleport runs loaded a different save than the usual river one (a mountain at level 1; the same start in both).
The foggy exterior after `coc Whiterun` looks like a fog weather (distance-graded, near objects clearer), which a cell change
can roll at random. A missed hazard would show stale or corrupt texels, not graded fog. The guest teleport's exterior was clear.

## Decided by the agent, to confirm

- The foggy native exterior is weather, not this change. A repeat was blocked by the save problem; repeat
  `native-vulkan+quick` with the teleport steps (and `fw 81a` after the coc) once loading works again.

- Three captures per prediction slot: two cover a stage that runs on alternate frames, one spare. Costs a fingerprint check
  per extra capture tried (19.5 tries for 14.5 tickets).
- No switch: the old paths are gone. These waits guard GPU hazards, so the first merge should run the usual validation configs.

## Open items

- `CommitStates` (0.20, 108 calls) and `ApplyFrameRebindings` (0.13) are fixed costs per batch and per ticket rather than
  per-access walks. Caching the rebinding checks needs the shared rebinding vector's identity (an interface change in
  `PrepareAtSubmission`, which the `tickets` lead is working near), so it was left.
