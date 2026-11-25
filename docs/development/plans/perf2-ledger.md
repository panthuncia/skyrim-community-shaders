# Render thread round 2: the admission ledgers (leads `ledger`, `ledger2`)

Lead `ledger`, finished by `ledger2`, 2026-11-25. Brief: `BRIEF2.md`; round 1's notes: `perf-hazards.md`. All timings are Dev-Fast (/Od), guest, the
frozen river scene, `trun.sh` + `buckets.py`, per frame.

## What the cross-frame hazard tracking is for

Each compiled graph has its barriers and in-graph waits fixed at compile. What it can't know is which earlier executions
(other graphs, earlier frames, other queues) last wrote or read the backings it touches. The access ledger
(`BackingAccessAdmissionLedger`) keeps that per backing and subresource cell, and turns it into timeline waits at admission.
"Cross-frame" really means cross-execution. Waits on the consumer's own timeline are dropped (queue order and the entry
barriers cover them), so with one graphics queue almost all of it ends in no GPU wait: in the river scene there are 2 real
waits a frame, both from DCLF's async compute.

Its cost was not the waits but keeping the cells current. A cell kept the latest writer and the reads since. A cell written by
two producers in one frame with a read between (W1, R, W2) changed writer and lost its readers twice a frame, so every commit
walked its cells and the grids never went quiet.

## What changed (ORG `perf2/ledger`)

### Access ledger: cells that converge

- A cell keeps every producer that ever wrote it and every one that read it (a stream's batch once; fixed points as the latest
  per timeline). Nothing is taken out, so once each producer has touched the cell it stops changing. A steady frame changes no
  cell at all.
- A read waits for all the cell's writers, a write for all its writers and readers.
- A stream commit whose stream already walked the same graph, backing set and batches (and no grid was erased since: the
  table's generation) only moves its producers' signals: O(batches), no cell walk. Fixed points elsewhere don't defeat it:
  nothing but an erase takes an open producer out of a cell.
- Grids carry change stamps (`changedIn`, `writersChangedIn`) instead of fingerprints, and the ledger a change count. A capture
  taken (or last checked) at the current count holds with one comparison; otherwise it holds while none of its grids' stamps
  moved. Since cells only grow, a capture that misses never holds again, so an admission keeps one capture, not three.
- A capture leaves out sources on the consumer's own timeline (99.9% of them in the river scene: 332 per admission down to
  0.16). The capture keeps each batch's consumer timeline and misses if it changes.
- Growth is bounded: when a walk adds a new reference to a cell (a new stream after a rebuild), closed producers' references in
  that cell fold into points, so a cell holds the open producers plus a point per timeline. `FixClosedProducers` still folds
  everything after 1024 closures.

Why the extra waits are safe. The waits are a superset of the old rule's (latest writer plus the reads since). Each extra
reference is an older write or read. The latest writer already waited for it (write after write, write after read) when it was
admitted after that access committed, so the wait is implied, or it is on the consumer's own timeline and dropped. Either way it
names submitted work (a reference enters a cell at commit), so it can't deadlock. A producer's latest signal is always one at
which it touched the cell, because a stream touches the same cells every time (SynchronousAdmission opens a new stream for a new
backing set). Measured: 2.0 cross-queue waits a frame before and after (the `CrossFrameHazardWaits` plot over the whole capture),
so no extra serialisation in this scene.

Steady state in game: 0 walks a frame (`CommitCrossFrameWalk` zone), 0 full resolutions, and no fixed points committed (every
ticket commits through a stream). The ledger's change count stood still across the capture after loading.

### State ledger

Instrumented first (temporary counters): per commit 138 entries, 222 cells visited, 115 cells changed. More than half the cells
changed every frame, so the director's "a steady commit writes nothing" was not true yet. The cause: each batch commits its own
states, so a resource used in two batches of one execution was taken through its earlier state and back every frame.

- `committedSupersededAt` (computed in `Prepare`): a commit whose cells a later batch of the same execution commits again is left
  out when that batch commits too. Changed cells went from 115 a commit to about 1.
- One `CommitBatches` call per execution instead of one `CommitBatch` per batch (108 calls a frame to 14.5).
- The fingerprint is an XOR of per-cell hashes, updated for the cells a commit changes, not rehashed over the grid.
- Grids carry a change stamp and the ledger a change count. A call that repeats the last commit of the same batches (same
  backings, batch count and unmoved set, kept in `PreparedBatchBarriers::lastCommit`) skips every grid that hasn't changed since:
  those commits would write nothing. A grid is committed or skipped as a whole, in batch order, so the result is exact. A
  whole-call skip (nothing changed anywhere) never fired in game: a few shared resources legitimately alternate state between
  graphs every frame, which is why the skip is per grid.

### Alias ledger: one conflict scan

`ApplyInitialStates` (activations) and `AppendIncomingWaits` (waits) scanned the same heap conflicts for the same resources in
each admission. `AliasAccessAdmissionLedger::Resolve` does both in one scan of each first-use resource; a resource no batch uses
first is still scanned for its activation. `AdmitLedger` calls it once, at the start, where the activations are needed. The zone
`ResolveAliasActivations` is gone; `ResolveAliasHazards` covers both.

### Dead code removed

`ResolvePlannedPoints` (access and alias ledgers) and the three ledgers' `Reset` had no callers. The access grid fingerprints,
`kAccessCaptures` and the capture rotation in `AdmitLedger` went with the old design.

## Measured effect (per frame, Dev-Fast)

Baseline `p2-ledger-base2` (ORG 358597f) and after `p2-ledger-after4` (ORG 06d147f), taken back to back on the same day.
Other leads were building during the baseline. Its engine self time was 1.436 ms against 0.914, and zones this work did not
change (`CommitAliasHazards`, `CheckPrediction`) ran about 1.25x slower. The "base, scaled" column divides by 1.25. An earlier
after capture (`after3`) ran during heavy builds (window 7.6 ms) and is not used.

| Zone (inclusive, ms/frame) | base2 | base, scaled | after4 |
|---|---|---|---|
| `ORG.Persistent.Commit` | 0.603 | 0.48 | 0.110 |
| `ORG.Execution.CommitCrossFrameHazards` | 0.304 | 0.24 | 0.008 |
| `ORG.Execution.CommitStates` (108 calls a frame before, 14.5 after) | 0.215 | 0.17 | 0.042 |
| `ORG.Persistent.AdmitLedger` | 0.217 | 0.17 | 0.128 |
| `ORG.Execution.PredictedCrossFrameHazards` | 0.061 | 0.049 | 0.009 |
| alias resolve (`ResolveAliasActivations` + `ResolveAliasHazards`) | 0.062 | 0.050 | 0.050 |
| `ORG.Execution.CheckPrediction` (unchanged) | 0.019 | 0.015 | 0.015 |
| `ORG.Execution.CommitAliasHazards` (unchanged) | 0.019 | 0.015 | 0.016 |
| Ledger total (`Persistent.Commit` + `AdmitLedger`) | 0.820 | 0.66 | 0.238 |

The window from DCLF's scene tables to Present was 4.31 ms in after4. The handoff's after2 (ORG 6105257) was 4.62 ms; the
baseline's 6.05 ms was taken under load and can't be compared directly. The ledgers cost about 0.4 ms a frame less
(Dev-Fast). `CommitCrossFrameWalk` is 0 a frame. The single alias scan made no measurable difference: it is within the noise.
It stays because it is simpler (one function and one scan instead of two).

## Tests

ORG unit tests (`tests/AsyncCompileTests.cpp`, built by hand against the Dev-Fast `OpenRenderGraph.lib`), all passing:

- W1/R/W2 churn on one cell (W1 on queue 1, R and W2 on queue 0) plus a stage reading on queue 1 on odd frames only: from the
  second frame no commit walks, the change count stands for eight frames, every admission takes its capture, and the waits equal
  both a full resolution and an oracle of the old rule (latest writer plus the reads since).
- A fixed point invalidates every capture once, no stream walks. A stream closed and reopened walks once and folds the closed
  producer. 1100 closures fix a closed producer's reference: reads' captures hold, writes' miss once. A stream on other backings
  walks once each way. A retired grid makes every stream walk once.
- Boundary access waits name every writer and reader at its latest signal per timeline.
- Alias ledger: `Resolve` gives the waits and the activation of a re-entering occupant, and nothing for a disjoint one.
- State ledger: superseded commits (plan check, a partial receipt keeps them), and the repeated-commit skip: skipped while
  nothing changes, visited again after another plan's commit, a partial receipt is another commit.
- `OpenRenderGraphPersistentTests` passes.

Game runs (stage `p2-ledger`):

All runs on the final build (ORG 06d147f), passed, every shot checked by eye:

| Config | Label | Result |
|---|---|---|
| guest+quick | p2-ledger-v-guest | passed |
| native-vulkan+quick | p2-ledger-v-nvk | passed |
| native-d3d12-quick | p2-ledger-v-nd12 | passed |
| guest-interior (coc WhiterunDragonsreach, coc Whiterun back) | p2-ledger-v-interior | passed, 4 shots |

Earlier: `p2-ledger-instr1` and `-instr2` (guest+quick, intermediate builds) passed. Timing captures: `p2-ledger-base2`,
`-after3`, `-after4` (trun.sh; cs-test reports them as failed because they write no shot, which is expected).

## Decided by the agent, to confirm

- Monotone cells widen the wait set. In the river scene the cross-queue waits are the same (2 a frame). A scene where a cell
  is touched from another queue before its latest write would now add an implied cross-queue wait, which in the host stream can
  start a new segment. Not seen; worth a look in a capture with more async compute.
- The per-grid state skip is O(committed entries) a call. Going to O(changed grids) needs a reverse index from grids to a plan's
  entries; not worth it at the current cost.

## Open items

- The alias ledger's scan is still O(occupants of the heap) per aliased resource, and `CommitAliasHazards` runs every
  admission. An interval index per heap would make it O(overlaps); not worth it at the current cost.
- `CheckPrediction` (the state plan's fingerprint check) 0.016 ms.

## Commits

- ORG `perf2/ledger` on 358597f: 5bbb5c3 (access ledger cells only grow), 58f30e5 (state ledger commits only changed cells),
  06d147f (one alias conflict scan per admission).
- CS `perf2/ledger` on org_integration 719c5d2a0: b7ed674ac (ORG pointer), then these notes.
