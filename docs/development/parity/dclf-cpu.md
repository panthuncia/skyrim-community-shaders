# DCLF's CPU pipeline on BasicRHI (lane `par-dclf-cpu`)

Plan: `we-ve-been-working-on-foamy-journal.md`, WS5.7 to WS5.9. Decision: the CPU-driven pipeline stays, and its sites record
BasicRHI lists on every backend. Foundation: `parity/inline-exec.md` (ORG's inline at-execution passes, `EmitCommands`,
`RecordsAtExecution`).

## What changed

### No raw Vulkan in the sites

- **Guest emission.** `Emit` (a site's segments), the forward hand-off (`HandOffRun`) and its cancel (`ForwardHandOffCancel`) go
  through `RenderGraphRuntime::EmitCommands`. The callbacks take an `rhi::CommandList&`, and `EmitCommands` adopts DXVK's buffer
  and keeps the wrapper until later D3D11 work completes. `SiteRun::adopted`/`adoptedEarlier` are gone.
- **Waits and signals.** `FrameValues::FrameWaitPoint()` returns an `rhi::TimelinePoint` (value 0: none). The frame signal
  (`QueueFrameSignal`) is an `rhi::TimelinePoint` on DCLF's CPU-frame timeline (`cs.dclf.cpu-frames`), emitted with
  `EmitCommands(nullptr, ..., &signal, 1, ...)`. The timeline is no longer queried for a `VkSemaphore` (`State::semaphore` is gone),
  so it can be made on D3D12.
- **`SiteStats`** (`CS_DCLF_SITE_STATS=1`) uses a BasicRHI pipeline statistics query pool. Each query is resolved into a readback
  buffer and read once its frame's CPU-frame value completed. BasicRHI has no clipper counters, so the log line now has IA
  vertices, IA primitives, VS and PS only.

### Native mode: sites record inline (both backends)

`Direct::SitesInline()` is true when the host records at-execution passes inline (`GraphHost::InlineAtExecution`: native mode).
Then:

- **CPU frames.** `GraphConditions` no longer adds `CpuPipelineEpochs()` to the external epochs. `Direct::ExecuteEpoch` calls
  `ExecuteInlineEpoch`, which submits the epoch with `RenderGraphRuntime::ExecuteEpoch`. The body is the ticket's commit, and it
  opens the site's run (`State::pending`). The site's late pass (`SitePasses.cpp`, already `RecordAtExecution`) is recorded inline
  during the same submission on the render thread. `RecordExecutingSite` takes `pending` there. There is no `Emit`, no
  `SiteCallback`, no adoption and no `CloseExternalEpoch`. ORG's batches carry the frame wait (`SetFrameWaitValue`), and ORG holds
  the epoch's owner.
- **Bookkeeping in the late pass.** Site stats, `siteRuns`, `siteEmpty` and `recordUs` are kept in `RecordExecutingSite` when no
  guest callback runs. The views' queues are not drained there: `Release` discards what is left, as for a callback that was not
  queued.
- **Secondaries.** `SiteOut::direct` records everything straight into the epoch's primary list. No secondary is made on either
  backend: D3D12 bundles cannot set viewports or hold barriers, and nothing in the code needed secondaries outside guest's state
  isolation from DXVK. Guest
  keeps secondaries (`Inline()`).
- **GPU-driven frames.** `HandOffRun` and `ForwardHandOffCancel` set `handedOff` directly. The epoch records its late pass inline
  after the call, on the same thread.
- **The direct forward and first-person families.** `GraphConditions::forwardDirect` and `firstPersonForwardDirect` test
  `RecordsAtExecution(EpochOf(Forward))` and `(FirstPerson)` instead of `InStreamEpochs()`. They come back in native mode. Guest is
  unchanged, since `RecordsAtExecution` there is "in-stream".
- **The cost moves.** In guest, DXVK's CS thread waits for the culls and records. In native, the render thread does both, inside
  the epoch's submission.

### D3D12

- **Indirect records** (`IndirectDraw`, 32 bytes): a leading instance word, then DrawIndexed's arguments, then the sequence (24)
  and the object word (28). `DCLFObjects.hlsli` reads the record at `instance * 32 + 24`.
  - D3D12's signature is Constant(`kDrawPushInstance`) then DrawIndexed. It is made per layout (`IndirectSignatureFor`), because a
    root constant argument names a root signature: one each for the Z-prepass layout and the shadow layout.
  - Vulkan's signature is DrawIndexed alone, started 4 bytes into the record (`IndirectBuffer::argumentsOffset`).
  - A direct draw after a run on D3D12 pushes 0 into the instance word again.
- **CPU shadow site.** On D3D12 each view's push words 4-13 are replaced, as `ShadowViewPass` does: the view block and its
  VS_PerFrame copy by ref (the adopted version of `viewBlocks`), the zero block by ref (`constantsRef`), and SharedData and
  FeatureData by constant buffer view (`blockViewIndices`).
- **Pushed colour and depth** are no-ops on D3D12 (`BeginList`). They need unpulled
  pipelines, which D3D12 does not build.
- **`kD3D12DclfSwitches`** (its own commit): `CS_DCLF_PIPELINE=gpu`, `CS_DCLF_FORWARD_DIRECT=0`, `CS_DCLF_FORWARD=0` and
  `CS_DCLF_FIRST_PERSON=0` are gone. The Effect and particle rows stay: particles draw in the forward site's sort, but they need
  the `effects` toggle, and the Effect family's D3D12 builds are another lane's (`parity/dclf-d3d12.md`, WS4.3). Refraction stays off
  (`CS_DCLF_REFRACTION=0`): the forward site's refraction pass pushes `MainShadowLayoutPush` addresses, which D3D12 cannot read.

## How to test

Scenarios (new): `tools/cs-ab/scenarios/native-dclf-cpu.env` and `native-d3d12-dclf-cpu.env`. Each runs Whiterun at midday
(GPU-driven, forward family on the CPU) and Dragonsreach (CPU-driven by Auto), each in third and first person with a torch, with
site stats on. Run guest and the existing `native.env` and `native-d3d12.env` too.

1. **Guest quick run and tour.** Nothing should change. Look for the `[ORG] in-stream epochs` line, the CPU pipeline's interiors,
   the `[CPU]` report line, and the same shots as before. Indirect mode now uses 32-byte records, so look at Dragonsreach closely.
2. **Native Vulkan, `coc WhiterunDragonsreach`.** This is the CPU-driven interior.
   - `[DCLF] graph conditions: interior cell, ..., CPU pipeline` should appear.
   - No `[DCLF] CPU pipeline:` errors, in particular `submitted without recording its late pass` or `ORG epoch was not submitted`.
   - The `[CPU]` line should show site runs, draws, and no `NOT QUEUED`.
   - `[DCLF] site stats depth/colour/...` lines should appear every 300 frames.
   - Shots: hearth, banners, tables and NPCs as in guest. Point-light shadows should be present (shadow site).
3. **Native Vulkan, Whiterun.** `[DCLF] graph conditions: exterior cell, ..., GPU pipeline, forward draws on the CPU` should appear.
   Blended objects should be in order: smoke over the logs behind it, and flames in front. Look also at first person's arms and
   torch (`firstPersonForwardDirect`, so no `first person stays the engine's` warning).
4. **Native D3D12, the same.** `[ORG native] D3D12: Drawcall Limit Fix switches set` no longer lists `CS_DCLF_PIPELINE`,
   `CS_DCLF_FORWARD_DIRECT`, `CS_DCLF_FORWARD` or `CS_DCLF_FIRST_PERSON`. There should be no
   `D3D12 indirect draws' signature could not be made` line and no `D3D12 CPU shadow site: no buffer view ref` line. Compare the
   shots with native Vulkan's. Effects and particles are still off on D3D12, so smoke and flames are missing there.
5. **`native-d3d12-validation.env` with `CS_DCLF_PIPELINE=cpu`.** The debug layer should report nothing around
   `cs.dclf.*.direct` passes, ExecuteIndirect or root constants.
6. **Epoch stats** (`CS_ORG_EPOCH_STATS=1`, native). The CPU frames' site epochs now count as plain executions. There should be
   one extra submission a frame for the frame signal (`DCLF CPU frame`).

## What is left

- **The deletion commit** (`Native::External`, `CS_ORG_NATIVE_IN_STREAM`) is cherry-picked last (see the report). Nothing in DCLF
  calls `EmitExternalCommands` any more.
- **Guest still uses `ExternalEpochExecution`** (`ExecuteExternalEpoch`, `Emit`, `CloseExternalEpoch`) for the CPU frames' epochs.
  That is DXVK's in-stream path, kept by design.
- **The frame signal in native** is a submission of its own (`Native::EmitCommands`). It could ride the last site epoch's
  submission as a signal, which would need an ORG hook.
- **Not verified without a build:** the D3D12 per-layout signature's root index 0 (taken from `DrawPipelines`' plain-draw
  signatures), and the adopted version of `viewBlocks` as the shadow site's ref (`ShadowViewPass` resolves it from its recording).
- **For `par-dclf-d3d12b`:** refraction on D3D12 needs `MainShadowLayoutPush` refs and views for the CPU forward site's
  refraction pass as well as for `ForwardPasses`. The hosted views' frame push words (`shape.push`, reflection and first person's
  depth) are the GPU view passes' words. If those passes replace words on D3D12, the CPU depth site must do the same.
