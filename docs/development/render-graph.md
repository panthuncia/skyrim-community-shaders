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
        version 6). The graph sees it as a host-owned texture whose home state is the layout DXVK rests it in, so every epoch gives
        it back in that layout, as for DXVK's own images (DxvkImageImport). DXVK zeroes it in its stream when it is wrapped.
    -   The image has no mutable format, so D3D11 views use the texture's own typed format. 3D textures take no RTV.
-   **Rule: exported means not aliasable**, whatever the lifetime. D3D11's use is outside the graph, which would otherwise reuse
    the memory across it. A resource becomes aliasable when its last D3D11 reader is ported and the export goes.
-   **Teardown and resize.** `Reset`, the destructor and `Resize` release the D3D11 views and resource first. Then
    `RenderGraphRuntime::ReleaseAfterD3D11` holds the graph resource until an event query issued after that release completes
    (checked every frame). DXVK keeps its own objects alive for in-flight work, but not the image's memory, which the graph owns.
    After a `Resize` the owner calls `FrameAnchors::Refresh(id)`: its anchors' graphs are built again with the new resource. A pass
    reads `Graph()` when it is made, never across builds.

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
// At a resolution change:
if (out.Resize(width, height))
	FrameAnchors::Refresh("cs.myfeature");
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
    `cs.org.published-resources` again, which rebuilds every graph once.
-   `Withdraw(id)` when the producer leaves its ORG path (its `onFailed`, a setting that turns it off). Readers then go back to D3D11.
    The resource stays registered until the next `Publish` under that id, because a graph built earlier may still declare it.
-   `ShaderResourceIndex(id, index)` gives the bindless index of the whole-resource SRV (every mip; a cube's `TextureCube` view, a 2D
    array's full array view), false while it is not published. That index is what a frame record holds for a bindless reader.

```cpp
// The producer, after creating its output (and after every Resize):
out = std::make_unique<GraphResources::Texture2D>(desc, "MyFeature::Output",
	GraphResources::Options{ .lifetime = GraphResources::Lifetime::History, .d3d11 = GraphResources::Export::SRV });
PublishedResources::Publish(*out);  // id "MyFeature::Output"
// Its client's onFailed:
PublishedResources::Withdraw("MyFeature::Output");
```

DCLF reads a published texture through its frame texture contract (`Api::FrameTexture::graphResource` and `graph`;
dclf-architecture.md, "Frame graph textures"). `CS_DCLF_GRAPH_TEXTURE_TEST=graph|d3d11` is the end-to-end test: a copy pass at the
Prepass anchor fills a published cubemap from Dynamic Cubemaps' reflections, and DCLF's t30 reads it through the graph or through
its export.

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
| `Prepass` | `Deferred::PrepassPasses` (the main pass's first opaque batch) |
| `DeferredPasses` | `Deferred::DeferredPasses`, after the final depth copy |
| `DeferredComposite` | `Deferred::DeferredPasses`, after the D3D11 SSGI, SSS and cubemap update, where the composite runs |
| `PostProcessing` | `Upscaling::Main_PostProcessing`, before the upscaler |
| `Tonemap` | `Main_HDRTonemapBlendCinematic_Render` (`Hooks.cpp`) |
| `Present` | `IDXGISwapChain_Present` (`Hooks.cpp`), before DCLF's `BeforePresent` |
| `Overlay` | `OverlayRenderer::FinalizeImGuiFrame`, where CS's ImGui frame is drawn (`ImGuiRenderer::RenderDrawData`) |

Each anchor's ordering rule is in [org-extension-api-notes.md](./org-extension-api-notes.md), "Attaching passes to the frame".

**How a feature uses one.**

1.  Before the graph initializes (`Load` or `PostPostLoad`, when its ORG path is on): `FrameAnchors::Reserve(anchor)`.
2.  Once the runtime is active (`SetupResources`): make its programs and resources, then `FrameAnchors::Register(client)`. A
    client is an id, its passes (name, anchor, order key, kind, and a `make` that returns a new `TypedRenderGraphPass` at each
    build), and optional callbacks: `registerResources`, `commit` (latch writes inside the epoch), `active` and `onFailed`.
3.  The passes declare what they read and write as any graph pass does. Engine targets come from
    `FrameAnchors::EngineTarget(RE::RENDER_TARGET)` and `EngineDepth(...)`: DXVK images imported once (`DxvkImageImport`) and
    registered with every anchor graph as `cs.engine.target.<n>` and `cs.engine.depth.<n>`. Cubemap targets come from
    `EngineCubemap(RE::RENDER_TARGETS_CUBEMAP)` (`cs.engine.cubemap.<n>`, a cube default SRV), and a feature's own D3D11
    texture that is not ported yet from `D3D11Texture(name, texture, srv, uav)` (`cs.d3d11.<name>`; Screen Space Shadows reads Terrain Blending's blended depth through it). Every import
    describes all its mips and slices; a `D3D11_RESOURCE_MISC_TEXTURECUBE` texture gets cube views.
4.  When the graph fails, `onFailed` is called and the client is removed; the feature returns to its D3D11 path.
5.  CS's shared constant buffers (FrameBuffer, SharedData, FeatureData, Permutation): set `.sharedConstants = true`. The anchor's
    epoch then writes their values into a `LatchBlock` at the hook (the bytes D3D11 sees there: CS's cached `PerFrame` mapping and
    State's last uploads) and copies them into four CBV buffers (`cs.shared.*`) before the anchor's passes. A pass declares them
    with `SharedConstants::Declare` and pushes `SharedConstants::Index` as the shader's `SHARED` entry. `CS_ORG_SHARED_CONSTANTS_CHECK=1`
    reads the D3D11 buffers back after each such epoch and logs whether the bytes match (a debug switch: it stalls).
6.  Textures D3D11 features produce (not engine targets) can be read in place, untracked, through
    `DCLF::GpuTextures::ResolveBinding(srv)` (the ORG deferred composite does, `src/ORGDeferredComposite.cpp`). Keep the binding's
    owner until no submission can read it.

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
-   **The hazard rule.** After the late anchors (`PostProcessing`, `Tonemap`, `Present`, `Overlay`) have run an epoch, `Run` calls
    `Upscaling::SeparateFromGraphEpochs()`. An upscaler, frame generation or the present can follow these, and none may run directly
    behind a graph epoch (above, "Hazard: external Vulkan work directly after a graph epoch"). The earlier anchors are followed by
    the engine's own draws.
-   **Not shared with DCLF.** DCLF keeps its schedule (`Schedule/Frame.cpp`, `Api/Passes.h`): its passes depend on its scene
    revisions and recorded shapes. Anchors are the thin layer for every other feature. DCLF's imports and the anchors' imports of
    the same engine image are separate graph resources; that is safe because they never share an epoch.

## Raster programs

`RasterProgram` (`src/RenderGraph/RasterProgram.h`) is `ComputeProgram`'s raster twin, for fullscreen vertex and pixel shader
passes on ORG render passes. It replaces `PostProcessing/RasterPass` and the pixel-shader passes in Upscaling and Effects11 as
they are ported.

-   **Compile.** Both stages compile with DXC through the same path (`ProgramBuild.{h,cpp}`, shared with `ComputeProgram`):
    ORGModuleServices' shader store, SPIR-V, BasicRHI's bindless ABI. The default vertex stage is `RenderGraph/FullscreenVS.hlsl`,
    a three-vertex triangle with `TEXCOORD0` as in PostProcessing's `fullscreen.hlsli`.
-   **State.** `Desc` gives up to eight colour targets, each with its format and `rhi::BlendAttachment`, an optional depth format
    and `DepthStencilState` (off by default), the `RasterState` (no culling by default) and the push-block size. The push block is a
    `cbuffer` at b0 that both stages read.
-   **Draw.** A pass declares its targets (`RenderTarget`, a mip or slice through `RtvView`) and depth (`DepthRead` or
    `DepthReadWrite`), then calls `Draw` at record time with the declared views, load ops and clear values, the viewport, and the
    constants. The viewport is the render area, so a mip's size is set there.
-   **Hot reload.** As for compute programs: registered with the pipeline registry with both sources as dependencies.
-   **Not covered yet:** the blend factor and sample mask that CODBloom's upsample uses (`D3D11_BLEND_BLEND_FACTOR`), which wait
    for P0.2. A viewport smaller than the render area, or a separate scissor, waits for P0.4.

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

Any failure before the hand-over leaves the owned frame off, logged, and DXVK creates its own device.

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
    (which suspends and resumes every thread) once released it. `owned-frame-skiptail` (`CS_ORG_OWNED_SKIPTAIL=1`) tells
    whether the tail's Vulkan present on DXVK's submission thread is needed for it.
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

## The schedule inspector

To see what the persistent graph executes (batches per queue, the barriers before each pass, and every cross-queue wait with its cause), use the menu's
"Render graph schedule" window or the `communityshaders.schedule` DevBench tool. See [render-graph-schedule.md](./render-graph-schedule.md).

## Switches

| Variable | Effect |
| --- | --- |
| `CS_ORG=0` | No feature request and no graph: every feature stays on D3D11. |
| `CS_ORG_OWNED_FRAME=1` | BasicRHI creates the device, DXVK runs as its guest, ORG presents ("Owned frame"). Frame generation is unavailable. |
| `CS_ORG_OWNED_SKIPTAIL=1` | Diagnostic: the owned frame's tail neither copies nor presents (nothing is shown). |
| `CS_ORG_CRASH_MARKERS=0` | With the owned frame, BasicRHI's crash markers off even in hang mode. They cost about a third of the frame rate in Dragonsreach on the RX 6600 (43 against 60 fps, Dev-Fast). |
| `CS_ORG=features` | Request the device features but never create the graph (isolates device-configuration side effects). |
| `CS_ORG_SUBMIT=flush` | Use the flush-and-lock submission path even when DXVK supports the stream path. |
| `CS_ORG_LLF_SHADOWS=1` | With LLF's local light shadows on, its shadow cache runs at the EarlyPrepass anchor (`ORGLocalShadows.cpp`): the engine slices drawn this frame are copied by `LocalShadowCopyCS` on the graph, and the cache and slot data are Persistent graph resources exported to the engine's pixel shaders (t102, t103). Logs `[ORG] LLF local shadows run on the render graph`; falls back to the D3D11 cache on any failure. |
| `CS_ORG_LLF_PARITY=1` | Every 300 frames, also run LLF's D3D11 culling and compare each cluster's light set with the graph's (logs `LLF parity OK` / `MISMATCH`). |
| `CS_ORG_ANCHOR_SELFTEST=1` | Reserve every frame anchor and run a test client at each: a raster pass writes a texture, a compute pass reads it and one texel of kMAIN into a buffer, and a copy reads that back. Each anchor logs `[ORG] anchor self-test: <anchor> OK` once, then `every anchor OK`. |
| `CS_ORG_GRAPHRES_SELFTEST=1` | Reserve DeferredPasses and test the graph-backed resources there (above, "Graph-backed feature resources"). Logs `[ORG] graph resource self-test: 16x16 OK ...`, then `resized to 32x32`, `32x32 OK ...` and `every check OK; the texture is released`; `MISMATCH` otherwise. |
| `CS_ORG_IMGUI=1` | Draw CS's ImGui frames with the render graph's backend at the `Overlay` anchor once the graph is up ("ImGui on the graph"). Logs `[ORG] ImGui: drawn by the render graph` and `drawing onto ...`. |
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
