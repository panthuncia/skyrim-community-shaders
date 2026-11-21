# ORG / OMS library inventory for the CS-to-library survey

Checkout: `cs-org-dxvk` on `integ/hard-ports`, HEAD `7d7e6e01d`. It moved from `4625ec794` while this survey ran.
Submodules: `extern/OpenRenderGraph` at `bfd13bb` ("feat(upload): zero-fill fresh textures on the copy queue") and
`extern/ORGModuleServices` at `d48c924` ("feat(variables): add the variable set").
Paths below are relative to `extern/OpenRenderGraph` (ORG), `extern/ORGModuleServices` (OMS) or the CS repo root (`src/...`).

## 0. Scope documents that exist

| Library | Scope docs | Notes |
|---|---|---|
| ORG | `AGENTS.md` (31 lines), `README.md`, `RESOURCE_USES.md`, `docs/*.md` (persistent-epochs, job-graphs, pipelines, rebuild-plan, renderer-state-ownership, decision-trace) | `AGENTS.md` covers only comment style (plain language, AGENTS.md:3-31). It has **no** layering or "no game code" rule. |
| OMS | `README.md` only (207 lines). No AGENTS.md, no docs folder. | Scope comes from the README and header comments. |
| BasicRHI | `README.md` | "the low-level rendering hardware abstraction used by OpenRenderGraph and BasicRenderer" (README.md:3). Rule: "callers never branch on the API" (README.md:108-109). |

## 1. Scope and layering rules as stated

### ORG
- What it is: a pass-oriented render-graph framework. It compiles a persistent schedule, tracks states and queues, runs epochs, lowers into a host stream, hosts contributor DLLs through a C ABI, and offers runtime services (README.md:3-18).
- Dependencies: BasicRHI, flecs, Boost::container_hash, spdlog, imgui, implot, slang (README.md:229-239). BasicRHI comes as a package or the sibling submodule (README.md:247, 304-316).
- **Policy belongs to the host.** "The policy is the application's: which thread builds, when, what supersedes what, and what a failure does" (docs/persistent-epochs.md:31-34; GraphHost.h:238-242).
- **No shader compiler in ORG.** The host supplies `Runtime::Desc::pipelineCompiler` (docs/pipelines.md:51-63).
- Extension factories must read only snapshotted or concurrency-safe inputs. They must not write state the running graphs read (docs/persistent-epochs.md:126-139).
- Host contract for epochs: a read needs a write in that epoch or an earlier one, and one pass instance belongs to one epoch (docs/persistent-epochs.md:172-177).
- Game-specific code: there is no explicit rule. In practice the library is generic. CS, DXVK and DCLF appear only as examples in comments and docs (6 include files, 2 src files mention them).

### OMS
- "Optional module-facing services shared by OpenRenderGraph hosts. **OpenRenderGraph does not depend on this package**" (README.md:3).
- "The only mandatory library dependency is `BasicRHI::BasicRHI`". DXC is loaded dynamically. Vulkan is optional (README.md:205; 10).
- "Hosts install these services into their graph execution context; **graph scheduling remains entirely owned by ORG**" (README.md:207).
- Some parts are CPU-only and depend on nothing GPU-side:
  - `AsyncPrimitives` is header-only (README.md:12, 19-21).
  - `AsyncRuntime` provides suspension identities and must have a single owner across DLLs (README.md:17, 23-26).
  - `AsyncStateGraph` depends on TBB, spdlog and BasicTelemetry, and "not BasicRenderer/RHI/ORG" (README.md:36-41).
- **Host vocabulary stays in the host:**
  - "renderer event vocabularies and report contents remain host-owned" (README.md:16).
  - "Hosts retain their own scheduling vocabulary and defaults" (Async/ArtifactBuild.h:20-21).
  - "Host-defined classification IDs. The dependency graph does not own the host's domain inventory, worker pool, or priority policy" (Async/GraphScheduler.h:12-13).
  - "Kind belongs to the host … without importing a renderer kind inventory" (Async/ArtifactIdentity.h:13-14).
  - "Counts and class IDs come from the host" (Async/GraphSchedulingLayout.h:10-11).
  - "renderer-only event vocabulary stays in its adapter" (Async/StateGraphTypes.h:8).
- The render-thread rule: consumers use posted requests and completion mailboxes, not the synchronous StateGraph APIs (README.md:48-49).
- No-fallback rules: GrowableBufferSet has "no caps, no fallback" (README.md:80). BackingPool `allocate` "throws when it cannot: no fallback" (README.md:142-143).
- Game-specific code: there is no explicit rule. Headers name CS/DCLF only as example users (BackingPool.h:120, ComposedVersions.h:16, GrowableBufferSet.h:121, JournaledBufferSet.h:56,137). One leak: `Variables.h:30` cites "decision 6a", a CS frame-revision-plan decision.
- CS's own plan agrees. Frame-revision decision 1: "Generic code goes into OMS (`org::services`, `org::async`) and ORG. CS holds declarations, event sources, the stage producer and the adoption call" (`docs/development/plans/frame-revision-plan.md:111`). Step 4c: "Generic parts go to OMS later only if a second user appears" (frame-revision-plan.md:405).

## 2. Main public subsystems

### ORG (`include/`)
| Area | Headers | Purpose |
|---|---|---|
| Host, epochs, staged builds | `OpenRenderGraph/GraphHost.h` | One host, one graph per epoch. It covers async epochs (`SetAsyncEpochs`:501), external epochs (:631), per-epoch rebuilds (`RequestRebuild`:171-182) and builds off the owner thread (`BuildEpochGraph`:290, `AttachEpochGraph`:330, `StageEpochGraph`:353, `AttachStagedEpochGraphs`:379). It also has epoch failures (`PollEpochFailures`:430), frame positions and the frame alias arena (:548-575), and `NoteNewVersions` (:478). |
| Graph core | `Render/RenderGraph/RenderGraph.h`, `PersistentGraph.h`, `GraphCompiler.h`, `ExecutionState.h`, `ExternalEpochExecution.h`, `RhiExecution.h` | Persistent compile and schedule, admission ledgers, in-stream (DXVK) execution. |
| Job graphs | `OpenRenderGraph/Runtime.h`, `GraphDesc.h`, `GraphInstance.h`, `ExecutionHandle.h`, `ExternalClient.h` | A per-device runtime of shared services (Runtime.h:35-38). Graph descs and instances without a main graph (docs/job-graphs.md). |
| Passes | `RenderPasses/Base/TypedRenderGraphPass.h`, `Render/PassBuilders.h`, `PassExecutionContext.h`, `PreparedPass.h`, `Interfaces/IDynamicDeclaredResources.h` | Declare / Prepare / Record / Revision passes (README.md:60-135). |
| Extensions | `Render/RenderGraph/RenderGraphExtension.h` (`IRenderGraphExtension`:227, `ExternalPassDesc`:55, `ExternalInsertPoint`:23) | Supplies passes and resources at each structural build. |
| Contributors (C ABI) | `OpenRenderGraph/ContributorAPI.h`, `ContributorHost.h`, `ContributorRegistry.h`, `ShaderCompilerService.h` | Plugin passes from other DLLs, plus a C shader-compiler service ID. |
| Versioned resources | `Resources/VersionedResource.h` (`IVersionedResource`, `IResourceVersions`:56-63, `VersionedResource`:82), `Resources/VersionedTexture.h`, `Resources/Buffers/VersionedBuffer.h` | Immutable versions. `Make` works on any thread; `Adopt` and `Publish` run on the owner thread. Revision-named binding. One shape class per texture. Late-bound buffers. An adoption observer exists. |
| Other resources | `Resources/Buffer.h`, `PixelBuffer.h`, `ExternalTextureResource.h`, `ExternalBackingResource.h`, `DynamicResource.h`, `ResourceGroup.h`, `Render/ResourceRegistry.h` | Unmaterialized resources, imported textures (`CreateShared`), named registry. `DynamicResource` is deprecated except for the swapchain (persistent-epochs.md:497-505). |
| Latches / per-run values | `Render/LatchBlock.h`, `Render/FrameArena.h`, `Render/RunRef.h` | One host-visible region per run. Recordings do not bake in the values. |
| Uploads / staging | `Render/Runtime/IUploadService.h` (`IUploadProducer`, `SubmitStagedUploads`:91, `SubmitRecordedUploads`:112, `QueueTrackedStreamingTextureZeroFill`:201), `CopyQueueUploadService.h`, `StagedUploadBatch.h`, `StreamingUploadTypes.h`, `BufferUploadPolicy.h`, `IUploadPolicyService.h`, `RenderPasses/StreamingUploadPass.h` | Copy-queue uploads from any thread, using bump-allocated pages per producer. Staged batches. |
| Pipelines | `Render/PipelineDesc.h`, `PipelineState.h` (`PipelineStateSlot`:56), `PipelineLibrary.h`, `PipelinePermutations.h` | Pipelines described by value and built per device. Reload goes through `Rebuild`/`PublishRebuilt`/`Activate` and keeps 8 generations (docs/pipelines.md:167-180). |
| Tasks | `Render/Runtime/ITaskService.h`, `ThreadPoolTaskService.h`, `TaskServiceAccess.h`, `FrameWorkQueue.h` | `ParallelFor`, scopes, `Submit` at priority FrameCritical/Streaming/Background, and `ScheduleAfter`. |
| Retirement | `Render/Runtime/SubmissionFrontier.h`, `ResourceCleanupQueue.h`, `ExternalSignalReservation.h`; `GraphHost::SetRetiredCallback`/`SetCompletedFrameCallback` (GraphHost.h:462-475) | Queue progress points, CPU-only deferred destruction, and callbacks by submission count. Frame numbers are explicitly not a retirement clock (GraphHost.h:471-473). |
| Descriptors / bindless | `Render/Runtime/IDescriptorService.h` (`AllocateDescriptorSlot`/`RetireDescriptorSlot`:43-44), `Render/DescriptorHeap.h`, `DescriptorSnapshots.h`, `OwnedDescriptorBinding.h`, `BindlessResourceViews.h`, `BindingTable.h`, `DeclaredTableLayout.h`, `ExternalBindings.h` | Global descriptor arenas retired against fences, immutable owned bindings, and per-version bindless view snapshots. |
| Readback | `Render/Runtime/IReadbackService.h` (`RequestReadbackCapture`:48), `ReadbackCapturePlan.h`, `ReadbackCaptureReservation.h`, `RenderPasses/ReadbackCapturePass.h`, `ReadbackCopyCapturePass.h`, `Resources/ReadbackRequest.h` | End-of-frame and per-pass capture. |
| Timing / statistics | `Render/Runtime/IStatisticsService.h`, `StatisticsTypes.h`; `GraphHost::AsyncStats`(:765), `SegmentStats`, `FrameTimings`, `SubmitThreadStats`; `Render/RenderGraph/SubmissionCensus.h`, `CompileTelemetry.h`, `ScheduleTrace.h`; `OpenRenderGraph/Trace.h` (decision trace); `Telemetry/NvPerfCapture.h` | GPU timestamp queries, including owned timestamps for kept recordings. Submission census. Decision trace. |
| Recording / replay | GraphHost `RequestEpochRecording`/`EpochRecording`/`EpochRecordingDropped` (GraphHost.h:655-694), `Desc::reuseRecordings`/`reuseLiveRecordings` (persistent-epochs.md:324-351); `Render/RenderGraph/GraphReplay.h` | Revision-driven published copies and kept live recordings. `GraphReplay` is a numeric snapshot of compile structure for tests, not a command replay. |
| Memory / aliasing | `Render/RenderGraph/Aliasing/*`, `PersistentAliasArena.h`, `FrameAliasArena.h`, `InteropAllocator.h`, `Render/MemoryIntrospection*.h`, `DeviceMemory.h` | Alias planning, shared arenas, ECS memory tracking. |
| Debug UI | `DebugUI/ScheduleInspector.h`, `MemoryIntrospectionWidget.h`, `MemoryViewWidget.h` | ImGui widgets. |
| Built-in passes | `RenderPasses/GenerateMipsPass.h`, `ResolveMultisamplePass.h` | Shaders in ORG's `shaders/`. |

### OMS (`include/ORGModuleServices/`, namespaces `org::services`, `org::async`)
| Area | Headers | Purpose |
|---|---|---|
| Async state graph | `Async/StateGraph.h`, `StateGraphTypes.h`, `Detail/StateGraphImplementation.h` | A dependency engine of artifacts (kinds 0..63). It has a stall dump (`StateGraphImplementation.h:777`). |
| Artifacts | `Async/ArtifactBuild.h`, `ArtifactIdentity.h`, `ArtifactKindPolicy.h`, `ArtifactResources.h` (`ArtifactLease`, `ArtifactPayload`, `GpuSubmissionSet`:93), `ArtifactSnapshot.h`, `SuspensionIdentity.h` | Identities, recipes, leases, readiness and suspension. |
| Revision assembly | `Async/RevisionAssembly.h` (`RevisionFragment`:54, `AssembledRevision`:151, `RevisionAssembler`:205) | A revision names one fragment per slot. It is published only when every fragment is ready, and the frame selects it whole. |
| Scheduling | `Async/GraphScheduler.h` (`GraphScheduler`, `TaskClass`, `Scope`), `GraphSchedulingLayout.h`, `GraphTrace.h`, `GraphDiagnostics.h` | Abstract executor interface (the host implements it). Trace and diagnostics. |
| CPU primitives | `Async/SerializedTaskPump.h`, `PublicationExchange.h`, `LeasedArraySlots.h`; `ServiceExecutor.h` (`ServiceSubmit`) | Single-consumer pumps (lanes), an SPSC latest-publication exchange, and the executor hook for the compile and pipeline services. |
| Variables | `Variables.h` (`VariableSet`:236, `VariableSnapshot`:158, `Variable<T>`, `VariableTraits{sizes}`) | Typed variables with serials, immutable snapshots, and a wake on change. |
| Buffer versions / growth | `GrowableBufferSet.h` (`NamedVersions`:81, `GrowableBufferSet`:183), `GrowableBufferTypes.h`, `GrowthPolicy.h`, `ElasticCapacity.h`, `VersionFill.h`, `VersionedBufferGrowth.h`, `Async/VersionGrowthArtifact.h`, `ComposedVersions.h` | Requirement, then a growth batch as graph work, then a sealed set. Contents modes Fresh/Zero/Capture. Shrink policy. Up to four version sets composed into one `IResourceVersions`. |
| Texture versions | `VersionedTextureGrowth.h` (`TextureContents{Undefined,Zero}`:23-26, `MakeTextureVersions`, `FragmentWhenReady`, `PostTextureVersions`) | Next texture versions, with zero fill on the copy queue and a revision fragment that resolves when ready. |
| Pooled backings / retirement | `BackingPool.h` (`RetirementClock`:33, `BackingPool`:221, `BackingLease`), `BufferImage.h`, `BufferJournal.h`, `VersionedBufferBuild.h`, `JournaledBufferSet.h`, `Async/VersionedBufferProducer.h`, `Async/VersionedBufferFamily.h` | A lock-free retirement clock. Backings are reused once `safeAt` retires, and catch up from a journal. |
| Shaders | `ShaderCompiler.h`, `ShaderStore.h`, `ShaderServiceAdapter.h`, `CompileFlightRegistry.h` | Content-addressed DXC (DXIL or SPIR-V), deduplicated in-flight compiles, and a persistent store checked by file contents. |
| Pipelines | `PipelineService.h` (`PipelineRecipe`, `Request`, `PublishReady(fence)`), `PipelineRegistry.h` (`LivePipelineSlot`:22, `PipelineRegistry`:37: `Register`, `RequestReload`, `Service`, `Publish`, `Retire`) | Async deduplicated pipeline builds, live slots for hot reload, and fence retirement. |
| GPU uploads / descriptors (D3D12) | (README.md:7) | Upload allocations retired by exact fence, and shader-visible heaps. |
| Job graph glue | `GraphJob.h` | The GPU submission set of one job-graph execution, as a StateGraph artifact. |

## 3. CS subsystems against ORG/OMS

Key: **Have** = an equivalent exists; **Partial** = the primitives exist and CS adds policy or game specifics; **None** = no equivalent.

| CS subsystem (state on the main checkout) | ORG/OMS coverage | Where | What CS adds |
|---|---|---|---|
| Frame revision / adoption: `src/RenderGraph/Frame/Revision.{h,cpp}` (194/1453 lines). Already built on OMS. | **Partial, mostly OMS.** | OMS `Async/RevisionAssembly.h` (Revision.cpp:25, 318, 498-507, 922-954), `ComposedVersions.h`, `GrowableBufferSet.h` `NamedVersions`, `VersionedTextureGrowth.h`, `SerializedTaskPump.h` (the revision lane, Revision.cpp:675). ORG `IResourceVersions` (implemented as `RevisionBuffers`, Revision.cpp:153), `IHostExecutionData`, revision-driven epochs, `AttachStagedEpochGraphs`. | Coordinator: slot inventory (`kInputs/kTextures/kBuffers/kBuilds/kRecordings`), input waits, scene posting (latest wins), the adoption point at `Renderer::Begin`, DCLF build pairing (Revision.h:146-147), fatal policy. Plan 4c keeps it in CS until a second user exists. |
| Declarations: `Frame/Declare.{h,cpp}`, `DeclareLane.h` | **Partial.** | OMS `Variables.h` (Declare.cpp:18), `VersionedTextureGrowth.h`, `GrowableBufferSet.h`. ORG `VersionedTexture`/`VersionedBuffer`. | Declared texture/buffer/program keyed on variables and `CS::Setting`s (Declare.h:55-106), with extent-only changes per revision (:147). |
| Variables: `Frame/Variables.{h,cpp}`, `VariablesLane.h` | **Have (generic part).** | OMS `Variables.h` (`VariableSet`, `VariableSnapshot`), used at Variables.cpp:42-43, 91, 360. | `kTable` of CS variables (DisplaySize, RenderSize, ratios, upscaler, engine targets, shadow maps, HDR; Variables.h:37-228) and the setting lane. These are game-specific. |
| Recipe / stage table: `Frame/RecipeTable.h` (504), `Frame/Recipe.{h,cpp}` | **Partial (primitives only).** | ORG `GraphHost::Desc::epochOrder`, `ExternalPassDesc::Epoch`/`At` insert points, `SetFramePositions` (GraphHost.h:548-562), `ScheduleTrace`/`PersistentScheduleSnapshot`. | The recipe itself: stages, steps, anchors, clients, commit points, frame-kind recipes, the recipe check at build time. All of it is Skyrim-frame-specific. |
| CPU executor with lanes: `src/RenderGraph/Async/Executor.{h,cpp}`, `Lanes.{h,cpp}`, `EventQueue.h`, `LatestSlot.h` | **Partial.** The interface is OMS's; the implementation is CS's. | OMS `Async/GraphScheduler.h` (`Executor final : public org::async::GraphScheduler`, Executor.h:23), `GraphSchedulingLayout.h`, `SerializedTaskPump.h` (`Lane` wraps one, Lanes.h:49-80), `PublicationExchange.h`, `ServiceExecutor.h`. ORG `ITaskService`/`ThreadPoolTaskService` (a different, older interface). | A concrete MPMC executor: coordinator, serial and preparation pools, frame-critical queue and threads, a blocking domain, priorities and env switches (Executor.h:17-31; Lanes.h:17-23). `EventQueue` (a bounded Vyukov ring that spills onto a stack) and `LatestSlot` (a lock-free latest-wins mailbox) have **no** OMS equivalent. `PublicationExchange` is close to `LatestSlot` but SPSC with 3 slots. The executor is mostly generic; only the env names and thread counts are CS's. |
| Retirement clock: `Async/Retirement.{h,cpp}` | **Partial.** | OMS `RetirementClock` (BackingPool.h:33; Retirement.cpp:54-56 makes one). ORG `SetRetiredCallback` (GraphHost.h:470-475), `SubmissionFrontier.h`. | Joins two sources (the graph host's retired count and the CPU pipeline's timeline signals) into one point. Sealed-point vs submitted-point rules (Retirement.h:16-39). |
| Program service: `src/RenderGraph/ProgramService.{h,cpp}`, `ProgramBuild.*`, `ComputeProgram.*`, `RasterProgram.*`, `ShaderTarget.h` | **Partial.** | OMS `ShaderCompiler.h` (ProgramBuild.h:8), `ShaderStore.h`, `PipelineRegistry.h` (ComputeProgram.cpp:18, RasterProgram.cpp:22), `PipelineService.h`. ORG `PipelineLibrary`/`PipelinePermutations`/`PipelineCompiler` hook. | Three entry points (declared, fixed at setup, table keyed on observed state), revision gating, `HoldForPrograms`, a programs pool on CS lanes (ProgramService.cpp:49), fatal on failure (ProgramService.h:17-27). Common defines read CS `State` (:31). |
| Build manager: `src/RenderGraph/BuildManager.{h,cpp}` (163/876) | **Partial.** ORG provides the primitives and leaves the policy to the host by design. | ORG `BuildEpochGraph`/`AttachEpochGraph`/`StageEpochGraph`/`AttachStagedEpochGraphs`/`DropStagedEpochGraph`, `RebuildRequests`/`RebuildReasons`, `SetExplicitBuilds`/`BuildIfRequested` (GraphHost.h:217-379). | Policy: `Builds::Ask` queued to the adoption point, a build thread, polling at frame boundaries, staged groups for DCLF epochs (BuildManager.h:3-41). A candidate for an OMS "build service". |
| Frame clock: `src/RenderGraph/FrameClock.h` (55) | **None as one object.** | ORG `Desc::framesInFlight` (GraphHost.h:76), `SetEpochCopy` (frame slot = frame mod copies, :597-607), `CompletedFrameCallback`. | A static atomic clock: beginFrame, slot (kSlots=4), presentSerial, gpuCompleted. There is also a separate DCLF clock (`src/Features/DrawcallLimitFix/Core/Engine/FrameClock.h`). |
| Birth / first contents: `Frame/Birth.{h,cpp}`. **Not on the main checkout.** It is only on `org-native/cleanup-p4-birth` (worktree `org-port-worktrees/cleanup-p4-birth`). | **Partial.** | OMS `TextureContents{Undefined, Zero}` (VersionedTextureGrowth.h:23-26). ORG `IUploadService::QueueTrackedStreamingTextureZeroFill` (IUploadService.h:201, the latest ORG commit `bfd13bb`). OMS `GrowableContents{Fresh, Zero, Capture}` for buffers (README.md:87-91). | `Value` (a single texel value encoded in the format) and `Fill` (CPU data per subresource on the revision lane) (Birth.h:10-21). Birth.h includes no ORG/OMS headers, so it is easy to lift into OMS as extra `TextureContents` modes. |
| Epoch recordings cache/service: `Frame/EpochRecordings.{h,cpp}`. **Not on main.** It is on `cleanup-p5-coverage`, `-coverage-rec`, `-fragments`, `-reload`. Main has `Frame/RevisionRecordings.h` (member recordings per revision, opaque, in FrameAnchors.cpp). | **Partial.** | ORG `GraphHost::RequestEpochRecording`, `EpochRecording`, `EpochRecordingDropped` (GraphHost.h:655-694). | One outcome policy: Ready, Dropped (ask again) or Failed (fatal); build-generation matching (EpochRecordings.h:14-50). A thin layer; it could become a GraphHost helper or OMS. |
| Starvation watch: `Frame/Starvation.{h,cpp}` (`StarvationWatch`). **Not on main.** It is on `cleanup-p5-coverage`, `-fragments`, `-reload`. On main the rule lives inline in `Frame/Revision.cpp` (and DCLF `Render/Impl/SnapshotBuilder.h`). | **None.** The nearest thing is the StateGraph stall dump (StateGraphImplementation.h:777), which is a diagnostic dump, not a rule. | Counts misses since the last input; a miss past `kStarvedAfter` raises a `TapError::Starved` (Starvation.h:12-38). Generic and single-owner. |
| Bindless resolve: `src/RenderGraph/Bindless.{h,cpp}`. **Not on main.** It is only on `cleanup-p6-misc`. | **Partial.** | ORG `IDescriptorService` (Allocate/RetireDescriptorSlot), `OwnedDescriptorBinding`, `BindlessResourceViews`, `DescriptorSnapshots`. | Maps D3D11 SRVs to descriptor indices through DCLF's texture import, with zero views per dimension and a null descriptor (Bindless.h:11-59). The D3D11/DXVK part is CS-specific. The "zero view of the right kind" could be generic. |
| Image registry: `src/RenderGraph/ImageRegistry.{h,cpp}` | **None.** ORG `ResourceRegistry` names resources; it does not make native images unique. | ORG `ExternalTextureResource::CreateShared`; BasicRHI `rhi_interop_vulkan.h`. | One live graph resource per native image (registering a second is fatal), creator tags, lookup through DXVK handles (ImageRegistry.h:3-63). The generic core is "one tracker per native image". |
| Graph resources / export to D3D11: `src/RenderGraph/GraphResources.{h,cpp}` (446/1398) | **Partial.** | ORG `Buffer`, `PixelBuffer`, `VersionedTexture`, `ExternalTextureResource`, transient aliasing. | Twins of the `Buffer.h` wrappers with D3D11 export over DXVK. Lifetime classes; an exported resource is never aliased; D3D11-first teardown (GraphResources.h:1-40). CS/DXVK-specific. |
| D3D11 wrappers: `src/RenderGraph/D3D11Wrapper.{h,cpp}` | **None.** | — | Re-points a feature's `::Texture2D` at the twin's current export at adoption (D3D11Wrapper.h:24-91). CS-specific. |
| GPU constant block mapping: `src/Utils/GpuBlock.h` (94) | **None.** ORG `LatchBlock` covers per-run values written before submit, not a settings-to-struct mapping. | — | A field map from CPU settings to a GPU struct; `CS::Setting` fields read their adopted value; a colour hook (GpuBlock.h:3-69). Tied to `CS::Setting`; the pattern is generic. |
| Upload ring: `Engine/Buffers/UploadRing.{h,cpp}`. **Not on main.** It is only on `cleanup-p6-residue` (as `src/RenderGraph/Engine/Buffers/UploadRing.*`; `src/Engine/Buffers` does not exist). Main has `Engine/Buffers/DynamicRing.cpp`, `EngineBuffers.*`, `BufferBacking.h`. | **Partial.** | ORG `CopyQueueUploadService` (bump-allocated mapped pages per producer), `StagedUploadBatch`, `FrameArena`. OMS `BackingPool` + `RetirementClock` (reuse after retirement). | Chunks of host-visible memory parked per frame and released through `ReleaseAfterD3D11` after Present; serves the engine's dynamic vertex/index buffers and constant capture (UploadRing.h:16-80). Engine-facing. The ring would be generic if retirement went through `RetirementClock`. |
| Test harness counters: `src/RenderGraph/Testing/GraphFrames.*`, `OrgCommands.*`, `D3D12SelfTest.*` | **None** for test-runner counters. ORG has stats only: `AsyncStats`, `SubmissionCensus`, `AttachStats`, `SegmentStats`. | ORG `GraphHost::AsyncStats` (:765), `SubmissionCensus.h`. | Per-client counts of frames on the graph vs on D3D11, test commands for the CS test runner, and a D3D12 self-test thread. CS-test-specific. |

Branch-only files were found with `git ls-tree` on local branches and checked in `C:\Users\matth\source\repos\SARP\external_refs\org-port-worktrees\*`.

## 4. Gaps and open notes in ORG/OMS docs that match CS subsystems

- **Build policy is left to the host on purpose** (persistent-epochs.md:31-34; GraphHost.h:238-242). The synchronous `RequestRebuild` path holds the host thread (persistent-epochs.md:29-30). So no library BuildManager exists. ORG documents the traps such a manager must handle:
  - builds block on pipeline compiles, so a build should run on its own thread (persistent-epochs.md:45-47);
  - shared setup is deferred to the attach (:48-50);
  - an attach is rejected when the configuration changed (:56-60).
- **Texture first contents**: OMS `TextureContents` has only `Undefined`/`Zero`; Zero needs a Concurrent, single-sampled, non-depth texture (VersionedTextureGrowth.h:22-26, 47-48). CS Birth's `Value`/`Fill` are missing there.
- **Multisampled SRVs are not supported** (`PixelBuffer`/`ExternalTextureResource` are single-sampled; no `Texture2DMS` views) (pipelines.md:200-202). This matters for Birth (depth/MSAA only `Undefined`) and for bindless zero views.
- **Pipeline reload**: ORG `PipelineLibrary::Rebuild` fails on a changed resource interface unless `allowInterfaceChange` (pipelines.md:172-174). OMS `PipelineRegistry` is a separate reload path, modelled on BasicRenderer's PSOManager (PipelineRegistry.h:29-36). There are two pipeline stacks (ORG library vs OMS service/registry), and CS's ProgramService sits on the OMS one.
- **Two task interfaces**: ORG `ITaskService` (priority enum) and OMS `GraphScheduler` (TaskClass lane/domain). No doc reconciles them. CS's Executor implements only the OMS one.
- **Retirement**: GraphHost.h:471-473 warns that frame numbers are not a retirement clock. OMS `RetirementClock` has "one clock per host" (README.md:131-135). Neither provides CS's two-source join (graph host plus CPU pipeline timelines).
- **Late publication / adoption**: OMS `PublicationExchange` "is NOT a GPU readiness gate" (PublicationExchange.h:15-16; README.md:56-57). CS's revision adoption gate stays host-side.
- **Asset jobs gating readiness**: job-graphs.md:324-400 (BasicRenderer). Open item: `AppendTextureDisplayRequirements` has no caller (job-graphs.md:399). This parallels CS's revision input waits.
- **Contributor ABI / CS plugin API**: rebuild-plan.md Phase 6 (:185-212) exports `ORG_GetRenderGraphAPI` from CS with `cs.*` epoch names. A game-target export table is deferred "to a follow-up CS plugin-API project" (:212). This matters for ImageRegistry/GraphResources export.
- **Readback capture**: rebuild-plan.md Phase 8 (:247-271). CS wires it "if CS's readback path supports it" (:270).
- **Rebuild-plan open gaps G1-G7** (multi-RHI, queue activation, scheduler, transition elision, aliasing packing, idle dematerialization, inspector sources) (rebuild-plan.md:86-98). G4 is marked "both"; the rest are BasicRenderer. Status notes (:4-26) say the Phase 5/6 CS checks are still owed, and there is a known TrackingWorld test flake (:14).
- **OMS test breakage** noted in CS docs: `ORGBufferBuildTests` doesn't compile (`ProcessDeferredReleases` signature) (`docs/development/plans/director-decisions-2026-11.md:100`).
- **CS-flavoured comment in OMS**: `Variables.h:30` ("decision 6a"). It should be generalised if more frame code moves to OMS.
- No ORG/OMS doc mentions a frame clock, starvation rule, recipe/stage table, image registry, D3D11 wrappers, GPU settings blocks or test counters. These are not on either library's roadmap.

## 5. Quick read for the survey
- **Already in OMS, with CS as a thin host**: variables, revision assembly, buffer and texture growth, pooled backings and retirement clock, shader compile and store, pipeline registry and service, task pumps.
- **Generic in CS and good OMS candidates**:
  - the Executor implementation (an OMS `GraphScheduler` implementation);
  - `EventQueue`, `LatestSlot`;
  - `StarvationWatch`;
  - the BuildManager policy (a staged-build service over GraphHost primitives);
  - the `EpochRecordings` outcome wrapper (or an ORG GraphHost helper);
  - Birth modes (an extension of `TextureContents`);
  - the two-source retirement join;
  - the FrameClock shape;
  - the UploadRing core.
- **CS/Skyrim/DXVK-specific, stays in CS**: the recipe table and stages, the variables `kTable`, Declare's `CS::Setting` binding, GraphResources' D3D11 export, D3D11Wrapper, the ImageRegistry DXVK lookup, Bindless' D3D11 SRV import, GpuBlock's `CS::Setting` reads, and the Testing harness.
