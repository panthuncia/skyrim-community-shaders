# Inline at-execution passes (lane `par-inline-exec`)

Plan: `we-ve-been-working-on-foamy-journal.md`, WS1.5 and WS5.7. Decision: native mode has no external epochs, and DCLF's CPU
sites record BasicRHI lists on every backend.

## What changed

- **ORG primitive** (`extern/OpenRenderGraph`, branch `org-native/par-inline-exec`).
  `org::GraphHost::SetInlineAtExecution(true)` (`org::RenderGraph::SetInlineAtExecution` on every graph it builds) changes how a
  pass declared `RecordAtExecution()` is recorded when its epoch is not external:
  - Its late batch is recorded when the execution submits that batch. That happens on the submitting thread. For a ticket it
    happens after the run's commit (`SubmitEpoch`'s `beforeSubmit`), so the pass reads what the commit wrote.
  - It is recorded into a primary list from the queue's shared pool. The admission's barriers go before and after it, as for any
    batch, and a pass that skips the run (`SkipsAtExecution`) records only those barriers.
  - The list is submitted in the batch's place, with the batch's waits, handoff list and signals. There is no secondary list, no
    extra submission and no full barrier. It works on any queue and on both backends.
  - External epochs keep their behaviour: their late batches are still their host's to record.
  - Code: `PreparedRhiExecutionBatch::RecordInline` and `Submit` (`RhiExecution.h`), `RecordPersistentSegment`
    (`RenderGraphPersistentExecute.cpp`), `OwnedRecordingList::inlineAtExecution`.
- **CS native mode** (`RenderGraphRuntime::Initialize`, the async-epochs block): native mode turns inline recording on, on Vulkan
  and D3D12, and sets no in-stream epochs. Every epoch submits directly.
  - `CS_ORG_NATIVE_IN_STREAM=1` (native Vulkan only) keeps the old emulation: in-stream epochs whose segments `Native::External`
    submits. It is kept only until DCLF's sites move (below).
  - Without in-stream epochs, `InStreamEpochs()` is empty in native mode, so DCLF's direct forward and first-person families turn
    off there (`GraphConditions`) until they move.
- **Frame anchors** (a separate commit, which the main session can drop): `PassDesc::atExecution` passes are recorded at execution
  whenever `org::RenderGraph::PassEpochRecordsAtExecution()` says so. In native mode, ImageSpace's gated passes and the skeleton's
  clears are then recorded inline, and a closed pass costs only its barriers, as in guest.
- **The BasicRHI seam for DCLF**: `RenderGraphRuntime::EmitCommands` and `RenderGraphRuntime::RecordsAtExecution` (below).
- **Deletion, a separate final commit**: `Native::External`, `Native::EmitExternalCommands` and `CS_ORG_NATIVE_IN_STREAM`. Merge
  it only after DCLF's sites moved. After it, `RenderGraphRuntime::EmitExternalCommands` refuses in native mode.

## The API

ORG:

```cpp
host.SetInlineAtExecution(true);                // GraphHost, before the graphs are built (a change rebuilds the non-external epochs)
graph.PassEpochRecordsAtExecution();            // while an extension gathers its passes: in the stream, or inline
desc.RecordAtExecution();                       // ExternalPassDesc: the pass is recorded at execution
```

CS:

```cpp
// Whether a RecordAtExecution pass of the epoch is recorded at execution (guest: in-stream epochs; native: every epoch).
bool RenderGraphRuntime::RecordsAtExecution(std::uint32_t epoch) const;

// EmitExternalCommands with BasicRHI types, for work that is not a graph pass. Same call in guest and both native backends.
bool RenderGraphRuntime::EmitCommands(void (*record)(void* user, rhi::CommandList& commands), void* user,
	const rhi::TimelinePoint* waits, std::uint32_t waitCount, const rhi::TimelinePoint* signals, std::uint32_t signalCount,
	bool bindsState, const char* label, bool fullBarrier = false);
```

- `EmitCommands` in guest mode uses DXVK's external commands. DXVK's command buffer is adopted as the `rhi::CommandList`
  (`rhi::vulkan::AdoptCommandBuffer`), and the wrapper is kept until D3D11 work issued after the call completes
  (`ReleaseAfterD3D11`). The waits and signals are converted to DXVK's semaphores (`NativeTimelinePoint`). A point that is not on
  one of the host's timelines refuses the call.
- `EmitCommands` in native mode records into a command list of its own on the graphics queue (`Native::EmitCommands`, a ring of
  64 BasicRHI lists). It is submitted at the call, between full memory barriers, with the caller's waits and signals. This works on
  D3D12 too.
- In both modes the record callback starts with nothing bound. It binds its heaps, layout and push data, and it begins and ends
  every rendering it opens. A null record means waits and signals only. The record runs exactly once when the call returns true,
  and never when it returns false.

## How a client uses it

A pass that draws work only known after the commit (a CPU cull's draws):

```cpp
class SiteDrawsPass final : public org::TypedRenderGraphPass<SiteDrawsPass, SiteFrame, SiteBindings>
{
public:
	SiteBindings Declare(org::PassBuilder& b) { /* every target, buffer and texture the draws use */ }
	// After the commit: nothing to draw this run, so only the barriers are recorded.
	static bool SkipsAtExecution(const SiteBindings&, const SiteFrame& f, const org::PassRecordContext& r) { return Empty(f, r.Run()); }
	static void Record(const SiteBindings& b, org::PassRecordContext& r)
	{
		rhi::CommandList& list = r.Commands();  // the epoch's own primary list in native mode, DXVK's buffer in guest mode
		// Per-run data comes from the run (r.Run(): its latch slots) and from what the commit published for that run.
		DrawSite(list, b, r.Run());
	}
};

// Gathering passes:
auto desc = org::RenderGraph::ExternalPassDesc::Render("dclf.site.depth.draws", std::make_shared<SiteDrawsPass>(...)).Epoch(epoch);
if (graph.PassEpochRecordsAtExecution())
	desc.RecordAtExecution();
```

Work that is not a pass, such as a frame signal:

```cpp
const rhi::TimelinePoint signal{ cpuFrameTimeline->GetHandle(), value };
RenderGraphRuntime::Get().EmitCommands(nullptr, nullptr, nullptr, 0, &signal, 1, false, "DCLF CPU frame");
```

## Guest and native

| | Guest (DXVK) | Native (Vulkan and D3D12) |
|---|---|---|
| Epochs | In-stream epochs are external: their graphics batches are secondaries that DXVK's buffer executes | No external epochs: every batch is a primary list, submitted by the graph |
| `RecordAtExecution` pass | Recorded by `ExternalEpochExecution::Execute` into DXVK's buffer | Recorded by the batch's own submission into a primary list, in the graph's order |
| Barriers around it | The admission's, plus the external queue boundary | The admission's |
| `EmitCommands` | DXVK external command with an adopted list | A BasicRHI list of its own, submitted at the call |
| `EmitExternalCommands` | Unchanged | `Native::External` until the deletion commit, then refused |

## Recording reuse

- A recording whose late pass is not replayable (`PreparedPass::IsReplayable`) is never kept. This holds for external and inline
  late passes alike (`RenderGraph::RecordPersistentSegment`: `keepable` needs `!lateUnreplayable`).
- A kept recording, such as a revision's published copy, records its late batch again at every submission
  (`PreparedPass::RecordAgain`) into the same list. `CommandListPool::Begin` resets that list. This is safe because a copy is
  submitted only once its last run completed (`GraphHost::TakeCopyLease`), so the list is never pending twice.
- Live tickets record once and submit once. A ticket prepared ahead holds the late batch unrecorded until its submission, so a
  held ticket (`HoldEpochTicket`, regions) records the pass after the last commit that joined it.
- Statistics of a late batch are published at the frame slot's completion, as for an external epoch's late batch. Inline lists
  carry no Tracy GPU zones: the submitting thread records them while the recording workers may record other lists for the same
  queue.

## How DCLF's sites move (the later lane)

`Core/Render/Direct/Sites.cpp` today. Each site already draws through a late pass (`RecordExecutingSite`) inside an external epoch
that `GraphConditions` adds for the CPU pipeline (`CpuPipelineEpochs()`, `SetExternalEpochs`).

1. **`Emit` (~:1061-1107, `EmitExternalCommands(&SiteCallback, ...)` at ~:1095).** One callback per segment of the external
   execution. It carries the frame values' wait and the segment's waits and signals, and it adopts DXVK's buffer (~:1019).
   - Native: do not add `CpuPipelineEpochs()` to the external epochs when `RecordsAtExecution(epoch)` is true without them, that is
     in native mode. `ExecuteExternalEpoch` then becomes `ExecuteEpoch` there, and the late pass records the draws inline. There is
     no `Emit`, no `SiteCallback` and no adoption.
   - Move what `SiteCallback` does around the draws into the late pass: `SiteStats::Begin` and `End` (on BasicRHI query pools,
     WS5.7), the drawn counters, `DrainView` for every view, and the site-empty count. Native records it on the render thread at
     submission; guest still records it on DXVK's CS thread.
   - The frame values' wait (`FrameValues::FrameWaitPoint`, a `VkSemaphore`) becomes the graph's frame wait
     (`GraphHost::SetFrameWaitTimeline` and the frame wait value), which ORG already puts on the batches that read the frame's
     values.
   - Guest keeps `Emit`, or moves it to `EmitCommands` to drop the adoption code (`run.adopted`, `adoptedEarlier`).
2. **`HandOffRun` (~:1296) and `ForwardHandOffCancel` (~:1391).** A callback that records nothing and sets `handedOff` in stream
   order. Use `EmitCommands` with the same lambda, taking `rhi::CommandList&`. In native mode it runs at the call, which is
   stream order there. When the forward family records inline, the hand-off can instead be a flag the late pass reads.
3. **`QueueFrameSignal` (~:1591, `SignalCallback`).** Signal only, on DCLF's CPU-frame timeline (`s.semaphore`). Use
   `EmitCommands(nullptr, nullptr, nullptr, 0, &signal, 1, false, "DCLF CPU frame")` with an `rhi::TimelinePoint` of a BasicRHI
   timeline (WS5.7: `rhi::Timeline` points instead of `VkSemaphore`). In native mode it is a list of two barriers that signals the
   point. It must come after the frame's last site submission, which call order gives.
4. **`ExecuteEpoch` (~:1623-1674).** The external branch (`ExecuteExternalEpoch`, `CloseExternalEpoch`,
   `SignalExecutionUnexecuted`) stays for guest. In native mode `RenderGraphRuntime::ExecuteEpoch` submits the epoch, and its
   commit body runs as `beforeSubmit`.
5. **The direct forward and first-person families** (`GraphConditions::forwardDirect`, `firstPersonForwardDirect`) test
   `InStreamEpochs()`. They should test `RecordsAtExecution(EpochOf(Segment::Forward))` (and FirstPerson) instead, once their passes
   are late passes in those epochs.
6. **Then** merge the deletion commit, and delete the `EmitExternalCommands(VkCommandBuffer...)` signature and DCLF's raw Vulkan
   callbacks (WS5.9).

## What to test

ORG (done, `PersistentVulkanHostTests`, Vulkan, standalone build of the submodule): `inline`, `async inline`, `async open inline`, `reuse inline`,
`revision inline`, `revision open inline` pass; each checks that the late copy is recorded exactly once per submission and that
no external execution is handed back. The existing modes still pass (`sync`, `async`, `async open`, `reuse`, `reuse open`,
`revision`, `revision open`, `external`, `external open`, `external-reuse`, `stream`, `stream open home`, `split`, `built`,
`arena`, `rebinding`, `pipelines`, `contributor`). `leases` stops with "Synchronous frame capacity exhausted" on the base commit
53848cd too.

CS (main session; not built here):
1. **Guest quick run and tour.** Nothing should change: in-stream epochs as before (`[ORG] in-stream epochs: ...`), DCLF CPU and
   GPU pipelines, the same shots.
2. **Native Vulkan quick run.** Look for `[ORG] native: no external epochs; ...` and no `[ORG] in-stream epochs` line. The loading
   screen must finish (a note from Phase 4: with home states off and no in-stream epochs the load never ended; this run has home
   states on). Compare shots with the previous native default. Expect DCLF's direct forward families off
   (`[DCLF] graph conditions: ...` without "forward draws on the CPU"). First person may then fall to the engine's path or drop out.
3. **Native Vulkan with `CS_ORG_NATIVE_IN_STREAM=1`.** The old behaviour, as a control.
4. **Native D3D12 quick run.** ImageSpace through inline passes (with the frame-anchors commit), the same shots as before.
5. **`native-validation`** with the frame-anchors commit: no new layout or barrier errors around ImageSpace and skeleton passes.
6. **Epoch stats** (`CS_ORG_EPOCH_STATS=1`): executions a frame should not grow in native mode. Inline passes add no submission.

## What is left

- DCLF's move (above), then the deletion commit.
- `Native::EmitCommands` is a separate submission per call. Only out-of-graph work should use it. A site's draws belong in its
  epoch as a late pass.
- WS1.3 wants one submitter besides the graph host (`ExternalWork`). `ExternalWork::Submit` has no waits yet, so
  `Native::EmitCommands` has a ring of its own. Fold it into `ExternalWork` when that takes waits.
