# Round 2 lead `ticket`: fixed per-ticket cost of ORG epochs (2026-11-25)

Leads `ticket` (the change) and `ticket2` (staging, validation, A/B, these notes). CS branch `perf2/ticket`, ORG branch
`perf2/ticket` (46baf56 on 358597f). No BasicRHI, DXVK or OMS changes. All timings are Dev-Fast (/Od), guest mode, the frozen
river scene (`trun.sh guest+quick ... --set env.CS_ENGINE_ZONES=1`), per frame.

## What changed (ORG 46baf56)

1. **A pinned epoch predicts its next copy from its step** (`GraphHost::SubmitEpoch`, `Async::lastPinned`). The ticket prepared
   ahead was always for the next copy. The sun stage runs every other frame on a 4-slot clock, so every run named a copy two
   past the last, gave the prepared ticket back and waited in `WaitTicket` for a new one. The ticket prepared ahead now takes
   the copy as far past the last as the last was past the one before. The segment test has a "two slots apart" step (0 re-pins
   over 6 runs).
2. **Empty upload work returns early.** `UploadInstance::ProcessDeferredReleases` and `TakePendingUploads` return when nothing
   is queued. `FinishTicket` begins the other queues' upload lists only for a ticket with batches on those queues
   (`RenderGraph::PersistentTicketOnQueue`); lists are ended only while recording.
3. **Kept handoff lists are looked up before the barriers are sorted** (`TicketHandoff`). On a hit nothing is built
   (`HandoffCache::leftOut` keeps the count).
4. **Kept vectors.** `PersistentExecutionsState::TicketScratch` per graph (a busy flag gives nested calls their own);
   `AdmitPersistentTicket` keeps queue timeline identities and admitted queues; frame wait readers are kept on the recording,
   keyed by backing set id.
5. **`IPreparedExecutionBatch::AsRhi()`** is a virtual call, not a `dynamic_cast`.

## Measured

Back to back: `base1` (ORG 358597f, otherwise the same tree), then `after2` (ORG 46baf56). `after1` was taken before `base1`.
Code the change doesn't touch (engine self, DCLF CPU) reads 6-8% higher in `base1`, so the machine was busier then; the totals
below include that.

| | base1 | after1 | after2 |
|---|---|---|---|
| Window | 5.276 | 4.413 | 4.536 |
| ORG epochs (inclusive) | 2.622 | 2.021 | 2.068 |
| CS stage: sun | 0.195 | 0.046 | 0.047 |
| ORG.Host.WaitTicket | 0.118 | 0.000 | 0.000 |
| ORG.Persistent.TicketHandoff | 0.130 | 0.024 | 0.024 |
| UploadInstance::ProcessDeferredReleases | 0.041 | 0.000 | 0.000 |
| ORG.Upload.DrainImmediateCommands | 0.023 | 0.000 | 0.000 |
| ORG.Persistent.SubmitTicket | 0.098 | 0.066 | 0.068 |
| ORG.Persistent.TakePendingUploads | 0.037 | 0.015 | 0.016 |
| ORG.Persistent.AdmitTicket | 0.042 | 0.024 | 0.024 |
| CommandListPool::ResetClaimed | 0.032 | 0.019 | 0.019 |

The changed zones sum to about 0.37 ms a frame. ORG epochs drop 0.55 ms, about 0.18 ms of it from the machine load, which leaves
the same ~0.37 ms. Every epoch drops by 15-35 us (the fixed per-ticket cost); the sun stage drops most because it no longer
re-pins. Captures: `scratchpad/ticket2/perf/{base1,after1,after2}.buckets.txt` and `.tree.txt`.

## Test runs

| Config | Label | Result |
|---|---|---|
| guest+quick | p2-ticket-v2-guest | passed |
| native-vulkan+quick | p2-ticket-v2-nvk | passed |
| native-d3d12-quick | p2-ticket-v2-nd3d12 | passed |
| guest+fg | p2-ticket-v2-fg | passed |

ORG unit tests (`OpenRenderGraphPersistentVulkanHostTests`, standalone Vulkan build): default, async, revision, segment,
segment-external, external, external-reuse, reuse, stream, built and split pass. `leases` and `leases open` hang after the
`leases:` stats line on this branch **and on base 358597f** (same log line for line, including "Synchronous frame capacity
exhausted" for epoch 0 build 1). The hang is from before this change, so it is left alone.

## Decided by the agent, to confirm

- The next copy is predicted from the last two runs' step (step 0 allowed). A wrong guess costs what every guess cost before:
  one re-pin.
- Kept frame wait readers trust `AdmissionBackingSet::Id` to change whenever the backing set does.
- Stage and DCLF epochs are not paired further (10-15 us a pair).

## Open

- The `leases` / `leases open` hang (pre-existing, see above).
- `PersistentGraph.cpp`'s `PrepareFrame` and the ledger zones were left to lead `ledger2`.
- `ExtBatches` remainder (about 1 us a batch) is per-pass commit work.
