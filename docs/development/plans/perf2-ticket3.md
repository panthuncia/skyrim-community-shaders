# Round 2 lead `ticket3`: what is left of ORG's per-epoch cost (2026-11-25)

CS branch `perf2/ticket3` on org_integration 5c74d144f, ORG branch `perf2/ticket3` on 00d26a7. No BasicRHI, DXVK or OMS changes.
All timings are Dev-Fast (/Od), guest mode, the frozen river scene (`trun.sh guest+quick ... --set env.CS_ENGINE_ZONES=1`), per
frame. Leads `ticket` and `ledger2` had taken the obvious per-ticket costs; this round looked for what remains.

## Where the time is

An instrumented capture (`p2-ticket3-instr1`, temporary zones, since removed) split the epoch zones' self time:

- What is left is spread thinly. Every epoch runs the same chain: commit (`CommitTicket`), admission (`PrepareAdmission`,
  `AdmitLedger`), timeline composition and handoff (`SubmitTicket`), the external execution (`ExternalExecution`), the ledger
  commits and the posting of the result. Each step costs 1 to 7 us an epoch at /Od, mostly small vectors, hash lookups and
  shared pointers. No single step is above 0.1 ms a frame.
- `CS.ORG.ExecuteEpoch`'s self time (0.06) was mostly `FrameAnchors::BeforeGpuWork` (0.037), which belongs to lead `frame`.
- `RHI.Vulkan.Submit` on the render thread is the Z-prepass epoch's two async-compute submissions (DCLF). BasicRHI's
  `q_submit` builds its submit arrays twice (legacy and `VkSubmitInfo2`), about 6 us a call. CS's hook (`CS.ORG.OffStreamSubmit`,
  new zone) hands the batch to the compute submitter thread (7 us a call). The first submission of an epoch enqueues the
  compute entry point in DXVK's stream (`CS.ORG.StreamPoint`, new zone, 8 us): DXVK ends its command list there. The
  `vkQueueSubmit` itself is already on the submitter thread.

## What changed (ORG)

1. **Staged copies (`StagedUploadBatch::RecordCopies`).** The overlap tracking kept a `std::map` per target and allocated a
   node per copy. It is now a sorted vector of ranges per target, appended in order in the common case and kept between calls,
   so a steady frame allocates nothing. The size check's `dynamic_cast` runs once per target, not once per copy. Back-to-back
   copies into one target from one page, both contiguous, are recorded as one `CopyBufferRegion`.
2. **Upload lists only for the queues a ticket uses.** `FinishTicket` began a list on every compute and copy queue for any
   ticket with a batch off graphics. Uploads go only to the queue of a batch that uses them (`PlanTicketUploads`), so a queue
   with no batch of the ticket gets no list. The Z-prepass begins 2 lists, not 3. `PersistentTicketOnQueue` became
   `PersistentTicketUsesQueue`.
3. **`SynchronousAdmission::Commit`** copied its `std::map` of submitted values every commit, to check that the receipt's
   signals rise before changing anything. It now checks against a kept vector of the timelines it touches and updates the map
   after the ledgers commit, as before.
4. **`CommitTicketSegment`** copied every batch's waits into the receipt; the commit reads only the signals. The receipt is
   kept per thread and only its signals are written.
5. **Revision binds** copy only the fields a candidate holds (publication, recording, bindings, heaps, waits, rebindings),
   not a whole segment with an empty barrier plan. The bind's "why" string is empty until a bind fails.
6. **External segments** reserve their items and signals for the batches left.

Tried and reverted: an overlap index per alias heap for `ResolveAliasHazards` (the conflict scan only visits overlapping
occupants). It measured the same (0.047 before, 0.050 after): the hash lookups per resource, not the scan, are the cost.

## Measured (Dev-Fast, ms per frame)

Baseline `p2-ticket3-base1` (unchanged branch) and after `p2-ticket3-final` (ORG 783f369, the final build). Code these changes
don't touch read about 10% lower in the after capture (engine self 0.964 against 0.869, DCLF CPU 0.563 against 0.517), so the
machine was busier during the baseline.

| | base1 | final |
|---|---|---|
| Window | 4.012 | 3.569 |
| ORG epochs (inclusive) | 1.662 | 1.459 |
| Engine self (unchanged code) | 0.964 | 0.869 |
| `ORG.Upload.RecordCopies` | 0.055 | 0.041 |
| `CommandListPool::ResetClaimed` | 0.021 | 0.016 |
| `ORG.Persistent.ExternalExecution` (self) | 0.135 | 0.096 |
| `ORG.Host.CommitTicket` (self) | 0.075 | 0.065 |
| `ORG.Host.FinishTicket` (self) | 0.075 | 0.066 |
| `ORG.Persistent.Commit` (self) | 0.041 | 0.035 |
| `ORG.Persistent.SubmitTicket` (self) | 0.069 | 0.064 |

Every epoch's inclusive time dropped 9 to 20% (Z-prepass 0.512 to 0.450, post 0.127 to 0.101, deferred+water 0.083 to
0.072), about 10% of it the machine. Scaled by the engine's ratio, the ORG bucket's real cut is about 0.04 to 0.05 ms a frame
(Dev-Fast), within the 0.1 ms noise between captures for the window but consistent across every epoch and across three after
captures (`after1`, `after2`, `final`). Captures and trees: `scratchpad/ticket3/perf/`.

## Test runs

ORG unit tests, standalone Vulkan build of ORG 783f369 (`OPENRENDERGRAPH_BUILD_TESTS=ON`, sibling junctions to the worktree's
BasicRHI, BasicTelemetry and volk): `OpenRenderGraphPersistentVulkanHostTests` default, async, revision, segment,
segment-external, external, external-reuse, reuse, stream, built and split pass; `OpenRenderGraphAsyncCompileTests` and
`OpenRenderGraphPersistentTests` pass. (`leases` was not run: it hangs on the base too, see `perf2-ticket.md`.)

Game runs on the final build (stage `p2-ticket3`), all passed, every shot checked by eye:

| Config | Label | Result |
|---|---|---|
| guest+quick | p2-ticket3-v-guest | passed |
| native-vulkan+quick | p2-ticket3-v-nvk | passed |
| native-d3d12-quick | p2-ticket3-v-nd12 | passed |
| guest+fg | p2-ticket3-v-fg | passed, 5 shots |
| guest-interior | p2-ticket3-v-interior | passed, 4 shots |

Timing captures: `p2-ticket3-base1`, `-instr1` (temporary zones), `-after1`, `-after2`, `-final` (trun.sh; cs-test reports them
as failed because they write no shot, which is expected).

## Commits

- ORG `perf2/ticket3` on 00d26a7: 48cd618 (staged copies without allocating), 783f369 (per-ticket copies and lists).
- CS `perf2/ticket3` on 5c74d144f: 1fd73941a (ORG pointer), 6b88eaa18 (off-stream submission zones), then these notes.

## Decided by the agent, to confirm

- Copies are merged only when they are adjacent in the batch order, same target and same page, both offsets contiguous. The
  overlap tracking sees the same byte ranges either way.
- The kept copy tracking, the commit's rising check and the segment receipt are per thread or under the sequencer's mutex,
  and nothing re-enters them.
- A revision candidate is not moved into its ticket (that would avoid the copy) because the owner thread may still be walking
  the candidate list when the bind runs.

## Open items

- **Moving `FinishTicket` off the render thread in guest mode.** The render thread needs only the execution's segment shape
  (waits, signals, splits) to emit DXVK's external commands; the segments' contents run later on DXVK's CS thread. Finishing
  tickets on the submission thread would make DXVK's CS thread wait for it. A larger change than this round.
- **BasicRHI `q_submit`** builds its submit arrays twice on the hooked path (about 6 us a call, 2 calls a frame).
- **The compute entry point** (`CS.ORG.StreamPoint`, 8 us) ends DXVK's command list once an epoch with async compute.
- `ResolveAliasHazards` (0.05) and `PrepareAdmission` (0.05) are per-resource hash lookups and per-admission vectors.
