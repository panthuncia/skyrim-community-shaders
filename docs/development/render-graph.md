# OpenRenderGraph on DXVK

Community Shaders can run GPU work through [OpenRenderGraph](https://github.com/panthuncia/OpenRenderGraph)
(ORG) on the same Vulkan device that DXVK uses to translate the game's D3D11. Light Limit Fix's clustered
light culling is the first feature that does this. Its D3D11 path stays in place as the fallback.

## Runtime shape

-   **One device.** `RenderGraphRuntime` (`src/RenderGraph/`) has BasicRHI *adopt* DXVK's `VkDevice`
    (`rhi::vulkan::AdoptVulkanDevice`). There is no second device, no shared handles and no cross-API
    fences.
-   **Device features.** Before the game creates its device, `DxvkLoader` calls
    `dxvkRequestDeviceFeatures` to ask for what BasicRHI needs beyond DXVK's own set:
    `vulkanMemoryModelDeviceScope` (DXC's atomics use Device scope, and DXVK enables
    `vulkanMemoryModel`), `deviceGeneratedCommands` (DCLF), and the descriptor models' features below.
    These are enabled on the `VkDevice` only; DXVK's own code keeps the feature set it would have chosen.
-   **Descriptor buffers, negotiated.** BasicRHI records into DXVK's own command buffers, so the two must
    bind descriptors the same way (see "One descriptor model" below). The graph runs on descriptor
    buffers alone, with heaps emulated by BasicRHI's lowering. It never asks for `VK_EXT_descriptor_heap`,
    which on NVIDIA rendered DLSS black and needed workarounds beside DXVK.
    -   **The callback.** `DxvkLoader` registers `dxvkSetBindingModelCallback`. During device creation DXVK
        calls `NegotiateBindingModel` (`RenderGraphRuntime.cpp`) with the physical device and its own
        choice. The callback answers with the descriptor-buffer bit of `rhi::vulkan::EvaluateBindingModels`
        (BasicRHI's limit checks, with DXVK's 2048-sampler pool and descriptor buffers counted in; every
        check is logged under `[ORG]`).
    -   **DXVK's answer.** DXVK switches to descriptor buffers wherever the device has them with mutable
        descriptors, a model the user forced on included (with a warning).
    -   **fmask.** On amdvlk with fmask (RDNA2 and older), where DXVK's policy keeps legacy sets for fmask
        alone, DXVK takes descriptor buffers for the graph too, warning that fmask is off. Neither Skyrim
        runtime has an MSAA setting (1.5.97 and 1.6.1170 have only `bUseTAA:Display`), so the game creates
        no MSAA targets and loses nothing.
    -   **Legacy sets kept.** Pascal and most Intel keep legacy sets, and the graph is off there.
        `dxvk.enableDescriptorBuffer = False` turns it off too.
    -   **The grant.** DXVK then grants `descriptorBuffer` + `mutableDescriptorType`, and `Initialize` logs
        the model or disables the graph with the reason (RenderDoc's capture config forces legacy sets).
-   **Persistent graphs.** An `org::GraphHost` drives a graph per epoch (segment) in persistent mode: each is compiled
    once and each frame runs prepare + record + submit. A resize or a pass-set change rebuilds only the epochs whose
    graph conditions changed (`RequestRebuild(epoch)`), with no device wait. Embedding ORG is described in
    `extern/OpenRenderGraph/docs/persistent-epochs.md`.
-   **Epoch.** `RenderGraphRuntime::ExecuteEpoch` runs the graph once, on the thread that drives the
    D3D11 immediate context (LLF calls it from `UpdateLights`). CPU data for a frame, such as the light
    list, is uploaded by the graph's own `BUFFER_UPLOAD` path inside the epoch.
-   **Frames in flight.** `GraphHost::Desc::framesInFlight` is game frames: whole frames of the epoch order
    (4, `kGameFramesInFlight`). The host's frame slots are that many frames of every epoch (`FrameSlots`, 24 with six
    segments), one ring that every execution takes the next slot of. A slot's next execution waits for its last one's GPU
    work: the "ticket wait" in `CS_ORG_EPOCH_STATS`, on the host's thread, which the render thread sees only when the ring
    is shorter than the CPU's lead over the GPU. The ring used to be a fixed 16 slots: the reflection faces' sixth epoch
    shortened it to 2.7 frames, and every few frames the render thread waited 2-7 ms on whichever epoch came round
    (p99 frame time 25 ms instead of 12). Sized by the epochs, a new segment needs no change here.

## Ordering against D3D11

### In-stream epochs (the default)

With DXVK's external commands (`dxvkEmitExternalCommands`), the async epochs are external epochs
(`GraphHost::SetExternalEpochs`, `CS_ORG_IN_STREAM`). Their graphics work runs inside DXVK's own command list, and only a
wait on another queue splits that list:

1.  At the epoch's point, `ExecuteEpoch` submits the ticket (`SubmitEpoch`). ORG submits the batches on the compute queue
    itself; the graphics batches come back as an `ExternalEpochExecution` of ordered segments.
2.  `Impl::EmitExecution` emits each segment as an external command. DXVK calls back on its CS thread, the callback executes
    the segment's secondaries into DXVK's command buffer (`ExternalEpochExecution::Execute`), and the segment's waits and
    signals go on the DXVK submission it lands in.
3.  A segment marked `split` is preceded by a D3D11 `Flush`, which ends DXVK's command list: one of its waits is for compute
    work that waits for an earlier segment.
4.  `CloseComputeEpoch` adds the stream's wait for the epoch's compute and copy work (`computeTail`) as a wait-only external
    command, after a flush when the execution's `exitSplit` says so.
5.  DXVK's images are imported at the layout DXVK reports for them (`DxvkImageImport`), which is their home: the runtime opts in
    to `DXVK_ORG_INTEROP_RESTING_LAYOUTS`, so DXVK no longer pins them to GENERAL, and the graph uses real layouts on them. With
    external home states (`CS_ORG_EXTERNAL_HOME`, on), every epoch returns them to that layout on the graphics queue, where DXVK
    uses them between the epochs. `CS_ORG_RESTING_LAYOUTS=0` (or `CS_ORG_EXTERNAL_HOME=0`) keeps the old GENERAL pinning.
    Memory dependencies against D3D11 are the queue-boundary barriers (below).

In the tours (2026-11-01), DXVK's command list is split 1.3 times a frame by the epochs (the Z-prepass's compute entry),
against 6 when every epoch enqueued its batches.

### The compute and copy queues

DXVK creates queues it never submits to, for ORG's off-stream kinds:

-   `DxvkOrgInteropDeviceInfo::computeQueue` is ORG's compute queue, the graph's alone.
-   `DxvkOrgInteropDeviceInfo::copyQueue` (interop version 5), a spare queue of the transfer-only family, is ORG's copy queue.
    Without one, ORG's copy queue shares the uploader's (`uploadQueue`). `CS_ORG_COPY_QUEUE=0`, or neither queue, leaves it
    aliasing the graphics one.

A pass that touches a resource only DXVK's families may use (an exclusive import) stays on the graphics queue.

-   **Entry.** Before an epoch's first submission to either, DXVK's stream signals `streamPoint` where it stands (one
    enqueued submission). Each queue's first submission of the epoch waits for it.
-   **Exit.** Every submission also signals its queue's exit timeline (`computeExit`, `copyExit`). The stream waits for both
    at the epoch's end.
-   **Submission.** Each queue's submissions go through one thread of its own (`ComputeSubmitter`), under DXVK's submission
    lock.
-   **Sharing the uploader's queue.** Nothing on a shared queue may block it:
    -   Graph copies wait for DXVK's stream, the uploader's batches wait for frames in flight, and the stream waits for both.
    -   A batch blocked on the queue holds back whatever is behind it. A graph copy ahead of the frame values' signal
        deadlocked the GPU (seen on 2026-11-01).
    -   So both threads wait for a submission's points on the CPU (`WaitForPoints`), and it reaches the queue with nothing left
        to wait for. Only a shared queue does this.
-   **Which queues can be the copy queue.** The RTX 5080's transfer family has two queues, DXVK's and the uploader's.
    -   DXVK's cannot be shared: its relocation barrier (`setSubmissionBarrier`) blocks that queue on the graphics queue.
    -   Never a compute family: a 256 MB upload took 144 ms during a 150 ms dispatch there, and 4.8 ms on the transfer family.
    -   Never a video family: a driver need not support copies there correctly. Visual corruption was seen with one.
-   **Kept on graphics.** The small copies inside an epoch's graphics work (LLF's `cs.llf.latch-lights`, the cull counter
    and foliage readbacks) prefer the graphics queue. On the copy queue they would only add a stream entry or a split.
    `ORG_PERSISTENT_COPIES_ON_COPY_QUEUE=1` moves every copy pass to the copy queue whatever it prefers, to exercise it.

### Enqueued epochs

Without DXVK's external commands (or with `CS_ORG_IN_STREAM=0`), the graph's batches go into DXVK's own command stream
(`dxvkEnqueueInteropSubmission`):

1.  BasicRHI hands each queue submission to the host through `QueueSubmissionHooks::submit` instead of
    calling `vkQueueSubmit`.
2.  The runtime forwards it to DXVK. On the immediate context, DXVK ends its current command list and
    queues the graph's batch right behind it, as an external entry on its submission thread.
3.  So the batch executes after every D3D11 command issued before the epoch and before every one issued
    after it. The render thread waits for nothing: no `FlushRenderingCommands`, no CS-thread sync.
4.  Memory dependencies are covered by the graph's queue-boundary barriers
    (`RenderGraph::SetExternalQueueBoundary`): a full memory barrier at the start of the epoch's first
    command buffer and at the end of its last one.

Submissions from any other thread (none in steady state) first wait until everything already in the
stream has reached the queue, then submit directly under DXVK's queue lock, so the graph's own order
is preserved.

BasicRHI's Vulkan `q_submit` serialises a submission only on its queue's `accessMutex`; it no longer holds the device-wide
`timelinesMutex` across the submit and the host hook. Before (2026-10-14), the render thread's epoch submission
(`ORG.Frame.SubmitPackets`) waited 20-40 ms behind a copy-queue upload (`CopyQueueUploadService`, which reaches the queue
through DXVK's `LockSubmissionQueue`); it is now at most 0.88 ms. `sc_present` still holds `timelinesMutex` across the
present (SARP and BasicRenderer only).

With a DXVK build that lacks the export (or `CS_ORG_SUBMIT=flush`), each epoch instead flushes and
waits for DXVK, then submits under the queue lock. This is undesirable, and will likely be downgraded 
to a failure in the future.

## Hazard: external Vulkan work directly after a graph epoch

**Rule.** No third-party Vulkan command buffer that goes through DXVK's interop may run on the GPU directly behind a graph
epoch's command buffer. That covers Streamline's DLSS, FSR, XeSS, DLSS-G and FSR-FG, and any future FidelityFX, NRD or vendor SDK.
DXVK work that binds image descriptors must sit between them. Today that is `Upscaling::SeparateFromGraphEpochs()`
(`src/Features/Upscaling.cpp`): call it, or a shared helper if it ever moves, immediately before such work. Found at the Sleeping
Giant Inn, 2026-10-12/13; the investigation record is in `dclf-open-defects.md` ("the Sleeping Giant Inn's black or garbage
upscaler output").

**Mechanism, as far as it is known.** This is inferred from black-box experiments, not proven.

-   A Streamline plugin requests `VK_EXT_descriptor_heap`; the log says "requested by a plugin(s) added".
-   With DXVK on descriptor heaps (NVIDIA's default until 2026-10-25; now opt-in), the render graph is granted that extension through `DxvkLoader`'s
    `dxvkRequestDeviceFeatures` (see Device features above). So Streamline's features run on the descriptor-heap path.
-   Graph epochs bind the graph's descriptor heaps.
-   Seen in that configuration (found when DXVK still ran on descriptor buffers beside the graph's heaps); not re-checked with
    the graph on descriptor buffers.
-   When a heap-path consumer's command buffer executes right behind an epoch's, NVIDIA's driver computes it from the wrong
    descriptors.
-   Unknown: whether this is a driver defect, or state or heap memory the graph leaves behind that the consumer relies on.

**Symptom signature** (to recognise it quickly):

-   The consumer's output alternates frame to frame: black (zeros), then garbage, then right. The garbage is values of ±65504 with
    alpha intact, which means the consumer's own arithmetic, not memory written by something else.
-   Its inputs and constants are correct. A readback inside the consumer's own command buffer proves this.
-   Identical with DLSS and FSR.
-   Goes away with a device idle immediately before or after the consumer, or with any DXVK descriptor-binding work in between.
-   Unaffected by queue choice, the consumer's in-flight depth, resource lifetimes and ORG's deletion depth.
-   Appears only when an epoch is the last GPU work before the consumer. In practice that means after adding an epoch, or after
    moving a pass late in the frame.

**What separates them and what does not** (all tested):

| Separates | Does not separate |
| --- | --- |
| A D3D11 UAV clear of a small typed texture (DXVK runs it as a compute shader that binds image descriptors) | A D3D11 flush |
| A device idle before or after the consumer | An empty interop submission |
| | A typed-buffer UAV clear (a buffer fill) |
| | Waiting for the epoch's completion |
| | Bounding the consumer's frames in flight (1 or 2) |
| | Idling only the upload or compute queue |

**Where it applies today.**

-   **The upscaler evaluate:** covered. `Upscaling::Upscale` calls `SeparateFromGraphEpochs()` before `EvaluateDLSS`/`EvaluateFSR`/XeSS.
-   **Frame generation:** *not covered*. That is `Streamline::EvaluateFSRFrameGen` (`Upscaling/Streamline.cpp`) and the DLSS-G path.
    Frame generation was off in every run of the investigation. A later check of the inn with it on showed no corruption in a few
    screen grabs, which does not rule the hazard out.
    With `CS_ORG_UPSCALING=1` the hudless copy is an epoch at the `FrameGeneration` anchor, a late anchor, so on that path the
    separator does run between it and frame generation's calls.
-   **Epochs that can end up last before the upscaler or Present.**
    -   DCLF's first-person colour epoch (`RenderFirstPersonView`) is the one that triggered this. It is the only epoch with no DXVK
        work after it before the upscaler.
    -   The forward epoch would be exposed too without the engine's textured draws that follow it.
    -   A new epoch, or one moved late in the frame, must be checked against this rule.

**How to check a change.**

-   The repro: `coc RiverwoodSleepingGiantInn` with DCLF and first person on, about 16 s and 5 screenshots. Without the separator,
    every run reproduced it.
-   `CS_DCLF_TARGET_DUMP` stages the upscaler output at every dump point, including "before upscale" and "after evaluate", and writes
    raw texels.
-   The in-command-buffer readback (`InCbReadback` in `Upscaling/Streamline.cpp`) copies the consumer's inputs and output inside its
    own command buffer. It runs on dump frames only: it allocates 512 MB host-visible and idles the device once.

**Open questions; the workaround stays until one of these resolves the cause:**

-   Run Streamline without `VK_EXT_descriptor_heap` (do not grant the plugin's request), or give it heap state separate from the
    graph's.
-   Have the graph's last command buffer re-bind or reset descriptor-heap state.
-   Check with a capture tool that supports `VK_EXT_descriptor_heap`. RenderDoc 1.46 does not, and CS's RenderDoc setup puts DXVK on
    legacy descriptor sets, so with RenderDoc loaded the graph turns itself off.
-   If it reproduces in isolation, report it to NVIDIA.

## Hazard: bindings the driver keeps across an in-stream epoch

**Rule.** After an in-stream epoch, DXVK must not rely on re-binding the same vertex or index buffer it had bound before the epoch.
`DxvkContext::invalidateExternalCommandState` (`extern/dxvk`) binds placeholder vertex and index buffers after every client's
commands, so DXVK's own binds before its next draw always reach the driver. Keep that step if the function is rewritten.

**What happened.** On the RX 6600 (AMD's Windows driver, 2026-11-10), every frame went black when a graph client ran at the Tonemap
anchor with FSR: Effects11's ORG runtime and Post Processing's chain both. The graph's output was correct. The engine's depth of
field after it, `ISHDRDownSample4` and `ISDistantBlur`, drew garbage: both are indexed draws of the engine's full-screen quad, whose
index buffer `ISRefraction` had also bound just before the epoch. The epoch's secondary command buffers bind the graph's own index
buffer. Vulkan leaves the primary's bindings undefined after `vkCmdExecuteCommands`, and DXVK does bind its own again, but the driver
compares against the bindings it recorded before the secondaries and drops the identical bind as redundant. The draw then reads the
graph's index buffer.

**Why it was hard to see.**

-   The core and synchronization validation layers report nothing: the API use is correct.
-   Barriers, layout transitions, descriptor rebinds and new descriptor ranges change nothing.
-   It goes away with anything that resets the driver's record: a new command buffer (a flush on either side of the epoch, a GPU wait
    that moves DXVK's later flush points), or a copy, which the driver runs as an internal blit that saves and restores its state.
    That made it look timing dependent, and recording reuse looked like the trigger because it changed where DXVK flushed.
-   TAA was unaffected: its first draw after the epoch uses another quad, so its bind always differs.

**How it was found.** An RGP capture of a black frame and of a fixed one (`fx-org1` against the same with a one-texel copy after the
anchor), compared at the level of the driver's SQTT user-data markers (command buffers, barriers with their cache flags, layout
transitions, debug labels). The copy showed up as an internal graphics blit between the epoch and the depth of field draw, which
pointed at driver state rather than memory. Binding a placeholder index buffer after the epoch alone fixed it; a placeholder vertex
buffer alone did not.

The descriptor-heap hazard above, with NVIDIA's driver, may be the same kind of state record surviving the graph's secondaries.

## External commands in DXVK's command buffer

`RenderGraphRuntime::EmitExternalCommands(record, user, waits, signals, bindsState, label, fullBarrier)` (render thread) queues
`record(user, VkCommandBuffer)` into DXVK's command stream (`dxvkEmitExternalCommands`): DXVK's CS thread runs it once, in D3D11
stream order, on its own command buffer, outside any render pass. DCLF's CPU-driven pipeline draws through it
(`Core/Render/Direct/Sites.cpp`; dclf-architecture.md, "The CPU-driven pipeline"). The contract:

-   **Entry.** DXVK has flushed its barriers (all earlier D3D11 work is visible) and every image is in its default layout
    (`DxvkOrgInteropImageInfo::layout`: its resting layout, GENERAL only for storage images and unified layouts), deferred
    clears done.
-   **Inside.** The callback begins and ends every rendering it opens. With `bindsState` DXVK forgets every binding it tracks
    afterwards (pipelines, descriptor buffers or heaps, push data, vertex and index buffers, dynamic state, render targets) and
    binds its own descriptors again, so the callback binds BasicRHI's heaps, layout and push data itself. BasicRHI wraps the
    buffer for recording (`rhi::vulkan::AdoptCommandBuffer`: no submission, no tracked layouts); secondaries recorded elsewhere
    (inside or outside a rendering, `SecondaryInheritance::insideRendering`) are executed with `ExecuteCommands`.
-   **Exit.** Every image is left in its default layout, and the callback's writes are released: its own barrier to all commands,
    or `fullBarrier` (DXVK records one global barrier). DXVK's barrier tracker is empty after the callback, so it orders nothing
    against what the callback wrote.
-   **Waits and signals** join the graphics submission of the command list the commands land in: a signal completes once that
    whole submission did (the callback's retirement point; values grow in stream order per semaphore). DCLF waits for the frame
    values' timeline and signals its CPU-frame timeline once a frame, behind the frame's callbacks.
-   **Lifetime.** DXVK tracks nothing the callback uses: every D3D11 object, view and buffer it reads or writes is held until its
    signal completes. ORG's staged uploads a callback depends on are recorded on the render thread into a secondary
    (`RenderGraph::RecordPendingUploads`) and executed at the callback's start, so DXVK's list is not ended for them. ORG's latch
    regions a callback's work reads are written at an external frame slot (`GraphHost::BeginExternalFrameSlot`), reused only
    after its last frame's signal.
-   Once the path has been used, DXVK no longer reorders transfers or layout transitions on interop images, or on buffers with a
    stable address, ahead of the command buffer.
-   **One descriptor model.** BasicRHI and DXVK always run on descriptor buffers together (Runtime shape, "Descriptor buffers,
    negotiated"; `[ORG] Binding model:` in CS's log, `Binding model:` in DXVK's). The two models in one command buffer, BasicRHI
    on heaps and DXVK on descriptor buffers, made NVIDIA (RTX 5080, driver 621.01) fetch garbage descriptors in the callback's
    draws. Those were MMU faults with no owning resource, depending only on where in DXVK's stream the callback landed, even with
    every command in secondaries.
-   **Heaps bound in DXVK's buffer too.** A secondary that binds its own heaps is not enough. On the same GPU, when the graph ran on heaps, a
    secondary's heap binding did not hold while the buffer executing it had DXVK's heap bound, and the draws fetched DXVK's
    descriptors at the graph's indices (MMU faults with no owning resource; a callback at the start of a command list, before DXVK
    bound a heap, ran clean, and a separate command buffer within the list did not help). The sites bind the graph's heaps
    (`SetDescriptorHeaps`: the descriptor buffers and their offsets) in DXVK's buffer before each execution of their
    secondaries (`Direct::SiteOut::BindHeaps`), kept as the safe assumption on descriptor buffers. DXVK binds its own descriptor buffers again after the
    callback, and re-sets its descriptor buffer offsets at its next pipeline bind.

## Resources crossing the boundary

-   **Graph outputs** (LLF's `lights`, `lightIndexList`, `lightGrid`) are graph-owned buffers. D3D11 reads them
    through wrappers from `dxvkCreateBufferFromVkBuffer`. DXVK never renames, relocates or maps a
    wrapped buffer, and only `D3D11_USAGE_DEFAULT` without CPU access is accepted. Graph textures D3D11 still reads are
    wrapped the same way by `dxvkCreateTextureFromVkImage` (below, "Graph-backed feature resources").
-   **Game or DXVK resources** are not pinned or otherwise changed. Inputs the graph needs are either
    uploaded as copies (LLF's light list) or, for future passes, imported each epoch with the handle DXVK
    currently uses.

## Graph-backed feature resources

`src/RenderGraph/GraphResources.h` has graph twins of `Buffer.h`'s wrappers for ported features: `Texture2D`, `Texture3D`,
`StructuredBuffer` and `Buffer`. Constant buffers have none: a pass's constants are a `LatchBlock` or push constants.

-   **Graph side.** Each twin owns an ORG resource named as the D3D11 wrapper names its resource, `Feature::Name` (ORG reads the
    `::` as namespaces). The client registers it in `registerResources` (`tex.Register(graph)`), and its passes declare `Graph()`.
-   **Lifetime.** `Options::lifetime` is `Transient`, `History` or `Persistent` (org-aliasing-port-priorities.md, "Lifetime
    classes"). A transient resource is aliasable: an unmaterialized `PixelBuffer` or buffer with `allowAlias`, placed by the graph.
    Its first use in each frame must be a write. The others are materialized when made.
-   **D3D11 export.** `Options::d3d11` (`Export::SRV`, `UAV`, `RTV`) adds D3D11 views of the same memory for readers not ported
    yet: an engine draw that binds the output as a PS SRV, or another feature. They are named `Feature::Name SRV` / `UAV` / `RTV`
    (`Util::SetResourceName`), the texture or buffer `Feature::Name (D3D11)`.
    -   A buffer goes through `RenderGraphRuntime::WrapBuffer`.
    -   A texture is a BasicRHI image wrapped by `RenderGraphRuntime::WrapTexture` (`dxvkCreateTextureFromVkImage`, interop
        version 7). The graph sees it as a host-owned texture whose home state is the layout DXVK rests it in, so every epoch gives
        it back in that layout, as for DXVK's own images (DxvkImageImport). DXVK zeroes it in its stream when it is wrapped.
    -   The image has no mutable format, so D3D11 views use the texture's own typed format. 3D textures take no RTV.
-   **Rule: exported means not aliasable**, whatever the lifetime. D3D11's use is outside the graph, which would otherwise reuse
    the memory across it. A resource becomes aliasable when its last D3D11 reader is ported and the export goes.
-   **Teardown and resize.** `Reset`, the destructor and `Resize` release the D3D11 views and resource first. Then
    `RenderGraphRuntime::ReleaseAfterD3D11` holds the graph resource until an event query issued after that release completes
    (checked every frame). When D3D11 cannot make the query then, the owner is parked and a later frame issues it: nothing waits,
    and the query still follows the release in the stream. DXVK keeps its own objects alive for in-flight work, but not the image's memory, which the graph owns.
    A `Resize` bumps the twins' resize generation (`Resource::ResizeGeneration`). At their next run the anchors see it, build
    their graphs again with the new resource, and `PublishedResources::Refresh` publishes it again under the twin's id: the owner
    calls nothing. A pass reads `Graph()` when it is made, never across builds.

```cpp
// A transient output written at DeferredPasses and bound by engine draws as a PS SRV until P5.
GraphResources::Texture2D out(desc /* RGBA16F, SRV | UAV */, "MyFeature::Output",
	{ .lifetime = GraphResources::Lifetime::Transient, .d3d11 = GraphResources::Export::SRV });
FrameAnchors::Register({ .id = "cs.myfeature",
	.passes = { { .name = "cs.myfeature.resolve", .anchor = FrameAnchors::Anchor::DeferredPasses, .order = 100,
		.kind = FrameAnchors::PassKind::Compute, .make = [&] { return std::make_shared<MyPass>(out.Graph(), program); } } },
	.registerResources = [&](org::RenderGraph& g) { out.Register(g); } });
// Later in the frame, the D3D11 reader:
ID3D11ShaderResourceView* srv = out.SRV();
context->PSSetShaderResources(45, 1, &srv);
// At a resolution change: the anchors build again and the published id follows by themselves.
out.Resize(width, height);
```

`CS_ORG_GRAPHRES_SELFTEST=1` proves the path: a compute pass at DeferredPasses writes an exported texture, D3D11 copies it into
a staging texture right after the epoch and reads one texel back, then the texture is resized (the first one released through
`ReleaseAfterD3D11`) and checked again.

### Publishing a graph resource to other epochs

A client's `registerResources` registers its twins with the anchor graphs only. To let a pass in another epoch read one (DCLF's
colour passes reading a feature's output), the producer publishes it (`src/RenderGraph/PublishedResources.h`):

-   `PublishedResources::Publish(id, twin)` registers the twin's graph resource under `id` with every epoch's graph: the anchors' and
    DCLF's. It uses ORG's own cross-epoch machinery (persistent-epochs.md, "What ties the graphs together"): every graph admits
    against the runtime's one `ResourceSequencer`, so a read declared in a later epoch waits for the producer's write and enters
    the state it left. Nothing is copied.
-   Only a resource the graph never aliases can be published: `History`, `Persistent`, or exported. An aliased transient keeps no
    contents across executions, and the reader's epoch is another execution. Publish refuses an aliasable twin.
-   A reader in an epoch earlier in the frame than the producer's reads the previous frame's contents.
-   Publishing a new resource under an id (the first time, or after a `Resize`) adds the host extension
    `cs.org.published-resources` again, which rebuilds every graph once. After a twin's `Resize` the anchors do it
    (`PublishedResources::Refresh`); the producer need not publish again.
-   `Withdraw(id)` when the producer leaves its ORG path (its `onFailed`, a setting that turns it off). Readers then go back to D3D11.
    The resource stays registered until the next `Publish` under that id, because a graph built earlier may still declare it.
-   `ShaderResourceIndex(id, index)` gives the bindless index of the whole-resource SRV (every mip; a cube's `TextureCube` view, a 2D
    array's full array view), false while it is not published. That index is what a frame record holds for a bindless reader.

```cpp
// The producer, after creating its output:
out = std::make_unique<GraphResources::Texture2D>(desc, "MyFeature::Output",
	GraphResources::Options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV });
PublishedResources::Publish(*out);  // id "MyFeature::Output"
// Its client's onFailed:
PublishedResources::Withdraw("MyFeature::Output");
```

DCLF reads a published texture through its frame texture contract (`Api::FrameTexture::graphResources`; dclf-architecture.md,
"Frame graph textures"): when the view a commit resolved is the id's export (`PublishedResources::IsExportOf`).

## GPU timing in the profiling window

The profiling window's D3D11 timers are timestamp query pairs, which DXVK writes at
`VK_PIPELINE_STAGE_ALL_COMMANDS_BIT`: each one is written once everything submitted before it has finished.
Inside one DXVK command list, a pair therefore measures exactly the GPU work between them. A pair whose
two halves land in **different submissions** also counts the time the queue sat idle between them,
waiting for the CPU to submit the second one. How long that is depends on how far the CPU is behind the
GPU, not on the pass.

Measured on the auto-loaded save (FSR at 2560x1440 to 3840x2160). The old `Upscaling::Upscale` timer
bracketed the Streamline evaluation, which is submitted as its own command buffer after DXVK flushes:

| | D3D11 pair around the evaluation | Timestamps inside the evaluation buffer |
| --- | --- | --- |
| DCLF off | 4.77 ms | 1.87 ms |
| DCLF on | 2.09 ms | 1.91 ms |

With DCLF off the render thread is further behind the GPU, so the queue waits longer for the next command
list. `LightLimitFix::RenderGraphCull`, a D3D11 timer around the graph's epoch, had the same problem
(0.21 ms and 0.58 ms against 0.02 ms of graph work).

Where the timings come from now:

-   Work submitted outside DXVK's command lists is timed inside its own command buffers, never by a D3D11
    pair around it. Render-graph passes report through the graph's statistics
    (`<Feature>::<segment> / <pass>`). The Streamline interop ring writes a timestamp pair into each
    labelled buffer (`DXVKInterop::BeginFrameCommandBuffer(label)`), which feeds `Upscaling::Upscale`.
-   The DXVK fork exports `dxvkGetSubmissionCounter`, the immediate context's count of command lists it has
    submitted. The profiler reads it at each timer's begin and end, and flags a timer whose halves landed
    in different submissions. Implicit flushes cause this too: `Skylighting::OcclusionMask` renders the
    engine's precipitation mask, thousands of draws, and DXVK flushes partway through it in about 90% of
    frames. The window marks a timer with `*` when most of its recent samples were split (GPU mode only).
    `CS_PROFILER_LOG` prints `[split N%]`.
-   The reverse also happens. DXVK moves a copy or clear into the current command list's init buffer when
    none of its resources has been used in that list yet (`prepareOutOfOrderTransfer`), so it runs before
    every timestamp in the list. A D3D11 timer around such an operation reads about zero, and the
    operation's time appears in no timer. The copy of the upscaler's output into `kMAIN`, the first
    operation after the interop flush, is one (estimated from bandwidth at about 0.1 ms at 4K, not measured), so it has no timer.

## GPU timers from the graph

The end state has no D3D11 and no DXVK, so every GPU timer must come from the graph's own timestamp queries (ORG's statistics
service on BasicRHI query pools). `src/RenderGraph/GpuTimingSource.{h,cpp}` is that source. Two switches pick it; both keep
the D3D11/DXVK path as the default until the DXVK-free build.

-   `CS_GPU_TIMERS=graph`. The profiler (`Profiler.cpp`) makes no D3D11 query and needs no D3D11 device. Its `BeginPass` /
    `EndPass` ranges keep their CPU times. The graph's passes report exactly as before, through `AddExternalSample`, and are
    averaged over the frames since the last collection. While D3D11 work remains, its ranges show no GPU time here: the
    window's GPU total is the graph's work only. `CS_GPU_EVENT_TIMERS` logs the graph's ranges per Present instead of the engine's
    events: `[GpuEvents] graph N frames ...` with the graphics queue's span from the first graph range to the last, each queue's
    time inside graph ranges, then per range the inclusive and self (along its queue) time and calls. `dclf:gpu-window` windows work
    the same way. `CS_GPU_LOAD` becomes a compute pass at the Present anchor (`GpuLoadPass.cpp`, same shader arithmetic).
-   `CS_GPU_IDLE_SOURCE=graph`. `CS_GPU_IDLE_TRACE` takes its busy spans from the graph's graphics-queue ranges instead of
    `dxvkSetSubmissionTrace` / `dxvkReadSubmissionTrace`. The analysis and the log are the same.

Each epoch submission (`MarkEpoch`) records its host frame, the Present it belongs to and the CPU time. The completed-frame callback
adds that host frame's timestamp pairs (`IStatisticsService::GetCompletedTimestamps`: passes, barrier batches of kept recordings,
sub-ranges) to the Present. A Present is handed on once every epoch submitted in it has completed (given up after 32 Presents).

What the graph source cannot see, compared with the DXVK trace:

-   DXVK's own command lists. While the engine still draws through D3D11, the time between graph ranges is D3D11 work, not idle.
    The idle trace on the graph is only an idle trace once the whole frame is graph passes.
-   Work the graph does not time: barrier batches (timed only in recordings kept across frames), the kept recordings
    themselves unless `CS_ORG_RECORDING_TIMESTAMPS=1`, upload lists, the present and the presenter's blit.
-   The exact queue submission time. The graph knows when the render thread submitted the epoch, which is no later than the
    queue submission, so it still bounds the GPU-to-CPU clock offset but less tightly. BasicRHI enables
    `VK_EXT_calibrated_timestamps` only for Tracy and does not expose it, so there is no exact calibration yet.
-   The flush type and the app, CS thread and queue times of each DXVK submission, and the compute queue's idle time (the trace
    covers the graphics queue).

Remaining DXVK dependencies of the timers and diagnostics, all on the default path: `dxvkGetSubmissionCounter` (the
profiler's split flag), `dxvkSetSubmissionTrace` / `dxvkReadSubmissionTrace` (the idle trace), and the Streamline interop
ring's own timestamps (`DXVKInterop`, `Upscaling::Upscale`), which use DXVK's device and `dxvkEnqueueInteropSubmission`.
`CS_PASS_SKIP`, `CS_GPU_OP_PROBE` and `CS_GPU_EVENT_STATS` hook or query the D3D11 context and have no graph equivalent.

## Frame anchors: attaching feature passes

A feature that moves its GPU work onto the graph attaches its passes to a frame anchor (`src/RenderGraph/FrameAnchors.h`) instead
of adding a `Segment` and editing the epoch order. Each anchor is one of CS's hook points:

| Anchor | Hook |
| --- | --- |
| `ReflectionsPrepass` | `Deferred::ReflectionsPrepasses` (`BSCubeMapCamera::RenderCubemap`) |
| `EarlyPrepass` | `Deferred::EarlyPrepasses` (after `Main::RenderShadowMaps`) |
| `DepthDone` | `TerrainBlending::BlendPrepassDepths` (`Main::RenderDepth` returned; only with Terrain Blending on) |
| `Prepass` | `Deferred::PrepassPasses` (the main pass's first opaque batch) |
| `DeferredPasses` | `Deferred::DeferredPasses`, after the final depth copy |
| `DeferredComposite` | `Deferred::DeferredPasses`, after the D3D11 SSGI, SSS and cubemap update, where the composite runs |
| `PostComposite` | the end of `Deferred::DeferredPasses`, after the composite (D3D11 or ORG), where Effects11's rays draw |
| `PostProcessing` | `Upscaling::Main_PostProcessing`, before the upscaler |
| `Upscale` | `Upscaling::PerformUpscaling`, after the upscaler's evaluate, before the ImageSpace chain |
| `Tonemap` | `Main_HDRTonemapBlendCinematic_Render` (`Hooks.cpp`), through the seam `RenderGraph::Engine::RunTonemapAnchor` (the FX runtime's and Post Processing's preparation, then the run) |
| `FrameGeneration` | `Upscaling::CopyHudlessColor`, after the ImageSpace chain, where frame generation's hudless colour is captured |
| `Present` | `IDXGISwapChain_Present` (`Hooks.cpp`), before DCLF's `BeforePresent` |
| `Overlay` | `OverlayRenderer::FinalizeImGuiFrame`, where CS's ImGui frame is drawn (`ImGuiRenderer::RenderDrawData`) |
| `HDROutput` | `HDRDisplay::ApplyHDR`, HDR's output composite inside its present chain (run only by `CS_ORG_HDR_DISPLAY`) |
| `Capture` | `ScreenshotFeature::ProcessCaptureRequest`, at the end of HDRDisplay's present chain, before the native Present |

Each anchor's ordering rule is in [org-extension-api-notes.md](./org-extension-api-notes.md), "Attaching passes to the frame".

**How a feature uses one.**

1.  Before the graph initializes (`Load` or `PostPostLoad`, when its ORG path is on): `FrameAnchors::Reserve(anchor)`.
2.  Once the runtime is active (`SetupResources`): make its programs and resources, then `FrameAnchors::Register(client)`. A
    client is an id, its passes (name, anchor, order key, kind, and a `make` that returns a new `TypedRenderGraphPass` at each
    build), and optional callbacks: `registerResources`, `commit` (latch writes inside the epoch), `active`, `onFailed` and
    `executed`, and its run gates (below, "Per-run work").
3.  The passes declare what they read and write as any graph pass does. Engine targets come from
    `FrameAnchors::EngineTarget(RE::RENDER_TARGET)` and `EngineDepth(...)`: DXVK images imported once (`DxvkImageImport`) and
    registered with every anchor graph as `cs.engine.target.<n>` and `cs.engine.depth.<n>`. Cubemap targets come from
    `EngineCubemap(RE::RENDER_TARGETS_CUBEMAP)` (`cs.engine.cubemap.<n>`, a cube default SRV), and a feature's own D3D11
    texture that is not ported yet from `D3D11Texture(name, texture, srv, uav)` (`cs.d3d11.<name>`; Screen Space Shadows reads Terrain Blending's blended depth through it). Every import
    describes all its mips and slices; a `D3D11_RESOURCE_MISC_TEXTURECUBE` texture gets cube views.
4.  When the graph fails, `onFailed` is called and the client is removed; the feature returns to its D3D11 path.
5.  CS's shared constant buffers (FrameBuffer, SharedData, FeatureData, Permutation): set `.sharedConstants = true`. The anchor's
    epoch then writes their values into a `LatchBlock` at the hook (the bytes D3D11 sees there: CS's cached `PerFrame` mapping and
    State's last uploads) and copies them into four CBV buffers (`cs.shared.*`) before the anchor's passes. Such a client may
    register before CS's first upload has sized the buffers: it is held, and joins the graphs at the first run after they can
    be made (or gets `onFailed` if they never can). A pass declares them
    with `SharedConstants::Declare` and pushes `SharedConstants::Index` as the shader's `SHARED` entry. `CS_ORG_SHARED_CONSTANTS_CHECK=1`
    reads the D3D11 buffers back after each such epoch and logs whether the bytes match (a debug switch: it stalls).
6.  Textures D3D11 features produce (not engine targets) can be read in place, untracked, through
    `DCLF::GpuTextures::ResolveBinding(srv)` (the ORG deferred composite does, `src/ORGDeferredComposite.cpp`). Keep the binding's
    owner until no submission can read it: the recording that names it retains it (`RecordingContext::Retain` at record time),
    which covers every resubmission of a kept recording, with no frame count.

```cpp
// A compute pass at DeferredPasses that reads kMAIN and writes a graph texture.
struct Bindings { org::DeclaredViewToken main, out; };
class MyPass final : public org::TypedRenderGraphPass<MyPass, Frame, Bindings> {
	Bindings Declare(org::PassBuilder& b) {
		b.PreferQueue(org::QueueKind::Graphics);
		return { b.ShaderResource(main).View(), b.UnorderedAccess(out).View() };
	}
	// Revision, Prepare and Record as in ORGLightCulling.cpp: bind program->layout and Pipeline(), push indices, Dispatch.
};

FrameAnchors::Reserve(FrameAnchors::Anchor::DeferredPasses);  // at load
auto main = FrameAnchors::EngineTarget(RE::RENDER_TARGET::kMAIN);
FrameAnchors::Register({ .id = "cs.myfeature",
	.passes = { { .name = "cs.myfeature.resolve", .anchor = FrameAnchors::Anchor::DeferredPasses, .order = 100,
		.kind = FrameAnchors::PassKind::Compute, .make = [=] { return std::make_shared<MyPass>(main, out, program); } } },
	.registerResources = [=](org::RenderGraph& g) { g.RegisterResource(org::ResourceIdentifier("cs.myfeature.out"), out); } });
```

**How anchors map onto epochs.**

-   Each anchor is a `Segment` of its own (`Segment::Anchor*`), and a reserved one is an epoch: in the host's epoch order (before
    the first epoch the frame runs after it), a live async epoch, and in DXVK's stream with the others.
-   `RenderGraphRuntime::Initialize` asks `FrameAnchors::AddToFrame` for the reserved epochs, then `FrameAnchors::Attach` adds one
    host extension (`cs.org.frame-anchors`). At each build it gives an anchor's graph that anchor's passes, sorted by key, and the
    clients' resources. Other epochs' graphs get nothing from it.
-   A client change rebuilds only its anchors' epochs (`GraphHost::RequestRebuild(epoch)`). DCLF's condition changes no longer
    rebuild them (`RenderGraphRuntime::IsDclfSegment`).
-   **Cost.** An anchor nobody reserved has no epoch: no graph, no frame slots, and its hook is one load. A reserved anchor whose
    clients are all inactive (`active`) submits nothing at its hook; its ticket waits, prepared, for the next run.
-   **Frames in flight.** The host's frame slots are frames in flight times the epochs, so every reserved anchor adds
    `kGameFramesInFlight` slots. Size `LatchBlock`s from `GraphHost::FrameSlots()` after the runtime is up, as LLF does. An anchor that
    runs several times a frame (`ReflectionsPrepass`, at each cubemap render) uses its ring that much faster, and may wait for a
    slot when the CPU is far ahead of the GPU.
-   **The hazard rule.** After the late anchors (`PostProcessing`, `Upscale`, `Tonemap`, `FrameGeneration`, `Present`, `Overlay`,
    `HDROutput`, `Capture`) have run an epoch, `Run` calls the late-epoch hook, which Upscaling installs in `PostPostLoad`
    (`FrameAnchors::SetLateEpochHook`, `Upscaling::SeparateFromGraphEpochs`). An upscaler, frame generation or the present can
    follow these, and none may run directly behind a graph epoch (above, "Hazard: external Vulkan work directly after a graph
    epoch"). The earlier anchors are followed by the engine's own draws. The anchors' core calls no feature and no self-test:
    the self-tests reserve in `RenderGraphRuntime::Initialize` and register as ordinary clients at the first frame's end
    (`SignalFramePace`), and check their runs from `executed`.
-   **Imports.** An import is kept while its D3D11 texture stays the same, and replaced when it changes. A feature texture that
    is destroyed or renamed is dropped with `FrameAnchors::ReleaseImport(id)` (`cs.d3d11.<name>`): the anchors build again
    without it, and the texture goes through `ReleaseAfterD3D11`.
-   **Not shared with DCLF.** DCLF keeps its schedule (`Schedule/Frame.cpp`, `Api/Passes.h`): its passes depend on its scene
    revisions and recorded shapes. Anchors are the thin layer for every other feature. DCLF's imports and the anchors' imports of
    the same engine image are separate graph resources; that is safe because they never share an epoch.

### The setup template

Every port sets up the same way:

```cpp
// PostPostLoad, when the ORG path is requested: the frame's epochs are fixed when the graph initializes.
static void Reserve() { if (Requested()) FrameAnchors::Reserve(FrameAnchors::Anchor::DeferredPasses); }

// SetupResources: programs, resources and the client. False keeps the D3D11 path; a_onFailed runs if the graph fails later.
bool Setup(rhi::Device a_device, std::function<void()> a_onFailed);

// Every frame, before the D3D11 path would run: the graph path only while the runtime is still up.
bool IsActive() const { return registered && RenderGraphRuntime::Get().IsActive(); }
```

-   `Register` may be called before CS's first constant upload, also by a client that reads the shared constants (above).
-   A resize needs no call: the anchors follow the twins' resize generation.
-   The client's `onFailed` clears what `IsActive` reads, withdraws what it published, and calls `a_onFailed`.

### Per-run work, one-shot passes and the executed signal

Three primitives replace the per-port workarounds (mismatch counters pushed into `Revision`, `prepared` atomics set in `Prepare`,
fill and clear frame counts). A pass's revision stays a function of what it was made with, so its recording is kept.

-   **Run gates** (`src/RenderGraph/RunGate.h`). A dispatch or draw that runs on some runs of its anchor only records
    `RunGate::Dispatch` (`ComputeProgram::DispatchIndirect`) or a `RasterProgram::DrawDesc` with `indirect` set to the gate's
    latch and offset. The group or vertex counts are in a latch block, so a zero there does no work. List the gate in
    `ClientDesc::gates`: each run of the client's anchors closes it before any commit, and the commit opens the entries this run
    does (`OpenDispatch`, `OpenDraw`). An inactive client, or a commit that opens nothing, therefore costs a kept recording with
    no work, and never repeats an older run's counts. A copy has no indirect form: one that runs on some runs only is a raster
    copy through a gate, or a separate one-shot pass.
-   **One-shot passes** (`PassDesc::once`). A clear, an upload or a fill. The pass is in its anchor's graph until one run with it
    has been submitted, then the graph is built again without it. `FrameAnchors::Rearm(id)` makes the client's one-shot passes
    pending again (a reset, a resize that needs a clear). The anchor runs while one is pending, even with no active client. Its
    work must give the same result if it ran twice. The helpers in "Pass helpers" below make the clear and the upload.
-   **The executed signal** (`ClientDesc::executed`). Called on the render thread after a run whose epoch was submitted with this
    registration's passes in its graph (built from this registration, or from a later `Refresh` or resize) and whose commit ran.
    What that commit wrote is then in a submitted execution, and every later GPU use of the client's resources is ordered after
    it. Use it for work that counts as done once it ran (a regenerated table, a history reset), instead of counting frames.

How the anchors know: every `Publish` of the clients has a serial, each client remembers the serial that first held its passes as
they are now, and an anchor graph's build records the serial it was made from. A run's commit reads that serial, so it compares
two numbers instead of watching passes prepare.

### Pass helpers

`src/RenderGraph/AnchorPasses.h` makes a client's common passes from plain descriptions, generalised from Post Processing's chain
builder with the anchor, the pass-name prefix and the order keys as parameters:

-   `Draw`: a fullscreen pixel stage into one target or one mip of it (`RasterProgram`), optionally through a run gate entry.
-   `Dispatch`: a compute program with fixed group counts, a run gate entry, or group counts at a latch offset.
-   `Copy`: a whole texture's first mip into another of the same size and format.
-   `Clear`: a UAV clear to a float or uint value, a one-shot pass by default.
-   `Upload`: a staging buffer into one subresource of a texture, always a one-shot pass (a heightmap, a lookup table).
-   `Bind` lists a pass's push block in the shader's resource order: `Srv`, `Uav` (whole or one mip), `Handle`, `Latch` (an offset
    into the client's latch), `Shared` (the shared constants; `ReadsSharedConstants` then tells the client to set
    `sharedConstants`).

`Builder::Take` returns the `PassDesc`s for `ClientDesc::passes`. Each pass's revision is its immutable spec, so its recording
is kept; per-frame values reach it only through the latch and the gate. Programs add the D3D11 defines themselves ("Raster
programs", Defines); `AnchorPasses::Defines` widens a port's own list.

`ComputeProgram` owns its dispatch command signature (`DispatchIndirect`, from a buffer or a latch region of the frame slot),
and `RasterProgram` its draw signature (`DrawDesc::indirect`). `ShaderBindings::SamplerFromD3D11` (and `CreateSampler` of a
`D3D11_SAMPLER_DESC`) is the one D3D11-to-rhi sampler converter.

## Raster programs

`RasterProgram` (`src/RenderGraph/RasterProgram.h`) is `ComputeProgram`'s raster twin, for fullscreen vertex and pixel shader
passes on ORG render passes. It replaces `PostProcessing/RasterPass` and the pixel-shader passes in Upscaling and Effects11 as
they are ported.

-   **Compile.** Both stages compile with DXC through the same path (`ProgramBuild.{h,cpp}`, shared with `ComputeProgram`):
    ORGModuleServices' shader store, SPIR-V, BasicRHI's bindless ABI. The default vertex stage is `RenderGraph/FullscreenVS.hlsl`,
    a three-vertex triangle with `TEXCOORD0` as in PostProcessing's `fullscreen.hlsli`.
-   **Defines.** Compute and raster programs get the defines `Util::CompileShader` gives the D3D11 build, after their own:
    CS's common shader defines (`Util::GetCommonShaderDefines`), the stage's (`COMPUTESHADER`, `VSHADER`, `PSHADER`), `WINPC`
    and `DX11` (`ProgramBuild::WithCommonDefines`). A dual-target shader then compiles the same permutation on both paths. A
    name the program already gives is not added again. They are resolved once at `Load`, so a hot reload keeps them.
    `Desc::commonDefines = false` opts out (the FX runtime's generated stages, which Effects11 builds without them).
-   **State.** `Desc` gives up to eight colour targets, each with its format and `rhi::BlendAttachment`, an optional depth format
    and `DepthStencilState` (off by default), the `RasterState` (no culling by default) and the push-block size. The push block is a
    `cbuffer` at b0 that both stages read.
-   **Draw.** A pass declares its targets (`RenderTarget`, a mip or slice through `RtvView`) and depth (`DepthRead` or
    `DepthReadWrite`), then calls `Draw` at record time with the declared views, load ops and clear values, the viewport, and the
    constants. The viewport is the render area, so a mip's size is set there.
-   **Hot reload.** As for compute programs: registered with the pipeline registry with both sources as dependencies.
-   **Blend factor.** A kept recording fixes the blend constants, so a factor that follows a setting cannot be one. The ORG
    COD Bloom upsample, which D3D11 blends by `D3D11_BLEND_BLEND_FACTOR`, returns the factor in alpha and blends by `SrcAlpha`
    with alpha masked out of the write (`ORG/ORGCODBloom.cpp`).
-   **Not covered yet:** a viewport smaller than the render area, or a separate scissor, waits for P0.4.

## ImGui on the graph

With `CS_ORG_IMGUI=1`, CS's ImGui frames are drawn by a render-graph pass instead of `imgui_impl_dx11`
(`src/RenderGraph/ImGuiBasicRHI.{h,cpp}`, chosen in `src/Menu/ImGuiRenderer.{h,cpp}`).

-   **When.** `imgui_impl_dx11` draws the first frames: the graph starts after the menu. Once the runtime is active with the
    `Overlay` anchor's epoch (reserved in `Menu::Init`), the next `NewFrame` shuts the D3D11 backend down and installs the
    graph's. ImGui then creates its textures again for it. If the graph fails or a frame cannot be drawn, the D3D11 backend is
    installed again for the rest of the session.
-   **Target.** The render target bound when the overlay draws. That is the engine's framebuffer (`EngineTarget(kFRAMEBUFFER)`),
    or HDR's UI texture, which is imported as `cs.imgui.target`. A new target refreshes the client. With the owned frame, the
    framebuffer is the guest back buffer that `OwnedFrame` copies into the BasicRHI swap chain image, so the pass draws on the
    final image there too.
-   **Textures.** ImGui's own textures use 1.92's protocol (`RendererHasTextures`):
    - The font atlas pages are BasicRHI textures that the backend creates, updates by rectangle and destroys. The graph does
      not track them: the pass records their copies and barriers itself, and the shaders read them by descriptor index.
    - D3D11 views used as `ImTextureID` get a descriptor at commit time. Icons and cursors use their ORG copies, made from the
      pixels `IconLoader` keeps (`IconLoader::OrgBinding`; one view per mip, as `ViewForSize` picks). Any other view (a preset
      screenshot, a feature's debug view) is DXVK's image imported by `DCLF::GpuTextures::ResolveBinding`.
-   **Per frame.** The client's commit copies the vertices, indices and texture rows into one host-visible staging buffer, a
    `LatchBlock` from a small pool. It stores the frame in the host frame slot. The pass takes its slot's frame at record time
    and keeps it, with its textures and descriptors, until the recording's GPU work retires (`RecordingContext::Retain`). The
    staging buffer then goes back to the pool. The vertex stage reads the geometry by index (`RenderGraph/ImGui.hlsl`). Each
    command gets its own scissor (`SetScissors`) and push block.
-   **Not drawn.** Draw callbacks (`ImDrawList::AddCallback`), which are D3D11 code. `ImDrawCallback_ResetRenderState` needs
    nothing. The screenshot editor's opaque preview blend is the one callback CS has: its preview is alpha-blended on this
    path.

## Shader bindings for ported passes

A feature shader that still has a D3D11 path builds for both from one source: `Common/OrgResources.hlsli` turns its resource
list into register declarations under FXC, and into push-block handles and descriptor-heap fetches under DXC
(`CS_ORG_BINDLESS`, which `ProgramBuild` defines for every ORG compile). How to write one, the limits and the checks are in
[shader-workflow.md](./shader-workflow.md), "Dual-target shaders".

On the C++ side (`src/RenderGraph/ShaderBindings.h`):

-   **The block.** A struct that mirrors the shader's push block: one `ShaderBindings::Handle` per resource entry in list order,
    then the `PUSH` structs as `alignas(16)` members. `ShaderBindings::Words<Block>()` is its size for `constantWords`, checked
    at compile time against the 32-word limit.
-   **Handles.** At record time from the pass's declared views: `a_recording.Index(token)`. A `CBUFFER` entry is a declared
    `PassBuilder::ConstantBuffer` use. Samplers are not graph resources: `ShaderBindings::CreateSampler` at setup returns an
    owned binding, kept as long as the program, whose `Index()` is the handle.
-   **Push.** `ComputeProgram::Dispatch(a_recording, ShaderBindings::Data(block), x, y, z)` binds the program, pushes the block
    and dispatches. Raster passes give `ShaderBindings::Data(block)` as `DrawDesc::constants`.
-   **DXIL.** The ORG expansion compiles to DXIL unchanged (`tools/check-dual-target-shaders.py` builds both). BasicRHI's D3D12
    layout already matches it: the program's push range (set 0, binding 0) becomes root constants at `b0`, and the root
    signature has `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` and `SAMPLER_HEAP_DIRECTLY_INDEXED` (`rhi_dx12.cpp`). What P7.2 still
    needs on CS's side: the binary format as a parameter of `ProgramBuild::MakeRequest` (SPIR-V is fixed there today, with
    `BASICRHI_SHADER_API_VULKAN`).
## Owned frame

`CS_ORG_OWNED_FRAME=1` inverts who owns the device ([org-full-port.md](./org-full-port.md), Phase 1). BasicRHI creates the
Vulkan instance and device, DXVK runs the game's D3D11 on them as a guest, and ORG owns the swap chain and Present. Off by
default: DXVK creates the device and presents, and the graph adopts it as above. The code is in `src/RenderGraph/Engine/`.

### Device creation (`GuestDevice`)

At the game's `D3D11CreateDeviceAndSwapChain` (CS's hook in `Upscaling.cpp`), before DXVK's own creation:

1.  `dxvkGetGuestInstanceRequirements` reports DXVK's instance extensions and its engine name ("DXVK_HDR" when HDR is configured,
    the NVIDIA profile trick in `extern/dxvk/FORK.md`).
2.  `rhi::CreateVulkanDevice` creates the device (Streamline's interposer off), on descriptor buffers only
    (`DeviceCreateInfo::vulkanBindingModels`), with room left for DXVK's sampler pool and descriptor buffers, and with BasicRHI's
    crash markers where DXVK's hang mode would be on (`DXVK_DEBUG=hang`, `CS_GPU_CRASH_ANALYSIS`).
3.  BasicRHI's creation hooks (`DeviceCreateInfo::vulkanCreateHooks`) add DXVK's needs. The instance hook adds the extensions. The
    device hook asks `dxvkGetGuestDeviceRequirements` on the chosen physical device, with BasicRHI's binding model, and merges the
    answer: its extensions, its core features, each feature struct ORed into BasicRHI's of the same sType or copied into the
    chain, and its queue counts.
4.  `dxvkSetGuestDevice` hands the device over, with the feature chain the device was created with (guest version 2).
    `dxvkGetInteropDeviceInfo` reports those features, so the graph's binding model check sees the mutable descriptors
    BasicRHI enabled and DXVK did not. Before this, the graph disabled itself and DCLF was forced off in the owned frame.
    BasicRHI likewise records what its device hook added (DXVK's `nullDescriptor`): without that it reported null
    descriptors unsupported and DCLF logged that warning for every texture. DXVK submits all its work (graphics, transfer, sparse) to the graphics family's
    first queue, the one BasicRHI's graphics queue is. The graph keeps the compute queue, the copy queue and the copy family's
    second queue (the uploader's, which BasicRHI's `CreateQueue` hands out); `dxvkGetInteropDeviceInfo` reports them as
    `computeQueue`, `copyQueue` and `uploadQueue`, so `RenderGraphRuntime::Initialize` runs as it does on an adopted device.
5.  After the game's device exists, `rhi::vulkan::ShareVulkanDevice` puts BasicRHI's queue accesses under DXVK's submission lock and
    turns its whole-device waits into waits on its own timelines. `RenderGraphRuntime::Initialize` shares the device instead of
    calling `AdoptVulkanDevice`, with the same submission hooks.

Any failure before the hand-over leaves the owned frame off, logged, and DXVK creates its own device: `Prepare` first checks every
DXVK export the owned frame calls later (the guest-device exports, `dxvkEmitExternalCommands`, `dxvkGetInteropResourceInfo`,
`dxvkEnqueueQueueCallback`), so a missing one never gets as far as BasicRHI's device. `Abandon` then releases the device and frees
its state. A failure after the game's device was made on BasicRHI's (sharing the device, resolving the present's exports) is
fatal, with a message that names `CS_ORG_OWNED_FRAME`: the guest's swap chain is headless, so going on would show a black screen.

### The frame tail (`OwnedFrame`)

DXVK's DXGI swap chain on a guest device is headless: no surface, no Vulkan swap chain. Its back buffer is an ordinary D3D11
texture that the engine, HDRDisplay and ImGui draw into as before. CS's Present chain ends in `OwnedFrame::Present` instead of
DXVK's Present:

1.  **Guest frame done.** An external command with no commands signals the guest-frame timeline in DXVK's stream. DXVK's headless
    Present ends the frame (`EndFrame`) and flushes, so the signal is on the submission that holds the frame's last D3D11 work.
2.  **Queue callback.** `dxvkEnqueueQueueCallback` queues the tail behind that submission, on DXVK's submission thread with its queue
    lock held (BasicRHI's lock hook knows that, `GuestDevice::QueueHeldScope`).
3.  **Copy.** The tail records a blit from the guest back buffer (in its resting layout, from `dxvkGetInteropResourceInfo`) into the
    swap chain image BasicRHI acquired, waits for the guest-frame point and signals the host timeline.
4.  **Present.** `rhi::Swapchain::Present(vsync, PresentSyncDesc{graphics queue, host point})` presents and acquires the next image.
    Its frame-latency limit blocks DXVK's submission thread, which holds the next frame's command lists back from the GPU, so it
    is set to `kGameFramesInFlight` and only backs up the render thread's own pacing (`CS_FRAME_LATENCY`). At 2 Whiterun ran
    about 12% slower (50 against 57 fps, Dev-Fast).

Every failure path of `OwnedFrame::Present` still calls DXVK's Present once and returns its result, so the game's frame ends;
the frame is then not shown (logged once). A swap chain format the owned frame has no match for is logged and shown as
R8G8B8A8_UNORM through the blit.

**Teardown.** `RenderGraphRuntime::Shutdown` (DXVK's device teardown callback) calls `OwnedFrame::Shutdown` first: no new frame is
taken, the queued tails drain, the host timeline is waited for, then the swap chain, the guest-frame semaphore and the command pool
are destroyed, all before DXVK's device goes. Both waits are bounded; a tail that never runs leaves the objects to the process exit.

The caller's thread does no Vulkan work and holds no BasicRHI lock, so the main frame, the loading screen's job thread
(`JobListManager::ServingThread`, under `Renderer::Lock`) and Bink present the same way. A resize (the guest back buffer's size
changed, or a present asked for a rebuild) and HDRDisplay's colour space are applied at the next Present, after every queued tail
has run. With HDR on, HDRDisplay's output texture is what the tail copies into the swap chain image
(`OwnedFrame::SetFrameSource`): no `CopyResource` into the guest back buffer first. A frame whose output a clean
screenshot composite overwrites goes through the back buffer as before, and screenshots read the shown texture. The
compute shader does not write the swap chain image itself: the image is acquired on DXVK's submission thread after the
previous frame's present, up to 8 frames after the D3D11 work that would write it was recorded. SDR output is
unchanged (HDRDisplay does not run). HDR is untested: the test display reports no HDR10 or scRGB support
(`output supports HDR10 false, scRGB false`). The surface state comes from BasicRHI's swap chain
(`OwnedFrame::GetSurfaceState`, read by `DXVKInterop::RefreshPresenterSurfaceState`). Frame generation is unavailable in the
owned frame (logged once): no frame-generation presenter wraps BasicRHI's swap chain yet (P1.4). Upscalers that run inside the
frame through DXVK's interop are unchanged.

### Status (2026-11-09, RX 6600, Dev-Fast)

-   With the feature fixes above, the graph runs in the owned frame (`[ORG] Binding model: descriptor buffer`, no
    `Render graph disabled`, no `Forced off`).
-   **Open: a stall with DCLF on.** The owned frame stops presenting at the main menu or the first loading screen, every
    run (runs `1109-1233..`, `1245..`, `1253..`, `1306..`, `1322..` `p1fix-owned-frame-*`). It does not happen with DCLF
    off (`owned-frame-nodclf`) or with the owned frame off. It still happens with every graph queue on graphics
    (`owned-frame-onequeue`) and with the epochs outside DXVK's stream (`owned-frame-nostream`). A dump shows DXVK's
    submission thread asleep in `submitCmdLists` with 33 entries queued and no waiter on its condition variable, the
    CS thread blocked on the full queue, and the presenting thread waiting for a frame slot. The CS hang watchdog's dump
    (which suspends and resumes every thread) once released it.
-   Because of the stall there is no owned-on frame rate with DCLF on yet. The earlier 20-30% slower result compared DCLF
    on against DCLF off and does not count. Owned off, DCLF on, `DXVK_DEBUG=hang`: Whiterun 72.8, Dragonsreach 84.4 fps
    (`1109-122526-p1fix-owned-frame-off-off`).
-   Window cases: `owned-frame-window` drives focus loss, minimize/restore and a resize with `dclf:window=...` (no key
    presses). Not run yet; the Bethesda logo's Bink presents and loading screens ran in every owned run before the stall.

### What becomes unnecessary later

The graph's epochs still run inside DXVK's stream as external commands, so everything above in "Ordering against D3D11" stays
for now. As later phases move work out of DXVK:

-   **Queue boundaries** and **external home states** go once no DXVK image is imported (the guest back buffer included: the copy
    becomes a graph pass when the frame's last writer is ORG, P1.6 and P5).
-   **Segment splits** and the **stream point** go once the compute and copy queues order against ORG's own graphics work.
-   **The guest-frame signal and the queue callback** become an ordinary graph present (`SetPresentPass`, `GetLastPresentDependency`)
    once the frame's graphics work is ORG's.
-   **The binding model negotiation** (`dxvkSetBindingModelCallback`) and `dxvkRequestDeviceFeatures` are already unused in the owned
    frame: the host decides.

## Tripwire handles

Phase 4 of [org-full-port.md](./org-full-port.md) puts these in the engine's `ID3D11*` and `IDXGISwapChain` fields once ORG
owns what is behind them (`src/RenderGraph/Engine/Tripwire/Tripwire.h`). Nothing installs them yet: P4.2-P4.6 do, each
for its creation functions.

-   **What a handle does.** IUnknown works (atomic count, Release from any thread). `QueryInterface` answers only the
    handle's interface chain. `GetDesc` returns the creation description, and debug names are kept. Every other method
    logs its caller once per call site (module+offset, with the Address Library ID for the game) and fails with E_FAIL,
    null or zeroed outputs, or zero.
-   **Guest mode.** Made with a D3D11 object (`a_wrap`), the handle forwards every call and logs only the methods in its
    `WatchSet`. Handle arguments are unwrapped on the way, so a wrapping device or context passes DXVK its own objects.
    A handle given straight to DXVK crashes there: a port installs one only where its readers go through the handle.
-   **Releases.** The last Release drops the wrapped object at once and queues the ORG resource and owner. The render
    thread hands them to `ReleaseAfterD3D11` through `Tripwire::ServiceRetired`, every frame at the frame pace point
    (`RenderGraphRuntime::SignalFramePace`, beside `ServiceD3D11Releases`).
-   **Report.** `CS_ORG_TRIPWIRE_LOG`'s periodic report runs on a joinable thread that `Tripwire::Stop` stops and joins at the
    game window's close, before the exit report.
-   **Generated vtables.** `tools/generate-tripwire-interfaces.py` reads the SDK's `d3d11.h` and `dxgi.h` into
    `TripwireInterfaces.inl`: each method's signature, its forwarding arguments and its failure outputs (from the SAL
    annotations). `TripwireHandles.h` expands that into one class per interface. Every method is an `override`, and a
    class with a missing slot stays abstract, so a list that does not match the SDK fails to compile. Run the script
    again after an SDK change.

## The schedule inspector

To see what the persistent graph executes (batches per queue, the barriers before each pass, and every cross-queue wait with its cause), use the menu's
"Render graph schedule" window or the `communityshaders.schedule` DevBench tool. See [render-graph-schedule.md](./render-graph-schedule.md).

## Switches

Every switch is read once, through `src/RenderGraph/Switches.h` (`Switches::On`, `Off`, `Equals`, `Value`, `Number`): the first
read of a name is kept for the session, from any thread. New switches go through it and get a row here. The feature ports' own
switches (`CS_ORG_<FEATURE>=1`) read through it too.

| Variable | Effect |
| --- | --- |
| `CS_ORG=0` | No feature request and no graph: every feature stays on D3D11. |
| `CS_ORG_OWNED_FRAME=1` | BasicRHI creates the device, DXVK runs as its guest, ORG presents ("Owned frame"). Frame generation is unavailable. |
| `CS_ORG_CRASH_MARKERS=0` | With the owned frame, BasicRHI's crash markers off even in hang mode. They cost about a third of the frame rate in Dragonsreach on the RX 6600 (43 against 60 fps, Dev-Fast). |
| `CS_ORG=features` | Request the device features but never create the graph (isolates device-configuration side effects). |
| `CS_ORG_SUBMIT=flush` | Use the flush-and-lock submission path even when DXVK supports the stream path. |
| `CS_ORG_LIGHT_LIMIT_FIX_SHADOWS=1` | With LLF's local light shadows on, its shadow cache runs at the EarlyPrepass anchor (`ORGLocalShadows.cpp`): the engine slices drawn this frame are copied by `LocalShadowCopyCS`, one dispatch per staged copy through a run gate, and the cache and slot data are Persistent graph resources exported to the engine's pixel shaders (t102, t103) and published (`LightLimitFix::LocalShadowData`, `LocalShadowCache`). Logs `[ORG] LLF local shadows run on the render graph`; falls back to the D3D11 cache on any failure. The old name `CS_ORG_LLF_SHADOWS` is still read when this one is unset, with a deprecation warning. LLF's light culling has no switch: it runs on the graph whenever the runtime is active, and publishes `LightLimitFix::Lights`, `LightIndexList` and `LightGrid`. |
| `CS_ORG_LLF_PARITY=1` | Every 300 frames, also run LLF's D3D11 culling and compare each cluster's light set with the graph's (logs `LLF parity OK` / `MISMATCH`). |
| `CS_ORG_IBL=1` | IBL's SH projection (`DiffuseIBLCS`) as two compute passes at the Prepass anchor (`IBL/ORGIBL.cpp`) into History graph twins exported to D3D11 (t76, t77, the composite's t14 and t15) and published (`IBL::EnvIBL`, `IBL::SkyIBL`). The environment source is Dynamic Cubemaps' published `DynamicCubemaps::Env` when that is on the graph, else its D3D11 texture imported; a missing source clears the SH to zero, as D3D11's projection of a null view does. Logs `[ORG] IBL: SH projection runs on the render graph (Prepass anchor)`; falls back to D3D11 on any failure. |
| `CS_ORG_DYNAMIC_CUBEMAPS=1` | Dynamic Cubemaps' update (capture, inference, specular irradiance, BC6H encoding, publishing) at the DeferredPasses anchor (`DynamicCubemaps/ORGDynamicCubemaps.cpp`), as indirect dispatches whose group counts the commit latches each frame (zero for skipped work). The cubemaps and their BC6H copies are Persistent graph twins exported to D3D11 and published (`DynamicCubemaps::Env`, `EnvReflections`, `EnvTextureBC6H`, `EnvReflectionsBC6H`). Logs `[ORG] Dynamic Cubemaps: the cubemaps update on the render graph at DeferredPasses (...)`; falls back to D3D11 on any failure. |
| `CS_ORG_SKYLIGHTING=1` | Skylighting's probe update (`UpdateProbesCS`) at the Prepass anchor (`Skylighting/ORGSkylighting.cpp`), reading CS's shared constants and latching only the directional shadow data, through a run gate opened on frames that update. The probe volumes are History graph textures exported to D3D11 (t50, t53) and published (`Skylighting::ProbeArray`, `ShadowVisibility`). A reset re-arms four one-shot clears. Logs `[ORG] Skylighting: the probe update runs on the render graph at the Prepass anchor`; falls back to D3D11 on any failure. |
| `CS_ORG_SCREEN_SPACE_SHADOWS=1` | Screen Space Shadows at the Prepass anchor (`ScreenSpaceShadows/ORGScreenSpaceShadows.cpp`): a clear, then Bend's raymarch dispatches through a run gate (one entry per dispatch). The depth is kPOST_ZPREPASS_COPY, or Terrain Blending's published `TerrainBlending::BlendedDepth` on its graph path. The output is exported to the engine's t45 and published as `ScreenSpaceShadows::ShadowTexture`. Logs `[ORG] Screen Space Shadows runs on the render graph`; falls back to D3D11 on any failure. |
| `CS_ORG_EXPONENTIAL_HEIGHT_FOG=1` | Exponential Height Fog's volumetric fog at the Prepass anchor (`ExponentialHeightFog/ORGExponentialHeightFog.cpp`): conservative depth, material, scattering and integration per volume through a run gate, with the history copies and the far clear. The integrated volumes are exported to D3D11 (t19, t22) and published. Inputs are read by their producers' published ids when the bound view is that id's export, else imported. Logs `[ORG] Exponential Height Fog runs on the render graph (...)`; a frame whose inputs the graph cannot read runs on D3D11, and a failed program or graph returns it to D3D11 for good. |
| `CS_ORG_TERRAIN_SHADOWS=1` | Terrain Shadows at the EarlyPrepass anchor (`TerrainShadows/ORGTerrainShadows.cpp`): a one-shot upload of each heightmap, then `ShadowUpdate.cs.hlsl` over the frame's stripes through a run gate. The shadow heights are a Persistent graph texture exported to the engine's t60 and published (`TerrainShadows::ShadowHeight`). Logs `[ORG] Terrain Shadows: heightmaps go to the render graph`; on any failure the heightmap is loaded again on D3D11. |
| `CS_ORG_VOLUMETRIC_SHADOWS=1` | Volumetric Shadows' downsample and blurs at the EarlyPrepass anchor (`VolumetricShadows/ORGVolumetricShadows.cpp`), reading the engine's sun cascades. The shared shadow map is a History graph texture exported to the engine's t18 and published (`VolumetricShadows::ShadowCopy`). Logs `[ORG] Volumetric Shadows runs on the render graph at EarlyPrepass`; on any failure, or when the shadow mask's t4 is not the cascade map, the D3D11 dispatches run. |
| `CS_ORG_CLOUD_SHADOWS=1` | Cloud Shadows' occlusion copy at the ReflectionsPrepass anchor (`CloudShadows/ORGCloudShadows.cpp`): `OcclusionCopyCS` copies the last cloud layer into the occlusion cubemap, one dispatch per mip, through a run gate opened on the frame's first reflections prepass only. The cubemap is a History graph texture exported to t25 and t26 and published (`CloudShadows::Occlusion`); the layers stay on D3D11. Logs `[ORG] Cloud Shadows: the occlusion copy runs on the render graph`; on any failure the D3D11 copy is made, filled and bound. |
| `CS_ORG_GRASS_COLLISION=1` | Grass Collision's update at the EarlyPrepass anchor (`GrassCollision/ORGGrassCollision.cpp`): a copy of the latched colliders, then `CollisionUpdateCS` through a run gate opened on frames that staged one. The collision texture is a History graph texture exported to the engine's VS t100 and published (`GrassCollision::Collision`). Logs `[ORG] Grass Collision: the collision update runs on the render graph`; falls back to D3D11 on any failure. |
| `CS_GRASS_COLLISION_STATS=1` | Every 120 frames, log the frame's collider and collision-shape counts and the path that ran them (`[GRASS COLLISION] frame N (ORG or D3D11): ...`). |
| `CS_ORG_SCREEN_SPACE_GI=1` | Screen Space GI's chain at the DeferredPasses anchor (`ScreenSpaceGI/ORGScreenSpaceGI.cpp`): prefilter depths, radiance and disocclusion, prefilter radiance and normals, GI, blur and upsample through a run gate, with SSGICB in the latch and the shared constants. The history clears are one-shot passes of `cs.ssgi.reset`, armed again on a history reset. The composite's inputs are graph textures exported to D3D11 (t10-t13). Logs `[ORG] Screen Space GI runs on the render graph (...)`; on any failure `[ORG] Screen Space GI returns to D3D11: ...`. |
| `CS_ORG_SUBSURFACE_SCATTERING=1` | Subsurface Scattering at the DeferredPasses anchor (`SubsurfaceScattering/ORGSubsurfaceScattering.cpp`): the diffuse extraction and the separable or Burley blur through a run gate opened on frames with skin, reading the shared constants; the intermediate textures are transient graph textures. Logs `[ORG] Subsurface Scattering runs on the render graph`; falls back to D3D11 on any failure. |
| `CS_ORG_TERRAIN_BLENDING=1` | Terrain Blending's depth blend runs at the DepthDone anchor (`ORGTerrainBlending.cpp`): `DepthBlend` writes the blended depths and the terrain mask (t55) as graph textures exported to D3D11 and published (`TerrainBlending::BlendedDepth`, `BlendedDepth16`, `Mask`); DCLF's t55 reads the mask by name. The terrain depth stays a D3D11 texture (org-extension-api-notes.md, "Depth targets a D3D11 draw still writes"). Logs `[ORG] Terrain Blending: the depth blend runs on the render graph`; falls back to D3D11 on any failure. A frame whose anchor submitted nothing blends on D3D11, and every view Terrain Blending hands out names the D3D11 textures for that frame (`ORGTerrainBlending::Holds`). |
| `CS_ORG_ANCHOR_SELFTEST=1` | Reserve every frame anchor and run a test client at each: a raster pass writes a texture, a compute pass reads it and one texel of kMAIN into a buffer, and a copy reads that back. Each anchor logs `[ORG] anchor self-test: <anchor> OK` once, then `every anchor OK`. |
| `CS_ORG_GRAPHRES_SELFTEST=1` | Reserve DeferredPasses and test the graph-backed resources there (above, "Graph-backed feature resources"). Logs `[ORG] graph resource self-test: 16x16 OK ...`, then `resized to 32x32`, `32x32 OK ...` and `every check OK; the texture is released`; `MISMATCH` otherwise. |
| `CS_ORG_UPSCALING=1` | Upscaling's own passes on the graph (`Upscaling/ORGUpscaling.cpp`): the depth upscale (the copies, `DepthRefractionUpscalePS`, `UnderwaterMaskUpscalePS`; in the graph only while the depth upscale runs) and the upscaled colour's copy into kMAIN (a `CopyHudlessPS` draw through a run gate, on frames the upscaler returned a result) at the `Upscale` anchor, and frame generation's hudless copy (`CopyHudlessPS`) at `FrameGeneration`. The upscaled and hudless textures are graph textures exported to D3D11, which Streamline's evaluates in the interop ring still write and read. Logs `[ORG] Upscaling's own passes run on the render graph` and `created the upscaled texture on the graph`; on any failure the D3D11 textures and passes come back (`[ORG] Upscaling returns to D3D11: ...`). |
| `CS_ORG_IMGUI=1` | Draw CS's ImGui frames with the render graph's backend at the `Overlay` anchor once the graph is up ("ImGui on the graph"). Logs `[ORG] ImGui: drawn by the render graph` and `drawing onto ...`. |
| `CS_ORG_HDR_DISPLAY=1` | HDR Display's output composite, its copy into the back buffer and the UI texture's clear as passes at the `HDROutput` anchor (`ORGHDRDisplay`), on frames HDR composites. Logs `[ORG] HDR Display runs on the render graph` and where the back buffer copy runs; on a failure `[ORG] HDR Display returns to D3D11`. Test command `dclf:hdr=0|1` forces Enable HDR on an SDR display. |
| `CS_ORG_SCREENSHOT=1` | The Screenshot feature's captures (the hotkey, the menu button, `dclf:shot`) are copied into an ORG readback buffer by a pass at the `Capture` anchor, at the end of HDRDisplay's present chain, instead of a D3D11 staging texture (`src/Features/Screenshot/ORGScreenshot.cpp`). The frame never waits: the bytes reach the encoding worker when the copy's fence passes, and the worker crops them. Logs `[ORG] Screenshot: captures are read back through the render graph`; any failure (no anchor, a source that cannot be imported, a failed epoch, a submission at the capture point without the copy) copies that capture with D3D11. The copy is a one-shot pass that each capture arms again: frames without a capture submit nothing for it. |
| `CS_ORG_POST_PROCESSING=1` | Post Processing's tonemap chain at the `Tonemap` anchor (`PostProcessing/ORGPostProcessing.cpp`, `ORGPostEffect.h`), on frames every enabled sub-effect has an ORG port. Logs `[ORG] Post Processing: the chain runs on the render graph` or `... on D3D11: <reason>` whenever that changes. |
| `CS_ORG_FX=1` | Effects11's .fx passes at the `Tonemap` anchor (`Effects11/ORG/ORGFxRuntime.cpp`, [org-fx-runtime.md](./org-fx-runtime.md)). |
| `CS_ORG_E11_RAYS=1` | Effects11's volumetric and sun rays at the `PostComposite` anchor (`Effects11/ORGSunRays.cpp`); on a failure the rays stay on D3D11. |
| `CS_ORG_GLINTS=1` | TruePBR's glints noise is a graph texture filled once by a one-shot pass at `EarlyPrepass` (`TruePBR/ORGGlintsNoise.cpp`), bound only after the run that filled it. |
| `CS_FG_DISPLAY_SUSPEND`, `CS_UPSCALE_UNFOCUSED`, `CS_FSRFG_QDEPTH`, `CS_SL_VERBOSE`, `CS_FORCE_FSR_FG` | Upscaling and frame generation diagnostics, read through `Switches`: display-off unloading and unfocused upscaling (dclf-validation.md), FSR frame generation's queue depth (1 to 8, default 2), Streamline's verbose log, and forcing the FSR frame generation path. |
| `CS_VK_DRIVER_TRACE`, `CS_VK_PRIORITY`, `CS_VK_CLIENT_PRIORITY`, `CS_VK_SPILL` | The Vulkan driver trace and residency experiments (`Upscaling/VkDriverTrace.cpp`), described in [vram-spill-lab.md](./vram-spill-lab.md). |
| `CS_ORG_TRIPWIRE_LOG=1` | Log every tripwire call site with its count ("Tripwire handles"): `[ORG] tripwire report (<reason>)`, a line per interface and one per site, every `CS_ORG_TRIPWIRE_LOG_INTERVAL` seconds (default 60, 0 = only at exit) when calls changed, and at exit. Without it each site still logs its first call (`[ORG] tripwire: <Interface>::<Method> from ...`). |
| `CS_ORG_TRIPWIRE_SELFTEST=1` | At startup, make every handle type standalone and check refcounting, `QueryInterface`, `GetDesc`, debug names, forbidden calls (fail, zeroed outputs, counted), guest forwarding and unwrapping, Release from several threads, and caller naming. Logs `[ORG] tripwire self-test: PASS (<n> checks, 20 interfaces)` or a `FAIL` line per check. |
| `CS_ORG_ENGINE_TARGETS=1` | The engine's render targets, depth-stencil targets, cubemap and 3D targets are images ORG makes (`RenderGraph/Engine/Targets`, [p4/render-targets.md](./p4/render-targets.md)); the engine's fields hold DXVK's D3D11 objects of the same memory. Ids `Engine::kMAIN`, `Engine::Depth::kMAIN`, `Engine::Cubemap::kREFLECTIONS`, `Engine::3D::<name>` are published on first request, and FrameAnchors' and DCLF's imports of those images return them. Logs `[ORG] engine targets: <n> targets now on the render graph, ...` at startup and `the set was made again on the render graph (...)` after a resize. |
| `CS_ORG_ENGINE_TARGET_HANDLES=1` | With `CS_ORG_ENGINE_TARGETS=1` and the tripwire's guest device and context installed, the engine's target fields hold tripwire handles wrapping DXVK's objects. Without the guests it logs a warning and keeps DXVK's objects. |
| `CS_ORG_ENGINE_TARGETS_DCLF=0` | With `CS_ORG_ENGINE_TARGETS=1`, DCLF imports the engine targets' images itself instead of reading their published textures (a bisecting aid). |
| `CS_ORG_EPOCHS=0` | One graph for every segment instead of a graph per epoch; no frame anchor can be reserved. |
| `CS_ORG_REUSE_RECORDINGS=0` | Record every epoch again each frame instead of submitting the kept recordings again. |
| `CS_ORG_RECORDING_TIMESTAMPS=1` | Kept recordings carry ORG's pass timestamps (they carry none by default). |
| `CS_ORG_BATCH_SUBMIT=0` | Enqueue each of an epoch's submissions alone instead of one batch (`dxvkEnqueueInteropSubmissions`). |
| `CS_ORG_EARLY_FLUSH=1` | Flush DXVK's command list before each in-stream epoch instead of when the epoch's first submission is enqueued. |
| `CS_ORG_UPLOAD_QUEUE=0` | No dedicated upload queue: uploads go to the copy queue or the graphics queue. |
| `CS_ORG_PINNED_IMPORTS=1` | Imported DXVK images are pinned instead of tracked (`dxvkTrackInteropResources` unused). |
| `CS_ORG_RESTING_LAYOUTS=0` | DXVK keeps imported images in GENERAL instead of its own layouts (DxvkImageImport). |
| `CS_ORG_DETAILED_CHECKPOINTS=1` | BasicRHI's crash checkpoints per command instead of per pass. |
| `CS_ORG_ALIAS_LOG=1` | Log every alias plan's placements, ranks and exclusions (ORG's `autoAliasEnableLogging`). |
| `CS_ORG_SHARED_CONSTANTS_CHECK=1` | After each epoch that uploads CS's shared constants, read the D3D11 buffers back and log whether the bytes match (stalls). |
| `CS_ORG_COMPOSITE=1` | The deferred composite runs on the graph at the DeferredComposite anchor (`src/ORGDeferredComposite.cpp`). |
| `CS_ORG_TEST_CONTRIBUTOR=<dll>` | Load ORG's sample contributor into `CS_ORG_TEST_CONTRIBUTOR_EPOCH` (default `cs.light-culling`) for `CS_ORG_TEST_CONTRIBUTOR_FRAMES` frames (default 600). |
| `CS_FRAME_LATENCY=<n>` | The frame-pace timeline's depth, 0 to `kGameFramesInFlight` (0 turns the render thread's own pacing off). |
| `CS_GPU_LOAD=<groups>` | A synthetic GPU load of that many groups each frame (`GpuEventTimers`; a graph pass at the Present anchor with `CS_GPU_TIMERS=graph`). |
| `CS_GPU_EVENT_TIMERS=<frames>` | Log the GPU event timers every that many frames ("GPU timers from the graph"). |
| `CS_ORG_EPOCH_STATS=1` | Log render-thread CPU time per epoch (average and maximum every 600 epochs). |
| `CS_ORG_ASYNC_EPOCHS=0` | Run the epochs synchronously. By default each epoch is prepared and recorded ahead on the graph host's thread, and the render thread only admits and submits ("Revisions and recordings" in `dclf-architecture.md`). |
| `CS_ORG_IN_STREAM=0` | Enqueue the async epochs' batches behind DXVK's command list (`dxvkEnqueueInteropSubmission`) instead of running their graphics work inside it. A list such as `0,3` keeps only those epochs in the stream. |
| `CS_ORG_EXTERNAL_HOME=0` | Epochs no longer give DXVK's images back on the graphics queue in their home state (unsafe: DXVK expects them there). |
| `CS_ORG_CLOSED=1` | Closed executions: every epoch returns every resource to its home state and starts with a full barrier. An interop fallback; by default the epochs' resources flow between them through ORG's ledgers. |
| `CS_ORG_COMPUTE_QUEUE=0` | The graph's compute queue aliases the graphics one (no DXVK compute queue). |
| `CS_ORG_COPY_QUEUE=0` | The graph's copy queue aliases the graphics one instead of a transfer-family queue. |
| `ORG_PERSISTENT_COPIES_ON_COPY_QUEUE=1` | Every copy pass on the copy queue whatever it prefers (a test of that queue). Set it in the launching process's environment: ORG reads it with `getenv`, which does not see the switches file. |
| `CS_ORG_QUEUE_UPLOADS=0` | Every upload of an epoch on the graphics queue, and every compute batch waits for them. |
| `CS_GPU_IDLE_TRACE=<frames>` | Every that many frames, log one complete frame's GPU idle gaps and what the render thread was doing during each (`[GpuIdle]`). DXVK timestamps every submission on its queue (`dxvkSetSubmissionTrace`, two extra timestamp-only submissions per submission), the Streamline ring's buffers add their own spans, and the render thread's perf events are the CPU markers. Forces Frame Annotations on for the session (without saving it) but leaves out the per-draw geometry events, whose formatting would dominate the timeline. Diagnostics only: the trace's own overhead makes gaps somewhat longer than in a normal run. |
| `CS_GPU_TIMERS=graph` | GPU timers from the render graph alone ("GPU timers from the graph"): the profiler makes no D3D11 query, its D3D11 ranges keep CPU times only, `CS_GPU_EVENT_TIMERS` logs the graph's ranges (`[GpuEvents] graph ...`), and `CS_GPU_LOAD` runs as a graph pass at the Present anchor. Default `d3d11`. |
| `CS_GPU_IDLE_SOURCE=graph` | `CS_GPU_IDLE_TRACE` reads the graph's graphics-queue ranges instead of DXVK's submission trace. Default `dxvk`. |
| `CS_PROFILER_LOG=<frames>` | Log the profiling window's rolling averages every that many collected frames, sorted by cost, with `[split N%]` on timers that straddle DXVK submissions. For comparing two configurations from their logs. |

Every `CS_*` switch can also be set in `CommunityShaders.env`, beside `CommunityShaders.log`
(`Documents\My Games\Skyrim Special Edition\SKSE`): one `NAME=value` per line, `#` for comments, read
when the plugin loads. This covers launches that do not pass a new environment to the game, such as an MO2
instance that was already running. A variable set in the real environment wins, and every value taken
from the file is logged (`[DevEnv]`).

## DXVK fork: allocation-pool locking

`DxvkResourceAllocationPool` is an unsynchronized free list that `DxvkMemoryAllocator` may touch only under `m_mutex`
(`extern/dxvk/src/dxvk/dxvk_memory.cpp`). Upstream DXVK has three paths that call `m_allocationPool.create` without the lock:
-   `importBufferResource`: ORG's interop reaches it through `RenderGraphRuntime::WrapBuffer` → `dxvkCreateBufferFromVkBuffer`.
-   `importImageResource`: swapchain images and D3D11on12.
-   `createAllocation(DxvkSparsePageTable*, ...)`: every sparse or tiled resource.

**The crash.** When one of these runs while another thread allocates or releases under the lock (Skyrim's loading thread
creating buffers), both threads can take the same storage. The symptom is an access violation reading `0x1` in
`DxvkResourceAllocationPool::alloc`, inside an engine `CreateBuffer`.

**The fix.** The fork takes `m_mutex` around the pool access on those three paths only. None of their callers holds the lock,
which is not recursive.

**Status.** The patch is not yet listed in `extern/dxvk/FORK.md` or offered upstream. Keep it when rebasing the fork.

## Building and tests

-   The libraries are submodules under `extern/` (`OpenRenderGraph`, `BasicRHI`, `BasicTelemetry`,
    `volk`); `git submodule update --init` fetches them. `cmake/RenderGraph.cmake` builds them from
    source (Vulkan only) and compiles the SPIR-V at build time with BasicRHI's DXC flags
    (`BasicRHIShaderFlags.cmake`). Set `CS_ORG_ROOT` to another directory with the same side-by-side
    layout to build against local changes to those libraries. If they are missing, the plugin builds
    without the render graph.
-   `tests/OrgDxvkInterop` runs the graph on DXVK's device without the game, in both submission modes
    (`OrgDxvkInteropTest.lock` / `.stream`). D3D11 writes a sentinel before the epoch and reads back
    after it, so either misordering fails the test.
